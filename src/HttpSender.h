#pragma once
#include "precomp.h"

// Sends a JSON payload to a Treblle ingress URL using WinHTTP.
// One instance should be created per background worker thread and reused
// across sends. The session and connection handles are cached so TCP
// connections and TLS sessions are reused across requests.
class HttpSender {
public:
    HttpSender();
    ~HttpSender();

    // POST jsonPayload to url. Returns true on HTTP 2xx response.
    // Logs to Windows Event Log on error if debugMode is true.
    bool Send(const std::string& jsonPayload,
              const std::string& url,
              const std::string& sdkToken,
              bool               debugMode);

private:
    // Open (or reuse) the connection handle for the given URL.
    // Re-parses and reconnects only when the URL changes.
    bool EnsureConnected(const std::string& url, bool debugMode);

    HINTERNET    hSession_  = nullptr;
    HINTERNET    hConnect_  = nullptr; // cached per-host connection handle
    std::string  cachedUrl_;           // URL hConnect_ was opened for
    std::wstring wHost_;
    std::wstring wPath_;
    INTERNET_PORT port_     = 0;
    bool         isHttps_   = false;
};
