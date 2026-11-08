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
    // Decorators and services (Unreal's, as nodes with one child):
    Condition,       // runs the child only while key <op> value holds (op 0-3 as Compare, 4 is set, 5 is not set);
                     // aborts it the moment it stops holding (Unreal's Blackboard decorator, Observer Aborts Self)
    Loop,            // runs the child `value` times (0 = forever); fails when the child fails
    Cooldown,        // fails for `value` seconds after the child last finished
    TimeLimit,       // fails the child if it runs longer than `value` seconds
    ForceSuccess,    // the child's failure counts as success
    Service,         // text: a Lua function called every `value` seconds while the child runs (its result ignored)
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

// A Blackboard asset's keys (Unreal's Blackboard Data): each named key has a type and a default.
enum class BbType : u8 { Bool, Float, Int, Vector, Object };
struct BbKey {
    char name[32] = "";
    BbType type = BbType::Float;
    f32 value[3] = {};  // the default (Object: none, it starts unset)
};
struct BlackboardSchema {
    BbKey keys[kMaxBlackboardKeys];
    u32 count = 0;  // 0: untyped, any key goes
    const BbKey* find(const char* name) const;
};
void apply_defaults(const BlackboardSchema& schema, Blackboard& board);  // every key but Objects gets its default
// A script's write checked against the schema, the value made to fit (Int rounded, Bool 0 / 1). nullptr when written,
// else why not ("not a key of the Blackboard", "wants a location", "wants a number or object").
const char* typed_set(const BlackboardSchema& schema, Blackboard& board, const char* name, const f32* value, bool vector);

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
    f32 clock_ = 0.0f;                  // seconds this runner has ticked
    f32 cooldown_until_[kMaxBtNodes] = {};  // Cooldown: kept through aborts
    bool moved_ = false;  // a Move To was started this tick: an interrupted one must not stop it
};

} // namespace lsd::ai
