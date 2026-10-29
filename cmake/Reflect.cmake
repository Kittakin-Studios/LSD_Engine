# lsd_reflect(<target> <header>...): runs lsd_reflect_gen on each header before compiling and adds
# the generated <header>.gen.cpp to the target (like UnrealHeaderTool before the compiler).
function(lsd_reflect target)
  foreach(header IN LISTS ARGN)
    get_filename_component(header_path "${header}" ABSOLUTE)
    get_filename_component(header_name "${header}" NAME_WE)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/reflect_gen/${header_name}.gen.cpp")
    add_custom_command(
      OUTPUT "${output}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/reflect_gen"
      COMMAND lsd_reflect_gen "${header_path}" "${output}"
      DEPENDS "${header_path}" lsd_reflect_gen
      COMMENT "Reflecting ${header_name}.h"
      VERBATIM
    )
    target_sources(${target} PRIVATE "${output}")
  endforeach()
  if(NOT target STREQUAL "lsd_reflect")  # the reflect library reflects its own color types
    target_link_libraries(${target} PUBLIC lsd_reflect)
  endif()
endfunction()
