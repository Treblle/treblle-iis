#include "precomp.h"
#include "Config.h"
#include "Utils.h"
#include "vendor/json.hpp"

// ── Singleton ─────────────────────────────────────────────────────────────────

Config& Config::Instance() {
    static Config instance;
    return instance;
}

// ── Public API ────────────────────────────────────────────────────────────────

bool Config::Load(const std::wstring& dllPath) {
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

std::shared_ptr<const TreblleConfig> Config::Get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

bool Config::IsExcluded(const std::string& host, const std::string& urlPath) const {
    auto cfg = Get();
    for (const auto& route : cfg->excludeRoutes) {
        if (!_stricmp(route.host.c_str(), host.c_str())) {
            if (route.path.empty() || StartsWithCI(urlPath, route.path))
                return true;
        }
    }
    return false;
}

// ── File reader ───────────────────────────────────────────────────────────────

namespace {

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

static const std::vector<std::string> kDefaultMaskedKeywords = {
    "password", "pwd", "secret", "password_confirmation", "passwordConfirmation",
    "cc", "card_number", "cardNumber", "ccv", "credit_score", "creditScore", "ssn"
};

} // namespace

// ── LoadFromFile ──────────────────────────────────────────────────────────────

bool Config::LoadFromFile() {
    std::string content = ReadConfigFile(configPath_);
    if (content.empty()) return false;

    // Record mtime before parsing so a bad config suppresses further retries
    // until the file is saved again (mtime changes).
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (GetFileAttributesExW(configPath_.c_str(), GetFileExInfoStandard, &fa))
        lastWriteTime_ = fa.ftLastWriteTime;

    try {
        auto j = nlohmann::json::parse(content);

        auto newCfg = std::make_shared<TreblleConfig>();
        newCfg->sdkToken  = j.value("sdk_token", std::string{});
        newCfg->apiKey    = j.value("api_key",   std::string{});
        newCfg->debugMode = j.value("debug",     false);
        newCfg->disabled  = j.value("disabled",  false);

        std::string url = j.value("treblle_url", std::string{});
        if (!url.empty()) newCfg->treblleUrl = url;

        if (j.contains("exclude_routes") && j["exclude_routes"].is_array()) {
            for (const auto& route : j["exclude_routes"]) {
                if (!route.is_object()) continue;
                RouteFilter rf;
                rf.host = ToLower(route.value("host", std::string{}));
                rf.path = route.value("path", std::string{});
                if (!rf.path.empty() && rf.path[0] != '/') rf.path = "/" + rf.path;
                if (!rf.host.empty()) newCfg->excludeRoutes.push_back(std::move(rf));
            }
        }

        if (j.contains("masked_keywords")) {
            const auto& mk = j["masked_keywords"];
            if (mk.is_array()) {
                for (const auto& kw : mk)
                    if (kw.is_string()) newCfg->maskedKeywords.push_back(kw.get<std::string>());
            } else if (mk.is_string()) {
                // Also accept a comma-separated string for convenience
                std::string csv = mk.get<std::string>();
                size_t start = 0;
                while (start <= csv.size()) {
                    size_t comma = csv.find(',', start);
                    std::string tok = (comma == std::string::npos)
                        ? csv.substr(start)
                        : csv.substr(start, comma - start);
                    size_t b = tok.find_first_not_of(' ');
                    if (b != std::string::npos) {
                        size_t e = tok.find_last_not_of(' ');
                        tok = tok.substr(b, e - b + 1);
                    }
                    if (!tok.empty()) newCfg->maskedKeywords.push_back(std::move(tok));
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
            }
        } else {
            newCfg->maskedKeywords = kDefaultMaskedKeywords;
        }

        if (newCfg->sdkToken.empty() || newCfg->apiKey.empty()) {
            LogDebug("Treblle: config must have non-empty 'sdk_token' and 'api_key' — module disabled", true);
            return false;
        }

        newCfg->loaded = true;

        std::lock_guard<std::mutex> lock(mutex_);
        config_ = std::move(newCfg);
        return true;

    } catch (const nlohmann::json::exception& e) {
        LogDebug(std::string("Treblle: config parse error: ") + e.what(), true);
        return false;
    }
}
