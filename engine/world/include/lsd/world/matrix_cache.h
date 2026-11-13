#pragma once
// World matrices kept between frames: an entity's is worked out again only when its Transform, its parent or its
// parent's matrix changed, so a level of thousands of still objects costs a compare each instead of a sin / cos
// matrix per object and per ancestor every time a pass asks (World::world_matrix walks the whole chain).
#include "lsd/foundation/array.h"
#include "lsd/world/world.h"

namespace lsd::world {

class MatrixCache {
public:
    explicit MatrixCache(Allocator& allocator) : slots_(allocator) {}
    // Starts a frame: each entity is checked at most once per frame after this.
    void begin_frame(const World& world);
    // The entity's world matrix (as World::world_matrix). `moved`: it differs from the last frame's (or is new).
    const Mat4& get(const World& world, Entity entity, bool* moved = nullptr);

private:
    struct Slot {
        u32 generation;
        u32 frame;  // the frame it was last checked in (0: never)
        Entity parent;
        Transform local;
        Mat4 world;
        bool moved;
    };
    Array<Slot> slots_;
    u32 frame_ = 0;
};

} // namespace lsd::world
