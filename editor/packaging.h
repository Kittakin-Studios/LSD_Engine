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

struct SDL_Window;
namespace lsd::asset {
class AssetRegistry;
}

namespace lsd::editor {

constexpr char kGameMarker[] = "game.json";

// The packaged game's splash (Project Settings > Platform > Splash), copied beside the exe as
// splash.<ext>: a borderless window over the game's while it loads (SDL's 2D renderer, so it shows
// before the graphics device has anything to draw). Null when there is none.
SDL_Window* open_splash();
void close_splash(SDL_Window* splash);

// Writes the package; false with the reason in `message` (also the output folder on success).
// Validates the assets first (problems go to the Message Log; they do not stop the package).
bool package_project(const ProjectSettings& project, const asset::AssetRegistry& registry, char* message, usize capacity);

// Unreal's Data Validation: references to assets that no longer exist (GUIDs in levels, prefabs,
// data assets and graphs), oversize textures and (if the project asks) name prefixes. Each
// problem is a warning in the Message Log; returns how many.
u32 validate_assets(const ProjectSettings& project, const asset::AssetRegistry& registry);

} // namespace lsd::editor
