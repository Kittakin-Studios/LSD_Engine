#include "lsd/net/voice.h"

#include <opus.h>

namespace lsd::net {

VoiceEncoder::VoiceEncoder(u32 bitrate) {
    int error = 0;
    OpusEncoder* e = opus_encoder_create(kVoiceRate, 1, OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK) return;
    opus_encoder_ctl(e, OPUS_SET_BITRATE(static_cast<opus_int32>(bitrate)));
    opus_encoder_ctl(e, OPUS_SET_INBAND_FEC(1));         // a little redundancy: single lost packets come back
    opus_encoder_ctl(e, OPUS_SET_PACKET_LOSS_PERC(10));
    state_ = e;
}

VoiceEncoder::~VoiceEncoder() { opus_encoder_destroy(static_cast<OpusEncoder*>(state_)); }

u32 VoiceEncoder::encode(const f32* frame, u8* out, u32 capacity) {
    if (!state_) return 0;
    const opus_int32 n = opus_encode_float(static_cast<OpusEncoder*>(state_), frame, kVoiceFrame, out, static_cast<opus_int32>(capacity));
    return n > 0 ? static_cast<u32>(n) : 0;
}

VoiceDecoder::VoiceDecoder() {
    int error = 0;
    OpusDecoder* d = opus_decoder_create(kVoiceRate, 1, &error);
    if (error == OPUS_OK) state_ = d;
}

VoiceDecoder::~VoiceDecoder() { opus_decoder_destroy(static_cast<OpusDecoder*>(state_)); }

bool VoiceDecoder::decode(const u8* packet, u32 bytes, f32* frame) {
    if (!state_) return false;
    const int n = opus_decode_float(static_cast<OpusDecoder*>(state_), bytes ? packet : nullptr, static_cast<opus_int32>(bytes), frame, kVoiceFrame, 0);
    return n == static_cast<int>(kVoiceFrame);
}

} // namespace lsd::net
