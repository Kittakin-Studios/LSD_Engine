#include "lsd/asset/asset_registry.h"
#include "lsd/asset/data_asset.h"
#include "lsd/foundation/log.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>

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

bool AssetRegistry::save_meta(const AssetRecord& record, const AssetMeta& meta) const {
    char path[300];
    meta_path(record, path, sizeof path);
    return reflect::save_file(meta, path);
}

bool AssetRegistry::scan() {
    std::error_code error;
    if (!fs::is_directory(content_dir_, error)) return false;
    bool changed = false;

    // Drop records whose source file is gone.
    for (usize i = records_.size(); i-- > 0;) {
        char path[300];
        source_path(records_[i], path, sizeof path);
        if (!fs::exists(path, error)) {
            records_.remove_swap(i);
            changed = true;
        }
    }

    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(content_dir_, error)) {
        if (!entry.is_regular_file(error)) continue;
        const AssetType type = type_from_extension(entry.path().extension().string().c_str());
        if (type == AssetType::Unknown) continue;

        const std::string relative = fs::relative(entry.path(), content_dir_, error).generic_string();
        const u64 size = static_cast<u64>(entry.file_size(error));
        const i64 time = write_time(entry.path());

        AssetRecord* existing = nullptr;
        for (AssetRecord& r : records_) {
            if (std::strcmp(r.path, relative.c_str()) == 0) existing = &r;
        }
        if (existing) {
            if (existing->source_size != size || existing->source_time != time) {
                existing->source_size = size;
                existing->source_time = time;
                if (type == AssetType::Data) read_data_asset_type(entry.path().string().c_str(), existing->data_type, sizeof existing->data_type);
                changed = true;
            }
            continue;
        }

        AssetRecord record;
        record.type = type;
        record.source_size = size;
        record.source_time = time;
        copy_string(record.path, sizeof record.path, relative.c_str());
        copy_string(record.folder, sizeof record.folder, fs::path(relative).parent_path().generic_string().c_str());
        copy_string(record.name, sizeof record.name, entry.path().stem().string().c_str());
        if (type == AssetType::Data && !read_data_asset_type(entry.path().string().c_str(), record.data_type, sizeof record.data_type)) {
            LSD_LOG_WARN("asset", "%s is not a valid data asset", relative.c_str());
        }

        // Read the GUID from the .meta; create one if missing, invalid, or already taken
        // (a file copied together with its .meta must become a new asset).
        AssetMeta meta;
        const bool had_meta = load_meta(record, meta);
        const bool guid_ok = had_meta && parse_guid(meta.guid, record.guid) && !find(record.guid);
        if (!guid_ok) {
            record.guid = new_guid();
            to_string(record.guid, meta.guid);
            copy_string(meta.type, sizeof meta.type, to_string(type));
            if (!save_meta(record, meta)) LSD_LOG_WARN("asset", "Could not write %s.meta", record.path);
            if (had_meta) LSD_LOG_INFO("asset", "%s had a duplicate or invalid GUID; assigned a new one.", record.path);
        }
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
            if (!contains_ignore_case(r.name, text)) continue;
        } else if (folder && std::strcmp(r.folder, folder) != 0) {
            continue;
        }
        out[found++] = i;
    }
    return found;
}

} // namespace lsd::asset
