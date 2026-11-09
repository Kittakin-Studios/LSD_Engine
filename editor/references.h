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
    // How many assets use each GUID (one pass, for the Asset Audit's column).
    std::unordered_map<std::string, u32> user_counts() const;

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

// Size Map (Unreal's Size Map): the roots and everything they use, followed as cook_set follows
// them (GUIDs, and names / paths in scripts and text assets), each with its file size, biggest first.
struct SizeEntry {
    usize record = 0;  // index in the registry
    u64 bytes = 0;
};
std::vector<SizeEntry> size_map(const asset::AssetRegistry& registry, const std::vector<std::string>& root_guids);

// Asset Audit (Unreal's Asset Audit): one row per asset. `cooked` is cook_set of the project
// settings; unused = no asset names it by GUID and the game does not reach it by name either.
struct AuditRow {
    usize record = 0;
    u64 disk = 0;     // the source file
    u64 derived = 0;  // its compiled data in `derived_dir` (files named <guid>...)
    u32 users = 0;
    bool unused = false;
};
std::vector<AuditRow> audit_assets(const asset::AssetRegistry& registry, const ReferenceIndex& index, const std::vector<std::string>& cooked, const char* derived_dir);

// Fix Up Redirectors: GUID references survive a rename or move, but scripts, UI and text assets
// can name an asset by its name or path ("Content/Levels/Main.lsdlevel", lsd.play_sound("Door")),
// and those go stale. Renames and moves are recorded (old path + GUID); the fix-up rewrites them.
struct Redirect {
    std::string from;  // the old path, relative to Content/, e.g. "Sounds/Door.wav"
    asset::Guid guid;
};
// Rewrites quoted "from" / "from" without its extension / "Content/..." of those to `to` in `text`,
// and the bare old name to the new one when `names`; returns how many strings changed.
u32 fix_moved_names(std::string& text, const std::string& from, const std::string& to, bool names);
// Applies every redirect to the scripts, UI and text assets; returns how many files changed.
u32 fix_up_redirectors(const asset::AssetRegistry& registry, const std::vector<Redirect>& redirects);
// Applies the redirects to one text (the project settings); returns how many strings changed.
u32 fix_up_text(const asset::AssetRegistry& registry, const std::vector<Redirect>& redirects, std::string& text);

} // namespace lsd::editor
