# Game code (Unreal: the project's C++ module). The editor's Tools > Compile Game Code (Ctrl+Alt+F11)
# builds every .c / .cpp file in this folder into Binaries/Game.dll and loads it. Game code includes only
# <lsd/game.hpp> (C++) or <lsd/lsd_game.h> (C); docs/CODING.md lists every command.
cmake_minimum_required(VERSION 3.25)
project(Game C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Debug info and no optimisation (breakpoints work), but the release C runtime: packaged games run on
# PCs without Visual Studio.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_library(Game SHARED ${GAME_SOURCES})
target_include_directories(Game PRIVATE "${LSD_GAME_API_INCLUDE}")
set_target_properties(Game PROPERTIES PREFIX ""
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../Binaries"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../Binaries")
if(MSVC)
  target_compile_options(Game PRIVATE /W4 /utf-8)
else()
  target_compile_options(Game PRIVATE -Wall -Wextra)
endif()
