#include "sound_cue.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lsd::editor {

namespace {

struct Evaluator {
    const script::Graph& g;
    CueEnv& env;
    CueVoice* out;
    u32 capacity;
    u32 count = 0;

    f32 random01() {  // xorshift: cheap, and seeded per CueEnv so tests are repeatable
        env.seed ^= env.seed << 13;
        env.seed ^= env.seed >> 17;
        env.seed ^= env.seed << 5;
        return static_cast<f32>(env.seed & 0xFFFFFF) / static_cast<f32>(0x1000000);
    }
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * random01(); }
    static f32 number(const script::GraphNode& n, u32 pin) { return static_cast<f32>(std::atof(n.values[pin])); }

    const script::GraphNode* source(u32 node, u32 pin) const {
        const script::GraphLink* link = g.link_into(node, pin);
        return link ? g.find(link->from_node) : nullptr;
    }

    void eval(const script::GraphNode* n, int depth) {
        if (!n || depth > 32) return;
        const char* t = n->type;
        const u32 start = count;
        if (std::strcmp(t, "CueWave") == 0) {
            if (count == capacity || !n->values[0][0]) return;
            CueVoice& v = out[count++];
            std::snprintf(v.sound, sizeof v.sound, "%s", n->values[0]);
            v.volume = v.pitch = 1.0f;
            v.delay = 0.0f;
            v.looping = std::strcmp(n->values[1], "true") == 0;
        } else if (std::strcmp(t, "CueMixer") == 0) {
            for (u32 i = 0; i < 4; ++i) eval(source(n->id, i), depth + 1);
        } else if (std::strcmp(t, "CueConcat") == 0) {
            f32 at = 0.0f;  // each input starts when the previous one ends
            for (u32 i = 0; i < 4; ++i) {
                const u32 first = count;
                eval(source(n->id, i), depth + 1);
                f32 end = at;
                for (u32 k = first; k < count; ++k) {
                    out[k].delay += at;
                    const f32 seconds = env.length ? env.length(env.user, out[k].sound) : 0.0f;
                    const f32 finish = out[k].delay + seconds / (out[k].pitch > 0.01f ? out[k].pitch : 0.01f);
                    end = finish > end ? finish : end;
                }
                at = end;
            }
        } else if (std::strcmp(t, "CueRandom") == 0) {
            i32 inputs[4];
            u32 n_inputs = 0;
            for (u32 i = 0; i < 4; ++i) {
                if (g.link_into(n->id, i)) inputs[n_inputs++] = static_cast<i32>(i);
            }
            if (!n_inputs) return;
            CueEnv::LastPick* last = nullptr;
            for (u32 i = 0; i < env.last_count && !last; ++i) {
                if (env.last[i].node == n->id) last = &env.last[i];
            }
            if (!last && env.last_count < 32) {
                last = &env.last[env.last_count++];
                *last = {n->id, -1};
            }
            const bool no_repeat = std::strcmp(n->values[4], "true") == 0 && n_inputs > 1;
            i32 pick;
            do {
                pick = inputs[static_cast<u32>(random01() * static_cast<f32>(n_inputs)) % n_inputs];
            } while (no_repeat && last && pick == last->index);
            if (last) last->index = pick;
            eval(source(n->id, static_cast<u32>(pick)), depth + 1);
        } else if (std::strcmp(t, "CueSwitch") == 0) {
            const f32 value = env.parameter ? env.parameter(env.user, n->values[4]) : 0.0f;
            const i32 index = static_cast<i32>(value);
            if (index >= 0 && index < 4) eval(source(n->id, static_cast<u32>(index)), depth + 1);
        } else if (std::strcmp(t, "CueModulator") == 0) {
            eval(source(n->id, 0), depth + 1);
            const f32 volume = range(number(*n, 1), number(*n, 2)), pitch = range(number(*n, 3), number(*n, 4));
            for (u32 k = start; k < count; ++k) {
                out[k].volume *= volume;
                out[k].pitch *= pitch;
            }
        } else if (std::strcmp(t, "CueDelay") == 0) {
            eval(source(n->id, 0), depth + 1);
            const f32 delay = range(number(*n, 1), number(*n, 2));
            for (u32 k = start; k < count; ++k) out[k].delay += delay;
        } else if (std::strcmp(t, "CueLooping") == 0) {
            eval(source(n->id, 0), depth + 1);
            for (u32 k = start; k < count; ++k) out[k].looping = true;
        }
    }
};

} // namespace

u32 evaluate_cue(const script::Graph& cue, CueEnv& env, CueVoice* out, u32 capacity) {
    Evaluator e{cue, env, out, capacity};
    for (const script::GraphNode& n : cue.nodes) {
        if (std::strcmp(n.type, "CueOutput") != 0) continue;
        e.eval(e.source(n.id, 0), 0);
        const f32 volume = Evaluator::number(n, 1), pitch = Evaluator::number(n, 2);
        for (u32 k = 0; k < e.count; ++k) {
            out[k].volume *= volume;
            out[k].pitch *= pitch;
        }
        break;  // one output per cue
    }
    return e.count;
}

namespace {

struct SynthCompiler {
    const script::Graph& g;
    audio::SynthProgram& p;
    char* error;
    usize capacity;
    u32 ids[audio::kSynthMaxNodes] = {};  // graph node id of each program node
    u32 visiting[64] = {};
    u32 visiting_count = 0;
    bool ok = true;

    SynthCompiler(const script::Graph& graph, audio::SynthProgram& program, char* error_out, usize error_capacity)
        : g(graph), p(program), error(error_out), capacity(error_capacity) {}
    void fail(const char* what, const script::GraphNode& n) {
        if (ok) std::snprintf(error, capacity, "%s (node %u): %s", n.type, n.id, what);
        ok = false;
    }
    static f32 number(const script::GraphNode& n, u32 pin) { return static_cast<f32>(std::atof(n.values[pin])); }
    u32 slot(char (*names)[64], u32& count, u32 max, const char* name) {  // waves by name, shared
        for (u32 i = 0; i < count; ++i) {
            if (std::strcmp(names[i], name) == 0) return i;
        }
        if (count == max) return max;
        std::snprintf(names[count], sizeof names[0], "%s", name);
        return count++;
    }
    u32 param(const char* name, f32 default_value) {
        for (u32 i = 0; i < p.param_count; ++i) {
            if (std::strcmp(p.params[i], name) == 0) return i;
        }
        if (p.param_count == audio::kSynthMaxParams) return audio::kSynthMaxParams;
        std::snprintf(p.params[p.param_count], sizeof p.params[0], "%s", name);
        p.param_defaults[p.param_count] = default_value;
        return p.param_count++;
    }
    // A data input: the program index of the node wired into it, or its typed number.
    audio::SynthInput input(const script::GraphNode& n, u32 pin) {
        audio::SynthInput in;
        in.value = number(n, pin);
        if (const script::GraphLink* link = g.link_into(n.id, pin)) in.node = visit(link->from_node);
        return in;
    }
    // Inputs first, then the node: the program's order is the order to compute in.
    i32 visit(u32 id) {
        for (u32 i = 0; i < p.count; ++i) {
            if (ids[i] == id) return static_cast<i32>(i);
        }
        const script::GraphNode* found = g.find(id);
        if (!found || !ok) return -1;
        const script::GraphNode& n = *found;
        for (u32 i = 0; i < visiting_count; ++i) {
            if (visiting[i] == id) {
                fail("feedback loop: a node feeds itself", n);
                return -1;
            }
        }
        if (visiting_count == 64) {
            fail("the graph is too deep", n);
            return -1;
        }
        visiting[visiting_count++] = id;
        audio::SynthNode node;
        const char* t = n.type;
        if (std::strcmp(t, "SynOscillator") == 0) {
            node.op = audio::SynthOp::Oscillator;
            node.in[0] = input(n, 0);
            node.in[1] = input(n, 1);
            const char* shape = n.values[2];
            node.mode = static_cast<u8>(std::strcmp(shape, "Saw") == 0      ? audio::SynthShape::Saw
                                        : std::strcmp(shape, "Square") == 0 ? audio::SynthShape::Square
                                        : std::strcmp(shape, "Noise") == 0  ? audio::SynthShape::Noise
                                                                            : audio::SynthShape::Sine);
        } else if (std::strcmp(t, "SynEnvelope") == 0) {
            // shortcut: Attack..Release are the typed numbers (not modulated), wire them when a patch needs it
            node.op = audio::SynthOp::Envelope;
            node.in[0] = input(n, 0);
            node.a = number(n, 1);
            node.b = number(n, 2);
            node.c = number(n, 3);
            node.d = number(n, 4);
        } else if (std::strcmp(t, "SynFilter") == 0) {
            node.op = audio::SynthOp::Filter;
            for (u32 k = 0; k < 3; ++k) node.in[k] = input(n, k);
            node.mode = std::strcmp(n.values[3], "true") == 0 ? 1 : 0;
        } else if (std::strcmp(t, "SynGain") == 0) {
            node.op = audio::SynthOp::Gain;
            node.in[0] = input(n, 0);
            node.in[1] = input(n, 1);
        } else if (std::strcmp(t, "SynMix") == 0) {
            node.op = audio::SynthOp::Mix;
            for (u32 k = 0; k < 4; ++k) node.in[k] = input(n, k);
        } else if (std::strcmp(t, "SynParam") == 0 || std::strcmp(t, "SynTrigger") == 0) {
            const bool trigger = t[3] == 'T';
            node.op = trigger ? audio::SynthOp::Trigger : audio::SynthOp::Param;
            const u32 index = param(n.values[0], trigger ? 0.0f : number(n, 1));
            if (index == audio::kSynthMaxParams) fail("too many parameters (16)", n);
            node.slot = static_cast<u8>(index);
            node.b = trigger ? number(n, 1) : 0.0f;
        } else if (std::strcmp(t, "SynWave") == 0) {
            node.op = audio::SynthOp::Wave;
            const u32 index = slot(p.waves, p.wave_count, audio::kSynthMaxWaves, n.values[0]);
            if (index == audio::kSynthMaxWaves) fail("too many Wave Players (8)", n);
            node.slot = static_cast<u8>(index);
            node.mode = std::strcmp(n.values[1], "true") == 0 ? 1 : 0;
            node.in[0] = input(n, 2);
            node.b = g.link_into(n.id, 2) ? 1.0f : 0.0f;
        } else {
            fail("not a Sound Synth node", n);
        }
        --visiting_count;
        if (!ok) return -1;
        if (p.count == audio::kSynthMaxNodes) {
            fail("too many nodes (64)", n);
            return -1;
        }
        ids[p.count] = id;
        p.nodes[p.count] = node;
        return static_cast<i32>(p.count++);
    }
};

} // namespace

bool compile_synth(const script::Graph& graph, audio::SynthProgram& out, char* error, usize error_capacity) {
    out = audio::SynthProgram();
    if (error_capacity) error[0] = '\0';
    SynthCompiler c(graph, out, error, error_capacity);
    for (const script::GraphNode& n : graph.nodes) {
        if (std::strcmp(n.type, "SynOutput") != 0) continue;
        out.output = c.input(n, 0);
        out.volume = SynthCompiler::number(n, 1);
        out.duration = SynthCompiler::number(n, 2);
        return c.ok;
    }
    std::snprintf(error, error_capacity, "No Output node");
    return false;
}

} // namespace lsd::editor
