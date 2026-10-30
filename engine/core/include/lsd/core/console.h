#pragma once
// Console variables and commands, like Unreal's CVars and exec commands: `r.Grid 0`, `stat fps`.
// Variables point at existing values (no copies), so the owner keeps using its own field.
// Lines are "name [args...]"; a variable with no argument prints its value and help.
#include "lsd/foundation/types.h"

namespace lsd::console {

enum class VarType : u8 { Bool, I32, F32 };

// args = the text after the name (trimmed, may be empty).
using Command = void (*)(void* user, const char* args);

// Registration keeps the pointers; names and help must outlive the console (string literals).
void add_variable(const char* name, bool* value, const char* help);
void add_variable(const char* name, i32* value, const char* help);
void add_variable(const char* name, f32* value, const char* help);
void add_command(const char* name, Command command, void* user, const char* help);
void remove_all();  // drops every registration (tests, shutdown)

// Runs one line. Results and errors go to the log ("console" channel). False if the name is unknown
// or the value does not parse.
bool execute(const char* line);

// Names starting with `prefix` (case-insensitive), for completion; returns how many were written.
u32 complete(const char* prefix, const char** out, u32 max);
const char* help(const char* name);  // nullptr if unknown

} // namespace lsd::console
