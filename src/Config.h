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
    std::vector<RouteFilter> excludeRoutes;
    bool                     loaded      = false;
};

// Thread-safe singleton that loads treblle.config from the DLL directory.
// Call CheckReload() once per request — it re-reads the file only when mtime changes.
class Config {
public:
    static Config& Instance();

    // Load config from <dllPath directory>\treblle.config.
    // Safe to call from RegisterModule; returns false on parse error (silently).
    bool Load(const std::wstring& dllPath);

    // Check if the config file has been modified and reload if so.
    void CheckReload();

    // Returns a snapshot of the current config (copied under lock).
    TreblleConfig Get() const;

    // Returns true if the host+path matches an exclude_routes entry.
    bool IsExcluded(const std::string& host, const std::string& urlPath) const;

private:
    Config() = default;
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    bool LoadFromFile();

    mutable std::mutex mutex_;
    TreblleConfig      config_;
    std::wstring       configPath_;
    FILETIME           lastWriteTime_ = {};
};
