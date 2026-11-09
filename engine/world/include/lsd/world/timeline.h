#pragma once
// Timelines (Unreal's Sequencer, the basics): a data asset of tracks over time, edited in the
// Timeline window and played with lsd.play_timeline. Tracks name their object in the level:
//   Transform   keys hold a position and rotation (the object's own Transform); in between: smooth
//   Property    keys hold a number or a colour for "Component.field" (a light's intensity, its colour...)
//   Event       at each key, lsd.broadcast(key text, timeline name): call Lua / Custom Events
//   Sound       at each key, plays the sound named by the key text (at the object, if it has one)
//   Camera Cut  at each key, the view cuts to the Camera object named by the key text
//   Shot        each key starts a shot (Unreal's Shot track): its camera (key text) until the next shot,
//               and optionally a sub-sequence (another Timeline) played from the shot's start
// Keys interpolate Auto (cubic, automatic tangents), Linear or Constant to the next key.
#include "lsd/foundation/math.h"
#include "lsd/reflect/color.h"
#include "lsd/reflect/reflect.h"

namespace lsd::world {

enum class TrackType : u32 { Transform, Property, Event, Sound, CameraCut, Shot };

LSD_STRUCT()
struct TimelineTrack {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The object's name in the level (Camera Cut and Event tracks need none)") char object[64] = "";
    LSD_PROPERTY(Edit, Widget = "Choice:Transform,Property,Event,Sound,Camera Cut,Shot", Tooltip = "What the track animates: an object's Transform, a Property, Events (script calls), Sounds, Camera Cuts, or Shots (a camera and a time range, optionally a sub-sequence)") u32 type = 0;
    LSD_PROPERTY(Edit, Tooltip = "Property tracks: Component.field, a number or a colour, e.g. PointLight.intensity or PointLight.color") char property[64] = "";
};

LSD_STRUCT()
struct TimelineKey {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The track the key belongs to") u32 track = 0;
    LSD_PROPERTY(Edit, Units = "s", Min = 0, Tooltip = "When the key is") f32 time = 0.0f;
    LSD_PROPERTY(Edit, Units = "m", Tooltip = "Transform tracks: where the object is at this key") Vec3 position;
    LSD_PROPERTY(Edit, Units = "deg", Tooltip = "Transform tracks: how it is turned at this key") Vec3 rotation;
    LSD_PROPERTY(Edit, Tooltip = "Property tracks: the value at this key") f32 value = 0.0f;
    LSD_PROPERTY(Edit, Tooltip = "Property tracks on a colour: the colour at this key") LinearColor color;
    LSD_PROPERTY(Edit, Widget = "Choice:Auto,Linear,Constant", Tooltip = "How the values go from this key to the next: Auto (a smooth cubic curve with automatic tangents), Linear, or Constant (a step at the next key)") u32 interp = 0;
    LSD_PROPERTY(Edit, Tooltip = "Event: the function to call; Sound: the sound; Camera Cut and Shot: the camera object") char text[48] = "";
    LSD_PROPERTY(Edit, Tooltip = "Shot tracks: a Timeline played as this shot's sub-sequence (from its start when the shot starts)") char sequence[48] = "";
};

LSD_STRUCT(Category = "Data")
struct Timeline {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "s", Min = 0.1, Tooltip = "How long the timeline plays") f32 length = 5.0f;
    LSD_PROPERTY(Edit, Tooltip = "What it animates; each track drives one object") List<TimelineTrack, 16> tracks;
    LSD_PROPERTY(Edit, Tooltip = "Every track's keys, any order") List<TimelineKey, 512> keys;
};

// Channels of a key, for curves: 0-2 position, 3-5 rotation, 6 value, 7-10 colour (r, g, b, a).
constexpr u32 kKeyChannels = 11;
f32* key_channel(TimelineKey& key, u32 channel);
inline f32 key_channel(const TimelineKey& key, u32 channel) { return *key_channel(const_cast<TimelineKey&>(key), channel); }
// One channel of `track` at `time` through its keys (Auto / Linear / Constant); false if it has no keys.
bool sample_channel(const Timeline& timeline, u32 track, u32 channel, f32 time, f32& value);
bool sample_color(const Timeline& timeline, u32 track, f32 time, LinearColor& color);
// The Shot key playing at `time` (the last one at or before it on any Shot track), or null.
const TimelineKey* shot_at(const Timeline& timeline, f32 time);
// The camera object the view looks through at `time`: the last Camera Cut or Shot key at or before it
// with a camera; null before the first (the view stays the player's / the editor's).
const char* camera_at(const Timeline& timeline, f32 time);
// Frames a movie render writes at `fps` over `length` seconds (both ends included).
inline u32 movie_frame_count(f32 length, f32 fps) { return static_cast<u32>(length * fps + 1e-3f) + 1; }

// Cine Camera (Unreal's Cine Camera Actor): a lens from a film back and a focal length (millimetres).
f32 cine_fov_degrees(f32 sensor_height_mm, f32 focal_length_mm);  // vertical field of view
// Depth of field of a thin lens at f-stop `aperture` focused at `focus_m`, as the post pass takes it:
// metres either side of focus that stay sharp, metres to full blur, and the blur radius in pixels at
// an image `image_height` pixels high.
struct LensBlur {
    f32 focal_region = 0.0f;
    f32 transition = 0.0f;
    f32 max_blur = 0.0f;
};
LensBlur cine_depth_of_field(f32 focal_length_mm, f32 sensor_width_mm, f32 sensor_height_mm, f32 aperture, f32 focus_m, f32 image_height);

// A Transform track at `time`: eased between the keys around it, held before the first and after the
// last. False if the track has no keys.
bool sample_transform(const Timeline& timeline, u32 track, f32 time, Vec3& position, Vec3& rotation);
// A Property track's value at `time` (same easing); false if it has no keys.
bool sample_value(const Timeline& timeline, u32 track, f32 time, f32& value);
// Keys of `track` with from < time <= to (Event, Sound, Camera Cut fire them), in time order; how many.
u32 keys_crossed(const Timeline& timeline, u32 track, f32 from, f32 to, const TimelineKey** out, u32 capacity);

} // namespace lsd::world
