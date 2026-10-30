add_library(lsd_world STATIC
  src/world.cpp
  src/level.cpp
  src/prefab.cpp
)
target_include_directories(lsd_world PUBLIC include)
target_link_libraries(lsd_world PUBLIC lsd_foundation lsd_reflect)
lsd_configure_target(lsd_world)
lsd_reflect(lsd_world include/lsd/world/components.h)
