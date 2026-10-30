#pragma once
// Small JSON reader used by load_text, public so other text formats (levels) can read the parts
// around reflected objects and hand each object to read_object(). Logs the first error.
#include "lsd/foundation/log.h"
#include "lsd/reflect/reflect.h"

#include <cstdlib>

namespace lsd::reflect {

struct JsonReader {
    const char* p;
    const char* end;
    int line = 1;
    bool ok = true;

    void fail(const char* what) {
        if (ok) LSD_LOG_ERROR("reflect", "Load failed at line %d: %s", line, what);
        ok = false;
    }
    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
            if (*p == '\n') ++line;
            ++p;
        }
    }
    bool peek(char c) { skip_ws(); return p < end && *p == c; }
    bool accept(char c) {
        if (!peek(c)) return false;
        ++p;
        return true;
    }
    void expect(char c) {
        if (!accept(c)) {
            char msg[] = "expected 'x'";
            msg[10] = c;
            fail(msg);
        }
    }
    // Reads a quoted string into out (truncated to capacity).
    void read_string(char* out, usize capacity) {
        expect('"');
        usize n = 0;
        while (ok && p < end && *p != '"') {
            char c = *p;
            if (c == '\\' && p + 1 < end) {
                c = *++p;
                if (c == 'n') c = '\n';
                else if (c == 't') c = '\t';
                else if (c == 'u' && p + 4 < end) {  // \u00XX only (we write nothing wider)
                    char hex[5] = {p[1], p[2], p[3], p[4], 0};
                    c = static_cast<char>(std::strtol(hex, nullptr, 16));
                    p += 4;
                }
            }
            if (*p == '\n') ++line;
            if (n + 1 < capacity) out[n++] = c;
            ++p;
        }
        if (capacity) out[n] = '\0';
        if (p >= end) fail("unterminated string");
        else ++p;
    }
    // Reads a bare token (number, true, false, null) into out.
    void read_token(char* out, usize capacity) {
        skip_ws();
        usize n = 0;
        while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            if (n + 1 < capacity) out[n++] = *p;
            ++p;
        }
        out[n] = '\0';
        if (n == 0) fail("expected a value");
    }
    void skip_value() {
        if (peek('"')) {
            char ignored[1];
            read_string(ignored, sizeof ignored);
        } else if (accept('{') || accept('[')) {
            const char close = p[-1] == '{' ? '}' : ']';
            if (accept(close)) return;
            do {
                if (close == '}') {
                    char key[1];
                    read_string(key, sizeof key);
                    expect(':');
                }
                skip_value();
            } while (ok && accept(','));
            expect(close);
        } else {
            char token[64];
            read_token(token, sizeof token);
        }
    }
};

// Reads one JSON object into a reflected object (same rules as load_text). Check reader.ok after.
void read_object(JsonReader& reader, const TypeInfo& type, void* object);

} // namespace lsd::reflect
