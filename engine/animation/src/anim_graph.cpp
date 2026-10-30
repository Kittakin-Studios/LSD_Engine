#include "lsd/animation/anim_graph.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace lsd::anim {

void GraphState::set(const char* name, f32 value) {
    for (u32 i = 0; i < param_count; ++i) {
        if (std::strcmp(params[i].name, name) == 0) {
            params[i].value = value;
            return;
        }
    }
    if (param_count == kMaxAnimParams) return;
    std::snprintf(params[param_count].name, sizeof params[0].name, "%s", name);
    params[param_count++].value = value;
}

f32 GraphState::get(const char* name) const {
    for (u32 i = 0; i < param_count; ++i) {
        if (std::strcmp(params[i].name, name) == 0) return params[i].value;
    }
    return 0.0f;
}

namespace {

i32 find_state(const AnimGraph& g, const char* name) {
    for (u32 i = 0; i < g.states.count; ++i) {
        if (std::strcmp(g.states[i].name, name) == 0) return static_cast<i32>(i);
    }
    return -1;
}

const Clip* clip_at(const Clip* clips, u32 count, u32 index) { return index < count ? &clips[index] : nullptr; }

// A state's clips and weights at its parameter value (a 1D blend space: the two samples around
// the value, linearly), and its length in seconds for advancing the phase.
u32 state_inputs(const AnimState& s, const Clip* clips, u32 count, const GraphState& gs, f32 phase, f32 weight, BlendInput* out, f32* length) {
    if (!s.blend_parameter[0] || s.samples.count == 0) {
        const Clip* c = clip_at(clips, count, s.clip);
        out[0] = {c, phase, weight};
        if (length) *length = c ? c->duration : 0.0f;
        return 1;
    }
    // Samples sorted by position on the fly (at most 8).
    u32 order[8];
    for (u32 i = 0; i < s.samples.count; ++i) order[i] = i;
    for (u32 i = 1; i < s.samples.count; ++i) {
        const u32 o = order[i];
        u32 j = i;
        while (j > 0 && s.samples[order[j - 1]].position > s.samples[o].position) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = o;
    }
    const f32 x = gs.get(s.blend_parameter);
    const BlendSample* a = &s.samples[order[0]];
    const BlendSample* b = a;
    f32 t = 0.0f;
    if (x >= s.samples[order[s.samples.count - 1]].position) {
        a = b = &s.samples[order[s.samples.count - 1]];
    } else if (x > a->position) {
        for (u32 i = 0; i + 1 < s.samples.count; ++i) {
            const BlendSample& lo = s.samples[order[i]];
            const BlendSample& hi = s.samples[order[i + 1]];
            if (x >= lo.position && x <= hi.position) {
                a = &lo;
                b = &hi;
                t = hi.position > lo.position ? (x - lo.position) / (hi.position - lo.position) : 0.0f;
                break;
            }
        }
    }
    const Clip* ca = clip_at(clips, count, a->clip);
    const Clip* cb = clip_at(clips, count, b->clip);
    // Both play at the same phase (feet stay in step), their lengths blended.
    if (length) *length = (ca ? ca->duration : 0.0f) * (1.0f - t) + (cb ? cb->duration : 0.0f) * t;
    if (a == b || t <= 0.0f) {
        out[0] = {ca, phase, weight};
        return 1;
    }
    if (t >= 1.0f) {
        out[0] = {cb, phase, weight};
        return 1;
    }
    out[0] = {ca, phase, weight * (1.0f - t)};
    out[1] = {cb, phase, weight * t};
    return 2;
}

bool passes(const AnimTransition& t, const GraphState& gs) {
    const f32 v = gs.get(t.parameter);
    switch (t.condition) {
        case 0: return v > t.value;
        case 1: return v < t.value;
        case 2: return v != 0.0f;
        case 3: return v == 0.0f;
        case 4: return gs.finished;
        default: return false;
    }
}

f32 advance(f32 phase, f32 dt, f32 rate, f32 length, bool looping, bool* finished) {
    if (length <= 0.0f) return phase;
    phase += dt * rate / length;
    if (looping) return phase - std::floor(phase);
    if (phase >= 1.0f && finished) *finished = true;
    return phase > 1.0f ? 1.0f : phase;
}

} // namespace

u32 step_graph(const AnimGraph& g, const Clip* clips, u32 clip_count, GraphState& gs, f32 dt, BlendInput* out) {
    if (g.states.count == 0) return 0;
    if (gs.current < 0 || gs.current >= static_cast<i32>(g.states.count)) {
        const i32 entry = g.entry_state[0] ? find_state(g, g.entry_state) : 0;
        gs.current = entry >= 0 ? entry : 0;
        gs.previous = -1;
        gs.phase = 0.0f;
        gs.fade = 1.0f;
        gs.finished = false;
    }
    // Transitions: the first whose state and condition match.
    for (const AnimTransition& t : g.transitions) {
        if (t.from[0] && find_state(g, t.from) != gs.current) continue;
        const i32 to = find_state(g, t.to);
        if (to < 0 || to == gs.current || !passes(t, gs)) continue;
        gs.previous = gs.current;
        gs.previous_phase = gs.phase;
        gs.current = to;
        gs.phase = 0.0f;
        gs.finished = false;
        gs.fade_time = t.blend_time;
        gs.fade = t.blend_time > 0.0f ? 0.0f : 1.0f;
        break;
    }
    // Advance the states and the fade.
    const AnimState& current = g.states[static_cast<u32>(gs.current)];
    BlendInput scratch[kMaxBlendInputs];
    f32 length = 0.0f;
    state_inputs(current, clips, clip_count, gs, gs.phase, 1.0f, scratch, &length);
    gs.phase = advance(gs.phase, dt, current.play_rate, length, current.looping, &gs.finished);
    if (gs.previous >= 0) {
        const AnimState& previous = g.states[static_cast<u32>(gs.previous)];
        f32 previous_length = 0.0f;
        state_inputs(previous, clips, clip_count, gs, gs.previous_phase, 1.0f, scratch, &previous_length);
        gs.previous_phase = advance(gs.previous_phase, dt, previous.play_rate, previous_length, previous.looping, nullptr);
        gs.fade = gs.fade_time > 0.0f ? gs.fade + dt / gs.fade_time : 1.0f;
        if (gs.fade >= 1.0f) {
            gs.fade = 1.0f;
            gs.previous = -1;
        }
    }
    // The clips to sample: the current state (and the one fading out), weighted by the fade.
    u32 n = state_inputs(current, clips, clip_count, gs, gs.phase, gs.previous >= 0 ? gs.fade : 1.0f, out, nullptr);
    if (gs.previous >= 0) n += state_inputs(g.states[static_cast<u32>(gs.previous)], clips, clip_count, gs, gs.previous_phase, 1.0f - gs.fade, out + n, nullptr);
    return n;
}

const char* state_name(const AnimGraph& g, const GraphState& gs) {
    return gs.current >= 0 && gs.current < static_cast<i32>(g.states.count) ? g.states[static_cast<u32>(gs.current)].name : "";
}

} // namespace lsd::anim
