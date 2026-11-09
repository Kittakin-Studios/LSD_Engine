#include "pcg_graph.h"

#include "lsd/world/spline.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <unordered_map>

namespace lsd::editor {

namespace {

constexpr f32 kPi = 3.14159265f;

struct Run {
    const script::Graph& graph;
    const PcgInputs& in;
    std::unordered_map<u32, std::vector<PcgPoint>> done;  // each node's points, worked out once
    std::unordered_map<u32, bool> busy;                   // on the way down: a loop if met again
    char* error;
    usize capacity;
    bool ok = true;

    void fail(const script::GraphNode& node, const char* what) {
        if (!ok) return;
        const script::NodeDef* def = script::find_node_def(node.type);
        std::snprintf(error, capacity, "%s (node %u): %s", def ? def->title : node.type, node.id, what);
        ok = false;
    }
    f32 number(const script::GraphNode& node, u32 pin) const { return static_cast<f32>(std::atof(node.values[pin])); }
    bool flag(const script::GraphNode& node, u32 pin) const { return std::strcmp(node.values[pin], "true") == 0; }
    u32 seed(const script::GraphNode& node, u32 pin) const { return static_cast<u32>(std::fabs(number(node, pin))) * 2654435761u + node.id; }

    // The points wired into `pin` (none when unwired).
    std::vector<PcgPoint> input(const script::GraphNode& node, u32 pin) {
        const script::GraphLink* link = graph.link_into(node.id, pin);
        if (!link) return {};
        const script::GraphNode* from = graph.find(link->from_node);
        return from ? points(*from) : std::vector<PcgPoint>{};
    }

    // Down and the surface below a column's top, with its normal from two nearby hits.
    bool drop(Vec3 top, Vec3 bottom, PcgPoint& p) const {
        const f32 reach = length(bottom - top);
        if (reach < 1e-4f || !in.surface) return false;
        const Vec3 down = (bottom - top) * (1.0f / reach);
        Vec3 at, ax, ay;
        if (!in.surface(top, down, reach, at)) return false;
        p.position = at;
        // Two nearby hits (the other side when one misses: at the box's edge) give the slope.
        f32 sx = 0.2f, sy = 0.2f;
        bool hx = in.surface(top + Vec3{sx, 0, 0}, down, reach, ax), hy = in.surface(top + Vec3{0, sy, 0}, down, reach, ay);
        if (!hx) hx = in.surface(top + Vec3{sx = -0.2f, 0, 0}, down, reach, ax);
        if (!hy) hy = in.surface(top + Vec3{0, sy = -0.2f, 0}, down, reach, ay);
        if (hx && hy) {
            Vec3 n = cross((ax - at) * (sx > 0.0f ? 1.0f : -1.0f), (ay - at) * (sy > 0.0f ? 1.0f : -1.0f));
            if (length(n) > 1e-6f) n = normalize(n);
            if (n.z < 0.0f) n = n * -1.0f;
            p.normal = n;
        }
        return true;
    }

    std::vector<PcgPoint> points(const script::GraphNode& node) {
        if (const auto it = done.find(node.id); it != done.end()) return it->second;
        if (busy[node.id]) {
            fail(node, "its points come back into itself (a loop)");
            return {};
        }
        busy[node.id] = true;
        std::vector<PcgPoint> out;
        const char* type = node.type;
        std::uniform_real_distribution<f32> unit(0.0f, 1.0f);
        const Mat4& m = in.box;
        if (std::strcmp(type, "PcgSurface") == 0) {  // Density per 100 square metres of the box's floor, Seed
            std::mt19937 r(seed(node, 1));
            const f32 area = length(Vec3{m.at(0, 0), m.at(1, 0), m.at(2, 0)}) * length(Vec3{m.at(0, 1), m.at(1, 1), m.at(2, 1)});
            const u32 count = static_cast<u32>(std::fmin(static_cast<f32>(in.capacity) * 4.0f, std::fmax(number(node, 0), 0.0f) * area / 100.0f));
            for (u32 i = 0; i < count; ++i) {
                const f32 u = unit(r) - 0.5f, v = unit(r) - 0.5f;
                PcgPoint p;
                if (drop(transform_point(m, {u, v, 1.0f}), transform_point(m, {u, v, 0.0f}), p)) out.push_back(p);
            }
        } else if (std::strcmp(type, "PcgSpline") == 0) {  // Spacing, Width, Seed
            std::mt19937 r(seed(node, 2));
            const f32 spacing = std::fmax(number(node, 0), 0.1f), width = std::fmax(number(node, 1), 0.0f);
            const f32 total = in.curve_points >= 2 ? world::spline_length(in.curve, in.curve_points, in.closed, in.tangents) : 0.0f;
            const u32 count = total > 0.0f ? static_cast<u32>(std::fmin(static_cast<f32>(in.capacity) * 4.0f, std::floor(total / spacing) + 1.0f)) : 0u;
            for (u32 i = 0; i < count; ++i) {
                Vec3 along;
                const Vec3 at = world::spline_at_distance(in.curve, in.curve_points, in.closed, static_cast<f32>(i) * spacing, &along, in.tangents);
                Vec3 side{-along.y, along.x, 0.0f};
                side = length(side) > 1e-5f ? normalize(side) : Vec3{0, 1, 0};
                const Vec3 c = at + side * ((unit(r) - 0.5f) * width);
                PcgPoint p;
                if (drop(c + Vec3{0, 0, 20.0f}, c - Vec3{0, 0, 20.0f}, p)) out.push_back(p);
            }
        } else if (std::strcmp(type, "PcgSlope") == 0) {  // Points, Min, Max (degrees)
            for (const PcgPoint& p : input(node, 0)) {
                const f32 slope = std::acos(std::fmin(std::fmax(p.normal.z, -1.0f), 1.0f)) * 180.0f / kPi;
                if (slope >= number(node, 1) && slope <= number(node, 2)) out.push_back(p);
            }
        } else if (std::strcmp(type, "PcgHeight") == 0) {  // Points, Min, Max (world Z)
            for (const PcgPoint& p : input(node, 0)) {
                if (p.position.z >= number(node, 1) && p.position.z <= number(node, 2)) out.push_back(p);
            }
        } else if (std::strcmp(type, "PcgDensity") == 0) {  // Points, Keep (0..1), Patch Size (m, 0: each point alone), Seed
            const f32 keep = number(node, 1), patch = number(node, 2);
            const u32 salt = seed(node, 3);
            auto lattice = [&](i32 x, i32 y) {  // a hash of a lattice corner, 0..1
                u32 h = static_cast<u32>(x) * 73856093u ^ static_cast<u32>(y) * 19349663u ^ salt;
                h ^= h >> 13;
                h *= 0x5bd1e995u;
                h ^= h >> 15;
                return static_cast<f32>(h & 0xFFFFFF) / static_cast<f32>(0xFFFFFF);
            };
            std::mt19937 r(salt);
            for (const PcgPoint& p : input(node, 0)) {
                f32 noise = unit(r);
                if (patch > 0.0f) {  // value noise: whole patches kept or dropped together (clearings, clumps)
                    const f32 fx = p.position.x / patch, fy = p.position.y / patch;
                    const i32 x0 = static_cast<i32>(std::floor(fx)), y0 = static_cast<i32>(std::floor(fy));
                    f32 tx = fx - static_cast<f32>(x0), ty = fy - static_cast<f32>(y0);
                    tx = tx * tx * (3.0f - 2.0f * tx);
                    ty = ty * ty * (3.0f - 2.0f * ty);
                    const f32 a = lattice(x0, y0) + (lattice(x0 + 1, y0) - lattice(x0, y0)) * tx;
                    const f32 b = lattice(x0, y0 + 1) + (lattice(x0 + 1, y0 + 1) - lattice(x0, y0 + 1)) * tx;
                    noise = a + (b - a) * ty;
                }
                if (noise < keep) out.push_back(p);
            }
        } else if (std::strcmp(type, "PcgTransform") == 0) {  // Points, Scale Min, Scale Max, Random Yaw, Align To Surface, Offset Z, Seed
            std::mt19937 r(seed(node, 6));
            const f32 lo = number(node, 1), hi = number(node, 2), lift = number(node, 5);
            for (PcgPoint p : input(node, 0)) {
                const f32 yaw = unit(r) * 360.0f, k = unit(r);  // drawn every time: same seed, same layout
                p.scale = lo + (hi - lo) * k;
                p.yaw = flag(node, 3) ? yaw : 0.0f;
                p.align = flag(node, 4);
                p.position = p.position + p.normal * lift;
                out.push_back(p);
            }
        } else if (std::strcmp(type, "PcgMerge") == 0) {  // A, B
            out = input(node, 0);
            const std::vector<PcgPoint> b = input(node, 1);
            out.insert(out.end(), b.begin(), b.end());
        } else if (std::strcmp(type, "PcgSpawn") != 0) {
            fail(node, "not a PCG node");
        }
        busy[node.id] = false;
        done[node.id] = out;
        return out;
    }
};

} // namespace

bool run_pcg_graph(const script::Graph& graph, const PcgInputs& in, std::vector<PcgSpawn>& out, char* error, usize capacity) {
    out.clear();
    if (capacity) error[0] = '\0';
    Run run{graph, in, {}, {}, error, capacity};
    for (const script::GraphNode& node : graph.nodes) {
        if (std::strcmp(node.type, "PcgSpawn") != 0) {
            if (std::strncmp(node.type, "Pcg", 3) != 0) run.fail(node, "not a PCG node");
            continue;
        }
        PcgSpawn spawn;  // Points, Mesh, Material, Color
        std::snprintf(spawn.mesh, sizeof spawn.mesh, "%s", node.values[1]);
        std::snprintf(spawn.material, sizeof spawn.material, "%s", node.values[2]);
        f32 rgb[3] = {0.31f, 0.54f, 0.24f};
        std::sscanf(node.values[3], "%f,%f,%f", &rgb[0], &rgb[1], &rgb[2]);
        spawn.color.r = rgb[0];
        spawn.color.g = rgb[1];
        spawn.color.b = rgb[2];
        for (const PcgPoint& p : run.input(node, 0)) {
            if (spawn.points.size() >= in.capacity) break;
            bool inside = false;
            for (const Mat4& x : in.excluded) {
                const Vec3 q = transform_point(x, p.position);
                inside |= q.x >= -0.5f && q.x <= 0.5f && q.y >= -0.5f && q.y <= 0.5f && q.z >= 0.0f && q.z <= 1.0f;
            }
            if (!inside) spawn.points.push_back(p);
        }
        out.push_back(spawn);
    }
    return run.ok;
}

} // namespace lsd::editor
