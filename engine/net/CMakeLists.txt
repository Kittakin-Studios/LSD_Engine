add_library(lsd_net STATIC
  src/net.cpp
  src/voice.cpp
  src/join_code.cpp
)
target_include_directories(lsd_net PUBLIC include)
target_link_libraries(lsd_net PUBLIC lsd_foundation lsd_world PRIVATE enet opus miniupnpc)
lsd_configure_target(lsd_net)
