#pragma once
// Material Graphs (.lsdmatgraph), Unreal's Material Editor: material nodes (graph.h, GraphKind::Material)
// wired into a Material Output, compiled to shader code. The code is one function in the syntax
// both backends accept (HLSL spelling; the OpenGL backend maps float3 / lerp / frac / saturate):
//
//   Surface material_main(MaterialIn mi)   // mi: uv, wpos, normal, view (all float3), time
//
// Every value is a float3. The RHI wraps it into the lit-mesh shader (rhi::Device::create_material_shader),
// which defines MaterialIn, Surface, lsd_v3(x) and lsd_tex0..2(uv) for the textures listed here.
#include "lsd/script/graph.h"

namespace lsd::script {

constexpr char kMaterialGraphExtension[] = ".lsdmatgraph";
constexpr u32 kMaxMaterialTextures = 3;  // the lit-mesh shader's three texture slots

struct MaterialTextureSlot {
    char name[64];  // texture asset name, as typed on the node
    bool srgb;      // Color: sRGB; Normal and Data: linear
};

struct MaterialCode {
    explicit MaterialCode(Allocator& allocator) : code(allocator) {}
    Array<char> code;  // NUL-terminated
    MaterialTextureSlot textures[kMaxMaterialTextures] = {};
    u32 texture_count = 0;
};

// False with a message naming the problem: no (or two) Material Output, a loop, a Texture Sample
// without a texture, more than kMaxMaterialTextures textures.
bool compile_material_graph(const Graph& graph, MaterialCode& out, char* error, usize error_capacity);

} // namespace lsd::script
