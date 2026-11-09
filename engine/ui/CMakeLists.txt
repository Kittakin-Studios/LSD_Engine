add_library(lsd_ui STATIC
  src/ui.cpp
  src/ui_script.cpp
)
target_include_directories(lsd_ui PUBLIC include)
target_link_libraries(lsd_ui PUBLIC lsd_foundation lsd_rhi PRIVATE lsd_asset lsd_reflect RmlUi::RmlUi SDL3::SDL3-static quickjs)
# The UI Text Scale wraps RmlUi's default font engine, whose header is in its sources (pinned to 6.0).
get_target_property(LSD_RMLUI_CORE_DIR rmlui_core SOURCE_DIR)
target_include_directories(lsd_ui SYSTEM PRIVATE ${LSD_RMLUI_CORE_DIR})
lsd_configure_target(lsd_ui)
