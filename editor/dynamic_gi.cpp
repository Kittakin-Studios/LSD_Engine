#include "dynamic_gi.h"

#include "light_bake.h"

#include "lsd/asset/asset_registry.h"
#include "lsd/asset/data_asset.h"
#include "lsd/foundation/hash_id.h"
#include "lsd/foundation/profile.h"
#include "lsd/render/material.h"
#include "lsd/render/mesh.h"
#include "lsd/world/components.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string_view>

namespace lsd::editor {

bool load_level_mesh(const char* key, const asset::AssetRegistry& registry, const char* data_dir, Allocator& allocator, render::MeshData& out);  // light_bake.cpp

namespace {
constexpr f32 kBlend = 0.5f;  // how much of a sweep is new: noise averages over a few sweeps, a moved light shows in two
f64 now_ms() { return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
u64 mix(u64 h, const void* data, usize size) { return hash64(std::string_view(static_cast<const char*>(data), size)) ^ (h * 1099511628211ull); }
} // namespace

void gi_grid_origin(Vec3 eye, f32 cell, const u32 dims[3], i32 out[3]) {
    const f32 at[3] = {eye.x, eye.y, eye.z};
    for (int a = 0; a < 3; ++a) out[a] = static_cast<i32>(std::floor(at[a] / cell)) - static_cast<i32>(dims[a] / 2);
}

void gi_scroll(const i32 from[3], const i32 to[3], const u32 dims[3], const f32* in, f32* out, u8* kept) {
    const u32 count = dims[0] * dims[1] * dims[2];
    for (u32 i = 0; i < count; ++i) {
        const i32 local[3] = {static_cast<i32>(i % dims[0]), static_cast<i32>((i / dims[0]) % dims[1]), static_cast<i32>(i / (dims[0] * dims[1]))};
        i32 old[3];
        bool inside = true;
        for (int a = 0; a < 3; ++a) {
            old[a] = local[a] + to[a] - from[a];  // the same world cell in the old grid
            inside = inside && old[a] >= 0 && old[a] < static_cast<i32>(dims[a]);
        }
        kept[i] = inside ? 1 : 0;
        const usize j = inside ? static_cast<usize>(old[0]) + static_cast<usize>(old[1]) * dims[0] + static_cast<usize>(old[2]) * dims[0] * dims[1] : 0;
        for (int k = 0; k < 12; ++k) out[static_cast<usize>(i) * 12 + static_cast<usize>(k)] = inside ? in[j * 12 + static_cast<usize>(k)] : 0.0f;
    }
}

void DynamicGi::stop() {
    if (worker_.joinable()) worker_.join();
    meshes_.clear();
    materials_.clear();
    tracer_.reset();
    pending_tracer_.reset();
    scene_hash_ = 0;
    sh_.clear();
    have_ = false;
}

void DynamicGi::update(const world::World& w, const asset::AssetRegistry& registry, const char* data_dir, Vec3 eye, rhi::LightSetup& lights) {
    LSD_PROFILE_SCOPE("dynamic_gi");
    if (!enabled || !w.settings().dynamic_gi) {
        if (have_ || worker_.joinable()) stop();
        return;
    }
    const f64 start = now_ms();
    if (worker_.joinable() && done_) {  // a sweep finished: its probes are the ones drawn from now on
        worker_.join();
        sh_.swap(result_);
        std::memcpy(origin_, sweep_origin_, sizeof origin_);
        cell_ = sweep_cell_;
        tracer_ = pending_tracer_;
        have_ = true;
        sweep_ms_ = now_ms() - sweep_start_;
    }
    if (!worker_.joinable()) start_sweep(w, registry, data_dir, eye, lights);
    apply(lights);
    main_ms_ = now_ms() - start;
}

void DynamicGi::apply(rhi::LightSetup& s) const {
    if (!have_) return;
    for (int a = 0; a < 3; ++a) {
        s.probe_origin[a] = static_cast<f32>(origin_[a]) * cell_;
        s.probe_cell[a] = cell_;
        s.probe_dims[a] = kGiDims[a];
    }
    s.probe_origin[3] = 1.0f;
    std::memcpy(s.probes, sh_.data(), sh_.size() * sizeof(f32));
    s.lightmap_info[1] = 0.0f;  // replaces Build Lighting's lightmaps too (as Lumen does): everything gets the live light
}

void DynamicGi::start_sweep(const world::World& w, const asset::AssetRegistry& registry, const char* data_dir, Vec3 eye, const rhi::LightSetup& lights) {
    constexpr u32 count = kGiDims[0] * kGiDims[1] * kGiDims[2];
    sweep_cell_ = std::max(spacing, 0.25f);
    gi_grid_origin(eye, sweep_cell_, kGiDims, sweep_origin_);
    history_.assign(static_cast<usize>(count) * 12, 0.0f);
    kept_.assign(count, 0);
    if (have_ && cell_ == sweep_cell_) gi_scroll(origin_, sweep_origin_, kGiDims, sh_.data(), history_.data(), kept_.data());
    const bool any_kept = std::find(kept_.begin(), kept_.end(), u8{1}) != kept_.end();

    // The static meshes as they are now: gathered again (from cached triangles) only when one moved or changed.
    const reflect::TypeInfo& mesh_type = reflect::type_of<world::StaticMesh>();
    u64 hash = 14695981039346656037ull;
    for (u32 i = 0; i < w.count(mesh_type); ++i) {
        const world::Entity e = w.owner(mesh_type, i);
        if (w.hidden_in_hierarchy(e)) continue;
        const auto* m = static_cast<const world::StaticMesh*>(w.at(mesh_type, i));
        const Mat4 world_matrix = w.world_matrix(e);
        hash = mix(hash, &world_matrix, sizeof world_matrix);
        hash = mix(hash, m->mesh, std::strlen(m->mesh));
        hash = mix(hash, m->material, std::strlen(m->material));
        hash = mix(hash, &m->color, sizeof m->color);
    }
    std::shared_ptr<BakeScene> scene;
    if (!tracer_ || hash != scene_hash_) {
        scene_hash_ = hash;
        scene = std::make_shared<BakeScene>();
        Allocator& allocator = default_allocator();
        for (u32 i = 0; i < w.count(mesh_type); ++i) {
            const world::Entity e = w.owner(mesh_type, i);
            if (w.hidden_in_hierarchy(e)) continue;
            const auto* m = static_cast<const world::StaticMesh*>(w.at(mesh_type, i));
            auto [mesh, fresh] = meshes_.try_emplace(m->mesh);
            if (fresh) {  // shortcut: loaded once per run (a reimported mesh keeps its old shape here until GI is turned off and on)
                render::MeshData data;
                if (load_level_mesh(m->mesh, registry, data_dir, allocator, data)) {
                    for (u32 k = 0; k + 2 < data.index_count; k += 3) {
                        for (u32 v = 0; v < 3; ++v) {
                            const f32* p = data.vertices[data.indices[k + v]].position;
                            mesh->second.positions.push_back(Vec3{p[0], p[1], p[2]});
                        }
                    }
                    render::free_mesh(allocator, data);
                }
            }
            Vec3 albedo{m->color.r, m->color.g, m->color.b};
            if (m->material[0]) {
                auto [material, new_material] = materials_.try_emplace(m->material, Vec3{1, 1, 1});
                asset::Guid guid;
                if (new_material && asset::parse_guid(m->material, guid)) {
                    if (const asset::AssetRecord* r = registry.find(guid)) {
                        char path[300];
                        registry.source_path(*r, path, sizeof path);
                        render::Material loaded;
                        if (asset::load_data_asset(path, loaded)) material->second = Vec3{loaded.base_color.r, loaded.base_color.g, loaded.base_color.b};
                    }
                }
                albedo = Vec3{albedo.x * material->second.x, albedo.y * material->second.y, albedo.z * material->second.z};
            }
            const Mat4 world_matrix = w.world_matrix(e);
            const std::vector<Vec3>& local = mesh->second.positions;
            for (usize k = 0; k < local.size(); ++k) scene->positions.push_back(transform_point(world_matrix, local[k]));
            scene->albedo.insert(scene->albedo.end(), local.size() / 3, albedo);
        }
    }
    // The lights drawn this frame (nearest the camera, the sky as the atmosphere has it now); world_lights keeps Static
    // lights real-time while dynamic GI is on, so none of their light belongs in the probes directly.
    auto light_setup = std::make_shared<rhi::LightSetup>(lights);  // copied straight onto the heap: it is too big for the stack
    constexpr u32 static_lights = 0;

    pending_tracer_ = scene ? nullptr : tracer_;
    result_.assign(static_cast<usize>(count) * 12, 0.0f);
    done_ = false;
    sweep_start_ = now_ms();
    const u32 seed = ++sweep_count_ * 7919u;  // a new ray spiral per sweep: the blend averages them
    const u32 ray_count = static_cast<u32>(std::clamp(rays, 8, 256));
    const u32 threads = std::max(1u, std::thread::hardware_concurrency() / 2);  // half the cores: the game keeps the rest
    worker_ = std::thread([this, scene, light_setup, any_kept, seed, ray_count, threads] {
        if (!pending_tracer_) pending_tracer_ = make_probe_tracer(std::move(*scene));
        set_tracer_lights(*pending_tracer_, *light_setup, static_lights);
        const f32 origin[3] = {static_cast<f32>(sweep_origin_[0]) * sweep_cell_, static_cast<f32>(sweep_origin_[1]) * sweep_cell_,
                               static_cast<f32>(sweep_origin_[2]) * sweep_cell_};
        const f32 cell[3] = {sweep_cell_, sweep_cell_, sweep_cell_};
        // The bounce reads the last probes; the first sweep has none (relight_probe_grid lights the hits with half the sky).
        relight_probe_grid(*pending_tracer_, origin, cell, kGiDims, ray_count, seed, threads, any_kept ? history_.data() : nullptr, result_.data());
        for (u32 i = 0; i < count; ++i) {
            if (!kept_[i]) continue;
            for (usize k = static_cast<usize>(i) * 12; k < static_cast<usize>(i) * 12 + 12; ++k) result_[k] = history_[k] + (result_[k] - history_[k]) * kBlend;
        }
        done_ = true;
    });
}

} // namespace lsd::editor
