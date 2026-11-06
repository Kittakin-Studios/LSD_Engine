#include "anim_graph_editor.h"
#include "keymap.h"
#include "animation_player.h"
#include "tips.h"
#include "workspaces.h"

#include "lsd/animation/anim_graph.h"
#include "lsd/asset/data_asset.h"
#include "lsd/debug_ui/details_panel.h"
#include "lsd/world/components.h"
#include "lsd/world/world.h"

#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace lsd::editor {

namespace {

constexpr f32 kNodeW = 160.0f, kNodeH = 48.0f;

struct Window {
    bool open = false;
    bool dirty = false;
    char path[300] = "";
    char name[96] = "";
    char guid[33] = "";
    anim::AnimGraph graph;
    i32 state = -1;       // selected state
    i32 transition = -1;  // selected transition
    i32 dragging = -1;    // the state being moved
    ImVec2 pan{0.0f, 0.0f};
    i32 menu_state = -1;  // the state the right-click menu is for (-1: empty canvas)
    ImVec2 menu_at{0.0f, 0.0f};
};
Window g_ag;

i32 find_state(const anim::AnimGraph& g, const char* name) {
    for (u32 i = 0; i < g.states.count; ++i) {
        if (std::strcmp(g.states[i].name, name) == 0) return static_cast<i32>(i);
    }
    return -1;
}

void save(EditorState& s) {
    if (asset::save_data_asset(g_ag.path, reflect::type_of<anim::AnimGraph>(), &g_ag.graph)) {
        g_ag.dirty = false;
        std::snprintf(s.status, sizeof s.status, "%s: saved", g_ag.name);
    } else {
        std::snprintf(s.status, sizeof s.status, "Could not save %s", g_ag.path);
    }
}

// States never placed (both 0) get a spot in rows of four, so an old graph opens readable.
void lay_out(anim::AnimGraph& g) {
    for (u32 i = 0; i < g.states.count; ++i) {
        anim::AnimState& st = g.states[i];
        if (st.node_x != 0.0f || st.node_y != 0.0f) continue;
        st.node_x = 200.0f + static_cast<f32>(i % 4) * 220.0f;
        st.node_y = 60.0f + static_cast<f32>(i / 4) * 120.0f;
    }
}

// Where the segment from `c` toward `toward` leaves the box of half size h around c.
ImVec2 box_edge(ImVec2 c, ImVec2 toward, ImVec2 h) {
    const f32 dx = toward.x - c.x, dy = toward.y - c.y;
    const f32 t = std::fmin(std::fabs(dx) > 1e-3f ? h.x / std::fabs(dx) : 1e9f, std::fabs(dy) > 1e-3f ? h.y / std::fabs(dy) : 1e9f);
    return t >= 1.0f ? c : ImVec2(c.x + dx * t, c.y + dy * t);
}

// Which state an object using this graph is in during Play (and the one it fades from); "" if none.
const char* live_state(EditorState& s, const char** from) {
    *from = "";
    if (!s.playing || !s.world || !s.animations || !s.skinned) return "";
    const reflect::TypeInfo& type = reflect::type_of<world::SkeletalMesh>();
    for (u32 i = 0; i < s.world->count(type); ++i) {
        const auto* mesh = static_cast<const world::SkeletalMesh*>(s.world->at(type, i));
        if (std::strcmp(mesh->anim_graph, g_ag.guid) != 0) continue;
        const char* state = s.animations->graph_state(*s.skinned, *mesh, s.world->owner(type, i), from);
        if (state[0]) return state;  // "" when it is not stepping the graph (Motion Matching drives it): the next one
    }
    return "";
}

void add_state(ImVec2 canvas_at) {
    anim::AnimState st;
    for (int n = 1; n < 100; ++n) {  // State, State 2, State 3...
        std::snprintf(st.name, sizeof st.name, n == 1 ? "State" : "State %d", n);
        if (find_state(g_ag.graph, st.name) < 0) break;
    }
    st.node_x = canvas_at.x;
    st.node_y = canvas_at.y;
    if (g_ag.graph.states.push_back(st)) {
        g_ag.state = static_cast<i32>(g_ag.graph.states.count) - 1;
        g_ag.transition = -1;
        g_ag.dirty = true;
    }
}

void add_transition(const char* from, const char* to) {
    anim::AnimTransition t;
    std::snprintf(t.from, sizeof t.from, "%s", from);
    std::snprintf(t.to, sizeof t.to, "%s", to);
    if (g_ag.graph.transitions.push_back(t)) {
        g_ag.transition = static_cast<i32>(g_ag.graph.transitions.count) - 1;
        g_ag.state = -1;
        g_ag.dirty = true;
    }
}

void delete_selection() {
    if (g_ag.state >= 0) anim::remove_state(g_ag.graph, static_cast<u32>(g_ag.state));
    else if (g_ag.transition >= 0) g_ag.graph.transitions.remove(static_cast<u32>(g_ag.transition));
    else return;
    g_ag.state = g_ag.transition = -1;
    g_ag.dirty = true;
}

void draw_canvas(EditorState& s) {
    anim::AnimGraph& g = g_ag.graph;
    ImGui::BeginChild("##canvas", ImVec2(-340.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 corner = ImGui::GetCursorScreenPos(), size = ImGui::GetContentRegionAvail();
    const ImVec2 origin(corner.x + g_ag.pan.x, corner.y + g_ag.pan.y);
    ImGui::InvisibleButton("##surface", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    // The grid, like the other graph canvases.
    for (f32 x = std::fmod(g_ag.pan.x, 32.0f); x < size.x; x += 32.0f) dl->AddLine(ImVec2(corner.x + x, corner.y), ImVec2(corner.x + x, corner.y + size.y), IM_COL32(255, 255, 255, 12));
    for (f32 y = std::fmod(g_ag.pan.y, 32.0f); y < size.y; y += 32.0f) dl->AddLine(ImVec2(corner.x, corner.y + y), ImVec2(corner.x + size.x, corner.y + y), IM_COL32(255, 255, 255, 12));

    const ImVec2 half(kNodeW * 0.5f, kNodeH * 0.5f);
    auto centre = [&](u32 i) { return ImVec2(origin.x + g.states[i].node_x + half.x, origin.y + g.states[i].node_y + half.y); };
    const ImVec2 entry_c(origin.x + 70.0f, origin.y + 40.0f), any_c(origin.x + 70.0f, origin.y + 140.0f);
    bool any_used = false;
    for (const anim::AnimTransition& t : g.transitions) any_used = any_used || !t.from[0];

    // Transitions: an arrow per transition (two between the same pair lean apart), a dot to click.
    i32 hovered_transition = -1;
    for (u32 k = 0; k < g.transitions.count; ++k) {
        const anim::AnimTransition& t = g.transitions[k];
        const i32 from = find_state(g, t.from), to = find_state(g, t.to);
        if (to < 0 || (t.from[0] && from < 0)) continue;
        const ImVec2 a = from >= 0 ? centre(static_cast<u32>(from)) : any_c, b = centre(static_cast<u32>(to));
        const f32 dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
        if (len < 1.0f) continue;
        const ImVec2 side(-dy / len * 8.0f, dx / len * 8.0f);  // to the right of the way it goes: A->B and B->A do not overlap
        const ImVec2 a2(a.x + side.x, a.y + side.y), b2(b.x + side.x, b.y + side.y);
        const ImVec2 p0 = box_edge(a2, b2, half), p1 = box_edge(b2, a2, half);
        const bool selected = g_ag.transition == static_cast<i32>(k);
        const ImU32 col = selected ? IM_COL32(243, 195, 18, 255) : IM_COL32(200, 200, 210, 200);
        dl->AddLine(p0, p1, col, selected ? 3.0f : 2.0f);
        const ImVec2 dir(dx / len, dy / len);
        dl->AddTriangleFilled(p1, ImVec2(p1.x - dir.x * 12.0f + dir.y * 6.0f, p1.y - dir.y * 12.0f - dir.x * 6.0f),
                              ImVec2(p1.x - dir.x * 12.0f - dir.y * 6.0f, p1.y - dir.y * 12.0f + dir.x * 6.0f), col);
        const ImVec2 mid((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
        dl->AddCircleFilled(mid, 6.0f, selected ? IM_COL32(243, 195, 18, 255) : IM_COL32(37, 82, 145, 255));
        if (hovered && (mouse.x - mid.x) * (mouse.x - mid.x) + (mouse.y - mid.y) * (mouse.y - mid.y) < 64.0f) {
            hovered_transition = static_cast<i32>(k);
            ImGui::SetTooltip("%s -> %s: %s", t.from[0] ? t.from : "Any State", t.to, t.parameter);
        }
    }

    // Entry (Unreal's): where the graph starts. Any State: transitions with no From.
    const i32 entry = g.entry_state[0] ? find_state(g, g.entry_state) : (g.states.count ? 0 : -1);
    dl->AddRectFilled(ImVec2(entry_c.x - 40, entry_c.y - 14), ImVec2(entry_c.x + 40, entry_c.y + 14), IM_COL32(60, 120, 60, 255), 12.0f);
    dl->AddText(ImVec2(entry_c.x - 18, entry_c.y - 8), IM_COL32_WHITE, "Entry");
    if (entry >= 0) {
        const ImVec2 to = box_edge(centre(static_cast<u32>(entry)), entry_c, half);
        dl->AddLine(ImVec2(entry_c.x + 40, entry_c.y), to, IM_COL32(120, 220, 120, 220), 2.0f);
    }
    if (any_used) {
        dl->AddRectFilled(ImVec2(any_c.x - 50, any_c.y - 14), ImVec2(any_c.x + 50, any_c.y + 14), IM_COL32(90, 60, 110, 255), 12.0f);
        dl->AddText(ImVec2(any_c.x - 32, any_c.y - 8), IM_COL32_WHITE, "Any State");
    }

    // States: boxes; the one playing glows green, the one fading out lighter.
    const char* fading = "";
    const char* playing = live_state(s, &fading);
    i32 hovered_state = -1;
    for (u32 i = 0; i < g.states.count; ++i) {
        const anim::AnimState& st = g.states[i];
        const ImVec2 lo(origin.x + st.node_x, origin.y + st.node_y), hi(lo.x + kNodeW, lo.y + kNodeH);
        const bool selected = g_ag.state == static_cast<i32>(i);
        const bool live = playing[0] && std::strcmp(playing, st.name) == 0, fade = fading[0] && std::strcmp(fading, st.name) == 0;
        dl->AddRectFilled(lo, hi, live ? IM_COL32(60, 110, 30, 255) : IM_COL32(60, 56, 56, 255), 6.0f);
        dl->AddRect(lo, hi, selected ? IM_COL32(243, 195, 18, 255) : live ? IM_COL32(155, 225, 31, 255) : fade ? IM_COL32(155, 225, 31, 120) : IM_COL32(110, 110, 120, 255), 6.0f, 0,
                    selected || live ? 3.0f : 1.5f);
        dl->AddText(ImVec2(lo.x + 10, lo.y + 6), IM_COL32_WHITE, st.name);
        char sub[64];
        if (st.blend_parameter[0]) std::snprintf(sub, sizeof sub, "Blend: %s%s%s (%u)", st.blend_parameter, st.blend_parameter_y[0] ? ", " : "", st.blend_parameter_y, st.samples.count);
        else std::snprintf(sub, sizeof sub, "Clip %u%s", st.clip, st.looping ? ", loops" : "");
        dl->AddText(ImVec2(lo.x + 10, lo.y + 26), IM_COL32(190, 190, 200, 255), sub);
        if (hovered && mouse.x >= lo.x && mouse.x <= hi.x && mouse.y >= lo.y && mouse.y <= hi.y) hovered_state = static_cast<i32>(i);
    }

    // Mouse: click selects, drag moves a box, middle drag pans, right-click opens the menu.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_ag.state = hovered_state;
        g_ag.transition = hovered_state < 0 ? hovered_transition : -1;
        g_ag.dragging = hovered_state;
    }
    if (g_ag.dragging >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        if (d.x != 0.0f || d.y != 0.0f) {
            g.states[static_cast<u32>(g_ag.dragging)].node_x += d.x;
            g.states[static_cast<u32>(g_ag.dragging)].node_y += d.y;
            g_ag.dirty = true;
        }
    } else {
        g_ag.dragging = -1;
    }
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        g_ag.pan.x += ImGui::GetIO().MouseDelta.x;
        g_ag.pan.y += ImGui::GetIO().MouseDelta.y;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        g_ag.menu_state = hovered_state;
        g_ag.menu_at = ImVec2(mouse.x - origin.x - half.x, mouse.y - origin.y - half.y);
        if (hovered_state >= 0) g_ag.state = hovered_state, g_ag.transition = -1;
        ImGui::OpenPopup("##state_menu");
    }
    if (ImGui::BeginPopup("##state_menu")) {
        if (g_ag.menu_state < 0) {
            if (with_tip(ImGui::MenuItem("Add State"), "A new state here; set what it plays beside the canvas")) add_state(g_ag.menu_at);
        } else if (static_cast<u32>(g_ag.menu_state) < g.states.count) {
            char from[32];
            std::snprintf(from, sizeof from, "%s", g.states[static_cast<u32>(g_ag.menu_state)].name);
            if (ImGui::BeginMenu("Add Transition To")) {
                for (u32 i = 0; i < g.states.count; ++i) {
                    if (static_cast<i32>(i) != g_ag.menu_state && ImGui::MenuItem(g.states[i].name)) add_transition(from, g.states[i].name);
                }
                ImGui::EndMenu();
            }
            if (with_tip(ImGui::MenuItem("Add Transition From Any State"), "Taken from whatever state plays (jumps, deaths, hit reactions)")) add_transition("", from);
            if (with_tip(ImGui::MenuItem("Set as Entry"), "The graph starts in this state")) {
                std::snprintf(g.entry_state, sizeof g.entry_state, "%s", from);
                g_ag.dirty = true;
            }
            if (with_tip(ImGui::MenuItem("Delete State", "Del"), "Removes it and the transitions to and from it")) delete_selection();
        }
        ImGui::EndPopup();
    }
    if (ImGui::IsWindowFocused() && command_pressed(Command::Delete)) delete_selection();
    if (!g.states.count) dl->AddText(ImVec2(corner.x + 200, corner.y + 60), IM_COL32(200, 200, 200, 200), "Right-click to add a state");
    ImGui::EndChild();
}

// Beside the canvas: the selection's fields (as in Details), or the graph's own.
void draw_side(EditorState& s) {
    anim::AnimGraph& g = g_ag.graph;
    ImGui::SameLine();
    ImGui::BeginChild("##side", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    const DetailsStyle style = editor_details_style(s);
    if (g_ag.state >= 0 && static_cast<u32>(g_ag.state) < g.states.count) {
        anim::AnimState& st = g.states[static_cast<u32>(g_ag.state)];
        ImGui::SeparatorText("State");
        char before[32];
        std::snprintf(before, sizeof before, "%s", st.name);
        if (draw_properties(reflect::type_of<anim::AnimState>(), &st, &style)) {
            anim::rename_state(g, before, st.name);  // its transitions follow a new name
            g_ag.dirty = true;
        }
    } else if (g_ag.transition >= 0 && static_cast<u32>(g_ag.transition) < g.transitions.count) {
        ImGui::SeparatorText("Transition");
        if (draw_properties(reflect::type_of<anim::AnimTransition>(), &g.transitions[static_cast<u32>(g_ag.transition)], &style)) g_ag.dirty = true;
    } else {
        ImGui::SeparatorText("Graph");
        ImGui::TextWrapped("Click a state or a transition's dot to edit it. Right-click: Add State; on a state: Add Transition, Set as Entry, Delete.");
        // The graph's own fields (its animation file, its entry state); the lists are the canvas.
        static const char* const own[] = {"animation", "entry"};  // filters match the shown names ("Entry State")
        for (const char* name : own) {
            DetailsStyle one = style;
            one.filter = name;
            if (draw_properties(reflect::type_of<anim::AnimGraph>(), &g, &one)) g_ag.dirty = true;
        }
    }
    ImGui::EndChild();
}

} // namespace

void open_anim_graph_editor(EditorState& s, const asset::AssetRecord& record) {
    if (g_ag.open && g_ag.dirty) save(s);
    char path[300];
    s.registry->source_path(record, path, sizeof path);
    anim::AnimGraph graph;
    if (!asset::load_data_asset(path, graph)) {
        std::snprintf(s.status, sizeof s.status, "Could not open %s", record.name);
        return;
    }
    g_ag = Window{};
    g_ag.graph = graph;
    lay_out(g_ag.graph);
    std::snprintf(g_ag.path, sizeof g_ag.path, "%s", path);
    std::snprintf(g_ag.name, sizeof g_ag.name, "%s", record.name);
    asset::to_string(record.guid, g_ag.guid);
    g_ag.open = true;
    ImGui::SetWindowFocus("###StateMachine");
}

void draw_anim_graph_editor(EditorState& s) {
    if (!g_ag.open) return;
    char title[160];
    std::snprintf(title, sizeof title, "%s%s - State Machine###StateMachine", g_ag.name, g_ag.dirty ? "*" : "");
    ImGui::SetNextWindowSize(ImVec2(1100, 560), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!begin_editor(title, &open)) return;  // a panel tab, or the workspace's editor area
    if (!open) {
        ImGui::End();
        if (g_ag.dirty) save(s);
        g_ag.open = false;
        return;
    }
    if (with_tip(ImGui::Button("Save"), "Writes the graph to disk; skeletal meshes using it pick it up at once")) save(s);
    ImGui::SameLine();
    ImGui::TextDisabled("%u states, %u transitions   Drag boxes, middle-drag to pan%s", g_ag.graph.states.count, g_ag.graph.transitions.count,
                        s.playing ? "   Play: the green state is playing" : "");
    draw_canvas(s);
    draw_side(s);
    ImGui::End();
}

} // namespace lsd::editor
