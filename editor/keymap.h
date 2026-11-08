#pragma once
// Editor keyboard shortcuts (Unreal's Editor Preferences > Keyboard Shortcuts): every level-editor
// command has a default key chord; the user's changes are saved in Saved/editor_settings.json.
// Save / Undo / Redo / Copy / Cut / Paste / Duplicate / Delete also work in the asset editor windows
// (graphs, tables, UI, timelines, state machines), like Unreal's generic commands.
#include <imgui.h>

namespace lsd::editor {

struct EditorSettings;

enum class Command {
    Play, Simulate, PlayWindow, CompileGameCode,
    StopPlay, ReleaseMouse, Pause, StepFrame, Eject, KeepSimulation,
    Undo, Redo, RedoAlt, Save, SaveAs, NewLevel,
    Copy, Cut, Paste, Duplicate, Delete, SelectAll, Deselect, Group, Ungroup, FindInContentBrowser,
    Translate, Rotate, Scale, LocalGizmo, PivotEdit, Focus, DropToFloor, GameView, ToggleNavigation,
    ViewPerspective, ViewTop, ViewFront, ViewRight,
    Lit, Unlit, Wireframe, LightingOnly, Overdraw, Collision,
    Continue, StepOver, StepInto, StepOut, ToggleBreakpoint,
    Count
};

struct CommandInfo {
    const char* id;        // saved in the settings
    const char* label;     // shown in Preferences
    const char* category;
    ImGuiKeyChord chord;   // the default
};

const CommandInfo& command_info(Command c);
ImGuiKeyChord command_chord(Command c);             // the current chord (0: none)
void set_command_chord(Command c, ImGuiKeyChord chord);
const char* command_text(Command c);                // "Ctrl+S", "" when unbound (menus show it)

// "Ctrl+Shift+S" <-> chord; 0 when the text names no key.
void chord_text(ImGuiKeyChord chord, char* out, int size);
ImGuiKeyChord parse_chord(const char* text);

// The chord's key went down this frame with exactly its modifiers held.
bool command_pressed(Command c, bool repeat = false);

void load_keymap(const EditorSettings& settings);   // defaults, then the user's changes
void save_keymap(EditorSettings& settings);         // only the chords that differ from the defaults

// Editor Preferences > Keyboard Shortcuts. True while it waits for a key (shortcuts are off then).
void draw_keymap(EditorSettings& settings);
bool keymap_capturing();

} // namespace lsd::editor
