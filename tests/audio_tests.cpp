// Audio tests without a sound card: the engine is mixed by hand (read_frames) and measured.
#include "lsd/audio/audio.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); ++g_failures; } } while (0)

using namespace lsd;
using namespace lsd::audio;

static const char kTone[] = LSD_TEST_TEMP_DIR "/tone.wav";

// Half a second of a 440 Hz sine, 48 kHz mono 16-bit PCM.
static void write_tone() {
    const u32 rate = 48000, frames = rate / 2, bytes = frames * 2;
    FILE* f = std::fopen(kTone, "wb");
    auto u32le = [&](u32 v) { std::fwrite(&v, 4, 1, f); };
    auto u16le = [&](u16 v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    u32le(36 + bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32le(16);
    u16le(1);  // PCM
    u16le(1);  // mono
    u32le(rate);
    u32le(rate * 2);
    u16le(2);
    u16le(16);
    std::fwrite("data", 1, 4, f);
    u32le(bytes);
    for (u32 i = 0; i < frames; ++i) {
        const i16 s = static_cast<i16>(std::sin(static_cast<double>(i) * 2.0 * 3.14159265 * 440.0 / rate) * 16000.0);
        std::fwrite(&s, 2, 1, f);
    }
    std::fclose(f);
}

// RMS of the next `frames` stereo frames of the mix.
static f32 loudness(AudioSystem& a, u32 frames = 4800) {
    static f32 buffer[48000 * 2];
    const u32 got = a.read_frames(buffer, frames);
    double sum = 0;
    for (u32 i = 0; i < got * 2; ++i) sum += static_cast<double>(buffer[i]) * buffer[i];
    return got ? static_cast<f32>(std::sqrt(sum / (got * 2))) : 0.0f;
}

int main() {
    write_tone();
    AudioSystem a;
    CHECK(a.init(false) && !a.has_device());
    CHECK(loudness(a) == 0.0f);  // nothing playing: silence

    PlayDesc d;
    d.path = kTone;
    d.name = "Tone";
    const u32 tone = a.play(d);
    CHECK(tone != 0 && a.playing(tone) && a.voice_count() == 1);
    const f32 full = loudness(a);
    CHECK(full > 0.1f);
    a.stop(tone);
    CHECK(!a.playing(tone) && a.voice_count() == 0);
    CHECK(a.play(PlayDesc{}) == 0);  // no file

    // Sound Classes: Master -> SFX -> Footsteps; muting SFX silences its children.
    const u32 sfx = a.add_class("SFX", kMasterClass, 1.0f, 1.0f);
    const u32 steps = a.add_class("Footsteps", sfx, 0.5f, 1.0f);
    CHECK(a.class_count() == 3 && std::strcmp(a.class_name(steps), "Footsteps") == 0);
    d.sound_class = steps;
    a.play(d);
    const f32 half = loudness(a);
    CHECK(half > full * 0.4f && half < full * 0.6f);  // class volume 0.5
    a.set_class_volume(sfx, 0.0f);
    loudness(a);  // volume changes ramp in over a few frames
    CHECK(loudness(a) < 0.01f);
    a.set_class_volume(sfx, 1.0f);
    a.stop_all();

    // Attenuation: further from the listener is quieter; beyond max distance it stops fading.
    a.set_listener({0, 0, 0}, {1, 0, 0});
    d.sound_class = kMasterClass;
    d.spatial = true;
    d.attenuation.falloff = 1;  // linear
    d.attenuation.min_distance = 1.0f;
    d.attenuation.max_distance = 20.0f;
    d.position = {2, 0, 0};
    u32 v = a.play(d);
    const f32 near_level = loudness(a);
    a.set_position(v, {15, 0, 0});
    loudness(a);
    const f32 far_level = loudness(a);
    CHECK(near_level > 0.05f && far_level < near_level * 0.5f);
    CHECK(a.voice_count() == 1 && std::fabs(a.voice(0).distance - 15.0f) < 0.01f && a.voice(0).spatial);
    a.stop_all();

    // Concurrency: two at most; the third stops the oldest, or is refused.
    d.spatial = false;
    d.concurrency = a.add_concurrency(2, 0);
    const u32 first = a.play(d), second = a.play(d), third = a.play(d);
    CHECK(first && second && third && !a.playing(first) && a.playing(second) && a.playing(third) && a.voice_count() == 2);
    a.stop_all();
    d.concurrency = a.add_concurrency(2, 2);
    a.play(d);
    a.play(d);
    CHECK(a.play(d) == 0 && a.voice_count() == 2);
    a.stop_all();

    // Finished voices are freed; looping ones keep going.
    d.concurrency = kNoGroup;
    a.play(d);
    d.looping = true;
    const u32 loop = a.play(d);
    loudness(a, 48000);  // 1 s: the half-second tone ends
    a.update();
    CHECK(a.voice_count() == 1 && a.playing(loop));
    a.reset_classes();
    CHECK(a.voice_count() == 0 && a.class_count() == 1);
    a.shutdown();
    if (g_failures == 0) std::printf("All audio tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
