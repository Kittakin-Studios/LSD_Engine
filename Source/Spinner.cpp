// Spinner: a behaviour (Unreal: an Actor Component class). Put it on an object: Details > Add
// Component > Native Script, Behaviour = Spinner. Tools > Compile Game Code (Ctrl+Alt+F11) builds it;
// it reloads by itself, also while playing. Every command: docs/CODING.md.
#include <lsd/game.hpp>

using namespace lsd::game;

struct Spinner {
    float speed = 90.0f;  // degrees a second; each object can change it in Details

    static void properties(Properties<Spinner>& p) { p.add("speed", &Spinner::speed); }

    void on_start(Self self) { log("%s: Spinner started", name(self)); }

    void on_update(Self self, float dt) {
        Vec3 r = rotation(self);
        r.z += speed * dt;
        set_rotation(self, r);
    }

    // Also available: on_stop(self), on_hit(self, other), on_overlap_begin(self, other),
    // on_overlap_end(self, other), on_event(self, name, other) for lsd.call / Call Event / UI actions.
};
LSD_BEHAVIOUR(Spinner)
