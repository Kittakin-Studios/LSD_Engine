#include "lsd/render/fracture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace lsd::render {

namespace {

Vec3 pos(const rhi::MeshVertex& v) { return {v.position[0], v.position[1], v.position[2]}; }

// Exact float bits (with -0 as 0): vertices from one source share bit-identical positions along their seams.
u32 bits(f32 f) {
    if (f == 0.0f) return 0;
    u32 b;
    std::memcpy(&b, &f, sizeof b);
    return b;
}
struct PosKey {
    u32 x, y, z;
    bool operator==(const PosKey& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct PosHash {
    usize operator()(const PosKey& k) const { return static_cast<usize>(k.x * 73856093u ^ k.y * 19349663u ^ k.z * 83492791u); }
};
PosKey key_of(Vec3 p) { return {bits(p.x), bits(p.y), bits(p.z)}; }

// One id per distinct position (the topology the cut follows, across UV and normal seams).
std::vector<u32> position_ids(const CutMesh& m) {
    std::vector<u32> ids(m.vertices.size());
    std::unordered_map<PosKey, u32, PosHash> seen;
    seen.reserve(m.vertices.size());
    for (usize i = 0; i < m.vertices.size(); ++i) ids[i] = seen.try_emplace(key_of(pos(m.vertices[i])), static_cast<u32>(seen.size())).first->second;
    return ids;
}

rhi::MeshVertex lerp(const rhi::MeshVertex& a, const rhi::MeshVertex& b, f32 t) {
    rhi::MeshVertex v{};
    for (int k = 0; k < 3; ++k) v.position[k] = a.position[k] + (b.position[k] - a.position[k]) * t;
    Vec3 n{a.normal[0] + (b.normal[0] - a.normal[0]) * t, a.normal[1] + (b.normal[1] - a.normal[1]) * t, a.normal[2] + (b.normal[2] - a.normal[2]) * t};
    const f32 len = length(n);
    n = len > 1e-12f ? n * (1.0f / len) : Vec3{a.normal[0], a.normal[1], a.normal[2]};
    v.normal[0] = n.x, v.normal[1] = n.y, v.normal[2] = n.z;
    for (int k = 0; k < 2; ++k) {
        v.uv[k] = a.uv[k] + (b.uv[k] - a.uv[k]) * t;
        v.lightmap_uv[k] = a.lightmap_uv[k] + (b.lightmap_uv[k] - a.lightmap_uv[k]) * t;
    }
    return v;
}

// ---- Caps: the cut outline as polygons on the plane, triangulated ----

struct P2 {
    f32 x, y;
};
f32 cross2(P2 o, P2 a, P2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }
f64 signed_area(const std::vector<P2>& pts, const std::vector<u32>& loop) {
    f64 a = 0.0;
    for (usize i = 0; i < loop.size(); ++i) {
        const P2 p = pts[loop[i]], q = pts[loop[(i + 1) % loop.size()]];
        a += static_cast<f64>(p.x) * q.y - static_cast<f64>(q.x) * p.y;
    }
    return 0.5 * a;
}
bool inside_loop(P2 q, const std::vector<P2>& pts, const std::vector<u32>& loop) {  // even-odd
    bool in = false;
    for (usize i = 0, j = loop.size() - 1; i < loop.size(); j = i++) {
        const P2 a = pts[loop[i]], b = pts[loop[j]];
        if ((a.y > q.y) != (b.y > q.y) && q.x < (b.x - a.x) * (q.y - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    return in;
}
bool same(P2 a, P2 b) { return a.x == b.x && a.y == b.y; }
// Do segments ab and cd cross at a point inside both (touching at an end does not count)?
bool crosses(P2 a, P2 b, P2 c, P2 d) {
    if (same(a, c) || same(a, d) || same(b, c) || same(b, d)) return false;
    const f32 d1 = cross2(a, b, c), d2 = cross2(a, b, d), d3 = cross2(c, d, a), d4 = cross2(c, d, b);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0)) && d1 != 0 && d2 != 0 && d3 != 0 && d4 != 0;
}
bool segment_blocked(P2 a, P2 b, const std::vector<P2>& pts, const std::vector<u32>& loop) {
    for (usize i = 0; i < loop.size(); ++i) {
        if (crosses(a, b, pts[loop[i]], pts[loop[(i + 1) % loop.size()]])) return true;
    }
    return false;
}
// Drops repeated points (several cut edges meeting in a mesh vertex on the plane). Points on a straight line
// stay: the cap then shares every edge with the surface around it (watertight, no T-junctions).
void simplify(const std::vector<P2>& pts, std::vector<u32>& loop, f32 tolerance) {
    for (usize i = 0; i < loop.size() && loop.size() > 2;) {
        const P2 a = pts[loop[(i + loop.size() - 1) % loop.size()]], p = pts[loop[i]];
        if (std::fabs(p.x - a.x) + std::fabs(p.y - a.y) <= tolerance) loop.erase(loop.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
}
// Ear clipping of one counter-clockwise polygon (holes already bridged in); triangles into `out`.
void ear_clip(const std::vector<P2>& pts, std::vector<u32> poly, std::vector<u32>& out) {
    while (poly.size() > 3) {
        const usize n = poly.size();
        bool clipped = false;
        for (usize i = 0; i < n && !clipped; ++i) {
            const u32 ia = poly[(i + n - 1) % n], ib = poly[i], ic = poly[(i + 1) % n];
            const P2 a = pts[ia], b = pts[ib], c = pts[ic];
            if (cross2(a, b, c) <= 0.0f) continue;  // reflex or flat
            bool empty = true;
            for (usize k = 0; k < n && empty; ++k) {
                const P2 q = pts[poly[k]];
                if (same(q, a) || same(q, b) || same(q, c)) continue;
                empty = !(cross2(a, b, q) >= 0.0f && cross2(b, c, q) >= 0.0f && cross2(c, a, q) >= 0.0f);
            }
            if (!empty) continue;
            out.insert(out.end(), {ia, ib, ic});
            poly.erase(poly.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
        }
        if (clipped) continue;
        // No ear (rounding on a degenerate outline): cut off the flattest corner to keep going.
        usize best = 0;
        f32 flattest = 1e30f;
        for (usize i = 0; i < n; ++i) {
            const f32 c = std::fabs(cross2(pts[poly[(i + n - 1) % n]], pts[poly[i]], pts[poly[(i + 1) % n]]));
            if (c < flattest) flattest = c, best = i;
        }
        if (cross2(pts[poly[(best + n - 1) % n]], pts[poly[best]], pts[poly[(best + 1) % n]]) > 0.0f) out.insert(out.end(), {poly[(best + n - 1) % n], poly[best], poly[(best + 1) % n]});
        poly.erase(poly.begin() + static_cast<std::ptrdiff_t>(best));
    }
    if (poly.size() == 3 && cross2(pts[poly[0]], pts[poly[1]], pts[poly[2]]) > 0.0f) out.insert(out.end(), {poly[0], poly[1], poly[2]});
}
// Joins each hole into its outer polygon through a bridge to a vertex it can see, then clips ears.
void triangulate(const std::vector<P2>& pts, std::vector<u32> outer, std::vector<std::vector<u32>> holes, std::vector<u32>& out) {
    auto rightmost = [&](const std::vector<u32>& h) {
        usize best = 0;
        for (usize i = 1; i < h.size(); ++i) best = pts[h[i]].x > pts[h[best]].x ? i : best;
        return best;
    };
    std::sort(holes.begin(), holes.end(), [&](const std::vector<u32>& a, const std::vector<u32>& b) { return pts[a[rightmost(a)]].x > pts[b[rightmost(b)]].x; });
    for (usize h = 0; h < holes.size(); ++h) {
        const std::vector<u32>& hole = holes[h];
        const usize m = rightmost(hole);
        const P2 pm = pts[hole[m]];
        std::vector<usize> order(outer.size());
        for (usize i = 0; i < order.size(); ++i) order[i] = i;
        auto dist2 = [&](usize i) { const P2 p = pts[outer[i]]; return (p.x - pm.x) * (p.x - pm.x) + (p.y - pm.y) * (p.y - pm.y); };
        std::sort(order.begin(), order.end(), [&](usize a, usize b) { return dist2(a) < dist2(b); });
        usize bridge = order.empty() ? 0 : order[0];
        for (const usize i : order) {
            const P2 p = pts[outer[i]];
            bool blocked = segment_blocked(pm, p, pts, outer);
            for (usize k = h; k < holes.size() && !blocked; ++k) blocked = segment_blocked(pm, p, pts, holes[k]);
            if (!blocked) {
                bridge = i;
                break;
            }
        }
        std::vector<u32> merged(outer.begin(), outer.begin() + static_cast<std::ptrdiff_t>(bridge) + 1);
        for (usize k = 0; k <= hole.size(); ++k) merged.push_back(hole[(m + k) % hole.size()]);
        merged.insert(merged.end(), outer.begin() + static_cast<std::ptrdiff_t>(bridge), outer.end());
        outer.swap(merged);
    }
    ear_clip(pts, outer, out);
}

// The cut outline: directed segments between cut points (a cap point each), chained into loops.
void add_caps(const std::vector<Vec3>& points, const std::vector<std::pair<u32, u32>>& segments, Vec3 n, f32 eps, f32 uv_scale, CutMesh& out) {
    if (segments.empty()) return;
    std::vector<std::vector<u32>> starts(points.size());
    for (u32 s = 0; s < segments.size(); ++s) starts[segments[s].first].push_back(s);
    std::vector<bool> used(segments.size(), false);
    std::vector<std::vector<u32>> closed, open;
    for (u32 s = 0; s < segments.size(); ++s) {
        if (used[s]) continue;
        std::vector<u32> chain{segments[s].first};
        u32 at = s;
        bool loop = false;
        while (true) {
            used[at] = true;
            const u32 next = segments[at].second;
            if (next == chain.front()) {
                loop = true;
                break;
            }
            chain.push_back(next);
            u32 follow = ~0u;
            for (const u32 cand : starts[next]) {
                if (!used[cand]) {
                    follow = cand;
                    break;
                }
            }
            if (follow == ~0u) break;
            at = follow;
        }
        (loop ? closed : open).push_back(std::move(chain));
    }
    // A source with holes leaves open outlines: join each end to the nearest start (its own closes it).
    while (!open.empty()) {
        std::vector<u32> chain = std::move(open.back());
        open.pop_back();
        while (true) {
            const Vec3 end = points[chain.back()];
            f32 best = dot(points[chain.front()] - end, points[chain.front()] - end);
            usize pick = open.size();  // = close it
            for (usize k = 0; k < open.size(); ++k) {
                const Vec3 d = points[open[k].front()] - end;
                if (dot(d, d) < best) best = dot(d, d), pick = k;
            }
            if (pick == open.size()) break;
            chain.insert(chain.end(), open[pick].begin(), open[pick].end());
            open.erase(open.begin() + static_cast<std::ptrdiff_t>(pick));
        }
        closed.push_back(std::move(chain));
    }
    // 2D on the plane: (u, v, n) right-handed, so counter-clockwise in 2D faces +n.
    const Vec3 helper = std::fabs(n.z) < 0.9f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    const Vec3 u = normalize(cross(helper, n)), v = cross(n, u);
    std::vector<P2> pts(points.size());
    for (usize i = 0; i < points.size(); ++i) pts[i] = {dot(points[i], u), dot(points[i], v)};
    std::vector<std::vector<u32>> outers, holes;
    std::vector<f64> outer_area;
    for (std::vector<u32>& loop : closed) {
        simplify(pts, loop, 0.0f);
        if (loop.size() < 3) continue;
        const f64 a = signed_area(pts, loop);
        if (std::fabs(a) <= static_cast<f64>(eps) * eps) continue;
        if (a > 0) outers.push_back(std::move(loop)), outer_area.push_back(a);
        else holes.push_back(std::move(loop));
    }
    if (outers.empty() && !holes.empty()) {  // the source faces inward: its outline runs the other way
        for (std::vector<u32>& h : holes) {
            std::reverse(h.begin(), h.end());
            outer_area.push_back(signed_area(pts, h));
            outers.push_back(std::move(h));
        }
        holes.clear();
    }
    // Each hole in the smallest outer polygon around it.
    std::vector<std::vector<std::vector<u32>>> holes_of(outers.size());
    for (std::vector<u32>& h : holes) {
        usize best = outers.size();
        for (usize o = 0; o < outers.size(); ++o) {
            if (inside_loop(pts[h[0]], pts, outers[o]) && (best == outers.size() || outer_area[o] < outer_area[best])) best = o;
        }
        if (best < outers.size()) holes_of[best].push_back(std::move(h));
    }
    std::vector<u32> tris;
    for (usize o = 0; o < outers.size(); ++o) triangulate(pts, outers[o], holes_of[o], tris);
    if (tris.empty()) return;
    std::vector<u32> made(points.size(), ~0u);  // cap vertex per point
    for (const u32 p : tris) {
        if (made[p] == ~0u) {
            rhi::MeshVertex vertex{};
            vertex.position[0] = points[p].x, vertex.position[1] = points[p].y, vertex.position[2] = points[p].z;
            vertex.normal[0] = n.x, vertex.normal[1] = n.y, vertex.normal[2] = n.z;
            vertex.uv[0] = pts[p].x * uv_scale;
            vertex.uv[1] = -pts[p].y * uv_scale;  // V down, as textures are
            made[p] = static_cast<u32>(out.vertices.size());
            out.vertices.push_back(vertex);
        }
        out.indices.push_back(made[p]);
    }
    out.sections.resize(out.indices.size() / 3, kInsideSection);
}

// Keeps the part of `in` below the plane (dot(n, p) - offset < 0; within eps of it counts as above) and caps it.
// 0: all of it is below (nothing to do), 1: cut into `out`, 2: none of it is below.
int clip(const CutMesh& in, Vec3 n, f32 offset, f32 eps, f32 uv_scale, CutMesh& out) {
    const usize vc = in.vertices.size();
    std::vector<f32> d(vc);
    bool above = false, below = false;
    for (usize i = 0; i < vc; ++i) {
        f32 f = dot(n, pos(in.vertices[i])) - offset;
        if (std::fabs(f) < eps) f = 0.0f;
        d[i] = f;
        above = above || f > 0.0f;  // only points past the plane: a face lying in it is not cut off
        below = below || f < 0.0f;
    }
    if (!above) return 0;
    if (!below) return 2;
    const std::vector<u32> pid = position_ids(in);
    out.vertices.clear();
    out.indices.clear();
    out.sections.clear();
    std::vector<u32> kept(vc, ~0u);
    auto keep = [&](u32 i) {
        if (kept[i] == ~0u) {
            kept[i] = static_cast<u32>(out.vertices.size());
            out.vertices.push_back(in.vertices[i]);
        }
        return kept[i];
    };
    std::unordered_map<u64, u32> cut_point;  // per crossed edge (its two position ids): its cap point
    std::vector<Vec3> points;
    std::vector<std::pair<u32, u32>> segments;
    // The crossing on edge a-b, worked out from the lower position id so both triangles on the edge agree to the bit.
    auto crossing = [&](u32 a, u32 b, u32& cap) {
        const bool flip = pid[a] > pid[b];
        const u32 lo = flip ? b : a, hi = flip ? a : b;
        const f32 t = d[lo] / (d[lo] - d[hi]);
        const rhi::MeshVertex v = lerp(in.vertices[lo], in.vertices[hi], t);
        const u64 key = static_cast<u64>(pid[lo]) << 32 | pid[hi];
        const auto [it, added] = cut_point.try_emplace(key, static_cast<u32>(points.size()));
        if (added) points.push_back(pos(v));
        cap = it->second;
        return v;
    };
    for (usize t = 0; t < in.indices.size() / 3; ++t) {
        const u32 c[3] = {in.indices[t * 3], in.indices[t * 3 + 1], in.indices[t * 3 + 2]};
        const bool b[3] = {d[c[0]] < 0.0f, d[c[1]] < 0.0f, d[c[2]] < 0.0f};
        const int count = b[0] + b[1] + b[2];
        if (count == 0) continue;
        const u16 section = in.sections[t];
        if (count == 3) {
            out.indices.insert(out.indices.end(), {keep(c[0]), keep(c[1]), keep(c[2])});
            out.sections.push_back(section);
            continue;
        }
        u32 poly[4];
        int corners = 0;
        u32 enter = ~0u, leave = ~0u;
        for (int k = 0; k < 3; ++k) {
            const u32 here = c[k], next = c[(k + 1) % 3];
            if (b[k]) poly[corners++] = keep(here);
            if (b[k] == b[(k + 1) % 3]) continue;
            u32 cap;
            const rhi::MeshVertex v = crossing(here, next, cap);
            poly[corners++] = static_cast<u32>(out.vertices.size());
            out.vertices.push_back(v);
            (b[k] ? leave : enter) = cap;
        }
        for (int k = 1; k + 1 < corners; ++k) {
            const Vec3 p0 = pos(out.vertices[poly[0]]), p1 = pos(out.vertices[poly[k]]), p2 = pos(out.vertices[poly[k + 1]]);
            if (dot(cross(p1 - p0, p2 - p0), cross(p1 - p0, p2 - p0)) <= 0.0f) continue;  // a corner on the plane: nothing there
            out.indices.insert(out.indices.end(), {poly[0], poly[k], poly[k + 1]});
            out.sections.push_back(section);
        }
        if (enter != leave) segments.push_back({enter, leave});  // the cap runs from where the surface came in to where it left
    }
    add_caps(points, segments, n, eps, uv_scale, out);
    return 1;
}

f32 epsilon_for(const CutMesh& m) {
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const rhi::MeshVertex& v : m.vertices) lo = vmin(lo, pos(v)), hi = vmax(hi, pos(v));
    const Vec3 size = hi - lo;
    return std::fmax(std::fmax(size.x, size.y), std::fmax(size.z, 1e-3f)) * 1e-5f;
}

// Small deterministic random numbers (every machine fractures the same way for a seed).
struct Random {
    u64 state;
    explicit Random(u32 seed) : state(0x9E3779B97F4A7C15ull ^ (static_cast<u64>(seed) * 0xBF58476D1CE4E5B9ull)) { next(); }
    u32 next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        const u32 x = static_cast<u32>(((state >> 18u) ^ state) >> 27u);
        const u32 r = static_cast<u32>(state >> 59u);
        return (x >> r) | (x << ((32u - r) & 31u));
    }
    f32 unit() { return static_cast<f32>(next() >> 8) * (1.0f / 16777216.0f); }  // 0..1
};

} // namespace

bool slice_mesh(const CutMesh& in, Vec3 normal, f32 offset, CutMesh* back, CutMesh* front, f32 uv_scale) {
    const f32 len = length(normal);
    if (len <= 0.0f || in.indices.empty()) return false;
    const Vec3 n = normal * (1.0f / len);
    offset /= len;
    const f32 eps = epsilon_for(in);
    CutMesh lower, upper;
    if (clip(in, n, offset, eps, uv_scale, lower) != 1) return false;
    clip(in, -n, -offset, eps, uv_scale, upper);
    weld_mesh(lower);
    weld_mesh(upper);
    if (back) *back = std::move(lower);
    if (front) *front = std::move(upper);
    return true;
}

void weld_mesh(CutMesh& mesh) {
    struct VKey {
        u32 b[10];
        bool operator==(const VKey& o) const { return std::memcmp(b, o.b, sizeof b) == 0; }
    };
    struct VHash {
        usize operator()(const VKey& k) const {
            u64 h = 1469598103934665603ull;
            for (const u32 x : k.b) h = (h ^ x) * 1099511628211ull;
            return static_cast<usize>(h);
        }
    };
    std::unordered_map<VKey, u32, VHash> seen;
    seen.reserve(mesh.vertices.size());
    std::vector<rhi::MeshVertex> vertices;
    std::vector<u32> remap(mesh.vertices.size());
    for (usize i = 0; i < mesh.vertices.size(); ++i) {
        const rhi::MeshVertex& v = mesh.vertices[i];
        const f32 f[10] = {v.position[0], v.position[1], v.position[2], v.normal[0], v.normal[1], v.normal[2], v.uv[0], v.uv[1], v.lightmap_uv[0], v.lightmap_uv[1]};
        VKey key;
        for (int k = 0; k < 10; ++k) key.b[k] = bits(f[k]);
        const auto [it, added] = seen.try_emplace(key, static_cast<u32>(vertices.size()));
        if (added) vertices.push_back(v);
        remap[i] = it->second;
    }
    for (u32& index : mesh.indices) index = remap[index];
    mesh.vertices.swap(vertices);
}

std::vector<CutMesh> split_islands(const CutMesh& mesh) {
    const std::vector<u32> pid = position_ids(mesh);
    u32 ids = 0;
    for (const u32 p : pid) ids = std::max(ids, p + 1);
    std::vector<u32> parent(ids);
    for (u32 i = 0; i < ids; ++i) parent[i] = i;
    auto root = [&](u32 x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    const u32 tris = mesh.triangle_count();
    for (u32 t = 0; t < tris; ++t) {
        const u32 a = root(pid[mesh.indices[t * 3]]), b = root(pid[mesh.indices[t * 3 + 1]]), c = root(pid[mesh.indices[t * 3 + 2]]);
        parent[b] = a;
        parent[root(c)] = a;
    }
    std::unordered_map<u32, u32> island_of;  // root -> output index
    std::vector<CutMesh> out;
    std::vector<std::vector<u32>> remap;
    for (u32 t = 0; t < tris; ++t) {
        const u32 r = root(pid[mesh.indices[t * 3]]);
        const auto [it, added] = island_of.try_emplace(r, static_cast<u32>(out.size()));
        if (added) {
            out.emplace_back();
            remap.emplace_back(mesh.vertices.size(), ~0u);
        }
        CutMesh& m = out[it->second];
        std::vector<u32>& map = remap[it->second];
        for (int k = 0; k < 3; ++k) {
            const u32 v = mesh.indices[t * 3 + static_cast<u32>(k)];
            if (map[v] == ~0u) {
                map[v] = static_cast<u32>(m.vertices.size());
                m.vertices.push_back(mesh.vertices[v]);
            }
            m.indices.push_back(map[v]);
        }
        m.sections.push_back(mesh.sections[t]);
    }
    return out;
}

f32 mesh_volume(const CutMesh& mesh, Vec3& centre_of_mass) {
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const rhi::MeshVertex& v : mesh.vertices) lo = vmin(lo, pos(v)), hi = vmax(hi, pos(v));
    const Vec3 o = mesh.vertices.empty() ? Vec3{} : (lo + hi) * 0.5f;
    f64 volume = 0.0, cx = 0.0, cy = 0.0, cz = 0.0;
    for (u32 t = 0; t < mesh.triangle_count(); ++t) {
        const Vec3 a = pos(mesh.vertices[mesh.indices[t * 3]]) - o, b = pos(mesh.vertices[mesh.indices[t * 3 + 1]]) - o, c = pos(mesh.vertices[mesh.indices[t * 3 + 2]]) - o;
        const f64 v = static_cast<f64>(dot(a, cross(b, c))) / 6.0;
        volume += v;
        cx += v * (a.x + b.x + c.x) / 4.0;
        cy += v * (a.y + b.y + c.y) / 4.0;
        cz += v * (a.z + b.z + c.z) / 4.0;
    }
    const Vec3 size = hi - lo;
    if (std::fabs(volume) <= 1e-9 * (static_cast<f64>(size.x) * size.y * size.z + 1e-12)) {
        centre_of_mass = o;
        return 0.0f;
    }
    centre_of_mass = o + Vec3{static_cast<f32>(cx / volume), static_cast<f32>(cy / volume), static_cast<f32>(cz / volume)};
    return static_cast<f32>(std::fabs(volume));
}

std::vector<Vec3> hull_points(const CutMesh& mesh) {
    constexpr u32 kDirections = 64;  // a Fibonacci sphere: even, deterministic directions
    std::vector<u32> picked;
    for (u32 k = 0; k < kDirections && !mesh.vertices.empty(); ++k) {
        const f32 z = 1.0f - 2.0f * (static_cast<f32>(k) + 0.5f) / static_cast<f32>(kDirections);
        const f32 r = std::sqrt(std::fmax(0.0f, 1.0f - z * z)), a = 2.39996323f * static_cast<f32>(k);
        const Vec3 dir{r * std::cos(a), r * std::sin(a), z};
        u32 best = 0;
        f32 reach = -1e30f;
        for (u32 i = 0; i < mesh.vertices.size(); ++i) {
            const f32 s = dot(pos(mesh.vertices[i]), dir);
            if (s > reach) reach = s, best = i;
        }
        picked.push_back(best);
    }
    // The box corners' directions too: thin slabs keep their edges.
    for (int corner = 0; corner < 8 && !mesh.vertices.empty(); ++corner) {
        const Vec3 dir{corner & 1 ? 1.0f : -1.0f, corner & 2 ? 1.0f : -1.0f, corner & 4 ? 1.0f : -1.0f};
        u32 best = 0;
        f32 reach = -1e30f;
        for (u32 i = 0; i < mesh.vertices.size(); ++i) {
            const f32 s = dot(pos(mesh.vertices[i]), dir);
            if (s > reach) reach = s, best = i;
        }
        picked.push_back(best);
    }
    std::vector<Vec3> out;
    std::unordered_map<PosKey, bool, PosHash> seen;
    for (const u32 i : picked) {
        if (seen.try_emplace(key_of(pos(mesh.vertices[i])), true).second) out.push_back(pos(mesh.vertices[i]));
    }
    return out;
}

std::vector<Vec3> voronoi_seeds(Vec3 lo, Vec3 hi, const FractureSettings& settings) {
    Random rng(settings.seed);
    std::vector<Vec3> seeds;
    const u32 count = std::max(settings.pieces, 1u);
    const u32 close_count = settings.cluster ? static_cast<u32>(std::lround(static_cast<f32>(count) * std::clamp(settings.cluster_share, 0.0f, 1.0f))) : 0u;
    const Vec3 size = hi - lo;
    for (u32 i = 0; i < count; ++i) {
        Vec3 p;
        if (i < close_count) {  // uniform in a ball around the point
            Vec3 d;
            do d = Vec3{rng.unit(), rng.unit(), rng.unit()} * 2.0f - Vec3{1, 1, 1};
            while (dot(d, d) > 1.0f);
            p = settings.cluster_point + d * std::fmax(settings.cluster_radius, 1e-3f);
            p = vmax(lo, vmin(hi, p));
        } else {
            p = lo + mul(size, Vec3{rng.unit(), rng.unit(), rng.unit()});
        }
        seeds.push_back(p);
    }
    return seeds;
}

std::vector<FracturePiece> fracture_voronoi(const CutMesh& mesh, const FractureSettings& settings, FractureStats* stats) {
    const auto started = std::chrono::steady_clock::now();
    FractureStats local;
    FractureStats& s = stats ? *stats : local;
    s = {};
    std::vector<FracturePiece> pieces;
    if (mesh.indices.empty()) return pieces;
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const rhi::MeshVertex& v : mesh.vertices) lo = vmin(lo, pos(v)), hi = vmax(hi, pos(v));
    const std::vector<Vec3> seeds = voronoi_seeds(lo, hi, settings);
    const f32 eps = epsilon_for(mesh);
    std::vector<u32> order(seeds.size());
    for (u32 i = 0; i < seeds.size(); ++i) {
        // Nearest neighbours first: they cut the most, and once the cell is smaller than half the distance
        // to the next seed no farther bisector can touch it.
        for (u32 k = 0; k < order.size(); ++k) order[k] = k;
        auto dist2 = [&](u32 k) { return dot(seeds[k] - seeds[i], seeds[k] - seeds[i]); };
        std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return dist2(a) < dist2(b); });
        CutMesh cell = mesh, next;
        f32 reach2 = 0.0f;  // squared distance from the seed to the cell's farthest point
        for (const rhi::MeshVertex& v : cell.vertices) reach2 = std::fmax(reach2, dot(pos(v) - seeds[i], pos(v) - seeds[i]));
        bool empty = false;
        for (const u32 j : order) {
            if (j == i) continue;
            const Vec3 gap = seeds[j] - seeds[i];
            const f32 gap2 = dot(gap, gap);
            if (gap2 <= 1e-12f) {  // two seeds on one spot: the later one gets nothing
                if (j < i) empty = true;
                if (empty) break;
                continue;
            }
            if (gap2 * 0.25f > reach2) break;
            const Vec3 n = gap * (1.0f / std::sqrt(gap2));
            const int r = clip(cell, n, dot(n, (seeds[i] + seeds[j]) * 0.5f), eps, settings.uv_scale, next);
            if (r == 0) {
                ++s.skipped;
                continue;
            }
            if (r == 2) {
                empty = true;
                break;
            }
            ++s.cuts;
            cell.vertices.swap(next.vertices);
            cell.indices.swap(next.indices);
            cell.sections.swap(next.sections);
            reach2 = 0.0f;
            for (const rhi::MeshVertex& v : cell.vertices) reach2 = std::fmax(reach2, dot(pos(v) - seeds[i], pos(v) - seeds[i]));
        }
        if (empty || cell.indices.empty()) continue;
        weld_mesh(cell);
        std::vector<CutMesh> islands = split_islands(cell);
        s.islands += islands.empty() ? 0u : static_cast<u32>(islands.size() - 1);
        for (CutMesh& island : islands) {
            FracturePiece piece;
            piece.volume = mesh_volume(island, piece.centre_of_mass);
            if (island.triangle_count() < 4 || piece.volume < settings.min_volume) continue;
            piece.hull = hull_points(island);
            piece.bounds_min = {1e30f, 1e30f, 1e30f};
            piece.bounds_max = {-1e30f, -1e30f, -1e30f};
            for (const rhi::MeshVertex& v : island.vertices) piece.bounds_min = vmin(piece.bounds_min, pos(v)), piece.bounds_max = vmax(piece.bounds_max, pos(v));
            piece.mesh = std::move(island);
            pieces.push_back(std::move(piece));
        }
    }
    s.milliseconds = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count();
    return pieces;
}

} // namespace lsd::render
