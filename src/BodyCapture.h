#pragma once
#include "precomp.h"
#include "Constants.h"

// Reads the full request entity body from IIS, then re-inserts it so the
// downstream application handler can still read it unchanged.
// Returns the raw body string. Sets truncated=true if the body exceeded maxBytes.
std::string ReadRequestBody(IHttpContext* pCtx, bool& truncated,
                             size_t maxBytes = TreblleConst::kMaxBodyBytes);

// Accumulates response body chunks from an HTTP_RESPONSE into 'body'.
// Stops accumulating once 'body' reaches maxBytes.
// Sets truncated=true if the total response size exceeded maxBytes.
void CaptureResponseChunks(HTTP_RESPONSE* pRaw,
                           std::string&   body,
                           LONGLONG&      totalSize,
                           bool&          truncated,
                           size_t         maxBytes = TreblleConst::kMaxBodyBytes);

// Returns true if the body is plausibly valid JSON (non-empty, starts/ends correctly).
// This is a cheap structural check — see IsValidJson for an actual parse.
bool IsLikelyJson(const std::string& body);

// Classifies a Content-Type header value for JSON tracking purposes.
enum class ContentTypeClass {
    Json,       // application/json, text/json, or any '+json' suffix (vnd.api+json, etc.)
    NotJson,    // a recognized non-JSON family (html, images, fonts, media, xml, ...)
    Ambiguous,  // missing, empty, or not recognized either way — caller should sniff the body
};
ContentTypeClass ClassifyContentType(const std::string& contentTypeHeader);

// True only if there IS a non-whitespace byte in 'bodySoFar' and it is neither
// '{' nor '[' — a definite signal the content is not JSON. False if every byte
// seen so far is whitespace (undecided — caller should keep buffering) or if
// the first non-whitespace byte is a JSON opener (still a candidate).
// Intended for bailing out of an in-progress, unconfirmed capture early.
bool IsDefinitelyNotJsonStart(const std::string& bodySoFar);

// Strict validation for the sniff-confirm path: true only if 'body' parses as
// well-formed JSON. Distinct from IsLikelyJson's cheap structural check.
bool IsValidJson(const std::string& body);

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
