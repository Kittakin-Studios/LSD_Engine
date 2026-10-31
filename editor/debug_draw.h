#pragma once
// Debug drawing from scripts (Unreal's DrawDebugLine / Sphere / Box / String): shapes that stay for
// some seconds (0: this frame only), drawn over the viewport. Cleared when Play stops.
#include "lsd/foundation/math.h"

struct ImDrawList;
struct ImVec2;

namespace lsd::editor::debug_draw {

void line(Vec3 a, Vec3 b, Vec3 color, f32 seconds);
void sphere(Vec3 centre, f32 radius, Vec3 color, f32 seconds);
void box(Vec3 centre, Vec3 half_size, Vec3 color, f32 seconds);
void text(Vec3 at, const char* text, Vec3 color, f32 seconds);

void tick(f32 dt);  // after drawing: ages the shapes, drops expired and one-frame ones
void clear();
u32 count();
void draw(ImDrawList* list, const Mat4& view_proj, const ImVec2& pos, const ImVec2& size);

} // namespace lsd::editor::debug_draw
