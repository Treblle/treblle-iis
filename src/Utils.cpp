#include "precomp.h"
#include "Utils.h"

// ── String helpers ────────────────────────────────────────────────────────────

std::string ToLower(const std::string& s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(),
        [](unsigned char c){ return (char)std::tolower(c); });
    return r;
}

bool StartsWithCI(const std::string& str, const std::string& prefix) {
    if (prefix.empty()) return true;
    if (str.size() < prefix.size()) return false;
    return _strnicmp(str.c_str(), prefix.c_str(), prefix.size()) == 0;
}

// ── JSON escaping ─────────────────────────────────────────────────────────────

std::string JsonEscape(const std::string& s) {
    std::string r;
    r.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\b': r += "\\b";  break;
            case '\f': r += "\\f";  break;
            case '\n': r += "\\n";  break;
            case '\r': r += "\\r";  break;
            case '\t': r += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    r += buf;
                } else {
                    r += (char)c;
                }
        }
    }
    return r;
}

// ── Timestamp ─────────────────────────────────────────────────────────────────

std::string FormatUtcTimestamp() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// ── URL helpers ───────────────────────────────────────────────────────────────

std::string ParseQueryPath(const std::string& rawUrl) {
    size_t q = rawUrl.find('?');
    return q != std::string::npos ? rawUrl.substr(0, q) : rawUrl;
}

std::map<std::string, std::string> ParseQueryString(const std::string& rawUrl) {
    std::map<std::string, std::string> result;
    size_t q = rawUrl.find('?');
    if (q == std::string::npos) return result;

    std::string qs = rawUrl.substr(q + 1);
    size_t pos = 0;
    while (pos < qs.size()) {
        size_t amp = qs.find('&', pos);
        std::string pair = qs.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos)
            result[pair.substr(0, eq)] = pair.substr(eq + 1);
        else if (!pair.empty())
            result[pair] = "";
        pos = amp == std::string::npos ? qs.size() : amp + 1;
    }
    return result;
}

// ── IP helpers ────────────────────────────────────────────────────────────────

static bool IsValidIP(const std::string& s) {
    if (s.empty() || s == "127.0.0.1" || s == "::1") return false;
    struct sockaddr_in sa4;
    struct sockaddr_in6 sa6;
    if (inet_pton(AF_INET, s.c_str(), &sa4.sin_addr) == 1) return true;
    if (inet_pton(AF_INET6, s.c_str(), &sa6.sin6_addr) == 1) return true;
    return false;
}

std::string GetClientIP(IHttpContext* pCtx) {
    // Prefer forwarded headers for clients behind proxies
    static const char* kHeaders[] = {
        "HTTP_X_FORWARDED_FOR",
        "HTTP_X_REAL_IP",
        "HTTP_CF_CONNECTING_IP",
        "REMOTE_ADDR"
    };
    for (const char* hdr : kHeaders) {
        DWORD cbVal = 0;
        PCSTR pVal  = nullptr;
        if (SUCCEEDED(pCtx->GetServerVariable(hdr, &pVal, &cbVal)) && pVal && cbVal > 0) {
            std::string val(pVal, cbVal);
            // X-Forwarded-For may be comma-separated — take the first entry
            size_t comma = val.find(',');
            if (comma != std::string::npos) val = val.substr(0, comma);
            // Trim whitespace
            val.erase(0, val.find_first_not_of(" \t"));
            val.erase(val.find_last_not_of(" \t") + 1);
            if (IsValidIP(val)) return val;
        }
    }
    return "bogon";
}

std::string GetServerIP() {
    static std::string cached;
    static std::once_flag flag;
    std::call_once(flag, []() {
        char hostname[256] = {};
        if (gethostname(hostname, sizeof(hostname)) != 0) { cached = "bogon"; return; }
        struct addrinfo hints = {}, *res = nullptr;
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(hostname, nullptr, &hints, &res) == 0 && res) {
            char buf[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &((struct sockaddr_in*)res->ai_addr)->sin_addr, buf, sizeof(buf));
            cached = buf;
            freeaddrinfo(res);
        } else {
            cached = "bogon";
        }
    });
    return cached;
}

// ── OS info ───────────────────────────────────────────────────────────────────

OsInfo GetOsInfo() {
    static OsInfo cached;
    static std::once_flag flag;
    std::call_once(flag, []() {
        // Version via RtlGetVersion (avoids compatibility shim)
        typedef LONG (WINAPI* RtlGetVersionFn)(OSVERSIONINFOEXW*);
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
        OSVERSIONINFOEXW osvi = {};
        osvi.dwOSVersionInfoSize = sizeof(osvi);
        if (hNtdll) {
            auto fn = (RtlGetVersionFn)GetProcAddress(hNtdll, "RtlGetVersion");
            if (fn) fn(&osvi);
        }

        if (osvi.dwMajorVersion == 10 && osvi.dwBuildNumber >= 20000) {
            cached.name    = "Windows Server 2025";
        } else if (osvi.dwMajorVersion == 10 && osvi.dwBuildNumber >= 17763) {
            cached.name    = "Windows Server 2022";
        } else if (osvi.dwMajorVersion == 10) {
            cached.name    = "Windows Server 2019";
        } else {
            cached.name    = "Windows Server";
        }
        char release[32];
        snprintf(release, sizeof(release), "%lu.%lu.%lu",
            osvi.dwMajorVersion, osvi.dwMinorVersion, osvi.dwBuildNumber);
        cached.release = release;

        SYSTEM_INFO si = {};
        GetNativeSystemInfo(&si);
        switch (si.wProcessorArchitecture) {
            case PROCESSOR_ARCHITECTURE_AMD64: cached.architecture = "x86_64"; break;
            case PROCESSOR_ARCHITECTURE_ARM64: cached.architecture = "arm64";  break;
            case PROCESSOR_ARCHITECTURE_INTEL: cached.architecture = "x86";    break;
            default:                           cached.architecture = "unknown";
        }
    });
    return cached;
}

// ── IIS version ───────────────────────────────────────────────────────────────

std::string GetIISVersion(IHttpContext* pCtx) {
    DWORD cbVal = 0;
    PCSTR pVal  = nullptr;
    if (SUCCEEDED(pCtx->GetServerVariable("SERVER_SOFTWARE", &pVal, &cbVal)) && pVal && cbVal > 0)
        return std::string(pVal, cbVal);
    return "IIS";
}

// ── Host identity ─────────────────────────────────────────────────────────────

std::string ComputeHostId(const std::string& host) {
    auto fnv1a = [](const std::string& s, uint64_t seed) -> uint64_t {
        uint64_t h = seed;
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
        return h;
    };
    uint64_t hi = fnv1a("treblle:" + host, 14695981039346656037ULL);
    uint64_t lo = fnv1a("route:"   + host, 14695981039346656037ULL);
    char buf[37];
    snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%04x%08x",
        (uint32_t)(hi >> 32),
        (uint16_t)(hi >> 16),
        (uint16_t)(hi & 0xFFFF),
        (uint16_t)(lo >> 48),
        (uint16_t)(lo >> 32),
        (uint32_t)(lo & 0xFFFFFFFF));
    return buf;
}

// ── Debug logging ─────────────────────────────────────────────────────────────

void LogDebug(const std::string& msg) {
    std::string prefixed = "[TREBLLE]: " + msg;
    HANDLE hLog = RegisterEventSourceA(nullptr, "Treblle");
    if (!hLog) return;
    LPCSTR msgs[] = { prefixed.c_str() };
    ReportEventA(hLog, EVENTLOG_INFORMATION_TYPE, 0, 0, nullptr, 1, 0, msgs, nullptr);
    DeregisterEventSource(hLog);
}
