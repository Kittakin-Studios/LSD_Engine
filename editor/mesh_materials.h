#pragma once
// Import Materials and Textures for a mesh file (Unreal's FBX import options of the same names):
// every texture map its materials use is found (where the file says, next to the file, its .fbm
// folder, then anywhere in Content by name), copied into <mesh folder>/Textures, and one Material
// per file material is made in <mesh folder>/Materials (existing ones are kept). Separate maps are
// packed into the engine's slots: metallic / roughness / occlusion into one ORM texture, an opacity
// map into the colour's alpha (the material becomes Masked). The mesh's Material Slots point at them.
#include "lsd/asset/asset_registry.h"

#include <string>
#include <utility>
#include <vector>

namespace lsd::editor {

struct MaterialImportReport {
    u32 created = 0, reused = 0, textures = 0, missing = 0;
    std::vector<std::pair<std::string, std::string>> materials;  // file material name -> Material GUID
};

// `original_folder`: where the mesh file was imported from ("" = unknown), searched first for
// relative texture paths. `only_with_textures`: do nothing unless at least one texture is found
// (automatic import: animation-only FBX files bring no materials). Rescans `registry`.
bool import_mesh_materials(asset::AssetRegistry& registry, const asset::AssetRecord& mesh, const char* original_folder, bool only_with_textures,
                           MaterialImportReport& report, char* status, usize status_capacity);

} // namespace lsd::editor
