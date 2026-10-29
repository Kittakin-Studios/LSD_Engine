#pragma once
// 64-bit hashed identifiers for resource names, types and lookups.
// Hashing is constexpr, so IDs written in code are computed at compile time: LSD_ID("unit").
#include "lsd/foundation/types.h"
#include <string_view>

namespace lsd {

// FNV-1a 64-bit. Simple and constexpr; can be swapped for a faster hash later
// as long as the data compiler and runtime agree.
constexpr u64 hash64(std::string_view text) {
    u64 h = 0xcbf29ce484222325ull;
    for (char c : text) {
        h ^= static_cast<u8>(c);
        h *= 0x100000001b3ull;
    }
    return h;
}

struct HashId {
    u64 value = 0;
    constexpr HashId() = default;
    constexpr explicit HashId(u64 v) : value(v) {}
    constexpr explicit HashId(std::string_view text) : value(hash64(text)) {}
    constexpr bool valid() const { return value != 0; }
    friend constexpr bool operator==(HashId a, HashId b) { return a.value == b.value; }
    friend constexpr bool operator!=(HashId a, HashId b) { return a.value != b.value; }
};

} // namespace lsd

#define LSD_ID(literal) (::lsd::HashId{std::string_view{literal}})
