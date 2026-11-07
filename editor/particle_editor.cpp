#include "particle_editor.h"
#include "tips.h"
#include "workspaces.h"

#include "lsd/debug_ui/details_panel.h"

#include <imgui.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace lsd::editor {

namespace {
constexpr f32 kStep = 1.0f / 60.0f;  // scrubbing re-simulates from the start in steps of this
constexpr f32 kGravity = 9.81f;      // the preview has no level: standard gravity
}

ParticleEditor::ParticleEditor(rhi::Device& device, Allocator& allocator, ParticlePlayer& player, const render::GpuLines& grid)
    : player_(player), grid_(grid), instance_(allocator) {
    target_ = device.create_render_target(kPreviewSize, kPreviewSize);
}

void ParticleEditor::open(EditorState& s, const asset::AssetRecord& r) {
    guid_ = r.guid;
    std::snprintf(name_, sizeof name_, "%s", r.name);
    open_ = true;
    paused_ = false;
    time_ = 0.0f;
    instance_.restart();
    select_asset(s, r.name);  // its emitters in Details
    if (std::strcmp(s.settings.workspace, "Particles") != 0) std::snprintf(s.workspace_request, sizeof s.workspace_request, "Particles");  // Niagara opens in its own space
    ImGui::SetWindowFocus("###ParticleEditor");
}

const particles::ParticleSystem* ParticleEditor::system(EditorState&) {
    if (void* edited = edited_data_asset(guid_, reflect::type_of<particles::ParticleSystem>())) return static_cast<const particles::ParticleSystem*>(edited);
    char text[33];
    asset::to_string(guid_, text);
    return player_.system(text);
}

f32 ParticleEditor::timeline_length(const particles::ParticleSystem& system) const {
    f32 length = 0.0f;
    for (const particles::ParticleEmitter& e : system.emitters) {
        const f32 end = (e.duration > 0.0f ? e.duration : 3.0f) + e.lifetime_max;  // emitting forever: show a few seconds
        length = end > length ? end : length;
    }
    return length > 0.5f ? length : 0.5f;
}

void ParticleEditor::scrub_to(const particles::ParticleSystem& system, f32 time) {
    instance_.restart();
    for (time_ = 0.0f; time_ + kStep <= time; time_ += kStep) instance_.update(system, Mat4{}, kStep, kGravity, true);
}

void ParticleEditor::draw(EditorState& s) {
    drawn_ = nullptr;
    if (!open_) return;
    char title[128];
    std::snprintf(title, sizeof title, "%s - Particle Editor###ParticleEditor", name_);
    ImGui::SetNextWindowSize(ImVec2(560, 720), ImGuiCond_FirstUseEver);
    if (!begin_editor(title, &open_)) return;  // a panel tab, or the workspace's editor area
    const particles::ParticleSystem* system = this->system(s);
    if (!system) {
        ImGui::TextDisabled("The asset could not be read.");
        ImGui::End();
        return;
    }

    // Toolbar.
    if (ImGui::Button(paused_ ? "Play" : "Pause")) paused_ = !paused_;
    ImGui::SameLine();
    if (with_tip(ImGui::Button("Restart"), "Plays the system again from the start")) scrub_to(*system, 0.0f);
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &loop_);
    ImGui::SetItemTooltip("Start again when every emitter has finished (non-looping systems)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    with_tip(ImGui::SliderFloat("Speed", &speed_, 0.05f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic), "Preview speed (slow motion or fast forward)");

    // Simulate.
    const auto start = std::chrono::steady_clock::now();
    if (!paused_) {
        const f32 dt = ImGui::GetIO().DeltaTime * speed_;
        instance_.update(*system, Mat4{}, dt, kGravity, true);
        time_ += dt;
        if (loop_ && instance_.finished()) scrub_to(*system, 0.0f);
    }
    sim_ms_ = std::chrono::duration<f32, std::milli>(std::chrono::steady_clock::now() - start).count();

    // Wide enough: the stack in its own scrolling column beside the preview (Niagara's layout).
    const bool wide = ImGui::GetContentRegionAvail().x > 512.0f + 360.0f;
    if (wide) ImGui::BeginChild("##preview_side", ImVec2(512.0f, 0.0f));

    // Preview (rendered by render() this frame), orbit with the mouse.
    const f32 side = ImGui::GetContentRegionAvail().x < 512.0f ? ImGui::GetContentRegionAvail().x : 512.0f;
    const bool flipped = s.device->render_targets_flipped();
    ImGui::Image(static_cast<ImTextureID>(s.device->debug_ui_texture(target_)), ImVec2(side, side), ImVec2(0, flipped ? 1.0f : 0.0f),
                 ImVec2(1, flipped ? 0.0f : 1.0f));
    if (ImGui::IsItemHovered()) {
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            yaw_ -= io.MouseDelta.x * 0.01f;
            pitch_ += io.MouseDelta.y * 0.01f;
            pitch_ = pitch_ < -1.4f ? -1.4f : pitch_ > 1.4f ? 1.4f : pitch_;
        }
        if (io.MouseWheel != 0.0f) distance_ *= std::pow(0.85f, io.MouseWheel);
        distance_ = distance_ < 0.5f ? 0.5f : distance_ > 200.0f ? 200.0f : distance_;
        ImGui::SetItemTooltip("Left-drag: orbit. Wheel: zoom");
    }
    const Vec3 dir{std::cos(pitch_) * std::cos(yaw_), std::cos(pitch_) * std::sin(yaw_), std::sin(pitch_)};
    eye_ = focus_ + dir * distance_;
    view_ = look_at(eye_, focus_, Vec3{0, 0, 1});
    view_proj_ = perspective(0.9f, 1.0f, 0.05f, 500.0f) * view_;
    drawn_ = system;

    // Timeline: drag to jump to a moment (re-simulated from the start).
    const f32 length = timeline_length(*system);
    f32 shown = time_ < length ? time_ : length;
    ImGui::SetNextItemWidth(side);
    if (ImGui::SliderFloat("##time", &shown, 0.0f, length, "%.2f s")) {
        scrub_to(*system, shown);
        paused_ = true;
    }

    // Stats.
    u32 total = 0;
    if (ImGui::BeginTable("emitters", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Emitter");
        ImGui::TableSetupColumn("Particles");
        ImGui::TableSetupColumn("Blend");
        ImGui::TableSetupColumn("State");
        ImGui::TableHeadersRow();
        for (u32 i = 0; i < system->emitters.count; ++i) {
            const particles::ParticleEmitter& e = system->emitters[i];
            const u32 count = instance_.count(i);
            total += count;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.name);
            ImGui::TableNextColumn();
            ImGui::Text("%u / %u", count, e.max_particles);
            if (count >= e.max_particles) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "full");
                ImGui::SetItemTooltip("At its cap: raise Max Particles or lower the rate / lifetime");
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.blend == 1 ? "Additive" : "Alpha");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(!e.enabled ? "Disabled" : e.duration <= 0.0f ? "Forever" : e.looping ? "Looping" : "Once");
        }
        ImGui::EndTable();
    }
    ImGui::Text("%u particles   sim %.3f ms   time %.2f s", total, sim_ms_, time_);
    if (wide) {
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##stack_side", ImVec2(0.0f, 0.0f));
        draw_stack(s);
        ImGui::EndChild();
    } else {
        draw_stack(s);
    }
    ImGui::End();
}

// Niagara's emitter stack: the selected emitter's modules in the order a particle meets them, under
// their stage (Emitter Update, Particle Spawn, Particle Update, Render), editing the same object as
// Details (saved by itself a moment after a change).
void ParticleEditor::draw_stack(EditorState& s) {
    auto* system = static_cast<particles::ParticleSystem*>(edited_data_asset(guid_, reflect::type_of<particles::ParticleSystem>()));
    ImGui::SeparatorText("Emitter Stack");
    if (!system) {
        ImGui::TextDisabled("Select this Particle System in the Content Browser to edit its stack here.");
        return;
    }
    if (ImGui::BeginTabBar("##stack_emitters", ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (u32 i = 0; i < system->emitters.count; ++i) {
            char label[64];
            std::snprintf(label, sizeof label, "%s%s###emitter%u", system->emitters[i].name, system->emitters[i].enabled ? "" : " (off)", i);
            if (ImGui::BeginTabItem(label)) {
                stack_emitter_ = i;
                ImGui::EndTabItem();
            }
        }
        if (with_tip(ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip), "Adds an emitter (up to 8)") &&
            system->emitters.push_back(particles::ParticleEmitter{})) {
            mark_data_asset_edited();
        }
        ImGui::EndTabBar();
    }
    if (system->emitters.count == 0) {
        ImGui::TextDisabled("No emitters yet: + adds one.");
        return;
    }
    if (stack_emitter_ >= system->emitters.count) stack_emitter_ = 0;
    particles::ParticleEmitter& e = system->emitters[stack_emitter_];
    DetailsStyle style = editor_details_style(s);
    struct Stage {
        const char* label;
        ImVec4 color;
        const char* modules;
        const char* tip;
    };
    static const Stage stages[] = {
        {"Emitter", ImVec4(0.45f, 0.45f, 0.5f, 1.0f), "Emitter", "Its name, on / off, local or world space"},
        {"Emitter Update", ImVec4(0.93f, 0.55f, 0.1f, 1.0f), "Spawn", "How many particles it makes, and for how long"},
        {"Particle Spawn", ImVec4(0.3f, 0.7f, 0.25f, 1.0f), "Initialize Particle,Shape,Velocity", "Once, when each particle is born: its life, where, how fast"},
        {"Particle Update", ImVec4(0.2f, 0.45f, 0.85f, 1.0f), "Forces,Color,Size,Rotation,Over Life,Collision", "Every frame of its life: forces, colour, size, turning, hits"},
        {"Render", ImVec4(0.8f, 0.25f, 0.3f, 1.0f), "Renderer", "How it is drawn: sprite, mesh or ribbon, texture, blend"},
    };
    bool changed = false;
    for (const Stage& stage : stages) {
        ImGui::PushStyleColor(ImGuiCol_Header, stage.color);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(stage.color.x * 1.15f, stage.color.y * 1.15f, stage.color.z * 1.15f, 1.0f));
        const bool open = ImGui::CollapsingHeader(stage.label, ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor(2);
        ImGui::SetItemTooltip("%s", stage.tip);
        if (!open) continue;
        ImGui::PushID(stage.label);
        // Each module of the stage under its own header with an on / off box (Niagara's module checkboxes).
        static const struct {
            const char* category;
            u32 bit;  // 0: always on
        } kModules[] = {{"Emitter", 0}, {"Spawn", 0}, {"Initialize Particle", 0}, {"Shape", particles::kModuleShape}, {"Velocity", particles::kModuleVelocity},
                        {"Forces", particles::kModuleForces}, {"Color", particles::kModuleColor}, {"Size", particles::kModuleSize},
                        {"Rotation", particles::kModuleRotation}, {"Over Life", particles::kModuleOverLife}, {"Collision", particles::kModuleCollision},
                        {"Renderer", 0}};
        for (const auto& m : kModules) {
            if (!std::strstr(stage.modules, m.category)) continue;
            if (m.bit) {  // its box above its settings; off: the settings fold away
                bool on = (e.disabled_modules & m.bit) == 0;
                char label[48];
                std::snprintf(label, sizeof label, "Use %s", m.category);
                if (ImGui::Checkbox(label, &on)) {
                    e.disabled_modules = on ? e.disabled_modules & ~m.bit : e.disabled_modules | m.bit;
                    changed = true;
                }
                ImGui::SetItemTooltip("Switches the %s module off (its settings stay) to see the effect without it", m.category);
                if (!on) continue;
            }
            style.categories = m.category;
            changed = draw_details(reflect::type_of<particles::ParticleEmitter>(), &e, &style) || changed;
        }
        ImGui::PopID();
    }
    if (with_tip(ImGui::Button("Remove Emitter"), "Takes this emitter out of the system (undo with Ctrl+Z in Details is not offered: copy the asset first)") && system->emitters.count > 1) {
        system->emitters.remove(stack_emitter_);
        stack_emitter_ = 0;
        changed = true;
    }
    if (changed) mark_data_asset_edited();
}

void ParticleEditor::render(rhi::Device& device, const f32 background[4]) {
    if (!open_ || !drawn_ || !target_.valid()) return;
    device.set_render_target(target_, background);
    if (grid_.vertex_count) device.draw_lines(grid_.vertices, grid_.vertex_count, view_proj_.m);
    player_.draw_instance(device, instance_, *drawn_, Mat4{}, view_proj_, view_, eye_);
    device.set_render_target({}, nullptr);
}

} // namespace lsd::editor
