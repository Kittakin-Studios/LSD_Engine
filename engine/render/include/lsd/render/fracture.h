#pragma once
// Mesh cutting for destruction (Unreal's Fracture Mode / Geometry Collections): a triangle mesh cut by a
// plane into two closed parts whose cut faces are capped, and Voronoi fracture into pieces (each cell is the
// mesh clipped by the bisector planes to the other seeds, so concave meshes such as buildings work; a cell
// that falls apart into several islands gives several pieces). Pure geometry, editor time; the pieces keep
// the mesh's own space, so unbroken pieces put together are the mesh again without gaps.
//
// shortcut: std containers and O(n^2) cap triangulation (editor-time, caps have hundreds of points at most);
// move to the frame allocator if fracture ever runs per frame.
// Known limits: a source with holes (an open bottom, missing faces) gets its caps closed by joining the open
// ends of the cut outline, so a cap across a hole may be a little off; a cut lying exactly in a face of the
// mesh caps that face with the inside material.
#include "lsd/foundation/math.h"
#include "lsd/rhi/rhi.h"

#include <vector>

namespace lsd::render {

constexpr u16 kInsideSection = 0xFFFF;  // triangles of cut faces (Unreal's inside / interior material)

// A mesh to cut: vertices, triangles, and per triangle its material section (which slot draws it).
struct CutMesh {
    std::vector<rhi::MeshVertex> vertices;
    std::vector<u32> indices;
    std::vector<u16> sections;  // indices.size() / 3 entries
    u32 triangle_count() const { return static_cast<u32>(indices.size() / 3); }
};

// Cuts `in` by the plane dot(normal, p) = offset: `back` gets what lies below it (dot < offset), `front`
// the rest, each closed with a cap (kInsideSection, planar UVs: `uv_scale` tiles per metre). Either output may
// be null. Returns false (outputs untouched) when the plane misses the mesh.
bool slice_mesh(const CutMesh& in, Vec3 normal, f32 offset, CutMesh* back, CutMesh* front, f32 uv_scale = 1.0f);

// Merges vertices equal in position, normal and UV (after cutting, each side has many duplicates).
void weld_mesh(CutMesh& mesh);

// Splits a mesh into its connected parts (triangles sharing a corner position).
std::vector<CutMesh> split_islands(const CutMesh& mesh);

// Volume (m^3) and centre of mass of a closed mesh (signed tetrahedra); a flat or open mesh: its bounds centre.
f32 mesh_volume(const CutMesh& mesh, Vec3& centre_of_mass);

// A few dozen points on the piece's convex hull (the farthest point along many directions), for a convex
// physics shape without handing it thousands of points.
std::vector<Vec3> hull_points(const CutMesh& mesh);

struct FractureSettings {
    u32 pieces = 20;            // Voronoi seeds (Unreal's Uniform: Min / Max Voronoi Sites)
    u32 seed = 1;               // the same seed cuts the same way (every machine in multiplayer)
    bool cluster = false;       // put most seeds near `cluster_point` (Unreal's Cluster / impact point): small pieces there
    Vec3 cluster_point;         // in the mesh's space
    f32 cluster_radius = 1.0f;  // m
    f32 cluster_share = 0.7f;   // of the seeds, how many go near the point (the rest anywhere)
    f32 uv_scale = 1.0f;        // inside faces: texture tiles per metre
    f32 min_volume = 0.0f;      // pieces smaller than this (m^3) are dropped (crumbs)
};

struct FracturePiece {
    CutMesh mesh;          // in the mesh's space
    Vec3 centre_of_mass;
    f32 volume = 0.0f;
    std::vector<Vec3> hull;  // convex hull points (mesh space)
    Vec3 bounds_min, bounds_max;
};

struct FractureStats {
    u32 cuts = 0;          // plane clips that changed something
    u32 skipped = 0;       // bisector planes that missed the cell's current mesh
    u32 islands = 0;       // extra pieces from cells that fell apart (concave meshes)
    f64 milliseconds = 0;
};

// The seeds fracture_voronoi uses (inside the mesh's bounds; deterministic for a seed).
std::vector<Vec3> voronoi_seeds(Vec3 bounds_min, Vec3 bounds_max, const FractureSettings& settings);
std::vector<FracturePiece> fracture_voronoi(const CutMesh& mesh, const FractureSettings& settings, FractureStats* stats = nullptr);

} // namespace lsd::render
