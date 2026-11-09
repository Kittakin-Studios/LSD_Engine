#pragma once
// State Trees (Unreal's State Tree): states, each running its tasks in order (the behaviour tree's own tasks:
// Move To, Wait, Run Script, Use Smart Object, Run Query...), and transitions that leave it when the tasks
// succeed or fail, a blackboard check holds, or after some seconds. One state is active at a time.
// shortcut: flat (no child states or parent transitions), add a parent index when trees grow past ~10 states.
#include "lsd/ai/behavior_tree.h"

namespace lsd::ai {

enum class StTrigger : u8 { Succeeded, Failed, Completed, Condition, Timer };

struct StTransition {
    StTrigger trigger = StTrigger::Completed;
    char key[32] = "";  // Condition: key <op> value (op as test_key)
    u32 op = 4;
    f32 value = 0.0f;   // Condition: compared with; Timer: seconds in the state
    u32 target = 0;     // the state it goes to (itself: start it again)
};

constexpr u32 kMaxStStates = 16;
constexpr u32 kMaxStTasks = 4;
constexpr u32 kMaxStTransitions = 4;

struct StState {
    char name[32] = "";
    BtNode tasks[kMaxStTasks];  // leaf tasks only (no composites): run in order like a Sequence
    u32 task_count = 0;
    StTransition transitions[kMaxStTransitions];
    u32 transition_count = 0;  // checked in order every tick; the first that fires wins
};

struct StateTree {
    StState states[kMaxStStates];
    u32 count = 0;  // states[0] is where it starts
};

// One agent running a State Tree.
class StRunner {
public:
    void tick(const StateTree& tree, Blackboard& board, const BtHost& host, f32 dt);
    void reset(const BtHost& host);  // stops what the state was doing; the next tick starts at state 0
    u32 state() const { return state_; }
    f32 time_in_state() const { return time_; }

private:
    void enter(const StateTree& tree, u32 state, const BtHost& host);
    BehaviorTree tasks_;  // the active state's tasks as Root -> Sequence -> tasks, run by the behaviour tree runner
    BtRunner runner_;
    const StateTree* tree_ = nullptr;
    u32 state_ = 0;
    f32 time_ = 0.0f;
    bool done_ = false;  // its tasks finished (they are not run again until it is entered again)
    BtStatus result_ = BtStatus::Running;
};

// Which transition of `state` fires (its index), or -1: pure, for the runner and tests.
i32 pick_transition(const StState& state, const Blackboard& board, bool done, BtStatus result, f32 time);

} // namespace lsd::ai
