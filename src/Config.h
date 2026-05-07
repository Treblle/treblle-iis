#pragma once
#include "precomp.h"

struct RouteFilter {
    std::string host;  // required, lowercase, e.g. "api.example.com"
    std::string path;  // optional, e.g. "/v1" — empty means match all paths on host
};

struct TreblleConfig {
    std::string              apiKey;
    std::string              sdkToken;
    std::string              treblleUrl  = "https://ingress.treblle.com";
    bool                     debugMode   = false;
    std::vector<RouteFilter> includeRoutes;
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

    // Returns true if 'host' (lowercase) and 'urlPath' match any include_route entry.
    bool Matches(const std::string& host, const std::string& urlPath) const;

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
