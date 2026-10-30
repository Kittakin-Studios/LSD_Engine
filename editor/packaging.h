#pragma once
// Package Project (Unreal's File > Package Project): a folder that runs on another PC.
//
//   Build/<Project Name>/
//     <Project Name>.exe   the engine (the Release build when there is one) in game mode
//     game.json            marks the folder as a packaged game: the exe starts the project's
//                          Game Default Level in Play, full window, no editor
//     Content/  Config/    the project
//     vcruntime / msvcp    the Visual C++ runtime, app-local, for PCs without it
//
// Assets stay in their source formats and are compiled on first run (DerivedData/), as in the editor.
// shortcut: no cooking, compression or encryption; compile assets into one archive when packages grow.
#include "editor_settings.h"

#include "lsd/foundation/types.h"

namespace lsd::editor {

constexpr char kGameMarker[] = "game.json";

// Writes the package; false with the reason in `message` (also the output folder on success).
bool package_project(const ProjectSettings& project, char* message, usize capacity);

} // namespace lsd::editor
