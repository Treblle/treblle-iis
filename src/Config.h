#pragma once
#include "precomp.h"

struct RouteFilter {
    std::string host;   // required, lowercase, e.g. "api.example.com"
    std::string path;   // optional prefix, e.g. "/v1" — empty means entire host
};

struct TreblleConfig {
    std::string              sdkToken;    // workspace auth token → sent as api_key in payload and x-api-key header
    std::string              apiKey;      // API project key → sent as project_id in payload
    std::string              treblleUrl  = "https://ingress.treblle.com";
    bool                     debugMode   = false;
    bool                     disabled    = false;
    std::vector<RouteFilter> excludeRoutes;
    std::vector<std::string> maskedKeywords; // keys whose values are replaced with '*'
    bool                     loaded      = false;
};

// Thread-safe singleton that loads treblle.config from the DLL directory.
// Call CheckReload() once per request — it re-reads the file only when mtime changes.
class Config {
public:
    static Config& Instance();

    // Load config from <dllPath directory>\treblle.config.
    // Safe to call from RegisterModule; returns false on parse/validation error.
    bool Load(const std::wstring& dllPath);

    // Check if the config file has been modified and reload if so.
    void CheckReload();

    // Returns a snapshot of the current config as a shared_ptr.
    // Callers hold the pointer for the duration of the request — no lock needed after Get().
    std::shared_ptr<const TreblleConfig> Get() const;

    // Returns the matched default-excluded path prefix, or empty string if none.
    // These are built-in and cannot be overridden by config.
    static std::string MatchDefaultPath(const std::string& urlPath);

    // Returns true if the host+path matches an exclude_routes entry.
    bool IsExcluded(const std::string& host, const std::string& urlPath) const;

private:
    Config() = default;
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    bool LoadFromFile();

    mutable std::mutex                   mutex_;
    std::shared_ptr<TreblleConfig>       config_ = std::make_shared<TreblleConfig>();
    std::wstring                         configPath_;
    FILETIME                             lastWriteTime_ = {};
};
