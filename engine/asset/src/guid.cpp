#include "lsd/asset/guid.h"

#include <random>

namespace lsd::asset {

Guid new_guid() {
    static std::random_device device;  // OS entropy; called rarely (once per new asset)
    std::mt19937_64 rng((static_cast<u64>(device()) << 32) ^ device());
    Guid g{rng(), rng()};
    if (!g.valid()) g.lo = 1;
    return g;
}

void to_string(Guid guid, char out[33]) {
    constexpr char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) out[i] = kHex[(guid.hi >> (60 - 4 * i)) & 0xF];
    for (int i = 0; i < 16; ++i) out[16 + i] = kHex[(guid.lo >> (60 - 4 * i)) & 0xF];
    out[32] = '\0';
}

bool parse_guid(const char* text, Guid& out) {
    Guid g;
    for (int i = 0; i < 32; ++i) {
        const char c = text[i];
        u64 digit = 0;
        if (c >= '0' && c <= '9') digit = static_cast<u64>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<u64>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = static_cast<u64>(c - 'A' + 10);
        else return false;
        u64& half = i < 16 ? g.hi : g.lo;
        half = (half << 4) | digit;
    }
    if (text[32] != '\0' || !g.valid()) return false;
    out = g;
    return true;
}

} // namespace lsd::asset
