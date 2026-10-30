#pragma once
// Asset registry: every importable file under Content/ with its GUID, type, path and folder.
// Each source file gets a <file>.meta sidecar (AssetMeta) on first scan; the GUID in it is the
// asset's permanent identity. Like Unreal's Asset Registry, minus tags/dependencies (later).
#include "lsd/asset/asset_meta.h"
#include "lsd/asset/guid.h"
#include "lsd/foundation/array.h"

namespace lsd::asset {

enum class AssetType : u8 { Unknown, Mesh, Level, Data, Prefab, Script, Graph, Table, Sound };

const char* to_string(AssetType type);
AssetType type_from_extension(const char* extension);  // ".fbx" -> Mesh, ".lsdlevel" -> Level, ".lsdasset" -> Data, ".lsdprefab" -> Prefab, ".lua" -> Script, ".lsdgraph" -> Graph, ".csv" -> Table, ".wav" ".ogg" ".flac" ".mp3" -> Sound; case-insensitive

struct AssetRecord {
    Guid guid;
    AssetType type = AssetType::Unknown;
    char path[256] = "";    // relative to the content root, '/' separators, e.g. "Props/Chair.fbx"
    char folder[256] = "";  // "Props" ("" for the root)
    char name[96] = "";     // file name without extension, e.g. "Chair"
    char data_type[32] = "";  // Data assets: the reflected type inside, e.g. "InputAction"
    u64 source_size = 0;
    i64 source_time = 0;    // last write time, for the data compiler's up-to-date check
};

// True for a usable file or folder name: non-empty, not "." / "..", no \ / : * ? \" < > | or control characters.
bool valid_name(const char* name);

class AssetRegistry {
public:
    AssetRegistry(Allocator& allocator, const char* content_dir);

    // Re-reads Content/: adds new files (writing their .meta), drops deleted ones, refreshes
    // sizes and times. Returns true if anything changed.
    // shortcut: synchronous full scan; move to a background job with file-change notifications
    // when projects reach thousands of assets.
    bool scan();

    usize count() const { return records_.size(); }
    const AssetRecord& get(usize i) const { return records_[i]; }
    const AssetRecord* find(Guid guid) const;
    const char* content_dir() const { return content_dir_; }

    // Indices of assets whose name contains `text` (case-insensitive) and whose type matches
    // (Unknown = any). With empty text, only assets directly in `folder` match.
    usize search(const char* text, AssetType type, const char* folder, usize* out, usize max) const;

    // Absolute-or-relative path of the source file and its .meta sidecar.
    void source_path(const AssetRecord& record, char* out, usize capacity) const;
    void meta_path(const AssetRecord& record, char* out, usize capacity) const;

    // File operations that keep each asset's .meta (and therefore its GUID) with it, then rescan.
    // Names are validated (no path separators or characters Windows forbids); `error` says why on failure.
    // Copy `record` first: the operations rescan, which can move records around.
    bool rename_asset(AssetRecord record, const char* new_name, char* error, usize error_capacity);
    bool move_asset(AssetRecord record, const char* folder, char* error, usize error_capacity);
    bool delete_asset(AssetRecord record, char* error, usize error_capacity);

    bool load_meta(const AssetRecord& record, AssetMeta& out) const;
    bool save_meta(const AssetRecord& record, const AssetMeta& meta) const;

private:
    Array<AssetRecord> records_;
    char content_dir_[256] = "";
};

} // namespace lsd::asset
