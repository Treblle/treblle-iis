#include "precomp.h"
#include "TreblleModule.h"
#include "Config.h"
#include "BodyCapture.h"
#include "PayloadBuilder.h"
#include "HttpSender.h"
#include "Utils.h"

// ── Globals ───────────────────────────────────────────────────────────────────

AsyncQueue* g_pQueue        = nullptr;
HANDLE      g_hWorkerThread = nullptr;
HMODULE     g_hModule       = nullptr;

// ── DllMain ───────────────────────────────────────────────────────────────────

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

// ── Background worker ─────────────────────────────────────────────────────────

static DWORD WINAPI WorkerThreadProc(LPVOID) {
    HttpSender sender;
    while (true) {
        std::string payload;
        if (!g_pQueue->Pop(payload, 1000)) {
            if (g_pQueue->IsShutdown()) break;
            continue;
        }
        TreblleConfig cfg = Config::Instance().Get();
        sender.Send(payload, cfg.treblleUrl, cfg.sdkToken, cfg.debugMode);
    }
    // Drain remaining entries on shutdown
    std::string payload;
    while (g_pQueue->Pop(payload, 0)) {
        TreblleConfig cfg = Config::Instance().Get();
        HttpSender().Send(payload, cfg.treblleUrl, cfg.sdkToken, cfg.debugMode);
    }
    return 0;
}

// ── Factory ───────────────────────────────────────────────────────────────────

HRESULT CTreblleModuleFactory::GetHttpModule(OUT CHttpModule** ppModule,
                                              IN  IModuleAllocator*) {
    *ppModule = new(std::nothrow) CTreblleModule();
    return *ppModule ? S_OK : E_OUTOFMEMORY;
}

void CTreblleModuleFactory::Terminate() {
    // Signal the background worker to drain remaining payloads and exit.
    if (g_pQueue) {
        g_pQueue->Shutdown();
    }
    if (g_hWorkerThread) {
        // Give the worker up to 5 seconds to send any queued payloads.
        WaitForSingleObject(g_hWorkerThread, 5000);
        CloseHandle(g_hWorkerThread);
        g_hWorkerThread = nullptr;
    }
    delete g_pQueue;
    g_pQueue = nullptr;
    delete this;
}

// ── RegisterModule (IIS entry point) ─────────────────────────────────────────

HRESULT __stdcall RegisterModule(DWORD,
                                  IHttpModuleRegistrationInfo* pInfo,
                                  IHttpServer*) {
    // Locate and load treblle.config from the same directory as this DLL
    WCHAR dllPath[MAX_PATH] = {};
    GetModuleFileNameW(g_hModule, dllPath, MAX_PATH);

    bool loaded = Config::Instance().Load(dllPath);
    TreblleConfig cfg = Config::Instance().Get();
    bool dbg = cfg.debugMode;

    char narrowPath[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, dllPath, -1, narrowPath, MAX_PATH, nullptr, nullptr);
    LogDebug(std::string("RegisterModule — DLL path: ") + narrowPath, dbg);
    LogDebug(std::string("Config loaded: ") + (loaded ? "true" : "false"), dbg);
    LogDebug("Excluded routes count: " + std::to_string(cfg.excludeRoutes.size()), dbg);

    // Start the background sender thread
    g_pQueue = new(std::nothrow) AsyncQueue();
    if (!g_pQueue) return E_OUTOFMEMORY;
    g_hWorkerThread = CreateThread(nullptr, 0, WorkerThreadProc, nullptr, 0, nullptr);

    HRESULT hr = pInfo->SetRequestNotifications(
        new(std::nothrow) CTreblleModuleFactory(),
        RQ_BEGIN_REQUEST | RQ_SEND_RESPONSE | RQ_END_REQUEST,
        0);

    LogDebug(std::string("SetRequestNotifications hr=") + std::to_string(hr), dbg);
    return hr;
}

// ── Helper methods ────────────────────────────────────────────────────────────

std::string CTreblleModule::GetMethodString(HTTP_VERB verb, PCSTR pUnknown, USHORT unknownLen) {
    switch (verb) {
        case HttpVerbGET:     return "GET";
        case HttpVerbPOST:    return "POST";
        case HttpVerbPUT:     return "PUT";
        case HttpVerbDELETE:  return "DELETE";
        case HttpVerbHEAD:    return "HEAD";
        case HttpVerbOPTIONS: return "OPTIONS";
        case HttpVerbPATCH:   return "PATCH";
        default:
            if (pUnknown && unknownLen > 0)
                return std::string(pUnknown, unknownLen);
            return "UNKNOWN";
    }
}

bool CTreblleModule::IsTrackedMethod(const std::string& method) {
    static const char* kTracked[] = {
        "GET","POST","PUT","PATCH","DELETE","HEAD","OPTIONS"
    };
    for (const char* m : kTracked)
        if (_stricmp(method.c_str(), m) == 0) return true;
    return false;
}

void CTreblleModule::CollectRequestHeaders(HTTP_REQUEST* pRaw) {
    static const struct { int id; const char* name; } kKnown[] = {
        { HttpHeaderHost,            "host" },
        { HttpHeaderContentType,     "content-type" },
        { HttpHeaderContentLength,   "content-length" },
        { HttpHeaderAccept,          "accept" },
        { HttpHeaderAcceptEncoding,  "accept-encoding" },
        { HttpHeaderAcceptLanguage,  "accept-language" },
        { HttpHeaderAuthorization,   "authorization" },
        { HttpHeaderUserAgent,       "user-agent" },
        { HttpHeaderReferer,         "referer" },
        { HttpHeaderCacheControl,    "cache-control" },
        { HttpHeaderConnection,      "connection" },
        { HttpHeaderCookie,          "cookie" },
        { HttpHeaderTransferEncoding,"transfer-encoding" },
    };
    for (const auto& h : kKnown) {
        PCSTR  val = pRaw->Headers.KnownHeaders[h.id].pRawValue;
        USHORT len = pRaw->Headers.KnownHeaders[h.id].RawValueLength;
        if (val && len > 0)
            ctx_.requestHeaders[h.name] = std::string(val, len);
    }
    for (USHORT i = 0; i < pRaw->Headers.UnknownHeaderCount; ++i) {
        const HTTP_UNKNOWN_HEADER& uh = pRaw->Headers.pUnknownHeaders[i];
        if (uh.pName && uh.NameLength > 0 && uh.pRawValue && uh.RawValueLength > 0)
            ctx_.requestHeaders[ToLower(std::string(uh.pName, uh.NameLength))] =
                std::string(uh.pRawValue, uh.RawValueLength);
    }
}

void CTreblleModule::CollectResponseHeaders(HTTP_RESPONSE* pRaw) {
    static const struct { int id; const char* name; } kKnown[] = {
        { HttpHeaderContentType,      "content-type" },
        { HttpHeaderContentLength,    "content-length" },
        { HttpHeaderCacheControl,     "cache-control" },
        { HttpHeaderTransferEncoding, "transfer-encoding" },
        { HttpHeaderServer,           "server" },
        { HttpHeaderDate,             "date" },
        { HttpHeaderEtag,             "etag" },
        { HttpHeaderLocation,         "location" },
    };
    for (const auto& h : kKnown) {
        PCSTR  val = pRaw->Headers.KnownHeaders[h.id].pRawValue;
        USHORT len = pRaw->Headers.KnownHeaders[h.id].RawValueLength;
        if (val && len > 0)
            ctx_.responseHeaders[h.name] = std::string(val, len);
    }
    for (USHORT i = 0; i < pRaw->Headers.UnknownHeaderCount; ++i) {
        const HTTP_UNKNOWN_HEADER& uh = pRaw->Headers.pUnknownHeaders[i];
        if (uh.pName && uh.NameLength > 0 && uh.pRawValue && uh.RawValueLength > 0)
            ctx_.responseHeaders[ToLower(std::string(uh.pName, uh.NameLength))] =
                std::string(uh.pRawValue, uh.RawValueLength);
    }
}

std::string CTreblleModule::BuildFullUrl(IHttpContext* pCtx, HTTP_REQUEST* pRaw) {
    // Determine scheme via HTTPS server variable
    DWORD  cbHttps = 0;
    PCSTR  pHttps  = nullptr;
    bool isSecure  = SUCCEEDED(pCtx->GetServerVariable("HTTPS", &pHttps, &cbHttps))
                     && pHttps && cbHttps > 0
                     && _strnicmp(pHttps, "on", 2) == 0;
    std::string scheme = isSecure ? "https" : "http";

    // Host
    PCSTR pHost = pRaw->Headers.KnownHeaders[HttpHeaderHost].pRawValue;
    std::string host = pHost ? pHost : "";

    // Raw URL (path + query)
    std::string rawUrl = pRaw->pRawUrl ? pRaw->pRawUrl : "";

    return scheme + "://" + host + rawUrl;
}

// ── OnBeginRequest ────────────────────────────────────────────────────────────

REQUEST_NOTIFICATION_STATUS CTreblleModule::OnBeginRequest(
    IHttpContext* pCtx, IHttpEventProvider*) {
    try {
        Config::Instance().CheckReload();
        TreblleConfig cfg = Config::Instance().Get();

        if (!cfg.loaded) return RQ_NOTIFICATION_CONTINUE;
        bool dbg = cfg.debugMode;
        ctx_.debugMode = dbg;

        IHttpRequest* pReq = pCtx->GetRequest();
        HTTP_REQUEST* pRaw = pReq->GetRawHttpRequest();
        if (!pRaw) return RQ_NOTIFICATION_CONTINUE;

        // Extract host (lowercase, strip port)
        PCSTR pHostRaw = pRaw->Headers.KnownHeaders[HttpHeaderHost].pRawValue;
        std::string host = pHostRaw ? ToLower(pHostRaw) : "";
        size_t colon = host.rfind(':');
        if (colon != std::string::npos) host = host.substr(0, colon);

        // Extract URL path (no query string)
        std::string rawUrl = pRaw->pRawUrl ? pRaw->pRawUrl : "";
        std::string path   = ParseQueryPath(rawUrl);

        // Exclusion check — skip hosts/paths the user opted out of
        if (Config::Instance().IsExcluded(host, path)) {
            LogDebug("skip host=" + host + " url=" + path + " — matched exclude_routes", dbg);
            return RQ_NOTIFICATION_CONTINUE;
        }

        // Stable identity derived from host
        ctx_.internalId   = ComputeHostId(host);
        ctx_.internalName = host;

        // Method check
        std::string method = GetMethodString(pRaw->Verb,
                                              pRaw->pUnknownVerb,
                                              pRaw->UnknownVerbLength);
        if (!IsTrackedMethod(method)) {
            LogDebug("skip host=" + host + " url=" + path + " — method " + method + " not tracked", dbg);
            return RQ_NOTIFICATION_CONTINUE;
        }

        // Passed all checks — mark for tracking
        ctx_.shouldTrack = true;
        QueryPerformanceFrequency(&ctx_.frequency);
        QueryPerformanceCounter(&ctx_.startTime);

        ctx_.method    = method;
        ctx_.url       = BuildFullUrl(pCtx, pRaw);
        ctx_.routePath = path;
        ctx_.clientIp  = GetClientIP(pCtx);
        ctx_.queryParams = ParseQueryString(rawUrl);

        // Determine protocol
        ctx_.protocol = (pRaw->Version.MajorVersion == 2) ? "HTTP/2" : "HTTP/1.1";

        // Collect request headers
        CollectRequestHeaders(pRaw);
        auto ua = ctx_.requestHeaders.find("user-agent");
        ctx_.userAgent = (ua != ctx_.requestHeaders.end()) ? ua->second : "";

        // Request body — JSON captured verbatim; file uploads replaced with metadata
        PCSTR pCT = pRaw->Headers.KnownHeaders[HttpHeaderContentType].pRawValue;
        std::string ct = pCT ? ToLower(pCT) : "";

        if (ct.find("application/json") != std::string::npos) {
            ctx_.requestBody = ReadRequestBody(pCtx, ctx_.requestBodyTruncated);
        } else if (ct.find("multipart/form-data") != std::string::npos) {
            PCSTR pCL = pRaw->Headers.KnownHeaders[HttpHeaderContentLength].pRawValue;
            LONGLONG cl = pCL ? _atoi64(pCL) : 0;
            ctx_.requestBody = SummarizeMultipartBody(pCtx, pCT, cl);
        }
    } catch (...) {}

    return RQ_NOTIFICATION_CONTINUE;
}

// ── OnSendResponse ────────────────────────────────────────────────────────────

REQUEST_NOTIFICATION_STATUS CTreblleModule::OnSendResponse(
    IHttpContext* pCtx, ISendResponseProvider*) {
    if (!ctx_.shouldTrack) return RQ_NOTIFICATION_CONTINUE;
    try {
        IHttpResponse* pResp = pCtx->GetResponse();
        HTTP_RESPONSE* pRaw  = pResp->GetRawHttpResponse();
        if (!pRaw) return RQ_NOTIFICATION_CONTINUE;

        // On first call: check response Content-Type and collect response headers
        if (!ctx_.responseHeadersDone) {
            PCSTR pCT  = pRaw->Headers.KnownHeaders[HttpHeaderContentType].pRawValue;
            std::string ct = pCT ? ToLower(pCT) : "";
            bool isJson = ct.find("application/json") != std::string::npos;
            if (!isJson) {
                LogDebug("skip host=" + ctx_.internalName + " url=" + ctx_.routePath
                    + " — response Content-Type \"" + ct + "\" is not JSON", ctx_.debugMode);
                ctx_.shouldTrack = false;
                return RQ_NOTIFICATION_CONTINUE;
            }
            USHORT status = 0;
            pResp->GetStatus(&status);
            ctx_.statusCode = status;
            CollectResponseHeaders(pRaw);
            ctx_.responseHeadersDone = true;
        }

        // Accumulate body chunks (may be called multiple times for chunked responses)
        CaptureResponseChunks(pRaw, ctx_.responseBody, ctx_.responseSize, ctx_.responseBodyTruncated);
    } catch (...) {}

    return RQ_NOTIFICATION_CONTINUE;
}

// ── OnEndRequest ──────────────────────────────────────────────────────────────

REQUEST_NOTIFICATION_STATUS CTreblleModule::OnEndRequest(
    IHttpContext* pCtx, IHttpEventProvider*) {
    if (!ctx_.shouldTrack || !ctx_.responseHeadersDone) return RQ_NOTIFICATION_CONTINUE;
    try {
        LARGE_INTEGER endTime;
        QueryPerformanceCounter(&endTime);

        double loadTimeMs = 0.0;
        if (ctx_.frequency.QuadPart > 0) {
            loadTimeMs = static_cast<double>(endTime.QuadPart - ctx_.startTime.QuadPart)
                         / static_cast<double>(ctx_.frequency.QuadPart) * 1000.0;
        }

        TreblleConfig cfg = Config::Instance().Get();
        std::string payload = PayloadBuilder::Build(ctx_, cfg, loadTimeMs, pCtx);

        if (g_pQueue) g_pQueue->Push(std::move(payload));
    } catch (...) {}

    return RQ_NOTIFICATION_CONTINUE;
}
