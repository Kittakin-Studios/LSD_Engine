# FreeType comes from third_party/CMakeLists.txt (FetchContent), not the system: this makes
# find_package(Freetype) (RmlUi asks for it) find that target.
if(TARGET freetype)
  if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
  endif()
  get_target_property(_lsd_ft_includes freetype INTERFACE_INCLUDE_DIRECTORIES)
  set(FREETYPE_INCLUDE_DIRS "${freetype_SOURCE_DIR}/include")
  set(FREETYPE_LIBRARIES freetype)
  set(FREETYPE_VERSION_STRING "2.13.3")
  set(FREETYPE_FOUND TRUE)
  set(Freetype_FOUND TRUE)
endif()
