add_library(lsd_asset STATIC
  src/guid.cpp
  src/asset_registry.cpp
  src/mesh_import.cpp
  src/cgltf_impl.cpp
)
target_include_directories(lsd_asset PUBLIC include)
target_link_libraries(lsd_asset PUBLIC lsd_foundation lsd_reflect lsd_render PRIVATE ufbx cgltf meshoptimizer)
lsd_configure_target(lsd_asset)
lsd_reflect(lsd_asset include/lsd/asset/asset_meta.h)
