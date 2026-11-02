/* A C script exactly as game code writes it (the template's shape): it registers itself with
 * LSD_C_SCRIPT, and script_tests.cpp checks the module found it. Moves +1 m on x per second. */
#include "lsd/lsd_game.h"

static void c_mover_update(lsd_entity self, void* state, float dt) {
    lsd_vec3 p = lsd_position(self);
    (void)state;
    p.x += dt;
    lsd_set_position(self, p);
}

static void c_mover_register(lsd_registry* registry) {
    lsd_script script = {0};
    script.name = "CMover";
    script.state_size = sizeof(int);
    script.on_update = c_mover_update;
    lsd_register_script(registry, &script);
}
LSD_C_SCRIPT(c_mover_register)
