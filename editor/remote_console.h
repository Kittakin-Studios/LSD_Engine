#pragma once
// Remote console (Unreal's remote console / Session Frontend console): a game started with
// -remote[=port] listens on 127.0.0.1 for console lines; the editor's Tools > Remote Console window
// sends them and shows what each one logged. Text over TCP, one line per command; each reply ends
// with a line holding a single "." .
// shortcut: this PC only (localhost), one client at a time, no password; add both before consoles or LAN devkits.
#include "lsd/foundation/types.h"

namespace lsd::editor::remote {

constexpr u16 kDefaultPort = 7780;

// Game side.
bool listen(u16 port);
void poll();  // every frame on the main thread: accepts, runs the lines that arrived
void close();

// Editor side.
void draw_window(bool* open);

} // namespace lsd::editor::remote
