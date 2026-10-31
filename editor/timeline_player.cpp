#include "timeline_player.h"

#include "lsd/world/components.h"

#include <cstdio>
#include <cstring>

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

// "PointLight.intensity" -> the field's address, if it is an f32 on a component e has.
f32* number_field(const world::World& w, world::Entity e, const char* path) {
    const char* dot = path ? std::strchr(path, '.') : nullptr;
    if (!dot) return nullptr;
    char type_name[64];
    std::snprintf(type_name, sizeof type_name, "%.*s", static_cast<int>(dot - path), path);
    const reflect::TypeInfo* type = world::find_component_type(type_name);
    void* component = type ? const_cast<world::World&>(w).get(e, *type) : nullptr;
    if (!component) return nullptr;
    for (u32 i = 0; i < type->property_count; ++i) {
        const reflect::Property& p = type->properties[i];
        if (p.type == reflect::FieldType::F32 && std::strcmp(p.name, dot + 1) == 0) return reinterpret_cast<f32*>(static_cast<u8*>(component) + p.offset);
    }
    return nullptr;
}

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
        } else {
            f32 value;
            if (world::sample_value(t, i, time, value)) set_number_property(w, e, track.property, value);
        }
    }
}

} // namespace lsd::editor
