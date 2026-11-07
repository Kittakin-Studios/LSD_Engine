#include "mesh_edit.h"

#include "lsd/foundation/allocator.h"

#include <cmath>
#include <cstdio>

namespace lsd::editor {

namespace {

u32 corner(const world::EditFace& f, u32 i) {
    const u32 v[4] = {f.a, f.b, f.c, f.d};
    return v[i];
}

world::EditFace make_face(const u32* v, u32 n) {
    world::EditFace f;
    f.a = v[0];
    f.b = v[1];
    f.c = v[2];
    f.d = n > 3 ? v[3] : kNoPoint;
    return f;
}

bool valid(const world::EditableMesh& m, u32 face) {
    if (face >= m.faces.count) return false;
    const world::EditFace& f = m.faces[face];
    for (u32 i = 0; i < face_size(f); ++i) {
        if (corner(f, i) >= m.points.count) return false;
    }
    return true;
}

u32 add_point(world::EditableMesh& m, Vec3 p) {
    world::EditPoint e;
    e.p = p;
    m.points.push_back(e);
    return m.points.count - 1;
}

} // namespace

u32 face_size(const world::EditFace& f) { return f.d == kNoPoint ? 3u : 4u; }

Vec3 face_point(const world::EditableMesh& m, const world::EditFace& f, u32 i) { return m.points[corner(f, i)].p; }

Vec3 face_normal(const world::EditableMesh& m, u32 face) {
    if (!valid(m, face)) return {0, 0, 1};
    const world::EditFace& f = m.faces[face];
    Vec3 n{};  // Newell's method: right for any flat (or nearly flat) polygon
    const u32 count = face_size(f);
    for (u32 i = 0; i < count; ++i) {
        const Vec3 a = face_point(m, f, i), b = face_point(m, f, (i + 1) % count);
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    const f32 l = length(n);
    return l > 1e-12f ? n * (1.0f / l) : Vec3{0, 0, 1};
}

Vec3 face_centre(const world::EditableMesh& m, u32 face) {
    if (!valid(m, face)) return {};
    const world::EditFace& f = m.faces[face];
    Vec3 c{};
    for (u32 i = 0; i < face_size(f); ++i) c = c + face_point(m, f, i);
    return c * (1.0f / static_cast<f32>(face_size(f)));
}

void make_box(world::EditableMesh& m) {
    m.points.count = 0;
    m.faces.count = 0;
    for (u32 i = 0; i < 8; ++i) add_point(m, {static_cast<f32>(i & 1) - 0.5f, static_cast<f32>((i >> 1) & 1) - 0.5f, static_cast<f32>((i >> 2) & 1)});
    const u32 quads[6][4] = {{4, 5, 7, 6}, {0, 2, 3, 1}, {1, 3, 7, 5}, {0, 4, 6, 2}, {2, 6, 7, 3}, {0, 1, 5, 4}};  // top, bottom, +X, -X, +Y, -Y
    for (const auto& q : quads) m.faces.push_back(make_face(q, 4));
}

bool extrude(world::EditableMesh& m, u32 face, f32 distance) {
    if (!valid(m, face)) return false;
    const world::EditFace f = m.faces[face];
    const u32 n = face_size(f);
    if (m.points.count + n > m.points.capacity() || m.faces.count + n > m.faces.capacity()) return false;
    const Vec3 up = face_normal(m, face) * distance;
    u32 moved[4];
    for (u32 i = 0; i < n; ++i) moved[i] = add_point(m, face_point(m, f, i) + up);
    for (u32 i = 0; i < n; ++i) {  // a wall along each edge, facing out
        const u32 wall[4] = {corner(f, i), corner(f, (i + 1) % n), moved[(i + 1) % n], moved[i]};
        m.faces.push_back(make_face(wall, 4));
    }
    m.faces[face] = make_face(moved, n);
    return true;
}

bool inset(world::EditableMesh& m, u32 face, f32 fraction) {
    if (!valid(m, face) || fraction <= 0.0f || fraction >= 1.0f) return false;
    const world::EditFace f = m.faces[face];
    const u32 n = face_size(f);
    if (m.points.count + n > m.points.capacity() || m.faces.count + n > m.faces.capacity()) return false;
    const Vec3 c = face_centre(m, face);
    u32 inner[4];
    for (u32 i = 0; i < n; ++i) {
        const Vec3 p = face_point(m, f, i);
        inner[i] = add_point(m, p + (c - p) * fraction);
    }
    for (u32 i = 0; i < n; ++i) {  // the ring between the outline and the smaller face
        const u32 ring[4] = {corner(f, i), corner(f, (i + 1) % n), inner[(i + 1) % n], inner[i]};
        m.faces.push_back(make_face(ring, 4));
    }
    m.faces[face] = make_face(inner, n);
    return true;
}

bool bevel(world::EditableMesh& m, u32 face, f32 fraction, f32 distance) {
    const u32 points = m.points.count, faces = m.faces.count;
    const world::EditFace before = face < m.faces.count ? m.faces[face] : world::EditFace{};
    if (!inset(m, face, fraction)) return false;
    if (!extrude(m, face, distance)) {  // all or nothing
        m.points.count = points;
        m.faces.count = faces;
        m.faces[face] = before;
        return false;
    }
    return true;
}

bool push_pull(world::EditableMesh& m, u32 face, f32 distance) {
    if (!valid(m, face)) return false;
    const world::EditFace& f = m.faces[face];
    const Vec3 d = face_normal(m, face) * distance;
    for (u32 i = 0; i < face_size(f); ++i) m.points[corner(f, i)].p = m.points[corner(f, i)].p + d;  // shared points move the neighbours too
    return true;
}

bool delete_face(world::EditableMesh& m, u32 face) {
    if (face >= m.faces.count) return false;
    for (u32 k = face; k + 1 < m.faces.count; ++k) m.faces[k] = m.faces[k + 1];
    --m.faces.count;
    return true;
}

bool flip_face(world::EditableMesh& m, u32 face) {
    if (face >= m.faces.count) return false;
    world::EditFace& f = m.faces[face];
    if (face_size(f) == 4) {
        const u32 t = f.b;
        f.b = f.d;
        f.d = t;
    } else {
        const u32 t = f.b;
        f.b = f.c;
        f.c = t;
    }
    return true;
}

i32 pick_face(const world::EditableMesh& m, Vec3 origin, Vec3 dir, f32& t) {
    i32 best = -1;
    t = 1e30f;
    for (u32 face = 0; face < m.faces.count; ++face) {
        if (!valid(m, face)) continue;
        const world::EditFace& f = m.faces[face];
        for (u32 tri = 0; tri + 2 < face_size(f); ++tri) {  // a fan: (0, 1, 2), (0, 2, 3)
            const Vec3 a = face_point(m, f, 0), b = face_point(m, f, tri + 1), c = face_point(m, f, tri + 2);
            const Vec3 e1 = b - a, e2 = c - a, p = cross(dir, e2);  // Moller-Trumbore, both sides
            const f32 det = dot(e1, p);
            if (std::fabs(det) < 1e-9f) continue;
            const f32 inv = 1.0f / det;
            const Vec3 s = origin - a;
            const f32 u = dot(s, p) * inv;
            if (u < 0.0f || u > 1.0f) continue;
            const Vec3 q = cross(s, e1);
            const f32 v = dot(dir, q) * inv;
            if (v < 0.0f || u + v > 1.0f) continue;
            const f32 hit = dot(e2, q) * inv;
            if (hit > 1e-5f && hit < t) {
                t = hit;
                best = static_cast<i32>(face);
            }
        }
    }
    return best;
}

void build_edit_mesh(const world::EditableMesh& m, render::MeshData& out) {
    u32 corners = 0;
    for (u32 face = 0; face < m.faces.count; ++face) {
        if (valid(m, face)) corners += (face_size(m.faces[face]) - 2) * 3;
    }
    out = render::allocate_mesh(default_allocator(), corners, corners);
    const f32 scale = m.uv_scale > 1e-4f ? m.uv_scale : 1.0f;
    u32 k = 0;
    for (u32 face = 0; face < m.faces.count; ++face) {
        if (!valid(m, face)) continue;
        const world::EditFace& f = m.faces[face];
        const Vec3 n = face_normal(m, face);
        const f32 ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
        for (u32 tri = 0; tri + 2 < face_size(f); ++tri) {
            for (const u32 i : {0u, tri + 1, tri + 2}) {
                const Vec3 p = face_point(m, f, i);
                // Box UV: projected along the face's main axis.
                const f32 u = az >= ax && az >= ay ? p.x : ax >= ay ? p.y : p.x, v = az >= ax && az >= ay ? p.y : p.z;
                rhi::MeshVertex& vx = out.vertices[k];
                vx = {{p.x, p.y, p.z}, {n.x, n.y, n.z}, {u * scale, -v * scale}};
                out.indices[k] = k;
                ++k;
            }
        }
    }
}

std::string to_obj(const world::EditableMesh& m, const char* name) {
    std::string s = std::string("# LSD_Engine Editable Mesh\no ") + name + "\n";
    char line[128];
    for (const world::EditPoint& p : m.points) {
        // OBJ files are Y-up (Blender, Maya): the engine's Z-up point (x, y, z) is written as (x, z, -y).
        std::snprintf(line, sizeof line, "v %.6f %.6f %.6f\n", static_cast<f64>(p.p.x), static_cast<f64>(p.p.z), static_cast<f64>(-p.p.y));
        s += line;
    }
    for (u32 face = 0; face < m.faces.count; ++face) {
        if (!valid(m, face)) continue;
        const world::EditFace& f = m.faces[face];
        s += "f";
        for (u32 i = 0; i < face_size(f); ++i) {
            std::snprintf(line, sizeof line, " %u", corner(f, i) + 1);
            s += line;
        }
        s += "\n";
    }
    return s;
}

} // namespace lsd::editor
