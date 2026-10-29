// LSD_Engine data compiler: turns editable source assets into runtime-ready binary data.
// Milestone 2 fills this in (see docs/ROADMAP.md). For now it only parses its command line.
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
    LSD_LOG_INFO("compiler", "Source: %s", source_dir);
    LSD_LOG_INFO("compiler", "Output: %s", data_dir);
    LSD_LOG_INFO("compiler", "Nothing to compile yet (milestone 2).");
    return 0;
}
