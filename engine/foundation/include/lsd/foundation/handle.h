#pragma once
// 32-bit generational handle: low bits index a slot, high bits are a generation counter.
// When a slot is reused its generation increases, so stale handles can be detected.
#include "lsd/foundation/types.h"

namespace lsd {

template <typename Tag, u32 IndexBits = 22>
struct Handle {
    static_assert(IndexBits > 0 && IndexBits < 32);
    static constexpr u32 kIndexMask = (1u << IndexBits) - 1u;
    static constexpr u32 kGenerationBits = 32u - IndexBits;
    static constexpr u32 kGenerationMask = (1u << kGenerationBits) - 1u;
    static constexpr u32 kInvalid = 0xFFFFFFFFu;

    u32 value = kInvalid;

    static constexpr Handle make(u32 index, u32 generation) {
        return Handle{(index & kIndexMask) | ((generation & kGenerationMask) << IndexBits)};
    }
    constexpr u32  index() const { return value & kIndexMask; }
    constexpr u32  generation() const { return (value >> IndexBits) & kGenerationMask; }
    constexpr bool valid() const { return value != kInvalid; }
    friend constexpr bool operator==(Handle a, Handle b) { return a.value == b.value; }
    friend constexpr bool operator!=(Handle a, Handle b) { return a.value != b.value; }
};

} // namespace lsd
