#pragma once
// 128-bit asset identifier. Written as 32 hex digits in .meta files; never changes for an asset,
// so references survive renames and moves.
#include "lsd/foundation/types.h"

namespace lsd::asset {

struct Guid {
    u64 hi = 0;
    u64 lo = 0;
    bool valid() const { return hi != 0 || lo != 0; }
    friend bool operator==(Guid a, Guid b) { return a.hi == b.hi && a.lo == b.lo; }
    friend bool operator!=(Guid a, Guid b) { return !(a == b); }
};

Guid new_guid();                                // random (version 4 style)
void to_string(Guid guid, char out[33]);        // 32 lowercase hex digits + NUL
bool parse_guid(const char* text, Guid& out);   // false unless exactly 32 hex digits

} // namespace lsd::asset
