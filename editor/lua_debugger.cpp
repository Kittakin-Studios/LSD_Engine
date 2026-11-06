#include "lua_debugger.h"
#include "keymap.h"
#include "tips.h"
#include "workspaces.h"

#include "lsd/foundation/log.h"

#include <SDL3/SDL_timer.h>
#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace lsd::editor::debugger {

namespace {

script::ScriptSystem* g_scripts = nullptr;
void (*g_frame)(void*) = nullptr;
void* g_frame_user = nullptr;
const script::DebugStop* g_stop = nullptr;
bool g_resume = false;
script::DebugAction g_action = script::DebugAction::Continue;
u32 g_frame_index = 0;  // the call-stack frame shown
char g_file[300] = "";  // the file in the source view
std::vector<std::string> g_lines;
std::string g_loaded;
char g_watch[8][64] = {};

std::string g_traced;
std::vector<f64> g_heat;  // per line: when it last ran (seconds)

f64 now() { return static_cast<f64>(SDL_GetTicksNS()) * 1e-9; }

void trace(void*, const char* source, i32 line) {
    if (g_traced.empty() || line <= 0 || std::strcmp(source, g_traced.c_str()) != 0) return;
    if (g_heat.size() <= static_cast<size_t>(line)) g_heat.resize(static_cast<size_t>(line) + 64, -1e9);
    g_heat[static_cast<size_t>(line)] = now();
}

script::DebugAction on_stop(void*, const script::DebugStop& stop) {
    if (stop.frame_count) LSD_LOG_INFO("debug", "Stopped at %s:%d in %s", stop.frames[0].source, stop.frames[0].line, stop.frames[0].function);
    g_stop = &stop;
    g_frame_index = 0;
    std::snprintf(g_file, sizeof g_file, "%s", stop.frame_count ? stop.frames[0].source : "");
    g_resume = false;
    while (!g_resume && g_frame) g_frame(g_frame_user);  // the editor keeps drawing; the game waits here
    g_stop = nullptr;
    return g_resume ? g_action : script::DebugAction::Continue;
}

void load_source() {
    if (g_loaded == g_file) return;
    g_loaded = g_file;
    g_lines.clear();
    std::ifstream file(g_file);
    for (std::string line; std::getline(file, line);) g_lines.push_back(line);
}

const char* lookup(const script::DebugFrame& f, const char* name) {
    for (u32 i = 0; i < f.local_count; ++i) {
        if (std::strcmp(f.locals[i].name, name) == 0) return f.locals[i].value;
    }
    return nullptr;
}

} // namespace

std::vector<std::pair<std::string, i32>> g_pending;  // set before attach (command line)

void attach(script::ScriptSystem& scripts) {
    g_scripts = &scripts;
    scripts.set_debugger(on_stop, nullptr);
    for (const auto& [file, line] : g_pending) scripts.set_breakpoint(file.c_str(), line, true);
    g_pending.clear();
}

void set_frame_callback(void (*frame)(void*), void* user) {
    g_frame = frame;
    g_frame_user = user;
}

void toggle(const char* source, i32 line) {
    if (g_scripts) g_scripts->set_breakpoint(source, line, !g_scripts->has_breakpoint(source, line));
    else g_pending.emplace_back(source, line);
}

bool has(const char* source, i32 line) { return g_scripts && g_scripts->has_breakpoint(source, line); }
bool stopped() { return g_stop != nullptr; }
const script::DebugStop* stop() { return g_stop; }

void resume(script::DebugAction action) {
    g_action = action;
    g_resume = true;
}

void trace_source(const char* source) {
    const std::string wanted = source ? source : "";
    if (wanted == g_traced) return;
    g_traced = wanted;
    g_heat.clear();
    if (g_scripts) g_scripts->set_line_trace(g_traced.empty() ? nullptr : trace, nullptr);
}

f64 seconds_since(i32 line) { return line > 0 && static_cast<size_t>(line) < g_heat.size() ? now() - g_heat[static_cast<size_t>(line)] : 1e9; }

void draw_window(EditorState& s, bool* open) {
    if (g_stop) *open = true;  // a stop always shows it
    if (!*open) return;
    ImGui::SetNextWindowSize(ImVec2(1000, 560), ImGuiCond_FirstUseEver);
    if (g_stop) ImGui::SetNextWindowFocus();
    if (!begin_editor("Lua Debugger", open)) return;  // a panel tab, or the workspace's editor area
    // Toolbar: the controls work while stopped (F5 / F10 / F11 / Shift+F11 like Visual Studio).
    ImGui::BeginDisabled(!g_stop);
    // A button labelled with its current keys (Editor Preferences > Keyboard Shortcuts).
    const auto control = [](const char* name, Command c, const char* tip) {
        char label[64];
        std::snprintf(label, sizeof label, "%s (%s)###%s", name, command_text(c), name);
        return with_tip(ImGui::Button(label), tip) || (g_stop && command_pressed(c));
    };
    if (control("Continue", Command::Continue, "Runs on until the next breakpoint")) resume(script::DebugAction::Continue);
    ImGui::SameLine();
    if (control("Step Over", Command::StepOver, "Runs this line and stops at the next")) resume(script::DebugAction::StepOver);
    ImGui::SameLine();
    if (control("Step Into", Command::StepInto, "Goes into the function this line calls")) resume(script::DebugAction::StepInto);
    ImGui::SameLine();
    if (control("Step Out", Command::StepOut, "Runs to the end of this function")) resume(script::DebugAction::StepOut);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(g_stop ? "Stopped: the game waits" : s.playing ? "Running" : "Set breakpoints, then Play");
    // File picker: the Lua and JavaScript scripts in Content.
    ImGui::SetNextItemWidth(420);
    if (ImGui::BeginCombo("File", g_file[0] ? g_file : "(pick a script)")) {
        for (usize i = 0; s.registry && i < s.registry->count(); ++i) {
            const asset::AssetRecord& r = s.registry->get(i);
            if (r.type != asset::AssetType::Script) continue;
            char path[300];
            s.registry->source_path(r, path, sizeof path);
            if (ImGui::Selectable(path, std::strcmp(path, g_file) == 0)) std::snprintf(g_file, sizeof g_file, "%s", path);
        }
        ImGui::EndCombo();
    }
    load_source();
    const script::DebugFrame* frame = g_stop && g_frame_index < g_stop->frame_count ? &g_stop->frames[g_frame_index] : nullptr;
    // Source (left): click the gutter (or F9 on a hovered line) for a breakpoint.
    ImGui::BeginChild("source", ImVec2(ImGui::GetContentRegionAvail().x * 0.6f, 0), ImGuiChildFlags_Borders);
    for (size_t i = 0; i < g_lines.size(); ++i) {
        const i32 line = static_cast<i32>(i + 1);
        const bool here = frame && frame->line == line && std::strcmp(frame->source, g_file) == 0;
        ImGui::PushID(line);
        const bool bp = has(g_file, line);
        ImGui::PushStyleColor(ImGuiCol_Text, bp ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f) : ImVec4(0.45f, 0.45f, 0.5f, 1.0f));
        char gutter[16];
        std::snprintf(gutter, sizeof gutter, "%s%4d", bp ? "o" : " ", line);
        if (ImGui::Selectable(gutter, false, 0, ImVec2(48, 0))) toggle(g_file, line);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && command_pressed(Command::ToggleBreakpoint)) toggle(g_file, line);
        ImGui::SameLine();
        if (here) ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.25f, 1.0f), "> %s", g_lines[i].c_str());
        else ImGui::TextUnformatted(g_lines[i].c_str());
        if (here && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
        ImGui::PopID();
    }
    if (g_lines.empty()) ImGui::TextDisabled("Pick a script. Visual scripts: set breakpoints on nodes in the graph (right-click > Toggle Breakpoint).");
    ImGui::EndChild();
    ImGui::SameLine();
    // Stack, locals, watch (right).
    ImGui::BeginChild("state", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::SeparatorText("Call stack");
    for (u32 i = 0; g_stop && i < g_stop->frame_count; ++i) {
        const script::DebugFrame& f = g_stop->frames[i];
        char label[360];
        std::snprintf(label, sizeof label, "%s  (%s:%d)", f.function, f.source, f.line);
        if (ImGui::Selectable(label, i == g_frame_index)) {
            g_frame_index = i;
            std::snprintf(g_file, sizeof g_file, "%s", f.source);
        }
    }
    ImGui::SeparatorText("Locals");
    for (u32 i = 0; frame && i < frame->local_count; ++i) ImGui::Text("%s = %s", frame->locals[i].name, frame->locals[i].value);
    if (!frame) ImGui::TextDisabled("(when stopped)");
    ImGui::SeparatorText("Watch");
    for (auto& w : g_watch) {
        ImGui::PushID(&w);
        ImGui::SetNextItemWidth(140);
        ImGui::InputTextWithHint("##w", "name or self.field", w, sizeof w);
        if (w[0]) {
            ImGui::SameLine();
            const char* value = frame ? lookup(*frame, w) : nullptr;
            ImGui::TextUnformatted(value ? value : frame ? "(not here)" : "");
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace lsd::editor::debugger
