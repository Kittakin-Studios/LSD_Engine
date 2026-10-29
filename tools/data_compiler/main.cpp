// LSD_Engine data compiler: turns editable source assets into runtime-ready binary data.
// Scans the source (Content) directory, gives new files a .meta, and compiles every asset whose
// output is missing or older than its source or import settings. The editor runs the same step.
#include "lsd/asset/asset_registry.h"
#include "lsd/asset/mesh_import.h"
#include "lsd/foundation/log.h"

#include <cstring>

int main(int argc, char** argv) {
    const char* source_dir = nullptr;
    const char* data_dir = nullptr;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--source-dir") == 0) source_dir = argv[++i];
        else if (std::strcmp(argv[i], "--data-dir") == 0) data_dir = argv[++i];
    }
    if (!source_dir || !data_dir) {
        LSD_LOG_ERROR("compiler", "usage: lsd_data_compiler --source-dir <dir> --data-dir <dir>");
        return 1;
    }

    lsd::Allocator& allocator = lsd::default_allocator();
    lsd::asset::AssetRegistry registry(allocator, source_dir);
    registry.scan();
    LSD_LOG_INFO("compiler", "%zu assets in %s", registry.count(), source_dir);

    int failed = 0;
    for (lsd::usize i = 0; i < registry.count(); ++i) {
        const lsd::asset::AssetRecord& record = registry.get(i);
        char compiled[300];
        if (record.type == lsd::asset::AssetType::Mesh &&
            !lsd::asset::compile_mesh_asset(registry, record, data_dir, allocator, compiled, sizeof compiled)) {
            ++failed;
        }
    }
    if (failed) LSD_LOG_ERROR("compiler", "%d asset(s) failed to compile.", failed);
    else LSD_LOG_INFO("compiler", "All assets up to date in %s", data_dir);
    return failed ? 1 : 0;
}
