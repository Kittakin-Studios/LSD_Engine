add_library(lsd_foundation STATIC
  src/allocator.cpp
  src/log.cpp
  src/system_info.cpp
)
target_include_directories(lsd_foundation PUBLIC include)
lsd_configure_target(lsd_foundation)
find_package(Threads REQUIRED)
target_link_libraries(lsd_foundation PUBLIC Threads::Threads)
if(LSD_TRACY)
  target_link_libraries(lsd_foundation PUBLIC Tracy::TracyClient)
  target_compile_definitions(lsd_foundation PUBLIC LSD_TRACY)
endif()
