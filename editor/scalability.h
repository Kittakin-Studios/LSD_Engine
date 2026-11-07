#pragma once
// Engine Scalability (Unreal's Scalability groups, sg.*): the overall Quality (Low .. Epic) or a
// level per group: Shadows, Post Processing, View Distance, Effects. Pure, so the tests check it.
#include "editor_settings.h"

namespace lsd::rhi {
struct PostDraw;
}

namespace lsd::editor {

struct Scalability {
    u32 shadows = 2, post = 2, view_distance = 2, effects = 2;  // 0 Low .. 3 Epic
};
// Each group's level: its own setting, or the overall Quality when it is on Overall.
Scalability resolve_scalability(const GameUserSettings& settings);
// Times every draw / cull distance (and the distance LODs are picked at): Low 0.5 .. Epic 1.5.
f32 view_distance_scale(u32 quality);
// Times every particle emitter's rate and burst: Low 0.25, Medium 0.5, High and Epic 1.
f32 effects_scale(u32 quality);
// Trims the post pass to the level: Low keeps bloom, tonemap, grading and FXAA only (no AO, SSR,
// depth of field, motion blur, contact shadows, TAA); Medium adds AO back; High and Epic as set.
void limit_post(u32 quality, rhi::PostDraw& post);

} // namespace lsd::editor
