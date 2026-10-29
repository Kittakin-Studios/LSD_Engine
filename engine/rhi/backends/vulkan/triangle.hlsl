// Test triangle for the Vulkan backend, compiled to SPIR-V by DXC at build time.
// Mirrors the D3D12 inline shader; parameters arrive as push constants.
struct Params { float scale; float angle; float aspect; };
[[vk::push_constant]] Params params;

struct VsOut { float4 pos : SV_Position; float3 color : COLOR; };

VsOut vs_main(uint id : SV_VertexID) {
    const float2 pos[3]   = { float2(0.0, 0.6), float2(0.55, -0.5), float2(-0.55, -0.5) };
    const float3 color[3] = { float3(1, 0.2, 0.2), float3(0.2, 1, 0.2), float3(0.2, 0.4, 1) };
    float s, c;
    sincos(params.angle, s, c);
    const float2 p = mul(float2x2(c, -s, s, c), pos[id] * params.scale);
    VsOut o;
    o.pos = float4(p.x / params.aspect, p.y, 0, 1);
    o.color = color[id];
    return o;
}

float4 ps_main(VsOut i) : SV_Target { return float4(i.color, 1); }
