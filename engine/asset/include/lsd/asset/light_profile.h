#pragma once
// IES light profiles (Unreal's IES Texture on a light): an IESNA LM-63 photometric file's candela by vertical angle,
// averaged around the axis and normalised to its brightest, as kLightProfileSamples values from the light's axis
// (0 degrees, straight ahead) to straight behind it (180).
// shortcut: averaged around the axis (no left / right asymmetry), 16 samples (sharp cut-offs soften); a texture per light for more
#include "lsd/foundation/types.h"

namespace lsd::asset {

constexpr u32 kLightProfileSamples = 16;

// False if `text` is not an LM-63 file (no TILT line, too few numbers).
bool parse_ies(const char* text, f32 out[kLightProfileSamples]);
// The profile at the angle whose cosine from the axis is `cos_axis`, linear between samples (what the lit shader does).
f32 light_profile_at(const f32 profile[kLightProfileSamples], f32 cos_axis);

} // namespace lsd::asset
