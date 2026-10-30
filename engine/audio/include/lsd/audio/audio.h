#pragma once
// Audio on miniaudio (hidden behind this header): voices playing sound files, a Sound Class tree
// of volume groups, 3D attenuation around one listener, and concurrency groups. World convention:
// Z up, metres. Without an audio device (servers, tests) everything still runs, silently, and
// read_frames() pulls the mix by hand.
#include "lsd/audio/sound_assets.h"
#include "lsd/foundation/math.h"

namespace lsd::audio {

constexpr u32 kMasterClass = 0;
constexpr u32 kNoGroup = 0xFFFFFFFFu;

struct PlayDesc {
    const char* path = nullptr;  // .wav .ogg .flac .mp3
    const char* name = "";       // shown in the audio overlay
    bool spatial = false;        // at `position`, faded by `attenuation`
    Vec3 position;
    SoundAttenuation attenuation;
    f32 volume = 1.0f;
    f32 pitch = 1.0f;
    bool looping = false;
    bool stream = false;         // read from disk while playing (music) instead of decoding into memory
    u32 sound_class = kMasterClass;
    u32 concurrency = kNoGroup;  // from add_concurrency
};

struct VoiceInfo {
    u32 id;
    const char* name;
    u32 sound_class;
    f32 volume;    // its own volume (before class and distance)
    f32 distance;  // from the listener; 0 when not spatial
    bool spatial;
};

class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    // Opens the default output device (or none, for tests). False if miniaudio could not start at all.
    bool init(bool use_device = true);
    void shutdown();
    bool has_device() const;

    // Sound Classes. Master (index 0) always exists; parents must be added before children.
    u32 add_class(const char* name, u32 parent, f32 volume, f32 pitch);
    void set_class_volume(u32 sound_class, f32 volume);  // settings sliders and mixes
    u32 class_count() const;
    const char* class_name(u32 sound_class) const;
    void reset_classes();  // back to only Master (stops every voice)

    u32 add_concurrency(u32 max_voices, u32 when_full);

    // Returns a voice id (0 if refused or the file could not be read).
    u32 play(const PlayDesc& desc);
    void stop(u32 voice);
    void stop_all();
    bool playing(u32 voice) const;
    void set_position(u32 voice, Vec3 position);
    void set_listener(Vec3 position, Vec3 forward);

    void update();  // frees finished voices
    u32 voice_count() const;
    VoiceInfo voice(u32 index) const;

    // Without a device: mixes `frames` stereo float frames into `out`. Returns the frames written.
    u32 read_frames(f32* out, u32 frames);

    static constexpr u32 kMaxVoices = 128;
    static constexpr u32 kMaxClasses = 64;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace lsd::audio
