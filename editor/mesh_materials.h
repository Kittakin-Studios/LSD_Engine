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

// The same in steps, one material's textures per step, so the editor keeps drawing (and shows a progress bar):
// begin (null when there is nothing to do: `status` says why), then step until it returns true (`report` and
// `status` filled, the import freed). progress() is 0..1 with the material being worked on.
struct MaterialImport;
MaterialImport* begin_mesh_materials(asset::AssetRegistry& registry, const asset::AssetRecord& mesh, const char* original_folder, bool only_with_textures,
                                     char* status, usize status_capacity);
bool step_mesh_materials(MaterialImport* import, MaterialImportReport& report, char* status, usize status_capacity);
f32 mesh_materials_progress(const MaterialImport* import, char* label, usize label_capacity);

// The editor's queue (the Content Browser menu, asset.ImportMaterials, imports): queue one, and once a frame
// update_mesh_materials does a step of the first; `progress` is 0..1 while one runs (with `label`), -1 when idle.
void queue_mesh_materials(const asset::AssetRecord& mesh, const char* original_folder, bool only_with_textures);
void update_mesh_materials(asset::AssetRegistry& registry, char* status, usize status_capacity, f32& progress, char* label, usize label_capacity);

// Create Material on textures: the selected textures and the others in their folders are grouped by name
// (T_Bike_Body01_BaseColor / _Normal / _Roughness / _Metallic / _AmbientOcclusion / _Opacity / _Emissive / _Specular...
// asset::group_map_names), and one Material per set is made next to them (existing ones are kept). Roughness, metallic and
// occlusion maps go to the Material's own inputs; opacity is packed into the colour's alpha (Masked), glTF metallic-roughness
// and smoothness into an ORM texture.
struct TextureMaterialReport {
    u32 created = 0, reused = 0;
    std::string last_path;  // the last material made or found (to select it)
};
bool create_materials_from_textures(asset::AssetRegistry& registry, const std::vector<asset::AssetRecord>& selected, TextureMaterialReport& report, char* status,
                                    usize status_capacity);

} // namespace lsd::editor
