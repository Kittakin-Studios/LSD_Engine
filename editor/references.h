#pragma once
// Which assets use which (Unreal's Reference Viewer): assets refer to each other by GUID in their
// text (levels, prefabs, data assets, graphs) and in their .meta, so a scan of those files finds
// every link. Also replaces references, which fixes broken ones (Unreal's Replace References).
// shortcut: a full scan when asked (the viewer opens, a delete asks); cache it per file time if
// projects get thousands of text assets.
#include "lsd/asset/asset_registry.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace lsd::editor {

class ReferenceIndex {
public:
    void build(const asset::AssetRegistry& registry);
    // GUIDs (32 hex digits) the asset mentions; missing ones are not in the registry.
    const std::vector<std::string>& uses(const std::string& guid) const;
    std::vector<std::string> used_by(const std::string& guid) const;

private:
    std::unordered_map<std::string, std::vector<std::string>> uses_;
};

// The assets a packaged game needs (Unreal's cook), as GUIDs: what `root_text` (the project
// settings) names by GUID, asset path or asset name in a quoted string, then everything those use,
// followed through GUIDs and, in text assets (levels, data, graphs, scripts, UI), through quoted
// names and paths too (lsd.play_sound("Door"), open_level("Level2"), a stylesheet's href). A UI
// document brings its own .js. shortcut: a name built at run time ("Level" .. n) is not found; such
// assets need a GUID reference somewhere, or cooking left off.
std::vector<std::string> cook_set(const asset::AssetRegistry& registry, const std::string& root_text);

// Every GUID-like run of 32 hex digits in `text` (each once).
std::vector<std::string> find_guids(const std::string& text);
// Rewrites `from` to `to` in every text asset and .meta; returns how many files changed.
u32 replace_references(const asset::AssetRegistry& registry, const std::string& from, const std::string& to);

} // namespace lsd::editor
