#include "lsd/world/matrix_cache.h"

#include <cstring>

namespace lsd::world {

void MatrixCache::begin_frame(const World& world) {
    const usize old = slots_.size();
    if (world.slot_count() > old) {
        slots_.resize(world.slot_count());
        std::memset(slots_.data() + old, 0, sizeof(Slot) * (slots_.size() - old));
    }
    if (++frame_ == 0) {  // wrapped: every slot is checked again
        for (Slot& s : slots_) s.frame = 0;
        frame_ = 1;
    }
}

const Mat4& MatrixCache::get(const World& world, Entity e, bool* moved) {
    static const Mat4 kNone{};
    const Transform* t = world.get<Transform>(e);
    if (!t || e.index() >= slots_.size()) {
        if (moved) *moved = true;
        return kNone;
    }
    Slot& s = slots_[e.index()];
    if (s.frame == frame_ && s.generation == e.generation()) {
        if (moved) *moved = s.moved;
        return s.world;
    }
    const Entity p = world.parent(e);
    bool parent_moved = false;
    const Mat4* parent_world = p.valid() ? &get(world, p, &parent_moved) : nullptr;  // slots_ is not resized here: the reference stays good
    const bool same = s.frame != 0 && s.generation == e.generation() && s.parent == p && !parent_moved && std::memcmp(&s.local, t, sizeof(Transform)) == 0;
    if (!same) {
        s.generation = e.generation();
        s.parent = p;
        s.local = *t;
        const Mat4 local = trs(t->position, t->rotation, t->scale);
        s.world = parent_world ? *parent_world * local : local;
    }
    s.moved = !same;
    s.frame = frame_;
    if (moved) *moved = s.moved;
    return s.world;
}

} // namespace lsd::world
