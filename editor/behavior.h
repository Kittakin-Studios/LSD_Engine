#pragma once
// Behaviour Tree assets (.lsdbt): the graph window edits them; this turns one into the
// ai::BehaviorTree that AI Controllers run in Play.
#include "lsd/ai/behavior_tree.h"
#include "lsd/script/graph.h"

namespace lsd::editor {

constexpr char kBehaviorTreeExtension[] = ".lsdbt";

// False with a message naming the problem (no Root, a loop, a node with two parents, too many nodes).
bool build_behavior_tree(const script::Graph& graph, ai::BehaviorTree& out, char* error, usize capacity);
bool load_behavior_tree(const char* path, ai::BehaviorTree& out, char* error, usize capacity);

} // namespace lsd::editor
