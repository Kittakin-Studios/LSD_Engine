#pragma once
// The project's game code (Unreal: the project's C++ module). Source/ builds Binaries/Game.dll against
// lsd_game.h; the editor loads it from a copy (so a rebuild is never blocked by the loaded file) and
// reloads it whenever the file changes, also during Play. Packaged games load it once.
#include "lsd/script/native.h"

namespace lsd::editor::game_module {

bool load();     // Binaries/Game.dll if there is one; false if none, or it failed (logged)
void unload();   // stop the behaviours using it first (ScriptSystem::stop_natives)
bool changed();  // the file on disk differs from the loaded one (or appeared / went away)
bool loaded();
const script::NativeModule& module();

// Tools > Compile Game Code (Ctrl+Alt+F11): configures and builds Source/ on a background thread with
// the installed MSVC (or the compilers on PATH); the build's lines go to the Output Log.
void compile();
bool compiling();
void shutdown();  // waits for a running compile

// Tools > New C++ Behaviour: Source/<Name>.cpp from a template, plus Source/CMakeLists.txt and
// Source/GameModule.cpp when they are missing. False with the reason in `message`.
bool new_behaviour(const char* name, char* message, usize capacity);

} // namespace lsd::editor::game_module
