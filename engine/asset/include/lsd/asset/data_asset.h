#pragma once
// Data assets (.lsdasset): one instance of a registered reflected type, saved as JSON text and
// edited in the Details panel, like Unreal's Data Assets. Input Actions and Mapping Contexts are
// data assets; game code registers its own types the same way.
//
//   {
//     "type": "InputAction",
//     "data": {...}
//   }
#include "lsd/reflect/reflect.h"

namespace lsd::asset {

constexpr char kDataAssetExtension[] = ".lsdasset";

// Types that can be created as data assets (Content Browser > New Data Asset). Registering twice is harmless.
void register_data_type(const reflect::TypeInfo& type);
const reflect::TypeInfo* find_data_type(const char* name);  // by TypeInfo name
const reflect::TypeInfo* const* data_types(u32& count);

bool save_data_asset(const char* path, const reflect::TypeInfo& type, const void* object);
// Loads into `object` only if the file holds `type`; on any failure `object` is unchanged.
bool load_data_asset(const char* path, const reflect::TypeInfo& type, void* object);
// Reads just the "type" name (for the registry and pickers). False if the file is not a data asset.
bool read_data_asset_type(const char* path, char* out, usize capacity);

template <typename T>
bool load_data_asset(const char* path, T& object) { return load_data_asset(path, reflect::type_of<T>(), &object); }

} // namespace lsd::asset
