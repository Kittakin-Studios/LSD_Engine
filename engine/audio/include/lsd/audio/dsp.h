#pragma once
// Pure audio maths, no miniaudio: the Sound Synth's program and its sample renderer (Unreal's
// MetaSounds: oscillators, envelope, filter, gain, mix, a wave player, parameters and triggers),
// the music clock (quantised segment switches, stem gains by intensity) and the distance helpers
// for spatial sounds. Everything here is tested offline in tests/audio_tests.cpp.
#include "lsd/foundation/types.h"

#include <atomic>

namespace lsd::audio {

// ---- Sound Synth ----

enum class SynthOp : u8 { Param, Trigger, Oscillator, Envelope, Filter, Gain, Mix, Wave };
enum class SynthShape : u8 { Sine, Saw, Square, Noise };

constexpr u32 kSynthMaxNodes = 64;
constexpr u32 kSynthMaxParams = 16;
constexpr u32 kSynthMaxWaves = 8;
constexpr u32 kSynthInputs = 4;

// An input: another node's output (node >= 0, it comes earlier in the program) or a typed number.
struct SynthInput {
    i32 node = -1;
    f32 value = 0.0f;
};

// One node. Its inputs by op:
//   Param      -                               slot = parameter index
//   Trigger    -                               slot = parameter index, b = pulse seconds
//   Oscillator Frequency, Amplitude            mode = SynthShape
//   Envelope   Gate                            a..d = attack, decay (s), sustain (0..1), release (s)
//   Filter     In, Cutoff (Hz), Resonance      mode 0 low pass, 1 high pass
//   Gain       In, Gain
//   Mix        A, B, C, D
//   Wave       Play (rising edge restarts)     slot = wave index, mode 1 = looping, b = 1 when Play is wired
struct SynthNode {
    SynthOp op = SynthOp::Param;
    u8 mode = 0;
    u8 slot = 0;
    SynthInput in[kSynthInputs];
    f32 a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f;
};

// A compiled graph: nodes in evaluation order (inputs before the nodes that read them).
struct SynthProgram {
    SynthNode nodes[kSynthMaxNodes];
    u32 count = 0;
    SynthInput output;     // what is heard
    f32 volume = 1.0f;
    f32 duration = 0.0f;   // seconds before it ends by itself; 0 = until stopped
    char params[kSynthMaxParams][32] = {};
    f32 param_defaults[kSynthMaxParams] = {};
    u32 param_count = 0;
    char waves[kSynthMaxWaves][64] = {};  // sound asset names the Wave nodes play
    u32 wave_count = 0;
    // Filled by whoever plays it: each wave decoded to mono at the render rate (not owned).
    const f32* wave_data[kSynthMaxWaves] = {};
    u64 wave_frames[kSynthMaxWaves] = {};
};

// A playing synth's memory. Parameters may be set from the game thread while the audio thread renders.
struct SynthState {
    f32 value[kSynthMaxNodes];   // each node's output this sample
    f64 phase[kSynthMaxNodes];   // oscillators 0..1; waves: read position in frames
    f32 s1[kSynthMaxNodes], s2[kSynthMaxNodes];  // filter state; envelope level / stage; trigger last value / timer
    f32 gate[kSynthMaxNodes];    // last gate (envelope, wave) for edges
    std::atomic<f32> params[kSynthMaxParams];
    u32 noise = 22222;
    u64 frame = 0;               // rendered so far
};

void synth_reset(const SynthProgram& program, SynthState& state);
// Renders `frames` mono samples at `rate` Hz into `out`. Returns the frames written (fewer once
// `duration` has passed). No allocation, no locks: safe on the audio thread.
u32 synth_render(const SynthProgram& program, SynthState& state, f32* out, u32 frames, u32 rate);
// Sets a parameter by name (any thread). False if the program has no such parameter.
bool synth_set_param(const SynthProgram& program, SynthState& state, const char* name, f32 value);

// The building blocks, one sample at a time (tested on their own).
f32 synth_oscillator(SynthShape shape, f64 phase, u32& noise);  // phase 0..1 -> -1..1
// ADSR: `level` and `stage` (0 idle, 1 attack, 2 decay, 3 sustain, 4 release) advance by one sample.
f32 synth_envelope(bool gate, f32 attack, f32 decay, f32 sustain, f32 release, f32 rate, f32& level, f32& stage);
// A state-variable filter (two integrators): low or high pass with resonance 0..1.
f32 synth_filter(f32 in, f32 cutoff, f32 resonance, bool high_pass, f32 rate, f32& s1, f32& s2);

// ---- Music clock (Unreal's Quartz, small) ----

// Seconds from `now` (since the segment started) to the next beat (bar = false) or bar (bar = true)
// at `bpm` with `beats_per_bar`. On a boundary already (within a millisecond): 0.
f32 music_next_boundary(f32 now, f32 bpm, u32 beats_per_bar, bool bar);
// A stem's volume at an intensity: silent at or below `from`, full at or above `full`, linear between.
f32 music_stem_gain(f32 intensity, f32 from, f32 full);

// ---- Distance (spatial sounds) ----

// 0 at min_distance, 1 at max_distance and beyond.
f32 distance_fraction(f32 distance, f32 min_distance, f32 max_distance);
// Air absorption: high frequencies fade with distance. The low-pass cutoff (Hz) at a distance,
// open (20000) at min_distance down to `far_cutoff` at max_distance (log-spaced, as ears hear pitch).
f32 air_absorption_cutoff(f32 distance, f32 min_distance, f32 max_distance, f32 far_cutoff);

} // namespace lsd::audio
