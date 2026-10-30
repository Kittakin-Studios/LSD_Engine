#pragma once
// Material (a data asset, "Material"): physically based surface settings, metal / roughness like
// Unreal's default lit material and glTF. Picked on a StaticMesh; textures are optional.
#include "lsd/foundation/math.h"
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

// One Scalar / Vector Parameter of a Material Graph set by an instance (scalars use X).
LSD_STRUCT()
struct MaterialParameter {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The parameter node's Name") char name[32] = "";
    LSD_PROPERTY(Edit, Tooltip = "Scalar parameters use X") Vec3 value = {1.0f, 1.0f, 1.0f};
};

// A Material with some values changed (Unreal's Material Instance): tick a value to override it;
// the rest follow the parent, also when the parent changes. With a Material Graph as the parent,
// its Scalar / Vector Parameters are set by name in Parameters (no recompile).
LSD_STRUCT(Category = "Rendering")
struct MaterialInstance {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Material") char parent[33] = "";
    LSD_PROPERTY(Edit) bool override_base_color = false;
    LSD_PROPERTY(Edit) LinearColor base_color = LinearColor::hex(0xFFFFFF);
    LSD_PROPERTY(Edit) bool override_base_color_texture = false;
    LSD_PROPERTY(Edit, Widget = "Asset:Texture") char base_color_texture[33] = "";
    LSD_PROPERTY(Edit) bool override_metallic = false;
    LSD_PROPERTY(Edit, Min = 0, Max = 1) f32 metallic = 0.0f;
    LSD_PROPERTY(Edit) bool override_roughness = false;
    LSD_PROPERTY(Edit, Min = 0, Max = 1) f32 roughness = 0.5f;
    LSD_PROPERTY(Edit) bool override_normal_texture = false;
    LSD_PROPERTY(Edit, Widget = "Asset:Texture") char normal_texture[33] = "";
    LSD_PROPERTY(Edit) bool override_emissive = false;
    LSD_PROPERTY(Edit) LinearColor emissive = LinearColor::hex(0x000000);
    LSD_PROPERTY(Edit, Min = 0, Max = 100) f32 emissive_strength = 1.0f;
    LSD_PROPERTY(Edit, Tooltip = "Material Graph parents: parameter values by name") List<MaterialParameter, 16> parameters;
};

// The instance's parent with its overrides applied.
Material resolve(const Material& parent, const MaterialInstance& instance);

} // namespace lsd::render
