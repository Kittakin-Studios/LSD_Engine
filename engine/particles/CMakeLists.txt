add_library(lsd_particles STATIC
  src/particles.cpp
)
target_include_directories(lsd_particles PUBLIC include)
target_link_libraries(lsd_particles PUBLIC lsd_foundation lsd_reflect lsd_rhi)
lsd_configure_target(lsd_particles)
lsd_reflect(lsd_particles include/lsd/particles/particles.h)
