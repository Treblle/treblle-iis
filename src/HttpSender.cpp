#include "precomp.h"
#include "HttpSender.h"
#include "Constants.h"
#include "Utils.h"

HttpSender::HttpSender()
    : hSession_(WinHttpOpen(L"TreblleIISAgent/1.0",
                             WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS,
                             0)) {
    if (hSession_) {
        DWORD timeout = TreblleConst::kHttpTimeoutMs;
        WinHttpSetOption(hSession_, WINHTTP_OPTION_RESOLVE_TIMEOUT, &timeout, sizeof(timeout));
        WinHttpSetOption(hSession_, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
        WinHttpSetOption(hSession_, WINHTTP_OPTION_SEND_TIMEOUT,    &timeout, sizeof(timeout));
        WinHttpSetOption(hSession_, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

        // Request HTTP/2 via ALPN; WinHTTP falls back to HTTP/1.1 if the server
        // doesn't advertise h2. Requires Windows 10 1607+ / Server 2016+.
        DWORD protocols = WINHTTP_PROTOCOL_FLAG_HTTP2;
        WinHttpSetOption(hSession_, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL,
                         &protocols, sizeof(protocols));
    }
}

HttpSender::~HttpSender() {
    if (hConnect_) WinHttpCloseHandle(hConnect_);
    if (hSession_) WinHttpCloseHandle(hSession_);
}

// ── Connection caching ────────────────────────────────────────────────────────

bool HttpSender::EnsureConnected(const std::string& url, bool debugMode) {
    if (hConnect_ && url == cachedUrl_) return true;

    if (hConnect_) { WinHttpCloseHandle(hConnect_); hConnect_ = nullptr; }
    cachedUrl_.clear();

    int wlen = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring wUrl(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, &wUrl[0], wlen);

    URL_COMPONENTS uc = {};
    uc.dwStructSize      = sizeof(uc);
    uc.dwHostNameLength  = (DWORD)-1;
    uc.dwUrlPathLength   = (DWORD)-1;
    uc.dwSchemeLength    = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wUrl.c_str(), 0, 0, &uc)) {
        if (debugMode) LogDebug("Treblle: WinHttpCrackUrl failed for URL: " + url, true);
        return false;
    }

    wHost_   = std::wstring(uc.lpszHostName, uc.dwHostNameLength);
    wPath_   = std::wstring(uc.lpszUrlPath,  uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength > 0)
        wPath_ += std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (wPath_.empty()) wPath_ = L"/";
    port_    = uc.nPort ? uc.nPort : INTERNET_DEFAULT_HTTPS_PORT;
    isHttps_ = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    hConnect_ = WinHttpConnect(hSession_, wHost_.c_str(), port_, 0);
    if (!hConnect_) {
        if (debugMode) LogDebug("Treblle: WinHttpConnect failed", true);
        return false;
    }

    cachedUrl_ = url;
    return true;
}

// ── Send ──────────────────────────────────────────────────────────────────────

bool HttpSender::Send(const std::string& jsonPayload,
                      const std::string& url,
                      const std::string& sdkToken,
                      bool               debugMode) {
    if (!hSession_ || jsonPayload.empty() || url.empty()) return false;

    // 429 backoff: a previous response told us to wait via Retry-After.
    if (retryAfterDelayMs_ != 0) {
        if (GetTickCount() - retryAfterOpenedMs_ < retryAfterDelayMs_) return false;
        retryAfterDelayMs_ = 0; // backoff window elapsed — reset
    }

    // Circuit breaker: trip open after consecutive 5xx/network failures;
    // allow one probe attempt after the cooldown window elapses.
    if (consecutiveFailures_ >= kTripThreshold) {
        if (GetTickCount() - openedAtMs_ < kCooldownMs) return false;
        if (debugMode)
            LogDebug("Treblle: circuit breaker probing after cooldown", true);
    }

    if (debugMode)
        LogDebug("Treblle: sending payload (" + std::to_string(jsonPayload.size()) + " bytes)", true);

    if (!EnsureConnected(url, debugMode)) return false;

    DWORD flags = isHttps_ ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConnect_, L"POST", wPath_.c_str(),
                                        nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) {
        // Stale connection — drop and reconnect once.
        WinHttpCloseHandle(hConnect_); hConnect_ = nullptr; cachedUrl_.clear();
        if (!EnsureConnected(url, debugMode)) return false;
        hReq = WinHttpOpenRequest(hConnect_, L"POST", wPath_.c_str(),
                                  nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hReq) {
            if (debugMode) LogDebug("Treblle: WinHttpOpenRequest failed", true);
            return false;
        }
    }

    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!sdkToken.empty()) {
        int wTokenLen = MultiByteToWideChar(CP_UTF8, 0, sdkToken.c_str(), -1, nullptr, 0);
        if (wTokenLen > 1) {
            std::wstring wToken(wTokenLen, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, sdkToken.c_str(), -1, &wToken[0], wTokenLen);
            wToken.resize(wTokenLen - 1);
            headers += L"x-api-key: " + wToken + L"\r\n";
        }
    }

    BOOL  ok         = WinHttpSendRequest(hReq,
                                          headers.c_str(),
                                          (DWORD)headers.size(),
                                          (LPVOID)jsonPayload.c_str(),
                                          (DWORD)jsonPayload.size(),
                                          (DWORD)jsonPayload.size(),
                                          0);
    bool  success    = false;
    DWORD statusCode = 0; // stays 0 if we never get an HTTP response (network failure)

    if (ok) {
        ok = WinHttpReceiveResponse(hReq, nullptr);
        if (!ok && debugMode) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Treblle: WinHttpReceiveResponse failed (0x%08lX)", GetLastError());
            LogDebug(msg, true);
        }
        if (ok) {
            DWORD statusSize = sizeof(statusCode);
            WinHttpQueryHeaders(hReq,
                                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX,
                                &statusCode, &statusSize,
                                WINHTTP_NO_HEADER_INDEX);
            success = (statusCode >= 200 && statusCode < 300);
            if (debugMode) {
                char msg[64];
                snprintf(msg, sizeof(msg), "Treblle: ingress responded HTTP %lu", statusCode);
                LogDebug(msg, true);
            }

            if (statusCode == 429) {
                // Read Retry-After (numeric seconds); fall back to default.
                WCHAR retryBuf[32] = {};
                DWORD retryLen     = sizeof(retryBuf);
                DWORD retryIndex   = WINHTTP_NO_HEADER_INDEX;
                DWORD delaySec     = 0;
                if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CUSTOM, L"Retry-After",
                                        retryBuf, &retryLen, &retryIndex))
                    delaySec = (DWORD)_wtol(retryBuf);
                if (delaySec == 0 || delaySec > 3600) delaySec = kRetryAfterDefaultMs / 1000;
                retryAfterOpenedMs_ = GetTickCount();
                retryAfterDelayMs_  = delaySec * 1000;
                if (debugMode) {
                    char msg[64];
                    snprintf(msg, sizeof(msg),
                             "Treblle: 429 received — backing off for %lu s", delaySec);
                    LogDebug(msg, true);
                }
            }

            // Drain response body to allow the connection to be reused via keep-alive.
            DWORD dwSize = 0;
            do {
                dwSize = 0;
                if (!WinHttpQueryDataAvailable(hReq, &dwSize) || dwSize == 0) break;
                std::vector<char> buf(dwSize);
                DWORD dwDownloaded = 0;
                if (!WinHttpReadData(hReq, buf.data(), dwSize, &dwDownloaded)) break;
            } while (dwSize > 0);
        }
    } else if (debugMode) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Treblle: WinHttpSendRequest failed (0x%08lX)", GetLastError());
        LogDebug(msg, true);
    }

    WinHttpCloseHandle(hReq);

    // Circuit breaker tracking: only 5xx and network failures (statusCode == 0) count.
    // 4xx responses mean the server is reachable — don't penalise the breaker.
    if (success) {
        consecutiveFailures_ = 0;
    } else if (statusCode == 0 || statusCode >= 500) {
        ++consecutiveFailures_;
        if (consecutiveFailures_ == kTripThreshold) {
            openedAtMs_ = GetTickCount();
            if (debugMode)
                LogDebug("Treblle: circuit breaker tripped — pausing sends for 30s", true);
        }
    }

    return success;
}
