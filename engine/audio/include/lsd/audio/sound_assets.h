#pragma once
// Sound data assets, edited in Details like Unreal's: Sound Class (a volume tree: Master -> SFX ->
// Footsteps), Sound Attenuation (how a sound fades with distance) and Sound Concurrency (how many
// may play at once). A sound file's own settings live in its .meta (asset/asset_meta.h).
#include "lsd/reflect/reflect.h"

namespace lsd::audio {

LSD_STRUCT(Category = "Audio")
struct SoundClass {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:SoundClass", Tooltip = "Its volume and pitch multiply into this one's; none = under Master") char parent[33] = "";
    LSD_PROPERTY(Edit, Min = 0, Max = 4) f32 volume = 1.0f;
    LSD_PROPERTY(Edit, Min = 0.1, Max = 4) f32 pitch = 1.0f;
};

LSD_STRUCT(Category = "Audio")
struct SoundAttenuation {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Off: plays at the same volume everywhere, centred (UI, music)") bool spatialize = true;
    LSD_PROPERTY(Edit, Widget = "Choice:Inverse,Linear,Exponential", Tooltip = "Falloff shape between Min and Max Distance") u32 falloff = 0;
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Tooltip = "Full volume inside this distance") f32 min_distance = 2.0f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Tooltip = "No further fading beyond this distance") f32 max_distance = 40.0f;
    LSD_PROPERTY(Edit, Min = 0, Max = 10, Tooltip = "How fast it fades (Inverse and Exponential)") f32 rolloff = 1.0f;
};

LSD_STRUCT(Category = "Audio")
struct SoundConcurrency {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Min = 1, Max = 128) u32 max_voices = 8;
    LSD_PROPERTY(Edit, Widget = "Choice:Stop Oldest,Stop Quietest,Refuse New", Tooltip = "What a new sound does when the group is full") u32 when_full = 0;
};

LSD_STRUCT()
struct SoundMixAdjuster {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:SoundClass", Tooltip = "Its children follow (Master > SFX > Footsteps)") char sound_class[33] = "";
    LSD_PROPERTY(Edit, Min = 0, Max = 4, Tooltip = "Multiplies the class volume while the mix is pushed; 0.5 is about -6 dB") f32 volume = 0.5f;
};

// A temporary balance change (Unreal's Sound Mix): push it when something happens (a pause menu
// muffles the game, dialogue ducks the music), pop it when it ends.
LSD_STRUCT(Category = "Audio")
struct SoundMix {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) List<SoundMixAdjuster, 16> adjusters;
    LSD_PROPERTY(Edit, Units = "s", Min = 0) f32 fade_in = 0.2f;
    LSD_PROPERTY(Edit, Units = "s", Min = 0) f32 fade_out = 0.5f;
};

} // namespace lsd::audio
