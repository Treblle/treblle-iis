#include "precomp.h"
#include "BodyCapture.h"
#include "Constants.h"
#include "Utils.h"

std::string ReadRequestBody(IHttpContext* pCtx, bool& truncated) {
    truncated = false;
    IHttpRequest* pReq = pCtx->GetRequest();

    std::string body;
    body.reserve(4096);

    BYTE  buf[TreblleConst::kBodyReadBuffer];
    DWORD cbRead = 0;

    while (true) {
        cbRead = 0;
        HRESULT hr = pReq->ReadEntityBody(buf, TreblleConst::kBodyReadBuffer, FALSE, &cbRead, nullptr);
        if (FAILED(hr) || cbRead == 0) break;

        if (body.size() + cbRead > TreblleConst::kMaxBodyBytes) {
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

        if (!truncated && body.size() < TreblleConst::kMaxBodyBytes) {
            size_t canAppend = TreblleConst::kMaxBodyBytes - body.size();
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

// ── Multipart helpers ─────────────────────────────────────────────────────────

namespace {

static std::string ExtractBoundary(const std::string& ct) {
    std::string lower = ToLower(ct);
    size_t pos = lower.find("boundary=");
    if (pos == std::string::npos) return {};
    pos += 9;
    if (pos < ct.size() && ct[pos] == '"') {
        size_t end = ct.find('"', pos + 1);
        return end != std::string::npos ? ct.substr(pos + 1, end - pos - 1) : ct.substr(pos + 1);
    }
    size_t end = pos;
    while (end < ct.size() && ct[end] != ';' && ct[end] != ' ' &&
           ct[end] != '\t' && ct[end] != '\r' && ct[end] != '\n')
        ++end;
    return ct.substr(pos, end - pos);
}

static std::string ExtractParam(const std::string& hval, const std::string& param) {
    std::string lower = ToLower(hval);
    std::string needle = param + "=";
    size_t pos = lower.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    if (pos < hval.size() && hval[pos] == '"') {
        size_t end = hval.find('"', pos + 1);
        return end != std::string::npos ? hval.substr(pos + 1, end - pos - 1) : hval.substr(pos + 1);
    }
    size_t end = pos;
    while (end < hval.size() && hval[end] != ';' && hval[end] != ' ' &&
           hval[end] != '\t' && hval[end] != '\r' && hval[end] != '\n')
        ++end;
    return hval.substr(pos, end - pos);
}

static std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

} // namespace

std::string ParseMultipartFiles(const std::string& raw,
                                 const std::string& boundary,
                                 LONGLONG           contentLength) {
    if (boundary.empty()) return "[]";

    std::string delim = "--" + boundary;

    struct Part { std::string name, type; LONGLONG size; };
    std::vector<Part> files;

    size_t pos = 0;
    while (pos < raw.size()) {
        size_t bpos = raw.find(delim, pos);
        if (bpos == std::string::npos) break;
        pos = bpos + delim.size();

        if (pos + 1 < raw.size() && raw[pos] == '-' && raw[pos + 1] == '-') break;

        if (pos + 1 < raw.size() && raw[pos] == '\r' && raw[pos + 1] == '\n') pos += 2;
        else if (pos < raw.size() && raw[pos] == '\n')                         pos += 1;

        size_t hdrEnd = raw.find("\r\n\r\n", pos);
        if (hdrEnd == std::string::npos) break;
        size_t dataStart = hdrEnd + 4;

        std::string filename, mimeType;
        size_t hpos = pos;
        while (hpos < hdrEnd) {
            size_t lineEnd = raw.find("\r\n", hpos);
            if (lineEnd == std::string::npos || lineEnd > hdrEnd) lineEnd = hdrEnd;
            std::string line = raw.substr(hpos, lineEnd - hpos);

            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string hname = ToLower(line.substr(0, colon));
                std::string hval  = Trim(line.substr(colon + 1));

                if (hname == "content-disposition") {
                    filename = ExtractParam(hval, "filename");
                } else if (hname == "content-type") {
                    size_t semi = hval.find(';');
                    mimeType = Trim(semi != std::string::npos ? hval.substr(0, semi) : hval);
                }
            }

            if (lineEnd >= hdrEnd) break;
            hpos = lineEnd + 2;
        }

        if (!filename.empty()) {
            LONGLONG partSize;
            size_t dataEnd = raw.find("\r\n" + delim, dataStart);
            if (dataEnd != std::string::npos) {
                partSize = static_cast<LONGLONG>(dataEnd - dataStart);
            } else {
                partSize = contentLength;
            }
            files.push_back({filename,
                             mimeType.empty() ? "application/octet-stream" : mimeType,
                             partSize});
        }

        pos = dataStart;
    }

    if (files.empty()) return "[]";

    std::string json = "[";
    for (size_t i = 0; i < files.size(); ++i) {
        if (i > 0) json += ',';
        json += "{\"name\":\"";
        json += JsonEscape(files[i].name);
        json += "\",\"size\":";
        json += std::to_string(files[i].size);
        json += ",\"type\":\"";
        json += JsonEscape(files[i].type);
        json += "\"}";
    }
    json += "]";
    return json;
}

std::string SummarizeMultipartBody(IHttpContext*       pCtx,
                                    const std::string& contentType,
                                    LONGLONG           contentLength) {
    std::string boundary = ExtractBoundary(contentType);
    if (boundary.empty()) return "[]";

    IHttpRequest* pReq = pCtx->GetRequest();

    BYTE  buf[TreblleConst::kMultipartWindow];
    DWORD cbRead = 0;
    HRESULT hr = pReq->ReadEntityBody(buf, TreblleConst::kMultipartWindow, FALSE, &cbRead, nullptr);
    if (FAILED(hr) || cbRead == 0) return "[]";

    LPVOID pMem = pCtx->AllocateRequestMemory(cbRead);
    if (pMem) {
        memcpy(pMem, buf, cbRead);
        pReq->InsertEntityBody(pMem, cbRead);
    }

    return ParseMultipartFiles(std::string(reinterpret_cast<char*>(buf), cbRead),
                               boundary, contentLength);
}

// ── IsLikelyJson ──────────────────────────────────────────────────────────────

bool IsLikelyJson(const std::string& body) {
    if (body.empty()) return false;
    size_t start = 0;
    while (start < body.size() && (body[start] == ' ' || body[start] == '\t' ||
                                   body[start] == '\r' || body[start] == '\n'))
        ++start;
    if (start >= body.size()) return false;
    char first = body[start];
    if (first != '{' && first != '[') return false;
    size_t end = body.size() - 1;
    while (end > start && (body[end] == ' ' || body[end] == '\t' ||
                           body[end] == '\r' || body[end] == '\n'))
        --end;
    char last = body[end];
    if (first == '{') return last == '}';
    if (first == '[') return last == ']';
    return false;
}
