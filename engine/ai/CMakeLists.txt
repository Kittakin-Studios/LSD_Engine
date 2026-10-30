add_library(lsd_ai STATIC
  src/navigation.cpp
  src/behavior_tree.cpp
)
target_include_directories(lsd_ai PUBLIC include)
target_link_libraries(lsd_ai PUBLIC lsd_foundation PRIVATE Recast Detour DetourCrowd)
lsd_configure_target(lsd_ai)
