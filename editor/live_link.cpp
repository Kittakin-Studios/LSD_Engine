#include "live_link.h"

#include "lsd/asset/asset_registry.h"
#include "lsd/foundation/log.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace lsd::editor {

unsigned live_link_sync(const char* watched, const char* content_dir, char* status, unsigned long long capacity) {
    namespace fs = std::filesystem;
    if (!watched || !watched[0]) return 0;
    std::error_code error;
    const fs::path source(watched);
    if (!fs::is_directory(source, error)) return 0;
    const fs::path target = fs::path(content_dir) / "LiveLink";
    unsigned copied = 0;
    std::string last;
    for (auto it = fs::recursive_directory_iterator(source, error); !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        if (!it->is_regular_file(error)) continue;
        const fs::path& file = it->path();
        if (asset::type_from_extension(file.extension().string().c_str()) == asset::AssetType::Unknown && file.extension() != ".bin") continue;
        const fs::path dest = target / fs::relative(file, source, error);
        std::error_code missing;
        const auto from_time = fs::last_write_time(file, error);
        const auto to_time = fs::last_write_time(dest, missing);
        if (!missing && to_time >= from_time) continue;  // up to date
        fs::create_directories(dest.parent_path(), error);
        if (fs::copy_file(file, dest, fs::copy_options::overwrite_existing, error)) {
            ++copied;
            last = dest.generic_string();
        }
    }
    if (copied) {
        if (copied == 1) std::snprintf(status, static_cast<size_t>(capacity), "Live Link: %s updated", last.c_str());
        else std::snprintf(status, static_cast<size_t>(capacity), "Live Link: %u files updated", copied);
        LSD_LOG_INFO("livelink", "%s", status);
    }
    return copied;
}

} // namespace lsd::editor
