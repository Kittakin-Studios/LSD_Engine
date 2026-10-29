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
