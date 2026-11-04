add_library(lsd_rhi STATIC
  src/device_selection.cpp
  src/culling.cpp
  src/device_factory.cpp
)
target_include_directories(lsd_rhi PUBLIC include)
target_link_libraries(lsd_rhi PUBLIC lsd_foundation PRIVATE SDL3::SDL3-static imgui)
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
  target_sources(lsd_rhi PRIVATE backends/d3d12/d3d12_device.cpp ${LSD_IMGUI_DIR}/backends/imgui_impl_dx12.cpp)
  target_link_libraries(lsd_rhi PRIVATE d3d12 dxgi d3dcompiler)
endif()

if(LSD_RHI_OPENGL)
  find_package(OpenGL REQUIRED)
  target_sources(lsd_rhi PRIVATE backends/opengl/opengl_device.cpp ${LSD_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
  target_link_libraries(lsd_rhi PRIVATE OpenGL::GL)
endif()

if(LSD_RHI_VULKAN)
  target_sources(lsd_rhi PRIVATE backends/vulkan/vulkan_device.cpp ${LSD_IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
  target_link_libraries(lsd_rhi PRIVATE volk)
  target_compile_definitions(lsd_rhi PRIVATE IMGUI_IMPL_VULKAN_USE_VOLK)

  # HLSL -> SPIR-V needs a DXC built with SPIR-V support (the Vulkan SDK's; the Windows SDK's
  # dxc cannot). Without it the Vulkan backend still clears and draws the UI, minus the triangle.
  find_program(LSD_DXC_SPIRV NAMES dxc PATHS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" NO_DEFAULT_PATH
               DOC "DXC with SPIR-V code generation (Vulkan SDK)")
  set(spirv_dir "${CMAKE_CURRENT_BINARY_DIR}/spirv")
  if(LSD_DXC_SPIRV)
    set(hlsl "${CMAKE_CURRENT_SOURCE_DIR}/backends/vulkan/triangle.hlsl")
    foreach(stage IN ITEMS vs ps)
      set(spv "${spirv_dir}/triangle_${stage}.spv")
      set(header "${spirv_dir}/triangle_${stage}.spv.h")
      if(stage STREQUAL "vs")
        set(array kTriangleVsSpirv)
      else()
        set(array kTrianglePsSpirv)
      endif()
      add_custom_command(
        OUTPUT "${header}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${spirv_dir}"
        COMMAND "${LSD_DXC_SPIRV}" -spirv -fspv-target-env=vulkan1.3 -T ${stage}_6_0 -E ${stage}_main -Fo "${spv}" "${hlsl}"
        COMMAND "${CMAKE_COMMAND}" -DINPUT=${spv} -DOUTPUT=${header} -DNAME=${array} -P "${PROJECT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        DEPENDS "${hlsl}" "${PROJECT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        COMMENT "Compiling triangle.hlsl (${stage}) to SPIR-V"
        VERBATIM
      )
      target_sources(lsd_rhi PRIVATE "${header}")
    endforeach()
    target_include_directories(lsd_rhi PRIVATE "${spirv_dir}")
    target_compile_definitions(lsd_rhi PRIVATE LSD_VULKAN_HAVE_SPIRV=1)
  else()
    message(STATUS "LSD_Engine: Vulkan SDK not found (its DXC compiles shaders to SPIR-V): "
                   "the Vulkan backend shows the UI only until it is installed. D3D12 and OpenGL are unaffected.")
    target_compile_definitions(lsd_rhi PRIVATE LSD_VULKAN_HAVE_SPIRV=0)
  endif()
endif()

# ImGui's D3D12 backend (third-party) keeps HRESULTs it only asserts on: unused in Release.
set_source_files_properties(${LSD_IMGUI_DIR}/backends/imgui_impl_dx12.cpp PROPERTIES
  COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/wd4189,-Wno-unused-variable>")
