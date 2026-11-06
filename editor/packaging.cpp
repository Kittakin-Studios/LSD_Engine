#include "packaging.h"
#include "references.h"

#include "lsd/asset/asset_registry.h"
#include "lsd/asset/texture.h"
#include "lsd/foundation/log.h"
#include "lsd/reflect/reflect.h"

#include <SDL3/SDL_filesystem.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

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

bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

// The name prefix Unreal's naming conventions expect for an asset ("" = none expected).
const char* expected_prefix(const asset::AssetRecord& r) {
    switch (r.type) {
        case asset::AssetType::Texture: return "T_";
        case asset::AssetType::MaterialGraph: return "M_";
        case asset::AssetType::BehaviorTree: return "BT_";
        case asset::AssetType::UiDocument: return "UI_";
        case asset::AssetType::Cue: return "SC_";
        case asset::AssetType::Sound: return "S_";
        case asset::AssetType::Prefab: return "BP_";
        case asset::AssetType::Data:
            if (std::strcmp(r.data_type, "Material") == 0) return "M_";
            if (std::strcmp(r.data_type, "MaterialInstance") == 0) return "MI_";
            if (std::strcmp(r.data_type, "ParticleSystem") == 0) return "PS_";
            if (std::strcmp(r.data_type, "InputAction") == 0) return "IA_";
            if (std::strcmp(r.data_type, "InputMappingContext") == 0) return "IMC_";
            return "";
        default: return "";
    }
}

} // namespace

u32 validate_assets(const ProjectSettings& project, const asset::AssetRegistry& registry) {
    u32 problems = 0;
    for (usize i = 0; i < registry.count(); ++i) {
        const asset::AssetRecord& r = registry.get(i);
        char path[300];
        registry.source_path(r, path, sizeof path);
        // References: every quoted 32-hex-digit GUID in a text asset must name an asset.
        const bool text = r.type == asset::AssetType::Level || r.type == asset::AssetType::Data || r.type == asset::AssetType::Prefab ||
                          r.type == asset::AssetType::Graph || r.type == asset::AssetType::Cue || r.type == asset::AssetType::MaterialGraph;
        if (text) {
            FILE* f = std::fopen(path, "rb");
            std::string content;
            if (f) {
                char buffer[4096];
                for (usize n; (n = std::fread(buffer, 1, sizeof buffer, f)) > 0;) content.append(buffer, n);
                std::fclose(f);
            }
            for (usize k = 0; k + 33 < content.size(); ++k) {
                if (content[k] != '"' || (content[k + 33] != '"' && content[k + 33] != '#')) continue;  // '#': a part of a mesh file
                bool guid_like = true;
                for (usize h = 1; h <= 32 && guid_like; ++h) guid_like = is_hex(content[k + h]);
                if (!guid_like) continue;
                asset::Guid guid;
                const std::string text_guid = content.substr(k + 1, 32);
                if (asset::parse_guid(text_guid.c_str(), guid) && !registry.find(guid)) {
                    LSD_LOG_WARN("validate", "%s refers to a missing asset (%s)", r.path, text_guid.c_str());
                    ++problems;
                }
                k += 33;
            }
        }
        if (r.type == asset::AssetType::Texture) {
            u32 w = 0, h = 0;
            if (asset::texture_size(path, w, h) && (w > project.max_texture_size || h > project.max_texture_size)) {
                LSD_LOG_WARN("validate", "%s is %u x %u (over %u): set Max Size in its Details or scale it down", r.path, w, h, project.max_texture_size);
                ++problems;
            }
        }
        if (project.check_naming) {
            const char* prefix = expected_prefix(r);
            if (prefix[0] && std::strncmp(r.name, prefix, std::strlen(prefix)) != 0) {
                LSD_LOG_WARN("validate", "%s: name should start with %s", r.path, prefix);
                ++problems;
            }
        }
    }
    LSD_LOG_INFO("validate", "Asset validation: %u problem%s in %zu assets", problems, problems == 1 ? "" : "s", registry.count());
    return problems;
}

// Cooking: only the assets the game uses (cook_set), each with its .meta and its compiled data
// (DerivedData/<guid>.*), Content/Fonts whole (UI fonts are found by folder), and cook_report.txt.
static bool cook(const ProjectSettings& project, const asset::AssetRegistry& registry, const fs::path& out, std::error_code& error) {
    static char settings[65536];  // the project settings as text: the roots
    const usize n = reflect::save_text(reflect::type_of<ProjectSettings>(), &project, settings, sizeof settings);
    const std::vector<std::string> used = cook_set(registry, std::string(settings, n < sizeof settings ? n : sizeof settings - 1));
    std::vector<bool> keep(registry.count(), false);
    u64 kept_bytes = 0, all_bytes = 0;
    for (usize i = 0; i < registry.count(); ++i) {
        const asset::AssetRecord& r = registry.get(i);
        char guid[33];
        asset::to_string(r.guid, guid);
        char source[300];
        registry.source_path(r, source, sizeof source);
        std::error_code ignored;
        const u64 size = fs::file_size(source, ignored);
        all_bytes += ignored ? 0 : size;
        if (std::find(used.begin(), used.end(), std::string(guid)) == used.end()) continue;
        keep[i] = true;
        kept_bytes += ignored ? 0 : size;
        const fs::path to = out / "Content" / r.path;
        fs::create_directories(to.parent_path(), error);
        fs::copy_file(source, to, fs::copy_options::overwrite_existing, error);
        if (!error && fs::exists(std::string(source) + ".meta", ignored)) fs::copy_file(std::string(source) + ".meta", to.string() + ".meta", fs::copy_options::overwrite_existing, error);
        if (error) return false;
        for (const fs::directory_entry& d : fs::directory_iterator("DerivedData", ignored)) {  // compiled already: the game does not redo it
            if (d.path().filename().string().rfind(guid, 0) != 0) continue;
            fs::create_directories(out / "DerivedData", ignored);
            fs::copy_file(d.path(), out / "DerivedData" / d.path().filename(), fs::copy_options::overwrite_existing, ignored);
        }
    }
    std::error_code ignored;
    if (fs::exists("Content/Fonts", ignored)) fs::copy("Content/Fonts", out / "Content" / "Fonts", fs::copy_options::recursive | fs::copy_options::overwrite_existing, error);
    if (FILE* report = std::fopen((out / "cook_report.txt").generic_string().c_str(), "wb")) {
        std::fprintf(report, "Cooked %zu of %zu assets (%.1f of %.1f MB). Left out (nothing names them):\n", used.size(), registry.count(),
                     static_cast<double>(kept_bytes) / 1048576.0, static_cast<double>(all_bytes) / 1048576.0);
        for (usize i = 0; i < registry.count(); ++i) {
            if (!keep[i]) std::fprintf(report, "  %s\n", registry.get(i).path);
        }
        std::fclose(report);
    }
    LSD_LOG_INFO("editor", "Cooked %zu of %zu assets (%.1f of %.1f MB); the rest is listed in cook_report.txt", used.size(), registry.count(),
                 static_cast<double>(kept_bytes) / 1048576.0, static_cast<double>(all_bytes) / 1048576.0);
    return !error;
}

bool package_project(const ProjectSettings& project, const asset::AssetRegistry& registry, char* message, usize capacity) {
    std::error_code error;
    const u32 problems = validate_assets(project, registry);
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
    if (project.cook_used_only) {
        if (!cook(project, registry, out, error)) {
            std::snprintf(message, capacity, "Cooking into %s failed: %s", out.generic_string().c_str(), error.message().c_str());
            return false;
        }
    } else if (!error) {
        fs::copy("Content", out / "Content", copy_options, error);
    }
    if (!error) fs::copy("Config", out / "Config", copy_options, error);
    if (!error && fs::exists("Binaries", error)) fs::copy("Binaries", out / "Binaries", copy_options, error);  // the C / C++ game code
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
    if (project.steam_app_id) {  // Steamworks reads it when the game is started outside Steam
        if (FILE* id = std::fopen((out / "steam_appid.txt").generic_string().c_str(), "wb")) {
            std::fprintf(id, "%u\n", project.steam_app_id);
            std::fclose(id);
        }
    }
    if (FILE* marker = std::fopen((out / kGameMarker).generic_string().c_str(), "wb")) {
        std::fprintf(marker, "{\n  \"packaged\": true\n}\n");
        std::fclose(marker);
    }
    const bool stale = release && fs::exists(here / "lsd_editor.exe", error) && fs::last_write_time(exe, error) < fs::last_write_time(here / "lsd_editor.exe", error);
    char validated[64] = "";
    if (problems) std::snprintf(validated, sizeof validated, " (%u asset problem%s: see the Message Log)", problems, problems == 1 ? "" : "s");
    std::snprintf(message, capacity, "Packaged %s%s%s", out.generic_string().c_str(), validated,
                  !release ? " (Debug build: other PCs need Visual Studio; build Release first: cmake --build --preset windows-msvc --config Release)"
                  : stale  ? " (the Release build is older than this editor: rebuild it for the latest engine)"
                           : "");
    LSD_LOG_INFO("editor", "%s", message);
    return true;
}

} // namespace lsd::editor
