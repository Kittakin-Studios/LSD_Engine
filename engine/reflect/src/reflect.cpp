#include "lsd/reflect/reflect.h"
#include "lsd/reflect/json.h"
#include "lsd/foundation/allocator.h"
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

// JSON string with the escapes JSON requires; control characters become \u00XX.
void write_string(Writer& w, const char* text, u32 capacity) {
    w.put("\"");
    for (u32 i = 0; i < capacity && text[i]; ++i) {
        const char c = text[i];
        if (c == '"' || c == '\\') {
            const char escaped[2] = {'\\', c};
            w.put(escaped, 2);
        } else if (static_cast<u8>(c) < 0x20) {
            w.putf("\\u%04x", static_cast<unsigned>(c));
        } else {
            w.put(&c, 1);
        }
    }
    w.put("\"");
}

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
            case FieldType::String: write_string(w, reinterpret_cast<const char*>(field), p.size); break;
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

using Reader = JsonReader;

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
    if (prop.type == FieldType::String) {
        r.read_string(reinterpret_cast<char*>(field), prop.size);
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
        case FieldType::String:
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

// ---- Binary ----

struct BinaryWriter {
    u8* out;
    usize capacity;
    usize length = 0;
    void put(const void* data, usize n) {
        if (length + n <= capacity) std::memcpy(out + length, data, n);
        length += n;
    }
};

// Property record: u64 name hash, u32 payload size, payload. Structs nest the same format.
void write_binary(BinaryWriter& w, const TypeInfo& type, const u8* object) {
    const u32 count = type.property_count;
    w.put(&count, sizeof count);
    for (u32 i = 0; i < type.property_count; ++i) {
        const Property& p = type.properties[i];
        const u64 id = hash64(p.name);
        w.put(&id, sizeof id);
        if (p.type == FieldType::Struct) {
            const usize size_at = w.length;
            u32 size = 0;
            w.put(&size, sizeof size);
            const usize start = w.length;
            write_binary(w, *p.struct_type, object + p.offset);
            size = static_cast<u32>(w.length - start);
            if (size_at + sizeof size <= w.capacity) std::memcpy(w.out + size_at, &size, sizeof size);
        } else {
            w.put(&p.size, sizeof p.size);
            w.put(object + p.offset, p.size);
        }
    }
}

bool read_binary(const TypeInfo& type, u8* object, const u8* data, usize size) {
    usize at = 0;
    auto take = [&](void* dst, usize n) {
        if (at + n > size) return false;
        std::memcpy(dst, data + at, n);
        at += n;
        return true;
    };
    u32 count = 0;
    if (!take(&count, sizeof count)) return false;
    for (u32 i = 0; i < count; ++i) {
        u64 id = 0;
        u32 bytes = 0;
        if (!take(&id, sizeof id) || !take(&bytes, sizeof bytes) || at + bytes > size) return false;
        const Property* match = nullptr;
        for (u32 k = 0; k < type.property_count && !match; ++k) {
            if (hash64(type.properties[k].name) == id) match = &type.properties[k];
        }
        if (match && match->type == FieldType::Struct) {
            if (!read_binary(*match->struct_type, object + match->offset, data + at, bytes)) return false;
        } else if (match && match->type == FieldType::String) {
            const u32 n = bytes < match->size ? bytes : match->size;
            std::memcpy(object + match->offset, data + at, n);
            object[match->offset + match->size - 1] = 0;  // keep it terminated if it was truncated
        } else if (match && bytes == match->size) {
            std::memcpy(object + match->offset, data + at, bytes);
        }  // else: removed property or changed type -> keep the default
        at += bytes;
    }
    return true;
}

} // namespace

usize save_binary(const TypeInfo& type, const void* object, u8* out, usize capacity) {
    BinaryWriter w{out, capacity};
    write_binary(w, type, static_cast<const u8*>(object));
    return w.length;
}

bool load_binary(const TypeInfo& type, void* object, const u8* data, usize size) {
    return read_binary(type, static_cast<u8*>(object), data, size);
}

const Property* find_property(const TypeInfo& type, const char* name) {
    for (u32 i = 0; i < type.property_count; ++i) {
        if (std::strcmp(type.properties[i].name, name) == 0) return &type.properties[i];
    }
    return nullptr;
}

void read_object(JsonReader& reader, const TypeInfo& type, void* object) { read_object(reader, type, static_cast<u8*>(object)); }

usize save_text(const TypeInfo& type, const void* object, char* out, usize capacity, u32 depth) {
    Writer w{out, capacity};
    write_object(w, type, static_cast<const u8*>(object), depth);
    if (depth == 0) w.put("\n", 1);
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

bool save_file(const TypeInfo& type, const void* object, const char* path) {
    Allocator& heap = default_allocator();
    const usize length = save_text(type, object, nullptr, 0);
    char* text = static_cast<char*>(heap.allocate(length + 1, 1));
    save_text(type, object, text, length + 1);
    bool ok = false;
    if (FILE* file = std::fopen(path, "wb")) {
        ok = std::fwrite(text, 1, length, file) == length;
        ok = std::fclose(file) == 0 && ok;
    }
    heap.deallocate(text);
    if (!ok) LSD_LOG_ERROR("reflect", "Could not write %s", path);
    return ok;
}

bool load_file(const TypeInfo& type, void* object, const char* path) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size < 0) { std::fclose(file); return false; }
    Allocator& heap = default_allocator();
    char* text = static_cast<char*>(heap.allocate(static_cast<usize>(size) + 1, 1));
    const usize length = std::fread(text, 1, static_cast<usize>(size), file);
    std::fclose(file);

    void* copy = heap.allocate(type.size, type.alignment);
    std::memcpy(copy, object, type.size);
    const bool ok = load_text(type, copy, text, length);
    if (ok) std::memcpy(object, copy, type.size);
    else LSD_LOG_ERROR("reflect", "Could not load %s", path);
    heap.deallocate(copy);
    heap.deallocate(text);
    return ok;
}

} // namespace lsd::reflect
