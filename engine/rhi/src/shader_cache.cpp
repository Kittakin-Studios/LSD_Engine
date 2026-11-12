// Compiled shaders on disk (see shader_cache.h).
#include "lsd/rhi/shader_cache.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

namespace lsd::rhi {

namespace {

constexpr char kMagic[8] = {'L', 'S', 'D', 'S', 'H', 'D', '1', '\0'};
constexpr u64 kMaxShaderBytes = 64ull * 1024 * 1024;  // anything larger is a damaged header

struct Header {
    char magic[8];
    u64 key;
    u64 size;
    u64 checksum;  // FNV-1a of the bytecode
};

u64 fnv(u64 h, const void* data, usize size) {
    const u8* p = static_cast<const u8*>(data);
    for (usize i = 0; i < size; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

std::filesystem::path file_of(const char* folder, u64 key) {
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.lsdshader", static_cast<unsigned long long>(key));
    return std::filesystem::path(folder) / name;
}

} // namespace

u64 shader_cache_key(std::string_view source, std::string_view entry, std::string_view target, std::string_view defines, std::string_view compiler) {
    u64 h = 0xcbf29ce484222325ull;
    for (const std::string_view part : {source, entry, target, defines, compiler}) {
        const u64 length = part.size();  // with each part's length: "ab" + "c" is not "a" + "bc"
        h = fnv(h, &length, sizeof length);
        h = fnv(h, part.data(), part.size());
    }
    return h;
}

const char* shader_cache_folder(bool project) {
    if (project) return "DerivedData/Shaders";
    static const std::string user = [] {
#ifdef _WIN32
        const char* base = std::getenv("LOCALAPPDATA");
        if (base && *base) return std::string(base) + "/LSD_Engine/ShaderCache";
#else
        const char* cache = std::getenv("XDG_CACHE_HOME");
        if (cache && *cache) return std::string(cache) + "/lsd_engine/shaders";
        const char* home = std::getenv("HOME");
        if (home && *home) return std::string(home) + "/.cache/lsd_engine/shaders";
#endif
        return std::string("DerivedData/Shaders");  // no user folder: the project's
    }();
    return user.c_str();
}

bool load_cached_shader(const char* folder, u64 key, std::vector<u8>& out) {
    FILE* f = std::fopen(file_of(folder, key).string().c_str(), "rb");
    if (!f) return false;
    Header h{};
    bool ok = std::fread(&h, sizeof h, 1, f) == 1 && std::memcmp(h.magic, kMagic, sizeof kMagic) == 0 && h.key == key && h.size > 0 && h.size <= kMaxShaderBytes;
    if (ok) {
        out.resize(static_cast<usize>(h.size));
        ok = std::fread(out.data(), 1, out.size(), f) == out.size() && std::fgetc(f) == EOF && fnv(0xcbf29ce484222325ull, out.data(), out.size()) == h.checksum;
    }
    std::fclose(f);
    if (!ok) out.clear();
    return ok;
}

bool store_cached_shader(const char* folder, u64 key, const void* data, usize size) {
    namespace fs = std::filesystem;
    if (!data || !size) return false;
    std::error_code ec;
    fs::create_directories(folder, ec);
    const fs::path path = file_of(folder, key);
    // A name of its own (thread and time: other threads and editors may write the same shader), then renamed over the real one.
    const usize unique = std::hash<std::thread::id>{}(std::this_thread::get_id()) ^ static_cast<usize>(std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path temp = fs::path(path.string() + "." + std::to_string(unique) + ".tmp");
    FILE* f = std::fopen(temp.string().c_str(), "wb");
    if (!f) return false;
    Header h{};
    std::memcpy(h.magic, kMagic, sizeof kMagic);
    h.key = key;
    h.size = size;
    h.checksum = fnv(0xcbf29ce484222325ull, data, size);
    const bool written = std::fwrite(&h, sizeof h, 1, f) == 1 && std::fwrite(data, 1, size, f) == size;
    bool ok = std::fclose(f) == 0 && written;
    if (ok) fs::rename(temp, path, ec);
    ok = ok && !ec;
    if (fs::exists(temp, ec)) fs::remove(temp, ec);  // lost the race or failed: the other copy (or none) stays
    return ok;
}

bool find_cached_shader(u64 key, std::vector<u8>& out) {
    return load_cached_shader(shader_cache_folder(true), key, out) || load_cached_shader(shader_cache_folder(false), key, out);
}

} // namespace lsd::rhi
