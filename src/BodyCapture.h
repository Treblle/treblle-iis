#pragma once
#include "precomp.h"

static const size_t kMaxBodyBytes = 2 * 1024 * 1024; // 2 MB

// Reads the full request entity body from IIS, then re-inserts it so the
// downstream application handler can still read it unchanged.
// Returns the raw body string. Sets truncated=true if the body exceeded 2 MB.
std::string ReadRequestBody(IHttpContext* pCtx, bool& truncated);

// Accumulates response body chunks from an HTTP_RESPONSE into 'body'.
// Stops accumulating once 'body' reaches kMaxBodyBytes.
// Sets truncated=true if the total response size exceeded 2 MB.
void CaptureResponseChunks(HTTP_RESPONSE* pRaw,
                           std::string&   body,
                           LONGLONG&      totalSize,
                           bool&          truncated);

// Returns true if the body is plausibly valid JSON (non-empty, starts/ends correctly).
bool IsLikelyJson(const std::string& body);
