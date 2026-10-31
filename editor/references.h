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

// Every GUID-like run of 32 hex digits in `text` (each once).
std::vector<std::string> find_guids(const std::string& text);
// Rewrites `from` to `to` in every text asset and .meta; returns how many files changed.
u32 replace_references(const asset::AssetRegistry& registry, const std::string& from, const std::string& to);

} // namespace lsd::editor
