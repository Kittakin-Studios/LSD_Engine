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
    f32 delay = 0.0f;            // seconds before it starts
    u32 instance = 0;            // a voice id this one belongs to (a Sound Cue's voices stop together)
};

struct MixAdjust {
    u32 sound_class;
    f32 volume;
    f32 lowpass = 20000.0f;  // Hz while the mix is in; 20000 = not muffled
};

struct VoiceInfo {
    u32 id;
    const char* name;
    u32 sound_class;
    f32 volume;    // its own volume (before class and distance)
    f32 distance;  // from the listener; 0 when not spatial
    bool spatial;
    Vec3 position;
    f32 lowpass;   // Hz; 20000 = open
    bool occlusion;      // its attenuation asks to be muffled behind walls
    f32 occlusion_muffle;  // Hz, while blocked
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

    // Sound Classes. Master (index 0) always exists; parents must be added before children. A class
    // may run through a Submix effect chain on its way to its parent.
    u32 add_class(const char* name, u32 parent, f32 volume, f32 pitch, const SubmixEffect* effects = nullptr, u32 effect_count = 0, f32 submix_volume = 1.0f);
    void set_class_volume(u32 sound_class, f32 volume);  // settings sliders and mixes
    u32 class_count() const;
    const char* class_name(u32 sound_class) const;
    void reset_classes();  // back to only Master (stops every voice)

    u32 add_concurrency(u32 max_voices, u32 when_full);

    // Returns a voice id (0 if refused or the file could not be read).
    u32 play(const PlayDesc& desc);
    void stop(u32 voice);  // and the voices that belong to it
    f32 length_seconds(const char* path);  // 0 if unreadable
    void stop_all();
    bool playing(u32 voice) const;  // it, or a voice that belongs to it
    void set_position(u32 voice, Vec3 position);
    void set_listener(Vec3 position, Vec3 forward);

    // The listener's surroundings (Audio Volumes): reverb on everything heard, and per-voice
    // muffling for sounds on the other side of a wall.
    void set_environment(f32 reverb_amount, f32 room_size);
    void set_voice_lowpass(u32 voice, f32 cutoff_hz);  // 20000 = open
    // Music: fade a voice to a volume (0..1 times its own) over some seconds; stop it after a fade;
    // where it is (seconds) and jump there, to start a layer in step with the track it joins.
    void fade_voice(u32 voice, f32 volume, f32 seconds);
    void stop_with_fade(u32 voice, f32 seconds);
    f32 voice_time(u32 voice);
    void seek_voice(u32 voice, f32 seconds);

    // Sound Mixes: class volumes multiplied while pushed, faded in and out. Returns a mix id.
    u32 push_mix(const MixAdjust* adjusters, u32 count, f32 fade_in, f32 fade_out);
    void pop_mix(u32 mix);
    u32 mix_count() const;

    void update(f32 dt = 0.0f);  // frees finished voices, advances mix fades
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

// A sound file's loudness over its length, for pictures (Content Browser thumbnails): the peak of
// each of `columns` equal slices, 0..1, all channels. False if the file cannot be decoded.
bool waveform(const char* path, u32 columns, f32* peaks);

} // namespace lsd::audio
