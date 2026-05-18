#pragma once
// Minimal IHttpContext stub for unit tests.
// Only GetServerVariable(PCSTR) is implemented — everything else returns a
// safe no-op value. If the compiler reports "cannot instantiate abstract class",
// add the missing pure-virtual override and return E_NOTIMPL / nullptr.
#include "precomp.h"
#include <string>

class MockHttpContext final : public IHttpContext {
public:
    std::string serverSoftware = "Microsoft-IIS/10.0";

    HRESULT GetServerVariable(PCSTR name, PCSTR* val, DWORD* len) override {
        if (name && strcmp(name, "SERVER_SOFTWARE") == 0) {
            _sv = serverSoftware;
            *val = _sv.c_str();
            *len = static_cast<DWORD>(_sv.size());
            return S_OK;
        }
        if (val) *val = nullptr;
        if (len) *len = 0;
        return E_FAIL;
    }

    // Stubs — all remaining pure-virtual overrides
    IHttpSite*                   GetSite() override                                   { return nullptr; }
    IHttpApplication*            GetApplication() override                            { return nullptr; }
    IHttpConnection*             GetConnection() override                             { return nullptr; }
    IHttpRequest*                GetRequest() override                                { return nullptr; }
    IHttpResponse*               GetResponse() override                               { return nullptr; }
    BOOL GetIsLastNotification(REQUEST_NOTIFICATION_STATUS) override                  { return FALSE; }
    HRESULT GetCurrentExecutionStats(DWORD*, DWORD*, PCWSTR*, DWORD*, DWORD*, DWORD*) const override { return E_NOTIMPL; }
    IHttpTraceContext*           GetTraceContext() const override                      { return nullptr; }
    HRESULT GetServerVariable(PCSTR, PCWSTR*, DWORD*) override                        { return E_NOTIMPL; }
    HRESULT SetServerVariable(PCSTR, PCWSTR) override                                 { return E_NOTIMPL; }
    PVOID                        AllocateRequestMemory(DWORD) override                { return nullptr; }
    IHttpUrlInfo*                GetUrlInfo() override                                 { return nullptr; }
    IMetaDataInfo*               GetMetaData() override                               { return nullptr; }
    const HTTP_REQUEST*          GetCurrentHttpRequestRaw() const override            { return nullptr; }
    HRESULT GetScriptName(PCWSTR*, DWORD*) const override                             { return E_NOTIMPL; }
    IScriptMapInfo*              GetScriptMap() const override                         { return nullptr; }
    VOID                         SetRequestHandled() override                         {}
    IHttpFileInfo*               GetFileInfo() const override                          { return nullptr; }
    HRESULT MapPath(PCWSTR, PWSTR, DWORD*) override                                   { return E_NOTIMPL; }
    HRESULT NotifyCustomNotification(ICustomNotificationProvider*, BOOL*) override    { return E_NOTIMPL; }
    IHttpContext*                GetParentContext() const override                     { return nullptr; }
    IHttpContext*                GetRootContext() const override                       { return nullptr; }
    HRESULT CloneContext(DWORD, IHttpContext**) override                               { return E_NOTIMPL; }
    HRESULT ReleaseClonedContext(IHttpContext*) override                               { return E_NOTIMPL; }
    IHttpUser*                   GetUser() const override                              { return nullptr; }
    IHttpModuleContextContainer* GetModuleContextContainer() override                 { return nullptr; }
    VOID IndicateCompletion(REQUEST_NOTIFICATION_STATUS) override                     {}
    VOID PostCompletion(DWORD) override                                               {}
    VOID DisableNotifications(DWORD, DWORD) override                                  {}
    BOOL GetNextNotification(REQUEST_NOTIFICATION_STATUS, DWORD*, BOOL*,
                             CHttpModule**, IHttpEventProvider**) override            { return FALSE; }
    BOOL GetIsChild() const override                                                  { return FALSE; }
    HRESULT MapHandler(DWORD, PCWSTR, PCWSTR, PCSTR,
                       IScriptMapInfo**, BOOL) override                               { return E_NOTIMPL; }
    HRESULT GetExtendedInterface(HTTP_MODULE_ID, DWORD, PVOID*) override              { return E_NOTIMPL; }

private:
    std::string _sv;
};
