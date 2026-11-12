#include "lsd/asset/asset_registry.h"
#include "lsd/asset/data_asset.h"
#include "lsd/foundation/log.h"

#include <algorithm>
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
        case AssetType::LightProfile: return "Light Profile";
        case AssetType::PcgGraph: return "PCG Graph";
        case AssetType::Unknown: break;
    }
    return "Unknown";
}

AssetType type_from_extension(const char* extension) {
    char lower[16] = "";
    for (usize i = 0; i + 1 < sizeof lower && extension[i]; ++i) lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(extension[i])));
    if (std::strcmp(lower, ".hlsl") == 0) return AssetType::MaterialGraph;  // a shader asset: a material written as code
    if (std::strcmp(lower, ".fbx") == 0 || std::strcmp(lower, ".gltf") == 0 || std::strcmp(lower, ".glb") == 0 || std::strcmp(lower, ".obj") == 0) return AssetType::Mesh;
    if (std::strcmp(lower, ".lsdlevel") == 0) return AssetType::Level;
    if (std::strcmp(lower, ".lsdasset") == 0) return AssetType::Data;
    if (std::strcmp(lower, ".lsdprefab") == 0) return AssetType::Prefab;
    if (std::strcmp(lower, ".lua") == 0 || std::strcmp(lower, ".js") == 0) return AssetType::Script;
    if (std::strcmp(lower, ".lsdgraph") == 0) return AssetType::Graph;
    if (std::strcmp(lower, ".csv") == 0) return AssetType::Table;
    if (std::strcmp(lower, ".lsdcue") == 0 || std::strcmp(lower, ".lsdsynth") == 0) return AssetType::Cue;  // shortcut: Sound Synths share the Sound Cue type (play, emitters, preview), give them their own when they need their own Content Browser filter
    if (std::strcmp(lower, ".lsdmatgraph") == 0) return AssetType::MaterialGraph;
    if (std::strcmp(lower, ".lsdbt") == 0) return AssetType::BehaviorTree;
    if (std::strcmp(lower, ".rml") == 0) return AssetType::UiDocument;
    if (std::strcmp(lower, ".rcss") == 0) return AssetType::UiStyle;
    if (std::strcmp(lower, ".ies") == 0) return AssetType::LightProfile;
    if (std::strcmp(lower, ".lsdpcg") == 0) return AssetType::PcgGraph;
    for (const char* image : {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd", ".hdr", ".exr"}) {
        if (std::strcmp(lower, image) == 0) return AssetType::Texture;
    }
    if (std::strcmp(lower, ".wav") == 0 || std::strcmp(lower, ".ogg") == 0 || std::strcmp(lower, ".flac") == 0 || std::strcmp(lower, ".mp3") == 0) return AssetType::Sound;
    return AssetType::Unknown;
}

AssetRegistry::AssetRegistry(Allocator& allocator, const char* content_dir) : records_(allocator) {
    copy_string(content_dir_, sizeof content_dir_, content_dir);
}

const AssetRecord* AssetRegistry::find(Guid guid) const {
    const auto it = by_guid_.find(guid);
    return it != by_guid_.end() && it->second < records_.size() ? &records_[it->second] : nullptr;
}

void AssetRegistry::add_folder(const char* relative) {
    const std::string folder(relative);
    const auto at = std::lower_bound(folders_.begin(), folders_.end(), folder);
    if (at != folders_.end() && *at == folder) return;
    folders_.insert(at, folder);
    ++generation_;
}

void AssetRegistry::index_guids() {
    by_guid_.clear();
    by_guid_.reserve(records_.size());
    for (usize i = 0; i < records_.size(); ++i) by_guid_.emplace(records_[i].guid, static_cast<u32>(i));
}

const AssetRecord* AssetRegistry::find_named(const char* name, AssetType type, const char* data_type) const {
    if (!name || !*name) return nullptr;
    if (by_name_generation_ != generation_ || by_name_.size() != records_.size()) {
        by_name_.clear();
        by_name_.reserve(records_.size());
        for (usize i = 0; i < records_.size(); ++i) by_name_.emplace(records_[i].name, static_cast<u32>(i));
        by_name_generation_ = generation_;
    }
    const AssetRecord* best = nullptr;  // several of a name (different folders): the first by path, the same every run
    const auto [from, to] = by_name_.equal_range(name);
    for (auto it = from; it != to; ++it) {
        const AssetRecord& r = records_[it->second];
        if (type != AssetType::Unknown && r.type != type) continue;
        if (data_type && std::strcmp(r.data_type, data_type) != 0) continue;
        if (!best || std::strcmp(r.path, best->path) < 0) best = &r;
    }
    return best;
}

const AssetRecord* AssetRegistry::best_match(const char* name, AssetType type) const {
    if (const AssetRecord* exact = find_named(name, type)) return exact;
    const AssetRecord* best = nullptr;
    bool best_exact = false;  // the whole name ignoring case beats a part of a name
    for (const AssetRecord& r : records_) {
        if ((type != AssetType::Unknown && r.type != type) || !contains_ignore_case(r.name, name)) continue;
        const bool exact = std::strlen(r.name) == std::strlen(name);
        if (!best || (exact && !best_exact) || (exact == best_exact && std::strcmp(r.name, best->name) < 0)) {
            best = &r;
            best_exact = exact;
        }
    }
    return best;
}

// The record's path inside its mount's folder (a mount with a prefix drops "prefix/").
const char* AssetRegistry::rel_of(const AssetRecord& record) const {
    if (record.mount == 0 || record.mount > mounts_.size()) return record.path;
    const usize n = std::strlen(mounts_[record.mount - 1].prefix);
    return n ? record.path + n + 1 : record.path;
}

void AssetRegistry::source_path(const AssetRecord& record, char* out, usize capacity) const {
    std::snprintf(out, capacity, "%s/%s", dir_of(record.mount), rel_of(record));
}

void AssetRegistry::meta_path(const AssetRecord& record, char* out, usize capacity) const {
    std::snprintf(out, capacity, "%s/%s.meta", dir_of(record.mount), rel_of(record));
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
        u8 mount = 0;
    };
    bool ok = false;  // false when Content/ is missing: then nothing is dropped
    std::vector<File> files;
    std::vector<std::string> folders;  // every folder, '/' separators, A-Z (mounts under their prefix)
    u64 hash = 0;                      // of everything above: an unchanged walk is not applied again
};

struct AssetRegistry::Async {
    std::thread thread;
    std::atomic<bool> done{false};
    u32 generation = 0;  // sync_scans_ when the walk started
    Walk walk;
};

namespace {

bool is_meta_extension(const std::string& extension) {
    if (extension.size() != 5) return false;
    for (usize i = 0; i < 5; ++i) {
        if (std::tolower(static_cast<unsigned char>(extension[i])) != ".meta"[i]) return false;
    }
    return true;
}

// Only touches the file system and its own result, so it is safe on a worker thread. One directory listing gives each
// file's size and time and its .meta's time (no call per file: with 50,000 assets those calls were most of a scan).
void walk_folder(const char* dir, const std::string& prefix, u8 mount, AssetRegistry::Walk& out) {
    std::error_code error;
    const fs::path root(dir);
    std::unordered_map<std::string, i64> meta_times;  // "<file>" -> its .meta's time
    const usize first = out.files.size();
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(dir, error)) {
        const std::string inside = entry.path().lexically_relative(root).generic_string();
        const std::string relative = prefix.empty() ? inside : prefix + "/" + inside;
        if (entry.is_directory(error)) {
            out.folders.push_back(relative);
            continue;
        }
        if (!entry.is_regular_file(error)) continue;
        const std::string extension = entry.path().extension().string();
        const i64 time = static_cast<i64>(entry.last_write_time(error).time_since_epoch().count());
        if (is_meta_extension(extension)) {
            meta_times[relative.substr(0, relative.size() - 5)] = time;
            continue;
        }
        const AssetType type = type_from_extension(extension.c_str());
        if (type == AssetType::Unknown) continue;
        out.files.push_back({relative, type, static_cast<u64>(entry.file_size(error)), time, 0, mount});
    }
    for (usize i = first; i < out.files.size(); ++i) {
        const auto it = meta_times.find(out.files[i].relative);
        out.files[i].meta_time = it != meta_times.end() ? it->second : 0;
    }
}

// The content root, then each mount; a path found twice keeps the later one (a mount overrides).
void walk_content(const char* content_dir, const std::vector<AssetMount>& mounts, AssetRegistry::Walk& out) {
    std::error_code error;
    if (!fs::is_directory(content_dir, error)) return;
    out.ok = true;
    walk_folder(content_dir, "", 0, out);
    for (usize i = 0; i < mounts.size(); ++i) {
        if (fs::is_directory(mounts[i].dir, error)) walk_folder(mounts[i].dir, mounts[i].prefix, static_cast<u8>(i + 1), out);
    }
    std::sort(out.folders.begin(), out.folders.end());
    u64 hash = 1469598103934665603ull;  // FNV-1a over what apply() reads
    auto mix = [&hash](const void* p, usize n) {
        for (usize i = 0; i < n; ++i) hash = (hash ^ static_cast<const u8*>(p)[i]) * 1099511628211ull;
    };
    for (const AssetRegistry::Walk::File& f : out.files) {
        mix(f.relative.data(), f.relative.size() + 1);
        mix(&f.size, sizeof f.size), mix(&f.time, sizeof f.time), mix(&f.meta_time, sizeof f.meta_time), mix(&f.mount, 1);
    }
    for (const std::string& folder : out.folders) mix(folder.data(), folder.size() + 1);
    out.hash = hash;
    if (mounts.empty()) return;
    std::unordered_map<std::string, usize> at;
    std::vector<AssetRegistry::Walk::File> unique;
    for (AssetRegistry::Walk::File& f : out.files) {
        const auto it = at.find(f.relative);
        if (it != at.end()) {
            unique[it->second] = f;
            continue;
        }
        at.emplace(f.relative, unique.size());
        unique.push_back(f);
    }
    out.files.swap(unique);
}

} // namespace

AssetRegistry::~AssetRegistry() {
    if (async_ && async_->thread.joinable()) async_->thread.join();
    delete async_;
}

bool AssetRegistry::scan() {
    ++sync_scans_;
    Walk walk;
    walk_content(content_dir_, mounts_, walk);
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
    async_->thread = std::thread([a = async_, dir = std::string(content_dir_), mounts = mounts_] {
        walk_content(dir.c_str(), mounts, a->walk);
        a->done.store(true, std::memory_order_release);
    });
    return changed;
}

bool AssetRegistry::apply(const Walk& walk) {
    if (!walk.ok) return false;
    if (walk.hash == applied_hash_ && !records_.empty()) return false;  // nothing on disk changed since the last walk
    applied_hash_ = walk.hash;
    bool changed = walk.folders != folders_;
    if (changed) folders_ = walk.folders;

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
    if (changed) index_guids();  // removals moved records around
    for (usize i = 0; i < records_.size(); ++i) known.emplace(records_[i].path, i);

    // New files' .meta (and data assets' types) read on worker threads first: a project opened for the first time,
    // or with thousands of assets copied in, has thousands of them. The GUID checks and writes below stay in order.
    struct Fresh {
        bool had_meta = false;
        struct {
            char guid[33] = "";
            char tags[96] = "";
        } meta;
        char data_type[32] = "";
        bool data_ok = true;
    };
    std::vector<const Walk::File*> fresh_files;
    for (const Walk::File& f : walk.files) {
        if (!known.count(f.relative)) fresh_files.push_back(&f);
    }
    std::vector<Fresh> fresh(fresh_files.size());
    {
        auto read = [&](usize from, usize step) {
            AssetMeta meta;
            for (usize k = from; k < fresh_files.size(); k += step) {
                AssetRecord probe;
                probe.mount = fresh_files[k]->mount;
                copy_string(probe.path, sizeof probe.path, fresh_files[k]->relative.c_str());
                meta = AssetMeta{};
                fresh[k].had_meta = load_meta(probe, meta);
                copy_string(fresh[k].meta.guid, sizeof fresh[k].meta.guid, meta.guid);
                copy_string(fresh[k].meta.tags, sizeof fresh[k].meta.tags, meta.tags);
                if (fresh_files[k]->type == AssetType::Data) {
                    char full[300];
                    source_path(probe, full, sizeof full);
                    fresh[k].data_ok = read_data_asset_type(full, fresh[k].data_type, sizeof fresh[k].data_type);
                }
            }
        };
        const u32 cores = std::thread::hardware_concurrency();
        const usize threads = fresh_files.size() < 64 ? 1 : static_cast<usize>(cores > 1 ? cores : 1);
        std::vector<std::thread> pool;
        for (usize t = 1; t < threads; ++t) pool.emplace_back(read, t, threads);
        read(0, threads);
        for (std::thread& t : pool) t.join();
    }
    usize next_fresh = 0;
    std::vector<usize> new_metas;  // records_ indices whose .meta is still to write

    for (const Walk::File& f : walk.files) {
        const auto it = known.find(f.relative);
        AssetRecord probe;  // only to ask source_path where this file lives
        probe.mount = f.mount;
        copy_string(probe.path, sizeof probe.path, f.relative.c_str());
        char full[300];
        source_path(probe, full, sizeof full);
        if (it != known.end()) {
            AssetRecord& existing = records_[it->second];
            if (existing.mount != f.mount) {  // a mount now provides (or no longer provides) this path
                existing.mount = f.mount;
                existing.meta_time = 0;
                changed = true;
            }
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

        const Fresh& read = fresh[next_fresh++];  // the same order as fresh_files
        AssetRecord record;
        record.type = f.type;
        record.source_size = f.size;
        record.source_time = f.time;
        record.meta_time = f.meta_time;
        record.mount = f.mount;
        copy_string(record.path, sizeof record.path, f.relative.c_str());
        const usize slash = f.relative.rfind('/');
        copy_string(record.folder, sizeof record.folder, slash == std::string::npos ? "" : f.relative.substr(0, slash).c_str());
        const std::string file = slash == std::string::npos ? f.relative : f.relative.substr(slash + 1);
        copy_string(record.name, sizeof record.name, file.substr(0, file.rfind('.')).c_str());
        copy_string(record.data_type, sizeof record.data_type, read.data_type);
        if (!read.data_ok) LSD_LOG_WARN("asset", "%s is not a valid data asset", f.relative.c_str());

        // Read the GUID from the .meta; create one if missing, invalid, or already taken
        // (a file copied together with its .meta must become a new asset).
        const bool had_meta = read.had_meta;
        const bool guid_ok = had_meta && parse_guid(read.meta.guid, record.guid) && !find(record.guid);
        if (!had_meta) {  // a new file: its .meta is written below, on worker threads with the others
            record.guid = new_guid();
            new_metas.push_back(records_.size());
        } else if (!guid_ok) {
            AssetMeta meta;  // a fresh .meta (one that was not valid is replaced whole, as before)
            if (had_meta) load_meta(record, meta);
            record.guid = new_guid();
            to_string(record.guid, meta.guid);
            copy_string(meta.type, sizeof meta.type, to_string(f.type));
            char path[300];
            meta_path(record, path, sizeof path);
            if (!reflect::save_file(meta, path)) LSD_LOG_WARN("asset", "Could not write %s.meta", record.path);
            else record.meta_time = write_time(path);
            if (had_meta) LSD_LOG_INFO("asset", "%s had a duplicate or invalid GUID; assigned a new one.", record.path);
        }
        copy_string(record.tags, sizeof record.tags, read.meta.tags);
        known.emplace(f.relative, records_.size());
        by_guid_.emplace(record.guid, static_cast<u32>(records_.size()));
        records_.push_back(record);
        changed = true;
    }
    // Fresh .meta files (a GUID and the type), many at once.
    auto write = [&](usize from, usize step) {
        AssetMeta meta;
        for (usize k = from; k < new_metas.size(); k += step) {
            AssetRecord& r = records_[new_metas[k]];
            meta = AssetMeta{};
            to_string(r.guid, meta.guid);
            copy_string(meta.type, sizeof meta.type, to_string(r.type));
            char path[300];
            meta_path(r, path, sizeof path);
            if (!reflect::save_file(meta, path)) LSD_LOG_WARN("asset", "Could not write %s.meta", r.path);
            else r.meta_time = write_time(path);
        }
    };
    {
        const u32 cores = std::thread::hardware_concurrency();
        const usize threads = new_metas.size() < 64 ? 1 : static_cast<usize>(cores > 1 ? cores : 1);
        std::vector<std::thread> pool;
        for (usize t = 1; t < threads; ++t) pool.emplace_back(write, t, threads);
        write(0, threads);
        for (std::thread& t : pool) t.join();
    }
    if (changed) ++generation_;
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

namespace {

bool search_matches(const AssetRecord& r, const char* text, AssetType type, const char* folder) {
    if (type != AssetType::Unknown && r.type != type) return false;
    if (text && *text) return contains_ignore_case(r.name, text) || contains_ignore_case(r.tags, text);
    return !folder || std::strcmp(r.folder, folder) == 0;
}

} // namespace

usize AssetRegistry::search(const char* text, AssetType type, const char* folder, usize* out, usize max) const {
    usize found = 0;
    for (usize i = 0; i < records_.size() && found < max; ++i) {
        if (search_matches(records_[i], text, type, folder)) out[found++] = i;
    }
    return found;
}

void AssetRegistry::search(const char* text, AssetType type, const char* folder, std::vector<usize>& out) const {
    out.clear();
    for (usize i = 0; i < records_.size(); ++i) {
        if (search_matches(records_[i], text, type, folder)) out.push_back(i);
    }
}

} // namespace lsd::asset
