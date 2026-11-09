#pragma once
// Behaviour Tree assets (.lsdbt): the graph window edits them; this turns one into the
// ai::BehaviorTree that AI Controllers run in Play.
#include "lsd/ai/behavior_tree.h"
#include "lsd/ai/env_query.h"
#include "lsd/ai/state_tree.h"
#include "lsd/world/components.h"
#include "lsd/script/graph.h"

namespace lsd::editor {

constexpr char kBehaviorTreeExtension[] = ".lsdbt";

// A Behavior Tree asset's file by name (Run Behavior nodes); false if there is none.
using BtResolver = bool (*)(void* user, const char* name, char* path, usize capacity);

// False with a message naming the problem (no Root, a loop, a node with two parents, too many nodes).
// Run Behavior nodes are replaced by the named tree's branch (found through `resolve`), up to 4 deep.
// shortcut: subtrees are copied in when the tree loads; editing one reloads only trees opened after.
bool build_behavior_tree(const script::Graph& graph, ai::BehaviorTree& out, char* error, usize capacity, BtResolver resolve = nullptr, void* user = nullptr);
bool load_behavior_tree(const char* path, ai::BehaviorTree& out, char* error, usize capacity, BtResolver resolve = nullptr, void* user = nullptr);

// A State Tree asset as the runner wants it: transitions by state index. False with a message (no states, a
// transition to a state that does not exist).
bool build_state_tree(const world::StateTreeData& data, ai::StateTree& out, char* error, usize capacity);
ai::EnvQuery to_env_query(const world::EnvQueryData& data);

} // namespace lsd::editor
