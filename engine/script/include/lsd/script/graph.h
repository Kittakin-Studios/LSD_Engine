#pragma once
// Visual scripts (.lsdgraph), Unreal's Blueprint event graph: nodes joined by wires. A graph is
// compiled to Lua and then runs exactly like a hand-written script (same events, timers, hot
// reload and error reporting), so the two can never drift apart.
//
// White "exec" wires give the order things happen in; coloured data wires carry values. Pure
// nodes (no exec pins) are read when something needs their value. Unconnected inputs use the
// value typed on the node.
#include "lsd/foundation/array.h"

namespace lsd::script {

enum class PinType : u8 { Exec, Float, Bool, String, Vector, Entity, Audio };

// Which node library a graph uses: visual scripts, Sound Cues or Material Graphs (same canvas, same
// file format). Material nodes are compiled to shader code by engine/render/material_graph.h.
enum class GraphKind : u8 { Script, SoundCue, Material, BehaviorTree };

struct PinDef {
    const char* name;
    PinType type;
    const char* default_value;  // as typed on the node; Entity "self" means the script's own object
};

struct NodeDef {
    const char* id;        // saved in the graph: "SetPosition"
    const char* title;     // shown: "Set Position"
    const char* category;  // node menu group: "Events", "Flow", "Transform"...
    const char* tooltip;
    PinDef inputs[6];
    u32 input_count;
    PinDef outputs[4];
    u32 output_count;
    // Lua for the node. Events: the function header. Exec nodes: a statement where $0.. are the
    // data inputs, $o0.. the node's output variables and #0.. where each exec output's chain goes
    // (a single "#0" is appended when absent). Pure nodes: unused, see output_code.
    const char* code;
    const char* output_code[4];  // pure nodes and events: the Lua expression of each data output
                                 // (material nodes: the shader expression, see material_graph.h)
};

const NodeDef* node_defs(u32& count, GraphKind kind = GraphKind::Script);
const NodeDef* find_node_def(const char* id);  // in either library
bool is_event(const NodeDef& def);
bool is_pure(const NodeDef& def);  // no exec pins
bool is_material_node(const NodeDef& def);

constexpr u32 kMaxPinValues = 6;

struct GraphNode {
    u32 id = 0;
    char type[32] = "";
    f32 x = 0, y = 0;                    // canvas position
    char values[kMaxPinValues][64] = {};  // typed values of the inputs, by input index
};

struct GraphLink {
    u32 from_node;  // output side
    u32 from_pin;
    u32 to_node;    // input side
    u32 to_pin;
};

struct Graph {
    explicit Graph(Allocator& allocator) : nodes(allocator), links(allocator) {}
    Array<GraphNode> nodes;
    Array<GraphLink> links;
    u32 next_id = 1;

    // New node of a known type at (x, y) with its inputs' default values; null if the type is unknown.
    GraphNode* add(const char* type, f32 x, f32 y);
    void remove(u32 node_id);  // and its wires
    GraphNode* find(u32 node_id);
    const GraphNode* find(u32 node_id) const;
    // Wires output (from, from_pin) to input (to, to_pin), replacing what that data input or exec
    // output had. False if the pin types do not match.
    bool connect(u32 from, u32 from_pin, u32 to, u32 to_pin);
    void disconnect_input(u32 node_id, u32 pin);
    const GraphLink* link_into(u32 node_id, u32 pin) const;  // the wire feeding an input (data inputs have one)
};

constexpr char kGraphExtension[] = ".lsdgraph";

void save_graph(const Graph& graph, Array<char>& out);
bool load_graph(Graph& graph, const char* text, usize length);
bool save_graph_file(const Graph& graph, const char* path);
bool load_graph_file(Graph& graph, const char* path);

// Lua for the graph. On a problem (unknown node, exec loop, an event used twice) returns false
// with a message naming the node.
bool compile_graph(const Graph& graph, Array<char>& lua, char* error, usize error_capacity);
// Reads a .lsdgraph and writes the Lua next to where the game loads it.
bool compile_graph_file(const char* graph_path, const char* lua_path, char* error, usize error_capacity);

} // namespace lsd::script
