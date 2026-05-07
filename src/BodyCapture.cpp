#include "precomp.h"
#include "BodyCapture.h"

std::string ReadRequestBody(IHttpContext* pCtx, bool& truncated) {
    truncated = false;
    IHttpRequest* pReq = pCtx->GetRequest();

    std::string body;
    body.reserve(4096);

    const DWORD kChunk = 8192;
    BYTE  buf[8192];
    DWORD cbRead = 0;

    while (true) {
        cbRead = 0;
        HRESULT hr = pReq->ReadEntityBody(buf, kChunk, FALSE, &cbRead, nullptr);
        if (FAILED(hr) || cbRead == 0) break;

        if (body.size() + cbRead > kMaxBodyBytes) {
            truncated = true;
            break;
        }
        body.append(reinterpret_cast<char*>(buf), cbRead);
        if (pReq->GetRemainingEntityBytes() == 0) break;
    }

    // Always re-insert what we read so the downstream handler receives the full body.
    // InsertEntityBody places our data BEFORE any remaining unread bytes in IIS's buffer,
    // reconstructing the original stream even if we stopped early at the 2MB limit.
    if (!body.empty()) {
        LPVOID pBuf = pCtx->AllocateRequestMemory(static_cast<DWORD>(body.size()));
        if (pBuf) {
            memcpy(pBuf, body.c_str(), body.size());
            pReq->InsertEntityBody(pBuf, static_cast<DWORD>(body.size()));
        }
    }

    return body;
}

void CaptureResponseChunks(HTTP_RESPONSE* pRaw,
                           std::string&   body,
                           LONGLONG&      totalSize,
                           bool&          truncated) {
    if (!pRaw) return;

    for (USHORT i = 0; i < pRaw->EntityChunkCount; ++i) {
        const HTTP_DATA_CHUNK& chunk = pRaw->pEntityChunks[i];
        if (chunk.DataChunkType != HttpDataChunkFromMemory) continue;

        const char* data = static_cast<const char*>(chunk.FromMemory.pBuffer);
        ULONG        len  = chunk.FromMemory.BufferLength;
        if (!data || len == 0) continue;

        totalSize += len;

        if (!truncated && body.size() < kMaxBodyBytes) {
            size_t canAppend = kMaxBodyBytes - body.size();
            if (len > canAppend) {
                body.append(data, canAppend);
                truncated = true;
            } else {
                body.append(data, len);
            }
        } else {
            truncated = true;
        }
    }
}

bool IsLikelyJson(const std::string& body) {
    if (body.empty()) return false;
    // Trim leading whitespace
    size_t start = 0;
    while (start < body.size() && (body[start] == ' ' || body[start] == '\t' ||
                                   body[start] == '\r' || body[start] == '\n'))
        ++start;
    if (start >= body.size()) return false;
    char first = body[start];
    if (first != '{' && first != '[') return false;
    // Trim trailing whitespace
    size_t end = body.size() - 1;
    while (end > start && (body[end] == ' ' || body[end] == '\t' ||
                           body[end] == '\r' || body[end] == '\n'))
        --end;
    char last = body[end];
    if (first == '{') return last == '}';
    if (first == '[') return last == ']';
    return false;
}
