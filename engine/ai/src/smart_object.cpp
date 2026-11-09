#include "lsd/ai/smart_object.h"

namespace lsd::ai {

bool SmartClaims::taken(u32 object, u32 slot) const {
    for (const Claim& c : claims) {
        if (c.live && c.object == object && c.slot == slot) return true;
    }
    return false;
}

i32 SmartClaims::claim(u32 object, u32 slot, u32 user) {
    if (taken(object, slot)) return -1;
    for (u32 i = 0; i < kMaxSmartClaims; ++i) {
        if (claims[i].live) continue;
        claims[i] = {object, slot, user, true};
        return static_cast<i32>(i);
    }
    return -1;
}

void SmartClaims::release(i32 id) {
    if (id >= 0 && static_cast<u32>(id) < kMaxSmartClaims) claims[id].live = false;
}

void SmartClaims::release_user(u32 user) {
    for (Claim& c : claims) {
        if (c.user == user) c.live = false;
    }
}

const SmartClaims::Claim* SmartClaims::get(i32 id) const {
    return id >= 0 && static_cast<u32>(id) < kMaxSmartClaims && claims[id].live ? &claims[id] : nullptr;
}

void SmartClaims::clear() {
    for (Claim& c : claims) c.live = false;
}

i32 nearest_free(const Vec3* candidates, const bool* taken, u32 count, Vec3 from, f32 radius) {
    i32 best = -1;
    f32 best_d = radius * radius;
    for (u32 i = 0; i < count; ++i) {
        const Vec3 d = candidates[i] - from;
        const f32 dd = dot(d, d);
        if (taken[i] || dd > best_d) continue;
        best_d = dd;
        best = static_cast<i32>(i);
    }
    return best;
}

} // namespace lsd::ai
