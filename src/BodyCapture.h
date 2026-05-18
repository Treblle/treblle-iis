#pragma once
#include "precomp.h"
#include "Constants.h"

// Reads the full request entity body from IIS, then re-inserts it so the
// downstream application handler can still read it unchanged.
// Returns the raw body string. Sets truncated=true if the body exceeded 2 MB.
std::string ReadRequestBody(IHttpContext* pCtx, bool& truncated);

// Accumulates response body chunks from an HTTP_RESPONSE into 'body'.
// Stops accumulating once 'body' reaches TreblleConst::kMaxBodyBytes.
// Sets truncated=true if the total response size exceeded 2 MB.
void CaptureResponseChunks(HTTP_RESPONSE* pRaw,
                           std::string&   body,
                           LONGLONG&      totalSize,
                           bool&          truncated);

// Returns true if the body is plausibly valid JSON (non-empty, starts/ends correctly).
bool IsLikelyJson(const std::string& body);

// Reads the leading bytes of a multipart/form-data body to extract file part metadata,
// then re-inserts those bytes so the downstream handler still receives the full body.
// contentType: the full Content-Type header value (must include the boundary parameter).
// contentLength: value of the Content-Length request header (0 if absent).
// Returns a JSON array: [{"name":"...","size":N,"type":"..."}]
// size is the exact part byte count when the part ends within the read window,
// otherwise the total request Content-Length.
std::string SummarizeMultipartBody(IHttpContext*       pCtx,
                                    const std::string& contentType,
                                    LONGLONG           contentLength);

// Pure parsing helper exposed for unit tests.
// raw: the raw multipart bytes (may be truncated at the buffer boundary).
// boundary: the boundary token from the Content-Type header.
// contentLength: fallback size when a part extends past the end of raw.
std::string ParseMultipartFiles(const std::string& raw,
                                 const std::string& boundary,
                                 LONGLONG           contentLength);
