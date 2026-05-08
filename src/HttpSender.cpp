#include "precomp.h"
#include "HttpSender.h"
#include "Utils.h"

HttpSender::HttpSender()
    : hSession_(WinHttpOpen(L"TreblleIISModule/1.0",
                             WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS,
                             0)) {
    if (hSession_) {
        DWORD timeout = 10000; // 10 s
        WinHttpSetOption(hSession_, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
        WinHttpSetOption(hSession_, WINHTTP_OPTION_SEND_TIMEOUT,    &timeout, sizeof(timeout));
        WinHttpSetOption(hSession_, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    }
}

HttpSender::~HttpSender() {
    if (hSession_) WinHttpCloseHandle(hSession_);
}

bool HttpSender::Send(const std::string& jsonPayload,
                      const std::string& url,
                      const std::string& sdkToken,
                      bool               debugMode) {
    if (!hSession_ || jsonPayload.empty() || url.empty()) return false;

    if (debugMode) {
        std::string preview = jsonPayload.substr(0, 500);
        if (jsonPayload.size() > 500) preview += "...";
        LogDebug("Treblle: sending payload: " + preview);
    }

    // Convert URL to wide string for WinHTTP
    int wlen = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring wUrl(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, &wUrl[0], wlen);

    // Crack the URL into components
    URL_COMPONENTS uc = {};
    uc.dwStructSize      = sizeof(uc);
    uc.dwHostNameLength  = (DWORD)-1;
    uc.dwUrlPathLength   = (DWORD)-1;
    uc.dwSchemeLength    = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wUrl.c_str(), 0, 0, &uc)) {
        if (debugMode) LogDebug("Treblle: WinHttpCrackUrl failed for URL: " + url);
        return false;
    }

    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength > 0) path += std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    INTERNET_PORT port = uc.nPort ? uc.nPort : INTERNET_DEFAULT_HTTPS_PORT;
    bool isHttps = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET hConn = WinHttpConnect(hSession_, host.c_str(), port, 0);
    if (!hConn) {
        if (debugMode) LogDebug("Treblle: WinHttpConnect failed");
        return false;
    }

    DWORD flags   = isHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"POST", path.c_str(),
                                        nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) {
        if (debugMode) LogDebug("Treblle: WinHttpOpenRequest failed");
        WinHttpCloseHandle(hConn);
        return false;
    }

    // Build headers
    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!sdkToken.empty()) {
        int wTokenLen = MultiByteToWideChar(CP_UTF8, 0, sdkToken.c_str(), -1, nullptr, 0);
        if (wTokenLen > 1) {
            std::wstring wToken(wTokenLen, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, sdkToken.c_str(), -1, &wToken[0], wTokenLen);
            wToken.resize(wTokenLen - 1); // strip null terminator before concatenating
            headers += L"x-api-key: " + wToken + L"\r\n";
        }
    }

    BOOL ok = WinHttpSendRequest(hReq,
                                  headers.c_str(),
                                  (DWORD)headers.size(),
                                  (LPVOID)jsonPayload.c_str(),
                                  (DWORD)jsonPayload.size(),
                                  (DWORD)jsonPayload.size(),
                                  0);
    bool success = false;
    if (ok) {
        ok = WinHttpReceiveResponse(hReq, nullptr);
        if (ok) {
            DWORD statusCode = 0;
            DWORD statusSize = sizeof(statusCode);
            WinHttpQueryHeaders(hReq,
                                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX,
                                &statusCode, &statusSize,
                                WINHTTP_NO_HEADER_INDEX);
            success = (statusCode >= 200 && statusCode < 300);
            if (!success && debugMode) {
                char msg[64];
                snprintf(msg, sizeof(msg), "Treblle: ingress returned HTTP %lu", statusCode);
                LogDebug(msg);
            }
        }
    } else if (debugMode) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Treblle: WinHttpSendRequest failed (0x%08lX)", GetLastError());
        LogDebug(msg);
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    return success;
}
