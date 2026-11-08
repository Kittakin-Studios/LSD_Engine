#include "navigation.h"

#include "lsd/world/components.h"

#include <algorithm>
#include <vector>

namespace lsd::editor {

namespace {

constexpr Vec3 kUnitLo{-0.5f, -0.5f, 0.0f}, kUnitHi{0.5f, 0.5f, 1.0f};  // volumes (see components.h)

// A mesh's own triangles under a world matrix (Collision Shape: Mesh, as physics uses them).
void add_triangles(std::vector<f32>& v, std::vector<u32>& idx, const Mat4& m, const MeshCache::Triangles& t, Vec3& world_lo, Vec3& world_hi) {
    const u32 base = static_cast<u32>(v.size() / 3);
    for (const Vec3& local : t.points) {
        const Vec3 p = transform_point(m, local);
        v.push_back(p.x);
        v.push_back(p.y);
        v.push_back(p.z);
        world_lo = vmin(world_lo, p);
        world_hi = vmax(world_hi, p);
    }
    const bool mirrored = dot(cross(transform_vector(m, {1, 0, 0}), transform_vector(m, {0, 1, 0})), transform_vector(m, {0, 0, 1})) < 0.0f;
    for (usize i = 0; i + 2 < t.indices.size(); i += 3) {
        idx.push_back(base + t.indices[i]);
        idx.push_back(base + t.indices[mirrored ? i + 2 : i + 1]);
        idx.push_back(base + t.indices[mirrored ? i + 1 : i + 2]);
    }
}

// A box (local bounds under a world matrix) as 8 corners and 12 outward triangles.
void add_box(std::vector<f32>& v, std::vector<u32>& idx, const Mat4& m, Vec3 lo, Vec3 hi, Vec3& world_lo, Vec3& world_hi) {
    const u32 base = static_cast<u32>(v.size() / 3);
    for (int c = 0; c < 8; ++c) {
        const Vec3 p = transform_point(m, {(c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z});
        v.push_back(p.x);
        v.push_back(p.y);
        v.push_back(p.z);
        world_lo = vmin(world_lo, p);
        world_hi = vmax(world_hi, p);
    }
    // Counter-clockwise from outside when the matrix keeps handedness; a mirrored object flips them.
    const bool mirrored = dot(cross(transform_vector(m, {1, 0, 0}), transform_vector(m, {0, 1, 0})), transform_vector(m, {0, 0, 1})) < 0.0f;
    static const u32 faces[12][3] = {{4, 5, 7}, {4, 7, 6}, {0, 2, 3}, {0, 3, 1}, {0, 1, 5}, {0, 5, 4},
                                     {2, 6, 7}, {2, 7, 3}, {0, 4, 6}, {0, 6, 2}, {1, 3, 7}, {1, 7, 5}};
    for (const auto& f : faces) {
        idx.push_back(base + f[0]);
        idx.push_back(base + (mirrored ? f[2] : f[1]));
        idx.push_back(base + (mirrored ? f[1] : f[2]));
    }
}

} // namespace

namespace {
bool gather_and_build(const world::World& world, MeshCache& meshes, ai::NavMesh* out, u64* input_hash, bool* reused, const std::vector<world::Entity>* skip);
}

bool build_level_navmesh(const world::World& world, MeshCache& meshes, ai::NavMesh& out, u64* input_hash, bool* reused, const std::vector<world::Entity>* skip) {
    return gather_and_build(world, meshes, &out, input_hash, reused, skip);
}

u64 navmesh_inputs_hash(const world::World& world, MeshCache& meshes, const std::vector<world::Entity>* skip) {
    u64 hash = 0;
    return gather_and_build(world, meshes, nullptr, &hash, nullptr, skip) ? hash : 0;
}

namespace {
// out null: only the inputs' hash into *input_hash.
bool gather_and_build(const world::World& world, MeshCache& meshes, ai::NavMesh* out_mesh, u64* input_hash, bool* reused, const std::vector<world::Entity>* skip) {
    if (reused) *reused = false;
    // What physics collides with: each mesh's box, or its triangles when its Collision Shape is Mesh.
    std::vector<f32> verts;
    std::vector<u32> idx;
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (u32 slot = 0; slot < world.slot_count(); ++slot) {
        const world::Entity e = world.entity_at_slot(slot);
        if (!e.valid() || world.get<world::RigidBody>(e) || world.get<world::SplineFollower>(e) || world.get<world::Mover>(e) || world.get<world::Transport>(e) ||
            world.get<world::Vehicle>(e) || (skip && std::find(skip->begin(), skip->end(), e) != skip->end()))
            continue;  // moving things are not floors
        const Mat4 m = world.world_matrix(e);
        if (const auto* mesh = world.get<world::StaticMesh>(e); mesh && mesh->collision) {
            const MeshCache::Triangles* exact = mesh->collision_shape == 1 ? meshes.triangles(mesh->mesh) : nullptr;  // arches, ramps, stairs
            if (exact) add_triangles(verts, idx, m, *exact, lo, hi);
            else if (const CachedMesh* cached = meshes.get(mesh->mesh)) add_box(verts, idx, m, cached->bounds_min, cached->bounds_max, lo, hi);
        }
        if (const auto* blocking = world.get<world::BlockingVolume>(e); blocking && blocking->enabled) add_box(verts, idx, m, kUnitLo, kUnitHi, lo, hi);
    }
    // Bounds: the Nav Mesh Bounds volumes together, else everything (a little room above).
    const reflect::TypeInfo& bounds_type = reflect::type_of<world::NavMeshBounds>();
    if (world.count(bounds_type)) {
        lo = {1e30f, 1e30f, 1e30f};
        hi = {-1e30f, -1e30f, -1e30f};
        for (u32 i = 0; i < world.count(bounds_type); ++i) {
            const Mat4 m = world.world_matrix(world.owner(bounds_type, i));
            for (int c = 0; c < 8; ++c) {
                const Vec3 p = transform_point(m, {(c & 1) ? kUnitHi.x : kUnitLo.x, (c & 2) ? kUnitHi.y : kUnitLo.y, (c & 4) ? kUnitHi.z : kUnitLo.z});
                lo = vmin(lo, p);
                hi = vmax(hi, p);
            }
        }
    } else {
        hi.z += 3.0f;
    }
    if (idx.empty() || lo.x > hi.x) {
        if (out_mesh) out_mesh->clear();
        return false;
    }
    const world::NavSettings& s = world.settings().navigation;
    ai::NavSettings settings;
    settings.agent_radius = s.agent_radius;
    settings.agent_height = s.agent_height;
    settings.max_climb = s.max_step_height;
    settings.max_slope = s.max_slope;
    settings.cell_size = s.cell_size;
    // Nav Modifier Volumes cut their footprint out (the unit cube under the transform; tilt is ignored).
    std::vector<ai::NavCutout> cutouts;
    const reflect::TypeInfo& modifier_type = reflect::type_of<world::NavModifierVolume>();
    for (u32 i = 0; i < world.count(modifier_type); ++i) {
        if (!static_cast<const world::NavModifierVolume*>(world.at(modifier_type, i))->enabled) continue;
        const Mat4 m = world.world_matrix(world.owner(modifier_type, i));
        ai::NavCutout cut;
        const Vec3 footprint[4] = {{kUnitLo.x, kUnitLo.y, 0}, {kUnitHi.x, kUnitLo.y, 0}, {kUnitHi.x, kUnitHi.y, 0}, {kUnitLo.x, kUnitHi.y, 0}};
        for (int c = 0; c < 4; ++c) cut.corners[c] = transform_point(m, footprint[c]);
        cut.z_min = transform_point(m, {0, 0, kUnitLo.z}).z;
        cut.z_max = transform_point(m, {0, 0, kUnitHi.z}).z;
        if (cut.z_min > cut.z_max) std::swap(cut.z_min, cut.z_max);
        cutouts.push_back(cut);
    }
    // Nav Links: from the object to its End (in the object's space).
    std::vector<ai::NavLink> links;
    const reflect::TypeInfo& link_type = reflect::type_of<world::NavLink>();
    for (u32 i = 0; i < world.count(link_type); ++i) {
        const auto* l = static_cast<const world::NavLink*>(world.at(link_type, i));
        if (!l->enabled) continue;
        const Mat4 m = world.world_matrix(world.owner(link_type, i));
        ai::NavLink link;
        link.start = transform_point(m, {0, 0, 0});
        link.end = transform_point(m, l->end);
        link.radius = l->snap_radius;
        link.both_ways = l->both_ways;
        links.push_back(link);
    }
    // Unchanged since the last build (FNV-1a over every input): keep the mesh already built.
    u64 hash = 1469598103934665603ull;
    auto mix = [&hash](const void* p, usize n) {
        for (usize i = 0; i < n; ++i) hash = (hash ^ static_cast<const u8*>(p)[i]) * 1099511628211ull;
    };
    mix(verts.data(), verts.size() * sizeof(f32));
    mix(idx.data(), idx.size() * sizeof(u32));
    mix(&lo, sizeof lo), mix(&hi, sizeof hi), mix(&settings, sizeof settings);
    mix(cutouts.data(), cutouts.size() * sizeof(ai::NavCutout));
    for (const ai::NavLink& l : links) mix(&l.start, sizeof l.start), mix(&l.end, sizeof l.end), mix(&l.radius, sizeof l.radius), mix(&l.both_ways, sizeof l.both_ways);
    if (!out_mesh) {
        *input_hash = hash;
        return true;
    }
    ai::NavMesh& out = *out_mesh;
    if (input_hash && *input_hash == hash && out.valid()) {
        if (reused) *reused = true;
        return true;
    }
    const bool built = out.build(verts.data(), static_cast<u32>(verts.size() / 3), idx.data(), static_cast<u32>(idx.size() / 3), lo, hi, settings, cutouts.data(),
                                 static_cast<u32>(cutouts.size()), links.data(), static_cast<u32>(links.size()));
    if (input_hash) *input_hash = built ? hash : 0;
    return built;
}
} // namespace

} // namespace lsd::editor
