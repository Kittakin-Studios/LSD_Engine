#include "scalability.h"

#include "lsd/rhi/rhi.h"

#include <cctype>

namespace lsd::editor {

namespace {
u32 level(u32 group, u32 overall) { return group ? (group - 1 < 3 ? group - 1 : 3) : (overall < 3 ? overall : 3); }
} // namespace

Scalability resolve_scalability(const GameUserSettings& s) {
    return {level(s.shadow_quality, s.quality), level(s.post_quality, s.quality), level(s.view_distance_quality, s.quality), level(s.effects_quality, s.quality),
            level(0, s.quality)};
}

f32 view_distance_scale(u32 quality) {
    static const f32 kScale[] = {0.5f, 0.75f, 1.0f, 1.5f};
    return kScale[quality < 3 ? quality : 3];
}

bool profile_matches(const DeviceProfile& p, const char* gpu, u64 vram, u32 platform) {
    if (p.platform && p.platform != platform) return false;
    const u64 mb = vram / (1024ull * 1024ull);
    if ((p.min_video_memory_mb && mb < p.min_video_memory_mb) || (p.max_video_memory_mb && mb > p.max_video_memory_mb)) return false;
    if (!p.gpu_contains[0]) return true;
    // Case-insensitive "contains" (GPU names come in mixed case).
    for (const char* s = gpu; *s; ++s) {
        usize k = 0;
        while (p.gpu_contains[k] && s[k] && std::tolower(static_cast<unsigned char>(s[k])) == std::tolower(static_cast<unsigned char>(p.gpu_contains[k]))) ++k;
        if (!p.gpu_contains[k]) return true;
    }
    return false;
}

void apply_profile(const DeviceProfile& p, GameUserSettings& s) {
    s.quality = p.quality < 4 ? p.quality : 3;
    s.shadow_quality = p.shadow_quality;
    s.post_quality = p.post_quality;
    s.view_distance_quality = p.view_distance_quality;
    s.effects_quality = p.effects_quality;
}

f32 texture_detail_scale(u32 quality) {
    static const f32 kScale[] = {0.25f, 0.5f, 1.0f, 2.0f};
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
