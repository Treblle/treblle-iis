#include "precomp.h"
#include "TreblleAgent.h"
#include "Config.h"
#include "BodyCapture.h"
#include "PayloadBuilder.h"
#include "HttpSender.h"
#include "Constants.h"
#include "Utils.h"

// ── Globals ───────────────────────────────────────────────────────────────────

AsyncQueue* g_pQueue                 = nullptr;
HANDLE      g_hWorkerThread          = nullptr;
AsyncQueue* g_pSecondaryQueue        = nullptr;
HANDLE      g_hSecondaryWorkerThread = nullptr;
HMODULE     g_hModule                = nullptr;

// ── DllMain ───────────────────────────────────────────────────────────────────

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

// ── Background worker ─────────────────────────────────────────────────────────
//
// Primary and secondary destinations each get their own queue, worker thread and
// HttpSender (own circuit breaker / 429 backoff state). This is deliberate: if the
// on-prem primary endpoint is unreachable, sends to it will block for up to
// kHttpTimeoutMs each before failing. Sharing one thread between destinations
// would let a stalled/timing-out primary delay or starve the secondary — exactly
// backwards for a customer whose reason for wanting a secondary is that the
// primary drops out.

static DWORD RunWorkerLoop(AsyncQueue* queue, bool secondary) {
    try {
        HttpSender sender;
        while (true) {
            std::string payload;
            if (!queue->Pop(payload, INFINITE)) break;
            auto cfg = Config::Instance().Get();
            const std::string& url = secondary ? cfg->secondaryTreblleUrl : cfg->treblleUrl;
            if (secondary && url.empty()) continue; // disabled mid-flight — drop
            sender.Send(payload, url, cfg->sdkToken, cfg->debugMode);
        }
        // Drain any payloads already in the queue before exiting
        std::string payload;
        while (queue->Pop(payload, 0)) {
            auto cfg = Config::Instance().Get();
            const std::string& url = secondary ? cfg->secondaryTreblleUrl : cfg->treblleUrl;
            if (secondary && url.empty()) continue;
            HttpSender().Send(payload, url, cfg->sdkToken, cfg->debugMode);
        }
    } catch (...) {
        LogDebug("Treblle: unhandled exception in worker thread — thread exiting", true);
    }
    return 0;
}

static DWORD WINAPI WorkerThreadProc(LPVOID)          { return RunWorkerLoop(g_pQueue, false); }
static DWORD WINAPI SecondaryWorkerThreadProc(LPVOID) { return RunWorkerLoop(g_pSecondaryQueue, true); }

// ── Factory ───────────────────────────────────────────────────────────────────

HRESULT CTreblleAgentFactory::GetHttpModule(OUT CHttpModule** ppModule,
                                              IN  IModuleAllocator*) {
    *ppModule = new(std::nothrow) CTreblleAgent();
    return *ppModule ? S_OK : E_OUTOFMEMORY;
}

void CTreblleAgentFactory::Terminate() {
    if (g_pQueue)          g_pQueue->Shutdown();
    if (g_pSecondaryQueue) g_pSecondaryQueue->Shutdown();

    // Wait for both threads together, bounded by ONE kShutdownDrainMs window total —
    // not kShutdownDrainMs per thread — so enabling a secondary destination doesn't
    // double how long IIS waits during an app-pool recycle/shutdown.
    HANDLE handles[2];
    DWORD  handleCount = 0;
    if (g_hWorkerThread)          handles[handleCount++] = g_hWorkerThread;
    if (g_hSecondaryWorkerThread) handles[handleCount++] = g_hSecondaryWorkerThread;
    if (handleCount > 0) {
        WaitForMultipleObjects(handleCount, handles, TRUE, TreblleConst::kShutdownDrainMs);
    }

    auto FinishThread = [](HANDLE& hThread, AsyncQueue*& pQueue, const char* label) {
        if (!hThread) {
            delete pQueue;
            pQueue = nullptr;
            return;
        }
        DWORD waitResult = WaitForSingleObject(hThread, 0); // already covered by the combined wait above
        CloseHandle(hThread);
        hThread = nullptr;
        if (waitResult == WAIT_OBJECT_0) {
            // Thread exited cleanly — safe to delete queue
            delete pQueue;
            pQueue = nullptr;
        } else {
            // Thread did not exit in time — leave queue alive to prevent crash
            LogDebug(std::string("Treblle: ") + label +
                     " worker thread did not exit within drain timeout — leaking queue to prevent crash", true);
        }
    };

    FinishThread(g_hWorkerThread,          g_pQueue,          "primary");
    FinishThread(g_hSecondaryWorkerThread, g_pSecondaryQueue, "secondary");

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

        // Secondary destination is opt-in and config-driven (hot-reloadable), so this
        // thread/queue is always created — cheap when idle — and the worker itself
        // no-ops whenever secondary_treblle_url is unset. Failure here is NOT fatal:
        // the module must keep tracking to the primary destination regardless.
        g_pSecondaryQueue = new(std::nothrow) AsyncQueue();
        if (!g_pSecondaryQueue) {
            LogDebug("Treblle: failed to allocate secondary AsyncQueue — secondary destination disabled", true);
        } else {
            g_hSecondaryWorkerThread = CreateThread(nullptr, 0, SecondaryWorkerThreadProc, nullptr, 0, nullptr);
            if (!g_hSecondaryWorkerThread) {
                LogDebug("Treblle: CreateThread failed for secondary worker — secondary destination disabled", true);
                delete g_pSecondaryQueue;
                g_pSecondaryQueue = nullptr;
            }
        }

        auto* pFactory = new(std::nothrow) CTreblleAgentFactory();
        if (!pFactory) {
            LogDebug("Treblle: failed to allocate CTreblleAgentFactory — E_OUTOFMEMORY", true);
            if (g_pQueue) { g_pQueue->Shutdown(); delete g_pQueue; g_pQueue = nullptr; }
            if (g_pSecondaryQueue) { g_pSecondaryQueue->Shutdown(); delete g_pSecondaryQueue; g_pSecondaryQueue = nullptr; }
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
            if (g_pSecondaryQueue) { g_pSecondaryQueue->Shutdown(); delete g_pSecondaryQueue; g_pSecondaryQueue = nullptr; }
            if (g_hSecondaryWorkerThread) { CloseHandle(g_hSecondaryWorkerThread); g_hSecondaryWorkerThread = nullptr; }
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
    if (hdrs.pUnknownHeaders) {
        for (USHORT i = 0; i < hdrs.UnknownHeaderCount; ++i) {
            const HTTP_UNKNOWN_HEADER& uh = hdrs.pUnknownHeaders[i];
            if (uh.pName && uh.NameLength > 0 && uh.pRawValue && uh.RawValueLength > 0)
                out[ToLower(std::string(uh.pName, uh.NameLength))] =
                    std::string(uh.pRawValue, uh.RawValueLength);
        }
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

static std::string ResolveInternalName(IHttpContext* pCtx, const std::string& host) {
    // 1. IIS site name — human-readable name set in IIS Manager
    IHttpSite* pSite = pCtx->GetSite();
    if (pSite) {
        PCWSTR pwName = pSite->GetSiteName();
        if (pwName && pwName[0] != L'\0') {
            int len = WideCharToMultiByte(CP_UTF8, 0, pwName, -1, nullptr, 0, nullptr, nullptr);
            if (len > 1) {
                std::string name(len - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, pwName, -1, &name[0], len, nullptr, nullptr);
                return name;
            }
        }
    }

    // 2. App pool ID
    PCSTR  pPool = nullptr;
    DWORD  cbPool = 0;
    if (SUCCEEDED(pCtx->GetServerVariable("APP_POOL_ID", &pPool, &cbPool))
        && pPool && cbPool > 0) {
        return std::string(pPool, cbPool);
    }

    // 3. Host header
    if (!host.empty()) return host;

    return "";
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
        ctx_.internalName = ResolveInternalName(pCtx, host);
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

        if (ct.find("multipart/form-data") != std::string::npos) {
            PCSTR pCL = pRaw->Headers.KnownHeaders[HttpHeaderContentLength].pRawValue;
            LONGLONG cl = pCL ? _atoi64(pCL) : 0;
            ctx_.requestBody = SummarizeMultipartBody(pCtx, pCT, cl);
        } else {
            ContentTypeClass ctClass = ClassifyContentType(ct);
            if (ctClass == ContentTypeClass::Json) {
                ctx_.requestBody = ReadRequestBody(pCtx, ctx_.requestBodyTruncated);
            } else if (ctClass == ContentTypeClass::Ambiguous) {
                // No/unrecognized Content-Type — GET/HEAD/DELETE naturally read back
                // empty here, which is fine: an empty body just never confirms as
                // JSON and requestBody stays unset, same as today.
                std::string body = ReadRequestBody(pCtx, ctx_.requestBodyTruncated,
                                                    TreblleConst::kUntypedSniffCapBytes);
                if (IsLikelyJson(body) && IsValidJson(body)) {
                    ctx_.requestBody = std::move(body);
                }
            }
            // NotJson (e.g. x-www-form-urlencoded): leave requestBody empty, as before.
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
            ContentTypeClass ctClass = ClassifyContentType(ct);

            if (ctClass == ContentTypeClass::NotJson) {
                LogDebug("Treblle: skip — response Content-Type \"" + ct + "\" is not JSON: "
                    + ctx_.internalName + ctx_.routePath, ctx_.debugMode);
                ctx_.shouldTrack = false;
                return RQ_NOTIFICATION_CONTINUE;
            }

            ctx_.responseContentTypeAmbiguous = (ctClass == ContentTypeClass::Ambiguous);

            USHORT status = 0;
            pResp->GetStatus(&status);
            ctx_.statusCode = status;
            CollectResponseHeaders(pRaw);
            ctx_.responseHeadersDone = true;
        }

        size_t captureCap = ctx_.responseContentTypeAmbiguous
            ? TreblleConst::kUntypedSniffCapBytes
            : TreblleConst::kMaxBodyBytes;
        CaptureResponseChunks(pRaw, ctx_.responseBody, ctx_.responseSize,
                               ctx_.responseBodyTruncated, captureCap);

        // No Content-Type to go on — bail as soon as the body proves it isn't
        // JSON-shaped, instead of buffering the rest of an unrelated payload.
        if (ctx_.responseContentTypeAmbiguous && IsDefinitelyNotJsonStart(ctx_.responseBody)) {
            LogDebug("Treblle: skip — untyped response body is not JSON-shaped: "
                + ctx_.internalName + ctx_.routePath, ctx_.debugMode);
            ctx_.shouldTrack = false;
            return RQ_NOTIFICATION_CONTINUE;
        }
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
        // Content-Type never told us either way — the only signal left is
        // whether the fully-captured body actually parses as JSON. A body cut
        // off by the untyped sniff cap correctly fails this (its real closing
        // bracket is beyond what we captured) and is skipped rather than guessed.
        if (ctx_.responseContentTypeAmbiguous && ctx_.responseSize > 0) {
            if (!IsLikelyJson(ctx_.responseBody) || !IsValidJson(ctx_.responseBody)) {
                LogDebug("Treblle: skip — untyped response body did not parse as JSON: "
                    + ctx_.internalName + ctx_.routePath, ctx_.debugMode);
                return RQ_NOTIFICATION_CONTINUE;
            }
        }
        // responseSize == 0 under a matched route (e.g. 204/304/HEAD) is trusted
        // and tracked with an empty body — include_routes is the operator's
        // explicit opt-in, and there's no body signal either way to contradict it.

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
            // Secondary destination shares credentials with primary, so the same
            // payload is queued to both — copy first, primary gets the original moved.
            if (g_pSecondaryQueue && cfg->HasSecondaryDestination())
                g_pSecondaryQueue->Push(payload);
            g_pQueue->Push(std::move(payload));
        } else {
            LogDebug("Treblle: queue is null — payload dropped", true);
        }
    } catch (...) {
        LogDebug("Treblle: unhandled exception in OnEndRequest", true);
    }

    return RQ_NOTIFICATION_CONTINUE;
}
