add_library(lsd_input STATIC
  src/input.cpp
  src/touch.cpp
)
target_include_directories(lsd_input PUBLIC include)
target_link_libraries(lsd_input PUBLIC lsd_foundation lsd_reflect PRIVATE SDL3::SDL3-static)
lsd_configure_target(lsd_input)
lsd_reflect(lsd_input include/lsd/input/input.h)
