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
// shortcut: no compression or encryption; compile assets into one archive when packages grow.
#include "editor_settings.h"

#include "lsd/foundation/types.h"

struct SDL_Window;
namespace lsd::asset {
class AssetRegistry;
}

namespace lsd::editor {

constexpr char kGameMarker[] = "game.json";
constexpr char kManifestFile[] = "manifest.lsdmanifest";  // in a package or a DLC package: every file with its hash
constexpr char kPluginsDir[] = "Plugins";
constexpr char kDlcDir[] = "DLC";
constexpr char kBuildDir[] = "Build";

// Mounts the enabled plugins' Content/ (Plugins/<Name>/Content, shown under Plugins/<Name>/) and the DLC packages in DLC/ (merged
// into Content/, overriding the same paths) into the registry; call before a scan (a packaged game finds DLC/ beside its exe).
void mount_extras(asset::AssetRegistry& registry);

// File > Package DLC (Unreal's patch .pak / chunking): Build/DLC/<Name>/ with Content/ (the assets under one content folder or
// plugin, "Plugins/<Name>"; "" = everything) and manifest.lsdmanifest. With `base_manifest` (the manifest.lsdmanifest of a
// package or an earlier DLC) it carries only the assets that are new or changed since.
// shortcut: a plain folder, not one archive file; pack it when download size matters.
bool package_dlc(const asset::AssetRegistry& registry, const char* folder, const char* base_manifest, char* message, usize capacity);

// The packaged game's splash (Project Settings > Platform > Splash), copied beside the exe as
// splash.<ext>: a borderless window over the game's while it loads (SDL's 2D renderer, so it shows
// before the graphics device has anything to draw). Null when there is none.
SDL_Window* open_splash();
void close_splash(SDL_Window* splash);

// Build > Build Release Engine: the Release build packaging copies, made from the engine's own source tree
// (found above the running exe: CMakePresets.json) with `cmake --build --preset windows-msvc --config Release`,
// on a background thread; output in Saved/Logs/ReleaseBuild.log. False with the reason when it cannot start
// (no source tree, no Visual Studio, one already running).
bool start_release_build(char* message, usize capacity);
// While one runs: true, `line` the build's latest output line. Once done: false, and `finished` / `ok` say how it went
// (reported once: the next call returns finished = false).
bool release_build_running(char* line, usize capacity, bool& finished, bool& ok);

// Writes the package; false with the reason in `message` (also the output folder on success).
// Validates the assets first (problems go to the Message Log; they do not stop the package).
bool package_project(const ProjectSettings& project, const asset::AssetRegistry& registry, char* message, usize capacity);

// Unreal's Data Validation: references to assets that no longer exist (GUIDs in levels, prefabs,
// data assets and graphs), oversize textures and (if the project asks) name prefixes. Each
// problem is a warning in the Message Log; returns how many.
u32 validate_assets(const ProjectSettings& project, const asset::AssetRegistry& registry);

} // namespace lsd::editor
