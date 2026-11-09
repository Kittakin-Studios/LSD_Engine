#include "ui_panels.h"

#include "lsd/asset/asset_registry.h"
#include "lsd/asset/guid.h"
#include "lsd/foundation/log.h"
#include "lsd/world/components.h"

#include <SDL3/SDL_events.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace lsd::editor {

void UiPanels::set_actions(ui::ActionHandler handler, void* user) {
    actions_ = handler;
    actions_user_ = user;
    for (Panel& p : panels_) p.context->set_actions(handler, user);
}

void UiPanels::clear() {
    for (Panel& p : panels_) {
        delete p.context;
        if (device_ && p.target.valid()) device_->destroy_texture(p.target);
    }
    panels_.clear();
}

void UiPanels::sync(const world::World& world, const asset::AssetRegistry* registry, rhi::Device& device, bool playing) {
    device_ = &device;
    if (playing != playing_) {  // a fresh document (and its .js) for Play, and again after it
        clear();
        playing_ = playing;
    }
    for (Panel& p : panels_) p.seen = false;
    const reflect::TypeInfo& type = reflect::type_of<world::UiPanel>();
    for (u32 i = 0; i < world.count(type); ++i) {
        const auto* c = static_cast<const world::UiPanel*>(world.at(type, i));
        const world::Entity e = world.owner(type, i);
        const u32 res = c->resolution < 64 ? 64u : c->resolution > 2048 ? 2048u : c->resolution;
        const f32 aspect = c->width > 0.01f ? c->height / c->width : 1.0f;
        const u32 w = res, h = static_cast<u32>(std::fmin(std::fmax(static_cast<f32>(res) * aspect, 16.0f), 2048.0f));
        Panel* panel = nullptr;
        for (Panel& p : panels_) {
            if (p.entity == e) panel = &p;
        }
        if (panel && (std::strcmp(panel->document, c->document) != 0 || panel->width != w || panel->height != h)) {  // changed: open it again
            delete panel->context;
            device.destroy_texture(panel->target);
            *panel = panels_.back();
            panels_.pop_back();
            panel = nullptr;
        }
        if (panel) {
            panel->seen = true;
            continue;
        }
        asset::Guid guid;
        const asset::AssetRecord* r = registry && asset::parse_guid(c->document, guid) ? registry->find(guid) : nullptr;
        if (!r) continue;  // no document picked yet: nothing drawn
        char name[32], path[300];
        std::snprintf(name, sizeof name, "panel%u", ++serial_);
        registry->source_path(*r, path, sizeof path);
        Panel p{e, {}, w, h, new ui::UiContext(name), device.create_render_target(w, h), true, false};
        std::snprintf(p.document, sizeof p.document, "%s", c->document);
        p.context->set_linear_colors(true);
        p.context->set_actions(actions_, actions_user_);
        if (p.context->open(path) < 0) LSD_LOG_WARN("ui", "UI Panel on %s: could not open %s", world.name(e), path);
        panels_.push_back(p);
    }
    for (usize i = 0; i < panels_.size();) {
        if (panels_[i].seen) {
            ++i;
            continue;
        }
        delete panels_[i].context;
        device.destroy_texture(panels_[i].target);
        panels_[i] = panels_.back();
        panels_.pop_back();
    }
}

void UiPanels::render(rhi::Device& device) {
    if (panels_.empty()) return;
    const f32 clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // see-through where the document draws nothing
    for (Panel& p : panels_) {
        if (!p.target.valid()) continue;
        device.set_render_target(p.target, clear);
        const f32 viewport[4] = {0.0f, 0.0f, static_cast<f32>(p.width), static_cast<f32>(p.height)};
        p.context->update(viewport);
        p.context->render(device);
    }
    device.set_render_target({}, nullptr);
}

rhi::TextureHandle UiPanels::texture(world::Entity e) const {
    for (const Panel& p : panels_) {
        if (p.entity == e) return p.target;
    }
    return {};
}

void UiPanels::set_data(const char* name, int type, f64 number, const char* text) {
    for (Panel& p : panels_) {
        if (type == 0) p.context->set_data(name, number);
        else if (type == 2) p.context->set_data(name, number != 0.0);
        else if (type == 3) p.context->set_data_json(name, text);
        else p.context->set_data(name, text);
    }
}

bool UiPanels::process_event(const SDL_Event& event, const world::World& world, const Mat4& view_proj, const f32 viewport[4]) {
    f32 mx, my;
    if (event.type == SDL_EVENT_MOUSE_MOTION) mx = event.motion.x, my = event.motion.y;
    else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) mx = event.button.x, my = event.button.y;
    else return false;
    if (panels_.empty() || viewport[2] < 1.0f || viewport[3] < 1.0f) return false;
    // The cursor's ray: two depths unprojected (either depth convention puts both on it).
    const Mat4 inv = inverse(view_proj);
    const f32 sx = (mx - viewport[0]) / viewport[2] * 2.0f - 1.0f, sy = 1.0f - (my - viewport[1]) / viewport[3] * 2.0f;
    auto unproject = [&](f32 z) {
        const f32 x = inv.at(0, 0) * sx + inv.at(0, 1) * sy + inv.at(0, 2) * z + inv.at(0, 3);
        const f32 y = inv.at(1, 0) * sx + inv.at(1, 1) * sy + inv.at(1, 2) * z + inv.at(1, 3);
        const f32 zz = inv.at(2, 0) * sx + inv.at(2, 1) * sy + inv.at(2, 2) * z + inv.at(2, 3);
        const f32 w = inv.at(3, 0) * sx + inv.at(3, 1) * sy + inv.at(3, 2) * z + inv.at(3, 3);
        return Vec3{x / w, y / w, zz / w};
    };
    const Vec3 from = unproject(0.1f), to = unproject(0.9f);
    Panel* best = nullptr;
    f32 best_t = 1e30f, best_x = 0.0f, best_y = 0.0f;
    for (Panel& p : panels_) {
        const auto* c = world.get<world::UiPanel>(p.entity);
        if (!c || !c->clickable || c->width <= 0.0f || c->height <= 0.0f) continue;
        const Mat4 local = affine_inverse(world.world_matrix(p.entity));
        const Vec3 o = transform_point(local, from), d = transform_point(local, to) - o;
        if (std::fabs(d.y) < 1e-6f) continue;
        const f32 t = -o.y / d.y;  // where the ray crosses the panel's plane (y = 0)
        if (t <= 0.0f || t >= best_t) continue;
        const f32 u = (o.x + d.x * t) / c->width + 0.5f, v = 0.5f - (o.z + d.z * t) / c->height;
        if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) continue;
        best = &p;
        best_t = t;
        best_x = u * static_cast<f32>(p.width);
        best_y = v * static_cast<f32>(p.height);
    }
    for (Panel& p : panels_) {  // the one the cursor left: off its elements
        if (&p == best || !p.hovered) continue;
        SDL_Event away = event;
        away.type = SDL_EVENT_MOUSE_MOTION;
        away.motion.x = away.motion.y = -1.0f;
        p.context->process_event(away);
        p.hovered = false;
    }
    if (!best) return false;
    SDL_Event at = event;
    if (event.type == SDL_EVENT_MOUSE_MOTION) at.motion.x = best_x, at.motion.y = best_y;
    else at.button.x = best_x, at.button.y = best_y;
    best->hovered = true;
    return best->context->process_event(at);
}

} // namespace lsd::editor
