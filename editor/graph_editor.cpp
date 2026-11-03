#include "graph_editor.h"
#include "workspaces.h"

#include "lua_debugger.h"

#include "lsd/foundation/log.h"
#include "sound_cue.h"

#include "behavior.h"
#include "material_cache.h"
#include "mesh_cache.h"

#include "lsd/rhi/rhi.h"
#include "lsd/script/material_graph.h"

#include <SDL3/SDL_timer.h>
#include "sound_player.h"
#include "lsd/script/graph.h"

#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <cstdlib>
#include <vector>
#include <fstream>
#include <string>

namespace lsd::editor {

namespace {

using script::Graph;
using script::GraphNode;
using script::NodeDef;
using script::PinType;

f32 g_zoom = 1.0f;  // the canvas zoom (mouse wheel); node sizes and positions scale with it
f32 header() { return 24.0f * g_zoom; }
f32 row() { return 24.0f * g_zoom; }
f32 pin_size() { return 6.0f * g_zoom; }
constexpr char kGraphLuaDir[] = "Saved/Graphs";

struct GraphWindow {
    Graph* graph = nullptr;
    script::GraphKind kind = script::GraphKind::Script;
    asset::Guid guid;
    char path[300] = "";
    char name[64] = "";
    bool open = false;
    bool dirty = false;
    ImVec2 scroll{40.0f, 40.0f};
    f32 zoom = 1.0f;
    u32 selected = 0;                // the primary selected node (F9, the Delete key's target with the rest)
    std::vector<u32> selection;      // every selected node (Ctrl / Shift click, box select)
    bool boxing = false;             // dragging a selection box on empty canvas
    ImVec2 box_start;
    u32 comment_resizing = 0;        // the Comment whose corner is being dragged
    std::vector<std::string> undo, redo;  // the graph as text before / after each finished edit
    std::string snapshot;                 // the graph as text after the last finished edit
    // A wire being dragged from a pin.
    bool wiring = false;
    u32 wire_node = 0, wire_pin = 0;
    bool wire_from_output = true;
    ImVec2 menu_at;  // canvas position for a node picked from the menu
    char search[64] = "";
    char palette_search[64] = "";
    i32 variable = -1;          // the variable selected in My Blueprint (its Details show), -1 none
    i32 drop_variable = -1;     // a variable dropped on the canvas, waiting for Get / Set
    ImVec2 drop_at;
    ImVec2 view_size{800.0f, 600.0f};  // the canvas on screen last frame (new nodes go to its middle)
    char message[2048] = "";  // a compile's errors: one line each
    bool message_is_error = false;
    // Material Graph preview: recompiled a moment after the graph last changed.
    rhi::Device* device = nullptr;
    rhi::MaterialShaderHandle preview_shader;
    rhi::TextureHandle preview_target;
    char preview_textures[3][33] = {};
    bool preview_srgb[3] = {};
    u32 preview_texture_count = 0;
    f32 preview_params[16][4] = {};
    u32 preview_param_count = 0;
    std::string preview_source;  // the graph as text when last compiled
    std::string preview_code;    // the shader code the preview shader was built from
    u64 changed_at = 0;          // ms; 0 = up to date
    bool preview_cube = false;
    char preview_error[256] = "";
};
GraphWindow g_window;
constexpr u32 kPreviewSize = 384;
const asset::AssetRegistry* g_registry = nullptr;  // for the Texture Sample's texture list
u32 g_active[64];  // behaviour tree nodes running in Play
u32 g_active_count = 0;

std::string graph_text(const Graph& g) {
    Array<char> text(default_allocator());
    script::save_graph(g, text);
    return std::string(text.data(), text.size());
}

bool is_selected(const GraphWindow& w, u32 id) {
    for (const u32 s : w.selection) {
        if (s == id) return true;
    }
    return false;
}

void select_only(GraphWindow& w, u32 id) {
    w.selected = id;
    w.selection.clear();
    if (id) w.selection.push_back(id);
}

// Copies the selected nodes and the wires between them as graph text (Ctrl+C: also to the clipboard).
std::string copy_selection(const GraphWindow& w) {
    Graph part(default_allocator());
    for (const u32 id : w.selection) {
        if (const GraphNode* n = w.graph->find(id)) part.nodes.push_back(*n);
    }
    for (const auto& l : w.graph->links) {
        if (is_selected(w, l.from_node) && is_selected(w, l.to_node)) part.links.push_back(l);
    }
    return graph_text(part);
}

// Adds a copied part with new ids, its first node at `at` (canvas units), and selects it. False if the text is not a graph.
bool paste(GraphWindow& w, const char* text, ImVec2 at) {
    Graph part(default_allocator());
    if (!text || !script::load_graph(part, text, std::strlen(text)) || part.nodes.size() == 0) return false;
    const f32 dx = at.x - part.nodes[0].x, dy = at.y - part.nodes[0].y;
    std::vector<std::pair<u32, u32>> ids;  // old -> new
    w.selection.clear();
    for (GraphNode n : part.nodes) {
        if (!script::find_node_def(n.type)) continue;
        ids.emplace_back(n.id, w.graph->next_id);
        n.id = w.graph->next_id++;
        n.x += dx;
        n.y += dy;
        w.graph->nodes.push_back(n);
        w.selection.push_back(n.id);
    }
    auto new_id = [&](u32 old) {
        for (const auto& [from, to] : ids) {
            if (from == old) return to;
        }
        return 0u;
    };
    for (const auto& l : part.links) {
        if (new_id(l.from_node) && new_id(l.to_node)) w.graph->connect(new_id(l.from_node), l.from_pin, new_id(l.to_node), l.to_pin);
    }
    w.selected = w.selection.empty() ? 0 : w.selection[0];
    w.dirty = true;
    return true;
}

ImU32 pin_color(PinType type) {
    switch (type) {
        case PinType::Any: return IM_COL32(170, 170, 170, 255);
        case PinType::Exec: return IM_COL32(240, 240, 240, 255);
        case PinType::Float: return IM_COL32(150, 230, 90, 255);
        case PinType::Bool: return IM_COL32(220, 50, 50, 255);
        case PinType::String: return IM_COL32(240, 110, 220, 255);
        case PinType::Vector: return IM_COL32(250, 200, 40, 255);
        case PinType::Entity: return IM_COL32(60, 160, 255, 255);
        case PinType::Audio: return IM_COL32(190, 140, 255, 255);
    }
    return IM_COL32_WHITE;
}

ImU32 header_color(const NodeDef& def) {
    if (std::strncmp(def.id, "Bt", 2) == 0) {
        if (std::strcmp(def.id, "BtRoot") == 0) return IM_COL32(85, 85, 90, 255);
        if (std::strcmp(def.category, "Composites") == 0) return IM_COL32(110, 70, 150, 255);
        if (std::strcmp(def.category, "Conditions") == 0) return IM_COL32(40, 90, 150, 255);
        return IM_COL32(150, 80, 40, 255);
    }
    if (script::is_material_node(def)) {
        if (std::strcmp(def.id, "MatOutput") == 0) return IM_COL32(150, 40, 40, 255);
        if (std::strcmp(def.category, "Texture") == 0) return IM_COL32(40, 90, 150, 255);
        if (std::strcmp(def.category, "Constants") == 0) return IM_COL32(110, 100, 40, 255);
        return IM_COL32(50, 110, 60, 255);
    }
    if (std::strcmp(def.category, "Sound Cue") == 0) return std::strcmp(def.id, "CueOutput") == 0 ? IM_COL32(150, 40, 40, 255) : IM_COL32(95, 60, 150, 255);
    if (script::is_event(def)) return IM_COL32(160, 30, 30, 255);
    if (script::is_pure(def)) return IM_COL32(50, 110, 60, 255);
    if (std::strcmp(def.category, "Flow") == 0) return IM_COL32(85, 85, 90, 255);
    return IM_COL32(40, 80, 150, 255);
}

f32 node_width(const NodeDef& def) {
    if (std::strncmp(def.id, "Reroute", 7) == 0) return 60.0f * g_zoom;  // a small knot in the wire
    for (u32 i = 0; i < def.input_count; ++i) {
        if (def.inputs[i].type == PinType::Vector) return (def.output_count > 1 || script::is_pure(def) ? 330.0f : 270.0f) * g_zoom;  // room for the output label
    }
    return 210.0f * g_zoom;
}

f32 node_height(const NodeDef& def) {
    const u32 rows = def.input_count > def.output_count ? def.input_count : def.output_count;
    return header() + row() * static_cast<f32>(rows > 0 ? rows : 1) + 6.0f * g_zoom;
}

ImVec2 pin_position(ImVec2 origin, const GraphNode& node, const NodeDef& def, bool output, u32 pin) {
    return ImVec2(origin.x + node.x * g_zoom + (output ? node_width(def) : 0.0f), origin.y + node.y * g_zoom + header() + row() * (static_cast<f32>(pin) + 0.5f));
}

void wire(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color) {
    const f32 bend = std::fabs(b.x - a.x) * 0.5f + 30.0f;
    dl->AddBezierCubic(a, ImVec2(a.x + bend, a.y), ImVec2(b.x - bend, b.y), b, color, 2.5f);
}

void draw_pin(ImDrawList* dl, ImVec2 at, PinType type, bool connected) {
    const ImU32 color = pin_color(type);
    if (type == PinType::Exec) {  // arrow
        const ImVec2 p[3] = {ImVec2(at.x - pin_size(), at.y - pin_size()), ImVec2(at.x + pin_size(), at.y), ImVec2(at.x - pin_size(), at.y + pin_size())};
        if (connected) dl->AddTriangleFilled(p[0], p[1], p[2], color);
        else dl->AddTriangle(p[0], p[1], p[2], color, 1.5f);
    } else if (connected) {
        dl->AddCircleFilled(at, pin_size(), color);
    } else {
        dl->AddCircle(at, pin_size(), color, 12, 1.5f);
    }
}

bool output_connected(const Graph& g, u32 node, u32 pin) {
    for (const auto& l : g.links) {
        if (l.from_node == node && l.from_pin == pin) return true;
    }
    return false;
}

// The typed value of an unconnected input, edited in place.
bool value_widget(const script::PinDef& pin, char* value, usize capacity) {
    bool changed = false;
    switch (pin.type) {
        case PinType::Float: {
            f32 v = static_cast<f32>(std::atof(value));
            ImGui::SetNextItemWidth(70.0f);
            if (ImGui::DragFloat("##v", &v, 0.05f, 0.0f, 0.0f, "%.3g")) {
                std::snprintf(value, capacity, "%g", static_cast<double>(v));
                changed = true;
            }
            break;
        }
        case PinType::Bool: {
            bool v = std::strcmp(value, "true") == 0;
            if (ImGui::Checkbox("##v", &v)) {
                std::snprintf(value, capacity, "%s", v ? "true" : "false");
                changed = true;
            }
            break;
        }
        case PinType::String:
            if (std::strcmp(pin.name, "Type") == 0 && std::strcmp(pin.default_value, "Color") == 0) {  // Texture Sample: how the texels are read
                static const char* const kinds[3] = {"Color", "Normal", "Data"};
                ImGui::SetNextItemWidth(100.0f);
                if (ImGui::BeginCombo("##v", value)) {
                    for (const char* k : kinds) {
                        if (ImGui::Selectable(k, std::strcmp(value, k) == 0)) {
                            std::snprintf(value, capacity, "%s", k);
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                break;
            }
            if (std::strcmp(pin.name, "Texture") == 0 && g_registry) {  // a texture asset by name
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::BeginCombo("##v", value[0] ? value : "(pick a texture)")) {
                    usize hits[256];
                    const usize count = g_registry->search(nullptr, asset::AssetType::Texture, nullptr, hits, 256);
                    for (usize i = 0; i < count; ++i) {
                        const char* name = g_registry->get(hits[i]).name;
                        if (ImGui::Selectable(name, std::strcmp(value, name) == 0)) {
                            std::snprintf(value, capacity, "%s", name);
                            changed = true;
                        }
                    }
                    if (!count) ImGui::TextDisabled("No textures: drop a PNG / JPG into the Content Browser");
                    ImGui::EndCombo();
                }
                break;
            }
            ImGui::SetNextItemWidth(100.0f);
            changed = ImGui::InputText("##v", value, capacity);
            break;
        case PinType::Any:  // a number, true / false or text, as typed
            ImGui::SetNextItemWidth(80.0f);
            changed = ImGui::InputText("##v", value, capacity);
            break;
        case PinType::Vector: {
            if (std::strcmp(value, "uv") == 0) {  // Material Graph: the mesh's own UVs unless something is wired in
                ImGui::TextDisabled("mesh UVs");
                break;
            }
            f32 v[3] = {0, 0, 0};
            std::sscanf(value, "%f , %f , %f", &v[0], &v[1], &v[2]);
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::DragFloat3("##v", v, 0.05f, 0.0f, 0.0f, "%.2g")) {
                std::snprintf(value, capacity, "%g,%g,%g", static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2]));
                changed = true;
            }
            break;
        }
        case PinType::Entity: ImGui::TextDisabled("self"); break;
        case PinType::Exec:
        case PinType::Audio: break;
    }
    return changed;
}

// The node each line of a compiled visual script belongs to (its "--@node N" markers); 0 = none.
struct LineMap {
    std::string path;
    std::filesystem::file_time_type time;
    std::vector<u32> node_of_line;  // [line]
};
LineMap g_lines;

const std::vector<u32>& node_lines(const std::string& path) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    if (g_lines.path == path && !error && time == g_lines.time) return g_lines.node_of_line;
    g_lines.path = path;
    g_lines.time = time;
    g_lines.node_of_line.assign(1, 0);
    std::ifstream file(path);
    u32 current = 0;
    for (std::string line; std::getline(file, line);) {
        if (line.rfind("--@node ", 0) == 0) current = static_cast<u32>(std::strtoul(line.c_str() + 8, nullptr, 10));
        g_lines.node_of_line.push_back(current);
    }
    return g_lines.node_of_line;
}

// The first line of a node's own code (just after its marker), 0 if it is not in the file.
i32 node_first_line(const std::string& path, u32 node) {
    const std::vector<u32>& lines = node_lines(path);
    for (size_t i = 1; i < lines.size(); ++i) {  // lines[i] is its marker (a comment): the code starts on the next line
        if (lines[i] == node) return static_cast<i32>(i + 1 < lines.size() && lines[i + 1] == node ? i + 1 : i);
    }
    return 0;
}

std::string lua_path_for(const asset::Guid& guid) {
    char text[33];
    asset::to_string(guid, text);
    return std::string(kGraphLuaDir) + "/" + text + ".lua";
}

void save_and_compile(EditorState& s) {
    GraphWindow& w = g_window;
    if (!w.graph) return;
    if (!script::save_graph_file(*w.graph, w.path)) {
        std::snprintf(w.message, sizeof w.message, "Could not save %s", w.path);
        w.message_is_error = true;
        return;
    }
    w.dirty = false;
    if (w.kind == script::GraphKind::BehaviorTree) {  // AI Controllers load it when Play starts
        ai::BehaviorTree tree;
        char problem[256] = "";
        const bool ok = build_behavior_tree(*w.graph, tree, problem, sizeof problem);
        std::snprintf(w.message, sizeof w.message, "%s", ok ? "Saved: used from the next Play" : problem);
        w.message_is_error = !ok;
        std::snprintf(s.status, sizeof s.status, "%s: %s", w.name, w.message);
        return;
    }
    if (w.kind == script::GraphKind::Material) {  // the editor compiles the shader when it sees the file change
        script::MaterialCode code(default_allocator());
        char problem[256] = "";
        const bool ok = script::compile_material_graph(*w.graph, code, problem, sizeof problem);
        std::snprintf(w.message, sizeof w.message, "%s", ok ? "Saved: objects using it update in a moment" : problem);
        w.message_is_error = !ok;
        std::snprintf(s.status, sizeof s.status, "%s: %s", w.name, w.message);
        return;
    }
    if (w.kind == script::GraphKind::SoundCue) {  // cues are read as they are: nothing to compile
        std::snprintf(w.message, sizeof w.message, "Saved");
        w.message_is_error = false;
        std::snprintf(s.status, sizeof s.status, "%s: saved", w.name);
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(kGraphLuaDir, error);
    char problem[2048];
    if (script::compile_graph_file(w.path, lua_path_for(w.guid).c_str(), problem, sizeof problem)) {
        std::snprintf(w.message, sizeof w.message, "Saved and compiled");
        w.message_is_error = false;
    } else {
        std::snprintf(w.message, sizeof w.message, "%s", problem);
        w.message_is_error = true;
        LSD_LOG_ERROR("graph", "%s: %s", w.name, problem);
    }
    std::snprintf(s.status, sizeof s.status, "%s: %.*s", w.name, static_cast<int>(std::strcspn(w.message, "\n")), w.message);
}

void node_menu(GraphWindow& w) {
    if (!ImGui::BeginPopup("AddNode")) return;
    if (ImGui::IsWindowAppearing()) {
        w.search[0] = '\0';
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##search", "Search nodes", w.search, sizeof w.search);
    u32 count = 0;
    const NodeDef* defs = script::node_defs(count, w.kind);
    auto matches = [&](const NodeDef& d) {
        if (!w.search[0]) return true;
        std::string title = d.title, needle = w.search;
        for (char& c : title) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return title.find(needle) != std::string::npos;
    };
    const char* category = "";
    for (u32 i = 0; i < count; ++i) {
        const NodeDef& d = defs[i];
        if (!matches(d)) continue;
        if (std::strcmp(category, d.category) != 0) {
            category = d.category;
            ImGui::SeparatorText(category);
        }
        if (ImGui::Selectable(d.title)) {
            if (GraphNode* n = w.graph->add(d.id, w.menu_at.x, w.menu_at.y)) select_only(w, n->id);
            w.dirty = true;
            ImGui::CloseCurrentPopup();
        }
        if (d.tooltip[0]) ImGui::SetItemTooltip("%s", d.tooltip);
    }
    ImGui::EndPopup();
}


// ---- Side panel: My Blueprint, Palette, Details (Unreal's Blueprint editor panels) ----

constexpr char kNodePayload[] = "LSD_GRAPH_NODE";
constexpr char kVariablePayload[] = "LSD_GRAPH_VAR";

// Where the middle of the visible canvas is, in canvas units (new nodes go there).
ImVec2 view_centre(const GraphWindow& w) {
    return ImVec2((w.view_size.x * 0.5f - w.scroll.x) / w.zoom - 80.0f, (w.view_size.y * 0.5f - w.scroll.y) / w.zoom - 30.0f);
}

// Scrolls the canvas so the node is in the middle, and selects it.
void focus_node(GraphWindow& w, const GraphNode& n) {
    w.scroll = ImVec2(w.view_size.x * 0.5f - (n.x + 80.0f) * w.zoom, w.view_size.y * 0.5f - (n.y + 30.0f) * w.zoom);
    select_only(w, n.id);
}

bool is_get_variable(const GraphNode& n) { return std::strcmp(n.type, "GetVariable") == 0; }
bool is_set_variable(const GraphNode& n) { return std::strcmp(n.type, "SetVariable") == 0; }
const char* variable_of(const GraphNode& n) { return is_get_variable(n) ? n.values[0] : is_set_variable(n) ? n.values[1] : nullptr; }

i32 find_variable(const Graph& g, const char* name) {
    for (usize i = 0; i < g.variables.size(); ++i) {
        if (std::strcmp(g.variables[i].name, name) == 0) return static_cast<i32>(i);
    }
    return -1;
}

// A Get or Set Variable node for a declared variable at canvas position `at`.
void add_variable_node(GraphWindow& w, const script::GraphVariable& v, bool set, ImVec2 at) {
    GraphNode* n = w.graph->add(set ? "SetVariable" : "GetVariable", at.x, at.y);
    if (!n) return;
    std::snprintf(n->values[set ? 1 : 0], sizeof n->values[0], "%s", v.name);
    if (set) std::snprintf(n->values[2], sizeof n->values[2], "%s", v.default_value);
    select_only(w, n->id);
    w.dirty = true;
}

ImVec4 type_colour(script::PinType t) {  // the wire colours of the canvas
    switch (t) {
        case script::PinType::Float: return ImVec4(0.55f, 0.85f, 0.45f, 1.0f);
        case script::PinType::Bool: return ImVec4(0.85f, 0.25f, 0.25f, 1.0f);
        case script::PinType::String: return ImVec4(0.95f, 0.45f, 0.85f, 1.0f);
        case script::PinType::Vector: return ImVec4(0.95f, 0.8f, 0.2f, 1.0f);
        case script::PinType::Entity: return ImVec4(0.3f, 0.6f, 1.0f, 1.0f);
        default: return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
    }
}

// A list header with a + button on the right (Unreal's My Blueprint sections).
bool section(const char* title, bool* add, const char* add_tip) {
    const bool open = ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
    if (add) {
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 14.0f);
        ImGui::PushID(title);
        *add = ImGui::SmallButton("+");
        ImGui::SetItemTooltip("%s", add_tip);
        ImGui::PopID();
    }
    return open;
}

void draw_my_blueprint(GraphWindow& w) {
    Graph& g = *w.graph;
    bool add = false;
    // Graphs: the built-in events that start chains (Unreal's Event Graph entries).
    if (section("Event Graph", nullptr, nullptr)) {
        for (const GraphNode& n : g.nodes) {
            const script::NodeDef* def = script::find_node_def(n.type);
            if (!def || !script::is_event(*def) || std::strcmp(n.type, "FunctionEntry") == 0 || std::strcmp(n.type, "CustomEvent") == 0) continue;
            char label[96];
            std::snprintf(label, sizeof label, "%s##%u", def->title, n.id);
            if (ImGui::Selectable(label, w.selected == n.id)) focus_node(w, n);
        }
    }
    // Event Dispatchers: this script's Custom Events; drag one out for a Call Event that fires it.
    if (section("Event Dispatchers", &add, "Add an Event Dispatcher (a Custom Event other scripts can call)")) {
        for (const GraphNode& n : g.nodes) {
            if (std::strcmp(n.type, "CustomEvent") != 0) continue;
            char label[96];
            std::snprintf(label, sizeof label, "%s##%u", n.values[0], n.id);
            if (ImGui::Selectable(label, w.selected == n.id)) focus_node(w, n);
            ImGui::SetItemTooltip("Drag onto the graph: Call Event %s (every object that has it)", n.values[0]);
            if (ImGui::BeginDragDropSource()) {
                char payload[48];
                std::snprintf(payload, sizeof payload, "CallEvent:%s", n.values[0]);
                ImGui::SetDragDropPayload(kNodePayload, payload, sizeof payload);
                ImGui::Text("Call %s", n.values[0]);
                ImGui::EndDragDropSource();
            }
        }
    }
    if (add) {
        char name[32] = "NewEventDispatcher";
        for (int k = 2;; ++k) {  // a name no other Custom Event has
            bool taken = false;
            for (const GraphNode& n : g.nodes) taken = taken || (std::strcmp(n.type, "CustomEvent") == 0 && std::strcmp(n.values[0], name) == 0);
            if (!taken) break;
            std::snprintf(name, sizeof name, "NewEventDispatcher%d", k);
        }
        const ImVec2 at = view_centre(w);
        if (GraphNode* n = g.add("CustomEvent", at.x, at.y)) {
            std::snprintf(n->values[0], sizeof n->values[0], "%s", name);
            select_only(w, n->id);
            focus_node(w, *n);
        }
        w.dirty = true;
    }
    // Functions.
    add = false;
    if (section("Functions", &add, "Add a Function (Call Function runs it by name)")) {
        for (const GraphNode& n : g.nodes) {
            if (std::strcmp(n.type, "FunctionEntry") != 0) continue;
            char label[96];
            std::snprintf(label, sizeof label, "%s()##%u", n.values[0], n.id);
            if (ImGui::Selectable(label, w.selected == n.id)) focus_node(w, n);
            if (ImGui::BeginDragDropSource()) {  // drop on the canvas: a Call Function for it
                char payload[32];
                std::snprintf(payload, sizeof payload, "CallFunction:%s", n.values[0]);
                ImGui::SetDragDropPayload(kNodePayload, payload, sizeof payload);
                ImGui::Text("Call %s", n.values[0]);
                ImGui::EndDragDropSource();
            }
        }
    }
    if (add) {
        char name[32] = "NewFunction";
        for (int k = 2;; ++k) {  // a name no other Function has
            bool taken = false;
            for (const GraphNode& n : g.nodes) taken = taken || (std::strcmp(n.type, "FunctionEntry") == 0 && std::strcmp(n.values[0], name) == 0);
            if (!taken) break;
            std::snprintf(name, sizeof name, "NewFunction%d", k);
        }
        const ImVec2 at = view_centre(w);
        if (GraphNode* n = g.add("FunctionEntry", at.x, at.y)) {
            std::snprintf(n->values[0], sizeof n->values[0], "%s", name);
            focus_node(w, *n);
        }
        w.dirty = true;
    }
    // Variables: declared ones (drag onto the canvas: Get, Ctrl Get, Alt Set), then names the graph
    // uses without declaring them.
    add = false;
    if (section("Variables", &add, "Add a variable")) {
        for (usize i = 0; i < g.variables.size(); ++i) {
            const script::GraphVariable& v = g.variables[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::ColorButton("##type", type_colour(v.type), ImGuiColorEditFlags_NoTooltip, ImVec2(10, 10));
            ImGui::SameLine();
            char label[96];
            std::snprintf(label, sizeof label, "%s%s", v.name, v.instance_editable ? "  (editable)" : "");
            if (ImGui::Selectable(label, w.variable == static_cast<i32>(i))) {
                w.variable = static_cast<i32>(i);
                select_only(w, 0);
            }
            if (v.tooltip[0]) ImGui::SetItemTooltip("%s", v.tooltip);
            if (ImGui::BeginDragDropSource()) {
                const u32 index = static_cast<u32>(i);
                ImGui::SetDragDropPayload(kVariablePayload, &index, sizeof index);
                ImGui::Text("%s (%s): drop for Get / Set; Ctrl = Get, Alt = Set", v.name, script::pin_type_name(v.type));
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
        }
        std::vector<std::string> undeclared;
        for (const GraphNode& n : g.nodes) {
            const char* name = variable_of(n);
            if (!name || !name[0] || find_variable(g, name) >= 0 || std::find(undeclared.begin(), undeclared.end(), name) != undeclared.end()) continue;
            undeclared.emplace_back(name);
        }
        for (const std::string& name : undeclared) {
            ImGui::PushID(name.c_str());
            ImGui::TextDisabled("%s (used, not declared)", name.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Declare")) {
                script::GraphVariable v;
                std::snprintf(v.name, sizeof v.name, "%s", name.c_str());
                v.type = script::PinType::Any;
                g.variables.push_back(v);
                w.variable = static_cast<i32>(g.variables.size()) - 1;
                w.dirty = true;
            }
            ImGui::PopID();
        }
    }
    if (add) {
        script::GraphVariable v;
        std::snprintf(v.name, sizeof v.name, "NewVar");
        for (int k = 2; find_variable(g, v.name) >= 0; ++k) std::snprintf(v.name, sizeof v.name, "NewVar%d", k);
        g.variables.push_back(v);
        w.variable = static_cast<i32>(g.variables.size()) - 1;
        select_only(w, 0);
        w.dirty = true;
    }
}

// Details of the selected variable (Unreal's Details for a Blueprint variable).
void draw_variable_details(GraphWindow& w) {
    Graph& g = *w.graph;
    if (w.variable < 0 || static_cast<usize>(w.variable) >= g.variables.size()) return;
    script::GraphVariable& v = g.variables[static_cast<usize>(w.variable)];
    ImGui::SeparatorText("Details");
    char name[32];
    std::snprintf(name, sizeof name, "%s", v.name);
    ImGui::SetNextItemWidth(-60.0f);
    if (ImGui::InputText("Name", name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue) && name[0] && find_variable(g, name) < 0) {
        for (GraphNode& n : g.nodes) {  // renaming moves every Get / Set with it
            char* used = is_get_variable(n) ? n.values[0] : is_set_variable(n) ? n.values[1] : nullptr;
            if (used && std::strcmp(used, v.name) == 0) std::snprintf(used, sizeof n.values[0], "%s", name);
        }
        std::snprintf(v.name, sizeof v.name, "%s", name);
        w.dirty = true;
    }
    ImGui::SetItemTooltip("Enter to rename (every Get / Set follows)");
    const script::PinType types[] = {script::PinType::Float, script::PinType::Bool, script::PinType::String, script::PinType::Vector, script::PinType::Entity, script::PinType::Any};
    ImGui::SetNextItemWidth(-60.0f);
    if (ImGui::BeginCombo("Type", script::pin_type_name(v.type))) {
        for (const script::PinType t : types) {
            if (!ImGui::Selectable(script::pin_type_name(t), t == v.type) || t == v.type) continue;
            v.type = t;
            std::snprintf(v.default_value, sizeof v.default_value, "%s",
                          t == script::PinType::Bool ? "false" : t == script::PinType::Vector ? "0,0,0" : t == script::PinType::Float || t == script::PinType::Any ? "0" : "");
            w.dirty = true;
        }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(-60.0f);
    switch (v.type) {
        case script::PinType::Float:
        case script::PinType::Any: {
            float f = static_cast<float>(std::atof(v.default_value));
            if (ImGui::DragFloat("Default", &f, 0.1f)) {
                std::snprintf(v.default_value, sizeof v.default_value, "%g", static_cast<double>(f));
                w.dirty = true;
            }
            break;
        }
        case script::PinType::Bool: {
            bool b = std::strcmp(v.default_value, "true") == 0;
            if (ImGui::Checkbox("Default", &b)) {
                std::snprintf(v.default_value, sizeof v.default_value, "%s", b ? "true" : "false");
                w.dirty = true;
            }
            break;
        }
        case script::PinType::Vector: {
            float xyz[3] = {};
            std::sscanf(v.default_value, "%f , %f , %f", &xyz[0], &xyz[1], &xyz[2]);
            if (ImGui::DragFloat3("Default", xyz, 0.1f)) {
                std::snprintf(v.default_value, sizeof v.default_value, "%g,%g,%g", static_cast<double>(xyz[0]), static_cast<double>(xyz[1]), static_cast<double>(xyz[2]));
                w.dirty = true;
            }
            break;
        }
        case script::PinType::Entity: ImGui::TextDisabled("Objects start empty (set them in a graph)"); break;
        default:
            if (ImGui::InputText("Default", v.default_value, sizeof v.default_value)) w.dirty = true;
            break;
    }
    const bool scalar = v.type == script::PinType::Float || v.type == script::PinType::Bool || v.type == script::PinType::String;
    ImGui::BeginDisabled(!scalar);
    if (ImGui::Checkbox("Instance Editable", &v.instance_editable)) w.dirty = true;
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(scalar ? "Each object using this script can change it in Details (under its Script)" : "Numbers, booleans and text only");
    ImGui::SetNextItemWidth(-60.0f);
    if (ImGui::InputText("Tooltip", v.tooltip, sizeof v.tooltip)) w.dirty = true;
    u32 uses = 0;
    for (const GraphNode& n : g.nodes) {
        const char* used = variable_of(n);
        uses += used && std::strcmp(used, v.name) == 0 ? 1u : 0u;
    }
    ImGui::TextDisabled("%u node%s use it", uses, uses == 1 ? "" : "s");
    ImGui::SameLine();
    if (ImGui::SmallButton("Find")) {  // the next node using it
        const GraphNode* first = nullptr;
        const GraphNode* after = nullptr;
        for (const GraphNode& n : g.nodes) {
            const char* used = variable_of(n);
            if (!used || std::strcmp(used, v.name) != 0) continue;
            if (!first) first = &n;
            if (!after && n.id > w.selected) after = &n;
        }
        if (after || first) {
            const i32 keep = w.variable;
            focus_node(w, after ? *after : *first);
            w.variable = keep;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Delete")) {
        g.variables[static_cast<usize>(w.variable)] = g.variables[g.variables.size() - 1];
        g.variables.resize(g.variables.size() - 1);
        w.variable = -1;
        w.dirty = true;
    }
}

// Every node of this graph kind by category, searchable; double-click or drag onto the canvas.
void draw_palette(GraphWindow& w) {
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##palette_search", "Search nodes", w.palette_search, sizeof w.palette_search);
    std::string needle = w.palette_search;
    for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    u32 count = 0;
    const NodeDef* defs = script::node_defs(count, w.kind);
    // Categories in order of first appearance, each listing all of its nodes (the library is not grouped).
    std::vector<const char*> categories;
    for (u32 i = 0; i < count; ++i) {
        bool seen = false;
        for (const char* c : categories) seen = seen || std::strcmp(c, defs[i].category) == 0;
        if (!seen) categories.push_back(defs[i].category);
    }
    for (const char* category : categories) {
        bool open = false;
        bool header = false;
        for (u32 i = 0; i < count; ++i) {
            const NodeDef& d = defs[i];
            if (std::strcmp(d.category, category) != 0) continue;
            std::string title = d.title;
            for (char& c : title) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!needle.empty() && title.find(needle) == std::string::npos) continue;
            if (!header) {
                header = true;
                open = ImGui::TreeNodeEx(category, needle.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen);
            }
            if (!open) continue;
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(d.title, false, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                const ImVec2 at = view_centre(w);
                if (GraphNode* n = w.graph->add(d.id, at.x, at.y)) focus_node(w, *n);
                w.dirty = true;
            }
            if (d.tooltip[0]) ImGui::SetItemTooltip("%s", d.tooltip);
            if (ImGui::BeginDragDropSource()) {
                char payload[32];
                std::snprintf(payload, sizeof payload, "%s", d.id);
                ImGui::SetDragDropPayload(kNodePayload, payload, sizeof payload);
                ImGui::TextUnformatted(d.title);
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
        }
        if (open) ImGui::TreePop();
    }
}

void draw_side_panel(GraphWindow& w) {
    if (ImGui::BeginTabBar("side_tabs")) {
        if (w.kind == script::GraphKind::Script && ImGui::BeginTabItem("My Blueprint")) {
            ImGui::BeginChild("my_blueprint", ImVec2(0, w.variable >= 0 ? -230.0f : 0.0f));
            draw_my_blueprint(w);
            ImGui::EndChild();
            draw_variable_details(w);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Palette")) {
            ImGui::BeginChild("palette");
            draw_palette(w);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// Drops from the side panel onto the canvas: a node from the Palette (or a Call Function), a
// variable as Get / Set (Ctrl: Get, Alt: Set, else a small menu like Unreal's).
void canvas_drops(GraphWindow& w, ImVec2 canvas, ImVec2 size, ImVec2 at) {
    ImGui::SetCursorScreenPos(canvas);
    ImGui::Dummy(size);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kNodePayload)) {
            const char* id = static_cast<const char*>(p->Data);
            if (std::strncmp(id, "CallFunction:", 13) == 0 || std::strncmp(id, "CallEvent:", 10) == 0) {  // a Function or Event Dispatcher dragged out
                const char* colon = std::strchr(id, ':');
                const std::string type(id, colon);
                if (GraphNode* n = w.graph->add(type.c_str(), at.x, at.y)) {
                    std::snprintf(n->values[1], sizeof n->values[1], "%s", colon + 1);
                    select_only(w, n->id);
                }
            } else if (GraphNode* n = w.graph->add(id, at.x, at.y)) {
                select_only(w, n->id);
            }
            w.dirty = true;
        }
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kVariablePayload)) {
            u32 index = 0;
            std::memcpy(&index, p->Data, sizeof index);
            const ImGuiIO& io = ImGui::GetIO();
            if (index < w.graph->variables.size() && (io.KeyCtrl || io.KeyAlt)) {
                add_variable_node(w, w.graph->variables[index], io.KeyAlt, at);
            } else if (index < w.graph->variables.size()) {
                w.drop_variable = static_cast<i32>(index);
                w.drop_at = at;
                ImGui::OpenPopup("VariableDrop");
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopup("VariableDrop")) {
        if (w.drop_variable >= 0 && static_cast<usize>(w.drop_variable) < w.graph->variables.size()) {
            const script::GraphVariable& v = w.graph->variables[static_cast<usize>(w.drop_variable)];
            if (ImGui::MenuItem((std::string("Get ") + v.name).c_str())) add_variable_node(w, v, false, w.drop_at);
            if (ImGui::MenuItem((std::string("Set ") + v.name).c_str())) add_variable_node(w, v, true, w.drop_at);
        }
        ImGui::EndPopup();
    }
}

} // namespace

std::string create_graph_file(const char* folder_path, const char* base) {
    const std::filesystem::path folder(folder_path);
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    std::filesystem::path path = folder / (std::string(base) + script::kGraphExtension);
    for (int n = 2; std::filesystem::exists(path, error); ++n) path = folder / (std::string(base) + std::to_string(n) + script::kGraphExtension);
    Graph graph(default_allocator());
    graph.add("OnStart", 0, 0);
    graph.add("OnUpdate", 0, 160);
    return script::save_graph_file(graph, path.generic_string().c_str()) ? path.generic_string() : std::string();
}

std::string create_cue_file(const char* folder_path, const char* base) {
    const std::filesystem::path folder(folder_path);
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    std::filesystem::path path = folder / (std::string(base) + kCueExtension);
    for (int n = 2; std::filesystem::exists(path, error); ++n) path = folder / (std::string(base) + std::to_string(n) + kCueExtension);
    Graph graph(default_allocator());
    const u32 wave = graph.add("CueWave", 0, 40)->id;
    const u32 output = graph.add("CueOutput", 320, 40)->id;
    graph.connect(wave, 0, output, 0);
    return script::save_graph_file(graph, path.generic_string().c_str()) ? path.generic_string() : std::string();
}

void set_graph_active_nodes(const u32* ids, u32 count) {
    g_active_count = count < 64 ? count : 64;
    std::memcpy(g_active, ids, g_active_count * sizeof(u32));
}

bool open_behavior_tree(asset::Guid& out) {
    if (!g_window.open || g_window.kind != script::GraphKind::BehaviorTree) return false;
    out = g_window.guid;
    return true;
}

std::string create_behavior_tree_file(const char* folder_path, const char* base) {
    const std::filesystem::path folder(folder_path);
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    std::filesystem::path path = folder / (std::string(base) + kBehaviorTreeExtension);
    for (int n = 2; std::filesystem::exists(path, error); ++n) path = folder / (std::string(base) + std::to_string(n) + kBehaviorTreeExtension);
    Graph graph(default_allocator());
    const u32 root = graph.add("BtRoot", 0, 0)->id;
    const u32 sequence = graph.add("BtSequence", 220, 0)->id;
    const u32 pick = graph.add("BtRandomLocation", 520, -40)->id;
    const u32 move = graph.add("BtMoveTo", 520, 80)->id;
    script::GraphNode* move_node = graph.find(move);
    std::snprintf(move_node->values[1], sizeof move_node->values[1], "PatrolLocation");
    const u32 wait = graph.add("BtWait", 520, 200)->id;
    graph.connect(root, 0, sequence, 0);
    graph.connect(sequence, 0, pick, 0);
    graph.connect(sequence, 1, move, 0);
    graph.connect(sequence, 2, wait, 0);
    return script::save_graph_file(graph, path.generic_string().c_str()) ? path.generic_string() : std::string();
}

std::string create_material_graph_file(const char* folder_path, const char* base) {
    const std::filesystem::path folder(folder_path);
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    std::filesystem::path path = folder / (std::string(base) + script::kMaterialGraphExtension);
    for (int n = 2; std::filesystem::exists(path, error); ++n) path = folder / (std::string(base) + std::to_string(n) + script::kMaterialGraphExtension);
    Graph graph(default_allocator());
    script::GraphNode* color = graph.add("MatColor", 0, 40);
    std::snprintf(color->values[0], sizeof color->values[0], "0.8,0.8,0.8");
    const u32 color_id = color->id;
    const u32 output = graph.add("MatOutput", 380, 0)->id;
    graph.connect(color_id, 0, output, 0);
    return script::save_graph_file(graph, path.generic_string().c_str()) ? path.generic_string() : std::string();
}

bool graph_lua(const asset::AssetRegistry& registry, const asset::AssetRecord& record, char* lua_path, usize capacity) {
    char path[300];
    registry.source_path(record, path, sizeof path);
    const std::string lua = lua_path_for(record.guid);
    std::snprintf(lua_path, capacity, "%s", lua.c_str());
    std::error_code error;
    const auto lua_time = std::filesystem::last_write_time(lua, error);
    if (!error && lua_time >= std::filesystem::last_write_time(path, error) && !error) {
        std::ifstream compiled(lua);
        std::string first, second;
        std::getline(compiled, first);
        for (std::string line; std::getline(compiled, line);) {
            if (line.rfind("--@node", 0) == 0) return true;  // up to date (with the debugger's node markers)
        }
    }
    std::filesystem::create_directories(kGraphLuaDir, error);
    char problem[256];
    if (script::compile_graph_file(path, lua.c_str(), problem, sizeof problem)) return true;
    LSD_LOG_ERROR("graph", "%s: %s", record.name, problem);
    return false;
}

void open_graph(EditorState& s, const asset::AssetRecord& record) {
    GraphWindow& w = g_window;
    if (w.graph && w.dirty) save_and_compile(s);  // switching graphs keeps the edits
    if (!w.graph) w.graph = new Graph(default_allocator());
    s.registry->source_path(record, w.path, sizeof w.path);
    if (!script::load_graph_file(*w.graph, w.path)) {
        std::snprintf(s.status, sizeof s.status, "Could not open %s", record.path);
        w.open = false;
        return;
    }
    w.guid = record.guid;
    w.kind = record.type == asset::AssetType::Cue ? script::GraphKind::SoundCue
             : record.type == asset::AssetType::MaterialGraph ? script::GraphKind::Material
             : record.type == asset::AssetType::BehaviorTree  ? script::GraphKind::BehaviorTree
                                                              : script::GraphKind::Script;
    std::snprintf(w.name, sizeof w.name, "%s", record.name);
    w.open = true;
    w.dirty = false;
    w.selected = 0;
    w.selection.clear();
    w.undo.clear();
    w.variable = w.drop_variable = -1;
    w.redo.clear();
    w.snapshot = graph_text(*w.graph);
    w.wiring = false;
    w.message[0] = '\0';
    w.preview_source.clear();  // a new graph: preview it straight away
    w.preview_code.clear();
    w.preview_error[0] = '\0';
    ImGui::SetWindowFocus("###VisualScript");
}

namespace {

// Compiles the graph into the preview shader when it has changed (and then stayed still a moment).
void update_preview(EditorState& s, GraphWindow& w) {
    Array<char> text(default_allocator());
    script::save_graph(*w.graph, text);
    const std::string now(text.data(), text.size());
    if (now != w.preview_source) {
        w.preview_source = now;
        w.changed_at = SDL_GetTicks() + (w.preview_shader.valid() ? 300 : 0);  // the first one at once
    }
    if (!w.changed_at || SDL_GetTicks() < w.changed_at || !s.device) return;
    w.changed_at = 0;
    w.device = s.device;
    script::MaterialCode code(default_allocator());
    if (!script::compile_material_graph(*w.graph, code, w.preview_error, sizeof w.preview_error)) return;
    // Building a shader stalls the editor (D3D12 compiles every pipeline variant), so only when the
    // generated code changed: moving nodes, comments and parameter values (a constant buffer) do not.
    if (!w.preview_shader.valid() || w.preview_code != code.code.data()) {
        const u64 started = SDL_GetTicks();
        const rhi::MaterialShaderHandle shader = s.device->create_material_shader(code.code.data());
        if (!shader.valid()) {
            std::snprintf(w.preview_error, sizeof w.preview_error, "The shader did not compile: see the Output Log");
            return;
        }
        LSD_LOG_INFO("editor", "Material preview rebuilt in %llu ms", static_cast<unsigned long long>(SDL_GetTicks() - started));
        if (w.preview_shader.valid()) s.device->destroy_material_shader(w.preview_shader);
        w.preview_shader = shader;
        w.preview_code = code.code.data();
    }
    w.preview_error[0] = '\0';
    w.preview_param_count = code.param_count;
    for (u32 i = 0; i < code.param_count; ++i) std::memcpy(w.preview_params[i], code.params[i].value, sizeof w.preview_params[i]);
    w.preview_texture_count = code.texture_count;
    for (u32 i = 0; i < code.texture_count; ++i) {
        find_texture_guid(*s.registry, code.textures[i].name, w.preview_textures[i]);
        w.preview_srgb[i] = code.textures[i].srgb;
    }
}

void draw_preview_panel(EditorState& s, GraphWindow& w) {
    update_preview(s, w);
    if (!w.preview_target.valid() && s.device) w.preview_target = s.device->create_render_target(kPreviewSize, kPreviewSize);
    const f32 side = ImGui::GetContentRegionAvail().x;
    if (w.preview_target.valid() && s.device) {
        const bool flipped = s.device->render_targets_flipped();
        ImGui::Image(static_cast<ImTextureID>(s.device->debug_ui_texture(w.preview_target)), ImVec2(side, side), ImVec2(0, flipped ? 1.0f : 0.0f),
                     ImVec2(1, flipped ? 0.0f : 1.0f));
    }
    if (ImGui::RadioButton("Sphere", !w.preview_cube)) w.preview_cube = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Cube", w.preview_cube)) w.preview_cube = true;
    if (w.preview_error[0]) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", w.preview_error);
    else ImGui::TextDisabled("Live: follows your edits. Save to update the level.");
}

} // namespace

void render_graph_preview(EditorState& s, const f32 background[4]) {
    GraphWindow& w = g_window;
    if (!w.open || w.kind != script::GraphKind::Material || !w.preview_shader.valid() || !w.preview_target.valid() || !s.device || !s.meshes) return;
    const CachedMesh* mesh = s.meshes->get(w.preview_cube ? "Cube" : "Sphere");
    if (!mesh) return;
    rhi::Device& device = *s.device;
    const f32 time = static_cast<f32>(SDL_GetTicks()) * 0.001f;
    const Vec3 center = (mesh->bounds_min + mesh->bounds_max) * 0.5f;
    const f32 radius = length(mesh->bounds_max - mesh->bounds_min) * (w.preview_cube ? 0.42f : 0.32f);  // fill the frame (the box's diagonal overstates a sphere)
    const f32 yaw = time * 0.4f, pitch = 0.35f, distance = radius / std::sin(0.3f) * 1.05f;
    const Vec3 eye = center + Vec3{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)} * distance;
    const Mat4 view_proj = perspective(0.6f, 1.0f, distance * 0.05f, distance * 3.0f) * look_at(eye, center, Vec3{0, 0, 1});
    device.set_render_target(w.preview_target, background);
    device.set_shadow_map({}, {});
    device.set_lights(rhi::default_lights());
    rhi::MeshDraw draw{};
    draw.vertices = mesh->gpu.vertices;
    draw.indices = mesh->gpu.indices;
    draw.index_count = mesh->gpu.index_count;
    std::memcpy(draw.mvp, view_proj.m, sizeof draw.mvp);
    const f32 identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::memcpy(draw.normal_matrix, identity, sizeof identity);
    std::memcpy(draw.world, identity, sizeof identity);
    draw.camera_position[0] = eye.x;
    draw.camera_position[1] = eye.y;
    draw.camera_position[2] = eye.z;
    const f32 white[4] = {1, 1, 1, 1};
    std::memcpy(draw.color, white, sizeof white);
    draw.material_shader = w.preview_shader;
    draw.time = time;
    draw.material_params = &w.preview_params[0][0];  // parameters at their defaults
    draw.material_param_count = w.preview_param_count;
    rhi::TextureHandle* slots[3] = {&draw.base_color_texture, &draw.normal_texture, &draw.orm_texture};
    for (u32 i = 0; i < w.preview_texture_count; ++i) *slots[i] = w.preview_textures[i][0] && s.textures ? s.textures->get(w.preview_textures[i], w.preview_srgb[i]) : rhi::TextureHandle{};
    device.draw_mesh(draw);
    device.set_render_target({}, nullptr);
}

void shutdown_graph_editor() {
    if (g_window.device) {
        if (g_window.preview_shader.valid()) g_window.device->destroy_material_shader(g_window.preview_shader);
        if (g_window.preview_target.valid()) g_window.device->destroy_texture(g_window.preview_target);
    }
    g_window.preview_shader = {};
    g_window.preview_target = {};
    delete g_window.graph;
    g_window.graph = nullptr;
    g_window.open = false;
}

void draw_graph_editor(EditorState& s) {
    GraphWindow& w = g_window;
    if (!w.open || !w.graph) return;
    Graph& g = *w.graph;
    char title[128];
    g_registry = s.registry;
    std::snprintf(title, sizeof title, "%s%s - %s###VisualScript", w.name, w.dirty ? "*" : "",
                  w.kind == script::GraphKind::SoundCue        ? "Sound Cue"
                  : w.kind == script::GraphKind::Material      ? "Material Graph"
                  : w.kind == script::GraphKind::BehaviorTree ? "Behavior Tree"
                                                               : "Visual Script");
    const ImGuiViewport* main = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(main->WorkPos.x + main->WorkSize.x * 0.5f, main->WorkPos.y + main->WorkSize.y * 0.45f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(1000, 620), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!begin_editor(title, &open)) return;  // a panel tab, or the workspace's editor area
    if (!open) {
        if (w.dirty) save_and_compile(s);
        w.open = false;
    }
    const bool cue = w.kind == script::GraphKind::SoundCue;
    if (ImGui::Button(w.kind == script::GraphKind::Script ? "Save + Compile" : "Save")) save_and_compile(s);
    ImGui::SetItemTooltip(cue ? "Ctrl+S" : "Ctrl+S. Playing objects pick the change up within half a second");
    ImGui::SameLine();
    if (cue && s.sounds) {  // hear it as the game would (not spatialised): saves first
        static u32 preview = 0;
        const bool playing = s.sounds->audio()->playing(preview);
        if (ImGui::Button(playing ? "Stop" : "Play")) {
            if (playing) {
                s.sounds->audio()->stop(preview);
            } else {
                if (w.dirty) save_and_compile(s);
                char guid[33];
                asset::to_string(w.guid, guid);
                preview = s.sounds->play(guid, false, {});
            }
        }
        ImGui::SameLine();
    }
    if (w.message[0] && w.message_is_error && std::strchr(w.message, '\n')) {  // Compiler Results: a line per problem, click to go to its node
        ImGui::NewLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.4f, 0.4f, 1));
        int k = 0;
        for (const char* line = w.message; *line; ++k) {
            const char* end = std::strchr(line, '\n');
            const std::string text(line, end ? end : line + std::strlen(line));
            line = end ? end + 1 : line + text.size();
            ImGui::PushID(k);
            const char* at = std::strstr(text.c_str(), "(node ");
            if (ImGui::Selectable(text.c_str()) && at) {
                if (const GraphNode* n = w.graph->find(static_cast<u32>(std::strtoul(at + 6, nullptr, 10)))) {
                    select_only(w, n->id);
                    focus_node(w, *n);
                }
            }
            ImGui::PopID();
        }
        ImGui::PopStyleColor();
    } else if (w.message[0]) ImGui::TextColored(w.message_is_error ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(0.5f, 1, 0.5f, 1), "%s", w.message);
    else ImGui::TextDisabled("Right-click: add node. Drag from a pin: wire. Middle / right drag: pan. Wheel: zoom. Drag on empty space: select. Ctrl+C / V / D / Z / Y.");

    const bool material = w.kind == script::GraphKind::Material;
    const f32 preview_width = material ? 300.0f : 0.0f;
    ImGui::BeginChild("Side", ImVec2(260.0f, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    draw_side_panel(w);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("Canvas", ImVec2(material ? -preview_width : 0.0f, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 canvas = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    const ImVec2 origin(canvas.x + w.scroll.x, canvas.y + w.scroll.y);
    const ImGuiIO& io = ImGui::GetIO();
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    g_zoom = w.zoom;
    ImGui::SetWindowFontScale(w.zoom);  // labels and the typed values scale with the nodes
    auto to_canvas = [&](ImVec2 screen) { return ImVec2((screen.x - origin.x) / w.zoom, (screen.y - origin.y) / w.zoom); };

    // Grid.
    const f32 cell = 32.0f * w.zoom;
    dl->AddRectFilled(canvas, ImVec2(canvas.x + size.x, canvas.y + size.y), IM_COL32(28, 28, 32, 255));
    for (f32 x = std::fmod(w.scroll.x, cell); x < size.x; x += cell) dl->AddLine(ImVec2(canvas.x + x, canvas.y), ImVec2(canvas.x + x, canvas.y + size.y), IM_COL32(45, 45, 52, 255));
    for (f32 y = std::fmod(w.scroll.y, cell); y < size.y; y += cell) dl->AddLine(ImVec2(canvas.x, canvas.y + y), ImVec2(canvas.x + size.x, canvas.y + y), IM_COL32(45, 45, 52, 255));

    // Comments: labelled boxes behind everything. Dragging the title moves the box and the nodes in it.
    bool hovered_item = false;
    for (usize i = 0; i < g.nodes.size(); ++i) {
        GraphNode& c = g.nodes[i];
        if (std::strcmp(c.type, "Comment") != 0) continue;
        const f32 cw = std::atof(c.values[1]) > 40.0 ? static_cast<f32>(std::atof(c.values[1])) : 400.0f;
        const f32 ch = std::atof(c.values[2]) > 40.0 ? static_cast<f32>(std::atof(c.values[2])) : 200.0f;
        const ImVec2 p(origin.x + c.x * w.zoom, origin.y + c.y * w.zoom);
        const ImVec2 q(p.x + cw * w.zoom, p.y + ch * w.zoom);
        const bool sel = is_selected(w, c.id);
        dl->AddRectFilled(p, q, IM_COL32(70, 80, 110, 70), 6.0f);
        dl->AddRectFilled(p, ImVec2(q.x, p.y + header()), IM_COL32(70, 80, 110, 200), 6.0f, ImDrawFlags_RoundCornersTop);
        dl->AddRect(p, q, sel ? IM_COL32(243, 195, 18, 255) : IM_COL32(110, 120, 160, 200), 6.0f, 0, sel ? 2.0f : 1.0f);
        ImGui::PushID(static_cast<int>(c.id));
        ImGui::SetCursorScreenPos(ImVec2(p.x + 6, p.y + 2));
        if (sel && w.selection.size() == 1) {  // selected alone: its title is editable in place
            ImGui::SetNextItemWidth(cw * w.zoom - 12.0f);
            if (ImGui::InputTextWithHint("##text", "Comment", c.values[0], sizeof c.values[0])) w.dirty = true;
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) hovered_item = true;
        } else {
            dl->AddText(ImVec2(p.x + 8, p.y + 4), IM_COL32_WHITE, c.values[0][0] ? c.values[0] : "Comment");
            ImGui::InvisibleButton("title", ImVec2(cw * w.zoom, header()));
            if (ImGui::IsItemHovered()) hovered_item = true;
            if (ImGui::IsItemActivated()) {
                if (io.KeyCtrl || io.KeyShift) w.selection.push_back(c.id);
                else select_only(w, c.id);
            }
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
                const f32 dx = io.MouseDelta.x / w.zoom, dy = io.MouseDelta.y / w.zoom;
                for (GraphNode& inside : g.nodes) {  // what sits in the box comes along
                    if (inside.id != c.id && inside.x >= c.x && inside.y >= c.y && inside.x <= c.x + cw && inside.y <= c.y + ch) {
                        inside.x += dx;
                        inside.y += dy;
                    }
                }
                c.x += dx;
                c.y += dy;
                w.dirty = true;
            }
        }
        // Corner handle: resize.
        ImGui::SetCursorScreenPos(ImVec2(q.x - 12, q.y - 12));
        ImGui::InvisibleButton("corner", ImVec2(12, 12));
        dl->AddTriangleFilled(ImVec2(q.x - 10, q.y - 2), ImVec2(q.x - 2, q.y - 10), ImVec2(q.x - 2, q.y - 2), IM_COL32(200, 200, 220, 200));
        if (ImGui::IsItemHovered()) {
            hovered_item = true;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            std::snprintf(c.values[1], sizeof c.values[1], "%g", static_cast<double>(std::fmax(cw + io.MouseDelta.x / w.zoom, 60.0f)));
            std::snprintf(c.values[2], sizeof c.values[2], "%g", static_cast<double>(std::fmax(ch + io.MouseDelta.y / w.zoom, 60.0f)));
            w.dirty = true;
        }
        ImGui::PopID();
    }

    // Wires behind the nodes.
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(0);
    for (const auto& l : g.links) {
        const GraphNode* a = g.find(l.from_node);
        const GraphNode* b = g.find(l.to_node);
        const NodeDef* da = a ? script::find_node_def(a->type) : nullptr;
        const NodeDef* db = b ? script::find_node_def(b->type) : nullptr;
        if (!da || !db || l.from_pin >= da->output_count || l.to_pin >= db->input_count) continue;
        wire(dl, pin_position(origin, *a, *da, true, l.from_pin), pin_position(origin, *b, *db, false, l.to_pin), pin_color(da->outputs[l.from_pin].type));
    }

    dl->ChannelsSetCurrent(1);
    u32 drop_node = 0, drop_pin = 0;
    bool drop_output = false, have_drop = false;
    for (usize i = 0; i < g.nodes.size(); ++i) {
        GraphNode& n = g.nodes[i];
        const NodeDef* def = script::find_node_def(n.type);
        if (!def || std::strcmp(n.type, "Comment") == 0) continue;  // comments are drawn above, behind the wires
        const f32 width = node_width(*def), height = node_height(*def);
        const ImVec2 p(origin.x + n.x * g_zoom, origin.y + n.y * g_zoom);
        ImGui::PushID(static_cast<int>(n.id));
        dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), IM_COL32(38, 38, 44, 240), 6.0f);
        dl->AddRectFilled(p, ImVec2(p.x + width, p.y + header()), header_color(*def), 6.0f, ImDrawFlags_RoundCornersTop);
        const bool sel = is_selected(w, n.id);
        dl->AddRect(p, ImVec2(p.x + width, p.y + height), sel ? IM_COL32(243, 195, 18, 255) : IM_COL32(10, 10, 10, 255), 6.0f, 0, sel ? 2.5f : 1.0f);
        if (w.kind == script::GraphKind::BehaviorTree) {  // running in Play: a green glow (live debugging)
            for (u32 a = 0; a < g_active_count; ++a) {
                if (g_active[a] == n.id) dl->AddRect(ImVec2(p.x - 3, p.y - 3), ImVec2(p.x + width + 3, p.y + height + 3), IM_COL32(80, 255, 120, 255), 8.0f, 0, 3.0f);
            }
        }
        if (w.kind == script::GraphKind::Script) {  // debugging: breakpoints, the stop, live flow
            const std::string lua = lua_path_for(w.guid);
            const i32 line = node_first_line(lua, n.id);
            if (line > 0 && debugger::has(lua.c_str(), line)) dl->AddCircleFilled(ImVec2(p.x - 7, p.y + header() * 0.5f), 5.0f, IM_COL32(230, 40, 40, 255));
            const script::DebugStop* stop = debugger::stop();
            if (stop && stop->frame_count && std::strcmp(stop->frames[0].source, lua.c_str()) == 0) {
                const std::vector<u32>& lines = node_lines(lua);
                const i32 at = stop->frames[0].line;
                if (at > 0 && static_cast<size_t>(at) < lines.size() && lines[static_cast<size_t>(at)] == n.id)
                    dl->AddRect(ImVec2(p.x - 4, p.y - 4), ImVec2(p.x + width + 4, p.y + height + 4), IM_COL32(255, 60, 60, 255), 8.0f, 0, 3.5f);
            }
            f64 since = 1e9;
            const std::vector<u32>& lines = node_lines(lua);
            for (size_t k = 1; k < lines.size(); ++k) {
                if (lines[k] == n.id) since = std::fmin(since, debugger::seconds_since(static_cast<i32>(k)));
            }
            if (since < 0.6) {  // ran just now: an orange glow that fades (Blueprint's execution flow)
                const int alpha = static_cast<int>(255.0 * (1.0 - since / 0.6));
                dl->AddRect(ImVec2(p.x - 3, p.y - 3), ImVec2(p.x + width + 3, p.y + height + 3), IM_COL32(238, 167, 14, alpha), 8.0f, 0, 3.0f);
            }
        }
        dl->AddText(ImVec2(p.x + 8, p.y + 4), IM_COL32_WHITE, def->title);

        // Header: select and drag.
        ImGui::SetCursorScreenPos(p);
        ImGui::InvisibleButton("header", ImVec2(width, header()));
        if (ImGui::IsItemHovered()) {
            hovered_item = true;
            if (def->tooltip[0]) ImGui::SetTooltip("%s", def->tooltip);
        }
        if (ImGui::IsItemActivated()) {  // Ctrl / Shift click: add to the selection; a click on an unselected node: just it
            if (io.KeyCtrl || io.KeyShift) {
                if (!sel) w.selection.push_back(n.id);
                w.selected = n.id;
            } else if (!sel) {
                select_only(w, n.id);
            } else {
                w.selected = n.id;
            }
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {  // the whole selection moves
            for (const u32 id : w.selection) {
                if (GraphNode* moved = g.find(id)) {
                    moved->x += io.MouseDelta.x / w.zoom;
                    moved->y += io.MouseDelta.y / w.zoom;
                }
            }
            w.dirty = true;
        }
        if (ImGui::BeginPopupContextItem("node_menu")) {
            if (w.kind == script::GraphKind::Script && ImGui::MenuItem("Collapse to Function", nullptr, false, !w.selection.empty())) {
                char name[32] = "NewFunction";
                for (int k = 2;; ++k) {  // a name no other Function has
                    bool taken = false;
                    for (const GraphNode& other : g.nodes) taken = taken || (std::strcmp(other.type, "FunctionEntry") == 0 && std::strcmp(other.values[0], name) == 0);
                    if (!taken) break;
                    std::snprintf(name, sizeof name, "NewFunction%d", k);
                }
                const std::vector<u32> ids = is_selected(w, n.id) ? w.selection : std::vector<u32>{n.id};
                u32 call = 0;
                char problem[160];
                const bool ok = script::collapse_to_function(g, ids.data(), static_cast<u32>(ids.size()), name, call, problem, sizeof problem);
                std::snprintf(w.message, sizeof w.message, ok ? "Collapsed into %s (rename it on its Function node)" : "Collapse to Function: %s", ok ? name : problem);
                w.message_is_error = !ok;
                if (ok) select_only(w, call);
                w.dirty = w.dirty || ok;
                ImGui::EndPopup();
                ImGui::PopID();
                break;  // the node array changed
            }
            if (w.kind == script::GraphKind::Script && ImGui::MenuItem("Toggle Breakpoint", "F9")) {
                const std::string lua = lua_path_for(w.guid);
                if (const i32 line = node_first_line(lua, n.id)) debugger::toggle(lua.c_str(), line);
            }
            if (ImGui::MenuItem("Delete", "Delete")) {
                const u32 id = n.id;
                ImGui::EndPopup();
                ImGui::PopID();
                g.remove(id);
                w.dirty = true;
                break;  // the node array changed
            }
            ImGui::EndPopup();
        }

        // Pins.
        for (int side = 0; side < 2; ++side) {
            const bool output = side == 1;
            const u32 count = output ? def->output_count : def->input_count;
            for (u32 k = 0; k < count; ++k) {
                const script::PinDef& pin = output ? def->outputs[k] : def->inputs[k];
                const ImVec2 at = pin_position(origin, n, *def, output, k);
                const bool connected = output ? output_connected(g, n.id, k) : g.link_into(n.id, k) != nullptr;
                draw_pin(dl, at, pin.type, connected);
                const ImVec2 label_size = ImGui::CalcTextSize(pin.name);
                if (output) dl->AddText(ImVec2(at.x - pin_size() - 6 - label_size.x, at.y - label_size.y * 0.5f), IM_COL32(220, 220, 220, 255), pin.name);
                else dl->AddText(ImVec2(at.x + pin_size() + 6, at.y - label_size.y * 0.5f), IM_COL32(220, 220, 220, 255), pin.name);
                ImGui::PushID(static_cast<int>(k + (output ? 100u : 0u)));
                ImGui::SetCursorScreenPos(ImVec2(at.x - 9, at.y - 9));
                ImGui::InvisibleButton("pin", ImVec2(18, 18));
                if (ImGui::IsItemHovered()) hovered_item = true;
                if (ImGui::IsItemActivated()) {
                    const script::GraphLink* existing = output ? nullptr : g.link_into(n.id, k);
                    if (existing && pin.type != PinType::Exec) {  // pull the wire off this input, keep its source end
                        w.wire_node = existing->from_node;
                        w.wire_pin = existing->from_pin;
                        w.wire_from_output = true;
                        g.disconnect_input(n.id, k);
                        w.dirty = true;
                    } else {
                        w.wire_node = n.id;
                        w.wire_pin = k;
                        w.wire_from_output = output;
                    }
                    w.wiring = true;
                }
                if (ImGui::IsMouseHoveringRect(ImVec2(at.x - 10, at.y - 10), ImVec2(at.x + 10, at.y + 10))) {
                    drop_node = n.id;
                    drop_pin = k;
                    drop_output = output;
                    have_drop = true;
                }
                // Typed value for an unconnected data input.
                if (!output && !connected && pin.type != PinType::Exec && k < script::kMaxPinValues) {
                    ImGui::SetCursorScreenPos(ImVec2(at.x + pin_size() + 12 + label_size.x, at.y - ImGui::GetFrameHeight() * 0.5f));
                    if (value_widget(pin, n.values[k], sizeof n.values[k])) w.dirty = true;
                    if (ImGui::IsItemHovered()) hovered_item = true;
                }
                ImGui::PopID();
            }
        }
        ImGui::PopID();
    }
    dl->ChannelsMerge();

    // Wire being dragged: follow the mouse, connect on release over a pin.
    if (w.wiring) {
        const GraphNode* from = g.find(w.wire_node);
        const NodeDef* def = from ? script::find_node_def(from->type) : nullptr;
        if (def) {
            const PinType type = w.wire_from_output ? def->outputs[w.wire_pin].type : def->inputs[w.wire_pin].type;
            const ImVec2 at = pin_position(origin, *from, *def, w.wire_from_output, w.wire_pin);
            if (w.wire_from_output) wire(dl, at, io.MousePos, pin_color(type));
            else wire(dl, io.MousePos, at, pin_color(type));
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (have_drop && drop_output != w.wire_from_output) {
                const bool ok = w.wire_from_output ? g.connect(w.wire_node, w.wire_pin, drop_node, drop_pin)
                                                   : g.connect(drop_node, drop_pin, w.wire_node, w.wire_pin);
                if (ok) {
                    w.dirty = true;
                } else {
                    std::snprintf(w.message, sizeof w.message, "Those pins do not fit together (types differ)");
                    w.message_is_error = true;
                }
            }
            w.wiring = false;
        }
    }

    // Canvas: pan with middle or right drag; right-click (no drag) opens the node menu; click empty space deselects.
    ImGui::SetCursorScreenPos(canvas);
    const bool canvas_hovered = ImGui::IsWindowHovered() && !hovered_item;
    if (canvas_hovered && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || ImGui::IsMouseDragging(ImGuiMouseButton_Right))) {
        w.scroll.x += io.MouseDelta.x;
        w.scroll.y += io.MouseDelta.y;
    }
    if (canvas_hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).x == 0.0f &&
        ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).y == 0.0f) {
        w.menu_at = to_canvas(io.MousePos);
        ImGui::OpenPopup("AddNode");
    }
    // Wheel: zoom about the mouse (the point under it stays put).
    if (canvas_hovered && io.MouseWheel != 0.0f) {
        const ImVec2 under = to_canvas(io.MousePos);
        w.zoom = std::clamp(w.zoom * (io.MouseWheel > 0.0f ? 1.1f : 1.0f / 1.1f), 0.3f, 2.0f);
        w.scroll = ImVec2(io.MousePos.x - canvas.x - under.x * w.zoom, io.MousePos.y - canvas.y - under.y * w.zoom);
    }
    // Left drag on empty canvas: a selection box (Ctrl / Shift add to the selection); a plain click clears it.
    if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        w.boxing = true;
        w.box_start = io.MousePos;
        if (!io.KeyCtrl && !io.KeyShift) select_only(w, 0);
    }
    if (w.boxing) {
        const ImVec2 a(std::fmin(w.box_start.x, io.MousePos.x), std::fmin(w.box_start.y, io.MousePos.y));
        const ImVec2 b(std::fmax(w.box_start.x, io.MousePos.x), std::fmax(w.box_start.y, io.MousePos.y));
        dl->AddRectFilled(a, b, IM_COL32(243, 195, 18, 30));
        dl->AddRect(a, b, IM_COL32(243, 195, 18, 200));
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            w.boxing = false;
            for (const GraphNode& n : g.nodes) {
                const NodeDef* def = script::find_node_def(n.type);
                if (!def) continue;
                const ImVec2 p(origin.x + n.x * w.zoom, origin.y + n.y * w.zoom);
                const ImVec2 q(p.x + node_width(*def), p.y + node_height(*def));
                if (p.x < b.x && q.x > a.x && p.y < b.y && q.y > a.y && !is_selected(w, n.id)) w.selection.push_back(n.id);
            }
            if (!w.selection.empty()) w.selected = w.selection.back();
        }
    }
    node_menu(w);

    // Copy / cut / paste / duplicate (the clipboard holds graph text) and undo / redo inside the graph.
    const bool keys = focused && !ImGui::IsAnyItemActive() && !w.wiring;
    if (keys && io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_C, false) || ImGui::IsKeyPressed(ImGuiKey_X, false)) && !w.selection.empty()) {
        ImGui::SetClipboardText(copy_selection(w).c_str());
        if (ImGui::IsKeyPressed(ImGuiKey_X, false)) {
            for (const u32 id : w.selection) g.remove(id);
            select_only(w, 0);
            w.dirty = true;
        }
    }
    if (keys && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        if (!paste(w, ImGui::GetClipboardText(), to_canvas(io.MousePos))) std::snprintf(w.message, sizeof w.message, "The clipboard holds no nodes");
    }
    if (keys && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && !w.selection.empty()) {
        const GraphNode* first = g.find(w.selection[0]);
        const std::string part = copy_selection(w);
        if (first) paste(w, part.c_str(), ImVec2(first->x + 30.0f, first->y + 30.0f));
    }
    const bool undo_key = keys && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z) && !io.KeyShift;
    const bool redo_key = keys && io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Y) || (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)));
    if ((undo_key && !w.undo.empty()) || (redo_key && !w.redo.empty())) {
        auto& from = undo_key ? w.undo : w.redo;
        auto& to = undo_key ? w.redo : w.undo;
        to.push_back(w.snapshot);
        w.snapshot = from.back();
        from.pop_back();
        script::load_graph(g, w.snapshot.data(), w.snapshot.size());
        select_only(w, 0);
        w.dirty = true;
    }
    // A finished edit (mouse up, no field being typed in): remember the graph as it was before it.
    // shortcut: compares the whole graph as text each idle frame; keep a changed flag if graphs get huge.
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive()) {
        std::string now = graph_text(g);
        if (now != w.snapshot) {
            w.undo.push_back(std::move(w.snapshot));
            if (w.undo.size() > 100) w.undo.erase(w.undo.begin());
            w.redo.clear();
            w.snapshot = std::move(now);
        }
    }

    if (w.kind == script::GraphKind::Script) {
        const std::string lua = lua_path_for(w.guid);
        debugger::trace_source(s.playing ? lua.c_str() : "");  // live flow while playing
        if (focused && w.selected && ImGui::IsKeyPressed(ImGuiKey_F9)) {
            if (const i32 line = node_first_line(lua, w.selected)) debugger::toggle(lua.c_str(), line);
            else std::snprintf(s.status, sizeof s.status, "Save the graph first (the breakpoint goes on its compiled Lua)");
        }
    }
    if (focused && !ImGui::IsAnyItemActive() && !w.selection.empty() && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        for (const u32 id : w.selection) g.remove(id);
        select_only(w, 0);
        w.dirty = true;
    }
    if (focused && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) save_and_compile(s);
    w.view_size = size;
    canvas_drops(w, canvas, size, to_canvas(io.MousePos));
    ImGui::EndChild();
    if (material) {
        ImGui::SameLine();
        ImGui::BeginChild("Preview", ImVec2(0, 0), ImGuiChildFlags_Borders);
        draw_preview_panel(s, w);
        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace lsd::editor
