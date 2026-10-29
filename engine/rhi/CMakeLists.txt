add_library(lsd_rhi STATIC
  src/device_selection.cpp
  src/device_factory.cpp
)
target_include_directories(lsd_rhi PUBLIC include)
target_link_libraries(lsd_rhi PUBLIC lsd_foundation)
lsd_configure_target(lsd_rhi)

if(NOT LSD_RHI_D3D12 AND NOT LSD_RHI_VULKAN AND NOT LSD_RHI_OPENGL)
  message(FATAL_ERROR "LSD_Engine: enable at least one graphics backend (LSD_RHI_D3D12, LSD_RHI_VULKAN, LSD_RHI_OPENGL).")
endif()
if(LSD_RHI_D3D12 AND NOT WIN32)
  message(STATUS "LSD_Engine: D3D12 backend requested on a non-Windows platform; disabling it.")
  set(LSD_RHI_D3D12 OFF CACHE BOOL "" FORCE)
endif()

target_compile_definitions(lsd_rhi PUBLIC
  LSD_RHI_D3D12=$<BOOL:${LSD_RHI_D3D12}>
  LSD_RHI_VULKAN=$<BOOL:${LSD_RHI_VULKAN}>
  LSD_RHI_OPENGL=$<BOOL:${LSD_RHI_OPENGL}>
)

if(LSD_RHI_D3D12)
  target_sources(lsd_rhi PRIVATE backends/d3d12/d3d12_device.cpp)
  target_link_libraries(lsd_rhi PRIVATE d3d12 dxgi d3dcompiler)
endif()
# Vulkan and OpenGL backends: next in milestone 1 (see docs/ROADMAP.md).
