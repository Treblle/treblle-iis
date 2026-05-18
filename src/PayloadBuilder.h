#pragma once
#include "precomp.h"
#include "RequestContext.h"
#include "Config.h"

class PayloadBuilder {
public:
    // Builds the Treblle JSON payload string from the captured request context.
    // loadTimeMs is the end-to-end request time in milliseconds.
    static std::string Build(const RequestContext& ctx,
                             const TreblleConfig&  cfg,
                             double                loadTimeMs,
                             const std::string&    iisVersion);
private:
    static std::string BuildHeadersObject(const std::map<std::string, std::string>& headers);
    static std::string BuildBodyField(const std::string& body, bool truncated);
    static std::string BuildQueryObject(const std::map<std::string, std::string>& params);
};
