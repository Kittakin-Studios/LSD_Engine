#pragma once
// Landscape paint weights (Landscape mode > Paint): four layer weights packed as bytes in a u32
// (layer 0 in the lowest), always summing to 255.
#include "lsd/foundation/types.h"

namespace lsd::editor {

// Moves `layer` toward full (erase: toward none) by `amount` (0..1); the other layers share what is
// left in the proportions they had (all of it to layer 0, or to layer 1 when painting layer 0, if
// they had none).
u32 paint_weight(u32 packed, int layer, f32 amount, bool erase);

} // namespace lsd::editor
