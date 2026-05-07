#pragma once
#include "precomp.h"

struct OsInfo {
    std::string name;
    std::string release;
    std::string architecture;
};

std::string  ToLower(const std::string& s);
bool         StartsWithCI(const std::string& str, const std::string& prefix);
std::string  JsonEscape(const std::string& s);
std::string  FormatUtcTimestamp();
std::string  ParseQueryPath(const std::string& rawUrl);
std::map<std::string, std::string> ParseQueryString(const std::string& rawUrl);

std::string  GetClientIP(IHttpContext* pCtx);
std::string  GetServerIP();
OsInfo       GetOsInfo();
std::string  GetIISVersion(IHttpContext* pCtx);

void         LogDebug(const std::string& msg);
