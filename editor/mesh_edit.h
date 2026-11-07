#pragma once
// Modeling-mode mesh editing (Unreal's Modeling Mode PolyEdit): an Editable Mesh's points and faces
// (quads and triangles) changed by Extrude, Inset, Bevel, Push / Pull, Delete and Flip, and built
// into a flat-shaded, box-UV mesh. Pure (no GPU, no editor state): the tests run it.
// shortcut: faces of 3 or 4 points (booleans cut bigger ones into triangles); add a half-edge mesh for loops and bevelled edges.
#include "lsd/render/mesh.h"
#include "lsd/world/components.h"

#include <string>

namespace lsd::editor {

constexpr u32 kNoPoint = 0xFFFFFFFFu;  // EditFace::d of a triangle

u32 face_size(const world::EditFace& f);  // 3 or 4
Vec3 face_point(const world::EditableMesh& mesh, const world::EditFace& f, u32 corner);
Vec3 face_normal(const world::EditableMesh& mesh, u32 face);  // unit, outward (counter-clockwise seen from outside)
Vec3 face_centre(const world::EditableMesh& mesh, u32 face);

// The 1 m box the built-in Cube is (x, y from -0.5 to 0.5, z from 0 to 1), as 8 points and 6 quads.
void make_box(world::EditableMesh& mesh);
// Each returns false (mesh unchanged) when the face is missing or the lists are full.
bool extrude(world::EditableMesh& mesh, u32 face, f32 distance);  // the face moves out along its normal, walls join it
bool inset(world::EditableMesh& mesh, u32 face, f32 fraction);    // a smaller copy inside it (0..1 toward the centre), ringed by quads
bool bevel(world::EditableMesh& mesh, u32 face, f32 fraction, f32 distance);  // inset, then extrude: a chamfered step
bool push_pull(world::EditableMesh& mesh, u32 face, f32 distance);  // moves the face's points along its normal
bool delete_face(world::EditableMesh& mesh, u32 face);
// Removes every face that uses the point (the point itself stays, unused); false if none did.
bool delete_point(world::EditableMesh& mesh, u32 point);
// Removes every face that has the edge a-b (both points, in either order); false if none did.
bool delete_edge(world::EditableMesh& mesh, u32 a, u32 b);

// Booleans (Unreal's Modeling Mode Boolean, as CSG on BSP trees): `other`'s points are given in
// `mesh`'s space (`to_mesh` maps them there). The result replaces `mesh`; false (unchanged) when it
// would not fit (1024 points / faces) or is empty.
enum class BooleanOp : u8 { Union, Subtract, Intersect };
bool boolean_op(world::EditableMesh& mesh, const world::EditableMesh& other, const Mat4& to_mesh, BooleanOp op);
bool flip_face(world::EditableMesh& mesh, u32 face);  // turns it inside out
// The nearest face a ray (in the mesh's own space) hits, -1 if none; `t` is the distance along `dir`.
i32 pick_face(const world::EditableMesh& mesh, Vec3 origin, Vec3 dir, f32& t);
// Triangles with a flat normal per face and UVs projected from the face's main axis (uv_scale tiles per unit).
void build_edit_mesh(const world::EditableMesh& mesh, render::MeshData& out);
// A Wavefront OBJ of the mesh (Save as Mesh Asset: imported like any mesh file).
std::string to_obj(const world::EditableMesh& mesh, const char* name);

} // namespace lsd::editor
