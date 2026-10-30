#include "graph_editor.h"

#include "lsd/foundation/log.h"
#include "lsd/script/graph.h"

#include <imgui.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace lsd::editor {

namespace {

using script::Graph;
using script::GraphNode;
using script::NodeDef;
using script::PinType;

constexpr f32 kHeader = 24.0f;
constexpr f32 kRow = 24.0f;
constexpr f32 kPin = 6.0f;
constexpr char kGraphLuaDir[] = "Saved/Graphs";

struct GraphWindow {
    Graph* graph = nullptr;
    asset::Guid guid;
    char path[300] = "";
    char name[64] = "";
    bool open = false;
    bool dirty = false;
    ImVec2 scroll{40.0f, 40.0f};
    u32 selected = 0;
    // A wire being dragged from a pin.
    bool wiring = false;
    u32 wire_node = 0, wire_pin = 0;
    bool wire_from_output = true;
    ImVec2 menu_at;  // canvas position for a node picked from the menu
    char search[64] = "";
    char message[256] = "";
    bool message_is_error = false;
};
GraphWindow g_window;

ImU32 pin_color(PinType type) {
    switch (type) {
        case PinType::Exec: return IM_COL32(240, 240, 240, 255);
        case PinType::Float: return IM_COL32(150, 230, 90, 255);
        case PinType::Bool: return IM_COL32(220, 50, 50, 255);
        case PinType::String: return IM_COL32(240, 110, 220, 255);
        case PinType::Vector: return IM_COL32(250, 200, 40, 255);
        case PinType::Entity: return IM_COL32(60, 160, 255, 255);
    }
    return IM_COL32_WHITE;
}

ImU32 header_color(const NodeDef& def) {
    if (script::is_event(def)) return IM_COL32(160, 30, 30, 255);
    if (script::is_pure(def)) return IM_COL32(50, 110, 60, 255);
    if (std::strcmp(def.category, "Flow") == 0) return IM_COL32(85, 85, 90, 255);
    return IM_COL32(40, 80, 150, 255);
}

f32 node_width(const NodeDef& def) {
    for (u32 i = 0; i < def.input_count; ++i) {
        if (def.inputs[i].type == PinType::Vector) return def.output_count > 1 || script::is_pure(def) ? 330.0f : 270.0f;  // room for the output label
    }
    return 210.0f;
}

f32 node_height(const NodeDef& def) {
    const u32 rows = def.input_count > def.output_count ? def.input_count : def.output_count;
    return kHeader + kRow * static_cast<f32>(rows > 0 ? rows : 1) + 6.0f;
}

ImVec2 pin_position(ImVec2 origin, const GraphNode& node, const NodeDef& def, bool output, u32 pin) {
    return ImVec2(origin.x + node.x + (output ? node_width(def) : 0.0f), origin.y + node.y + kHeader + kRow * (static_cast<f32>(pin) + 0.5f));
}

void wire(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color) {
    const f32 bend = std::fabs(b.x - a.x) * 0.5f + 30.0f;
    dl->AddBezierCubic(a, ImVec2(a.x + bend, a.y), ImVec2(b.x - bend, b.y), b, color, 2.5f);
}

void draw_pin(ImDrawList* dl, ImVec2 at, PinType type, bool connected) {
    const ImU32 color = pin_color(type);
    if (type == PinType::Exec) {  // arrow
        const ImVec2 p[3] = {ImVec2(at.x - kPin, at.y - kPin), ImVec2(at.x + kPin, at.y), ImVec2(at.x - kPin, at.y + kPin)};
        if (connected) dl->AddTriangleFilled(p[0], p[1], p[2], color);
        else dl->AddTriangle(p[0], p[1], p[2], color, 1.5f);
    } else if (connected) {
        dl->AddCircleFilled(at, kPin, color);
    } else {
        dl->AddCircle(at, kPin, color, 12, 1.5f);
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
            ImGui::SetNextItemWidth(100.0f);
            changed = ImGui::InputText("##v", value, capacity);
            break;
        case PinType::Vector: {
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
        case PinType::Exec: break;
    }
    return changed;
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
    std::error_code error;
    std::filesystem::create_directories(kGraphLuaDir, error);
    char problem[256];
    if (script::compile_graph_file(w.path, lua_path_for(w.guid).c_str(), problem, sizeof problem)) {
        std::snprintf(w.message, sizeof w.message, "Saved and compiled");
        w.message_is_error = false;
    } else {
        std::snprintf(w.message, sizeof w.message, "%s", problem);
        w.message_is_error = true;
        LSD_LOG_ERROR("graph", "%s: %s", w.name, problem);
    }
    std::snprintf(s.status, sizeof s.status, "%s: %s", w.name, w.message);
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
    const NodeDef* defs = script::node_defs(count);
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
            if (GraphNode* n = w.graph->add(d.id, w.menu_at.x, w.menu_at.y)) w.selected = n->id;
            w.dirty = true;
            ImGui::CloseCurrentPopup();
        }
        if (d.tooltip[0]) ImGui::SetItemTooltip("%s", d.tooltip);
    }
    ImGui::EndPopup();
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

bool graph_lua(const asset::AssetRegistry& registry, const asset::AssetRecord& record, char* lua_path, usize capacity) {
    char path[300];
    registry.source_path(record, path, sizeof path);
    const std::string lua = lua_path_for(record.guid);
    std::snprintf(lua_path, capacity, "%s", lua.c_str());
    std::error_code error;
    const auto lua_time = std::filesystem::last_write_time(lua, error);
    if (!error && lua_time >= std::filesystem::last_write_time(path, error) && !error) return true;  // up to date
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
    std::snprintf(w.name, sizeof w.name, "%s", record.name);
    w.open = true;
    w.dirty = false;
    w.selected = 0;
    w.wiring = false;
    w.message[0] = '\0';
    ImGui::SetWindowFocus("###VisualScript");
}

void draw_graph_editor(EditorState& s) {
    GraphWindow& w = g_window;
    if (!w.open || !w.graph) return;
    Graph& g = *w.graph;
    char title[128];
    std::snprintf(title, sizeof title, "%s%s - Visual Script###VisualScript", w.name, w.dirty ? "*" : "");
    const ImGuiViewport* main = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(main->WorkPos.x + main->WorkSize.x * 0.5f, main->WorkPos.y + main->WorkSize.y * 0.45f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(1000, 620), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!ImGui::Begin(title, &open)) {
        ImGui::End();
        return;
    }
    if (!open) {
        if (w.dirty) save_and_compile(s);
        w.open = false;
    }
    if (ImGui::Button("Save + Compile")) save_and_compile(s);
    ImGui::SetItemTooltip("Ctrl+S. Playing objects pick the change up within half a second");
    ImGui::SameLine();
    if (w.message[0]) ImGui::TextColored(w.message_is_error ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(0.5f, 1, 0.5f, 1), "%s", w.message);
    else ImGui::TextDisabled("Right-click: add node. Drag from a pin: wire. Middle or right drag: pan. Delete: remove node.");

    ImGui::BeginChild("Canvas", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 canvas = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    const ImVec2 origin(canvas.x + w.scroll.x, canvas.y + w.scroll.y);
    const ImGuiIO& io = ImGui::GetIO();
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

    // Grid.
    dl->AddRectFilled(canvas, ImVec2(canvas.x + size.x, canvas.y + size.y), IM_COL32(28, 28, 32, 255));
    for (f32 x = std::fmod(w.scroll.x, 32.0f); x < size.x; x += 32.0f) dl->AddLine(ImVec2(canvas.x + x, canvas.y), ImVec2(canvas.x + x, canvas.y + size.y), IM_COL32(45, 45, 52, 255));
    for (f32 y = std::fmod(w.scroll.y, 32.0f); y < size.y; y += 32.0f) dl->AddLine(ImVec2(canvas.x, canvas.y + y), ImVec2(canvas.x + size.x, canvas.y + y), IM_COL32(45, 45, 52, 255));

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
    bool hovered_item = false;
    u32 drop_node = 0, drop_pin = 0;
    bool drop_output = false, have_drop = false;
    for (usize i = 0; i < g.nodes.size(); ++i) {
        GraphNode& n = g.nodes[i];
        const NodeDef* def = script::find_node_def(n.type);
        if (!def) continue;
        const f32 width = node_width(*def), height = node_height(*def);
        const ImVec2 p(origin.x + n.x, origin.y + n.y);
        ImGui::PushID(static_cast<int>(n.id));
        dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), IM_COL32(38, 38, 44, 240), 6.0f);
        dl->AddRectFilled(p, ImVec2(p.x + width, p.y + kHeader), header_color(*def), 6.0f, ImDrawFlags_RoundCornersTop);
        dl->AddRect(p, ImVec2(p.x + width, p.y + height), w.selected == n.id ? IM_COL32(243, 195, 18, 255) : IM_COL32(10, 10, 10, 255), 6.0f, 0,
                    w.selected == n.id ? 2.5f : 1.0f);
        dl->AddText(ImVec2(p.x + 8, p.y + 4), IM_COL32_WHITE, def->title);

        // Header: select and drag.
        ImGui::SetCursorScreenPos(p);
        ImGui::InvisibleButton("header", ImVec2(width, kHeader));
        if (ImGui::IsItemHovered()) {
            hovered_item = true;
            if (def->tooltip[0]) ImGui::SetTooltip("%s", def->tooltip);
        }
        if (ImGui::IsItemActivated()) w.selected = n.id;
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            n.x += io.MouseDelta.x;
            n.y += io.MouseDelta.y;
            w.dirty = true;
        }
        if (ImGui::BeginPopupContextItem("node_menu")) {
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
                if (output) dl->AddText(ImVec2(at.x - kPin - 6 - label_size.x, at.y - label_size.y * 0.5f), IM_COL32(220, 220, 220, 255), pin.name);
                else dl->AddText(ImVec2(at.x + kPin + 6, at.y - label_size.y * 0.5f), IM_COL32(220, 220, 220, 255), pin.name);
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
                    ImGui::SetCursorScreenPos(ImVec2(at.x + kPin + 12 + label_size.x, at.y - ImGui::GetFrameHeight() * 0.5f));
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
        w.menu_at = ImVec2(io.MousePos.x - origin.x, io.MousePos.y - origin.y);
        ImGui::OpenPopup("AddNode");
    }
    if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) w.selected = 0;
    node_menu(w);

    if (focused && !ImGui::IsAnyItemActive() && w.selected && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        g.remove(w.selected);
        w.selected = 0;
        w.dirty = true;
    }
    if (focused && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) save_and_compile(s);
    ImGui::EndChild();
    ImGui::End();
}

} // namespace lsd::editor
