#pragma once
// Timelines (Unreal's Sequencer, the basics): a data asset of tracks over time, edited in the
// Timeline window and played with lsd.play_timeline. Tracks name their object in the level:
//   Transform   keys hold a position and rotation (the object's own Transform); in between: smooth
//   Property    keys hold a number for "Component.field" (a light's intensity, a fog's density...)
//   Event       at each key, lsd.broadcast(key text, timeline name): call Lua / Custom Events
//   Sound       at each key, plays the sound named by the key text (at the object, if it has one)
//   Camera Cut  at each key, the view cuts to the Camera object named by the key text
#include "lsd/foundation/math.h"
#include "lsd/reflect/reflect.h"

namespace lsd::world {

enum class TrackType : u32 { Transform, Property, Event, Sound, CameraCut };

LSD_STRUCT()
struct TimelineTrack {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The object's name in the level (Camera Cut and Event tracks need none)") char object[64] = "";
    LSD_PROPERTY(Edit, Widget = "Choice:Transform,Property,Event,Sound,Camera Cut", Tooltip = "What the track animates: an object's Transform, a Property, Events (script calls), Sounds, or Camera Cuts") u32 type = 0;
    LSD_PROPERTY(Edit, Tooltip = "Property tracks: Component.field, e.g. PointLight.intensity") char property[64] = "";
};

LSD_STRUCT()
struct TimelineKey {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The track the key belongs to") u32 track = 0;
    LSD_PROPERTY(Edit, Units = "s", Min = 0, Tooltip = "When the key is") f32 time = 0.0f;
    LSD_PROPERTY(Edit, Units = "m", Tooltip = "Transform tracks: where the object is at this key") Vec3 position;
    LSD_PROPERTY(Edit, Units = "deg", Tooltip = "Transform tracks: how it is turned at this key") Vec3 rotation;
    LSD_PROPERTY(Edit, Tooltip = "Property tracks: the value at this key") f32 value = 0.0f;
    LSD_PROPERTY(Edit, Tooltip = "Event: the function to call; Sound: the sound; Camera Cut: the camera object") char text[48] = "";
};

LSD_STRUCT(Category = "Data")
struct Timeline {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "s", Min = 0.1, Tooltip = "How long the timeline plays") f32 length = 5.0f;
    LSD_PROPERTY(Edit, Tooltip = "What it animates; each track drives one object") List<TimelineTrack, 16> tracks;
    LSD_PROPERTY(Edit, Tooltip = "Every track's keys, any order") List<TimelineKey, 512> keys;
};

// A Transform track at `time`: eased between the keys around it, held before the first and after the
// last. False if the track has no keys.
bool sample_transform(const Timeline& timeline, u32 track, f32 time, Vec3& position, Vec3& rotation);
// A Property track's value at `time` (same easing); false if it has no keys.
bool sample_value(const Timeline& timeline, u32 track, f32 time, f32& value);
// Keys of `track` with from < time <= to (Event, Sound, Camera Cut fire them), in time order; how many.
u32 keys_crossed(const Timeline& timeline, u32 track, f32 from, f32 to, const TimelineKey** out, u32 capacity);

} // namespace lsd::world
