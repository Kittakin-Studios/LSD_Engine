#pragma once
// Plugins and DLC / patch packages (Unreal's Plugins and Chunking / patch .pak).
//
//   Plugins/<Name>/plugin.json   descriptor (PluginDescriptor)
//   Plugins/<Name>/Content/      assets, shown in the Content Browser under Plugins/<Name>/
//   Plugins/<Name>/Source/       optional C / C++ game code, built into Game.dll while the plugin is enabled
//   DLC/<Name>/Content/          a DLC / patch package: the shipped game mounts it at start, its files override
//   DLC/<Name>/manifest.lsdmanifest   every file with its hash, so a later patch can diff against it
#include "lsd/asset/asset_registry.h"
#include "lsd/reflect/reflect.h"

#include <string>
#include <vector>

namespace lsd::asset {

LSD_STRUCT(Category = "Plugin", Tooltip = "A plugin's descriptor: Plugins/<Name>/plugin.json")
struct PluginDescriptor {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The plugin's name (also its folder name)") char name[48] = "";
    LSD_PROPERTY(Edit, Tooltip = "Its version, free text such as 1.0.0") char version[16] = "1.0.0";
    LSD_PROPERTY(Edit, Tooltip = "What the plugin is for") char description[200] = "";
    LSD_PROPERTY(Edit, Tooltip = "Mounted into the Content Browser, built into the game code and packaged only while this is on") bool enabled = true;
};

struct PluginInfo {
    PluginDescriptor descriptor;
    std::string folder;  // "Plugins/<Name>"
};

// True for a relative path that stays inside its folder: no "..", no drive or leading slash, no backslash.
bool safe_relative_path(const char* path);

// Every Plugins/<Name>/plugin.json under `root`, by name. The descriptor's name is the folder name (a mismatch is fixed up).
std::vector<PluginInfo> discover_plugins(const char* root);
// Writes Plugins/<Name>/plugin.json and Content/; false with the reason when the name is bad or taken.
bool create_plugin(const char* root, const char* name, std::string& error);
bool set_plugin_enabled(const char* root, const char* name, bool enabled);
// Content mounts of the enabled plugins ("Plugins/<Name>" -> Plugins/<Name>/Content).
std::vector<AssetMount> plugin_mounts(const std::vector<PluginInfo>& plugins);
// The C / C++ files of the enabled plugins' Source/ folders (added to the Game.dll build).
std::vector<std::string> plugin_source_files(const std::vector<PluginInfo>& plugins);

// ---- DLC / patch packages ----
struct ManifestEntry {
    std::string path;  // relative to the package's Content/, '/' separators
    u64 hash = 0;      // FNV-1a of the file's bytes
    u64 size = 0;
};
struct Manifest {
    std::string name;
    std::vector<ManifestEntry> entries;
};

u64 hash_file(const char* path);                       // 0 when it cannot be read
Manifest manifest_of_folder(const char* dir);          // every file below dir except .meta sidecars
std::string manifest_to_text(const Manifest& manifest);
bool manifest_from_text(const std::string& text, Manifest& out);  // false on a bad line or an unsafe path
bool save_manifest(const Manifest& manifest, const char* path);
bool load_manifest(const char* path, Manifest& out);
// True when `base` lists `path` with exactly this hash (the asset is unchanged since).
bool manifest_matches(const Manifest& base, const std::string& path, u64 hash);
// The entries of `now` that are new or whose hash differs from `base` (what a patch must carry).
std::vector<std::string> manifest_changed(const Manifest& base, const Manifest& now);

// Content mounts of the DLC packages in `root` (DLC/<Name>/Content, merged into the content root), name order:
// a later name overrides an earlier one, so Patch2 beats Patch1.
std::vector<AssetMount> dlc_mounts(const char* root);

} // namespace lsd::asset
