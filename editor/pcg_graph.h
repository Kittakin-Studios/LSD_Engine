#pragma once
// PCG Graphs (Unreal's PCG Graph): a PCG Volume's rules as nodes. Samplers make points on the surfaces in the
// volume's box (or along its Spline), filters drop some, Transform Points sizes and turns them, and each Spawn Mesh
// node puts a mesh on the points that reach it (an Instanced Static Mesh each). Pure: the surface comes in as a
// ray function, so the tests run it without a level.
#include "lsd/foundation/math.h"
#include "lsd/reflect/color.h"
#include "lsd/script/graph.h"

#include <functional>
#include <vector>

namespace lsd::editor {

struct PcgPoint {
    Vec3 position;
    Vec3 normal{0.0f, 0.0f, 1.0f};
    f32 yaw = 0.0f, scale = 1.0f;  // degrees around Z; uniform size
    bool align = false;            // tilt onto the normal
};

struct PcgSpawn {
    char mesh[40] = "";
    char material[33] = "";
    LinearColor color;
    std::vector<PcgPoint> points;
};

struct PcgInputs {
    Mat4 box;  // the volume's world matrix: the unit square (x, y in -0.5..0.5), z 0..1
    const Vec3* curve = nullptr;  // the volume's Spline in world space (Spline Sampler), if any
    const Vec3* tangents = nullptr;
    u32 curve_points = 0;
    bool closed = false;
    // The first surface along `down` from `top` within `reach`: true and `hit` (a level's renderable meshes).
    std::function<bool(Vec3 top, Vec3 down, f32 reach, Vec3& hit)> surface;
    std::vector<Mat4> excluded;  // exclusion volumes, world to box: no points inside them
    u32 capacity = 1024;         // points per Spawn Mesh at most
};

// Runs every Spawn Mesh node. False with `error` (the node and what is wrong) on an unknown node or a loop.
bool run_pcg_graph(const script::Graph& graph, const PcgInputs& in, std::vector<PcgSpawn>& out, char* error, usize capacity);

} // namespace lsd::editor
