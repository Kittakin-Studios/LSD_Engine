add_library(lsd_script STATIC
  src/script.cpp
  src/graph.cpp
  src/material_graph.cpp
  src/native.cpp
  src/js.cpp
)
target_include_directories(lsd_script PUBLIC include ${PROJECT_SOURCE_DIR}/engine/game_api/include)  # lsd_game.h: the C API game code sees
target_link_libraries(lsd_script PUBLIC lsd_foundation lsd_world PRIVATE lsd_asset luajit quickjs)
lsd_configure_target(lsd_script)
