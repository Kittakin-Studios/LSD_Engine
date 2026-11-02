// The game module's entry point (exactly one per module). Behaviours register themselves with
// LSD_BEHAVIOUR in their own files; a C behaviour calls lsd_register_behaviour from a function
// passed to LSD_GAME_MODULE_C instead (see docs/CODING.md).
#include <lsd/game.hpp>

LSD_GAME_MODULE()
