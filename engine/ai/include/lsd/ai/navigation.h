#pragma once
// Navigation on Recast & Detour (hidden behind this header): a navmesh built from the level's solid
// geometry (Unreal's Nav Mesh), paths across it, and a crowd of agents walking to targets while
// steering around each other (Unreal's AI Move To with RVO / detour crowd avoidance).
// World space is ours (Z up, metres); the Y-up conversion Recast wants stays inside.
#include "lsd/foundation/math.h"

namespace lsd::ai {

struct NavSettings {
    f32 agent_radius = 0.4f;    // how close to walls the mesh comes
    f32 agent_height = 1.8f;    // headroom needed
    f32 max_climb = 0.45f;      // step height
    f32 max_slope = 45.0f;      // degrees
    f32 cell_size = 0.15f;      // voxel size across (smaller: finer, slower)
    f32 cell_height = 0.1f;     // voxel size up
};

constexpr u32 kMaxAgents = 128;

// An area cut out of the navmesh (Unreal's Nav Modifier Volume with the Null area): a footprint of
// 4 world-space corners in order (a box seen from above, turned any way about Z) between two heights.
struct NavCutout {
    Vec3 corners[4];
    f32 z_min = 0.0f;
    f32 z_max = 0.0f;
};

class NavMesh {
public:
    NavMesh();
    ~NavMesh();
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    // Builds from world-space triangles (xyz per vertex, three indices per triangle) inside the box,
    // minus the cutouts. Replaces the previous mesh and its agents. False (and empty) if nothing is walkable.
    bool build(const f32* vertices, u32 vertex_count, const u32* indices, u32 triangle_count, Vec3 bounds_min, Vec3 bounds_max,
               const NavSettings& settings, const NavCutout* cutouts = nullptr, u32 cutout_count = 0);
    bool valid() const;
    void clear();
    void clear_agents();  // every crowd agent goes; the mesh stays (reused by the next Play when nothing changed)

    // The corners of the shortest path (both ends included); 0 if there is none. `to` may be off the
    // mesh: the path then ends at the nearest point on it.
    u32 find_path(Vec3 from, Vec3 to, Vec3* out, u32 capacity) const;
    bool nearest_point(Vec3 point, Vec3& out) const;  // within a few metres
    bool random_point(Vec3 around, f32 radius, Vec3& out) const;  // reachable from `around`
    // The navmesh's polygon edges as line pairs (the viewport's Show Navigation). Returns points written.
    u32 debug_lines(Vec3* out, u32 capacity) const;

    // Agents: -1 if the crowd is full or `position` is far from the mesh.
    i32 add_agent(Vec3 position, f32 radius, f32 height, f32 max_speed);
    void remove_agent(i32 agent);
    bool move_agent(i32 agent, Vec3 target);  // false if there is no walkable point near the target
    void stop_agent(i32 agent);
    void update(f32 dt);
    Vec3 agent_position(i32 agent) const;
    Vec3 agent_velocity(i32 agent) const;
    bool agent_moving(i32 agent) const;  // has a target it has not reached

private:
    struct Impl;
    Impl* impl_;
};

} // namespace lsd::ai
