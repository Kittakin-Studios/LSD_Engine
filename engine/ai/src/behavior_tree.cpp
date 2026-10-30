#include "lsd/ai/behavior_tree.h"

#include <cstdio>
#include <cstring>

namespace lsd::ai {

// ---- Blackboard ----

namespace {
Blackboard::Entry* slot(Blackboard& b, const char* name) {
    for (u32 i = 0; i < b.count; ++i) {
        if (std::strcmp(b.entries[i].name, name) == 0) return &b.entries[i];
    }
    if (b.count == kMaxBlackboardKeys) return nullptr;
    Blackboard::Entry& e = b.entries[b.count++];
    std::snprintf(e.name, sizeof e.name, "%s", name);
    return &e;
}
} // namespace

void Blackboard::set(const char* name, f32 number) {
    if (Entry* e = slot(*this, name)) {
        e->value[0] = e->value[1] = e->value[2] = number;
        e->vector = false;
    }
}

void Blackboard::set(const char* name, Vec3 location) {
    if (Entry* e = slot(*this, name)) {
        e->value[0] = location.x;
        e->value[1] = location.y;
        e->value[2] = location.z;
        e->vector = true;
    }
}

void Blackboard::clear(const char* name) {
    for (u32 i = 0; i < count; ++i) {
        if (std::strcmp(entries[i].name, name) == 0) {
            entries[i] = entries[--count];
            return;
        }
    }
}

const Blackboard::Entry* Blackboard::find(const char* name) const {
    for (u32 i = 0; i < count; ++i) {
        if (std::strcmp(entries[i].name, name) == 0) return &entries[i];
    }
    return nullptr;
}

bool Blackboard::is_set(const char* name) const {
    const Entry* e = find(name);
    return e && (e->vector || e->value[0] != 0.0f);
}

// ---- Runner ----

void BtRunner::abort(const BehaviorTree& tree, u32 index, const BtHost& host) {
    State& s = states_[index];
    if (!s.running) return;
    const BtNode& n = tree.nodes[index];
    if (n.type == BtType::MoveTo && host.stop && !moved_) host.stop(host.user);
    for (u32 c = 0; c < n.child_count; ++c) abort(tree, n.children[c], host);
    s = State{};
}

BtStatus BtRunner::run(const BehaviorTree& tree, u32 index, Blackboard& board, const BtHost& host, f32 dt) {
    const BtNode& n = tree.nodes[index];
    State& s = states_[index];
    const bool resumed = s.running;
    s.running = false;
    BtStatus result = BtStatus::Failure;
    switch (n.type) {
        case BtType::Root:
            result = n.child_count ? run(tree, n.children[0], board, host, dt) : BtStatus::Failure;
            break;
        case BtType::Selector: {
            // Reactive: earlier (higher-priority) children are tried again every tick.
            for (u32 c = 0; c < n.child_count; ++c) {
                const BtStatus r = run(tree, n.children[c], board, host, dt);
                if (r == BtStatus::Failure) continue;
                for (u32 k = c + 1; k < n.child_count; ++k) abort(tree, n.children[k], host);  // a lower one was running: interrupted
                s.child = c;
                result = r;
                break;
            }
            break;
        }
        case BtType::Sequence: {
            u32 c = resumed ? s.child : 0;
            result = BtStatus::Success;
            for (; c < n.child_count; ++c) {
                const BtStatus r = run(tree, n.children[c], board, host, dt);
                if (r == BtStatus::Success) continue;
                result = r;
                break;
            }
            s.child = result == BtStatus::Running ? c : 0;
            break;
        }
        case BtType::MoveTo: {
            const Blackboard::Entry* target = board.find(n.key);
            if (!target || !target->vector || !host.move_to || !host.moving) break;
            const Vec3 to{target->value[0], target->value[1], target->value[2]};
            if (host.position) {
                const Vec3 d = to - host.position(host.user);
                if (d.x * d.x + d.y * d.y <= n.value * n.value) {
                    if (resumed && host.stop) host.stop(host.user);
                    result = BtStatus::Success;
                    break;
                }
            }
            const Vec3 moved = to - s.target;
            if (!resumed || !host.moving(host.user) || dot(moved, moved) > 0.25f) {  // start, or follow a target that moved (chasing)
                if (!host.move_to(host.user, to, n.value)) break;
                s.target = to;
                moved_ = true;
            }
            result = BtStatus::Running;
            break;
        }
        case BtType::Wait:
            s.time = (resumed ? s.time : 0.0f) + dt;
            result = s.time >= n.value ? BtStatus::Success : BtStatus::Running;
            break;
        case BtType::SetKey: {
            f32 v[3] = {0, 0, 0};
            const int count = std::sscanf(n.text, "%f , %f , %f", &v[0], &v[1], &v[2]);
            if (count == 3) board.set(n.key, Vec3{v[0], v[1], v[2]});
            else board.set(n.key, v[0]);
            result = BtStatus::Success;
            break;
        }
        case BtType::ClearKey:
            board.clear(n.key);
            result = BtStatus::Success;
            break;
        case BtType::IsSet:
            result = board.is_set(n.key) ? BtStatus::Success : BtStatus::Failure;
            break;
        case BtType::Compare: {
            const Blackboard::Entry* e = board.find(n.key);
            const f32 v = e ? e->value[0] : 0.0f;
            const bool ok = n.op == 0 ? v > n.value : n.op == 1 ? v < n.value : n.op == 2 ? v == n.value : v != n.value;
            result = ok ? BtStatus::Success : BtStatus::Failure;
            break;
        }
        case BtType::RandomLocation: {
            Vec3 point;
            if (host.position && host.random_point && host.random_point(host.user, host.position(host.user), n.value, point)) {
                board.set(n.key, point);
                result = BtStatus::Success;
            }
            break;
        }
        case BtType::RunScript: {
            const int r = host.run_script ? host.run_script(host.user, n.text, dt) : -1;
            result = r == 1 ? BtStatus::Success : r == 2 ? BtStatus::Running : BtStatus::Failure;
            break;
        }
    }
    s.running = result == BtStatus::Running;
    s.ok = result != BtStatus::Failure;
    return result;
}

void BtRunner::tick(const BehaviorTree& tree, Blackboard& board, const BtHost& host, f32 dt) {
    if (tree_ != &tree) {  // a new (or reloaded) tree: start clean
        for (State& s : states_) s = State{};
        tree_ = &tree;
    }
    moved_ = false;
    for (State& s : states_) s.ok = false;
    if (tree.count) run(tree, 0, board, host, dt);
}

void BtRunner::reset(const BtHost& host) {
    if (tree_ && tree_->count) abort(*tree_, 0, host);
    for (State& s : states_) s = State{};
}

u32 BtRunner::active(u32* out, u32 capacity) const {
    u32 n = 0;
    if (!tree_) return 0;
    for (u32 i = 0; i < tree_->count && n < capacity; ++i) {
        if (states_[i].ok) out[n++] = tree_->nodes[i].id;
    }
    return n;
}

} // namespace lsd::ai
