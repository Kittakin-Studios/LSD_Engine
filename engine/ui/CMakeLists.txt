add_library(lsd_ui STATIC
  src/ui.cpp
  src/ui_script.cpp
)
target_include_directories(lsd_ui PUBLIC include)
target_link_libraries(lsd_ui PUBLIC lsd_foundation lsd_rhi PRIVATE lsd_asset lsd_reflect RmlUi::RmlUi SDL3::SDL3-static quickjs)
lsd_configure_target(lsd_ui)
