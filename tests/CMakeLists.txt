add_executable(lsd_foundation_tests foundation_tests.cpp)
target_link_libraries(lsd_foundation_tests PRIVATE lsd_foundation lsd_core lsd_rhi)
lsd_configure_target(lsd_foundation_tests)
add_test(NAME foundation COMMAND lsd_foundation_tests)

add_executable(lsd_reflect_tests reflect_tests.cpp)
lsd_reflect(lsd_reflect_tests reflect_test_types.h)
target_include_directories(lsd_reflect_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
lsd_configure_target(lsd_reflect_tests)
add_test(NAME reflect COMMAND lsd_reflect_tests)

# The generator must reject bad annotations with an error instead of producing code.
add_test(NAME reflect_gen_rejects_bad_header
  COMMAND lsd_reflect_gen ${CMAKE_CURRENT_SOURCE_DIR}/reflect_bad.h ${CMAKE_CURRENT_BINARY_DIR}/reflect_bad.gen.cpp)
set_tests_properties(reflect_gen_rejects_bad_header PROPERTIES WILL_FAIL TRUE)

add_executable(lsd_render_tests render_tests.cpp)
target_link_libraries(lsd_render_tests PRIVATE lsd_render lsd_rhi)
lsd_configure_target(lsd_render_tests)
add_test(NAME render COMMAND lsd_render_tests)

add_executable(lsd_asset_tests asset_tests.cpp)
target_link_libraries(lsd_asset_tests PRIVATE lsd_asset)
target_include_directories(lsd_asset_tests SYSTEM PRIVATE ${LSD_TINYEXR_DIR})  # the test writes an EXR
target_compile_definitions(lsd_asset_tests PRIVATE
  LSD_UFBX_DATA_DIR="${LSD_UFBX_DATA_DIR}"
  LSD_TEST_TEMP_DIR="${CMAKE_CURRENT_BINARY_DIR}")
lsd_configure_target(lsd_asset_tests)
add_test(NAME asset COMMAND lsd_asset_tests)

add_executable(lsd_world_tests world_tests.cpp)
target_link_libraries(lsd_world_tests PRIVATE lsd_world)
target_compile_definitions(lsd_world_tests PRIVATE LSD_TEST_TEMP_DIR="${CMAKE_CURRENT_BINARY_DIR}")
lsd_configure_target(lsd_world_tests)
add_test(NAME world COMMAND lsd_world_tests)

# Editor logic that needs no GPU (undo / redo history).
add_executable(lsd_editor_tests editor_tests.cpp ${PROJECT_SOURCE_DIR}/editor/assistant_core.cpp ${PROJECT_SOURCE_DIR}/editor/transactions.cpp ${PROJECT_SOURCE_DIR}/editor/sound_cue.cpp ${PROJECT_SOURCE_DIR}/editor/rml_edit.cpp
  ${PROJECT_SOURCE_DIR}/editor/light_bake.cpp ${PROJECT_SOURCE_DIR}/editor/dynamic_gi.cpp ${PROJECT_SOURCE_DIR}/editor/hlod.cpp ${PROJECT_SOURCE_DIR}/editor/lighting.cpp
  ${PROJECT_SOURCE_DIR}/editor/references.cpp ${PROJECT_SOURCE_DIR}/editor/keymap.cpp ${PROJECT_SOURCE_DIR}/editor/landscape_paint.cpp ${PROJECT_SOURCE_DIR}/editor/template_levels.cpp ${PROJECT_SOURCE_DIR}/editor/paper2d_core.cpp ${PROJECT_SOURCE_DIR}/editor/scalability.cpp ${PROJECT_SOURCE_DIR}/editor/behavior.cpp ${PROJECT_SOURCE_DIR}/editor/loc_gather.cpp ${PROJECT_SOURCE_DIR}/editor/landscape_core.cpp ${PROJECT_SOURCE_DIR}/editor/mesh_edit.cpp ${PROJECT_SOURCE_DIR}/editor/pcg_graph.cpp ${PROJECT_SOURCE_DIR}/editor/cloth_core.cpp ${PROJECT_SOURCE_DIR}/editor/editor_utility_core.cpp ${PROJECT_SOURCE_DIR}/editor/multi_user_core.cpp ${PROJECT_SOURCE_DIR}/editor/asset_compiler.cpp ${PROJECT_SOURCE_DIR}/editor/lod_cache.cpp)
target_compile_definitions(lsd_editor_tests PRIVATE LSD_TEST_TEMP_DIR="${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(lsd_editor_tests PRIVATE ${PROJECT_SOURCE_DIR}/editor ${PROJECT_SOURCE_DIR}/engine/ui/include)  # ui_sounds.h: header-only rules
target_link_libraries(lsd_editor_tests PRIVATE lsd_world lsd_script lsd_ai lsd_asset lsd_audio lsd_render lsd_rhi imgui xatlas)
lsd_configure_target(lsd_editor_tests)
add_test(NAME editor COMMAND lsd_editor_tests)

add_executable(lsd_input_tests input_tests.cpp)
target_link_libraries(lsd_input_tests PRIVATE lsd_input lsd_asset SDL3::SDL3-static)
target_compile_definitions(lsd_input_tests PRIVATE LSD_TEST_TEMP_DIR="${CMAKE_CURRENT_BINARY_DIR}")
lsd_configure_target(lsd_input_tests)
add_test(NAME input COMMAND lsd_input_tests)

add_executable(lsd_physics_tests physics_tests.cpp)
target_link_libraries(lsd_physics_tests PRIVATE lsd_physics)
lsd_configure_target(lsd_physics_tests)
add_test(NAME physics COMMAND lsd_physics_tests)

add_executable(lsd_script_tests script_tests.cpp c_script.c)  # c_script.c: a self-registering C script
target_link_libraries(lsd_script_tests PRIVATE lsd_script lsd_asset)
target_compile_definitions(lsd_script_tests PRIVATE LSD_TEST_TEMP_DIR="${CMAKE_CURRENT_BINARY_DIR}")
lsd_configure_target(lsd_script_tests)
add_test(NAME script COMMAND lsd_script_tests)

add_executable(lsd_audio_tests audio_tests.cpp)
target_link_libraries(lsd_audio_tests PRIVATE lsd_audio)
target_compile_definitions(lsd_audio_tests PRIVATE LSD_TEST_TEMP_DIR="${CMAKE_CURRENT_BINARY_DIR}")
lsd_configure_target(lsd_audio_tests)
add_test(NAME audio COMMAND lsd_audio_tests)

add_executable(lsd_animation_tests animation_tests.cpp)
target_link_libraries(lsd_animation_tests PRIVATE lsd_animation ufbx)
target_compile_definitions(lsd_animation_tests PRIVATE LSD_UFBX_DATA_DIR="${LSD_UFBX_DATA_DIR}")
lsd_configure_target(lsd_animation_tests)
add_test(NAME animation COMMAND lsd_animation_tests)

add_executable(lsd_particles_tests particles_tests.cpp)
target_link_libraries(lsd_particles_tests PRIVATE lsd_particles)
lsd_configure_target(lsd_particles_tests)
add_test(NAME particles COMMAND lsd_particles_tests)

add_executable(lsd_ai_tests ai_tests.cpp)
target_link_libraries(lsd_ai_tests PRIVATE lsd_ai)
lsd_configure_target(lsd_ai_tests)
add_test(NAME ai COMMAND lsd_ai_tests)

add_executable(lsd_net_tests net_tests.cpp)
target_link_libraries(lsd_net_tests PRIVATE lsd_net lsd_world lsd_audio)
lsd_configure_target(lsd_net_tests)
add_test(NAME net COMMAND lsd_net_tests)
