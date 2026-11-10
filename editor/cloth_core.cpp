#include "cloth_core.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace lsd::editor {

void subdivide_cloth(std::vector<rhi::MeshVertex>& vertices, std::vector<u32>& indices, u32 levels) {
    for (u32 level = 0; level < levels; ++level) {
        std::unordered_map<u64, u32> midpoints;  // shortcut: editor-time STL map per edge, fine for meshes of a few thousand triangles
        auto midpoint = [&](u32 a, u32 b) {
            const u64 key = a < b ? (static_cast<u64>(a) << 32) | b : (static_cast<u64>(b) << 32) | a;
            if (const auto it = midpoints.find(key); it != midpoints.end()) return it->second;
            rhi::MeshVertex m = vertices[a];
            const rhi::MeshVertex& o = vertices[b];
            for (int k = 0; k < 3; ++k) {
                m.position[k] = (m.position[k] + o.position[k]) * 0.5f;
                m.normal[k] = (m.normal[k] + o.normal[k]) * 0.5f;
            }
            for (int k = 0; k < 2; ++k) {
                m.uv[k] = (m.uv[k] + o.uv[k]) * 0.5f;
                m.lightmap_uv[k] = 0.0f;
            }
            vertices.push_back(m);
            const u32 index = static_cast<u32>(vertices.size() - 1);
            midpoints.emplace(key, index);
            return index;
        };
        std::vector<u32> out;
        out.reserve(indices.size() * 4);
        for (usize i = 0; i + 2 < indices.size(); i += 3) {
            const u32 a = indices[i], b = indices[i + 1], c = indices[i + 2];
            const u32 ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
            const u32 four[12] = {a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca};  // same winding as the original
            out.insert(out.end(), four, four + 12);
        }
        indices.swap(out);
    }
}

void weld_cloth(ClothMesh& mesh, f32 tolerance) {
    const usize n = mesh.vertices.size();
    mesh.particle_of.assign(n, 0);
    mesh.particles.clear();
    // Sorted along x, each vertex only compares with those within tolerance of it there.
    std::vector<u32> order(n);
    for (usize i = 0; i < n; ++i) order[i] = static_cast<u32>(i);
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return mesh.vertices[a].position[0] < mesh.vertices[b].position[0]; });
    std::vector<u8> done(n, 0);
    const f32 t2 = tolerance * tolerance;
    for (usize i = 0; i < n; ++i) {
        const u32 a = order[i];
        if (done[a]) continue;
        const f32* p = mesh.vertices[a].position;
        const u32 particle = static_cast<u32>(mesh.particles.size());
        mesh.particles.push_back({p[0], p[1], p[2]});
        for (usize j = i; j < n && mesh.vertices[order[j]].position[0] - p[0] <= tolerance; ++j) {
            const u32 b = order[j];
            const f32* q = mesh.vertices[b].position;
            const f32 dx = q[0] - p[0], dy = q[1] - p[1], dz = q[2] - p[2];
            if (done[b] || dx * dx + dy * dy + dz * dz > t2) continue;
            done[b] = 1;
            mesh.particle_of[b] = particle;
        }
    }
    mesh.particle_indices.resize(mesh.indices.size());
    for (usize i = 0; i < mesh.indices.size(); ++i) mesh.particle_indices[i] = mesh.particle_of[mesh.indices[i]];
    mesh.pinned.assign(mesh.particles.size(), 0);
}

u32 pin_cloth(ClothMesh& mesh, u32 side, f32 amount) {
    if (side > 5 || mesh.particles.empty()) return 0;
    const u32 axis = side < 2 ? 2 : side < 4 ? 0 : 1;
    const bool high = side % 2 == 0;
    auto value = [&](const Vec3& p) { return axis == 0 ? p.x : axis == 1 ? p.y : p.z; };
    f32 lo = value(mesh.particles[0]), hi = lo;
    for (const Vec3& p : mesh.particles) {
        lo = std::min(lo, value(p));
        hi = std::max(hi, value(p));
    }
    const f32 reach = (hi - lo) * std::clamp(amount, 0.0f, 1.0f) + 1e-4f;  // the very edge counts even at 0
    u32 count = 0;
    for (usize i = 0; i < mesh.particles.size(); ++i) {
        const f32 v = value(mesh.particles[i]);
        mesh.pinned[i] = high ? v >= hi - reach : v <= lo + reach;
        count += mesh.pinned[i];
    }
    return count;
}

void update_cloth_vertices(ClothMesh& mesh, const Vec3* positions) {
    std::vector<Vec3> normals(mesh.particles.size(), Vec3{});
    for (usize i = 0; i + 2 < mesh.particle_indices.size(); i += 3) {
        const u32 a = mesh.particle_indices[i], b = mesh.particle_indices[i + 1], c = mesh.particle_indices[i + 2];
        const Vec3 n = cross(positions[b] - positions[a], positions[c] - positions[a]);  // area-weighted
        normals[a] = normals[a] + n;
        normals[b] = normals[b] + n;
        normals[c] = normals[c] + n;
    }
    for (usize v = 0; v < mesh.vertices.size(); ++v) {
        const u32 p = mesh.particle_of[v];
        const f32 len = length(normals[p]);
        const Vec3 n = len > 1e-12f ? normals[p] * (1.0f / len) : Vec3{0, 0, 1};
        rhi::MeshVertex& out = mesh.vertices[v];
        out.position[0] = positions[p].x;
        out.position[1] = positions[p].y;
        out.position[2] = positions[p].z;
        out.normal[0] = n.x;
        out.normal[1] = n.y;
        out.normal[2] = n.z;
    }
}

} // namespace lsd::editor
