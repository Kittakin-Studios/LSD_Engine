# Dear ImGui tools shared by the runtime's debug overlay and the editor (v1).
add_library(lsd_debug_ui STATIC
  src/details_panel.cpp
)
target_include_directories(lsd_debug_ui PUBLIC include)
target_link_libraries(lsd_debug_ui PUBLIC lsd_reflect imgui)
lsd_configure_target(lsd_debug_ui)
