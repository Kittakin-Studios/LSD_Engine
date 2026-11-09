#include "timeline_player.h"

#include "lsd/asset/asset_registry.h"
#include "lsd/asset/data_asset.h"
#include "lsd/foundation/log.h"
#include "lsd/world/components.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>

namespace lsd::editor {

world::Entity find_named(const world::World& w, const char* name) {
    if (!name || !name[0]) return {};
    for (u32 slot = 0; slot < w.slot_count(); ++slot) {
        const world::Entity e = w.entity_at_slot(slot);
        if (e.valid() && std::strcmp(w.name(e), name) == 0) return e;
    }
    return {};
}

namespace {

// "PointLight.intensity" -> the field's address, if it is an f32 (`color`: a LinearColor) on a component e has.
void* property_field(const world::World& w, world::Entity e, const char* path, bool color) {
    const char* dot = path ? std::strchr(path, '.') : nullptr;
    if (!dot) return nullptr;
    char type_name[64];
    std::snprintf(type_name, sizeof type_name, "%.*s", static_cast<int>(dot - path), path);
    const reflect::TypeInfo* type = world::find_component_type(type_name);
    void* component = type ? const_cast<world::World&>(w).get(e, *type) : nullptr;
    if (!component) return nullptr;
    for (u32 i = 0; i < type->property_count; ++i) {
        const reflect::Property& p = type->properties[i];
        if (std::strcmp(p.name, dot + 1) != 0) continue;
        const bool is_color = p.type == reflect::FieldType::Struct && p.struct_type == &reflect::type_of<LinearColor>();
        if (color ? is_color : p.type == reflect::FieldType::F32) return static_cast<u8*>(component) + p.offset;
    }
    return nullptr;
}

f32* number_field(const world::World& w, world::Entity e, const char* path) { return static_cast<f32*>(property_field(w, e, path, false)); }
LinearColor* color_field(const world::World& w, world::Entity e, const char* path) { return static_cast<LinearColor*>(property_field(w, e, path, true)); }

const asset::AssetRegistry* g_registry = nullptr;
// shortcut: an STL map of loaded sub-sequences (Play looks one up per frame per shot); a handle table if many shots play at once.
std::map<std::string, std::unique_ptr<world::Timeline>> g_timelines;
u32 g_pose_depth = 0;  // sub-sequences inside sub-sequences, and loops of them, stop here

} // namespace

bool set_number_property(world::World& w, world::Entity e, const char* path, f32 value) {
    f32* field = number_field(w, e, path);
    if (field) *field = value;
    return field != nullptr;
}

bool get_number_property(const world::World& w, world::Entity e, const char* path, f32& value) {
    const f32* field = number_field(w, e, path);
    if (field) value = *field;
    return field != nullptr;
}

bool set_color_property(world::World& w, world::Entity e, const char* path, const LinearColor& value) {
    LinearColor* field = color_field(w, e, path);
    if (field) *field = value;
    return field != nullptr;
}

bool get_color_property(const world::World& w, world::Entity e, const char* path, LinearColor& value) {
    const LinearColor* field = color_field(w, e, path);
    if (field) value = *field;
    return field != nullptr;
}

bool is_color_property(const world::World& w, world::Entity e, const char* path) { return color_field(w, e, path) != nullptr; }

void set_timeline_registry(const asset::AssetRegistry* registry) {
    g_registry = registry;
    forget_timelines();
}

void forget_timelines() { g_timelines.clear(); }

const world::Timeline* find_timeline(const char* name) {
    if (!name || !name[0] || !g_registry) return nullptr;
    auto found = g_timelines.find(name);
    if (found != g_timelines.end()) return found->second.get();
    std::unique_ptr<world::Timeline> loaded;  // stays null when missing: no retry every frame
    usize hits[16];
    const usize n = g_registry->search(name, asset::AssetType::Data, nullptr, hits, 16);
    for (usize i = 0; i < n && !loaded; ++i) {
        const asset::AssetRecord& r = g_registry->get(hits[i]);
        if (std::strcmp(r.name, name) != 0 || std::strcmp(r.data_type, "Timeline") != 0) continue;
        char path[300];
        g_registry->source_path(r, path, sizeof path);
        loaded = std::make_unique<world::Timeline>();
        if (!asset::load_data_asset(path, *loaded)) loaded.reset();
    }
    if (!loaded) LSD_LOG_WARN("timeline", "Sub-sequence: no Timeline named '%s'", name);
    return (g_timelines[name] = std::move(loaded)).get();
}

const char* timeline_camera(const world::Timeline& t, f32 time) {
    const char* camera = world::camera_at(t, time);
    const world::TimelineKey* shot = world::shot_at(t, time);
    // A shot without a camera of its own looks through its sub-sequence's cuts (over this timeline's).
    if (shot && !shot->text[0]) {
        const world::Timeline* sub = find_timeline(shot->sequence);
        const char* inner = sub && sub != &t ? world::camera_at(*sub, time - shot->time) : nullptr;
        if (inner) return inner;
    }
    return camera;
}

f32 camera_fov_degrees(const world::Camera& c) { return c.focal_length > 0.0f ? world::cine_fov_degrees(c.sensor_height, c.focal_length) : c.fov; }

void cine_camera_post(const world::World& w, world::Entity camera, f32 image_height, rhi::PostDraw& post) {
    const auto* c = w.alive(camera) ? w.get<world::Camera>(camera) : nullptr;
    if (!c || c->aperture <= 0.0f || c->focal_length <= 0.0f) return;
    f32 focus = c->focus_distance;
    const world::Entity target = find_named(w, c->focus_object);
    if (w.alive(target)) {  // tracked focus: the object's distance along the view
        const Mat4 a = w.world_matrix(camera), b = w.world_matrix(target);
        const Vec3 forward = normalize(Vec3{a.at(0, 0), a.at(1, 0), a.at(2, 0)});
        focus = std::fmax(dot(Vec3{b.at(0, 3) - a.at(0, 3), b.at(1, 3) - a.at(1, 3), b.at(2, 3) - a.at(2, 3)}, forward), 0.05f);
    }
    const world::LensBlur lens = world::cine_depth_of_field(c->focal_length, c->sensor_width, c->sensor_height, c->aperture, focus, image_height);
    post.dof_focal_distance = focus;
    post.dof_focal_region = lens.focal_region;
    post.dof_transition = lens.transition;
    post.dof_max_blur = lens.max_blur;
}

void pose_timeline(world::World& w, const world::Timeline& t, f32 time) {
    for (u32 i = 0; i < t.tracks.count; ++i) {
        const world::TimelineTrack& track = t.tracks[i];
        const auto type = static_cast<world::TrackType>(track.type);
        if (type != world::TrackType::Transform && type != world::TrackType::Property) continue;
        const world::Entity e = find_named(w, track.object);
        if (!w.alive(e)) continue;
        if (type == world::TrackType::Transform) {
            Vec3 position, rotation;
            auto* transform = w.get<world::Transform>(e);
            if (transform && world::sample_transform(t, i, time, position, rotation)) {
                transform->position = position;
                transform->rotation = rotation;
            }
        } else if (is_color_property(w, e, track.property)) {
            LinearColor color;
            if (world::sample_color(t, i, time, color)) set_color_property(w, e, track.property, color);
        } else {
            f32 value;
            if (world::sample_value(t, i, time, value)) set_number_property(w, e, track.property, value);
        }
    }
    // The playing shot's sub-sequence, from the shot's start.
    const world::TimelineKey* shot = world::shot_at(t, time);
    const world::Timeline* sub = shot ? find_timeline(shot->sequence) : nullptr;
    if (sub && sub != &t && g_pose_depth < 4) {
        ++g_pose_depth;
        pose_timeline(w, *sub, time - shot->time);
        --g_pose_depth;
    }
}

} // namespace lsd::editor
