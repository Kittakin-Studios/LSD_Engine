#pragma once
// Timelines on the level's objects: the poses (Transform and Property tracks) for both the Timeline
// window's scrubbing and Play; Play also fires the Event, Sound and Camera Cut keys (play_session).
#include "lsd/rhi/rhi.h"
#include "lsd/world/timeline.h"
#include "lsd/world/world.h"

namespace lsd::asset {
class AssetRegistry;
}
namespace lsd::world {
struct Camera;
}

namespace lsd::editor {

world::Entity find_named(const world::World& world, const char* name);  // invalid if none
// Sets "Component.field" (a number field) on e; false if there is no such number.
bool set_number_property(world::World& world, world::Entity e, const char* path, f32 value);
bool get_number_property(const world::World& world, world::Entity e, const char* path, f32& value);
// The same for a colour field ("PointLight.color"); is_color_property tells which a track animates.
bool set_color_property(world::World& world, world::Entity e, const char* path, const LinearColor& value);
bool get_color_property(const world::World& world, world::Entity e, const char* path, LinearColor& value);
bool is_color_property(const world::World& world, world::Entity e, const char* path);
// Transform and Property tracks at `time` onto their objects, and the playing shot's sub-sequence.
void pose_timeline(world::World& world, const world::Timeline& timeline, f32 time);

// Sub-sequences: Timelines found by name in this registry (loaded once, kept until forget_timelines).
void set_timeline_registry(const asset::AssetRegistry* registry);
const world::Timeline* find_timeline(const char* name);
void forget_timelines();  // after a Timeline was saved
// The camera object the view looks through at `time` (Camera Cuts and Shots, into sub-sequences); null: none.
const char* timeline_camera(const world::Timeline& timeline, f32 time);

// Cine Camera: the camera's vertical field of view in degrees (its lens, else Field of View), and its
// depth of field into the post pass (when it has an Aperture).
f32 camera_fov_degrees(const world::Camera& camera);
void cine_camera_post(const world::World& world, world::Entity camera, f32 image_height, rhi::PostDraw& post);

} // namespace lsd::editor
