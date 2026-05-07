#pragma once
#include "precomp.h"

// Sends a JSON payload to a Treblle ingress URL using WinHTTP.
// One instance should be created per background worker thread and reused
// across sends so the session handle is kept alive (enables keep-alive).
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
    HINTERNET hSession_;
};
