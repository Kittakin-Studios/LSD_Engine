add_library(lsd_animation STATIC
  src/animation.cpp
)
target_include_directories(lsd_animation PUBLIC include)
target_link_libraries(lsd_animation PUBLIC lsd_foundation lsd_rhi PRIVATE ozz_animation ozz_animation_offline ufbx)
lsd_configure_target(lsd_animation)
