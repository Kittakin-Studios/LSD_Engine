add_library(lsd_script STATIC
  src/script.cpp
  src/graph.cpp
  src/material_graph.cpp
)
target_include_directories(lsd_script PUBLIC include)
target_link_libraries(lsd_script PUBLIC lsd_foundation lsd_world PRIVATE lsd_asset luajit)
lsd_configure_target(lsd_script)
