// Audio tests without a sound card: the engine is mixed by hand (read_frames) and measured.
#include "lsd/audio/audio.h"
#include "lsd/audio/dsp.h"

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

// The meters: a sound through a class shows on its meter and on Master's; reading resets them.
static void test_meters() {
    AudioSystem a;
    CHECK(a.init(false));
    SubmixEffect lowpass;
    lowpass.type = 1;
    lowpass.frequency = 2000.0f;
    const u32 sfx = a.add_class("SFX", 0, 1.0f, 1.0f, &lowpass, 1);
    CHECK(sfx == 1 && a.class_parent(sfx) == 0 && a.class_effect_count(sfx) == 1 && a.class_effect_type(sfx, 0) == 1);
    PlayDesc d;
    d.path = kTone;
    d.sound_class = sfx;
    CHECK(a.play(d) != 0);
    loudness(a);
    const f32 class_peak = a.take_class_peak(sfx), master_peak = a.take_class_peak(0);
    CHECK(class_peak > 0.3f && class_peak < 0.7f && master_peak > 0.05f);  // the tone peaks near 0.49; Master hears it after the low-pass
    CHECK(a.take_class_peak(sfx) == 0.0f);                                  // read: reset
    a.shutdown();
}


// Sound Synth building blocks and a whole program, rendered offline into a buffer.
static void test_synth() {
    u32 noise = 1;
    CHECK(std::fabs(synth_oscillator(SynthShape::Sine, 0.25, noise) - 1.0f) < 1e-5f);
    CHECK(synth_oscillator(SynthShape::Saw, 0.0, noise) == -1.0f && synth_oscillator(SynthShape::Square, 0.75, noise) == -1.0f);
    f32 lo = 1.0f, hi = -1.0f;
    for (int i = 0; i < 1000; ++i) {
        const f32 n = synth_oscillator(SynthShape::Noise, 0.0, noise);
        lo = n < lo ? n : lo;
        hi = n > hi ? n : hi;
    }
    CHECK(lo >= -1.0f && hi <= 1.0f && hi - lo > 1.5f);

    // ADSR at 1 kHz: 10 ms attack, 10 ms decay to 0.5, release 20 ms.
    f32 level = 0.0f, stage = 0.0f;
    for (int i = 0; i < 10; ++i) synth_envelope(true, 0.01f, 0.01f, 0.5f, 0.02f, 1000.0f, level, stage);
    CHECK(level == 1.0f && stage == 2.0f);
    for (int i = 0; i < 30; ++i) synth_envelope(true, 0.01f, 0.01f, 0.5f, 0.02f, 1000.0f, level, stage);
    CHECK(std::fabs(level - 0.5f) < 1e-4f && stage == 3.0f);
    for (int i = 0; i < 9; ++i) synth_envelope(false, 0.01f, 0.01f, 0.5f, 0.02f, 1000.0f, level, stage);
    CHECK(level > 0.0f && stage == 4.0f);
    for (int i = 0; i < 2; ++i) synth_envelope(false, 0.01f, 0.01f, 0.5f, 0.02f, 1000.0f, level, stage);
    CHECK(level == 0.0f && stage == 0.0f);

    // Filters: a 10 kHz sine through a 500 Hz low pass is quiet, through a high pass it stays.
    auto through = [](bool high, f32 freq) {
        f32 s1 = 0.0f, s2 = 0.0f, peak = 0.0f;
        for (int i = 0; i < 4800; ++i) {
            const f32 y = synth_filter(std::sin(static_cast<f32>(i) * 6.2831853f * freq / 48000.0f), 500.0f, 0.0f, high, 48000.0f, s1, s2);
            if (i > 2400) peak = std::fmax(peak, std::fabs(y));
        }
        return peak;
    };
    CHECK(through(false, 10000.0f) < 0.02f && through(false, 50.0f) > 0.9f);
    CHECK(through(true, 10000.0f) > 0.9f && through(true, 50.0f) < 0.05f);

    // A program: param "pitch" -> sine (amplitude by an envelope gated by trigger "hit"), into a gain.
    auto* program = new SynthProgram();
    SynthProgram& p = *program;
    std::snprintf(p.params[0], sizeof p.params[0], "pitch");
    p.param_defaults[0] = 1000.0f;
    std::snprintf(p.params[1], sizeof p.params[1], "hit");
    p.param_count = 2;
    p.nodes[0].op = SynthOp::Param;
    p.nodes[0].slot = 0;
    p.nodes[1].op = SynthOp::Trigger;
    p.nodes[1].slot = 1;
    p.nodes[1].b = 0.05f;
    p.nodes[2].op = SynthOp::Envelope;
    p.nodes[2].in[0].node = 1;
    p.nodes[2].a = 0.001f;
    p.nodes[2].b = 0.001f;
    p.nodes[2].c = 1.0f;
    p.nodes[2].d = 0.001f;
    p.nodes[3].op = SynthOp::Oscillator;
    p.nodes[3].mode = static_cast<u8>(SynthShape::Sine);
    p.nodes[3].in[0].node = 0;
    p.nodes[3].in[1].node = 2;
    p.nodes[4].op = SynthOp::Gain;
    p.nodes[4].in[0].node = 3;
    p.nodes[4].in[1].value = 0.5f;
    p.count = 5;
    p.output.node = 4;
    p.duration = 0.5f;
    auto* state = new SynthState();
    synth_reset(p, *state);
    static f32 buffer[48000];
    auto peak_of = [](const f32* b, u32 n) {
        f32 m = 0.0f;
        for (u32 i = 0; i < n; ++i) m = std::fmax(m, std::fabs(b[i]));
        return m;
    };
    CHECK(synth_render(p, *state, buffer, 4800, 48000) == 4800);
    CHECK(peak_of(buffer, 4800) == 0.0f);  // no trigger yet: silent
    CHECK(synth_set_param(p, *state, "hit", 1.0f) && !synth_set_param(p, *state, "nope", 1.0f));
    synth_render(p, *state, buffer, 1200, 48000);  // 25 ms into a 50 ms pulse
    CHECK(std::fabs(peak_of(buffer + 200, 1000) - 0.5f) < 0.01f);
    u32 crossings = 0;  // 1000 Hz: about 2 zero crossings per millisecond
    for (u32 i = 201; i < 1200; ++i) crossings += (buffer[i - 1] < 0.0f) != (buffer[i] < 0.0f);
    CHECK(crossings >= 40 && crossings <= 44);
    synth_set_param(p, *state, "pitch", 2000.0f);  // live from the game thread
    synth_render(p, *state, buffer, 960, 48000);
    crossings = 0;
    for (u32 i = 1; i < 960; ++i) crossings += (buffer[i - 1] < 0.0f) != (buffer[i] < 0.0f);
    CHECK(crossings >= 76 && crossings <= 82);
    synth_render(p, *state, buffer, 4800, 48000);  // pulse over, envelope released
    CHECK(peak_of(buffer + 2400, 2400) == 0.0f);
    CHECK(synth_render(p, *state, buffer, 48000, 48000) < 48000);  // duration 0.5 s: ends

    // A Wave node playing a decoded buffer once, then looping.
    static f32 wave[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    SynthProgram& w = *program;
    w = SynthProgram();
    w.nodes[0].op = SynthOp::Wave;
    w.nodes[0].slot = 0;
    w.count = 1;
    w.output.node = 0;
    w.wave_count = 1;
    w.wave_data[0] = wave;
    w.wave_frames[0] = 4;
    synth_reset(w, *state);
    synth_render(w, *state, buffer, 6, 48000);
    CHECK(buffer[0] == 0.1f && buffer[3] == 0.4f && buffer[4] == 0.0f);
    w.nodes[0].mode = 1;
    synth_reset(w, *state);
    synth_render(w, *state, buffer, 6, 48000);
    CHECK(buffer[4] == 0.1f && buffer[5] == 0.2f);
    delete state;
    delete program;
}

// The music clock and stems, and the distance helpers for spatial sounds.
static void test_music_and_distance() {
    // 120 bpm, 4/4: a beat is 0.5 s, a bar 2 s.
    CHECK(std::fabs(music_next_boundary(0.3f, 120.0f, 4, false) - 0.2f) < 1e-4f);
    CHECK(std::fabs(music_next_boundary(0.3f, 120.0f, 4, true) - 1.7f) < 1e-4f);
    CHECK(std::fabs(music_next_boundary(2.1f, 120.0f, 4, true) - 1.9f) < 1e-4f);
    CHECK(music_next_boundary(4.0f, 120.0f, 4, true) == 0.0f);  // on the bar: now
    CHECK(music_next_boundary(1.0f, 0.0f, 4, true) == 0.0f);
    CHECK(music_stem_gain(0.0f, 0.3f, 0.6f) == 0.0f && music_stem_gain(0.45f, 0.3f, 0.6f) > 0.49f && music_stem_gain(0.45f, 0.3f, 0.6f) < 0.51f);
    CHECK(music_stem_gain(1.0f, 0.3f, 0.6f) == 1.0f && music_stem_gain(0.5f, 0.5f, 0.5f) == 1.0f && music_stem_gain(0.4f, 0.5f, 0.5f) == 0.0f);
    CHECK(distance_fraction(1.0f, 2.0f, 40.0f) == 0.0f && distance_fraction(21.0f, 2.0f, 40.0f) == 0.5f && distance_fraction(90.0f, 2.0f, 40.0f) == 1.0f);
    CHECK(air_absorption_cutoff(1.0f, 2.0f, 40.0f, 2000.0f) == 20000.0f);
    CHECK(std::fabs(air_absorption_cutoff(40.0f, 2.0f, 40.0f, 2000.0f) - 2000.0f) < 1.0f);
    const f32 mid = air_absorption_cutoff(21.0f, 2.0f, 40.0f, 2000.0f);
    CHECK(mid > 6000.0f && mid < 6700.0f);  // log-spaced: sqrt(20000 * 2000)
}

int main() {
    write_tone();
    test_meters();
    test_synth();
    test_music_and_distance();
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
    a.set_voice_lowpass(muffled, 20000.0f);
    a.stop_all();

    // A mix's low-pass on a parent class muffles its children's voices while it is in, then lets go.
    a.reset_classes();
    const u32 world_class = a.add_class("World", kMasterClass, 1.0f, 1.0f);
    d.sound_class = a.add_class("Ambience", world_class, 1.0f, 1.0f);
    const u32 ambience = a.play(d);
    MixAdjust underwater{world_class, 1.0f};
    underwater.lowpass = 300.0f;
    const u32 dive = a.push_mix(&underwater, 1, 0.0f, 0.0f);
    a.update(0.0f);
    CHECK(a.voice(0).id == ambience && a.voice(0).lowpass < 400.0f);
    a.set_voice_lowpass(ambience, 5000.0f);  // a wall's muffle does not lift the mix's
    CHECK(a.voice(0).lowpass < 400.0f);
    a.pop_mix(dive);
    a.update(0.0f);
    CHECK(a.voice(0).lowpass > 4000.0f && a.voice(0).lowpass < 6000.0f);  // back to the wall's
    a.stop_all();

    // A Sound Synth through the mixer: a 440 Hz sine is heard, follows its parameter, ends with
    // its Duration; a scheduled stop lands on the mixer's clock.
    {
        auto* synth = new SynthProgram();
        std::snprintf(synth->params[0], sizeof synth->params[0], "level");
        synth->param_defaults[0] = 0.5f;
        synth->param_count = 1;
        synth->nodes[0].op = SynthOp::Param;
        synth->nodes[1].op = SynthOp::Oscillator;
        synth->nodes[1].in[0].value = 440.0f;
        synth->nodes[1].in[1].node = 0;
        synth->count = 2;
        synth->output.node = 1;
        synth->duration = 0.5f;
        PlayDesc s;
        s.name = "Synth";
        s.synth = synth;
        const u32 voice = a.play(s);
        delete synth;  // the voice has its own copy
        CHECK(voice != 0 && a.playing(voice));
        const f32 synth_level = loudness(a, 4800);
        CHECK(synth_level > 0.15f && synth_level < 0.4f);  // sine at 0.5: RMS ~0.35, mono spread to both sides
        a.set_synth_param("level", 0.0f);
        loudness(a, 960);
        CHECK(loudness(a, 4800) < 0.001f);
        a.set_synth_param("level", 0.5f);
        loudness(a, 48000);  // past its 0.5 s
        a.update(0.0f);
        CHECK(!a.playing(voice));
    }
    {
        d.sound_class = kMasterClass;
        d.looping = true;
        const u32 scheduled = a.play(d);
        const f32 t0 = a.time_seconds();
        a.stop_at(scheduled, 0.1f, 0.0f);
        loudness(a, 2400);  // 50 ms: still playing
        CHECK(a.playing(scheduled) && a.time_seconds() - t0 > 0.04f);
        loudness(a, 4800);  // past 100 ms
        CHECK(loudness(a, 4800) < 0.001f);
        a.update(0.0f);
        CHECK(!a.playing(scheduled));
        d.looping = false;
    }

    // Air absorption: a spatial voice far away gets a low-pass from its distance alone.
    d.spatial = true;
    d.attenuation.air_absorption = true;
    d.attenuation.air_absorption_far = 2000.0f;
    d.attenuation.reverb_send = true;
    d.position = Vec3{30.0f, 0.0f, 0.0f};
    a.set_listener({0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    const u32 far = a.play(d);
    a.update(0.0f);
    CHECK(far != 0 && a.voice(0).lowpass < 5000.0f && a.voice(0).lowpass > 2000.0f);
    a.set_position(far, {1.0f, 0.0f, 0.0f});  // inside Min Distance: open again
    a.update(0.0f);
    CHECK(a.voice(0).lowpass > 19000.0f);
    CHECK(loudness(a, 4800) > 0.01f);  // the send path mixes without trouble
    a.stop_all();
    d.spatial = false;

    a.reset_classes();
    a.shutdown();
    if (g_failures == 0) std::printf("All audio tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
