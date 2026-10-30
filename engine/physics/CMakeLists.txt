add_library(lsd_physics STATIC
  src/physics.cpp
)
target_include_directories(lsd_physics PUBLIC include)
target_link_libraries(lsd_physics PUBLIC lsd_foundation PRIVATE Jolt)
lsd_configure_target(lsd_physics)
