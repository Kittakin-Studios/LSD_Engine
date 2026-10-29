#include "lsd/reflect/reflect.h"
#include "lsd/foundation/log.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lsd::reflect {

namespace {

constexpr u32 kSavedFlags = kEdit | kVisible | kSave;

// ---- Writer ----

struct Writer {
    char* out;
    usize capacity;
    usize length = 0;

    void put(const char* text, usize n) {
        for (usize i = 0; i < n; ++i, ++length) {
            if (length + 1 < capacity) out[length] = text[i];
        }
    }
    void put(const char* text) { put(text, std::strlen(text)); }
    void indent(u32 depth) { for (u32 i = 0; i < depth; ++i) put("  ", 2); }
    template <typename... Args>
    void putf(const char* fmt, Args... args) {
        char buffer[64];
        const int n = std::snprintf(buffer, sizeof buffer, fmt, args...);
        put(buffer, static_cast<usize>(n));
    }
};

void write_object(Writer& w, const TypeInfo& type, const u8* object, u32 depth) {
    w.put("{");
    bool first = true;
    for (u32 i = 0; i < type.property_count; ++i) {
        const Property& p = type.properties[i];
        if (!(p.flags & kSavedFlags)) continue;
        w.put(first ? "\n" : ",\n");
        first = false;
        w.indent(depth + 1);
        w.put("\"");
        w.put(p.name);
        w.put("\": ");
        const u8* field = object + p.offset;
        switch (p.type) {
            case FieldType::Bool: w.put(*reinterpret_cast<const bool*>(field) ? "true" : "false"); break;
            case FieldType::I32:  w.putf("%d", *reinterpret_cast<const i32*>(field)); break;
            case FieldType::U32:  w.putf("%u", *reinterpret_cast<const u32*>(field)); break;
            case FieldType::I64:  w.putf("%lld", static_cast<long long>(*reinterpret_cast<const i64*>(field))); break;
            case FieldType::U64:  w.putf("%llu", static_cast<unsigned long long>(*reinterpret_cast<const u64*>(field))); break;
            // 9 / 17 significant digits round-trip f32 / f64 exactly.
            case FieldType::F32:  w.putf("%.9g", static_cast<f64>(*reinterpret_cast<const f32*>(field))); break;
            case FieldType::F64:  w.putf("%.17g", *reinterpret_cast<const f64*>(field)); break;
            case FieldType::Struct: write_object(w, *p.struct_type, field, depth + 1); break;
        }
    }
    if (!first) {
        w.put("\n");
        w.indent(depth);
    }
    w.put("}");
}

// ---- Reader ----

struct Reader {
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
            if (*p == '\\' && p + 1 < end) ++p;
            if (*p == '\n') ++line;
            if (n + 1 < capacity) out[n++] = *p;
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

template <typename T>
bool parse_int(const char* token, T& out, long long lo, unsigned long long hi) {
    char* stop = nullptr;
    errno = 0;
    if (lo < 0) {
        const long long v = std::strtoll(token, &stop, 10);
        if (errno || *stop || v < lo || v > static_cast<long long>(hi)) return false;
        out = static_cast<T>(v);
    } else {
        if (token[0] == '-') return false;
        const unsigned long long v = std::strtoull(token, &stop, 10);
        if (errno || *stop || v > hi) return false;
        out = static_cast<T>(v);
    }
    return true;
}

void read_object(Reader& r, const TypeInfo& type, u8* object);

void read_property(Reader& r, const Property& prop, u8* field) {
    if (prop.type == FieldType::Struct) {
        read_object(r, *prop.struct_type, field);
        return;
    }
    char token[64];
    r.read_token(token, sizeof token);
    if (!r.ok) return;
    bool valid = true;
    char* stop = nullptr;
    switch (prop.type) {
        case FieldType::Bool:
            valid = std::strcmp(token, "true") == 0 || std::strcmp(token, "false") == 0;
            if (valid) *reinterpret_cast<bool*>(field) = token[0] == 't';
            break;
        case FieldType::I32: valid = parse_int(token, *reinterpret_cast<i32*>(field), INT32_MIN, INT32_MAX); break;
        case FieldType::U32: valid = parse_int(token, *reinterpret_cast<u32*>(field), 0, UINT32_MAX); break;
        case FieldType::I64: valid = parse_int(token, *reinterpret_cast<i64*>(field), INT64_MIN, INT64_MAX); break;
        case FieldType::U64: valid = parse_int(token, *reinterpret_cast<u64*>(field), 0, UINT64_MAX); break;
        case FieldType::F32: {
            const f32 v = std::strtof(token, &stop);
            valid = *stop == '\0';
            if (valid) *reinterpret_cast<f32*>(field) = v;
            break;
        }
        case FieldType::F64: {
            const f64 v = std::strtod(token, &stop);
            valid = *stop == '\0';
            if (valid) *reinterpret_cast<f64*>(field) = v;
            break;
        }
        case FieldType::Struct: break;
    }
    if (!valid) {
        char msg[128];
        std::snprintf(msg, sizeof msg, "bad value '%s' for '%s'", token, prop.name);
        r.fail(msg);
    }
}

void read_object(Reader& r, const TypeInfo& type, u8* object) {
    r.expect('{');
    if (!r.ok || r.accept('}')) return;
    do {
        char key[128];
        r.read_string(key, sizeof key);
        r.expect(':');
        if (!r.ok) return;
        const Property* prop = find_property(type, key);
        if (prop && (prop->flags & kSavedFlags)) read_property(r, *prop, object + prop->offset);
        else r.skip_value();  // removed or renamed property: ignore
    } while (r.ok && r.accept(','));
    r.expect('}');
}

} // namespace

const Property* find_property(const TypeInfo& type, const char* name) {
    for (u32 i = 0; i < type.property_count; ++i) {
        if (std::strcmp(type.properties[i].name, name) == 0) return &type.properties[i];
    }
    return nullptr;
}

usize save_text(const TypeInfo& type, const void* object, char* out, usize capacity) {
    Writer w{out, capacity};
    write_object(w, type, static_cast<const u8*>(object), 0);
    w.put("\n", 1);
    if (capacity) out[w.length < capacity ? w.length : capacity - 1] = '\0';
    return w.length;
}

bool load_text(const TypeInfo& type, void* object, const char* text, usize length) {
    Reader r{text, text + length};
    read_object(r, type, static_cast<u8*>(object));
    r.skip_ws();
    if (r.ok && r.p != r.end) r.fail("unexpected text after the object");
    return r.ok;
}

} // namespace lsd::reflect
