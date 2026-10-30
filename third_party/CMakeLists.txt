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
