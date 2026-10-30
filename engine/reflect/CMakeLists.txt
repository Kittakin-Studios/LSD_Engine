add_library(lsd_reflect STATIC
  src/reflect.cpp
)
target_include_directories(lsd_reflect PUBLIC include)
target_link_libraries(lsd_reflect PUBLIC lsd_foundation)
lsd_configure_target(lsd_reflect)
lsd_reflect(lsd_reflect include/lsd/reflect/color.h ../foundation/include/lsd/foundation/math.h)
