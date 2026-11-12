#include "packaging.h"
#include "references.h"

#include "lsd/asset/asset_registry.h"
#include "lsd/asset/plugins.h"
#include "lsd/asset/texture.h"
#include "lsd/foundation/log.h"
#include "lsd/reflect/reflect.h"
#include "lsd/rhi/shader_cache.h"
#include "lsd/world/level.h"
#include "lsd/world/world.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

// Resource packages per level (Bitsquid's resource packages): Packages/<level path>.package lists the assets each packaged
// level uses (its own cook set), one "guid path" a line; the game loads a level's meshes from it before the level plays.
static void write_level_packages(const asset::AssetRegistry& registry, const fs::path& out) {
    std::error_code ignored;
    u32 written = 0;
    for (usize i = 0; i < registry.count(); ++i) {
        const asset::AssetRecord& r = registry.get(i);
        if (r.type != asset::AssetType::Level || !fs::exists(out / "Content" / r.path, ignored)) continue;
        char guid[33];
        asset::to_string(r.guid, guid);
        const std::vector<std::string> used = cook_set(registry, std::string("\"") + guid + "\"");
        fs::path file = out / "Packages" / r.path;
        file.replace_extension(".package");
        fs::create_directories(file.parent_path(), ignored);
        if (FILE* f = std::fopen(file.generic_string().c_str(), "wb")) {
            for (const std::string& g : used) {
                asset::Guid id;
                const asset::AssetRecord* a = asset::parse_guid(g.c_str(), id) ? registry.find(id) : nullptr;
                if (a) std::fprintf(f, "%s %s\n", g.c_str(), a->path);
            }
            std::fclose(f);
            ++written;
        }
    }
    LSD_LOG_INFO("editor", "Resource packages: %u level%s (Packages/)", written, written == 1 ? "" : "s");
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
        if (r.type == asset::AssetType::Level) {  // the level's built data (baked lighting, HLOD proxies) sits beside it, outside the registry
            for (const char* ext : {".lsdlight", ".lsdhlod"}) {
                const fs::path built = fs::path(source).parent_path() / (fs::path(source).stem().string() + "_BuiltData" + ext);
                if (fs::exists(built, ignored)) fs::copy_file(built, to.parent_path() / built.filename(), fs::copy_options::overwrite_existing, ignored);
            }
        }
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

namespace {

// The source file of a texture asset by GUID text ("" if none).
std::string asset_file(const asset::AssetRegistry& registry, const char* guid_text) {
    asset::Guid guid;
    const asset::AssetRecord* r = guid_text[0] && asset::parse_guid(guid_text, guid) ? registry.find(guid) : nullptr;
    if (!r) return {};
    char path[300];
    registry.source_path(*r, path, sizeof path);
    return path;
}

#ifdef _WIN32
// A VS_VERSIONINFO block (the exe's Properties > Details): nodes of length, value length, type,
// UTF-16 key, padding to 4 bytes, value, children.
struct VersionNode {
    std::vector<u8> bytes;
    void pad() {
        while (bytes.size() % 4) bytes.push_back(0);
    }
    void u16(u32 v) {
        bytes.push_back(static_cast<u8>(v & 0xFF));
        bytes.push_back(static_cast<u8>((v >> 8) & 0xFF));
    }
    void wide(const char* text) {  // ASCII / Latin-1 as UTF-16, NUL-terminated
        for (const char* c = text; *c; ++c) u16(static_cast<u8>(*c));
        u16(0);
    }
    // Starts a node; finish() writes its length.
    usize begin(const char* key, u32 value_length, u32 type) {
        pad();
        const usize at = bytes.size();
        u16(0), u16(value_length), u16(type);
        wide(key);
        pad();
        return at;
    }
    void finish(usize at) {
        const usize length = bytes.size() - at;
        bytes[at] = static_cast<u8>(length & 0xFF);
        bytes[at + 1] = static_cast<u8>((length >> 8) & 0xFF);
    }
    void string(const char* key, const char* value) {
        const usize at = begin(key, static_cast<u32>(std::strlen(value) + 1), 1);
        wide(value);
        finish(at);
    }
};

#endif  // _WIN32 (Build Release Engine below is on every platform)

namespace {
// The build's thread; quitting while it runs lets it finish on its own (a std::thread still joinable at exit would abort).
struct ReleaseThread {
    std::thread thread;
    ~ReleaseThread() {
        if (thread.joinable()) thread.detach();
    }
} g_release;
std::thread& g_release_thread = g_release.thread;
std::mutex g_release_mutex;
std::string g_release_line;
std::atomic<bool> g_release_running{false}, g_release_finished{false}, g_release_ok{false};

#ifdef _WIN32
// The engine's source tree: the first folder above the running exe with CMakePresets.json and engine/.
bool engine_source(fs::path& out) {
    std::error_code error;
    for (fs::path p = fs::current_path(error); !p.empty(); p = p.parent_path()) {
        if (fs::exists(p / "CMakePresets.json", error) && fs::exists(p / "engine", error)) {
            out = p;
            return true;
        }
        if (p == p.parent_path()) break;
    }
    return false;
}
#endif
} // namespace

} // namespace (reopened below)

bool start_release_build(char* message, usize capacity) {
    if (g_release_running) {
        std::snprintf(message, capacity, "The Release build is already running");
        return false;
    }
#ifdef _WIN32
    fs::path source;
    if (!engine_source(source)) {
        std::snprintf(message, capacity, "No engine source tree above this folder (CMakePresets.json): build Release with CMake where the source is");
        return false;
    }
    // Visual Studio's environment through vswhere (the installer always puts it here), as a developer prompt would.
    std::string vs;
    const char* program_files = std::getenv("ProgramFiles(x86)");
    const std::string vswhere = std::string(program_files ? program_files : "C:\\Program Files (x86)") + "\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    // cmd /c drops the first and last quote of a line that starts with one: the whole line goes in one more pair.
    const std::string find_vs = "\"\"" + vswhere + "\" -latest -property installationPath\"";
    if (FILE* p = _popen(find_vs.c_str(), "r")) {
        char line[512];
        if (std::fgets(line, sizeof line, p)) vs = line;
        _pclose(p);
    }
    while (!vs.empty() && (vs.back() == '\n' || vs.back() == '\r')) vs.pop_back();
    const fs::path vcvars = fs::path(vs) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
    std::error_code error;
    if (vs.empty() || !fs::exists(vcvars, error)) {
        std::snprintf(message, capacity, "Visual Studio (with C++) was not found: install it, or build Release with CMake by hand");
        return false;
    }
    if (g_release_thread.joinable()) g_release_thread.join();
    g_release_running = true;
    g_release_finished = false;
    fs::create_directories("Saved/Logs", error);
    const std::string command = "cmd /s /c \"\"" + vcvars.string() + "\" >nul 2>nul && cd /d \"" + source.string() +
                                "\" && cmake --build --preset windows-msvc --config Release 2>&1\"";
    g_release_thread = std::thread([command] {
        std::FILE* log = std::fopen("Saved/Logs/ReleaseBuild.log", "wb");
        bool ok = false;
        if (FILE* p = _popen(command.c_str(), "r")) {
            char line[1024];
            while (std::fgets(line, sizeof line, p)) {
                if (log) std::fputs(line, log);
                std::lock_guard<std::mutex> lock(g_release_mutex);
                g_release_line = line;
                while (!g_release_line.empty() && (g_release_line.back() == '\n' || g_release_line.back() == '\r')) g_release_line.pop_back();
            }
            ok = _pclose(p) == 0;
        }
        if (log) std::fclose(log);
        g_release_ok = ok;
        g_release_finished = true;
        g_release_running = false;
    });
    std::snprintf(message, capacity, "Building the Release engine in %s (Saved/Logs/ReleaseBuild.log)", source.generic_string().c_str());
    return true;
#else
    std::snprintf(message, capacity, "Build Release with CMake (cmake --build --preset linux-clang --config Release)");
    return false;
#endif
}

bool release_build_running(char* line, usize capacity, bool& finished, bool& ok) {
    finished = g_release_finished.exchange(false);
    ok = g_release_ok;
    if (finished && g_release_thread.joinable()) g_release_thread.join();
    std::lock_guard<std::mutex> lock(g_release_mutex);
    std::snprintf(line, capacity, "%s", g_release_line.c_str());
    return g_release_running;
}

namespace {
#ifdef _WIN32

// The project's icon and file details written into the packaged exe's resources.
void brand_exe(const ProjectSettings& project, const asset::AssetRegistry& registry, const fs::path& exe_path) {
    std::vector<u8> png;
    const std::string icon = asset_file(registry, project.icon);
    u32 icon_size = 0;
    if (!icon.empty()) {  // the icon as a PNG entry (Windows Vista and later read PNG icons), at most 256 px
        asset::TextureData pixels(default_allocator());
        const fs::path temp = exe_path.parent_path() / "icon_build.png";
        if (asset::load_texture(icon.c_str(), false, 256, pixels) && asset::write_png(temp.string().c_str(), pixels.width, pixels.height, pixels.mip(0))) {
            if (FILE* f = std::fopen(temp.string().c_str(), "rb")) {
                std::fseek(f, 0, SEEK_END);
                png.resize(static_cast<usize>(std::ftell(f)));
                std::fseek(f, 0, SEEK_SET);
                if (std::fread(png.data(), 1, png.size(), f) != png.size()) png.clear();
                std::fclose(f);
            }
            icon_size = pixels.width > pixels.height ? pixels.width : pixels.height;
        }
        std::error_code ignored;
        fs::remove(temp, ignored);
    }
    HANDLE update = BeginUpdateResourceW(exe_path.wstring().c_str(), FALSE);
    if (!update) {
        LSD_LOG_WARN("editor", "Could not write the icon / file details into %s", exe_path.generic_string().c_str());
        return;
    }
    const WORD language = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
    if (!png.empty()) {
#pragma pack(push, 2)
        struct GroupIcon {
            WORD reserved = 0, type = 1, count = 1;
            BYTE width, height, colors = 0, zero = 0;
            WORD planes = 1, bits = 32;
            DWORD bytes;
            WORD id = 1;
        } group;
#pragma pack(pop)
        group.width = group.height = static_cast<BYTE>(icon_size >= 256 ? 0 : icon_size);  // 0 means 256
        group.bytes = static_cast<DWORD>(png.size());
        UpdateResourceW(update, MAKEINTRESOURCEW(3) /* RT_ICON */, MAKEINTRESOURCEW(1), language, png.data(), static_cast<DWORD>(png.size()));
        UpdateResourceW(update, MAKEINTRESOURCEW(14) /* RT_GROUP_ICON */, MAKEINTRESOURCEW(1), language, &group, sizeof group);
    }
    VersionNode v;
    VS_FIXEDFILEINFO fixed{};
    fixed.dwSignature = 0xFEEF04BD;
    fixed.dwStrucVersion = 0x00010000;
    fixed.dwFileVersionMS = fixed.dwProductVersionMS = 0x00010000;  // 1.0
    fixed.dwFileFlagsMask = 0x3F;
    fixed.dwFileOS = VOS_NT_WINDOWS32;
    fixed.dwFileType = VFT_APP;
    const usize root_node = v.begin("VS_VERSION_INFO", sizeof fixed, 0);
    v.bytes.insert(v.bytes.end(), reinterpret_cast<const u8*>(&fixed), reinterpret_cast<const u8*>(&fixed) + sizeof fixed);
    const usize file_info = v.begin("StringFileInfo", 0, 1);
    const usize table = v.begin("040904b0", 0, 1);  // US English, Unicode
    char exe_name[96];
    std::snprintf(exe_name, sizeof exe_name, "%s", exe_path.filename().string().c_str());
    v.string("CompanyName", project.company);
    v.string("FileDescription", project.project_name);
    v.string("FileVersion", "1.0");
    v.string("ProductName", project.project_name);
    v.string("ProductVersion", "1.0");
    v.string("OriginalFilename", exe_name);
    v.finish(table);
    v.finish(file_info);
    const usize var_info = v.begin("VarFileInfo", 0, 1);
    const usize translation = v.begin("Translation", 4, 0);
    v.u16(0x0409), v.u16(0x04B0);
    v.finish(translation);
    v.finish(var_info);
    v.finish(root_node);
    UpdateResourceW(update, MAKEINTRESOURCEW(16) /* RT_VERSION */, MAKEINTRESOURCEW(1), MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), v.bytes.data(),
                    static_cast<DWORD>(v.bytes.size()));
    if (!EndUpdateResourceW(update, FALSE)) LSD_LOG_WARN("editor", "Could not write the icon / file details into %s", exe_path.generic_string().c_str());
}
#endif

const char* const kSplashNames[] = {"splash.png", "splash.jpg", "splash.jpeg", "splash.tga", "splash.bmp"};

} // namespace

SDL_Window* open_splash() {
    std::error_code ignored;
    const char* file = nullptr;
    for (const char* name : kSplashNames) {
        if (!file && fs::exists(name, ignored)) file = name;
    }
    if (!file) return nullptr;
    asset::TextureData pixels(default_allocator());
    if (!asset::load_texture(file, false, 1280, pixels)) return nullptr;
    SDL_Window* window = SDL_CreateWindow("", static_cast<int>(pixels.width), static_cast<int>(pixels.height), SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_UTILITY);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    SDL_Surface* surface = renderer ? SDL_CreateSurfaceFrom(static_cast<int>(pixels.width), static_cast<int>(pixels.height), SDL_PIXELFORMAT_ABGR8888,
                                                            const_cast<u8*>(pixels.mip(0)), static_cast<int>(pixels.width * 4))
                                    : nullptr;
    SDL_Texture* texture = surface ? SDL_CreateTextureFromSurface(renderer, surface) : nullptr;
    if (surface) SDL_DestroySurface(surface);
    if (!texture) {
        if (window) SDL_DestroyWindow(window);
        return nullptr;
    }
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
    SDL_PumpEvents();  // let it appear before the long loads that follow
    LSD_LOG_INFO("editor", "Splash: %s (%u x %u)", file, pixels.width, pixels.height);
    return window;
}

void close_splash(SDL_Window* splash) {
    if (!splash) return;
    LSD_LOG_INFO("editor", "Splash closed after %.1f s", static_cast<double>(SDL_GetTicks()) / 1000.0);
    if (SDL_Renderer* renderer = SDL_GetRenderer(splash)) SDL_DestroyRenderer(renderer);  // and its texture
    SDL_DestroyWindow(splash);
}

// Editor-only data out of the package (Unreal's Developers folder and editor-only actors): Developers / EditorOnly
// folders, and objects with an EditorOnly component in the shipped levels.
// shortcut: prefabs keep theirs (Play removes them when placed); strip prefab files too when a project needs it.
static void strip_editor_only(const fs::path& content) {
    std::error_code ec;
    u32 folders = 0, objects = 0;
    std::vector<fs::path> drop, levels;
    for (auto it = fs::recursive_directory_iterator(content, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec) && (name == "Developers" || name == "EditorOnly")) {
            drop.push_back(it->path());
            it.disable_recursion_pending();
        } else if (it->path().extension() == world::kLevelExtension) {
            levels.push_back(it->path());
        }
    }
    for (const fs::path& d : drop) folders += fs::remove_all(d, ec) ? 1u : 0u;
    for (const fs::path& level : levels) {
        world::World w(default_allocator());
        if (!world::load_level_file(w, level.string().c_str())) continue;
        const u32 gone = world::destroy_editor_only(w);
        if (gone && world::save_level_file(w, level.string().c_str())) objects += gone;
    }
    if (folders || objects) LSD_LOG_INFO("editor", "Stripped editor-only data: %u folder(s), %u object(s) from the levels", folders, objects);
}

void mount_extras(asset::AssetRegistry& registry) {
    std::vector<asset::AssetMount> mounts = asset::plugin_mounts(asset::discover_plugins(kPluginsDir));
    for (const asset::AssetMount& m : asset::dlc_mounts(kDlcDir)) mounts.push_back(m);  // after the plugins: DLC overrides
    registry.set_mounts(mounts);
}

bool package_dlc(const asset::AssetRegistry& registry, const char* folder, const char* base_manifest, char* message, usize capacity) {
    const std::string source = folder ? folder : "";
    const bool patch = base_manifest && *base_manifest;
    asset::Manifest base;
    if (patch && !asset::load_manifest(base_manifest, base)) {
        std::snprintf(message, capacity, "Could not read the base manifest %s", base_manifest);
        return false;
    }
    if (source.empty() ? !patch : !asset::safe_relative_path(source.c_str())) {
        std::snprintf(message, capacity, "Pick a content folder or a plugin (Plugins/<Name>) to package, or a base manifest to patch against");
        return false;
    }
    std::string name = source.empty() ? std::string("Patch") : folder_name(fs::path(source).filename().string().c_str());
    if (patch && !source.empty()) name += "_Patch";
    const fs::path out = fs::path(kBuildDir) / "DLC" / name;
    std::error_code error;
    if (fs::exists(out, error) && !fs::exists(out / kManifestFile, error)) {
        std::snprintf(message, capacity, "%s exists and is not a DLC package: move it away first", out.generic_string().c_str());
        return false;
    }
    fs::remove_all(out, error);
    u32 copied = 0;
    for (usize i = 0; i < registry.count() && !error; ++i) {
        const asset::AssetRecord& r = registry.get(i);
        if (!source.empty() && (std::strncmp(r.path, source.c_str(), source.size()) != 0 || r.path[source.size()] != '/')) continue;
        char from[300], meta[300];
        registry.source_path(r, from, sizeof from);
        registry.meta_path(r, meta, sizeof meta);
        if (patch && asset::manifest_matches(base, r.path, asset::hash_file(from))) continue;
        const fs::path to = out / "Content" / r.path;
        fs::create_directories(to.parent_path(), error);
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, error);
        if (!error && fs::exists(meta, error)) fs::copy_file(meta, to.string() + ".meta", fs::copy_options::overwrite_existing, error);
        ++copied;
    }
    if (error || !copied) {
        if (error) std::snprintf(message, capacity, "Packaging the DLC failed: %s", error.message().c_str());
        else std::snprintf(message, capacity, "Nothing to package%s", patch ? " (every asset matches the base)" : " in that folder");
        return false;
    }
    asset::Manifest manifest = asset::manifest_of_folder((out / "Content").generic_string().c_str());
    manifest.name = name;
    asset::save_manifest(manifest, (out / kManifestFile).generic_string().c_str());
    std::snprintf(message, capacity, "Packaged %s: %u asset%s%s. Copy the folder into the game's DLC/ folder; the game mounts it at start", out.generic_string().c_str(), copied,
                  copied == 1 ? "" : "s", patch ? " new or changed since the base" : "");
    LSD_LOG_INFO("editor", "%s", message);
    return true;
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
#ifdef NDEBUG
    fs::path exe = here / "lsd_editor.exe";  // this engine is a Release build (the root copy too): it ships itself
    const bool release = true;
#else
    fs::path exe = here.parent_path().parent_path() / "Release" / "lsd_editor.exe";
    const bool release = fs::exists(exe, error);
    if (!release) exe = here / "lsd_editor.exe";
#endif
    if (!fs::exists(exe, error)) {
        std::snprintf(message, capacity, "Could not find the engine executable (%s)", exe.generic_string().c_str());
        return false;
    }

    const std::string name = folder_name(project.project_name);
    const fs::path out = fs::path(kBuildDir) / name;
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
#ifdef _WIN32
    if (!error) brand_exe(project, registry, out / (name + ".exe"));
#endif
    if (const std::string splash = asset_file(registry, project.splash); !error && !splash.empty()) {
        std::string ext = fs::path(splash).extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::error_code ignored;
        fs::copy_file(splash, out / ("splash" + ext), fs::copy_options::overwrite_existing, ignored);
    }
    if (project.cook_used_only) {
        if (!cook(project, registry, out, error)) {
            std::snprintf(message, capacity, "Cooking into %s failed: %s", out.generic_string().c_str(), error.message().c_str());
            return false;
        }
    } else if (!error) {
        fs::copy("Content", out / "Content", copy_options, error);
        for (const asset::AssetMount& m : registry.mounts()) {  // plugins and DLC land in Content/ under their prefix
            if (!error && m.prefix[0] && fs::exists(m.dir, error)) fs::copy(m.dir, out / "Content" / m.prefix, copy_options, error);
        }
    }
    if (!error) {  // what the base package holds, for Package DLC's patches (hashes of the source files, before stripping)
        asset::Manifest manifest = asset::manifest_of_folder((out / "Content").generic_string().c_str());
        manifest.name = name;
        asset::save_manifest(manifest, (out / kManifestFile).generic_string().c_str());
    }
    if (!error) strip_editor_only(out / "Content");
    if (!error && project.cook_max_texture_size) {  // Cook Max Texture Size: the packaged copies only
        u32 scaled = 0;
        for (usize i = 0; i < registry.count(); ++i) {
            const asset::AssetRecord& r = registry.get(i);
            std::error_code ignored;
            const fs::path cooked = out / "Content" / r.path;
            if (r.type == asset::AssetType::Texture && fs::exists(cooked, ignored)) scaled += asset::downscale_texture_file(cooked.string().c_str(), project.cook_max_texture_size) ? 1u : 0u;
        }
        LSD_LOG_INFO("editor", "Cook Max Texture Size %u: %u texture%s scaled down", project.cook_max_texture_size, scaled, scaled == 1 ? "" : "s");
    }
    if (!error) write_level_packages(registry, out);
    if (!error) fs::copy("Config", out / "Config", copy_options, error);
    if (!error && fs::exists("Binaries", error)) fs::copy("Binaries", out / "Binaries", copy_options, error);  // the C / C++ game code
    if (!error) {  // compiled shaders (the project's and this PC's built-in ones): a player's first start reads them instead of compiling
        // shortcut: copies the whole user cache, older engine builds' shaders too (a few MB at most); copy only what this build used if that grows.
        std::error_code ignored;
        fs::create_directories(out / "DerivedData" / "Shaders", ignored);
        for (const bool project_folder : {true, false}) fs::copy(rhi::shader_cache_folder(project_folder), out / "DerivedData" / "Shaders", copy_options, ignored);
    }
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
        std::fprintf(marker, "{\n  \"packaged\": true,\n  \"shipping\": %s\n}\n", project.build_configuration == 1 ? "true" : "false");
        std::fclose(marker);
    }
    const bool stale = release && fs::exists(here / "lsd_editor.exe", error) && fs::last_write_time(exe, error) < fs::last_write_time(here / "lsd_editor.exe", error);
    char validated[64] = "";
    if (problems) std::snprintf(validated, sizeof validated, " (%u asset problem%s: see the Message Log)", problems, problems == 1 ? "" : "s");
    std::snprintf(message, capacity, "Packaged %s%s%s", out.generic_string().c_str(), validated,
                  !release ? " (Debug build: other PCs need Visual Studio; build Release first: Build > Build Release Engine)"
                  : stale  ? " (the Release build is older than this editor: Build > Build Release Engine for the latest engine)"
                           : "");
    LSD_LOG_INFO("editor", "%s", message);
    return true;
}

} // namespace lsd::editor
