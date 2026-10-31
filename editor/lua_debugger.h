#pragma once
// The Lua debugger (Tools > Lua Debugger) and the plumbing visual-script debugging uses: breakpoints
// by file and line, a stop that shows the call stack and locals while the game waits, stepping, a
// watch list, and a trace of which lines just ran (the graph editor lights its nodes with it).
#include "editor_ui.h"

#include "lsd/script/script.h"

namespace lsd::editor::debugger {

// Once: the Play session's script system (breakpoints survive Plays), and how to draw a frame
// while a script waits (main.cpp: events, the debugger window, present).
void attach(script::ScriptSystem& scripts);
void set_frame_callback(void (*frame)(void* user), void* user);

void toggle(const char* source, i32 line);
bool has(const char* source, i32 line);
bool stopped();
const script::DebugStop* stop();  // the current stop (nullptr when running)
void resume(script::DebugAction action);

// Live flow: lines of `source` that ran, as seconds since they last did (huge if never).
void trace_source(const char* source);  // one file at a time (the open visual script's Lua); "" = off
f64 seconds_since(i32 line);

void draw_window(EditorState& state, bool* open);

} // namespace lsd::editor::debugger
