#pragma once
// Sound Cues (.lsdcue): a node graph that decides what plays (Unreal's Sound Cue). Evaluating a
// cue walks back from its Output node and returns the voices to start, each with its own volume,
// pitch, delay and looping, picked fresh every time (Random, Modulator, Delay, Switch).
#include "lsd/audio/dsp.h"
#include "lsd/foundation/types.h"
#include "lsd/script/graph.h"

namespace lsd::editor {

constexpr char kCueExtension[] = ".lsdcue";

struct CueVoice {
    char sound[64];  // sound asset name (or GUID) from a Wave Player
    f32 volume, pitch, delay;
    bool looping;
};

// What a cue needs from outside, and what it remembers between plays (Random's last pick).
struct CueEnv {
    void* user = nullptr;
    f32 (*length)(void* user, const char* sound) = nullptr;  // seconds, for Concatenator
    f32 (*parameter)(void* user, const char* name) = nullptr;  // for Switch
    u32 seed = 12345;
    struct LastPick {
        u32 node;
        i32 index;
    };
    LastPick last[32] = {};
    u32 last_count = 0;
};

// Fills `out` (up to `capacity`) and returns how many voices the cue plays this time.
u32 evaluate_cue(const script::Graph& cue, CueEnv& env, CueVoice* out, u32 capacity);

// Sound Synths (.lsdsynth, Unreal's MetaSounds): the graph from its Output back, flattened into a
// program the audio thread renders sample by sample. False (with the reason) on a feedback loop,
// an unknown node or too many nodes / parameters / waves.
constexpr char kSynthExtension[] = ".lsdsynth";
bool compile_synth(const script::Graph& graph, audio::SynthProgram& out, char* error, usize error_capacity);

} // namespace lsd::editor
