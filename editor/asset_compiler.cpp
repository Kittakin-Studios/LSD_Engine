#include "asset_compiler.h"

#include "lod_cache.h"

#include "lsd/asset/data_asset.h"
#include "lsd/asset/mesh_import.h"
#include "lsd/asset/texture.h"
#include "lsd/foundation/allocator.h"
#include "lsd/foundation/log.h"
#include "lsd/render/material.h"

#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace lsd::editor::compiler {

namespace {

// shortcut: std containers and threads (editor-time work measured in seconds per job, not a hot path).
struct Job {
    bool texture = false;
    asset::MeshCompileJob mesh;
    bool compile_mesh = false;  // else only its generated LODs
    std::string path;  // textures: the source file
    bool srgb = false, normal_map = false;
    u32 max_size = 0;
    std::string key;   // what busy() looks for: the compiled mesh's path, or the texture's path and use
};

struct Pool {
    std::mutex mutex;
    std::condition_variable wake, idle;
    std::deque<Job> jobs;
    std::unordered_set<std::string> busy;  // queued or running
    std::vector<std::thread> workers;
    u32 running = 0;
    u32 done = 0, total = 0, failed = 0;
    bool quit = false;
};

Pool& pool() {
    static Pool p;
    return p;
}

void work() {
    Pool& p = pool();
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(p.mutex);
            p.wake.wait(lock, [&] { return p.quit || !p.jobs.empty(); });
            if (p.quit) return;
            job = std::move(p.jobs.front());
            p.jobs.pop_front();
            ++p.running;
        }
        bool ok = true;
        if (job.texture) {
            asset::TextureData data(default_allocator());
            ok = asset::load_texture_cached(job.path.c_str(), job.srgb, job.max_size, true, "DerivedData/Textures", data, job.normal_map, true);
        } else {
            // The mesh, then its generated LODs (simplifying a dense mesh is most of its first load: not on the main thread).
            f32 sizes[render::kMaxLods];
            render::lod_screen_sizes(job.mesh.lod_screen_size[0], job.mesh.lod_screen_size[1], job.mesh.lod_screen_size[2], sizes);
            ok = (!job.compile_mesh || asset::run_mesh_compile(job.mesh, default_allocator())) && warm_lod_cache(job.mesh.compiled, sizes, default_allocator());
        }
        std::lock_guard<std::mutex> lock(p.mutex);
        p.busy.erase(job.key);
        --p.running;
        ++p.done;
        p.failed += ok ? 0u : 1u;
        if (p.jobs.empty() && p.running == 0) p.idle.notify_all();
    }
}

// Starts the workers on first use: half the cores (at least 2), the rest stay for the editor's frame and its jobs.
void push(Job job) {
    Pool& p = pool();
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        if (p.workers.empty()) {
            const u32 cores = std::thread::hardware_concurrency();
            const u32 count = cores / 2 > 2 ? cores / 2 : 2;
            for (u32 i = 0; i < count; ++i) p.workers.emplace_back(work);
        }
        if (!p.busy.insert(job.key).second) return;  // already queued
        if (p.jobs.empty() && p.running == 0) p.done = p.total = p.failed = 0;  // a new batch
        ++p.total;
        p.jobs.push_back(std::move(job));
    }
    p.wake.notify_one();
}

bool is_busy(const std::string& key) {
    Pool& p = pool();
    std::lock_guard<std::mutex> lock(p.mutex);
    return p.busy.count(key) != 0;
}

} // namespace

bool mesh_busy(const char* key, const char* data_dir) {
    asset::Guid guid;
    i32 part = -1;
    if (!asset::parse_mesh_key(key, guid, part)) return false;
    char compiled[300];
    asset::compiled_mesh_path(data_dir, guid, part, compiled, sizeof compiled);
    return is_busy(compiled);
}

bool queue_mesh(const asset::AssetRegistry& registry, const char* key, const char* data_dir) {
    if (mesh_busy(key, data_dir)) return true;
    asset::Guid guid;
    i32 part = -1;
    const asset::AssetRecord* record = asset::parse_mesh_key(key, guid, part) ? registry.find(guid) : nullptr;
    if (!record || record->type != asset::AssetType::Mesh) return false;
    Job job;
    job.compile_mesh = asset::prepare_mesh_compile(registry, *record, data_dir, part, job.mesh);
    if (!job.compile_mesh) {  // compiled: are its generated LODs too?
        f32 sizes[render::kMaxLods];
        render::lod_screen_sizes(job.mesh.lod_screen_size[0], job.mesh.lod_screen_size[1], job.mesh.lod_screen_size[2], sizes);
        std::error_code error;
        const i64 time = static_cast<i64>(std::filesystem::last_write_time(job.mesh.compiled, error).time_since_epoch().count());
        if (lod_cache_current((std::string(job.mesh.compiled) + ".lod").c_str(), time, sizes)) return false;  // up to date
        u32 authored = 0;
        render::MeshData unused[asset::kMaxAuthoredLods];
        if (asset::load_compiled_lods(job.mesh.compiled, default_allocator(), unused, authored) && authored) {  // drawn with the file's own LODs
            for (u32 l = 0; l < authored; ++l) render::free_mesh(default_allocator(), unused[l]);
            return false;
        }
    }
    job.key = job.mesh.compiled;
    push(std::move(job));
    return true;
}

void queue_texture(const char* path, bool srgb, bool normal_map, u32 max_size) {
    Job job;
    job.texture = true;
    job.path = path;
    job.srgb = srgb;
    job.normal_map = normal_map;
    job.max_size = max_size;
    job.key = job.path + (srgb ? "|srgb" : "") + (normal_map ? "|normal" : "") + "|" + std::to_string(max_size);
    push(std::move(job));
}

u32 compile_all(const asset::AssetRegistry& registry, const char* data_dir, bool compress) {
    u32 queued = 0;
    std::unordered_set<std::string> textures;  // each texture once per use
    auto add_texture = [&](const char* guid_text, bool srgb, bool normal_map) {
        asset::Guid guid;
        const asset::AssetRecord* r = guid_text[0] && asset::parse_guid(guid_text, guid) ? registry.find(guid) : nullptr;
        if (!r || r->type != asset::AssetType::Texture || !compress || !(srgb || normal_map)) return;  // only compressed maps are cached
        char path[300];
        registry.source_path(*r, path, sizeof path);
        asset::AssetMeta meta;
        registry.load_meta(*r, meta);
        const std::string key = std::string(path) + (srgb ? "|s" : "|n");
        if (!textures.insert(key).second) return;
        queue_texture(path, srgb, normal_map, meta.max_size);
        ++queued;
    };
    for (usize i = 0; i < registry.count(); ++i) {
        const asset::AssetRecord& r = registry.get(i);
        if (r.type == asset::AssetType::Mesh) {
            char key[33];
            asset::to_string(r.guid, key);
            queued += queue_mesh(registry, key, data_dir) ? 1u : 0u;
        } else if (r.type == asset::AssetType::Data && (std::strcmp(r.data_type, "Material") == 0 || std::strcmp(r.data_type, "MaterialInstance") == 0)) {
            char path[300];
            registry.source_path(r, path, sizeof path);
            if (std::strcmp(r.data_type, "Material") == 0) {
                render::Material m;
                if (!asset::load_data_asset(path, m)) continue;
                add_texture(m.base_color_texture, true, false);  // as MaterialCache::apply asks for them
                add_texture(m.normal_texture, false, true);
                add_texture(m.emissive_texture, true, false);
            } else {
                render::MaterialInstance m;
                if (!asset::load_data_asset(path, m)) continue;
                if (m.override_base_color_texture) add_texture(m.base_color_texture, true, false);
                if (m.override_normal_texture) add_texture(m.normal_texture, false, true);
            }
        }
    }
    LSD_LOG_INFO("asset", "Compile All Assets: %u to compile on %zu worker threads", queued, pool().workers.size());
    return queued;
}

Progress progress() {
    Pool& p = pool();
    std::lock_guard<std::mutex> lock(p.mutex);
    return {p.done, p.total, p.failed};
}

bool busy() {
    Pool& p = pool();
    std::lock_guard<std::mutex> lock(p.mutex);
    return !p.jobs.empty() || p.running != 0;
}

void wait() {
    Pool& p = pool();
    std::unique_lock<std::mutex> lock(p.mutex);
    p.idle.wait(lock, [&] { return p.jobs.empty() && p.running == 0; });
}

void shutdown() {
    Pool& p = pool();
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.quit = true;
        p.jobs.clear();
        p.busy.clear();
    }
    p.wake.notify_all();
    for (std::thread& t : p.workers) {
        if (t.joinable()) t.join();
    }
    p.workers.clear();
    p.quit = false;
}

} // namespace lsd::editor::compiler
