add_executable(lsd_runtime main.cpp)
target_link_libraries(lsd_runtime PRIVATE lsd_foundation lsd_rhi)
lsd_configure_target(lsd_runtime)
