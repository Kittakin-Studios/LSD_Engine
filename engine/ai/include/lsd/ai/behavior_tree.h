#pragma once
// Behaviour trees (Unreal's Behavior Tree + Blackboard), interpreted: a tree of nodes ticked every
// frame from the root. Selectors try their children in order until one does not fail and are
// reactive (a higher-priority child that starts succeeding interrupts a lower one that is running);
// sequences run their children in order until one fails, remembering where they were. Leaves are
// tasks (move, wait, run a Lua function) and conditions on the blackboard (per-agent named values).
// Built from a .lsdbt graph by the editor; the game provides movement and scripts through BtHost.
#include "lsd/foundation/math.h"

namespace lsd::ai {

enum class BtStatus : u8 { Success, Failure, Running };

enum class BtType : u8 {
    Root, Selector, Sequence,
    MoveTo,          // key: a location (or entity) on the blackboard; value: acceptance radius
    Wait,            // value: seconds
    SetKey,          // key = text ("1", "x,y,z")
    ClearKey,
    IsSet,           // condition: the key is set and not 0
    Compare,         // condition: key <op> value (op: 0 >, 1 <, 2 ==, 3 !=)
    RandomLocation,  // key = a reachable point within value metres
    RunScript,       // text: a Lua function on the agent: true = success, false = failure, nil = running
};

constexpr u32 kMaxBtChildren = 4;
constexpr u32 kMaxBtNodes = 64;

struct BtNode {
    BtType type = BtType::Root;
    u32 id = 0;  // the graph node it came from (for highlighting while debugging)
    u32 children[kMaxBtChildren] = {};
    u32 child_count = 0;
    char key[32] = "";
    char text[64] = "";
    f32 value = 0.0f;
    u32 op = 0;
};

struct BehaviorTree {
    BtNode nodes[kMaxBtNodes];
    u32 count = 0;  // nodes[0] is the root
};

constexpr u32 kMaxBlackboardKeys = 32;

// Per-agent values by name: numbers, locations and entities (as their handle value).
struct Blackboard {
    struct Entry {
        char name[32];
        f32 value[3];
        bool vector;
    };
    Entry entries[kMaxBlackboardKeys] = {};
    u32 count = 0;

    void set(const char* name, f32 number);
    void set(const char* name, Vec3 location);
    void clear(const char* name);
    const Entry* find(const char* name) const;
    bool is_set(const char* name) const;  // present and not 0
};

// What the game does for the tree's tasks.
struct BtHost {
    void* user = nullptr;
    bool (*move_to)(void* user, Vec3 target, f32 acceptance) = nullptr;  // starts walking; false if unreachable
    bool (*moving)(void* user) = nullptr;
    void (*stop)(void* user) = nullptr;
    Vec3 (*position)(void* user) = nullptr;
    bool (*random_point)(void* user, Vec3 around, f32 radius, Vec3& out) = nullptr;
    // A Lua function on the agent: 1 success, 0 failure, 2 running, -1 missing.
    int (*run_script)(void* user, const char* function, f32 dt) = nullptr;
};

// One agent running a tree.
class BtRunner {
public:
    void tick(const BehaviorTree& tree, Blackboard& board, const BtHost& host, f32 dt);
    void reset(const BtHost& host);  // stops whatever was running
    // The ids of the nodes that ran this tick without failing (the active branch, root first), for
    // the graph window's highlight.
    u32 active(u32* out, u32 capacity) const;

private:
    struct State {
        u32 child = 0;      // composites: the child being run
        f32 time = 0.0f;    // Wait
        Vec3 target;        // MoveTo: where it was sent
        bool running = false;
        bool ok = false;    // ran this tick and did not fail
    };
    BtStatus run(const BehaviorTree& tree, u32 index, Blackboard& board, const BtHost& host, f32 dt);
    void abort(const BehaviorTree& tree, u32 index, const BtHost& host);
    const BehaviorTree* tree_ = nullptr;
    State states_[kMaxBtNodes];
    bool moved_ = false;  // a Move To was started this tick: an interrupted one must not stop it
};

} // namespace lsd::ai
