#pragma once
// Level editing modes (Unreal's mode dropdown, here in the Level tab): Select, 3D Modeling, Foliage,
// Landscape, Animation Sequence, PCG and VFX. The active mode's tools show in the Mode panel; Foliage
// and Landscape paint in the viewport with LMB (Shift: erase / lower).
#include "editor_ui.h"
#include "lsd/rhi/rhi.h"

#include <imgui.h>

namespace lsd::editor {

class MeshCache;

const char* mode_name(EditorMode mode);  // "Landscape", "3D Modeling"...
void draw_mode_menu(EditorState& s);   // the Level tab's list
void draw_mode_panel(EditorState& s);  // the active mode's tools (nothing in Select)
// The viewport's brush in Foliage and Landscape. True while it owns the mouse (no selection clicks).
bool mode_brush(EditorState& s, const Mat4& view_proj, ImVec2 pos, ImVec2 size, bool hovered);
// Each frame, in the editor and the game: landscape meshes follow their heights.
void update_landscapes(world::World& world, MeshCache& meshes);
// Editable Meshes (3D Modeling mode): their Static Mesh's generated mesh follows their points and faces.
void update_editable_meshes(world::World& world, MeshCache& meshes);
// A painted landscape's blended layers (its mesh key, "Landscape:..."); invalid when it has no layers.
rhi::TextureHandle landscape_texture(const char* mesh_key);
// Each frame in the editor: PCG volumes rebuild their copies when a rule or the box changed.
void update_pcg(EditorState& s);
// Rebuilds one PCG volume's copies now.
void generate_pcg(EditorState& s, world::Entity volume);

// From editor_ui.cpp, for the modes.
bool surface_ray(EditorState& s, Vec3 origin, Vec3 dir, f32& t);  // nearest mesh along the ray
void viewport_ray(const EditorCamera& camera, ImVec2 pos, ImVec2 size, ImVec2 mouse, Vec3& origin, Vec3& dir);
world::Entity place_object(EditorState& s, const char* name, const char* mesh, const char* component, Vec3 position);
void mark_edited(EditorState& s, const char* label);  // one undo step, committed when the mouse is idle
void new_data_asset(EditorState& s, const reflect::TypeInfo& type);

} // namespace lsd::editor
