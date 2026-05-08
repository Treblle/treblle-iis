#include "precomp.h"
#include "Config.h"
#include "Utils.h"

// ── Singleton ─────────────────────────────────────────────────────────────────

Config& Config::Instance() {
    static Config instance;
    return instance;
}

// ── Public API ────────────────────────────────────────────────────────────────

bool Config::Load(const std::wstring& dllPath) {
    // Build path: <dllDir>\treblle.config
    std::wstring dir = dllPath;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir = dir.substr(0, slash + 1);
    configPath_ = dir + L"treblle.config";
    return LoadFromFile();
}

void Config::CheckReload() {
    if (configPath_.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (!GetFileAttributesExW(configPath_.c_str(), GetFileExInfoStandard, &fa)) return;
    if (CompareFileTime(&fa.ftLastWriteTime, &lastWriteTime_) == 0) return;
    LoadFromFile();
}

TreblleConfig Config::Get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

bool Config::MatchRoute(const std::string& host, const std::string& urlPath,
                        std::string& outInternalId, std::string& outInternalName) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& route : config_.includeRoutes) {
        if (!_stricmp(route.host.c_str(), host.c_str())) {
            if (route.path.empty() || StartsWithCI(urlPath, route.path)) {
                outInternalId   = route.internalId;
                outInternalName = route.internalName;
                return true;
            }
        }
    }
    return false;
}

// ── Minimal JSON parser ───────────────────────────────────────────────────────

namespace {

// Returns the content of a file as a UTF-8 string, or empty on error.
std::string ReadConfigFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    DWORD size = GetFileSize(h, nullptr);
    if (size == INVALID_FILE_SIZE || size == 0) { CloseHandle(h); return {}; }
    std::string buf(size, '\0');
    DWORD bytesRead = 0;
    ::ReadFile(h, &buf[0], size, &bytesRead, nullptr);
    CloseHandle(h);
    buf.resize(bytesRead);
    return buf;
}

// Skip whitespace
size_t SkipWS(const std::string& s, size_t pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n'))
        ++pos;
    return pos;
}

// Parse a JSON string starting at pos (which should point at the opening '"').
// Returns the unescaped value and advances pos past the closing '"'.
std::string ParseString(const std::string& s, size_t& pos) {
    if (pos >= s.size() || s[pos] != '"') return {};
    ++pos; // skip opening quote
    std::string result;
    while (pos < s.size() && s[pos] != '"') {
        if (s[pos] == '\\' && pos + 1 < s.size()) {
            ++pos;
            switch (s[pos]) {
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                case '/':  result += '/';  break;
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                default:   result += s[pos]; break;
            }
        } else {
            result += s[pos];
        }
        ++pos;
    }
    if (pos < s.size()) ++pos; // skip closing quote
    return result;
}

// Find the value of a top-level JSON string key.
std::string FindString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) return {};
    size_t pos = SkipWS(json, k + needle.size());
    if (pos >= json.size() || json[pos] != ':') return {};
    pos = SkipWS(json, pos + 1);
    if (pos >= json.size() || json[pos] != '"') return {};
    return ParseString(json, pos);
}

// Find the value of a top-level JSON boolean key.
bool FindBool(const std::string& json, const std::string& key, bool defaultVal = false) {
    std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) return defaultVal;
    size_t pos = SkipWS(json, k + needle.size());
    if (pos >= json.size() || json[pos] != ':') return defaultVal;
    pos = SkipWS(json, pos + 1);
    if (json.compare(pos, 4, "true") == 0)  return true;
    if (json.compare(pos, 5, "false") == 0) return false;
    return defaultVal;
}

// Derive a stable UUID-style internal ID from an input string using FNV-1a (no dependencies).
static std::string ComputeInternalId(const std::string& input, const std::string& /*unused*/) {
    auto fnv1a = [](const std::string& s, uint64_t seed) -> uint64_t {
        uint64_t h = seed;
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
        return h;
    };
    uint64_t hi = fnv1a("treblle:" + input, 14695981039346656037ULL);
    uint64_t lo = fnv1a("route:"   + input, 14695981039346656037ULL);
    char buf[37];
    snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%04x%08x",
        (uint32_t)(hi >> 32),
        (uint16_t)(hi >> 16),
        (uint16_t)(hi & 0xFFFF),
        (uint16_t)(lo >> 48),
        (uint16_t)(lo >> 32),
        (uint32_t)(lo & 0xFFFFFFFF));
    return buf;
}

// Parse include_routes array — list of {"host":"...", "path":"..."} objects.
std::vector<RouteFilter> ParseIncludeRoutes(const std::string& json) {
    std::vector<RouteFilter> routes;

    size_t arrKey = json.find("\"include_routes\"");
    if (arrKey == std::string::npos) return routes;

    size_t pos = SkipWS(json, arrKey + strlen("\"include_routes\""));
    if (pos >= json.size() || json[pos] != ':') return routes;
    pos = SkipWS(json, pos + 1);
    if (pos >= json.size() || json[pos] != '[') return routes;
    ++pos; // skip '['

    while (pos < json.size()) {
        pos = SkipWS(json, pos);
        if (pos >= json.size() || json[pos] == ']') break;
        if (json[pos] != '{') { ++pos; continue; }

        // Find matching '}'
        size_t objEnd = json.find('}', pos);
        if (objEnd == std::string::npos) break;
        std::string obj = json.substr(pos, objEnd - pos + 1);

        RouteFilter rf;
        rf.host = ToLower(FindString(obj, "host"));
        rf.path = FindString(obj, "path");
        // Ensure path starts with '/' if non-empty
        if (!rf.path.empty() && rf.path[0] != '/') rf.path = "/" + rf.path;

        if (!rf.host.empty()) {
            std::string name = FindString(obj, "name");
            rf.internalName = name.empty()
                ? (rf.path.empty() ? rf.host : rf.host + rf.path)
                : name;
            // Include the name in the hash so projects with the same host+path stay distinct
            rf.internalId = ComputeInternalId(rf.host + rf.path + rf.internalName, "");
            routes.push_back(std::move(rf));
        }

        pos = objEnd + 1;
        pos = SkipWS(json, pos);
        if (pos < json.size() && json[pos] == ',') ++pos;
    }
    return routes;
}

} // namespace

// ── LoadFromFile ──────────────────────────────────────────────────────────────

bool Config::LoadFromFile() {
    std::string json = ReadConfigFile(configPath_);
    if (json.empty()) return false;

    TreblleConfig newCfg;
    newCfg.sdkToken      = FindString(json, "sdk_token");
    newCfg.apiKey        = FindString(json, "api_key");
    newCfg.debugMode     = FindBool(json, "debug", false);
    newCfg.includeRoutes = ParseIncludeRoutes(json);
    newCfg.loaded        = true;

    std::string url = FindString(json, "treblle_url");
    if (!url.empty()) newCfg.treblleUrl = url;

    // Record mtime
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (GetFileAttributesExW(configPath_.c_str(), GetFileExInfoStandard, &fa))
        lastWriteTime_ = fa.ftLastWriteTime;

    std::lock_guard<std::mutex> lock(mutex_);
    config_ = std::move(newCfg);
    return true;
}
