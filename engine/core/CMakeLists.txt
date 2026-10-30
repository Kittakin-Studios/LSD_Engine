add_library(lsd_core STATIC
  src/job_system.cpp
  src/console.cpp
)
target_include_directories(lsd_core PUBLIC include)
target_link_libraries(lsd_core PUBLIC lsd_foundation)
lsd_configure_target(lsd_core)
