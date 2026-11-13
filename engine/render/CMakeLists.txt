add_library(lsd_render STATIC
  src/mesh.cpp
  src/lod.cpp
  src/grid.cpp
  src/material.cpp
  src/occlusion.cpp
  src/reflection.cpp
  src/texture_streaming.cpp
  src/fracture.cpp
)
target_include_directories(lsd_render PUBLIC include)
target_link_libraries(lsd_render PUBLIC lsd_foundation lsd_rhi lsd_reflect PRIVATE meshoptimizer)
lsd_configure_target(lsd_render)
lsd_reflect(lsd_render include/lsd/render/material.h)
