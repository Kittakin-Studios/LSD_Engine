#include "lsd/ai/behavior_tree.h"

#include <cmath>
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

const BbKey* BlackboardSchema::find(const char* name) const {
    for (u32 i = 0; i < count; ++i) {
        if (std::strcmp(keys[i].name, name) == 0) return &keys[i];
    }
    return nullptr;
}

void apply_defaults(const BlackboardSchema& schema, Blackboard& board) {
    for (u32 i = 0; i < schema.count; ++i) {
        const BbKey& k = schema.keys[i];
        if (k.type == BbType::Vector) board.set(k.name, Vec3{k.value[0], k.value[1], k.value[2]});
        else if (k.type == BbType::Bool) board.set(k.name, k.value[0] != 0.0f ? 1.0f : 0.0f);
        else if (k.type != BbType::Object) board.set(k.name, k.type == BbType::Int ? std::round(k.value[0]) : k.value[0]);
    }
}

const char* typed_set(const BlackboardSchema& schema, Blackboard& board, const char* name, const f32* value, bool vector) {
    if (schema.count == 0) {
        if (vector) board.set(name, Vec3{value[0], value[1], value[2]});
        else board.set(name, value[0]);
        return nullptr;
    }
    const BbKey* k = schema.find(name);
    if (!k) return "not a key of the Blackboard";
    if ((k->type == BbType::Vector) != vector) return vector ? "wants a number or object" : "wants a location";
    switch (k->type) {
        case BbType::Vector: board.set(name, Vec3{value[0], value[1], value[2]}); break;
        case BbType::Bool: board.set(name, value[0] != 0.0f ? 1.0f : 0.0f); break;
        case BbType::Int: board.set(name, std::round(value[0])); break;
        default: board.set(name, value[0]); break;
    }
    return nullptr;
}

bool Blackboard::is_set(const char* name) const {
    const Entry* e = find(name);
    return e && (e->vector || e->value[0] != 0.0f);
}

bool test_key(const Blackboard& board, const char* key, u32 op, f32 value) {
    if (op == 4) return board.is_set(key);
    if (op == 5) return !board.is_set(key);
    const Blackboard::Entry* e = board.find(key);
    const f32 v = e ? e->value[0] : 0.0f;
    return op == 0 ? v > value : op == 1 ? v < value : op == 2 ? v == value : v != value;
}

namespace {
bool test_key(const Blackboard& board, const BtNode& n) { return test_key(board, n.key, n.op, n.value); }
} // namespace

// ---- Runner ----

void BtRunner::abort(const BehaviorTree& tree, u32 index, const BtHost& host) {
    State& s = states_[index];
    if (!s.running) return;
    const BtNode& n = tree.nodes[index];
    if ((n.type == BtType::MoveTo || n.type == BtType::UseSmartObject) && host.stop && !moved_) host.stop(host.user);
    if (s.claim >= 0 && host.release_smart_object) host.release_smart_object(host.user, s.claim);
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
        case BtType::Compare:
            result = test_key(board, n) ? BtStatus::Success : BtStatus::Failure;
            break;
        case BtType::Condition:
            // Checked every tick it is reached, so a running child is cut off as soon as it no longer holds.
            if (!n.child_count || !test_key(board, n)) {
                if (n.child_count) abort(tree, n.children[0], host);
                break;
            }
            result = run(tree, n.children[0], board, host, dt);
            break;
        case BtType::Loop: {
            if (!n.child_count) break;
            if (!resumed) s.child = 0;  // runs done
            result = run(tree, n.children[0], board, host, dt);
            if (result == BtStatus::Success) {
                s.child += 1;
                const bool forever = n.value <= 0.0f;
                result = forever || static_cast<f32>(s.child) < n.value ? BtStatus::Running : BtStatus::Success;  // again next tick
            }
            break;
        }
        case BtType::Cooldown:
            if (!n.child_count || (!resumed && clock_ < cooldown_until_[index])) break;
            result = run(tree, n.children[0], board, host, dt);
            if (result != BtStatus::Running) cooldown_until_[index] = clock_ + n.value;
            break;
        case BtType::TimeLimit:
            if (!n.child_count) break;
            s.time = (resumed ? s.time : 0.0f) + dt;
            if (s.time > n.value) {
                abort(tree, n.children[0], host);
                break;
            }
            result = run(tree, n.children[0], board, host, dt);
            break;
        case BtType::ForceSuccess:
            if (!n.child_count) break;
            result = run(tree, n.children[0], board, host, dt) == BtStatus::Running ? BtStatus::Running : BtStatus::Success;
            break;
        case BtType::Service:
            if (!n.child_count) break;
            // On entry, then every `value` seconds while the branch stays active.
            s.time = resumed ? s.time + dt : n.value;
            if (s.time >= n.value) {
                s.time = 0.0f;
                if (host.run_script) host.run_script(host.user, n.text, dt);
            }
            result = run(tree, n.children[0], board, host, dt);
            break;
        case BtType::RandomLocation: {
            Vec3 point;
            if (host.position && host.random_point && host.random_point(host.user, host.position(host.user), n.value, point)) {
                board.set(n.key, point);
                result = BtStatus::Success;
            }
            break;
        }
        case BtType::UseSmartObject: {
            if (!resumed) {
                s.claim = host.claim_smart_object ? host.claim_smart_object(host.user, n.text, n.value, s.target) : -1;
                s.used = -1.0f;
                if (s.claim < 0 || !host.move_to || !host.move_to(host.user, s.target, 0.3f)) break;
                moved_ = true;
            }
            if (s.used < 0.0f) {  // walking to the slot
                const Vec3 d = host.position ? s.target - host.position(host.user) : Vec3{};
                const f32 gap = d.x * d.x + d.y * d.y;
                const bool walking = host.moving && host.moving(host.user);
                if (gap > 0.5f * 0.5f && walking) {
                    result = BtStatus::Running;
                    break;
                }
                if (gap > 1.5f * 1.5f) break;  // stopped short (blocked): the claim is let go
                if (host.stop) host.stop(host.user);
                s.used = 0.0f;
            } else {
                s.used += dt;
            }
            const int r = host.use_smart_object ? host.use_smart_object(host.user, s.claim, s.used) : 1;
            if (r == 2) {
                result = BtStatus::Running;
                break;
            }
            result = r == 1 ? BtStatus::Success : BtStatus::Failure;
            break;
        }
        case BtType::RunQuery: {
            Vec3 point;
            if (host.run_query && host.run_query(host.user, n.text, point)) {
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
    if (!s.running && s.claim >= 0) {  // finished, failed or never got there: the slot is free again
        if (host.release_smart_object) host.release_smart_object(host.user, s.claim);
        s.claim = -1;
    }
    return result;
}

BtStatus BtRunner::tick(const BehaviorTree& tree, Blackboard& board, const BtHost& host, f32 dt) {
    if (tree_ != &tree) {  // a new (or reloaded) tree: start clean
        for (State& s : states_) s = State{};
        tree_ = &tree;
    }
    moved_ = false;
    clock_ += dt;
    for (State& s : states_) s.ok = false;
    return tree.count ? run(tree, 0, board, host, dt) : BtStatus::Failure;
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
