add_executable(lsd_mcp main.cpp)
target_link_libraries(lsd_mcp PRIVATE $<$<PLATFORM_ID:Windows>:ws2_32>)
lsd_configure_target(lsd_mcp)
# Next to lsd_editor.exe, where Editor Preferences > Plugins > Claude MCP points Claude Code.
set_target_properties(lsd_mcp PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/editor")
