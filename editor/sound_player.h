#pragma once
// Plays sound assets the way their settings say: the .meta of the sound file (volume, pitch,
// looping, streaming, class, attenuation, concurrency) and the Sound Class / Attenuation /
// Concurrency data assets it names. Classes and concurrency groups are registered with the audio
// system the first time a sound uses them.
#include "lsd/asset/asset_registry.h"
#include "lsd/audio/audio.h"
#include "sound_cue.h"

namespace lsd::editor {

class SoundPlayer {
public:
    void bind(audio::AudioSystem* audio, const asset::AssetRegistry* registry) { audio_ = audio; registry_ = registry; }
    audio::AudioSystem* audio() const { return audio_; }

    // `sound` is an asset GUID or the name of a sound or Sound Cue. Spatial sounds play at `position`
    // unless their attenuation turns spatialisation off. Returns the voice (a cue's voices share it; 0 if none).
    u32 play(const char* sound, bool spatial, Vec3 position);

    // Sound Mixes by asset name; pop removes the latest push of that mix.
    void push_mix(const char* name);
    void pop_mix(const char* name);
    void set_param(const char* name, f32 value);
    void set_class_volume(const char* name, f32 volume);  // a Sound Class by asset name (settings sliders)
    f32 param(const char* name) const;
    void stop_all_and_forget();  // stops every voice and drops the registered classes

private:
    const asset::AssetRecord* find(const char* sound, bool cues = true) const;
    u32 play_cue(const asset::AssetRecord& cue, bool spatial, Vec3 position);
    u32 play_file(const asset::AssetRecord& wave, const asset::AssetMeta* cue_settings, bool spatial, Vec3 position, f32 volume, f32 pitch, bool looping, f32 delay, u32 instance);
    u32 class_index(const char* guid, int depth = 0);
    u32 concurrency_index(const char* guid);

    audio::AudioSystem* audio_ = nullptr;
    const asset::AssetRegistry* registry_ = nullptr;
    struct Known {
        char guid[33];
        u32 index;
    };
    Known classes_[64];
    u32 class_count_ = 0;
    Known groups_[64];
    u32 group_count_ = 0;
    struct Pushed {
        char name[64];
        u32 id;
    };
    Pushed mixes_[16];
    u32 mix_count_ = 0;
    struct Param {
        char name[32];
        f32 value;
    };
    Param params_[32];
    u32 param_count_ = 0;
    CueEnv cue_env_;
};

} // namespace lsd::editor
