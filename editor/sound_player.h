#pragma once
// Plays sound assets the way their settings say: the .meta of the sound file (volume, pitch,
// looping, streaming, class, attenuation, concurrency) and the Sound Class / Attenuation /
// Concurrency data assets it names. Classes and concurrency groups are registered with the audio
// system the first time a sound uses them.
#include "lsd/asset/asset_registry.h"
#include "lsd/audio/audio.h"

namespace lsd::editor {

class SoundPlayer {
public:
    void bind(audio::AudioSystem* audio, const asset::AssetRegistry* registry) { audio_ = audio; registry_ = registry; }
    audio::AudioSystem* audio() const { return audio_; }

    // `sound` is an asset GUID or a sound asset's name. Spatial sounds play at `position` unless
    // their attenuation turns spatialisation off. Returns the voice (0 if none).
    u32 play(const char* sound, bool spatial, Vec3 position);
    void stop_all_and_forget();  // stops every voice and drops the registered classes

private:
    const asset::AssetRecord* find(const char* sound) const;
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
};

} // namespace lsd::editor
