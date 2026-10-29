#include "lsd/asset/asset_registry.h"
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
        case AssetType::Unknown: break;
    }
    return "Unknown";
}

AssetType type_from_extension(const char* extension) {
    char lower[16] = "";
    for (usize i = 0; i + 1 < sizeof lower && extension[i]; ++i) lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(extension[i])));
    if (std::strcmp(lower, ".fbx") == 0 || std::strcmp(lower, ".gltf") == 0 || std::strcmp(lower, ".glb") == 0) return AssetType::Mesh;
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
