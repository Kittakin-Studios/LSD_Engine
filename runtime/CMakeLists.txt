add_executable(lsd_runtime main.cpp)
target_link_libraries(lsd_runtime PRIVATE lsd_foundation lsd_core lsd_rhi SDL3::SDL3-static)
lsd_configure_target(lsd_runtime)
