cmake_minimum_required(VERSION 3.24)
project(LSD_Engine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- Graphics backends (each can be toggled; at least one must be ON) ----
option(LSD_RHI_D3D12  "Build the Direct3D 12 backend (Windows only)" ${WIN32})
option(LSD_RHI_VULKAN "Build the Vulkan backend"                       ON)
option(LSD_RHI_OPENGL "Build the OpenGL 4.6 fallback backend"          ON)

# ---- Optional compute plugins for editor/offline tools ----
option(LSD_COMPUTE_CUDA "Build the optional CUDA compute plugin"  OFF)
option(LSD_COMPUTE_HIP  "Build the optional ROCm/HIP compute plugin" OFF)

option(LSD_BUILD_EDITOR "Build the editor"         ON)
option(LSD_BUILD_TOOLS  "Build the data compiler"  ON)
option(LSD_BUILD_TESTS  "Build unit tests"         ON)

include(cmake/CompilerSettings.cmake)
include(cmake/Reflect.cmake)

add_subdirectory(third_party)
add_subdirectory(engine/foundation)
add_subdirectory(engine/core)
add_subdirectory(engine/reflect)
add_subdirectory(tools/reflect)
add_subdirectory(engine/rhi)
add_subdirectory(engine/render)
add_subdirectory(engine/asset)
add_subdirectory(engine/world)
add_subdirectory(engine/input)
add_subdirectory(engine/physics)
add_subdirectory(engine/script)
add_subdirectory(engine/audio)
add_subdirectory(engine/debug_ui)
add_subdirectory(runtime)

if(LSD_BUILD_EDITOR)
  add_subdirectory(editor)
endif()

if(LSD_BUILD_TOOLS)
  add_subdirectory(tools/data_compiler)
endif()

if(LSD_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
