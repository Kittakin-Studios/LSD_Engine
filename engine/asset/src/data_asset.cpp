#include "lsd/asset/data_asset.h"

#include "lsd/foundation/allocator.h"
#include "lsd/foundation/log.h"
#include "lsd/reflect/json.h"

#include <cstdio>
#include <cstring>

namespace lsd::asset {

namespace {

constexpr u32 kMaxDataTypes = 64;
const reflect::TypeInfo* g_types[kMaxDataTypes];
u32 g_type_count = 0;

// Reads a whole file into heap memory (caller frees). nullptr if it cannot be read.
char* read_file(const char* path, usize& length) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return nullptr;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(file);
        return nullptr;
    }
    char* text = static_cast<char*>(default_allocator().allocate(static_cast<usize>(size) + 1, 1));
    length = std::fread(text, 1, static_cast<usize>(size), file);
    std::fclose(file);
    return text;
}

// Walks the top-level keys; reads "type" into type_name and, if `type` matches, "data" into object.
bool parse(const char* text, usize length, char* type_name, usize type_capacity, const reflect::TypeInfo* type, void* object) {
    reflect::JsonReader r{text, text + length};
    bool have_type = false;
    r.expect('{');
    while (r.ok && !r.peek('}')) {
        char key[32];
        r.read_string(key, sizeof key);
        r.expect(':');
        if (std::strcmp(key, "type") == 0) {
            r.read_string(type_name, type_capacity);
            have_type = true;
        } else if (std::strcmp(key, "data") == 0 && type && have_type && std::strcmp(type_name, type->name) == 0) {
            reflect::read_object(r, *type, object);
        } else {
            r.skip_value();
        }
        if (!r.accept(',')) break;
    }
    r.expect('}');
    return r.ok && have_type;
}

} // namespace

void register_data_type(const reflect::TypeInfo& type) {
    if (find_data_type(type.name)) return;
    if (g_type_count == kMaxDataTypes) {
        LSD_LOG_ERROR("asset", "Too many data asset types; %s not registered", type.name);
        return;
    }
    g_types[g_type_count++] = &type;
}

const reflect::TypeInfo* find_data_type(const char* name) {
    for (u32 i = 0; i < g_type_count; ++i) {
        if (std::strcmp(g_types[i]->name, name) == 0) return g_types[i];
    }
    return nullptr;
}

const reflect::TypeInfo* const* data_types(u32& count) {
    count = g_type_count;
    return g_types;
}

bool save_data_asset(const char* path, const reflect::TypeInfo& type, const void* object) {
    Allocator& heap = default_allocator();
    const usize body = reflect::save_text(type, object, nullptr, 0, 1);
    char* text = static_cast<char*>(heap.allocate(body + 128, 1));
    const int head = std::snprintf(text, 128, "{\n  \"type\": \"%s\",\n  \"data\": ", type.name);
    reflect::save_text(type, object, text + head, body + 1, 1);
    const usize length = static_cast<usize>(head) + body;
    std::memcpy(text + length, "\n}\n", 3);
    bool ok = false;
    if (FILE* file = std::fopen(path, "wb")) {
        ok = std::fwrite(text, 1, length + 3, file) == length + 3;
        ok = std::fclose(file) == 0 && ok;
    }
    heap.deallocate(text);
    if (!ok) LSD_LOG_ERROR("asset", "Could not write %s", path);
    return ok;
}

bool load_data_asset(const char* path, const reflect::TypeInfo& type, void* object) {
    usize length = 0;
    char* text = read_file(path, length);
    if (!text) return false;
    Allocator& heap = default_allocator();
    void* copy = heap.allocate(type.size, type.alignment);
    std::memcpy(copy, object, type.size);
    char type_name[64] = "";
    bool ok = parse(text, length, type_name, sizeof type_name, &type, copy);
    if (ok && std::strcmp(type_name, type.name) != 0) {
        LSD_LOG_ERROR("asset", "%s holds a %s, not a %s", path, type_name, type.name);
        ok = false;
    }
    if (ok) std::memcpy(object, copy, type.size);
    heap.deallocate(copy);
    heap.deallocate(text);
    return ok;
}

bool read_data_asset_type(const char* path, char* out, usize capacity) {
    usize length = 0;
    char* text = read_file(path, length);
    if (!text) return false;
    const bool ok = parse(text, length, out, capacity, nullptr, nullptr);
    default_allocator().deallocate(text);
    return ok;
}

} // namespace lsd::asset
