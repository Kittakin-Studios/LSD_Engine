#include "scalability.h"

#include "lsd/rhi/rhi.h"

namespace lsd::editor {

namespace {
u32 level(u32 group, u32 overall) { return group ? (group - 1 < 3 ? group - 1 : 3) : (overall < 3 ? overall : 3); }
} // namespace

Scalability resolve_scalability(const GameUserSettings& s) {
    return {level(s.shadow_quality, s.quality), level(s.post_quality, s.quality), level(s.view_distance_quality, s.quality), level(s.effects_quality, s.quality)};
}

f32 view_distance_scale(u32 quality) {
    static const f32 kScale[] = {0.5f, 0.75f, 1.0f, 1.5f};
    return kScale[quality < 3 ? quality : 3];
}

f32 effects_scale(u32 quality) {
    static const f32 kScale[] = {0.25f, 0.5f, 1.0f, 1.0f};
    return kScale[quality < 3 ? quality : 3];
}

void limit_post(u32 quality, rhi::PostDraw& post) {
    if (quality >= 2) return;
    if (post.taa) post.fxaa = true;  // the cheap anti-aliasing instead
    post.taa = false;
    post.ssr_intensity = 0.0f;
    post.dof_focal_distance = 0.0f;
    post.motion_blur = 0.0f;
    post.contact_shadow[3] = 0.0f;
    if (quality == 0) post.ao_intensity = 0.0f;
}

} // namespace lsd::editor
