# miniaudio's implementation as its own library: third-party C, built without our warning flags.
add_library(lsd_miniaudio STATIC src/miniaudio_impl.c)
target_include_directories(lsd_miniaudio SYSTEM PUBLIC ${LSD_MINIAUDIO_DIR})
if(MSVC)
  target_compile_options(lsd_miniaudio PRIVATE /w)
else()
  target_compile_options(lsd_miniaudio PRIVATE -w)
  target_link_libraries(lsd_miniaudio PUBLIC dl pthread m)
endif()

add_library(lsd_audio STATIC
  src/audio.cpp
)
target_include_directories(lsd_audio PUBLIC include)
target_link_libraries(lsd_audio PUBLIC lsd_foundation lsd_reflect PRIVATE lsd_miniaudio)
lsd_configure_target(lsd_audio)
lsd_reflect(lsd_audio include/lsd/audio/sound_assets.h)
