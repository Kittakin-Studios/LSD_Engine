#pragma once
// Materials of a mesh file (Unreal's FBX import with Import Materials / Import Textures): each
// material's values and the texture maps it uses, so the editor can make a Material per slot; and
// the texture packing that turns separate maps into the engine's slots (ORM, colour + opacity).
#include "lsd/foundation/array.h"

namespace lsd::asset {

// A texture map a material uses: the path the file wrote (absolute or relative to the file), or
// the image's bytes when it is embedded in the file.
struct ImportedMap {
    char path[260] = "";           // as first written by the exporter (often absolute, on the artist's PC)
    char relative_path[260] = "";  // relative to where the file was saved ("" = none)
    const u8* embedded = nullptr;  // valid while the ImportedMaterials it came from lives
    usize embedded_size = 0;
    char embedded_name[96] = "";   // a file name for the embedded image ("Skin_Diffuse.png")
    u32 channel = 0;               // which channel holds the value (glTF metallic-roughness: G roughness, B metallic)
    bool exists() const { return path[0] || embedded; }
};

struct ImportedMaterial {
    char name[64] = "";
    f32 base_color[4] = {1, 1, 1, 1};
    f32 metallic = 0.0f;
    f32 roughness = 0.5f;
    f32 opacity = 1.0f;
    ImportedMap base_color_map;
    ImportedMap normal_map;
    ImportedMap metallic_map;
    ImportedMap roughness_map;
    ImportedMap occlusion_map;
    ImportedMap opacity_map;
    bool roughness_is_glossiness = false;  // the map / value is smoothness (1 = mirror): inverted when packed
};

// Every material of an FBX / glTF / GLB file, in the file's order (the names are the mesh's
// material slot names). Embedded images point into `owner` (kept alive while the materials are used).
struct ImportedMaterials {
    explicit ImportedMaterials(Allocator& allocator) : materials(allocator), blobs(allocator) {}
    ~ImportedMaterials();
    ImportedMaterials(const ImportedMaterials&) = delete;
    ImportedMaterials& operator=(const ImportedMaterials&) = delete;
    Array<ImportedMaterial> materials;
    Array<u8*> blobs;  // copies of embedded images
};
bool list_mesh_materials(const char* path, ImportedMaterials& out, char* error, usize error_capacity);

// An image file in memory as RGBA8 (any format stb_image reads), freed by the destructor.
struct Image {
    ~Image();
    u8* pixels = nullptr;
    u32 width = 0, height = 0;
};
bool load_image(const char* path, Image& out);
bool load_image(const u8* bytes, usize size, Image& out);

// One packed map's source: an image (null = the constant) and which channel of it to take.
struct PackSource {
    const Image* image = nullptr;
    u32 channel = 0;     // 0 R, 1 G, 2 B, 3 A
    u8 constant = 255;   // used without an image
    bool invert = false; // 1 - value (glossiness to roughness)
};
// Writes a PNG whose four channels come from `r`, `g`, `b`, `a` (sampled nearest at the size of the
// largest source image, so maps of different sizes line up by UV). False if nothing could be written.
bool pack_channels(const char* out_png, const PackSource& r, const PackSource& g, const PackSource& b, const PackSource& a);

} // namespace lsd::asset
