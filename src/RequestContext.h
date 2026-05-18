#pragma once
#include "precomp.h"

struct RequestContext {
    bool shouldTrack           = false;
    bool responseHeadersDone   = false;
    bool requestBodyTruncated  = false;
    bool responseBodyTruncated = false;
    bool debugMode             = false;

    LARGE_INTEGER startTime   = {};
    LARGE_INTEGER frequency   = {};

    std::string method;
    std::string url;
    std::string routePath;
    std::string clientIp;
    std::string userAgent;
    std::string protocol;
    std::string internalId;
    std::string internalName;

    std::string requestBody;
    std::string responseBody;

    std::map<std::string, std::string> requestHeaders;
    std::map<std::string, std::string> responseHeaders;
    std::map<std::string, std::string> queryParams;

    int       statusCode   = 0;
    LONGLONG  responseSize = 0;
};
