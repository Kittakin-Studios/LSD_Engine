#pragma once
// Smart Objects (Unreal's Smart Objects): usable spots (a bench, a workstation) with slots that one user
// each can claim. This is the claim table; the game finds the objects and walks the users there.
#include "lsd/foundation/math.h"

namespace lsd::ai {

constexpr u32 kMaxSmartClaims = 128;

struct SmartClaims {
    struct Claim {
        u32 object;  // the Smart Object (an entity handle value)
        u32 slot;
        u32 user;    // who holds it
        bool live;
    };
    Claim claims[kMaxSmartClaims] = {};

    bool taken(u32 object, u32 slot) const;
    i32 claim(u32 object, u32 slot, u32 user);  // its id, or -1 if taken (or the table is full)
    void release(i32 id);
    void release_user(u32 user);  // everything it holds
    const Claim* get(i32 id) const;  // nullptr once released
    void clear();
};

// The nearest free candidate within radius of `from` (candidates[i] is a slot's position, taken[i] its claim state); -1 if none.
i32 nearest_free(const Vec3* candidates, const bool* taken, u32 count, Vec3 from, f32 radius);

} // namespace lsd::ai
