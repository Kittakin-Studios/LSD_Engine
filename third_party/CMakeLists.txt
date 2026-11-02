# Third-party libraries, fetched at configure time and pinned to a release tag.
# Each library's licence ships in its source tree under build/<preset>/_deps/.
include(FetchContent)

# SDL3 (zlib licence): window, input, gamepads.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-3.4.18
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_MakeAvailable(SDL3)

# Dear ImGui (MIT): debug UI and editor v1. Docking branch for dockable editor panels later.
# Core + SDL3 platform backend here; each RHI backend compiles its own ImGui renderer.
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.93.0-docking
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
)
target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static)
set(LSD_IMGUI_DIR ${imgui_SOURCE_DIR} PARENT_SCOPE)

# Jolt Physics (MIT): rigid bodies, collision queries and the character controller.
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)  # we link the DLL runtime (/MD, /MDd)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
FetchContent_Declare(JoltPhysics
  GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
  GIT_TAG v5.6.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR Build
  SYSTEM
)
FetchContent_MakeAvailable(JoltPhysics)

# ImGuizmo (MIT): move / rotate / scale handles in the editor viewport. No release tags since
# 2021 that build against current ImGui, so pinned to a commit.
FetchContent_Declare(imguizmo
  GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
  GIT_TAG 105e4fde45e4e74aae8973d98750820ef7aca7e2
  SOURCE_SUBDIR not_used  # fetch only: we build ImGuizmo.cpp against our ImGui target below
)
FetchContent_MakeAvailable(imguizmo)
add_library(lsd_imguizmo STATIC ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp)
target_include_directories(lsd_imguizmo SYSTEM PUBLIC ${imguizmo_SOURCE_DIR}/src)
target_link_libraries(lsd_imguizmo PUBLIC imgui)

# Vulkan: headers (Apache-2.0) + volk loader (MIT). No Vulkan SDK needed to build; the
# runtime loads vulkan-1.dll / libvulkan.so from the GPU driver.
if(LSD_RHI_VULKAN)
  FetchContent_Declare(VulkanHeaders
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG vulkan-sdk-1.4.363.0
    GIT_SHALLOW TRUE
    SYSTEM
  )
  # SOURCE_SUBDIR points at a folder without CMakeLists.txt: fetch only, we build volk.c ourselves.
  FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG vulkan-sdk-1.4.363.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR do-not-configure
  )
  FetchContent_MakeAvailable(VulkanHeaders volk)
  add_library(volk STATIC ${volk_SOURCE_DIR}/volk.c)
  target_include_directories(volk SYSTEM PUBLIC ${volk_SOURCE_DIR})
  target_link_libraries(volk PUBLIC Vulkan::Headers)
  target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES)
  if(WIN32)
    target_compile_definitions(volk PUBLIC VK_USE_PLATFORM_WIN32_KHR)
  endif()
endif()

# Mesh import (all MIT): ufbx (FBX), cgltf (glTF 2.0), meshoptimizer (indexing, vertex cache order).
FetchContent_Declare(ufbx
  GIT_REPOSITORY https://github.com/ufbx/ufbx.git
  GIT_TAG v0.23.1
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR do-not-configure
)
FetchContent_Declare(cgltf
  GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
  GIT_TAG v1.15
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR do-not-configure
)
FetchContent_Declare(meshoptimizer
  GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
  GIT_TAG v1.3
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_MakeAvailable(ufbx cgltf meshoptimizer)
add_library(ufbx STATIC ${ufbx_SOURCE_DIR}/ufbx.c)
target_include_directories(ufbx SYSTEM PUBLIC ${ufbx_SOURCE_DIR})
add_library(cgltf INTERFACE)
target_include_directories(cgltf SYSTEM INTERFACE ${cgltf_SOURCE_DIR})
# Blender-exported sample files used by the import tests.
set(LSD_UFBX_DATA_DIR ${ufbx_SOURCE_DIR}/data PARENT_SCOPE)

# LuaJIT (MIT): gameplay scripting. Upstream has no CMake build, so this runs its own host tools
# the way src/msvcbuild.bat and src/Makefile do: minilua -> DynASM -> buildvm -> VM object + headers.
# Pinned to a commit on the v2.1 rolling branch; LSD_LUAJIT_RELVER is that commit's time (%ct).
FetchContent_Declare(luajit
  GIT_REPOSITORY https://github.com/LuaJIT/LuaJIT.git
  GIT_TAG 8ca762976ebc85ad9bde3e15f147a7e326e0dd01
  SOURCE_SUBDIR do-not-configure
)
FetchContent_MakeAvailable(luajit)
set(LSD_LUAJIT_RELVER 1793275035)
set(LJ_SRC ${luajit_SOURCE_DIR}/src)
set(LJ_GEN ${CMAKE_CURRENT_BINARY_DIR}/luajit_gen)
file(MAKE_DIRECTORY ${LJ_GEN})
file(WRITE ${LJ_GEN}/luajit_relver.txt "${LSD_LUAJIT_RELVER}\n")

add_executable(lsd_luajit_minilua ${LJ_SRC}/host/minilua.c)
if(UNIX)
  target_link_libraries(lsd_luajit_minilua PRIVATE m)
endif()

if(WIN32)
  set(LJ_DASM_FLAGS -D WIN -D JIT -D FFI -D ENDIAN_LE -D FPU -D P64)
  set(LJ_VM_MODE peobj)
  set(LJ_VM ${LJ_GEN}/lj_vm.obj)
else()
  set(LJ_DASM_FLAGS -D JIT -D FFI -D ENDIAN_LE -D FPU -D P64)
  set(LJ_VM_MODE elfasm)
  set(LJ_VM ${LJ_GEN}/lj_vm.S)
  enable_language(ASM)
endif()
add_custom_command(OUTPUT ${LJ_GEN}/buildvm_arch.h
  COMMAND lsd_luajit_minilua ${luajit_SOURCE_DIR}/dynasm/dynasm.lua -LN ${LJ_DASM_FLAGS} -o ${LJ_GEN}/buildvm_arch.h vm_x64.dasc
  WORKING_DIRECTORY ${LJ_SRC}
  DEPENDS lsd_luajit_minilua ${LJ_SRC}/vm_x64.dasc
  VERBATIM)
add_custom_command(OUTPUT ${LJ_GEN}/luajit.h
  COMMAND lsd_luajit_minilua host/genversion.lua luajit_rolling.h ${LJ_GEN}/luajit_relver.txt ${LJ_GEN}/luajit.h
  WORKING_DIRECTORY ${LJ_SRC}
  DEPENDS lsd_luajit_minilua ${LJ_SRC}/luajit_rolling.h
  VERBATIM)

file(GLOB LJ_BUILDVM_SOURCES ${LJ_SRC}/host/buildvm*.c)
add_executable(lsd_luajit_buildvm ${LJ_BUILDVM_SOURCES} ${LJ_GEN}/buildvm_arch.h ${LJ_GEN}/luajit.h)
target_include_directories(lsd_luajit_buildvm PRIVATE ${LJ_GEN} ${LJ_SRC} ${luajit_SOURCE_DIR}/dynasm)
target_compile_definitions(lsd_luajit_buildvm PRIVATE _CRT_SECURE_NO_DEPRECATE)

set(LJ_LIB_SOURCES lib_base.c lib_math.c lib_bit.c lib_string.c lib_table.c lib_io.c lib_os.c lib_package.c
  lib_debug.c lib_jit.c lib_ffi.c lib_buffer.c)
set(LJ_HEADERS)
foreach(mode bcdef ffdef libdef recdef)
  add_custom_command(OUTPUT ${LJ_GEN}/lj_${mode}.h
    COMMAND lsd_luajit_buildvm -m ${mode} -o ${LJ_GEN}/lj_${mode}.h ${LJ_LIB_SOURCES}
    WORKING_DIRECTORY ${LJ_SRC}
    DEPENDS lsd_luajit_buildvm
    VERBATIM)
  list(APPEND LJ_HEADERS ${LJ_GEN}/lj_${mode}.h)
endforeach()
add_custom_command(OUTPUT ${LJ_GEN}/lj_folddef.h
  COMMAND lsd_luajit_buildvm -m folddef -o ${LJ_GEN}/lj_folddef.h lj_opt_fold.c
  WORKING_DIRECTORY ${LJ_SRC}
  DEPENDS lsd_luajit_buildvm
  VERBATIM)
add_custom_command(OUTPUT ${LJ_VM}
  COMMAND lsd_luajit_buildvm -m ${LJ_VM_MODE} -o ${LJ_VM}
  DEPENDS lsd_luajit_buildvm
  VERBATIM)

file(GLOB LJ_CORE_SOURCES ${LJ_SRC}/lj_*.c ${LJ_SRC}/lib_*.c)
add_library(luajit STATIC ${LJ_CORE_SOURCES} ${LJ_VM} ${LJ_HEADERS} ${LJ_GEN}/lj_folddef.h ${LJ_GEN}/luajit.h)
target_include_directories(luajit SYSTEM PUBLIC ${LJ_SRC} ${LJ_GEN})
target_compile_definitions(luajit PRIVATE _CRT_SECURE_NO_DEPRECATE)
if(MSVC)
  target_compile_options(luajit PRIVATE /w)
else()
  target_compile_options(luajit PRIVATE -w)
  target_link_libraries(luajit PUBLIC m dl)
endif()

# miniaudio (public domain / MIT-0): audio device, decoding (WAV, FLAC, MP3; OGG through its bundled
# stb_vorbis), mixing, 3D spatialisation. Fetch only: engine/audio compiles the implementation.
FetchContent_Declare(miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio.git
  GIT_TAG 0.11.25
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR do-not-configure
)
FetchContent_MakeAvailable(miniaudio)
set(LSD_MINIAUDIO_DIR ${miniaudio_SOURCE_DIR} PARENT_SCOPE)

# stb_image (public domain / MIT): PNG, JPG, TGA, BMP, PSD, HDR decoding for texture import.
# Fetch only, pinned to a commit (stb has no release tags): engine/asset compiles the implementation.
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5
  SOURCE_SUBDIR do-not-configure
)
FetchContent_MakeAvailable(stb)
set(LSD_STB_DIR ${stb_SOURCE_DIR} PARENT_SCOPE)

# ozz-animation (MIT): skeletons, compressed animations, sampling / blending / IK jobs. Runtime +
# offline builder libraries only; our importer feeds the offline builders from ufbx.
set(ozz_build_tools OFF CACHE BOOL "" FORCE)
set(ozz_build_fbx OFF CACHE BOOL "" FORCE)
set(ozz_build_gltf OFF CACHE BOOL "" FORCE)
set(ozz_build_samples OFF CACHE BOOL "" FORCE)
set(ozz_build_howtos OFF CACHE BOOL "" FORCE)
set(ozz_build_tests OFF CACHE BOOL "" FORCE)
set(ozz_build_msvc_rt_dll ON CACHE BOOL "" FORCE)  # we link the DLL runtime (/MD, /MDd)
FetchContent_Declare(ozz
  GIT_REPOSITORY https://github.com/guillaumeblanc/ozz-animation.git
  GIT_TAG 0.17.0
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_MakeAvailable(ozz)

# Recast & Detour: navmesh building, path finding and crowd steering (zlib).
set(RECASTNAVIGATION_DEMO OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_TESTS OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(recastnavigation
  GIT_REPOSITORY https://github.com/recastnavigation/recastnavigation.git
  GIT_TAG v1.6.0
  GIT_SHALLOW TRUE
  SYSTEM
)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)  # its cmake_minimum_required predates what CMake 4 accepts
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)  # and says so: their notice, not ours to fix
FetchContent_MakeAvailable(recastnavigation)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# FreeType: font rasterising for RmlUi (FreeType Licence, the BSD-style option).
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
FetchContent_Declare(freetype
  GIT_REPOSITORY https://github.com/freetype/freetype.git
  GIT_TAG VER-2-13-3
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_MakeAvailable(freetype)

# RmlUi: HTML / CSS game UI (MIT). Its find_package(Freetype) is answered by cmake/FindFreetype.cmake.
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_FONT_ENGINE "freetype" CACHE STRING "" FORCE)
set(RMLUI_LUA_BINDINGS OFF CACHE BOOL "" FORCE)
set(RMLUI_BACKEND "native" CACHE STRING "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(rmlui
  GIT_REPOSITORY https://github.com/mikke89/RmlUi.git
  GIT_TAG 6.0
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_MakeAvailable(rmlui)

# QuickJS-ng (MIT): JavaScript for UI documents' own scripts. Built from its C sources only (no
# libc module, no tools); the maintained fork, which builds with MSVC.
FetchContent_Declare(quickjs
  GIT_REPOSITORY https://github.com/quickjs-ng/quickjs.git
  GIT_TAG v0.16.2
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR do-not-configure
)
FetchContent_MakeAvailable(quickjs)
add_library(quickjs STATIC ${quickjs_SOURCE_DIR}/dtoa.c ${quickjs_SOURCE_DIR}/libregexp.c ${quickjs_SOURCE_DIR}/libunicode.c ${quickjs_SOURCE_DIR}/quickjs.c)
set_target_properties(quickjs PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_include_directories(quickjs SYSTEM PUBLIC ${quickjs_SOURCE_DIR})
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601 _CRT_SECURE_NO_WARNINGS>)
if(MSVC)
  target_compile_options(quickjs PRIVATE /w /experimental:c11atomics)
else()
  target_compile_options(quickjs PRIVATE -w)
  target_link_libraries(quickjs PUBLIC m)
endif()

# Tracy: the CPU profiler client (BSD-3), only with -DLSD_TRACY=ON (it listens on a network port).
if(LSD_TRACY)
  set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)  # costs nothing until the profiler connects
  FetchContent_Declare(tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG v0.11.1
    GIT_SHALLOW TRUE
    SYSTEM
  )
  FetchContent_MakeAvailable(tracy)
endif()

# ENet (MIT): reliable UDP for multiplayer (engine/net). Built from its C sources (no install, no tests).
FetchContent_Declare(enet
  GIT_REPOSITORY https://github.com/lsalzman/enet.git
  GIT_TAG v1.3.18
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR do-not-configure
)
FetchContent_MakeAvailable(enet)
add_library(enet STATIC ${enet_SOURCE_DIR}/callbacks.c ${enet_SOURCE_DIR}/compress.c ${enet_SOURCE_DIR}/host.c ${enet_SOURCE_DIR}/list.c
  ${enet_SOURCE_DIR}/packet.c ${enet_SOURCE_DIR}/peer.c ${enet_SOURCE_DIR}/protocol.c ${enet_SOURCE_DIR}/unix.c ${enet_SOURCE_DIR}/win32.c)
target_include_directories(enet SYSTEM PUBLIC ${enet_SOURCE_DIR}/include)
if(WIN32)
  target_compile_definitions(enet PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(enet PUBLIC ws2_32 winmm)
else()
  target_compile_definitions(enet PRIVATE HAS_SOCKLEN_T=1 HAS_FCNTL=1 HAS_POLL=1 HAS_GETADDRINFO=1 HAS_GETNAMEINFO=1 HAS_INET_PTON=1 HAS_INET_NTOP=1 HAS_MSGHDR_FLAGS=1)
endif()
if(MSVC)
  target_compile_options(enet PRIVATE /w)
else()
  target_compile_options(enet PRIVATE -w)
endif()
