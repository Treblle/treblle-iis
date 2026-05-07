#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <winhttp.h>

// IIS Native Module SDK - included with Windows SDK 10+
#include <http.h>
#include <httpserv.h>

// HttpVerbPATCH was added in Windows 8 SDK - define fallback if missing
#ifndef HttpVerbPATCH
#define HttpVerbPATCH ((HTTP_VERB)28)
#endif

#include <string>
#include <vector>
#include <map>
#include <queue>
#include <mutex>
#include <atomic>
#include <sstream>
#include <algorithm>
#include <memory>
#include <cstdio>
#include <cstring>
#include <ctime>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
