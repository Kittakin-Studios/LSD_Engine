#pragma once
// Plays sound assets the way their settings say: the .meta of the sound file (volume, pitch,
// looping, streaming, class, attenuation, concurrency) and the Sound Class / Attenuation /
// Concurrency data assets it names. Classes and concurrency groups are registered with the audio
// system the first time a sound uses them.
#include "lsd/asset/asset_registry.h"
#include "lsd/audio/audio.h"
#include "lsd/audio/sound_assets.h"
#include "sound_cue.h"

namespace lsd::editor {

class SoundPlayer {
public:
    void bind(audio::AudioSystem* audio, const asset::AssetRegistry* registry) { audio_ = audio; registry_ = registry; }
    audio::AudioSystem* audio() const { return audio_; }

    // `sound` is an asset GUID or the name of a sound or Sound Cue. Spatial sounds play at `position`
    // unless their attenuation turns spatialisation off. Returns the voice (a cue's voices share it; 0 if none).
    u32 play(const char* sound, bool spatial, Vec3 position, bool loop = false);  // loop: sound files (cues have a Looping node)

    // Sound Mixes by asset name; pop removes the latest push of that mix.
    void push_mix(const char* name);
    void pop_mix(const char* name);
    void set_param(const char* name, f32 value);  // Sound Cue Switches and playing Sound Synths
    void trigger(const char* name);               // Sound Synth Trigger nodes of that name fire
    void set_class_volume(const char* name, f32 volume, bool warn = true);  // a Sound Class by asset name (settings sliders)
    f32 param(const char* name) const;
    void stop_all_and_forget();  // stops every voice and drops the registered classes

    // Music (one track at a time): play crossfades from the current track over `fade` seconds;
    // layers play in step with the track (stems: drums, strings...) and fade to a volume by game
    // state; stop fades everything out. A track that is already playing is left alone.
    void play_music(const char* name, f32 fade);
    void stop_music(f32 fade);
    void set_music_layer(const char* name, f32 volume, f32 fade);
    // A Music asset (play_music with its name): stems follow the intensity (0..1); a segment switch
    // waits for the next beat or bar on the mixer's clock.
    void set_music_intensity(f32 intensity);
    void set_music_segment(const char* name);

    // Subtitles: the Subtitle of the latest sound (or cue) that had one, while it still plays; "" if none.
    const char* subtitle();

private:
    const asset::AssetRecord* find(const char* sound, bool cues = true) const;
    u32 play_cue(const asset::AssetRecord& cue, bool spatial, Vec3 position);
    u32 play_synth(const asset::AssetRecord& synth, const char* path, bool spatial, Vec3 position);
    const asset::AssetRecord* find_data(const char* name, const char* data_type) const;
    void start_segment(u32 segment, f32 delay, f32 fade_in);
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
    char music_[64] = "";
    u32 music_voice_ = 0;
    struct Layer {
        char name[64];
        u32 voice;
    };
    Layer layers_[8];
    u32 layer_count_ = 0;
    struct MusicPlay {
        bool active = false;
        audio::Music asset;
        u32 segment = 0;
        f32 start = 0.0f;  // mixer time the segment's beat grid began
        f32 intensity = 0.0f;
        u32 base = 0;      // the segment's track
        u32 stems[16] = {};  // by stem index; 0 = not in this segment
    };
    MusicPlay music_play_;
    char subtitle_[128] = "";
    u32 subtitle_voice_ = 0;
};

} // namespace lsd::editor
