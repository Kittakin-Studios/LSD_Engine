#pragma once
// The Particle Editor window (Unreal's Niagara System editor, simplified): opens on double-clicking
// a Particle System asset. A preview of the system alone (orbit with the left mouse, zoom with the
// wheel), play / pause / restart, a timeline to scrub, and per-emitter particle counts and sim time.
// The emitters are edited in the Details panel next to it; the preview follows unsaved edits live.
#include "editor_ui.h"
#include "particle_player.h"

#include "lsd/render/grid.h"

namespace lsd::editor {

class ParticleEditor {
public:
    ParticleEditor(rhi::Device& device, Allocator& allocator, ParticlePlayer& player, const render::GpuLines& grid);
    ParticleEditor(const ParticleEditor&) = delete;
    ParticleEditor& operator=(const ParticleEditor&) = delete;

    void open(EditorState& state, const asset::AssetRecord& record);
    void draw(EditorState& state);  // the window; simulates the preview (ImGui frame)
    // Renders the preview into its own target; between begin_frame and the viewports.
    void render(rhi::Device& device, const f32 background[4]);

private:
    const particles::ParticleSystem* system(EditorState& state);  // Details' copy if it is being edited there, else the file
    void scrub_to(const particles::ParticleSystem& system, f32 time);
    f32 timeline_length(const particles::ParticleSystem& system) const;

    static constexpr u32 kPreviewSize = 512;
    ParticlePlayer& player_;
    const render::GpuLines& grid_;
    particles::ParticleInstance instance_;
    rhi::TextureHandle target_;
    const particles::ParticleSystem* drawn_ = nullptr;  // this frame's system, for render()
    asset::Guid guid_;
    char name_[64] = "";
    bool open_ = false;
    bool paused_ = false;
    bool loop_ = true;
    f32 speed_ = 1.0f;
    f32 time_ = 0.0f;
    f32 sim_ms_ = 0.0f;
    f32 yaw_ = -2.4f, pitch_ = 0.3f, distance_ = 5.0f;
    Vec3 focus_{0.0f, 0.0f, 1.0f};
    Mat4 view_, view_proj_;
    Vec3 eye_;
};

} // namespace lsd::editor
