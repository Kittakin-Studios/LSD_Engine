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
    // Music helpers: fades, the play position, a seek, stop after a fade.
    a.fade_voice(tone, 0.0f, 0.0f);
    CHECK(loudness(a) < full * 0.05f);  // faded to silence at once
    a.fade_voice(tone, 1.0f, 0.0f);
    CHECK(loudness(a) > full * 0.8f);
    const f32 at = a.voice_time(tone);
    CHECK(at > 0.15f && at < 0.4f);  // three reads of 0.1 s so far
    a.seek_voice(tone, 0.05f);
    CHECK(std::fabs(a.voice_time(tone) - 0.05f) < 0.01f);
    a.stop_with_fade(tone, 0.05f);
    loudness(a, 9600);  // past the fade
    a.update();
    CHECK(!a.playing(tone));
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
    a.stop_all();

    // Delayed start, voices grouped under one instance, length.
    CHECK(std::fabs(a.length_seconds(kTone) - 0.5f) < 0.01f && a.length_seconds("missing.wav") == 0.0f);
    // Waveform (Content Browser thumbnails): the steady tone peaks at 16000 / 32768 in every column.
    f32 peaks[64];
    CHECK(waveform(kTone, 64, peaks) && !waveform("missing.wav", 64, peaks));
    waveform(kTone, 64, peaks);
    bool steady = true;
    for (f32 p : peaks) steady = steady && std::fabs(p - 16000.0f / 32768.0f) < 0.01f;
    CHECK(steady);
    d.looping = false;
    d.delay = 0.3f;
    const u32 cue = a.play(d);
    d.delay = 0.0f;
    d.instance = cue;
    a.play(d);  // a second voice of the same "cue"
    a.stop(cue);
    CHECK(a.voice_count() == 0 && !a.playing(cue));
    d.instance = 0;
    d.delay = 0.3f;
    a.play(d);
    CHECK(loudness(a, 9600) < 0.001f);  // the first 0.2 s: still waiting
    loudness(a, 4800);
    CHECK(loudness(a, 4800) > 0.1f);    // started at 0.3 s
    d.delay = 0.0f;
    a.stop_all();

    // Sound Mixes: duck SFX (and its Footsteps child) to zero, faded; popping brings it back.
    const u32 sfx2 = a.add_class("SFX", kMasterClass, 1.0f, 1.0f);
    const u32 steps2 = a.add_class("Footsteps", sfx2, 1.0f, 1.0f);
    d.sound_class = steps2;
    d.looping = true;
    a.play(d);
    const MixAdjust duck{sfx2, 0.0f};
    const u32 mix = a.push_mix(&duck, 1, 0.5f, 0.0f);
    CHECK(mix && a.mix_count() == 1);
    a.update(0.25f);  // half way: half volume
    loudness(a);
    const f32 halfway = loudness(a);
    a.update(0.5f);
    loudness(a);
    const f32 ducked = loudness(a);
    a.pop_mix(mix);  // no fade out: straight back
    CHECK(a.mix_count() == 0);
    loudness(a);
    const f32 back = loudness(a);
    CHECK(ducked < 0.01f && halfway > back * 0.35f && halfway < back * 0.65f && back > 0.1f);
    a.set_class_volume(sfx2, 0.5f);  // a class set while a mix is up keeps both
    a.push_mix(&duck, 1, 0.0f, 0.0f);
    loudness(a);
    CHECK(loudness(a) < 0.01f);

    a.reset_classes();
    CHECK(a.voice_count() == 0 && a.class_count() == 1 && a.mix_count() == 0);

    // Submixes: the 440 Hz tone through each effect, measured against the same tone without one.
    d = PlayDesc{};
    d.path = kTone;
    d.looping = true;
    auto level_through = [&](const SubmixEffect* fx, u32 n) {
        a.reset_classes();
        d.sound_class = a.add_class("Fx", kMasterClass, 1.0f, 1.0f, fx, n);
        a.play(d);
        loudness(a, 9600);  // let filters and envelopes settle
        const f32 level = loudness(a, 9600);
        a.stop_all();
        return level;
    };
    const f32 dry = level_through(nullptr, 0);
    SubmixEffect fx;
    fx.type = 1;  // low pass at 100 Hz: 440 Hz mostly gone
    fx.frequency = 100.0f;
    CHECK(level_through(&fx, 1) < dry * 0.3f);
    fx.type = 2;  // high pass at 4 kHz: same
    fx.frequency = 4000.0f;
    CHECK(level_through(&fx, 1) < dry * 0.3f);
    fx.type = 3;  // EQ band +12 dB at 440 Hz: about four times louder
    fx.frequency = 440.0f;
    fx.gain_db = 12.0f;
    CHECK(level_through(&fx, 1) > dry * 2.5f);
    fx.type = 5;  // compressor: -40 dB threshold, 10:1, no make-up gain: much quieter
    fx.threshold_db = -40.0f;
    fx.ratio = 10.0f;
    fx.gain_db = 0.0f;
    CHECK(level_through(&fx, 1) < dry * 0.3f);
    SubmixEffect chain[2];  // a chain: low pass then EQ boost still ends quieter than dry at 440 Hz
    chain[0].type = 1;
    chain[0].frequency = 100.0f;
    chain[1].type = 3;
    chain[1].frequency = 440.0f;
    chain[1].gain_db = 6.0f;
    CHECK(level_through(chain, 2) < dry);

    // Reverb and echo leave a tail after the sound ends; dry sound does not.
    auto tail_after = [&](const SubmixEffect* effect) {
        a.reset_classes();
        d.sound_class = a.add_class("Fx", kMasterClass, 1.0f, 1.0f, effect, effect ? 1 : 0);
        d.looping = false;
        a.play(d);
        loudness(a, 24000 + 2400);  // the half-second tone, plus a little
        return loudness(a, 4800);
    };
    CHECK(tail_after(nullptr) < 0.001f);
    SubmixEffect reverb;
    reverb.type = 0;
    reverb.amount = 0.8f;
    reverb.size = 0.8f;
    CHECK(tail_after(&reverb) > 0.005f);
    SubmixEffect echo;
    echo.type = 4;
    echo.time = 0.1f;
    echo.amount = 0.8f;
    echo.size = 0.5f;
    CHECK(tail_after(&echo) > 0.005f);

    // Audio Volume pieces: environment reverb on everything, and a muffled voice.
    a.reset_classes();
    d.sound_class = kMasterClass;
    a.set_environment(0.8f, 0.8f);  // (tail_after would reset it with the classes)
    d.looping = false;
    a.play(d);
    loudness(a, 24000 + 2400);
    CHECK(loudness(a, 4800) > 0.005f);
    a.set_environment(0.0f, 0.5f);
    a.stop_all();
    d.looping = true;
    d.sound_class = kMasterClass;
    const u32 muffled = a.play(d);
    loudness(a, 4800);
    const f32 open_level = loudness(a);
    a.set_voice_lowpass(muffled, 100.0f);
    loudness(a, 4800);
    CHECK(loudness(a) < open_level * 0.3f && a.voice(0).lowpass < 200.0f);
    a.reset_classes();
    a.shutdown();
    if (g_failures == 0) std::printf("All audio tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
