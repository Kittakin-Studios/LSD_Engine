#include "game_module.h"

#include "lsd/foundation/log.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace lsd::editor::game_module {

namespace {

unsigned long process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}

#ifdef _WIN32
constexpr char kModuleFile[] = "Binaries/Game.dll";
constexpr char kCopyExtension[] = ".dll";
#else
constexpr char kModuleFile[] = "Binaries/Game.so";
constexpr char kCopyExtension[] = ".so";
#endif
constexpr char kCopyFolder[] = "Intermediate/GameModule";

using RegisterFn = int (*)(const lsd_api*, lsd_registry*);

script::NativeModule g_module;
void* g_library = nullptr;
fs::file_time_type g_loaded_time{};
bool g_had_file = false;
u32 g_copy_number = 0;

std::thread g_compile;
std::atomic<bool> g_compiling{false};

i64 file_stamp(const fs::path& p, bool& exists) {
    std::error_code error;
    exists = fs::exists(p, error);
    return exists ? static_cast<i64>(fs::last_write_time(p, error).time_since_epoch().count()) : 0;
}

void* open_library(const fs::path& path) {
#ifdef _WIN32
    return LoadLibraryW(path.wstring().c_str());
#else
    return dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* symbol(void* library, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

void close_library(void* library) {
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(library));
#else
    dlclose(library);
#endif
}

// Where lsd_game.h lives: the project folder when it is the engine's own repository, else above the exe.
std::string api_include_dir() {
    std::error_code error;
    if (fs::exists("engine/game_api/include/lsd/lsd_game.h", error)) return fs::absolute("engine/game_api/include", error).generic_string();
    fs::path dir = SDL_GetBasePath() ? fs::path(SDL_GetBasePath()) : fs::path();
    for (int up = 0; up < 6 && !dir.empty(); ++up, dir = dir.parent_path()) {
        if (fs::exists(dir / "engine/game_api/include/lsd/lsd_game.h", error)) return (dir / "engine/game_api/include").generic_string();
    }
    return "";
}

#ifdef _WIN32
// The newest Visual Studio's vcvars64.bat, through vswhere (installed with every Visual Studio).
std::string find_vcvars() {
    const char* x86 = std::getenv("ProgramFiles(x86)");
    const std::string vswhere = std::string(x86 ? x86 : "C:/Program Files (x86)") + "/Microsoft Visual Studio/Installer/vswhere.exe";
    std::error_code error;
    if (!fs::exists(vswhere, error)) return "";
    const std::string command = "\"\"" + vswhere + "\" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath\"";
    std::string path;
    if (FILE* pipe = _popen(command.c_str(), "r")) {
        char line[512];
        if (std::fgets(line, sizeof line, pipe)) path = line;
        _pclose(pipe);
    }
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r' || path.back() == ' ')) path.pop_back();
    const std::string vcvars = path + "/VC/Auxiliary/Build/vcvars64.bat";
    return !path.empty() && fs::exists(vcvars, error) ? vcvars : "";
}
#endif

void run_compile(std::string script) {
    LSD_LOG_INFO("compile", "Compiling game code (Source/ -> %s)...", kModuleFile);
#ifdef _WIN32
    FILE* pipe = _popen(script.c_str(), "r");
#else
    FILE* pipe = popen(script.c_str(), "r");
#endif
    int status = -1;
    if (pipe) {
        char line[1024];
        while (std::fgets(line, sizeof line, pipe)) {
            usize n = std::strlen(line);
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
            if (n == 0) continue;
            const bool error = std::strstr(line, ": error") || std::strstr(line, " error C") || std::strstr(line, "FAILED:") || std::strstr(line, "CMake Error");
            const bool warning = std::strstr(line, ": warning");
            if (error) LSD_LOG_ERROR("compile", "%s", line);
            else if (warning) LSD_LOG_WARN("compile", "%s", line);
            else LSD_LOG_INFO("compile", "%s", line);
        }
#ifdef _WIN32
        status = _pclose(pipe);
#else
        status = pclose(pipe);
#endif
    }
    if (status == 0) LSD_LOG_INFO("compile", "Game code compiled: it loads in a moment (also into a running Play)");
    else LSD_LOG_ERROR("compile", "Compiling game code failed (see the lines above); the previous game code keeps running");
    g_compiling = false;
}

bool write_if_missing(const fs::path& path, const std::string& text) {
    std::error_code error;
    if (fs::exists(path, error)) return true;
    std::ofstream file(path, std::ios::binary);
    file << text;
    return static_cast<bool>(file);
}

constexpr char kCMakeLists[] = R"(# Game code (Unreal: the project's C++ module). The editor's Tools > Compile Game Code (Ctrl+Alt+F11)
# builds every .c / .cpp file in this folder into Binaries/Game.dll and loads it. Game code includes only
# <lsd/game.hpp> (C++) or <lsd/lsd_game.h> (C); docs/CODING.md lists every command.
cmake_minimum_required(VERSION 3.25)
project(Game C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Debug info and no optimisation (breakpoints work), but the release C runtime: packaged games run on
# PCs without Visual Studio.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_library(Game SHARED ${GAME_SOURCES})
target_include_directories(Game PRIVATE "${LSD_GAME_API_INCLUDE}")
set_target_properties(Game PROPERTIES PREFIX ""
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../Binaries"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../Binaries")
if(MSVC)
  target_compile_options(Game PRIVATE /W4 /utf-8)
else()
  target_compile_options(Game PRIVATE -Wall -Wextra)
endif()
)";

constexpr char kGameModule[] = R"(// The game code's entry point (exactly one): it collects every C++ script (LSD_SCRIPT) and every
// C script (LSD_C_SCRIPT) in Source/ when the editor or the game loads Game.dll. Leave it as it is.
#include <lsd/game.hpp>

LSD_GAME_MODULE()
)";

constexpr char kCppScript[] = R"(// $NAME: a C++ script (Unreal: an Actor Component class). Put it on an object: Details > Add
// Component > Native Script, Script = $NAME. Tools > Compile Game Code (Ctrl+Alt+F11) builds it; it
// reloads by itself, also while playing. Every command: docs/CODING.md.
#include <lsd/game.hpp>

using namespace lsd::game;

struct $NAME {
    float speed = 90.0f;  // degrees a second; each object can change it in Details

    static void properties(Properties<$NAME>& p) { p.add("speed", &$NAME::speed); }

    void on_start(Self self) { log("%s: $NAME started", name(self)); }

    void on_update(Self self, float dt) {
        Vec3 r = rotation(self);
        r.z += speed * dt;
        set_rotation(self, r);
    }

    // Also available: on_stop(self), on_hit(self, other), on_overlap_begin(self, other),
    // on_overlap_end(self, other), on_event(self, name, other) for lsd.call / Call Event / UI actions.
};
LSD_SCRIPT($NAME)
)";

constexpr char kCScript[] = R"(/* $NAME: a C script (Unreal: an Actor Component class). Put it on an object: Details > Add
 * Component > Native Script, Script = $NAME. Tools > Compile Game Code (Ctrl+Alt+F11) builds it; it
 * reloads by itself, also while playing. Every command: docs/CODING.md (C names start with lsd_). */
#include <lsd/lsd_game.h>
#include <stdlib.h>

/* Per-object state. */
typedef struct $NAME {
    float speed; /* degrees a second; each object can change it in Details */
} $NAME;

static void* $NAME_create(void) {
    $NAME* state = ($NAME*)calloc(1, sizeof($NAME));
    if (state) state->speed = 90.0f; /* defaults */
    return state;
}

static void $NAME_destroy(void* state) { free(state); }

static void $NAME_start(lsd_entity self, void* state) {
    (void)state;
    lsd_log("%s: $NAME started", lsd_name(self));
}

static void $NAME_update(lsd_entity self, void* state, float dt) {
    lsd_vec3 r = lsd_rotation(self);
    r.z += (($NAME*)state)->speed * dt;
    lsd_set_rotation(self, r);
}

/* Also available: on_stop, on_hit, on_overlap_begin, on_overlap_end, on_event (see lsd_game.h). */

static const lsd_property $NAME_properties[] = {
    {"speed", LSD_FLOAT, offsetof($NAME, speed), 0},
};

static void $NAME_register(lsd_registry* registry) {
    lsd_script script = {0};
    script.name = "$NAME";
    script.state_size = sizeof($NAME);
    script.create = $NAME_create;
    script.destroy = $NAME_destroy;
    script.on_start = $NAME_start;
    script.on_update = $NAME_update;
    script.properties = $NAME_properties;
    script.property_count = 1;
    lsd_register_script(registry, &script);
}
LSD_C_SCRIPT($NAME_register)
)";

} // namespace

const script::NativeModule& module() { return g_module; }
bool loaded() { return g_library != nullptr; }

void unload() {
    g_module.clear();
    if (g_library) close_library(g_library);
    g_library = nullptr;
}

bool load() {
    unload();
    std::error_code error;
    g_had_file = fs::exists(kModuleFile, error);
    g_loaded_time = g_had_file ? fs::last_write_time(kModuleFile, error) : fs::file_time_type{};
    if (!g_had_file) return false;
    // Load a numbered copy: the build can overwrite Game.dll while this one stays loaded.
    fs::create_directories(kCopyFolder, error);
    for (const auto& old : fs::directory_iterator(kCopyFolder, error)) {
        std::error_code ignored;
        fs::remove(old.path(), ignored);  // copies still loaded (none after unload) stay locked: fine
    }
    const fs::path copy = fs::path(kCopyFolder) / ("Game-" + std::to_string(process_id()) + "-" + std::to_string(++g_copy_number) + kCopyExtension);  // per process: several game windows
    fs::copy_file(kModuleFile, copy, fs::copy_options::overwrite_existing, error);
    if (error) {
        LSD_LOG_ERROR("game", "Could not copy %s: %s", kModuleFile, error.message().c_str());
        return false;
    }
    g_library = open_library(copy);
    const auto entry = g_library ? reinterpret_cast<RegisterFn>(symbol(g_library, "lsd_game_register")) : nullptr;
    if (!entry) {
        LSD_LOG_ERROR("game", "%s is not a game module (no lsd_game_register: is LSD_GAME_MODULE in one of its files?)", kModuleFile);
        unload();
        return false;
    }
    if (!entry(script::native_api(), g_module.registry())) {
        LSD_LOG_ERROR("game", "%s was built for another version of lsd_game.h: compile it again", kModuleFile);
        unload();
        return false;
    }
    LSD_LOG_INFO("game", "C / C++ scripts loaded: %u", g_module.count);
    return true;
}

bool changed() {
    std::error_code error;
    const bool exists = fs::exists(kModuleFile, error);
    if (exists != g_had_file) return true;
    return exists && fs::last_write_time(kModuleFile, error) != g_loaded_time;
}

bool compiling() { return g_compiling; }

void compile() {
    if (g_compiling) return;
    std::error_code error;
    if (!fs::exists("Source/CMakeLists.txt", error)) {
        LSD_LOG_WARN("compile", "No C / C++ scripts yet: Tools > New C / C++ Script makes Source/ with a first one");
        return;
    }
    const std::string include = api_include_dir();
    if (include.empty()) {
        LSD_LOG_ERROR("compile", "lsd_game.h not found (engine/game_api/include)");
        return;
    }
    if (g_compile.joinable()) g_compile.join();
    // The build runs from a script file: no quoting puzzles in the shell line.
    fs::create_directories("Intermediate", error);
#ifdef _WIN32
    const std::string vcvars = find_vcvars();
    std::ofstream bat("Intermediate/compile_game.bat", std::ios::binary);
    bat << "@echo off\r\n";
    if (!vcvars.empty()) bat << "call \"" << vcvars << "\" >nul 2>&1\r\n";
    bat << "cmake -S Source -B Intermediate/GameBuild -G Ninja -DCMAKE_BUILD_TYPE=Debug \"-DLSD_GAME_API_INCLUDE=" << include << "\" || exit /b 1\r\n";
    bat << "cmake --build Intermediate/GameBuild || exit /b 1\r\n";
    bat.close();
    std::string script = "Intermediate\\compile_game.bat 2>&1";
#else
    std::string script = "cmake -S Source -B Intermediate/GameBuild -DCMAKE_BUILD_TYPE=Debug \"-DLSD_GAME_API_INCLUDE=" + include +
                         "\" 2>&1 && cmake --build Intermediate/GameBuild 2>&1";
#endif
    g_compiling = true;
    g_compile = std::thread(run_compile, script);
}

void shutdown() {
    if (g_compile.joinable()) g_compile.join();
}

bool new_script(const char* name, bool c, char* message, usize capacity) {
    bool valid = name && std::isalpha(static_cast<unsigned char>(name[0]));
    for (const char* ch = name; valid && *ch; ++ch) valid = std::isalnum(static_cast<unsigned char>(*ch)) || *ch == '_';
    if (!valid) {
        std::snprintf(message, capacity, "A script name is a C / C++ name: letters, digits and _, starting with a letter");
        return false;
    }
    std::error_code error;
    fs::create_directories("Source", error);
    const fs::path file = fs::path("Source") / (std::string(name) + (c ? ".c" : ".cpp"));
    for (const char* extension : {".c", ".cpp"}) {  // one script per name, whatever its language
        const fs::path existing = fs::path("Source") / (std::string(name) + extension);
        if (fs::exists(existing, error)) {
            std::snprintf(message, capacity, "%s already exists", existing.generic_string().c_str());
            return false;
        }
    }
    std::string text = c ? kCScript : kCppScript;
    for (usize at; (at = text.find("$NAME")) != std::string::npos;) text.replace(at, 5, name);
    const bool ok = write_if_missing("Source/CMakeLists.txt", kCMakeLists) && write_if_missing("Source/GameModule.cpp", kGameModule) && write_if_missing(file, text);
    std::snprintf(message, capacity, ok ? "Wrote %s: compiling it now" : "Could not write %s", file.generic_string().c_str());
    return ok;
}

} // namespace lsd::editor::game_module
