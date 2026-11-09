#pragma once
// Sound data assets, edited in Details like Unreal's: Sound Class (a volume tree: Master -> SFX ->
// Footsteps), Sound Attenuation (how a sound fades with distance) and Sound Concurrency (how many
// may play at once). A sound file's own settings live in its .meta (asset/asset_meta.h).
#include "lsd/reflect/reflect.h"

namespace lsd::audio {

// One effect in a Submix. Each effect uses the fields its tooltip names.
LSD_STRUCT()
struct SubmixEffect {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Choice:Reverb,Low Pass,High Pass,EQ Band,Echo,Compressor", Tooltip = "The effect: Reverb (rooms), Low / High Pass (muffle / thin), EQ Band, Echo, Compressor (evens loud and quiet)") u32 type = 0;
    LSD_PROPERTY(Edit, Units = "Hz", Min = 20, Max = 20000, Tooltip = "Low Pass / High Pass: cutoff. EQ Band: centre") f32 frequency = 1000.0f;
    LSD_PROPERTY(Edit, Units = "dB", Min = -24, Max = 24, Tooltip = "EQ Band: boost or cut. Compressor: make-up gain") f32 gain_db = 0.0f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "Reverb and Echo: how much of the effect is heard") f32 amount = 0.3f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "Reverb: room size. Echo: how much each repeat keeps") f32 size = 0.5f;
    LSD_PROPERTY(Edit, Units = "s", Min = 0.01, Max = 2, Tooltip = "Echo: time between repeats. Compressor: release") f32 time = 0.3f;
    LSD_PROPERTY(Edit, Units = "dB", Min = -60, Max = 0, Tooltip = "Compressor: level where it starts working") f32 threshold_db = -12.0f;
    LSD_PROPERTY(Edit, Min = 1, Max = 20, Tooltip = "Compressor: 4 means 4 dB over the threshold comes out as 1 dB over") f32 ratio = 4.0f;
};

// An effect chain a Sound Class runs through (Unreal's Submix): reverb for caves, a radio EQ for
// voices, a compressor on music. The chain sits between the class and its parent.
LSD_STRUCT(Category = "Audio")
struct Submix {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Effects applied, in order, to every sound sent to this submix") List<SubmixEffect, 8> effects;
    LSD_PROPERTY(Edit, Min = 0, Max = 4, Tooltip = "Level after the effects") f32 volume = 1.0f;
};

LSD_STRUCT(Category = "Audio")
struct SoundClass {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:SoundClass", Tooltip = "Its volume and pitch multiply into this one's; none = under Master") char parent[33] = "";
    LSD_PROPERTY(Edit, Min = 0, Max = 4, Tooltip = "Loudness of every sound in this class (and its children)") f32 volume = 1.0f;
    LSD_PROPERTY(Edit, Min = 0.1, Max = 4, Tooltip = "Pitch of every sound in this class") f32 pitch = 1.0f;
    LSD_PROPERTY(Edit, Widget = "Asset:Submix", Tooltip = "Effects this class's sound passes through") char submix[33] = "";
};

LSD_STRUCT(Category = "Audio")
struct SoundAttenuation {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Off: plays at the same volume everywhere, centred (UI, music)") bool spatialize = true;
    LSD_PROPERTY(Edit, Widget = "Choice:Inverse,Linear,Exponential", Tooltip = "Falloff shape between Min and Max Distance") u32 falloff = 0;
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Tooltip = "Full volume inside this distance") f32 min_distance = 2.0f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Tooltip = "No further fading beyond this distance") f32 max_distance = 40.0f;
    LSD_PROPERTY(Edit, Min = 0, Max = 10, Tooltip = "How fast it fades (Inverse and Exponential)") f32 rolloff = 1.0f;
    LSD_PROPERTY(Edit, Category = "Occlusion", Tooltip = "Muffled when something solid is between it and the listener (Unreal's Enable Occlusion)")
    bool occlusion = false;
    LSD_PROPERTY(Edit, Category = "Occlusion", Units = "Hz", Min = 100, Max = 20000, Tooltip = "Low-pass while blocked") f32 occlusion_muffle = 1200.0f;
    LSD_PROPERTY(Edit, Category = "Air Absorption", Tooltip = "High frequencies fade with distance, as in air: far sounds are duller (Unreal's Enable Air Absorption)")
    bool air_absorption = false;
    LSD_PROPERTY(Edit, Category = "Air Absorption", Units = "Hz", Min = 100, Max = 20000, Tooltip = "Low-pass cutoff at Max Distance; open at Min Distance")
    f32 air_absorption_far = 3000.0f;
    LSD_PROPERTY(Edit, Category = "Reverb", Tooltip = "Sends more of it to a reverb the farther it is, so distant sounds feel far away (Unreal's Reverb Send)")
    bool reverb_send = false;
    LSD_PROPERTY(Edit, Category = "Reverb", Min = 0, Max = 1, Tooltip = "Reverb send at Max Distance; none at Min Distance") f32 reverb_send_far = 0.5f;
};

LSD_STRUCT(Category = "Audio")
struct SoundConcurrency {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Min = 1, Max = 128, Tooltip = "How many sounds of this group may play at once") u32 max_voices = 8;
    LSD_PROPERTY(Edit, Widget = "Choice:Stop Oldest,Stop Quietest,Refuse New", Tooltip = "What a new sound does when the group is full") u32 when_full = 0;
};

LSD_STRUCT()
struct SoundMixAdjuster {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:SoundClass", Tooltip = "Its children follow (Master > SFX > Footsteps)") char sound_class[33] = "";
    LSD_PROPERTY(Edit, Min = 0, Max = 4, Tooltip = "Multiplies the class volume while the mix is pushed; 0.5 is about -6 dB") f32 volume = 0.5f;
    LSD_PROPERTY(Edit, Units = "Hz", Min = 100, Max = 20000, Tooltip = "Muffles the class (and its children) while the mix is pushed (a pause menu, underwater); 20000 = not")
    f32 lowpass = 20000.0f;
};

// A temporary balance change (Unreal's Sound Mix): push it when something happens (a pause menu
// muffles the game, dialogue ducks the music), pop it when it ends.
LSD_STRUCT(Category = "Audio")
struct SoundMix {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Sound Classes this mix changes (volume, pitch, low-pass) while it is pushed") List<SoundMixAdjuster, 16> adjusters;
    LSD_PROPERTY(Edit, Units = "s", Min = 0, Tooltip = "Time to blend the mix in when pushed") f32 fade_in = 0.2f;
    LSD_PROPERTY(Edit, Units = "s", Min = 0, Tooltip = "Time to blend it out when popped") f32 fade_out = 0.5f;
};

} // namespace lsd::audio
