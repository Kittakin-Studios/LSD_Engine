#pragma once
// Material (a data asset, "Material"): physically based surface settings, metal / roughness like
// Unreal's default lit material and glTF. Picked on a StaticMesh; textures are optional.
#include "lsd/reflect/color.h"

namespace lsd::render {

LSD_STRUCT(Category = "Rendering")
struct Material {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Multiplies the base colour texture") LinearColor base_color = LinearColor::hex(0xFFFFFF);
    LSD_PROPERTY(Edit, Widget = "Asset:Texture") char base_color_texture[33] = "";
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "0 = non-metal (wood, plastic, stone), 1 = metal") f32 metallic = 0.0f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "0 = mirror smooth, 1 = fully rough") f32 roughness = 0.5f;
    LSD_PROPERTY(Edit, Widget = "Asset:Texture", Tooltip = "Tangent-space normal map (OpenGL style, green up)") char normal_texture[33] = "";
    LSD_PROPERTY(Edit, Widget = "Asset:Texture", Tooltip = "Packed: R ambient occlusion, G roughness, B metallic (glTF / Substance ORM); G and B multiply the values above")
    char orm_texture[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Glow colour (not lit by anything)") LinearColor emissive = LinearColor::hex(0x000000);
    LSD_PROPERTY(Edit, Min = 0, Max = 100) f32 emissive_strength = 1.0f;
};

} // namespace lsd::render
