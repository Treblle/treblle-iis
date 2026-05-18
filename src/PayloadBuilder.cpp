#include "precomp.h"
#include "PayloadBuilder.h"
#include "BodyCapture.h"
#include "Constants.h"
#include "Utils.h"
#include "DataMasker.h"

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
                                  const std::string&    iisVersion) {
    OsInfo      os  = GetOsInfo();
    std::string serverIp = GetServerIP();
    const std::string& iisVer = iisVersion;

    const std::vector<std::string>& kws = cfg.maskedKeywords;
    auto reqHeaders  = ctx.requestHeaders;
    auto respHeaders = ctx.responseHeaders;
    MaskHeaders(reqHeaders,  kws);
    MaskHeaders(respHeaders, kws);
    const std::string reqBody  = MaskJson(ctx.requestBody,  kws);
    const std::string respBody = MaskJson(ctx.responseBody, kws);

    char loadTimeBuf[32];
    snprintf(loadTimeBuf, sizeof(loadTimeBuf), "%.3f", loadTimeMs);

    char responseSizeBuf[32];
    snprintf(responseSizeBuf, sizeof(responseSizeBuf), "%lld", ctx.responseSize);

    std::string payload;
    payload.reserve(4096);

    payload += "{";

    payload += "\"api_key\":\"";       payload += JsonEscape(cfg.apiKey);       payload += "\",";
    payload += "\"sdk_token\":\"";     payload += JsonEscape(cfg.sdkToken);     payload += "\",";
    payload += "\"sdk\":\"";           payload += TreblleConst::kSdkName;       payload += "\",";
    payload += "\"version\":";         payload += std::to_string(TreblleConst::kApiVersion); payload += ",";
    payload += "\"internal_id\":\"";   payload += JsonEscape(ctx.internalId);   payload += "\",";
    payload += "\"internal_name\":\""; payload += JsonEscape(ctx.internalName); payload += "\",";

    // data object
    payload += "\"data\":{";

    // server
    payload += "\"server\":{";
    payload += "\"ip\":\"";        payload += JsonEscape(serverIp);      payload += "\",";
    payload += "\"timezone\":\"UTC\",";
    payload += "\"software\":\"";  payload += JsonEscape(iisVer);        payload += "\",";
    payload += "\"protocol\":\"";  payload += JsonEscape(ctx.protocol);  payload += "\",";
    payload += "\"os\":{";
    payload += "\"name\":\"";      payload += JsonEscape(os.name);           payload += "\",";
    payload += "\"release\":\"";   payload += JsonEscape(os.release);        payload += "\",";
    payload += "\"architecture\":\""; payload += JsonEscape(os.architecture); payload += "\"";
    payload += "}";  // os
    payload += "},"; // server

    // language
    payload += "\"language\":{";
    payload += "\"name\":\"";    payload += TreblleConst::kSdkName;    payload += "\",";
    payload += "\"version\":\""; payload += TreblleConst::kSdkVersion; payload += "\"";
    payload += "},"; // language

    // request
    payload += "\"request\":{";
    payload += "\"timestamp\":\""; payload += JsonEscape(FormatUtcTimestamp()); payload += "\",";
    payload += "\"ip\":\"";        payload += JsonEscape(ctx.clientIp);         payload += "\",";
    payload += "\"url\":\"";       payload += JsonEscape(ctx.url);              payload += "\",";
    payload += "\"user_agent\":\"";payload += JsonEscape(ctx.userAgent);        payload += "\",";
    payload += "\"method\":\"";    payload += JsonEscape(ctx.method);           payload += "\",";
    payload += "\"headers\":";     payload += BuildHeadersObject(reqHeaders);   payload += ",";
    payload += "\"body\":";        payload += BuildBodyField(reqBody, ctx.requestBodyTruncated); payload += ",";
    payload += "\"route_path\":\"";payload += JsonEscape(ctx.routePath);        payload += "\",";
    payload += "\"query\":";       payload += BuildQueryObject(ctx.queryParams);
    payload += "},"; // request

    // response
    payload += "\"response\":{";
    payload += "\"headers\":";  payload += BuildHeadersObject(respHeaders); payload += ",";
    payload += "\"code\":";     payload += std::to_string(ctx.statusCode);  payload += ",";
    payload += "\"size\":";     payload += responseSizeBuf;                 payload += ",";
    payload += "\"load_time\":";payload += loadTimeBuf;                     payload += ",";
    payload += "\"body\":";     payload += BuildBodyField(respBody, ctx.responseBodyTruncated);
    payload += "},"; // response

    // errors (always empty — native module has no access to app-level exceptions)
    payload += "\"errors\":[]";

    payload += "}"; // data
    payload += "}"; // root

    return payload;
}
