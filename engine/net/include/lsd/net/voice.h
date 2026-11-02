#pragma once
// Voice chat codec (Opus): 20 ms frames of 48 kHz mono audio to small packets and back. A lost packet
// is concealed (decode with no data) instead of clicking.
#include "lsd/foundation/types.h"

namespace lsd::net {

constexpr u32 kVoiceRate = 48000;
constexpr u32 kVoiceFrame = 960;           // samples in 20 ms
constexpr u32 kMaxVoicePacket = 400;       // bytes; Opus at the voice bitrate stays far below

class VoiceEncoder {
public:
    explicit VoiceEncoder(u32 bitrate = 24000);
    ~VoiceEncoder();
    VoiceEncoder(const VoiceEncoder&) = delete;
    VoiceEncoder& operator=(const VoiceEncoder&) = delete;
    bool ok() const { return state_ != nullptr; }
    // One frame (kVoiceFrame samples, -1..1) into `out`; the packet's size, 0 on failure.
    u32 encode(const f32* frame, u8* out, u32 capacity);

private:
    void* state_ = nullptr;
};

class VoiceDecoder {
public:
    VoiceDecoder();
    ~VoiceDecoder();
    VoiceDecoder(const VoiceDecoder&) = delete;
    VoiceDecoder& operator=(const VoiceDecoder&) = delete;
    bool ok() const { return state_ != nullptr; }
    // A packet (or nullptr / 0 bytes: a lost one, concealed) into kVoiceFrame samples; false on failure.
    bool decode(const u8* packet, u32 bytes, f32* frame);

private:
    void* state_ = nullptr;
};

} // namespace lsd::net
