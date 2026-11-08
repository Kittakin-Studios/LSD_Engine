#include "landscape_paint.h"

#include <algorithm>
#include <cmath>

namespace lsd::editor {

u32 paint_weight(u32 packed, int layer, f32 amount, bool erase) {
    if (layer < 0 || layer > 3) return packed;
    f32 w[4];
    for (int k = 0; k < 4; ++k) w[k] = static_cast<f32>((packed >> (8 * k)) & 0xFFu) / 255.0f;
    const f32 before = w[layer];
    w[layer] += ((erase ? 0.0f : 1.0f) - w[layer]) * std::clamp(amount, 0.0f, 1.0f);
    const f32 rest = 1.0f - before, now_rest = 1.0f - w[layer];
    for (int o = 0; o < 4; ++o) {
        if (o == layer) continue;
        w[o] = rest > 1e-4f ? w[o] * now_rest / rest : (o == (layer == 0 ? 1 : 0) ? now_rest : 0.0f);
    }
    // To bytes. The side the stroke moves away from rounds down, so painting always reaches a full
    // layer and erasing an empty one (rounding to nearest would keep a 1/255 crumb forever).
    i32 b[4], sum = 0, taker = -1;
    for (int o = 0; o < 4; ++o) {
        const f32 v = std::clamp(w[o], 0.0f, 1.0f) * 255.0f;
        b[o] = static_cast<i32>((o == layer) == erase ? std::floor(v) : std::round(v));
        sum += b[o];
    }
    if (!erase) {
        taker = layer;
    } else {
        for (int o = 0; o < 4; ++o) {
            if (o != layer && (taker < 0 || b[o] > b[taker])) taker = o;
        }
    }
    b[taker] += 255 - sum;  // the difference: the painted layer, or when erasing the largest other one
    if (b[taker] < 0) {     // rounded up too far: take it back from the largest
        int largest = 0;
        for (int o = 1; o < 4; ++o) largest = b[o] > b[largest] ? o : largest;
        b[largest] += b[taker];
        b[taker] = 0;
    }
    u32 out = 0;
    for (int o = 0; o < 4; ++o) out |= static_cast<u32>(std::clamp(b[o], 0, 255)) << (8 * o);
    return out;
}

} // namespace lsd::editor
