#include "lsd/audio/dsp.h"

#include <cmath>
#include <cstring>

namespace lsd::audio {

namespace {
constexpr f64 kTau = 6.283185307179586;
constexpr f32 kPi = 3.14159265f;
} // namespace

f32 synth_oscillator(SynthShape shape, f64 phase, u32& noise) {
    switch (shape) {
        case SynthShape::Sine: return static_cast<f32>(std::sin(phase * kTau));
        // shortcut: naive saw / square alias at high pitches, band-limit them (PolyBLEP) when synths play leads
        case SynthShape::Saw: return static_cast<f32>(2.0 * phase - 1.0);
        case SynthShape::Square: return phase < 0.5 ? 1.0f : -1.0f;
        case SynthShape::Noise:
            noise ^= noise << 13;  // xorshift: cheap and allocation-free on the audio thread
            noise ^= noise >> 17;
            noise ^= noise << 5;
            return static_cast<f32>(noise & 0xFFFFFF) / static_cast<f32>(0x800000) - 1.0f;
    }
    return 0.0f;
}

f32 synth_envelope(bool gate, f32 attack, f32 decay, f32 sustain, f32 release, f32 rate, f32& level, f32& stage) {
    const f32 dt = 1.0f / rate;
    if (gate && (stage == 0.0f || stage == 4.0f)) stage = 1.0f;  // a new note (also cuts a release short)
    if (!gate && stage != 0.0f && stage != 4.0f) stage = 4.0f;
    if (stage == 1.0f) {
        level += attack > dt ? dt / attack : 1.0f;
        if (level >= 1.0f) {
            level = 1.0f;
            stage = 2.0f;
        }
    } else if (stage == 2.0f) {
        level -= decay > dt ? dt * (1.0f - sustain) / decay : 1.0f;
        if (level <= sustain) {
            level = sustain;
            stage = 3.0f;
        }
    } else if (stage == 3.0f) {
        level = sustain;
    } else if (stage == 4.0f) {
        level -= release > dt ? dt / release : 1.0f;  // full scale over Release: quieter notes end sooner
        if (level <= 0.0f) {
            level = 0.0f;
            stage = 0.0f;
        }
    }
    return level;
}

f32 synth_filter(f32 in, f32 cutoff, f32 resonance, bool high_pass, f32 rate, f32& s1, f32& s2) {
    cutoff = cutoff < 20.0f ? 20.0f : cutoff > rate * 0.45f ? rate * 0.45f : cutoff;
    resonance = resonance < 0.0f ? 0.0f : resonance > 1.0f ? 1.0f : resonance;
    const f32 g = std::tan(kPi * cutoff / rate);
    const f32 k = 1.4142f * (1.0f - 0.95f * resonance);  // 0 = Butterworth, 1 = ringing
    const f32 a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
    const f32 v3 = in - s2;
    const f32 v1 = a1 * s1 + a2 * v3;
    const f32 v2 = s2 + a2 * s1 + a3 * v3;
    s1 = 2.0f * v1 - s1;
    s2 = 2.0f * v2 - s2;
    return high_pass ? in - k * v1 - v2 : v2;
}

void synth_reset(const SynthProgram& p, SynthState& s) {
    for (u32 i = 0; i < kSynthMaxNodes; ++i) {
        s.value[i] = s.s1[i] = s.s2[i] = s.gate[i] = 0.0f;
        s.phase[i] = 0.0;
    }
    for (u32 i = 0; i < kSynthMaxParams; ++i) s.params[i].store(i < p.param_count ? p.param_defaults[i] : 0.0f, std::memory_order_relaxed);
    for (u32 i = 0; i < p.count && i < kSynthMaxNodes; ++i) {
        const SynthNode& n = p.nodes[i];
        if (n.op == SynthOp::Trigger) s.s1[i] = n.slot < kSynthMaxParams ? p.param_defaults[n.slot] : 0.0f;  // no pulse at start
        if (n.op == SynthOp::Wave) s.s1[i] = n.b > 0.5f ? 0.0f : 1.0f;  // unwired Play: playing from the start
    }
    s.noise = 22222;
    s.frame = 0;
}

bool synth_set_param(const SynthProgram& p, SynthState& s, const char* name, f32 value) {
    for (u32 i = 0; i < p.param_count; ++i) {
        if (std::strcmp(p.params[i], name) != 0) continue;
        s.params[i].store(value, std::memory_order_relaxed);
        return true;
    }
    return false;
}

u32 synth_render(const SynthProgram& p, SynthState& s, f32* out, u32 frames, u32 rate) {
    const f32 fr = static_cast<f32>(rate);
    u32 limit = frames;
    if (p.duration > 0.0f) {
        const u64 total = static_cast<u64>(static_cast<f64>(p.duration) * rate);
        limit = s.frame >= total ? 0 : total - s.frame < frames ? static_cast<u32>(total - s.frame) : frames;
    }
    const u32 count = p.count < kSynthMaxNodes ? p.count : kSynthMaxNodes;
    for (u32 f = 0; f < limit; ++f) {
        for (u32 i = 0; i < count; ++i) {
            const SynthNode& n = p.nodes[i];
            f32 in[kSynthInputs];
            for (u32 k = 0; k < kSynthInputs; ++k) {
                const i32 from = n.in[k].node;
                in[k] = from >= 0 && static_cast<u32>(from) < i ? s.value[from] : n.in[k].value;
            }
            f32 v = 0.0f;
            switch (n.op) {
                case SynthOp::Param: v = n.slot < kSynthMaxParams ? s.params[n.slot].load(std::memory_order_relaxed) : 0.0f; break;
                case SynthOp::Trigger: {  // fires when its parameter changes (lsd.trigger_audio counts up)
                    const f32 now = n.slot < kSynthMaxParams ? s.params[n.slot].load(std::memory_order_relaxed) : 0.0f;
                    if (now != s.s1[i]) {
                        s.s1[i] = now;
                        s.s2[i] = n.b > 0.0f ? n.b : 1.0f / fr;
                    }
                    v = s.s2[i] > 0.0f ? 1.0f : 0.0f;
                    s.s2[i] -= 1.0f / fr;
                    break;
                }
                case SynthOp::Oscillator:
                    v = synth_oscillator(static_cast<SynthShape>(n.mode), s.phase[i], s.noise) * in[1];
                    s.phase[i] += static_cast<f64>(in[0]) / rate;
                    s.phase[i] -= std::floor(s.phase[i]);  // negative frequencies wrap too
                    break;
                case SynthOp::Envelope: v = synth_envelope(in[0] > 0.5f, n.a, n.b, n.c, n.d, fr, s.s1[i], s.s2[i]); break;
                case SynthOp::Filter: v = synth_filter(in[0], in[1], in[2], n.mode == 1, fr, s.s1[i], s.s2[i]); break;
                case SynthOp::Gain: v = in[0] * in[1]; break;
                case SynthOp::Mix: v = in[0] + in[1] + in[2] + in[3]; break;
                case SynthOp::Wave: {
                    const u32 w = n.slot;
                    const bool gate = in[0] > 0.5f;
                    if (n.b > 0.5f && gate && s.gate[i] <= 0.5f) {  // Play's rising edge: from the start
                        s.phase[i] = 0.0;
                        s.s1[i] = 1.0f;
                    }
                    s.gate[i] = gate ? 1.0f : 0.0f;
                    if (w >= kSynthMaxWaves || !p.wave_data[w] || !p.wave_frames[w] || s.s1[i] < 0.5f) break;
                    u64 at = static_cast<u64>(s.phase[i]);
                    if (at >= p.wave_frames[w]) {
                        if (n.mode != 1) {
                            s.s1[i] = 0.0f;
                            break;
                        }
                        at = 0;
                        s.phase[i] = 0.0;
                    }
                    v = p.wave_data[w][at];
                    s.phase[i] += 1.0;
                    break;
                }
            }
            s.value[i] = v;
        }
        const i32 o = p.output.node;
        out[f] = (o >= 0 && static_cast<u32>(o) < count ? s.value[o] : p.output.value) * p.volume;
        ++s.frame;
    }
    return limit;
}

f32 music_next_boundary(f32 now, f32 bpm, u32 beats_per_bar, bool bar) {
    if (bpm <= 0.0f) return 0.0f;
    const f32 unit = 60.0f / bpm * static_cast<f32>(bar && beats_per_bar ? beats_per_bar : 1);
    const f32 into = now - std::floor(now / unit) * unit;
    return into < 0.001f || unit - into < 0.001f ? 0.0f : unit - into;
}

f32 music_stem_gain(f32 intensity, f32 from, f32 full) {
    if (full <= from) return intensity >= full ? 1.0f : 0.0f;
    const f32 t = (intensity - from) / (full - from);
    return t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
}

f32 distance_fraction(f32 distance, f32 min_distance, f32 max_distance) {
    if (max_distance <= min_distance) return distance > min_distance ? 1.0f : 0.0f;
    const f32 t = (distance - min_distance) / (max_distance - min_distance);
    return t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
}

f32 air_absorption_cutoff(f32 distance, f32 min_distance, f32 max_distance, f32 far_cutoff) {
    const f32 far = far_cutoff < 20.0f ? 20.0f : far_cutoff > 20000.0f ? 20000.0f : far_cutoff;
    return 20000.0f * std::pow(far / 20000.0f, distance_fraction(distance, min_distance, max_distance));
}

} // namespace lsd::audio
