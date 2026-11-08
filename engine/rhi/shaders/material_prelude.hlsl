// Material Graph code (lsd/script/material_graph.h) goes into shaders/mesh.hlsl at its MATERIAL marker after
// these definitions; D3D12 compiles the result at runtime, Vulkan through the Vulkan SDK's DXC.
#define LSD_MATERIAL 1
struct MaterialIn { float3 uv; float3 wpos; float3 normal; float3 view; float time; };
struct Surface { float3 base_color; float metallic; float roughness; float3 emissive; float3 normal; float ao; float opacity; };
float3 lsd_v3(float x) { return float3(x, x, x); }
float4 lsd_tex0(float3 uv) { return base_tex.Sample(samp, uv.xy); }
float4 lsd_tex1(float3 uv) { return normal_tex.Sample(samp, uv.xy); }
float4 lsd_tex2(float3 uv) { return orm_tex.Sample(samp, uv.xy); }
float4 lsd_tex3(float3 uv) { return emissive_tex.Sample(samp, uv.xy); }  // the emissive slot (a graph has no emissive map of its own)
VK_BIND(2, 2) cbuffer MaterialParams : register(b3) { float4 lsd_params[16]; };
