#include "lsd/audio/voice_io.h"

#include "lsd/foundation/log.h"

#include <miniaudio.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace lsd::audio {

namespace {
constexpr u32 kRingFrames = VoiceIO::kRate / 2;      // half a second each
constexpr u32 kPrebuffer = VoiceIO::kRate * 6 / 100;  // 60 ms before a speaker starts playing
} // namespace

struct VoiceIO::Impl {
    struct Speaker {
        std::atomic<bool> active{false};
        u32 id = 0;  // written by the game thread before `active` is set
        ma_pcm_rb ring;
        bool playing = false;  // device thread only
        std::atomic<f32> level{0.0f};
    };
    ma_context null_context;  // tests only
    ma_device playback;
    ma_device capture;
    ma_pcm_rb mic;
    bool null_ok = false, playback_ok = false, capture_ok = false, mic_ok = false;
    Speaker speakers[kMaxSpeakers];
    bool rings_ok[kMaxSpeakers] = {};

    static void play_callback(ma_device* device, void* out, const void*, ma_uint32 frames) {
        auto& self = *static_cast<Impl*>(device->pUserData);
        auto* dst = static_cast<f32*>(out);
        std::memset(dst, 0, frames * sizeof(f32));
        for (Speaker& s : self.speakers) {
            if (!s.active.load(std::memory_order_acquire)) continue;
            const ma_uint32 waiting = ma_pcm_rb_available_read(&s.ring);
            if (!s.playing && waiting < kPrebuffer) continue;  // still filling its jitter buffer
            s.playing = true;
            ma_uint32 done = 0;
            f32 peak = 0.0f;
            while (done < frames) {
                ma_uint32 n = frames - done;
                void* src = nullptr;
                if (ma_pcm_rb_acquire_read(&s.ring, &n, &src) != MA_SUCCESS || n == 0) break;
                const f32* in = static_cast<const f32*>(src);
                for (ma_uint32 i = 0; i < n; ++i) {
                    dst[done + i] += in[i];
                    peak = std::fmax(peak, std::fabs(in[i]));
                }
                ma_pcm_rb_commit_read(&s.ring, n);
                done += n;
            }
            if (done < frames) s.playing = false;  // ran dry: wait for the buffer again
            s.level.store(peak, std::memory_order_relaxed);
        }
        for (ma_uint32 i = 0; i < frames; ++i) dst[i] = std::fmax(-1.0f, std::fmin(1.0f, dst[i]));  // several talking at once
    }

    static void capture_callback(ma_device* device, void*, const void* in, ma_uint32 frames) {
        auto& self = *static_cast<Impl*>(device->pUserData);
        const auto* src = static_cast<const f32*>(in);
        ma_uint32 done = 0;
        while (done < frames) {
            ma_uint32 n = frames - done;
            void* dst = nullptr;
            if (ma_pcm_rb_acquire_write(&self.mic, &n, &dst) != MA_SUCCESS || n == 0) break;  // full: the game is not reading, drop
            std::memcpy(dst, src + done, n * sizeof(f32));
            ma_pcm_rb_commit_write(&self.mic, n);
            done += n;
        }
    }
};

VoiceIO::~VoiceIO() { stop(); }

bool VoiceIO::start(bool capture, bool null_devices) {
    stop();
    impl_ = new Impl();
    ma_context* context = nullptr;
    if (null_devices) {
        const ma_backend null_backend = ma_backend_null;
        impl_->null_ok = ma_context_init(&null_backend, 1, nullptr, &impl_->null_context) == MA_SUCCESS;
        if (!impl_->null_ok) {
            stop();
            return false;
        }
        context = &impl_->null_context;
    }
    for (u32 i = 0; i < kMaxSpeakers; ++i) impl_->rings_ok[i] = ma_pcm_rb_init(ma_format_f32, 1, kRingFrames, nullptr, nullptr, &impl_->speakers[i].ring) == MA_SUCCESS;
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 1;
    config.sampleRate = kRate;
    config.dataCallback = Impl::play_callback;
    config.pUserData = impl_;
    impl_->playback_ok = ma_device_init(context, &config, &impl_->playback) == MA_SUCCESS && ma_device_start(&impl_->playback) == MA_SUCCESS;
    if (!impl_->playback_ok) {
        LSD_LOG_WARN("voice", "No playback device for voice chat");
        stop();
        return false;
    }
    if (capture) {
        impl_->mic_ok = ma_pcm_rb_init(ma_format_f32, 1, kRingFrames, nullptr, nullptr, &impl_->mic) == MA_SUCCESS;
        ma_device_config c = ma_device_config_init(ma_device_type_capture);
        c.capture.format = ma_format_f32;
        c.capture.channels = 1;
        c.sampleRate = kRate;
        c.dataCallback = Impl::capture_callback;
        c.pUserData = impl_;
        impl_->capture_ok = impl_->mic_ok && ma_device_init(context, &c, &impl_->capture) == MA_SUCCESS && ma_device_start(&impl_->capture) == MA_SUCCESS;
        if (impl_->capture_ok) capture_ = &impl_->capture;
        else LSD_LOG_WARN("voice", "No microphone: you can hear voice chat but not talk");
    }
    return true;
}

void VoiceIO::stop() {
    if (!impl_) return;
    if (impl_->capture_ok) ma_device_uninit(&impl_->capture);
    if (impl_->playback_ok) ma_device_uninit(&impl_->playback);  // the callbacks have stopped: the rings can go
    if (impl_->mic_ok) ma_pcm_rb_uninit(&impl_->mic);
    for (u32 i = 0; i < kMaxSpeakers; ++i) {
        if (impl_->rings_ok[i]) ma_pcm_rb_uninit(&impl_->speakers[i].ring);
    }
    if (impl_->null_ok) ma_context_uninit(&impl_->null_context);
    delete impl_;
    impl_ = nullptr;
    capture_ = nullptr;
}

bool VoiceIO::read_frame(f32* frame) {
    if (!impl_ || !impl_->capture_ok || ma_pcm_rb_available_read(&impl_->mic) < kFrame) return false;
    u32 done = 0;
    while (done < kFrame) {
        ma_uint32 n = kFrame - done;
        void* src = nullptr;
        if (ma_pcm_rb_acquire_read(&impl_->mic, &n, &src) != MA_SUCCESS || n == 0) return false;
        std::memcpy(frame + done, src, n * sizeof(f32));
        ma_pcm_rb_commit_read(&impl_->mic, n);
        done += n;
    }
    return true;
}

void VoiceIO::play(u32 speaker, const f32* frame, f32 gain) {
    if (!impl_) return;
    Impl::Speaker* slot = nullptr;
    for (u32 i = 0; i < kMaxSpeakers && !slot; ++i) {
        if (impl_->speakers[i].active.load(std::memory_order_relaxed) && impl_->speakers[i].id == speaker) slot = &impl_->speakers[i];
    }
    for (u32 i = 0; i < kMaxSpeakers && !slot; ++i) {  // a new speaker takes a free slot (more than 16 at once: dropped)
        if (!impl_->rings_ok[i] || impl_->speakers[i].active.load(std::memory_order_relaxed)) continue;
        impl_->speakers[i].id = speaker;
        impl_->speakers[i].active.store(true, std::memory_order_release);
        slot = &impl_->speakers[i];
    }
    if (!slot) return;
    u32 done = 0;
    while (done < kFrame) {
        ma_uint32 n = kFrame - done;
        void* dst = nullptr;
        if (ma_pcm_rb_acquire_write(&slot->ring, &n, &dst) != MA_SUCCESS || n == 0) return;  // too far behind: drop
        auto* out = static_cast<f32*>(dst);
        for (ma_uint32 i = 0; i < n; ++i) out[i] = frame[done + i] * gain;
        ma_pcm_rb_commit_write(&slot->ring, n);
        done += n;
    }
}

f32 VoiceIO::level(u32 speaker) const {
    if (!impl_) return 0.0f;
    for (const Impl::Speaker& s : impl_->speakers) {
        if (s.active.load(std::memory_order_relaxed) && s.id == speaker) return s.level.load(std::memory_order_relaxed);
    }
    return 0.0f;
}

} // namespace lsd::audio
