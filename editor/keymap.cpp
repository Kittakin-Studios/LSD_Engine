#include "keymap.h"

#include "editor_settings.h"

#include <imgui_internal.h>  // ImStristr

#include <cstdio>
#include <cstring>

namespace lsd::editor {

namespace {

constexpr ImGuiKeyChord kCtrl = ImGuiMod_Ctrl, kShift = ImGuiMod_Shift, kAlt = ImGuiMod_Alt;
constexpr char kPlayCategory[] = "While Playing";

const CommandInfo kCommands[] = {
    {"Play", "Play", "Play", kAlt | ImGuiKey_P},
    {"Simulate", "Simulate", "Play", kAlt | ImGuiKey_S},
    {"PlayWindow", "Play in New Window", "Play", kAlt | ImGuiKey_N},
    {"CompileGameCode", "Compile Game Code", "Play", kCtrl | kAlt | ImGuiKey_F11},
    {"StopPlay", "Stop", kPlayCategory, ImGuiKey_Escape},
    {"ReleaseMouse", "Release the Mouse", kPlayCategory, kShift | ImGuiKey_F1},
    {"Pause", "Pause", kPlayCategory, ImGuiKey_Pause},
    {"StepFrame", "Step a Frame (paused)", kPlayCategory, ImGuiKey_RightBracket},
    {"Eject", "Eject / Possess", kPlayCategory, ImGuiKey_F8},
    {"KeepSimulation", "Keep Simulation Changes", kPlayCategory, ImGuiKey_K},
    {"Undo", "Undo", "Edit", kCtrl | ImGuiKey_Z},
    {"Redo", "Redo", "Edit", kCtrl | ImGuiKey_Y},
    {"RedoAlt", "Redo (second key)", "Edit", kCtrl | kShift | ImGuiKey_Z},
    {"Save", "Save Level", "Edit", kCtrl | ImGuiKey_S},
    {"SaveAs", "Save Level As", "Edit", kCtrl | kShift | ImGuiKey_S},
    {"NewLevel", "New Level", "Edit", kCtrl | ImGuiKey_N},
    {"Copy", "Copy", "Edit", kCtrl | ImGuiKey_C},
    {"Cut", "Cut", "Edit", kCtrl | ImGuiKey_X},
    {"Paste", "Paste", "Edit", kCtrl | ImGuiKey_V},
    {"Duplicate", "Duplicate", "Edit", kCtrl | ImGuiKey_D},
    {"Delete", "Delete", "Edit", ImGuiKey_Delete},
    {"SelectAll", "Select All", "Edit", kCtrl | ImGuiKey_A},
    {"Deselect", "Select None", "Edit", ImGuiKey_Escape},
    {"Group", "Group", "Edit", kCtrl | ImGuiKey_G},
    {"Ungroup", "Ungroup", "Edit", kShift | ImGuiKey_G},
    {"FindInContentBrowser", "Find in Content Browser", "Edit", kCtrl | ImGuiKey_B},
    {"Translate", "Move Tool", "Viewport", ImGuiKey_W},
    {"Rotate", "Rotate Tool", "Viewport", ImGuiKey_E},
    {"Scale", "Scale Tool", "Viewport", ImGuiKey_R},
    {"LocalGizmo", "World / Local Gizmo", "Viewport", kCtrl | ImGuiKey_GraveAccent},
    {"PivotEdit", "Move the Pivot", "Viewport", kAlt | ImGuiKey_V},
    {"Focus", "Focus Selection", "Viewport", ImGuiKey_F},
    {"DropToFloor", "Drop to Floor", "Viewport", ImGuiKey_End},
    {"GameView", "Game View", "Viewport", ImGuiKey_G},
    {"ToggleNavigation", "Show Navigation", "Viewport", ImGuiKey_P},
    {"ViewPerspective", "Perspective View", "Viewport", kAlt | ImGuiKey_G},
    {"ViewTop", "Top View", "Viewport", kAlt | ImGuiKey_J},
    {"ViewFront", "Front View", "Viewport", kAlt | ImGuiKey_H},
    {"ViewRight", "Right View", "Viewport", kAlt | ImGuiKey_K},
    {"Lit", "Lit", "View Modes", kAlt | ImGuiKey_4},
    {"Unlit", "Unlit", "View Modes", kAlt | ImGuiKey_3},
    {"Wireframe", "Wireframe", "View Modes", kAlt | ImGuiKey_2},
    {"LightingOnly", "Lighting Only", "View Modes", kAlt | ImGuiKey_6},
    {"Overdraw", "Shader Complexity / Overdraw", "View Modes", kAlt | ImGuiKey_0},
    {"Collision", "Collision", "View Modes", kAlt | ImGuiKey_C},
};
static_assert(sizeof kCommands / sizeof kCommands[0] == static_cast<size_t>(Command::Count), "a CommandInfo per Command");

constexpr int kCount = static_cast<int>(Command::Count);
ImGuiKeyChord g_chords[kCount] = {};
bool g_loaded = false;
int g_capture = -1;        // the command waiting for a key
int g_captured_frame = -1;  // the frame its key went down: shortcuts stay off for it too
char g_text[kCount][32];

void ensure_loaded() {
    if (g_loaded) return;
    for (int i = 0; i < kCount; ++i) set_command_chord(static_cast<Command>(i), kCommands[i].chord);
    g_loaded = true;
}

bool is_play(int i) { return std::strcmp(kCommands[i].category, kPlayCategory) == 0; }

} // namespace

const CommandInfo& command_info(Command c) { return kCommands[static_cast<int>(c)]; }

ImGuiKeyChord command_chord(Command c) {
    ensure_loaded();
    return g_chords[static_cast<int>(c)];
}

void set_command_chord(Command c, ImGuiKeyChord chord) {
    const int i = static_cast<int>(c);
    g_chords[i] = chord;
    chord_text(chord, g_text[i], sizeof g_text[i]);
}

const char* command_text(Command c) {
    ensure_loaded();
    return g_text[static_cast<int>(c)];
}

void chord_text(ImGuiKeyChord chord, char* out, int size) {
    const ImGuiKey key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
    if (key == ImGuiKey_None) {
        out[0] = 0;
        return;
    }
    std::snprintf(out, static_cast<size_t>(size), "%s%s%s%s", (chord & ImGuiMod_Ctrl) ? "Ctrl+" : "", (chord & ImGuiMod_Shift) ? "Shift+" : "",
                  (chord & ImGuiMod_Alt) ? "Alt+" : "", ImGui::GetKeyName(key));
}

ImGuiKeyChord parse_chord(const char* text) {
    ImGuiKeyChord mods = 0;
    for (;;) {
        if (std::strncmp(text, "Ctrl+", 5) == 0) mods |= ImGuiMod_Ctrl, text += 5;
        else if (std::strncmp(text, "Shift+", 6) == 0) mods |= ImGuiMod_Shift, text += 6;
        else if (std::strncmp(text, "Alt+", 4) == 0) mods |= ImGuiMod_Alt, text += 4;
        else break;
    }
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        if (std::strcmp(ImGui::GetKeyName(static_cast<ImGuiKey>(k)), text) == 0) return mods | k;
    }
    return 0;
}

bool command_pressed(Command c, bool repeat) {
    const ImGuiKeyChord chord = command_chord(c);
    if (!chord || keymap_capturing()) return false;
    return ImGui::GetIO().KeyMods == (chord & ImGuiMod_Mask_) && ImGui::IsKeyPressed(static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_), repeat);
}

void load_keymap(const EditorSettings& settings) {
    g_loaded = false;
    ensure_loaded();
    for (const ShortcutOverride& o : settings.shortcuts) {
        for (int i = 0; i < kCount; ++i) {
            if (std::strcmp(kCommands[i].id, o.command) == 0) set_command_chord(static_cast<Command>(i), parse_chord(o.keys));
        }
    }
}

void save_keymap(EditorSettings& settings) {
    settings.shortcuts.count = 0;
    for (int i = 0; i < kCount; ++i) {
        if (g_chords[i] == kCommands[i].chord) continue;
        ShortcutOverride o;
        std::snprintf(o.command, sizeof o.command, "%s", kCommands[i].id);
        std::snprintf(o.keys, sizeof o.keys, "%s", g_text[i]);
        settings.shortcuts.push_back(o);
    }
}

bool keymap_capturing() { return g_capture >= 0 || g_captured_frame == ImGui::GetFrameCount(); }

void draw_keymap(EditorSettings& settings) {
    ensure_loaded();
    static char filter[64] = "";
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##keyfilter", "Search commands or keys", filter, sizeof filter);
    ImGui::SameLine();
    if (ImGui::Button("Reset All")) {
        for (int i = 0; i < kCount; ++i) set_command_chord(static_cast<Command>(i), kCommands[i].chord);
        save_keymap(settings);
    }
    ImGui::SetItemTooltip("Every shortcut back to its default key");

    if (g_capture >= 0) {  // the next key (with Ctrl / Shift / Alt held) becomes the shortcut; Esc cancels
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_GamepadStart; ++k) {
            const ImGuiKey key = static_cast<ImGuiKey>(k);
            if ((key >= ImGuiKey_LeftCtrl && key <= ImGuiKey_RightSuper) || !ImGui::IsKeyPressed(key, false)) continue;
            if (key != ImGuiKey_Escape) {
                set_command_chord(static_cast<Command>(g_capture), (ImGui::GetIO().KeyMods & ~ImGuiMod_Super) | k);
                save_keymap(settings);
            }
            g_capture = -1;
            g_captured_frame = ImGui::GetFrameCount();
            break;
        }
    }

    const char* category = "";
    bool open = false;
    for (int i = 0; i < kCount; ++i) {
        const CommandInfo& c = kCommands[i];
        if (filter[0] && !ImStristr(c.label, nullptr, filter, nullptr) && !ImStristr(g_text[i], nullptr, filter, nullptr)) continue;
        if (std::strcmp(category, c.category) != 0) {
            category = c.category;
            open = ImGui::TreeNodeEx(category, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_NoTreePushOnOpen);
        }
        if (!open) continue;
        ImGui::PushID(i);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(c.label);
        ImGui::SameLine(260.0f);
        const bool waiting = g_capture == i;
        if (ImGui::Button(waiting ? "Press a key... (Esc cancels)" : g_text[i][0] ? g_text[i] : "(none)", ImVec2(200.0f, 0.0f))) g_capture = waiting ? -1 : i;
        ImGui::SetItemTooltip("Click, then press the new keys (hold Ctrl / Shift / Alt with them)");
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            set_command_chord(static_cast<Command>(i), 0);
            save_keymap(settings);
        }
        ImGui::SetItemTooltip("No key for this command");
        if (g_chords[i] != c.chord) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset")) {
                set_command_chord(static_cast<Command>(i), c.chord);
                save_keymap(settings);
            }
            ImGui::SetItemTooltip("Back to %s", c.chord ? ImGui::GetKeyChordName(c.chord) : "none");
        }
        for (int k = 0; k < kCount; ++k) {  // the same keys on another command that works at the same time
            if (k == i || !g_chords[i] || g_chords[k] != g_chords[i] || is_play(k) != is_play(i)) continue;
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "also %s", kCommands[k].label);
            break;
        }
        ImGui::PopID();
    }
}

} // namespace lsd::editor
