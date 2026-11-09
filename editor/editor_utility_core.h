#pragma once
// Editor Utility scripts and widgets (Unreal's Editor Utility Blueprints / Widgets): Lua files or visual
// scripts that run in the editor on the level being edited (never in Play or a packaged game). They get
// the whole lsd API plus lsd.editor:
//
//   function run(self) ... end            what Run does (a visual script: its Event Start, on_start(self))
//   lsd.editor.selection() -> {e, ...}    the selected objects; lsd.editor.select({e, ...}) replaces them
//   lsd.editor.open_asset(name) -> bool   what a double-click in the Content Browser does
//   lsd.editor.console(line)              a console command
//   lsd.editor.panel(title, {{"button", "Label", "function"}, {"checkbox" | "number" | "text", "Label",
//                    "variable", default}, {"label", "Text"}})   a dockable panel; widgets set the script's
//                                         globals, buttons call its functions
//
// Everything a run (or a button) changes in the level is one undo step. Pure logic, tested in editor_tests;
// editor_utility.cpp draws the panels and menus.
#include "lsd/script/script.h"
#include "lsd/world/world.h"

#include <string>
#include <vector>

namespace lsd::editor {

struct UtilityWidget {
    enum class Kind : u8 { Label, Button, Checkbox, Number, Text };
    Kind kind = Kind::Label;
    char label[64] = "";
    char name[64] = "";  // the function a Button calls, or the variable the others set
    bool on = false;
    f64 number = 0;
    char text[256] = "";
};

struct UtilityPanel {
    std::string script;  // the file that declared it (one panel per script)
    char title[64] = "";
    std::vector<UtilityWidget> widgets;
    bool open = true;
};

// What the editor lends the scripts; tests fill in their own.
struct UtilityHooks {
    void* user = nullptr;
    u32 (*selection)(void* user, world::Entity* out, u32 capacity) = nullptr;
    void (*select)(void* user, const world::Entity* list, u32 count) = nullptr;
    bool (*open_asset)(void* user, const char* name) = nullptr;
    void (*console)(void* user, const char* line) = nullptr;
    world::Entity (*spawn)(void* user, const char* name, Vec3 position) = nullptr;
    u32 (*assets)(void* user, const char* type, const char* folder, char* out, usize capacity) = nullptr;
};

class EditorUtilities {
public:
    void start(world::World& world, const UtilityHooks& hooks);  // once; the world object stays the editor's
    bool started() const { return world_ != nullptr; }
    // Runs a utility (a .lua file, or a visual script's compiled one): its code, then run(self) or on_start(self).
    // True when the level changed (the caller makes it an undo step). Errors go to the Output Log.
    bool run_file(const char* path);
    bool run_code(const char* path, const char* code);
    // A panel's button was clicked: calls its function. True when the level changed.
    bool press(u32 panel, u32 widget);
    // A panel's checkbox / number / text was edited: the script's variable takes the widget's value.
    void push_value(u32 panel, u32 widget);
    std::vector<UtilityPanel>& panels() { return panels_; }

private:
    static bool editor_call(void* user, const char* command, const char* arg, char* out, usize capacity);
    bool changed_since(const std::string& before) const;
    std::string level_text() const;

    script::ScriptSystem scripts_;
    world::World* world_ = nullptr;
    UtilityHooks hooks_;
    std::string current_;  // the script running now (its panel calls)
    std::vector<UtilityPanel> panels_;
};

// `text` as a Lua string literal (a long bracket no line of it closes).
std::string lua_quote(const char* text);

} // namespace lsd::editor
