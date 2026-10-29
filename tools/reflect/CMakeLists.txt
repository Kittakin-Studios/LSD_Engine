# Reflection code generator; always built because engine targets depend on its output.
add_executable(lsd_reflect_gen main.cpp)
lsd_configure_target(lsd_reflect_gen)
