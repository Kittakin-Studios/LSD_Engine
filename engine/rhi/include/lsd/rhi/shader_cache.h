#pragma once
// Compiled shaders kept on disk, so a start-up compiles a shader once and then reads it back
// (D3D12's lit shader takes FXC over a second per variant; Vulkan runs DXC per Material Graph stage).
// One file per shader, named by a hash of everything the compiler sees: the source, entry point,
// target, defines and a compiler id. A changed shader gets a new name; a damaged file fails its
// checksum and is compiled again. Files are written beside their name and renamed over it, so
// two editors starting at once never read half a file.
#include "lsd/foundation/types.h"

#include <string_view>
#include <vector>

namespace lsd::rhi {

// The key of one compiled shader: a hash over each part and its length.
u64 shader_cache_key(std::string_view source, std::string_view entry, std::string_view target, std::string_view defines, std::string_view compiler);

// project: DerivedData/Shaders (Material Graphs, post materials; packaged games ship it);
// else the user's cache for the built-in shaders, shared by every project
// (%LOCALAPPDATA%/LSD_Engine/ShaderCache, ~/.cache/lsd_engine/shaders).
const char* shader_cache_folder(bool project);

// The bytecode stored under the key in that folder; false when missing, damaged or another key's.
bool load_cached_shader(const char* folder, u64 key, std::vector<u8>& out);
// Writes it (creating the folder); false when it could not (the shader is then compiled next time too).
bool store_cached_shader(const char* folder, u64 key, const void* data, usize size);
// The project's folder first (what a package ships), then the user's.
bool find_cached_shader(u64 key, std::vector<u8>& out);

} // namespace lsd::rhi
