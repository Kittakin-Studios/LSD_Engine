#pragma once
// Timelines on the level's objects: the poses (Transform and Property tracks) for both the Timeline
// window's scrubbing and Play; Play also fires the Event, Sound and Camera Cut keys (play_session).
#include "lsd/world/timeline.h"
#include "lsd/world/world.h"

namespace lsd::editor {

world::Entity find_named(const world::World& world, const char* name);  // invalid if none
// Sets "Component.field" (a number field) on e; false if there is no such number.
bool set_number_property(world::World& world, world::Entity e, const char* path, f32 value);
bool get_number_property(const world::World& world, world::Entity e, const char* path, f32& value);
// Transform and Property tracks at `time` onto their objects.
void pose_timeline(world::World& world, const world::Timeline& timeline, f32 time);

} // namespace lsd::editor
