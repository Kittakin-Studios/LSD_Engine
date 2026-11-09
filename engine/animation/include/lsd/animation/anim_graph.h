#pragma once
// Animation graph (a data asset, "AnimGraph"): Unreal's Anim Blueprint state machine without code.
// States play a clip or a 1D blend space (clips along a parameter such as Speed); transitions
// switch state when a parameter passes a value (or a clip finishes), crossfading over a time.
// Parameters are numbers set by the game (the Character fills Speed, InAir, VerticalSpeed), Lua or
// visual scripts.
#include "lsd/animation/animation.h"
#include "lsd/reflect/reflect.h"

namespace lsd::anim {

LSD_STRUCT()
struct BlendSample {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Which animation in its file (0 = the first)") u32 clip = 0;
    LSD_PROPERTY(Edit, Tooltip = "The parameter value where this clip plays alone (e.g. walk at 1.5 m/s, run at 5)") f32 position = 0.0f;
    LSD_PROPERTY(Edit, Tooltip = "2D blend spaces: the second parameter's value here (e.g. Direction -90 strafe left)") f32 position_y = 0.0f;
    LSD_PROPERTY(Edit, Widget = "Asset:Mesh", Tooltip = "The animation file this sample comes from (one take per file, as Mixamo and Unreal export them); empty = the graph's file")
    char animation[33] = "";
};

LSD_STRUCT()
struct AnimState {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The state's name: transitions go to it by this name") char name[32] = "Idle";
    LSD_PROPERTY(Edit, Widget = "Asset:Mesh", Tooltip = "The animation file it plays (one take per file, as Mixamo and Unreal export them); empty = the graph's file")
    char animation[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Which animation in that file (ignored when it has blend samples)") u32 clip = 0;
    LSD_PROPERTY(Edit, Tooltip = "Blend space: the parameter that picks between the samples (e.g. Speed); empty = just the clip")
    char blend_parameter[32] = "";
    LSD_PROPERTY(Edit, Tooltip = "2D blend space (Unreal's Blend Space): a second parameter (e.g. Direction); the samples' Position Y place them on it")
    char blend_parameter_y[32] = "";
    LSD_PROPERTY(Edit, Tooltip = "1D blend space: clips placed along the Blend parameter (e.g. walk at 1.5, run at 5); blended by where the parameter is") List<BlendSample, 8> samples;
    LSD_PROPERTY(Edit, Min = 0, Max = 10, Tooltip = "Playback speed of the state's clip (1 = as made, 2 = twice as fast)") f32 play_rate = 1.0f;
    LSD_PROPERTY(Edit, Tooltip = "The clip starts over when it ends (off: holds the last frame)") bool looping = true;
    // Where its box sits on the State Machine canvas (the editor saves it; both 0: laid out by itself).
    LSD_PROPERTY(Save) f32 node_x = 0.0f;
    LSD_PROPERTY(Save) f32 node_y = 0.0f;
};

LSD_STRUCT()
struct AnimTransition {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The state it leaves; empty = from any state") char from[32] = "";
    LSD_PROPERTY(Edit, Tooltip = "The state to go to when the condition holds") char to[32] = "";
    LSD_PROPERTY(Edit, Tooltip = "Parameter it tests (Speed, InAir...)") char parameter[32] = "Speed";
    LSD_PROPERTY(Edit, Widget = "Choice:Greater,Less,True,False,Finished", Tooltip = "True / False: the parameter is non-zero / zero. Finished: the state's clip ended")
    u32 condition = 0;
    LSD_PROPERTY(Edit, Tooltip = "What the parameter is compared with (Greater / Less)") f32 value = 0.1f;
    LSD_PROPERTY(Edit, Units = "s", Min = 0, Tooltip = "Crossfade time") f32 blend_time = 0.2f;
};

// An animation layer (Unreal's Layered Blend per Bone in an Anim Blueprint): a clip played from a bone down over
// what the states play, by a weight a parameter sets (an upper-body aim, reload or wave over the legs' run).
LSD_STRUCT()
struct AnimLayer {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The layer's name (shown in the list)") char name[32] = "Upper Body";
    LSD_PROPERTY(Edit, Widget = "Asset:Mesh", Tooltip = "The animation file it plays (one take per file); empty = the graph's file") char animation[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Which animation in that file (0 = the first)") u32 clip = 0;
    LSD_PROPERTY(Edit, Tooltip = "It plays from this bone down (spine_01: the upper body; UE rigs' names, or your rig's); empty = the whole body") char bone[48] = "spine_01";
    LSD_PROPERTY(Edit, Tooltip = "The parameter that sets its weight 0..1 (lsd.set_anim_param / Set Anim Parameter); empty = Weight below") char weight_parameter[32] = "";
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "Its weight when no Weight Parameter is named (0 = off, 1 = it replaces those bones)") f32 weight = 1.0f;
    LSD_PROPERTY(Edit, Min = 0, Max = 10, Tooltip = "Playback speed (1 = as made)") f32 play_rate = 1.0f;
    LSD_PROPERTY(Edit, Tooltip = "The clip starts over when it ends (off: holds the last frame)") bool looping = true;
};

LSD_STRUCT(Category = "Animation")
struct AnimGraph {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Mesh", Tooltip = "The FBX whose animations the clip numbers refer to; empty = the mesh's own") char animation[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "The state it starts in; empty = the first") char entry_state[32] = "";
    LSD_PROPERTY(Edit, Tooltip = "The graph's states; the first is where it starts (Unreal's Entry)") List<AnimState, 16> states;
    LSD_PROPERTY(Edit, Tooltip = "Checked in order every frame; the first that matches wins") List<AnimTransition, 32> transitions;
    LSD_PROPERTY(Edit, Tooltip = "Animation layers over the states (Unreal's Layered Blend per Bone): each plays a clip from a bone down, by its weight; up to 4, in order")
    List<AnimLayer, 4> layers;
};

constexpr u32 kMaxAnimParams = 16;

// Where one object is in its graph, plus its parameters.
struct GraphState {
    i32 current = -1;      // state index; -1 = not started
    i32 previous = -1;     // fading out (crossfade), -1 = none
    f32 phase = 0.0f;      // 0..1 through the current state (blend spaces keep their clips in step)
    f32 previous_phase = 0.0f;
    f32 fade = 1.0f;       // 0..1 of the crossfade
    f32 fade_time = 0.0f;
    bool finished = false; // the current state's (non-looping) clip ended
    struct Param {
        char name[32];
        f32 value;
    };
    Param params[kMaxAnimParams] = {};
    u32 param_count = 0;

    void set(const char* name, f32 value);
    f32 get(const char* name) const;  // 0 if never set
};

// Advances the graph by dt (transitions, phases, crossfade) and writes the clips to sample, with
// their weights and ratios, into `out` (up to kMaxBlendInputs). Returns how many.
u32 step_graph(const AnimGraph& graph, const Clip* clips, u32 clip_count, GraphState& state, f32 dt, BlendInput* out);
// The same with the clips as a table of pointers (clips gathered from several files; null entries play nothing).
u32 step_graph(const AnimGraph& graph, const Clip* const* clips, u32 clip_count, GraphState& state, f32 dt, BlendInput* out);

const char* state_name(const AnimGraph& graph, const GraphState& state);  // "" before it starts

// Editing (the State Machine window): a state renamed takes its transitions and the entry state
// along; a state removed takes the transitions to or from it (transitions from any state stay).
void rename_state(AnimGraph& graph, const char* from, const char* to);
void remove_state(AnimGraph& graph, u32 index);

} // namespace lsd::anim
