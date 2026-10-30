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

} // namespace lsd::editor
