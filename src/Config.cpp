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

bool Config::IsExcluded(const std::string& host, const std::string& urlPath) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& route : config_.excludeRoutes) {
        if (!_stricmp(route.host.c_str(), host.c_str())) {
            if (route.path.empty() || StartsWithCI(urlPath, route.path))
                return true;
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

// Default sensitive-field names masked when 'masked_keywords' is absent from config.
static const std::vector<std::string> kDefaultMaskedKeywords = {
    "password", "pwd", "secret", "password_confirmation", "passwordConfirmation",
    "cc", "card_number", "cardNumber", "ccv", "credit_score", "creditScore", "ssn"
};

// Parse masked_keywords — JSON array ["k1","k2"] or comma-separated string "k1,k2".
// Returns the default list when the key is absent; returns exactly what is configured
// (possibly empty) when the key is present, so users can disable all masking.
static std::vector<std::string> ParseMaskedKeywords(const std::string& json) {
    const char* kKey = "\"masked_keywords\"";
    size_t k = json.find(kKey);
    if (k == std::string::npos) return kDefaultMaskedKeywords;

    size_t pos = SkipWS(json, k + strlen(kKey));
    if (pos >= json.size() || json[pos] != ':') return kDefaultMaskedKeywords;
    pos = SkipWS(json, pos + 1);
    if (pos >= json.size()) return kDefaultMaskedKeywords;

    std::vector<std::string> result;

    auto trimmed = [](const std::string& s) -> std::string {
        size_t b = s.find_first_not_of(' ');
        if (b == std::string::npos) return {};
        size_t e = s.find_last_not_of(' ');
        return s.substr(b, e - b + 1);
    };

    if (json[pos] == '[') {
        ++pos; // skip '['
        while (pos < json.size()) {
            pos = SkipWS(json, pos);
            if (pos >= json.size() || json[pos] == ']') break;
            if (json[pos] == '"') {
                std::string val = trimmed(ParseString(json, pos));
                if (!val.empty()) result.push_back(std::move(val));
            } else if (json[pos] == ',') {
                ++pos;
            } else {
                ++pos;
            }
        }
    } else if (json[pos] == '"') {
        std::string csv = ParseString(json, pos);
        size_t start = 0;
        while (start <= csv.size()) {
            size_t comma = csv.find(',', start);
            std::string token = trimmed(csv.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start));
            if (!token.empty()) result.push_back(std::move(token));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }

    return result;
}

// Parse exclude_routes array — list of {"host":"...", "path":"..."} objects.
std::vector<RouteFilter> ParseExcludeRoutes(const std::string& json) {
    std::vector<RouteFilter> routes;

    size_t arrKey = json.find("\"exclude_routes\"");
    if (arrKey == std::string::npos) return routes;

    size_t pos = SkipWS(json, arrKey + strlen("\"exclude_routes\""));
    if (pos >= json.size() || json[pos] != ':') return routes;
    pos = SkipWS(json, pos + 1);
    if (pos >= json.size() || json[pos] != '[') return routes;
    ++pos; // skip '['

    while (pos < json.size()) {
        pos = SkipWS(json, pos);
        if (pos >= json.size() || json[pos] == ']') break;
        if (json[pos] != '{') { ++pos; continue; }

        size_t objEnd = json.find('}', pos);
        if (objEnd == std::string::npos) break;
        std::string obj = json.substr(pos, objEnd - pos + 1);

        RouteFilter rf;
        rf.host = ToLower(FindString(obj, "host"));
        rf.path = FindString(obj, "path");
        if (!rf.path.empty() && rf.path[0] != '/') rf.path = "/" + rf.path;

        if (!rf.host.empty())
            routes.push_back(std::move(rf));

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
    newCfg.sdkToken        = FindString(json, "sdk_token");
    newCfg.apiKey          = FindString(json, "api_key");
    newCfg.debugMode       = FindBool(json, "debug", false);
    newCfg.excludeRoutes   = ParseExcludeRoutes(json);
    newCfg.maskedKeywords  = ParseMaskedKeywords(json);
    newCfg.loaded          = true;

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
