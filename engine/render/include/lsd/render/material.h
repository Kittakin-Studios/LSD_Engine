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
    LSD_PROPERTY(Edit, Widget = "Asset:Texture", Tooltip = "An image for the colour (multiplied by Base Color)") char base_color_texture[33] = "";
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "0 = non-metal (wood, plastic, stone), 1 = metal") f32 metallic = 0.0f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "0 = mirror smooth, 1 = fully rough") f32 roughness = 0.5f;
    LSD_PROPERTY(Edit, Widget = "Asset:Texture", Tooltip = "Tangent-space normal map (OpenGL style, green up)") char normal_texture[33] = "";
    LSD_PROPERTY(Edit, Widget = "Asset:Texture", Tooltip = "Packed: R ambient occlusion, G roughness, B metallic (glTF / Substance ORM); G and B multiply the values above")
    char orm_texture[33] = "";
    LSD_PROPERTY(Edit, Widget = "Asset:Texture", Tooltip = "Roughness map (one channel, white = rough): multiplies Roughness. Overrides the ORM Texture's green channel; the maps are packed together when the material loads")
    char roughness_texture[33] = "";
    LSD_PROPERTY(Edit, Widget = "Asset:Texture", Tooltip = "Metallic map (one channel, white = metal): multiplies Metallic. Overrides the ORM Texture's blue channel")
    char metallic_texture[33] = "";
    LSD_PROPERTY(Edit, Widget = "Asset:Texture", Tooltip = "Ambient occlusion map (one channel, white = open): darkens ambient light in crevices. Overrides the ORM Texture's red channel")
    char ambient_occlusion_texture[33] = "";
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "Strength of the highlight on non-metals (Unreal's Specular): 0.5 is the usual 4% reflectivity, 1 doubles it, 0 removes it. Metals ignore it")
    f32 specular = 0.5f;
    LSD_PROPERTY(Edit, Widget = "Asset:Texture", Tooltip = "Specular map (one channel, white = full): multiplies Specular")
    char specular_texture[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Glow colour (not lit by anything)") LinearColor emissive = LinearColor::hex(0x000000);
    LSD_PROPERTY(Edit, Min = 0, Max = 100, Tooltip = "How bright the glow is (above 1 blooms)") f32 emissive_strength = 1.0f;
    LSD_PROPERTY(Edit, Widget = "Asset:Texture", Tooltip = "Where it glows: multiplies Emissive (set Emissive to white to use the map's own colours)")
    char emissive_texture[33] = "";
    LSD_PROPERTY(Edit, Category = "Blending", Widget = "Choice:Opaque,Masked,Translucent,Additive",
                 Tooltip = "Masked: cut-outs from the base colour alpha (leaves, fences). Translucent: see-through (glass, water). Additive: adds light (fire, glows)")
    u32 blend_mode = 0;
    LSD_PROPERTY(Edit, Category = "Blending", Min = 0, Max = 1, Tooltip = "Times the base colour texture's alpha (Translucent, Additive, Masked)") f32 opacity = 1.0f;
    LSD_PROPERTY(Edit, Category = "Blending", Min = 0, Max = 1, Tooltip = "Masked: alpha below this is not drawn") f32 opacity_mask_clip = 0.333f;
    LSD_PROPERTY(Edit, Widget = "Choice:Default Lit,Hair",
                 Tooltip = "Hair (Unreal's Hair shading model, for hair cards): two highlights running along the strand (lay the strands along the texture's V), soft light through the hair; Roughness sets how sharp they are. Use with Masked and an alpha texture")
    u32 shading_model = 0;
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
    LSD_PROPERTY(Edit, Widget = "Asset:Material", Tooltip = "The Material this one starts from; it follows the parent's edits except where overridden") char parent[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Use this instance's Base Color instead of the parent's") bool override_base_color = false;
    LSD_PROPERTY(Edit, Tooltip = "The colour (when overridden)") LinearColor base_color = LinearColor::hex(0xFFFFFF);
    LSD_PROPERTY(Edit, Tooltip = "Use this instance's Base Color Texture instead of the parent's") bool override_base_color_texture = false;
    LSD_PROPERTY(Edit, Widget = "Asset:Texture", Tooltip = "The colour image (when overridden)") char base_color_texture[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Use this instance's Metallic instead of the parent's") bool override_metallic = false;
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "0 = paint, plastic, wood; 1 = bare metal") f32 metallic = 0.0f;
    LSD_PROPERTY(Edit, Tooltip = "Use this instance's Roughness instead of the parent's") bool override_roughness = false;
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "0 = mirror-smooth, 1 = matte") f32 roughness = 0.5f;
    LSD_PROPERTY(Edit, Tooltip = "Use this instance's Normal Texture instead of the parent's") bool override_normal_texture = false;
    LSD_PROPERTY(Edit, Widget = "Asset:Texture", Tooltip = "Surface bumps (a normal map)") char normal_texture[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Use this instance's glow instead of the parent's") bool override_emissive = false;
    LSD_PROPERTY(Edit, Tooltip = "Glow colour (black = none)") LinearColor emissive = LinearColor::hex(0x000000);
    LSD_PROPERTY(Edit, Min = 0, Max = 100, Tooltip = "How bright the glow is (above 1 blooms)") f32 emissive_strength = 1.0f;
    LSD_PROPERTY(Edit, Tooltip = "Material Graph parents: parameter values by name") List<MaterialParameter, 16> parameters;
};

// The instance's parent with its overrides applied.
Material resolve(const Material& parent, const MaterialInstance& instance);

} // namespace lsd::render
