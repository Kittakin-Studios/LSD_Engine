#include "packaging.h"

#include "lsd/foundation/log.h"

#include <SDL3/SDL_filesystem.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace lsd::editor {

namespace fs = std::filesystem;

namespace {

// A file-name-safe version of the project name ("My Game!" -> "My Game").
std::string folder_name(const char* name) {
    std::string out;
    for (const char* c = name; *c; ++c) {
        if (std::strchr("<>:\"/\\|?*", *c) || static_cast<unsigned char>(*c) < 32) continue;
        out += *c;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out.empty() ? std::string("Game") : out;
}

} // namespace

bool package_project(const ProjectSettings& project, char* message, usize capacity) {
    std::error_code error;
    if (!project.game_default_level[0] || !fs::exists(project.game_default_level, error)) {
        std::snprintf(message, capacity, "Set Edit > Project Settings > Maps > Game Default Level to a saved level first");
        return false;
    }
    // The engine: the Release build next to this one if there is one (it runs without Visual Studio).
    const char* base = SDL_GetBasePath();
    const fs::path here = base ? fs::path(base) : fs::current_path(error);
    fs::path exe = here.parent_path().parent_path() / "Release" / "lsd_editor.exe";
    const bool release = fs::exists(exe, error);
    if (!release) exe = here / "lsd_editor.exe";
    if (!fs::exists(exe, error)) {
        std::snprintf(message, capacity, "Could not find the engine executable (%s)", exe.generic_string().c_str());
        return false;
    }

    const std::string name = folder_name(project.project_name);
    const fs::path out = fs::path("Build") / name;
    // Only a folder we made is cleared (it has the marker), never anything else.
    if (fs::exists(out, error)) {
        if (!fs::exists(out / kGameMarker, error)) {
            std::snprintf(message, capacity, "%s exists and is not a package: move it away first", out.generic_string().c_str());
            return false;
        }
        fs::remove_all(out, error);
    }
    fs::create_directories(out, error);
    const auto copy_options = fs::copy_options::recursive | fs::copy_options::overwrite_existing;
    fs::copy_file(exe, out / (name + ".exe"), fs::copy_options::overwrite_existing, error);
    if (!error) fs::copy("Content", out / "Content", copy_options, error);
    if (!error) fs::copy("Config", out / "Config", copy_options, error);
    if (error) {
        std::snprintf(message, capacity, "Copying into %s failed: %s", out.generic_string().c_str(), error.message().c_str());
        return false;
    }
#ifdef _WIN32
    // The Visual C++ runtime beside the exe (app-local deployment), for PCs without the redistributable.
    if (release) {
        const char* windows = std::getenv("SystemRoot");
        for (const char* dll : {"vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"}) {
            const fs::path from = fs::path(windows ? windows : "C:/Windows") / "System32" / dll;
            std::error_code ignored;
            if (fs::exists(from, ignored)) fs::copy_file(from, out / dll, fs::copy_options::overwrite_existing, ignored);
        }
    }
#endif
    if (FILE* marker = std::fopen((out / kGameMarker).generic_string().c_str(), "wb")) {
        std::fprintf(marker, "{\n  \"packaged\": true\n}\n");
        std::fclose(marker);
    }
    const bool stale = release && fs::exists(here / "lsd_editor.exe", error) && fs::last_write_time(exe, error) < fs::last_write_time(here / "lsd_editor.exe", error);
    std::snprintf(message, capacity, "Packaged %s%s", out.generic_string().c_str(),
                  !release ? " (Debug build: other PCs need Visual Studio; build Release first: cmake --build --preset windows-msvc --config Release)"
                  : stale  ? " (the Release build is older than this editor: rebuild it for the latest engine)"
                           : "");
    LSD_LOG_INFO("editor", "%s", message);
    return true;
}

} // namespace lsd::editor
