#include "behavior.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lsd::editor {

namespace {

struct Builder {
    const script::Graph& graph;
    ai::BehaviorTree& out;
    char* error;
    usize capacity;
    u32 visited[ai::kMaxBtNodes] = {};  // graph node ids already in the tree
    u32 visited_count = 0;

    bool fail(const char* message, const script::GraphNode* node) {
        const script::NodeDef* def = node ? script::find_node_def(node->type) : nullptr;
        std::snprintf(error, capacity, def ? "%s (node '%s')" : "%s", message, def ? def->title : "");
        return false;
    }

    // Adds `node` and its subtree; `index` receives its place in out.nodes.
    bool add(const script::GraphNode& node, u32& index) {
        for (u32 i = 0; i < visited_count; ++i) {
            if (visited[i] == node.id) return fail("A node has two parents (or the wires loop)", &node);
        }
        if (out.count == ai::kMaxBtNodes) return fail("More than 64 nodes in one tree", &node);
        visited[visited_count++] = node.id;
        index = out.count++;
        ai::BtNode& n = out.nodes[index];
        n = ai::BtNode{};
        n.id = node.id;
        const char* t = node.type;
        using ai::BtType;
        n.type = std::strcmp(t, "BtRoot") == 0             ? BtType::Root
                 : std::strcmp(t, "BtSelector") == 0       ? BtType::Selector
                 : std::strcmp(t, "BtSequence") == 0       ? BtType::Sequence
                 : std::strcmp(t, "BtMoveTo") == 0         ? BtType::MoveTo
                 : std::strcmp(t, "BtWait") == 0           ? BtType::Wait
                 : std::strcmp(t, "BtRandomLocation") == 0 ? BtType::RandomLocation
                 : std::strcmp(t, "BtSetKey") == 0         ? BtType::SetKey
                 : std::strcmp(t, "BtClearKey") == 0       ? BtType::ClearKey
                 : std::strcmp(t, "BtRunScript") == 0      ? BtType::RunScript
                 : std::strcmp(t, "BtIsSet") == 0          ? BtType::IsSet
                 : std::strcmp(t, "BtCompare") == 0        ? BtType::Compare
                                                           : BtType::Root;
        if (n.type == BtType::Root && std::strcmp(t, "BtRoot") != 0) return fail("Not a behaviour tree node", &node);
        // Settings by pin (index 0 is the parent wire).
        switch (n.type) {
            case BtType::MoveTo:
            case BtType::RandomLocation:
                std::snprintf(n.key, sizeof n.key, "%s", node.values[1]);
                n.value = static_cast<f32>(std::atof(node.values[2]));
                break;
            case BtType::Wait: n.value = static_cast<f32>(std::atof(node.values[1])); break;
            case BtType::SetKey:
                std::snprintf(n.key, sizeof n.key, "%s", node.values[1]);
                std::snprintf(n.text, sizeof n.text, "%s", node.values[2]);
                break;
            case BtType::ClearKey:
            case BtType::IsSet: std::snprintf(n.key, sizeof n.key, "%s", node.values[1]); break;
            case BtType::RunScript: std::snprintf(n.text, sizeof n.text, "%s", node.values[1]); break;
            case BtType::Compare: {
                std::snprintf(n.key, sizeof n.key, "%s", node.values[1]);
                const char* op = node.values[2];
                n.op = std::strcmp(op, "<") == 0 ? 1 : std::strcmp(op, "==") == 0 ? 2 : std::strcmp(op, "!=") == 0 ? 3 : 0;
                n.value = static_cast<f32>(std::atof(node.values[3]));
                break;
            }
            default: break;
        }
        // Children: the Child pins left to right.
        const script::NodeDef* def = script::find_node_def(node.type);
        for (u32 pin = 0; def && pin < def->output_count; ++pin) {
            for (const script::GraphLink& l : graph.links) {
                if (l.from_node != node.id || l.from_pin != pin) continue;
                const script::GraphNode* child = graph.find(l.to_node);
                if (!child) continue;
                if (std::strcmp(child->type, "BtRoot") == 0) return fail("The Root cannot be a child", child);
                u32 child_index = 0;
                if (!add(*child, child_index)) return false;
                ai::BtNode& parent = out.nodes[index];  // (stable: a fixed array)
                if (parent.child_count < ai::kMaxBtChildren) parent.children[parent.child_count++] = child_index;
            }
        }
        return true;
    }
};

} // namespace

bool build_behavior_tree(const script::Graph& graph, ai::BehaviorTree& out, char* error, usize capacity) {
    out.count = 0;
    const script::GraphNode* root = nullptr;
    for (const script::GraphNode& n : graph.nodes) {
        if (std::strcmp(n.type, "BtRoot") != 0) continue;
        if (root) {
            std::snprintf(error, capacity, "Two Root nodes: keep one");
            return false;
        }
        root = &n;
    }
    if (!root) {
        std::snprintf(error, capacity, "No Root node");
        return false;
    }
    Builder b{graph, out, error, capacity};
    u32 index = 0;
    return b.add(*root, index);
}

bool load_behavior_tree(const char* path, ai::BehaviorTree& out, char* error, usize capacity) {
    script::Graph graph(default_allocator());
    if (!script::load_graph_file(graph, path)) {
        std::snprintf(error, capacity, "Could not read %s", path);
        return false;
    }
    return build_behavior_tree(graph, out, error, capacity);
}

} // namespace lsd::editor
