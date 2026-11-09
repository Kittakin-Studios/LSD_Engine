#pragma once
// Cloth meshes (Unreal's Chaos Cloth asset, made on the fly in Play): a Static Mesh's triangles,
// optionally subdivided, welded into particles (seams in the drawn mesh share one particle), some of
// them pinned. Pure logic; PlaySession hands the particles to physics::PhysicsWorld::add_cloth and
// redraws the mesh from them every frame.
#include "lsd/foundation/math.h"
#include "lsd/rhi/rhi.h"

#include <vector>

namespace lsd::editor {

struct ClothMesh {
    std::vector<rhi::MeshVertex> vertices;  // drawn (positions as given: Play passes world space)
    std::vector<u32> indices;
    std::vector<u32> particle_of;  // per drawn vertex: its particle
    std::vector<Vec3> particles;   // welded positions
    std::vector<u32> particle_indices;  // the triangles over the particles
    std::vector<u8> pinned;        // per particle
};

// Each triangle into four, `levels` times; edge midpoints are shared by the two triangles on that edge
// and take the average position, normal and UV.
void subdivide_cloth(std::vector<rhi::MeshVertex>& vertices, std::vector<u32>& indices, u32 levels);
// Fills particle_of / particles / particle_indices from vertices / indices: vertices closer than
// `tolerance` become one particle.
void weld_cloth(ClothMesh& mesh, f32 tolerance);
// Pins the particles within `amount` (0..1 of the cloth's size along that axis) of one side:
// 0 top (+Z), 1 bottom, 2 +X, 3 -X, 4 +Y, 5 -Y; anything else pins none. Returns how many.
u32 pin_cloth(ClothMesh& mesh, u32 side, f32 amount);
// Smooth normals from the particles' positions (`positions` per particle, in the space the vertices are drawn in),
// written to the drawn vertices along with those positions.
void update_cloth_vertices(ClothMesh& mesh, const Vec3* positions);

} // namespace lsd::editor
