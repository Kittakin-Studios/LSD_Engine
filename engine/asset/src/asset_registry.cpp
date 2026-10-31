#include "lsd/asset/asset_registry.h"
#include "lsd/asset/data_asset.h"
#include "lsd/foundation/log.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lsd::asset {

namespace fs = std::filesystem;

namespace {

void copy_string(char* out, usize capacity, const char* text) {
    std::snprintf(out, capacity, "%s", text);
}

bool contains_ignore_case(const char* haystack, const char* needle) {
    if (!*needle) return true;
    for (; *haystack; ++haystack) {
        const char* h = haystack;
        const char* n = needle;
        while (*h && *n && std::tolower(static_cast<unsigned char>(*h)) == std::tolower(static_cast<unsigned char>(*n))) { ++h; ++n; }
        if (!*n) return true;
    }
    return false;
}

i64 write_time(const fs::path& path) {
    std::error_code error;
    return static_cast<i64>(fs::last_write_time(path, error).time_since_epoch().count());
}

} // namespace

const char* to_string(AssetType type) {
    switch (type) {
        case AssetType::Mesh: return "Mesh";
        case AssetType::Level: return "Level";
        case AssetType::Data: return "Data";
        case AssetType::Prefab: return "Prefab";
        case AssetType::Script: return "Script";
        case AssetType::Graph: return "Visual Script";
        case AssetType::Table: return "Data Table";
        case AssetType::Sound: return "Sound";
        case AssetType::Cue: return "Sound Cue";
        case AssetType::Texture: return "Texture";
        case AssetType::MaterialGraph: return "Material Graph";
        case AssetType::BehaviorTree: return "Behavior Tree";
        case AssetType::UiDocument: return "UI Document";
        case AssetType::UiStyle: return "UI Style";
        case AssetType::Unknown: break;
    }
    return "Unknown";
}

AssetType type_from_extension(const char* extension) {
    char lower[16] = "";
    for (usize i = 0; i + 1 < sizeof lower && extension[i]; ++i) lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(extension[i])));
    if (std::strcmp(lower, ".fbx") == 0 || std::strcmp(lower, ".gltf") == 0 || std::strcmp(lower, ".glb") == 0) return AssetType::Mesh;
    if (std::strcmp(lower, ".lsdlevel") == 0) return AssetType::Level;
    if (std::strcmp(lower, ".lsdasset") == 0) return AssetType::Data;
    if (std::strcmp(lower, ".lsdprefab") == 0) return AssetType::Prefab;
    if (std::strcmp(lower, ".lua") == 0) return AssetType::Script;
    if (std::strcmp(lower, ".lsdgraph") == 0) return AssetType::Graph;
    if (std::strcmp(lower, ".csv") == 0) return AssetType::Table;
    if (std::strcmp(lower, ".lsdcue") == 0) return AssetType::Cue;
    if (std::strcmp(lower, ".lsdmatgraph") == 0) return AssetType::MaterialGraph;
    if (std::strcmp(lower, ".lsdbt") == 0) return AssetType::BehaviorTree;
    if (std::strcmp(lower, ".rml") == 0) return AssetType::UiDocument;
    if (std::strcmp(lower, ".rcss") == 0) return AssetType::UiStyle;
    for (const char* image : {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd"}) {
        if (std::strcmp(lower, image) == 0) return AssetType::Texture;
    }
    if (std::strcmp(lower, ".wav") == 0 || std::strcmp(lower, ".ogg") == 0 || std::strcmp(lower, ".flac") == 0 || std::strcmp(lower, ".mp3") == 0) return AssetType::Sound;
    return AssetType::Unknown;
}

AssetRegistry::AssetRegistry(Allocator& allocator, const char* content_dir) : records_(allocator) {
    copy_string(content_dir_, sizeof content_dir_, content_dir);
}

const AssetRecord* AssetRegistry::find(Guid guid) const {
    for (const AssetRecord& r : records_) {
        if (r.guid == guid) return &r;
    }
    return nullptr;
}

void AssetRegistry::source_path(const AssetRecord& record, char* out, usize capacity) const {
    std::snprintf(out, capacity, "%s/%s", content_dir_, record.path);
}

void AssetRegistry::meta_path(const AssetRecord& record, char* out, usize capacity) const {
    std::snprintf(out, capacity, "%s/%s.meta", content_dir_, record.path);
}

bool AssetRegistry::load_meta(const AssetRecord& record, AssetMeta& out) const {
    char path[300];
    meta_path(record, path, sizeof path);
    return reflect::load_file(out, path);
}

bool AssetRegistry::save_meta(const AssetRecord& record, const AssetMeta& meta) {
    char path[300];
    meta_path(record, path, sizeof path);
    if (!reflect::save_file(meta, path)) return false;
    for (AssetRecord& r : records_) {
        if (std::strcmp(r.path, record.path) != 0) continue;
        copy_string(r.tags, sizeof r.tags, meta.tags);
        r.meta_time = write_time(path);
    }
    return true;
}

// shortcut: std containers here (editor-time file walking, not a hot path).
struct AssetRegistry::Walk {
    struct File {
        std::string relative;  // '/' separators
        AssetType type;
        u64 size;
        i64 time;
        i64 meta_time;
    };
    bool ok = false;  // false when Content/ is missing: then nothing is dropped
    std::vector<File> files;
};

struct AssetRegistry::Async {
    std::thread thread;
    std::atomic<bool> done{false};
    u32 generation = 0;  // sync_scans_ when the walk started
    Walk walk;
};

namespace {

// Only touches the file system and its own result, so it is safe on a worker thread.
void walk_content(const char* content_dir, AssetRegistry::Walk& out) {
    std::error_code error;
    if (!fs::is_directory(content_dir, error)) return;
    out.ok = true;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(content_dir, error)) {
        if (!entry.is_regular_file(error)) continue;
        const AssetType type = type_from_extension(entry.path().extension().string().c_str());
        if (type == AssetType::Unknown) continue;
        const fs::path meta = fs::path(entry.path().string() + ".meta");
        out.files.push_back({fs::relative(entry.path(), content_dir, error).generic_string(), type, static_cast<u64>(entry.file_size(error)),
                             write_time(entry.path()), fs::exists(meta, error) ? write_time(meta) : 0});
    }
}

} // namespace

AssetRegistry::~AssetRegistry() {
    if (async_ && async_->thread.joinable()) async_->thread.join();
    delete async_;
}

bool AssetRegistry::scan() {
    ++sync_scans_;
    Walk walk;
    walk_content(content_dir_, walk);
    return apply(walk);
}

bool AssetRegistry::scan_async() {
    bool changed = false;
    if (async_) {
        if (!async_->done.load(std::memory_order_acquire)) return false;  // still walking
        async_->thread.join();
        if (async_->generation == sync_scans_) changed = apply(async_->walk);
        delete async_;
        async_ = nullptr;
    }
    async_ = new Async();
    async_->generation = sync_scans_;
    async_->thread = std::thread([a = async_, dir = std::string(content_dir_)] {
        walk_content(dir.c_str(), a->walk);
        a->done.store(true, std::memory_order_release);
    });
    return changed;
}

bool AssetRegistry::apply(const Walk& walk) {
    if (!walk.ok) return false;
    bool changed = false;

    std::unordered_map<std::string, const Walk::File*> on_disk;
    on_disk.reserve(walk.files.size());
    for (const Walk::File& f : walk.files) on_disk.emplace(f.relative, &f);

    // Drop records whose source file is gone; refresh the rest.
    std::unordered_map<std::string, usize> known;
    for (usize i = records_.size(); i-- > 0;) {
        if (!on_disk.count(records_[i].path)) {
            records_.remove_swap(i);
            changed = true;
        }
    }
    for (usize i = 0; i < records_.size(); ++i) known.emplace(records_[i].path, i);

    for (const Walk::File& f : walk.files) {
        char full[300];
        std::snprintf(full, sizeof full, "%s/%s", content_dir_, f.relative.c_str());
        const auto it = known.find(f.relative);
        if (it != known.end()) {
            AssetRecord& existing = records_[it->second];
            if (existing.source_size != f.size || existing.source_time != f.time) {
                existing.source_size = f.size;
                existing.source_time = f.time;
                if (f.type == AssetType::Data) read_data_asset_type(full, existing.data_type, sizeof existing.data_type);
                changed = true;
            }
            if (existing.meta_time != f.meta_time) {  // edited by hand or by another tool
                existing.meta_time = f.meta_time;
                AssetMeta meta;
                if (load_meta(existing, meta)) copy_string(existing.tags, sizeof existing.tags, meta.tags);
                changed = true;
            }
            continue;
        }

        AssetRecord record;
        record.type = f.type;
        record.source_size = f.size;
        record.source_time = f.time;
        record.meta_time = f.meta_time;
        copy_string(record.path, sizeof record.path, f.relative.c_str());
        copy_string(record.folder, sizeof record.folder, fs::path(f.relative).parent_path().generic_string().c_str());
        copy_string(record.name, sizeof record.name, fs::path(f.relative).stem().string().c_str());
        if (f.type == AssetType::Data && !read_data_asset_type(full, record.data_type, sizeof record.data_type)) {
            LSD_LOG_WARN("asset", "%s is not a valid data asset", f.relative.c_str());
        }

        // Read the GUID from the .meta; create one if missing, invalid, or already taken
        // (a file copied together with its .meta must become a new asset).
        AssetMeta meta;
        const bool had_meta = load_meta(record, meta);
        const bool guid_ok = had_meta && parse_guid(meta.guid, record.guid) && !find(record.guid);
        if (!guid_ok) {
            record.guid = new_guid();
            to_string(record.guid, meta.guid);
            copy_string(meta.type, sizeof meta.type, to_string(f.type));
            char path[300];
            meta_path(record, path, sizeof path);
            if (!reflect::save_file(meta, path)) LSD_LOG_WARN("asset", "Could not write %s.meta", record.path);
            else record.meta_time = write_time(path);
            if (had_meta) LSD_LOG_INFO("asset", "%s had a duplicate or invalid GUID; assigned a new one.", record.path);
        }
        copy_string(record.tags, sizeof record.tags, meta.tags);
        known.emplace(f.relative, records_.size());
        records_.push_back(record);
        changed = true;
    }
    return changed;
}

bool valid_name(const char* name) {
    if (!name || !*name || std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) return false;
    for (const char* c = name; *c; ++c) {
        if (static_cast<unsigned char>(*c) < 0x20 || std::strchr("\\/:*?\"<>|", *c)) return false;
    }
    const char last = name[std::strlen(name) - 1];
    return last != ' ' && last != '.';  // Windows silently strips these
}

namespace {

// Moves a source file plus its .meta (and a .gltf's .bin) to `to` (path without extension change).
bool move_with_meta(const fs::path& from, const fs::path& to, char* error, usize error_capacity) {
    std::error_code ec;
    if (fs::exists(to, ec)) {
        std::snprintf(error, error_capacity, "%s already exists", to.filename().string().c_str());
        return false;
    }
    fs::rename(from, to, ec);
    if (ec) {
        std::snprintf(error, error_capacity, "%s", ec.message().c_str());
        return false;
    }
    fs::rename(fs::path(from.string() + ".meta"), fs::path(to.string() + ".meta"), ec);
    if (from.extension() == ".gltf") fs::rename(fs::path(from).replace_extension(".bin"), fs::path(to).replace_extension(".bin"), ec);
    return true;
}

} // namespace

bool AssetRegistry::rename_asset(AssetRecord record, const char* new_name, char* error, usize error_capacity) {
    if (!valid_name(new_name)) {
        std::snprintf(error, error_capacity, "'%s' is not a valid name", new_name ? new_name : "");
        return false;
    }
    char path[300];
    source_path(record, path, sizeof path);
    const fs::path from(path);
    const fs::path to = from.parent_path() / (std::string(new_name) + from.extension().string());
    if (!move_with_meta(from, to, error, error_capacity)) return false;
    scan();
    return true;
}

bool AssetRegistry::move_asset(AssetRecord record, const char* folder, char* error, usize error_capacity) {
    char path[300];
    source_path(record, path, sizeof path);
    const fs::path from(path);
    const fs::path dir = fs::path(content_dir_) / folder;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        std::snprintf(error, error_capacity, "Folder Content/%s does not exist", folder);
        return false;
    }
    if (!move_with_meta(from, dir / from.filename(), error, error_capacity)) return false;
    scan();
    return true;
}

bool AssetRegistry::delete_asset(AssetRecord record, char* error, usize error_capacity) {
    char path[300];
    source_path(record, path, sizeof path);
    std::error_code ec;
    if (!fs::remove(path, ec)) {
        std::snprintf(error, error_capacity, "Could not delete %s: %s", record.path, ec.message().c_str());
        return false;
    }
    fs::remove(std::string(path) + ".meta", ec);
    if (fs::path(path).extension() == ".gltf") fs::remove(fs::path(path).replace_extension(".bin"), ec);
    scan();
    return true;
}

usize AssetRegistry::search(const char* text, AssetType type, const char* folder, usize* out, usize max) const {
    usize found = 0;
    for (usize i = 0; i < records_.size() && found < max; ++i) {
        const AssetRecord& r = records_[i];
        if (type != AssetType::Unknown && r.type != type) continue;
        if (text && *text) {
            if (!contains_ignore_case(r.name, text) && !contains_ignore_case(r.tags, text)) continue;
        } else if (folder && std::strcmp(r.folder, folder) != 0) {
            continue;
        }
        out[found++] = i;
    }
    return found;
}

} // namespace lsd::asset
