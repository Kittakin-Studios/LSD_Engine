add_executable(lsd_editor main.cpp editor_ui.cpp editor_modes.cpp
  paper2d.cpp graph_editor.cpp table_editor.cpp sound_player.cpp sound_cue.cpp material_cache.cpp lighting.cpp skinned_cache.cpp animation_player.cpp particle_player.cpp particle_editor.cpp packaging.cpp navigation.cpp behavior.cpp ui_editor.cpp mesh_cache.cpp play_session.cpp source_control.cpp remote_console.cpp game_module.cpp rml_edit.cpp light_bake.cpp debug_draw.cpp references.cpp live_link.cpp decals.cpp timeline_player.cpp timeline_editor.cpp anim_graph_editor.cpp mesh_editor.cpp mesh_materials.cpp assistant.cpp assistant_core.cpp assistant_lessons.cpp prefab_editor.cpp keymap.cpp lua_debugger.cpp thumbnails.cpp transactions.cpp)
lsd_reflect(lsd_editor editor_theme.h editor_settings.h)
target_include_directories(lsd_editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lsd_editor PRIVATE lsd_foundation lsd_core lsd_rhi lsd_render lsd_asset lsd_world lsd_input lsd_physics lsd_script lsd_audio lsd_animation lsd_particles lsd_ai lsd_net lsd_ui lsd_debug_ui lsd_imguizmo SDL3::SDL3-static $<$<PLATFORM_ID:Windows>:ws2_32>)
lsd_configure_target(lsd_editor)
if(WIN32)
  set_target_properties(lsd_editor PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  # Release builds are also copied to the project root: double-click lsd_editor.exe there (the working folder is the project).
  add_custom_command(TARGET lsd_editor POST_BUILD
    COMMAND "$<$<CONFIG:Release>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_FILE:lsd_editor>;${PROJECT_SOURCE_DIR}/lsd_editor.exe>"
    COMMAND_EXPAND_LISTS VERBATIM)
endif()
