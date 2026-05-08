#include "precomp.h"
#include "PayloadBuilder.h"
#include "BodyCapture.h"
#include "Utils.h"

static const char* kSdkName    = "iis-native";
static const char* kSdkVersion = "1.0.0";

// ── Helpers ───────────────────────────────────────────────────────────────────

std::string PayloadBuilder::BuildHeadersObject(const std::map<std::string, std::string>& headers) {
    if (headers.empty()) return "{}";
    std::string s = "{";
    bool first = true;
    for (const auto& kv : headers) {
        if (!first) s += ',';
        s += '"';
        s += JsonEscape(kv.first);
        s += "\":\"";
        s += JsonEscape(kv.second);
        s += '"';
        first = false;
    }
    s += '}';
    return s;
}

std::string PayloadBuilder::BuildQueryObject(const std::map<std::string, std::string>& params) {
    return BuildHeadersObject(params); // same structure
}

std::string PayloadBuilder::BuildBodyField(const std::string& body, bool truncated) {
    if (truncated)
        return "{\"treblle_error\":\"Payload exceeds 2MB and will not be tracked by Treblle\"}";
    if (body.empty())
        return "{}";
    if (IsLikelyJson(body))
        return body; // embed raw JSON directly — no escaping needed
    return "{}";
}

// ── Main builder ──────────────────────────────────────────────────────────────

std::string PayloadBuilder::Build(const RequestContext& ctx,
                                  const TreblleConfig&  cfg,
                                  double                loadTimeMs,
                                  IHttpContext*          pCtx) {
    OsInfo      os  = GetOsInfo();
    std::string serverIp = GetServerIP();
    std::string iisVer   = GetIISVersion(pCtx);

    // Format load_time with 3 decimal places
    char loadTimeBuf[32];
    snprintf(loadTimeBuf, sizeof(loadTimeBuf), "%.3f", loadTimeMs);

    // Format response size
    char responseSizeBuf[32];
    snprintf(responseSizeBuf, sizeof(responseSizeBuf), "%lld", ctx.responseSize);

    std::string payload;
    payload.reserve(4096);

    payload += "{";

    // Top-level fields — api_key falls back to sdk_token for autodiscovery
    const std::string& apiKey = cfg.apiKey.empty() ? cfg.sdkToken : cfg.apiKey;
    payload += "\"api_key\":\"";     payload += JsonEscape(apiKey);          payload += "\",";
    payload += "\"sdk_token\":\"";   payload += JsonEscape(cfg.sdkToken);    payload += "\",";
    payload += "\"version\":1.0,";
    payload += "\"internal_id\":\""; payload += JsonEscape(ctx.internalId);  payload += "\",";

    // data object
    payload += "\"data\":{";

    // server
    payload += "\"server\":{";
    payload += "\"ip\":\"";        payload += JsonEscape(serverIp);      payload += "\",";
    payload += "\"timezone\":\"UTC\",";
    payload += "\"software\":\"";  payload += JsonEscape(iisVer);        payload += "\",";
    payload += "\"protocol\":\"";  payload += JsonEscape(ctx.protocol);  payload += "\",";
    payload += "\"os\":{";
    payload += "\"name\":\"";      payload += JsonEscape(os.name);        payload += "\",";
    payload += "\"release\":\"";   payload += JsonEscape(os.release);     payload += "\",";
    payload += "\"architecture\":\""; payload += JsonEscape(os.architecture); payload += "\"";
    payload += "}";  // os
    payload += "},"; // server

    // language
    payload += "\"language\":{";
    payload += "\"name\":\"";    payload += kSdkName;    payload += "\",";
    payload += "\"version\":\""; payload += kSdkVersion; payload += "\"";
    payload += "},"; // language

    // request
    payload += "\"request\":{";
    payload += "\"timestamp\":\""; payload += JsonEscape(FormatUtcTimestamp()); payload += "\",";
    payload += "\"ip\":\"";        payload += JsonEscape(ctx.clientIp);         payload += "\",";
    payload += "\"url\":\"";       payload += JsonEscape(ctx.url);              payload += "\",";
    payload += "\"user_agent\":\"";payload += JsonEscape(ctx.userAgent);        payload += "\",";
    payload += "\"method\":\"";    payload += JsonEscape(ctx.method);           payload += "\",";
    payload += "\"headers\":";     payload += BuildHeadersObject(ctx.requestHeaders);  payload += ",";
    payload += "\"body\":";        payload += BuildBodyField(ctx.requestBody, ctx.requestBodyTruncated); payload += ",";
    payload += "\"route_path\":\"";payload += JsonEscape(ctx.routePath);        payload += "\",";
    payload += "\"query\":";       payload += BuildQueryObject(ctx.queryParams);
    payload += "},"; // request

    // response
    payload += "\"response\":{";
    payload += "\"headers\":";  payload += BuildHeadersObject(ctx.responseHeaders); payload += ",";
    payload += "\"code\":";     payload += std::to_string(ctx.statusCode);           payload += ",";
    payload += "\"size\":";     payload += responseSizeBuf;                          payload += ",";
    payload += "\"load_time\":";payload += loadTimeBuf;                              payload += ",";
    payload += "\"body\":";     payload += BuildBodyField(ctx.responseBody, ctx.responseBodyTruncated);
    payload += "},"; // response

    // errors (always empty — native module has no access to app-level exceptions)
    payload += "\"errors\":[]";

    payload += "}"; // data
    payload += "}"; // root

    return payload;
}
