#pragma once
// Remote console (Unreal's remote console / Session Frontend console): a game started with
// -remote[=port] listens for console lines; the editor's Tools > Remote Console window sends them and
// shows what each one logged. Text over TCP, one line per command; each reply ends with a line
// holding a single "." . Without a password it listens on 127.0.0.1 (this PC); with
// -remotepassword=<text> on every network, and a client's first line must be "auth <text>" (a wrong
// one closes the connection; five stop the listener).
// shortcut: one client at a time, the password in clear text over the LAN; add TLS before the internet.
#include "lsd/foundation/types.h"

#include <string>

namespace lsd::editor::remote {

constexpr u16 kDefaultPort = 7780;

// Game side (the editor too, for the Claude MCP plugin: tools/mcp/lsd_mcp bridges to it).
bool listen(u16 port, const char* password = nullptr);  // password: every network, else this PC only
void poll();  // every frame on the main thread: accepts, runs the lines that arrived
void close();
void stop_listening();  // closes the listener and its client, keeps the editor-side connection
bool listening();
bool client_connected();
// Text for the client running the current line (answers of list commands); the Output Log otherwise.
void print(const char* text);
// Runs one console line and returns what it logged (the remote client and LSD Assistant's tools use it).
std::string run_captured(const char* line);

// Editor side.
void draw_window(bool* open);

} // namespace lsd::editor::remote
