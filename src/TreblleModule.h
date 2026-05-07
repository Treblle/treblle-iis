#pragma once
#include "precomp.h"
#include "RequestContext.h"
#include "AsyncQueue.h"

// Per-request module instance. IIS creates one per request via the factory.
class CTreblleModule : public CHttpModule {
public:
    REQUEST_NOTIFICATION_STATUS OnBeginRequest(
        IHttpContext*   pCtx,
        IHttpEventProvider* pProvider) override;

    REQUEST_NOTIFICATION_STATUS OnSendResponse(
        IHttpContext*          pCtx,
        ISendResponseProvider* pProvider) override;

    REQUEST_NOTIFICATION_STATUS OnEndRequest(
        IHttpContext*   pCtx,
        IHttpEventProvider* pProvider) override;

    void Dispose() override { delete this; }

private:
    RequestContext ctx_;

    void CollectRequestHeaders(HTTP_REQUEST* pRaw);
    void CollectResponseHeaders(HTTP_RESPONSE* pRaw);
    std::string BuildFullUrl(IHttpContext* pCtx, HTTP_REQUEST* pRaw);
    std::string GetMethodString(HTTP_VERB verb, PCSTR pUnknown, USHORT unknownLen);
    bool        IsTrackedMethod(const std::string& method);
};

// Module factory — one instance per server, creates CTreblleModule per request.
class CTreblleModuleFactory : public IHttpModuleFactory {
public:
    HRESULT GetHttpModule(OUT CHttpModule** ppModule,
                          IN  IModuleAllocator*) override;
    void Terminate() override;
};

// Global state shared between RegisterModule and the background worker.
extern AsyncQueue* g_pQueue;
extern HANDLE      g_hWorkerThread;
extern HMODULE     g_hModule;
