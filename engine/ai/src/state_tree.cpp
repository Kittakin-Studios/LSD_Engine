#include "lsd/ai/state_tree.h"

namespace lsd::ai {

i32 pick_transition(const StState& state, const Blackboard& board, bool done, BtStatus result, f32 time) {
    for (u32 i = 0; i < state.transition_count; ++i) {
        const StTransition& t = state.transitions[i];
        const bool fires = t.trigger == StTrigger::Succeeded ? done && result == BtStatus::Success
                           : t.trigger == StTrigger::Failed  ? done && result == BtStatus::Failure
                           : t.trigger == StTrigger::Completed ? done
                           : t.trigger == StTrigger::Condition ? test_key(board, t.key, t.op, t.value)
                                                                 : time >= t.value;
        if (fires) return static_cast<i32>(i);
    }
    return -1;
}

void StRunner::enter(const StateTree& tree, u32 state, const BtHost& host) {
    runner_.reset(host);  // the old state's tasks stop (a walk, a claimed Smart Object)
    state_ = state < tree.count ? state : 0;
    time_ = 0.0f;
    done_ = false;
    result_ = BtStatus::Running;
    const StState& s = tree.states[state_];
    tasks_.count = 2 + s.task_count;
    tasks_.nodes[0] = BtNode{};
    tasks_.nodes[0].type = BtType::Root;
    tasks_.nodes[0].child_count = 1;
    tasks_.nodes[0].children[0] = 1;
    tasks_.nodes[1] = BtNode{};
    tasks_.nodes[1].type = BtType::Sequence;
    tasks_.nodes[1].child_count = 0;
    for (u32 i = 0; i < s.task_count && i < kMaxBtChildren; ++i) {
        tasks_.nodes[2 + i] = s.tasks[i];
        tasks_.nodes[2 + i].child_count = 0;  // leaves only
        tasks_.nodes[1].children[tasks_.nodes[1].child_count++] = 2 + i;
    }
}

void StRunner::tick(const StateTree& tree, Blackboard& board, const BtHost& host, f32 dt) {
    if (!tree.count) return;
    if (tree_ != &tree) {
        tree_ = &tree;
        enter(tree, 0, host);
    }
    time_ += dt;
    if (!done_) {
        result_ = runner_.tick(tasks_, board, host, dt);
        done_ = result_ != BtStatus::Running;
    }
    const i32 t = pick_transition(tree.states[state_], board, done_, result_, time_);
    if (t >= 0) enter(tree, tree.states[state_].transitions[t].target, host);
}

void StRunner::reset(const BtHost& host) {
    runner_.reset(host);
    tree_ = nullptr;
}

} // namespace lsd::ai
