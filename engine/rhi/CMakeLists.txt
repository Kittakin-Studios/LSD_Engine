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

if(LSD_RHI_D3D12 OR LSD_RHI_VULKAN)
  # Shared HLSL (shaders/) embedded as text: D3D12 compiles it at startup, Vulkan builds Material
  # Graph shaders from it (through the Vulkan SDK's DXC when installed).
  set(text_dir "${CMAKE_CURRENT_BINARY_DIR}/shader_text")
  foreach(pair IN ITEMS "post;kPostHlsl" "mesh;kMeshHlsl" "material_prelude;kMaterialPreludeHlsl" "cull;kCullHlsl")
    list(GET pair 0 file)
    list(GET pair 1 name)
    add_custom_command(
      OUTPUT "${text_dir}/${file}_hlsl.h"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${text_dir}"
      COMMAND "${CMAKE_COMMAND}" -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/shaders/${file}.hlsl -DOUTPUT=${text_dir}/${file}_hlsl.h -DNAME=${name}
              -P "${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${file}.hlsl" "${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake"
      COMMENT "Embedding ${file}.hlsl"
      VERBATIM
    )
    target_sources(lsd_rhi PRIVATE "${text_dir}/${file}_hlsl.h")
  endforeach()
  target_include_directories(lsd_rhi PRIVATE "${text_dir}")
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
    # Each shader's vertex and pixel stages, embedded as k<Name>VsSpirv / k<Name>PsSpirv.
    # "file;Name[;DEFINE]": a define makes a variant (mesh_skinned from mesh.hlsl with -DSKINNED...).
    foreach(pair IN ITEMS "triangle;Triangle" "lines;Lines"
                         "shadow;Shadow" "shadow;ShadowSkinned;SKINNED" "shadow;ShadowInstanced;INSTANCED"
                         "ui;Ui" "particle;Particle")
      list(GET pair 0 file)
      list(GET pair 1 name)
      list(LENGTH pair parts)
      set(defines "")
      set(out "${file}")
      if(parts GREATER 2)
        list(GET pair 2 define)
        set(defines "-D${define}")
        string(TOLOWER "${define}" suffix)
        set(out "${file}_${suffix}")
      endif()
      set(hlsl "${CMAKE_CURRENT_SOURCE_DIR}/backends/vulkan/${file}.hlsl")
      foreach(stage IN ITEMS vs ps)
        set(spv "${spirv_dir}/${out}_${stage}.spv")
        set(header "${spirv_dir}/${out}_${stage}.spv.h")
        if(stage STREQUAL "vs")
          set(array k${name}VsSpirv)
        else()
          set(array k${name}PsSpirv)
        endif()
        add_custom_command(
          OUTPUT "${header}"
          COMMAND "${CMAKE_COMMAND}" -E make_directory "${spirv_dir}"
          COMMAND "${LSD_DXC_SPIRV}" -spirv -fspv-target-env=vulkan1.3 -T ${stage}_6_0 -E ${stage}_main ${defines} -Fo "${spv}" "${hlsl}"
          COMMAND "${CMAKE_COMMAND}" -DINPUT=${spv} -DOUTPUT=${header} -DNAME=${array} -P "${PROJECT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
          DEPENDS "${hlsl}" "${PROJECT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
          COMMENT "Compiling ${file}.hlsl (${stage} ${defines}) to SPIR-V"
          VERBATIM
        )
        target_sources(lsd_rhi PRIVATE "${header}")
      endforeach()
    endforeach()
    # The shared lit meshes and sky (shaders/mesh.hlsl): "out|Name|entry|define" -> k<Name>Vs/PsSpirv
    # from vs_<entry> / ps_<entry> (renamed vs_main / ps_main).
    set(mesh_hlsl "${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh.hlsl")
    foreach(item IN ITEMS "mesh|Mesh|main|" "mesh_skinned|MeshSkinned|main|SKINNED" "mesh_instanced|MeshInstanced|main|INSTANCED" "sky|Sky|sky|")
      string(REPLACE "|" ";" parts "${item}")
      list(GET parts 0 out)
      list(GET parts 1 name)
      list(GET parts 2 entry)
      list(LENGTH parts count)
      set(defines "")
      if(count GREATER 3)
        list(GET parts 3 define)
        if(define)
          set(defines "-D${define}")
        endif()
      endif()
      foreach(stage IN ITEMS vs ps)
        set(spv "${spirv_dir}/${out}_${stage}.spv")
        set(header "${spirv_dir}/${out}_${stage}.spv.h")
        if(stage STREQUAL "vs")
          set(array k${name}VsSpirv)
        else()
          set(array k${name}PsSpirv)
        endif()
        add_custom_command(
          OUTPUT "${header}"
          COMMAND "${CMAKE_COMMAND}" -E make_directory "${spirv_dir}"
          COMMAND "${LSD_DXC_SPIRV}" -spirv -fspv-target-env=vulkan1.3 -T ${stage}_6_0 -E ${stage}_${entry} -fspv-entrypoint-name=${stage}_main ${defines} -Fo "${spv}" "${mesh_hlsl}"
          COMMAND "${CMAKE_COMMAND}" -DINPUT=${spv} -DOUTPUT=${header} -DNAME=${array} -P "${PROJECT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
          DEPENDS "${mesh_hlsl}" "${PROJECT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
          COMMENT "Compiling mesh.hlsl (${stage}_${entry} ${defines}) to SPIR-V"
          VERBATIM
        )
        target_sources(lsd_rhi PRIVATE "${header}")
      endforeach()
    endforeach()
    # The shared GPU culling passes (shaders/cull.hlsl): kCull<Pass>Spirv, entry points renamed main.
    set(cull_hlsl "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cull.hlsl")
    foreach(pair IN ITEMS "cull_reset;CullReset" "cull_main;CullMain" "cull_args;CullArgs")
      list(GET pair 0 entry)
      list(GET pair 1 name)
      add_custom_command(
        OUTPUT "${spirv_dir}/${entry}.spv.h"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${spirv_dir}"
        COMMAND "${LSD_DXC_SPIRV}" -spirv -fspv-target-env=vulkan1.3 -T cs_6_0 -E ${entry} -fspv-entrypoint-name=main -Fo "${spirv_dir}/${entry}.spv" "${cull_hlsl}"
        COMMAND "${CMAKE_COMMAND}" -DINPUT=${spirv_dir}/${entry}.spv -DOUTPUT=${spirv_dir}/${entry}.spv.h -DNAME=k${name}Spirv -P "${PROJECT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        DEPENDS "${cull_hlsl}" "${PROJECT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        COMMENT "Compiling cull.hlsl (${entry}) to SPIR-V"
        VERBATIM
      )
      target_sources(lsd_rhi PRIVATE "${spirv_dir}/${entry}.spv.h")
    endforeach()
    # The shared post-process passes (shaders/post.hlsl): vs_full and one pixel shader per pass,
    # embedded as kPost<Pass>Spirv (entry points renamed vs_main / ps_main); t registers at bindings 0-2, s registers from 16 (s2 = 18).
    set(post_hlsl "${CMAKE_CURRENT_SOURCE_DIR}/shaders/post.hlsl")
    foreach(entry IN ITEMS vs_full ps_prefilter ps_down ps_up ps_composite ps_fxaa ps_ssr ps_ao ps_ao_apply ps_taa ps_dof ps_motion ps_meter ps_expose ps_outline ps_velocity)
      string(SUBSTRING "${entry}" 0 2 stage)
      string(SUBSTRING "${entry}" 3 -1 pass)
      string(REPLACE "_" ";" words "${pass}")
      set(name "")
      foreach(word IN LISTS words)
        string(SUBSTRING "${word}" 0 1 first)
        string(TOUPPER "${first}" first)
        string(SUBSTRING "${word}" 1 -1 rest)
        string(APPEND name "${first}${rest}")
      endforeach()
      set(spv "${spirv_dir}/post_${pass}.spv")
      set(header "${spirv_dir}/post_${pass}.spv.h")
      add_custom_command(
        OUTPUT "${header}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${spirv_dir}"
        COMMAND "${LSD_DXC_SPIRV}" -spirv -fspv-target-env=vulkan1.3 -T ${stage}_6_0 -E ${entry} -fspv-entrypoint-name=${stage}_main -fvk-t-shift 0 0 -fvk-s-shift 16 0 -Fo "${spv}" "${post_hlsl}"
        COMMAND "${CMAKE_COMMAND}" -DINPUT=${spv} -DOUTPUT=${header} -DNAME=kPost${name}Spirv -P "${PROJECT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        DEPENDS "${post_hlsl}" "${PROJECT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        COMMENT "Compiling post.hlsl (${entry}) to SPIR-V"
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
