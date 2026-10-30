#include "lsd/render/material.h"

#include <cstring>

namespace lsd::render {

Material resolve(const Material& parent, const MaterialInstance& i) {
    Material m = parent;
    if (i.override_base_color) m.base_color = i.base_color;
    if (i.override_base_color_texture) std::memcpy(m.base_color_texture, i.base_color_texture, sizeof m.base_color_texture);
    if (i.override_metallic) m.metallic = i.metallic;
    if (i.override_roughness) m.roughness = i.roughness;
    if (i.override_normal_texture) std::memcpy(m.normal_texture, i.normal_texture, sizeof m.normal_texture);
    if (i.override_emissive) {
        m.emissive = i.emissive;
        m.emissive_strength = i.emissive_strength;
    }
    return m;
}

} // namespace lsd::render
