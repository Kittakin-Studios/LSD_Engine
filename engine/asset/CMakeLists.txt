# stb_image's implementation: third-party C, without our warning flags.
add_library(lsd_stb STATIC src/stb_impl.c src/tinyexr_impl.cpp)
target_include_directories(lsd_stb SYSTEM PUBLIC ${LSD_STB_DIR} ${LSD_TINYEXR_DIR})
if(MSVC)
  target_compile_options(lsd_stb PRIVATE /w)
else()
  target_compile_options(lsd_stb PRIVATE -w)
endif()

add_library(lsd_asset STATIC
  src/guid.cpp
  src/asset_registry.cpp
  src/data_asset.cpp
  src/data_table.cpp
  src/localization.cpp
  src/texture.cpp
  src/light_profile.cpp
  src/mesh_import.cpp
  src/material_import.cpp
  src/cgltf_impl.cpp
)
target_include_directories(lsd_asset PUBLIC include)
target_link_libraries(lsd_asset PUBLIC lsd_foundation lsd_reflect lsd_render PRIVATE ufbx cgltf meshoptimizer lsd_stb)
lsd_configure_target(lsd_asset)
lsd_reflect(lsd_asset include/lsd/asset/asset_meta.h)
