#include "precomp.h"
#include "DataMasker.h"

static const size_t kMaskSizeLimit = 500 * 1024; // 500 KB

namespace {

static size_t SkipWS(const std::string& s, size_t pos) {
    while (pos < s.size() &&
           (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n'))
        ++pos;
    return pos;
}

// Skip past a JSON string (pos must point at opening '"').
static size_t SkipStr(const std::string& s, size_t pos) {
    if (pos >= s.size() || s[pos] != '"') return pos;
    ++pos;
    while (pos < s.size()) {
        if (s[pos] == '\\') { pos += 2; continue; }
        if (s[pos] == '"')  { return pos + 1; }
        ++pos;
    }
    return pos;
}

// Read a JSON string (pos must point at opening '"').
// Returns decoded content and advances pos past the closing '"'.
static std::string ReadStr(const std::string& s, size_t& pos) {
    if (pos >= s.size() || s[pos] != '"') return {};
    ++pos;
    std::string out;
    while (pos < s.size() && s[pos] != '"') {
        if (s[pos] == '\\' && pos + 1 < s.size()) {
            ++pos;
            switch (s[pos]) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                default:   out += s[pos]; break;
            }
        } else {
            out += s[pos];
        }
        ++pos;
    }
    if (pos < s.size()) ++pos; // skip closing '"'
    return out;
}

static bool IsKeyword(const std::string& key, const std::vector<std::string>& kws) {
    for (const auto& kw : kws)
        if (_stricmp(key.c_str(), kw.c_str()) == 0) return true;
    return false;
}

// Write a masked replacement for the JSON value at pos. Advances pos past it.
// String values → same-length '*' string.
// Non-string values → same char-count '*' string.
// Object/array values → single "*".
static void WriteMasked(const std::string& src, size_t& pos, std::string& out) {
    if (pos >= src.size()) return;

    if (src[pos] == '"') {
        ++pos; // skip opening '"'
        size_t count = 0;
        while (pos < src.size()) {
            if (src[pos] == '\\') {
                pos += (pos + 1 < src.size()) ? 2 : 1;
                ++count;
                continue;
            }
            if (src[pos] == '"') { ++pos; break; }
            ++pos;
            ++count;
        }
        out += '"';
        out.append(count, '*');
        out += '"';
    } else if (src[pos] == '{' || src[pos] == '[') {
        // Skip the entire nested structure and emit a single "*"
        char open  = src[pos];
        char close = (open == '{') ? '}' : ']';
        size_t depth = 0;
        while (pos < src.size()) {
            char ch = src[pos];
            if (ch == '"')       { pos = SkipStr(src, pos); continue; }
            if (ch == open)      { ++depth; ++pos; }
            else if (ch == close) { ++pos; if (--depth == 0) break; }
            else                 { ++pos; }
        }
        out += '"';
        out += '*';
        out += '"';
    } else {
        // Number, boolean, null — skip to delimiter and emit same-length stars
        size_t start = pos;
        while (pos < src.size() &&
               src[pos] != ',' && src[pos] != '}' && src[pos] != ']' &&
               src[pos] != ' ' && src[pos] != '\t' &&
               src[pos] != '\r' && src[pos] != '\n') {
            ++pos;
        }
        out += '"';
        out.append(pos - start, '*');
        out += '"';
    }
}

// Forward declaration
static void ProcessValue(const std::string& src, size_t& pos, std::string& out,
                         const std::vector<std::string>& kws);

static void ProcessObject(const std::string& src, size_t& pos, std::string& out,
                          const std::vector<std::string>& kws) {
    out += '{';
    ++pos; // skip '{'
    bool first = true;

    while (pos < src.size()) {
        pos = SkipWS(src, pos);
        if (pos >= src.size()) break;
        if (src[pos] == '}') { out += '}'; ++pos; return; }

        if (!first) {
            if (src[pos] == ',') { out += ','; ++pos; }
            pos = SkipWS(src, pos);
            if (pos >= src.size()) break;
            if (src[pos] == '}') { out += '}'; ++pos; return; } // trailing comma
        }
        first = false;

        if (src[pos] != '"') break; // malformed JSON

        // Copy raw key and decode it simultaneously
        size_t rawStart = pos;
        std::string key = ReadStr(src, pos);
        out.append(src, rawStart, pos - rawStart);

        pos = SkipWS(src, pos);
        if (pos < src.size() && src[pos] == ':') { out += ':'; ++pos; }
        pos = SkipWS(src, pos);

        if (IsKeyword(key, kws)) {
            WriteMasked(src, pos, out);
        } else {
            ProcessValue(src, pos, out, kws);
        }
    }
}

static void ProcessArray(const std::string& src, size_t& pos, std::string& out,
                         const std::vector<std::string>& kws) {
    out += '[';
    ++pos; // skip '['
    bool first = true;

    while (pos < src.size()) {
        pos = SkipWS(src, pos);
        if (pos >= src.size()) break;
        if (src[pos] == ']') { out += ']'; ++pos; return; }

        if (!first) {
            if (src[pos] == ',') { out += ','; ++pos; }
            pos = SkipWS(src, pos);
            if (pos >= src.size()) break;
            if (src[pos] == ']') { out += ']'; ++pos; return; } // trailing comma
        }
        first = false;

        ProcessValue(src, pos, out, kws);
    }
}

static void ProcessValue(const std::string& src, size_t& pos, std::string& out,
                         const std::vector<std::string>& kws) {
    if (pos >= src.size()) return;

    char ch = src[pos];
    if (ch == '{') {
        ProcessObject(src, pos, out, kws);
    } else if (ch == '[') {
        ProcessArray(src, pos, out, kws);
    } else if (ch == '"') {
        size_t start = pos;
        pos = SkipStr(src, pos);
        out.append(src, start, pos - start);
    } else {
        // Number, boolean, null — copy until delimiter
        while (pos < src.size() &&
               src[pos] != ',' && src[pos] != '}' && src[pos] != ']' &&
               src[pos] != ' ' && src[pos] != '\t' &&
               src[pos] != '\r' && src[pos] != '\n') {
            out += src[pos++];
        }
    }
}

} // namespace

// ── Public API ────────────────────────────────────────────────────────────────

std::string MaskJson(const std::string& json, const std::vector<std::string>& keywords) {
    if (json.empty() || keywords.empty() || json.size() > kMaskSizeLimit)
        return json;

    std::string out;
    out.reserve(json.size());
    size_t pos = SkipWS(json, 0);
    ProcessValue(json, pos, out, keywords);
    return out;
}

void MaskHeaders(std::map<std::string, std::string>& headers,
                 const std::vector<std::string>& keywords) {
    if (keywords.empty()) return;
    for (auto& kv : headers) {
        for (const auto& kw : keywords) {
            if (_stricmp(kv.first.c_str(), kw.c_str()) == 0) {
                kv.second.assign(kv.second.size(), '*');
                break;
            }
        }
    }
}
