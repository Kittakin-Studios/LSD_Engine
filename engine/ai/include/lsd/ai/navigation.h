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

// A jump, drop or ladder between two places the navmesh does not join (Unreal's Nav Link Proxy): paths may use it,
// agents cross it in a straight line (Detour's off-mesh connection). Each end must be within `radius` of the mesh.
struct NavLink {
    Vec3 start, end;
    f32 radius = 0.5f;
    bool both_ways = true;  // false: start -> end only (a drop off a ledge)
};

class NavMesh {
public:
    NavMesh();
    ~NavMesh();
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    // Builds from world-space triangles (xyz per vertex, three indices per triangle) inside the box,
    // minus the cutouts. Replaces the previous mesh's agents. False (and empty) if nothing is walkable.
    // Tiled (64 x 64 cells a tile): with the same box and settings as the last build, only the tiles whose triangles,
    // cutouts or links changed are rebuilt (a door that opened, a crate pushed), the rest kept.
    bool build(const f32* vertices, u32 vertex_count, const u32* indices, u32 triangle_count, Vec3 bounds_min, Vec3 bounds_max,
               const NavSettings& settings, const NavCutout* cutouts = nullptr, u32 cutout_count = 0, const NavLink* links = nullptr, u32 link_count = 0);
    u32 last_rebuilt_tiles() const;  // how many tiles the last build made (all of them, or the changed ones)
    bool valid() const;
    void clear();
    void clear_agents();  // every crowd agent goes; the mesh stays (reused by the next Play when nothing changed)

    // The corners of the shortest path (both ends included); 0 if there is none. `to` may be off the
    // mesh: the path then ends at the nearest point on it.
    u32 find_path(Vec3 from, Vec3 to, Vec3* out, u32 capacity) const;
    bool nearest_point(Vec3 point, Vec3& out) const;  // within a few metres
    bool random_point(Vec3 around, f32 radius, Vec3& out) const;  // reachable from `around`
    // The navmesh's polygon edges as line pairs (the viewport's Show Navigation), nav links as one line each. Returns points written.
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
