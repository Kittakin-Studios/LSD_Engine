#pragma once
// Voice chat audio: the microphone (48 kHz mono, read as 20 ms frames) and a playback device that
// mixes each speaker's stream. Each speaker has its own small queue that starts once 60 ms is waiting
// and refills after running dry, so network jitter becomes a short delay instead of stutter.
// Device threads and the game thread share only lock-free rings.
#include "lsd/foundation/types.h"

namespace lsd::audio {

class VoiceIO {
public:
    static constexpr u32 kRate = 48000;
    static constexpr u32 kFrame = 960;  // 20 ms
    static constexpr u32 kMaxSpeakers = 16;

    VoiceIO() = default;
    ~VoiceIO();
    VoiceIO(const VoiceIO&) = delete;
    VoiceIO& operator=(const VoiceIO&) = delete;

    // Opens the playback device, and the default microphone when `capture` (it is only opened when
    // voice chat is on). False if no playback device works; capture failing only logs.
    // `null_devices` (tests): miniaudio's null backend, devices that run on a timer without hardware.
    bool start(bool capture, bool null_devices = false);
    void stop();
    bool capturing() const { return capture_ != nullptr; }

    // One 20 ms frame of microphone audio, oldest first; false when less than a frame is waiting.
    bool read_frame(f32* frame);
    // A decoded frame from `speaker` at `gain` (0..1: proximity voice fades with distance).
    void play(u32 speaker, const f32* frame, f32 gain);
    // How loud `speaker` is right now (0..1, for a "talking" icon).
    f32 level(u32 speaker) const;

    struct Impl;

private:
    Impl* impl_ = nullptr;
    void* capture_ = nullptr;
};

} // namespace lsd::audio
