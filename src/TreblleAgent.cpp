#include "precomp.h"
#include "TreblleAgent.h"
#include "Config.h"
#include "BodyCapture.h"
#include "PayloadBuilder.h"
#include "HttpSender.h"
#include "Constants.h"
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
    try {
        HttpSender sender;
        while (true) {
            std::string payload;
            if (!g_pQueue->Pop(payload, INFINITE)) break;
            auto cfg = Config::Instance().Get();
            sender.Send(payload, cfg->treblleUrl, cfg->sdkToken, cfg->debugMode);
        }
        // Drain any payloads already in the queue before exiting
        std::string payload;
        while (g_pQueue->Pop(payload, 0)) {
            auto cfg = Config::Instance().Get();
            HttpSender().Send(payload, cfg->treblleUrl, cfg->sdkToken, cfg->debugMode);
        }
    } catch (...) {
        LogDebug("Treblle: unhandled exception in worker thread — thread exiting", true);
    }
    return 0;
}

// ── Factory ───────────────────────────────────────────────────────────────────

HRESULT CTreblleAgentFactory::GetHttpModule(OUT CHttpModule** ppModule,
                                              IN  IModuleAllocator*) {
    *ppModule = new(std::nothrow) CTreblleAgent();
    return *ppModule ? S_OK : E_OUTOFMEMORY;
}

void CTreblleAgentFactory::Terminate() {
    if (g_pQueue) g_pQueue->Shutdown();
    if (g_hWorkerThread) {
        DWORD waitResult = WaitForSingleObject(g_hWorkerThread, TreblleConst::kShutdownDrainMs);
        CloseHandle(g_hWorkerThread);
        g_hWorkerThread = nullptr;
        if (waitResult == WAIT_OBJECT_0) {
            // Thread exited cleanly — safe to delete queue
            delete g_pQueue;
            g_pQueue = nullptr;
        } else {
            // Thread did not exit in time — leave queue alive to prevent crash
            LogDebug("Treblle: worker thread did not exit within drain timeout — leaking queue to prevent crash", true);
        }
    } else {
        delete g_pQueue;
        g_pQueue = nullptr;
    }
    delete this;
}

// ── RegisterModule (IIS entry point) ─────────────────────────────────────────

HRESULT __stdcall RegisterModule(DWORD,
                                  IHttpModuleRegistrationInfo* pInfo,
                                  IHttpServer*) {
    try {
        WCHAR dllPath[MAX_PATH] = {};
        GetModuleFileNameW(g_hModule, dllPath, MAX_PATH);

        char narrowPath[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, dllPath, -1, narrowPath, MAX_PATH, nullptr, nullptr);

        // Always log startup — debug flag not yet known, use raw Event Log write
        LogDebug(std::string("Treblle: RegisterModule starting — DLL: ") + narrowPath, true);

        bool loaded = Config::Instance().Load(dllPath);
        auto cfg    = Config::Instance().Get();
        bool dbg    = cfg->debugMode;

        if (!loaded) {
            LogDebug("Treblle: config failed to load — check api_key and sdk_token", true);
        }

        g_pQueue = new(std::nothrow) AsyncQueue();
        if (!g_pQueue) {
            LogDebug("Treblle: failed to allocate AsyncQueue — E_OUTOFMEMORY", true);
            return E_OUTOFMEMORY;
        }

        g_hWorkerThread = CreateThread(nullptr, 0, WorkerThreadProc, nullptr, 0, nullptr);
        if (!g_hWorkerThread) {
            LogDebug("Treblle: CreateThread failed — worker will not run", true);
        }

        auto* pFactory = new(std::nothrow) CTreblleAgentFactory();
        if (!pFactory) {
            LogDebug("Treblle: failed to allocate CTreblleAgentFactory — E_OUTOFMEMORY", true);
            if (g_pQueue) { g_pQueue->Shutdown(); delete g_pQueue; g_pQueue = nullptr; }
            return E_OUTOFMEMORY;
        }

        HRESULT hr = pInfo->SetRequestNotifications(
            pFactory,
            RQ_BEGIN_REQUEST | RQ_SEND_RESPONSE | RQ_END_REQUEST,
            0);

        if (FAILED(hr)) {
            LogDebug("Treblle: SetRequestNotifications failed — cleaning up", true);
            if (g_pQueue) { g_pQueue->Shutdown(); delete g_pQueue; g_pQueue = nullptr; }
            if (g_hWorkerThread) { CloseHandle(g_hWorkerThread); g_hWorkerThread = nullptr; }
            return hr;
        }

        LogDebug("Treblle: agent registered successfully — DLL: " + std::string(narrowPath), true);
        return hr;
    } catch (...) {
        LogDebug("Treblle: unhandled exception in RegisterModule", true);
        return E_FAIL;
    }
}

// ── Header collection helper ──────────────────────────────────────────────────

struct HeaderEntry { int id; const char* name; };

template<typename THeaders>
static void FillHeaderMap(const THeaders&                     hdrs,
                          std::map<std::string, std::string>& out,
                          const HeaderEntry*                  known,
                          size_t                              count) {
    for (size_t i = 0; i < count; ++i) {
        PCSTR  val = hdrs.KnownHeaders[known[i].id].pRawValue;
        USHORT len = hdrs.KnownHeaders[known[i].id].RawValueLength;
        if (val && len > 0)
            out[known[i].name] = std::string(val, len);
    }
    for (USHORT i = 0; i < hdrs.UnknownHeaderCount; ++i) {
        const HTTP_UNKNOWN_HEADER& uh = hdrs.pUnknownHeaders[i];
        if (uh.pName && uh.NameLength > 0 && uh.pRawValue && uh.RawValueLength > 0)
            out[ToLower(std::string(uh.pName, uh.NameLength))] =
                std::string(uh.pRawValue, uh.RawValueLength);
    }
}

// ── Helper methods ────────────────────────────────────────────────────────────

std::string CTreblleAgent::GetMethodString(HTTP_VERB verb, PCSTR pUnknown, USHORT unknownLen) {
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

bool CTreblleAgent::IsTrackedMethod(const std::string& method) {
    static const char* kTracked[] = {
        "GET","POST","PUT","PATCH","DELETE","HEAD","OPTIONS"
    };
    for (const char* m : kTracked)
        if (_stricmp(method.c_str(), m) == 0) return true;
    return false;
}

void CTreblleAgent::CollectRequestHeaders(HTTP_REQUEST* pRaw) {
    static const HeaderEntry kKnown[] = {
        { HttpHeaderHost,             "host" },
        { HttpHeaderContentType,      "content-type" },
        { HttpHeaderContentLength,    "content-length" },
        { HttpHeaderAccept,           "accept" },
        { HttpHeaderAcceptEncoding,   "accept-encoding" },
        { HttpHeaderAcceptLanguage,   "accept-language" },
        { HttpHeaderAuthorization,    "authorization" },
        { HttpHeaderUserAgent,        "user-agent" },
        { HttpHeaderReferer,          "referer" },
        { HttpHeaderCacheControl,     "cache-control" },
        { HttpHeaderConnection,       "connection" },
        { HttpHeaderCookie,           "cookie" },
        { HttpHeaderTransferEncoding, "transfer-encoding" },
    };
    FillHeaderMap(pRaw->Headers, ctx_.requestHeaders, kKnown, ARRAYSIZE(kKnown));
}

void CTreblleAgent::CollectResponseHeaders(HTTP_RESPONSE* pRaw) {
    static const HeaderEntry kKnown[] = {
        { HttpHeaderContentType,      "content-type" },
        { HttpHeaderContentLength,    "content-length" },
        { HttpHeaderCacheControl,     "cache-control" },
        { HttpHeaderTransferEncoding, "transfer-encoding" },
        { HttpHeaderServer,           "server" },
        { HttpHeaderDate,             "date" },
        { HttpHeaderEtag,             "etag" },
        { HttpHeaderLocation,         "location" },
    };
    FillHeaderMap(pRaw->Headers, ctx_.responseHeaders, kKnown, ARRAYSIZE(kKnown));
}

std::string CTreblleAgent::BuildFullUrl(IHttpContext* pCtx, HTTP_REQUEST* pRaw) {
    DWORD  cbHttps = 0;
    PCSTR  pHttps  = nullptr;
    bool isSecure  = SUCCEEDED(pCtx->GetServerVariable("HTTPS", &pHttps, &cbHttps))
                     && pHttps && cbHttps > 0
                     && _strnicmp(pHttps, "on", 2) == 0;
    std::string scheme = isSecure ? "https" : "http";

    PCSTR  pHost    = pRaw->Headers.KnownHeaders[HttpHeaderHost].pRawValue;
    USHORT hostLen  = pRaw->Headers.KnownHeaders[HttpHeaderHost].RawValueLength;
    std::string host = (pHost && hostLen > 0) ? std::string(pHost, hostLen) : "";

    std::string rawUrl = pRaw->pRawUrl ? pRaw->pRawUrl : "";
    return scheme + "://" + host + rawUrl;
}

// ── OnBeginRequest ────────────────────────────────────────────────────────────

REQUEST_NOTIFICATION_STATUS CTreblleAgent::OnBeginRequest(
    IHttpContext* pCtx, IHttpEventProvider*) {
    try {
        Config::Instance().CheckReload();
        auto cfg = Config::Instance().Get();

        if (!cfg->loaded) {
            LogDebug("Treblle: OnBeginRequest — config not loaded, skipping", true);
            return RQ_NOTIFICATION_CONTINUE;
        }
        if (cfg->disabled) {
            LogDebug("Treblle: OnBeginRequest — agent disabled, skipping", cfg->debugMode);
            return RQ_NOTIFICATION_CONTINUE;
        }
        bool dbg = cfg->debugMode;
        ctx_.debugMode = dbg;

        IHttpRequest* pReq = pCtx->GetRequest();
        HTTP_REQUEST* pRaw = pReq->GetRawHttpRequest();
        if (!pRaw) {
            LogDebug("Treblle: OnBeginRequest — null raw request, skipping", dbg);
            return RQ_NOTIFICATION_CONTINUE;
        }

        PCSTR pHostRaw = pRaw->Headers.KnownHeaders[HttpHeaderHost].pRawValue;
        std::string host = pHostRaw ? ToLower(pHostRaw) : "";
        size_t colon = host.rfind(':');
        if (colon != std::string::npos) host = host.substr(0, colon);

        std::string rawUrl = pRaw->pRawUrl ? pRaw->pRawUrl : "";
        std::string path   = ParseQueryPath(rawUrl);

        std::string method = GetMethodString(pRaw->Verb,
                                              pRaw->pUnknownVerb,
                                              pRaw->UnknownVerbLength);

        std::string defaultMatch = Config::MatchDefaultPath(path);
        if (!defaultMatch.empty()) {
            LogDebug("Treblle: skip — default excluded path prefix \"" + defaultMatch + "\": " + path, dbg);
            return RQ_NOTIFICATION_CONTINUE;
        }

        if (Config::Instance().IsExcluded(host, path)) {
            LogDebug("Treblle: skip — matched exclude_routes: " + host + path, dbg);
            return RQ_NOTIFICATION_CONTINUE;
        }

        if (!IsTrackedMethod(method)) {
            LogDebug("Treblle: skip — method not tracked: " + method, dbg);
            return RQ_NOTIFICATION_CONTINUE;
        }

        ctx_.internalId   = ComputeHostId(host);
        ctx_.internalName = host;
        ctx_.shouldTrack = true;
        QueryPerformanceFrequency(&ctx_.frequency);
        QueryPerformanceCounter(&ctx_.startTime);

        ctx_.method    = method;
        ctx_.url       = BuildFullUrl(pCtx, pRaw);
        ctx_.routePath = path;
        ctx_.clientIp  = GetClientIP(pCtx);
        ctx_.queryParams = ParseQueryString(rawUrl);

        ctx_.protocol = (pRaw->Version.MajorVersion == 2) ? "HTTP/2" : "HTTP/1.1";

        CollectRequestHeaders(pRaw);
        auto ua = ctx_.requestHeaders.find("user-agent");
        ctx_.userAgent = (ua != ctx_.requestHeaders.end()) ? ua->second : "";

        PCSTR pCT = pRaw->Headers.KnownHeaders[HttpHeaderContentType].pRawValue;
        std::string ct = pCT ? ToLower(pCT) : "";

        if (ct.find("application/json") != std::string::npos) {
            ctx_.requestBody = ReadRequestBody(pCtx, ctx_.requestBodyTruncated);
        } else if (ct.find("multipart/form-data") != std::string::npos) {
            PCSTR pCL = pRaw->Headers.KnownHeaders[HttpHeaderContentLength].pRawValue;
            LONGLONG cl = pCL ? _atoi64(pCL) : 0;
            ctx_.requestBody = SummarizeMultipartBody(pCtx, pCT, cl);
        }
    } catch (...) {
        LogDebug("Treblle: unhandled exception in OnBeginRequest", true);
    }

    return RQ_NOTIFICATION_CONTINUE;
}

// ── OnSendResponse ────────────────────────────────────────────────────────────

REQUEST_NOTIFICATION_STATUS CTreblleAgent::OnSendResponse(
    IHttpContext* pCtx, ISendResponseProvider*) {
    if (!ctx_.shouldTrack) return RQ_NOTIFICATION_CONTINUE;
    try {
        IHttpResponse* pResp = pCtx->GetResponse();
        HTTP_RESPONSE* pRaw  = pResp->GetRawHttpResponse();
        if (!pRaw) {
            LogDebug("Treblle: OnSendResponse — null raw response, skipping", ctx_.debugMode);
            return RQ_NOTIFICATION_CONTINUE;
        }

        if (!ctx_.responseHeadersDone) {
            PCSTR pCT  = pRaw->Headers.KnownHeaders[HttpHeaderContentType].pRawValue;
            std::string ct = pCT ? ToLower(pCT) : "";
            bool isJson = ct.find("application/json") != std::string::npos;
            if (!isJson) {
                LogDebug("Treblle: skip — response Content-Type \"" + ct + "\" is not JSON: "
                    + ctx_.internalName + ctx_.routePath, ctx_.debugMode);
                ctx_.shouldTrack = false;
                return RQ_NOTIFICATION_CONTINUE;
            }
            USHORT status = 0;
            pResp->GetStatus(&status);
            ctx_.statusCode = status;
            CollectResponseHeaders(pRaw);
            ctx_.responseHeadersDone = true;
        }

        CaptureResponseChunks(pRaw, ctx_.responseBody, ctx_.responseSize, ctx_.responseBodyTruncated);
    } catch (...) {
        LogDebug("Treblle: unhandled exception in OnSendResponse", true);
    }

    return RQ_NOTIFICATION_CONTINUE;
}

// ── OnEndRequest ──────────────────────────────────────────────────────────────

REQUEST_NOTIFICATION_STATUS CTreblleAgent::OnEndRequest(
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

        auto cfg = Config::Instance().Get();
        std::string payload = PayloadBuilder::Build(ctx_, *cfg, loadTimeMs, GetIISVersion(pCtx));

        if (g_pQueue) {
            g_pQueue->Push(std::move(payload));
        } else {
            LogDebug("Treblle: queue is null — payload dropped", true);
        }
    } catch (...) {
        LogDebug("Treblle: unhandled exception in OnEndRequest", true);
    }

    return RQ_NOTIFICATION_CONTINUE;
}
