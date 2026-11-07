// Lines for the Vulkan backend (grid, gizmos, debug draw): as the D3D12 line shader.
// Compiled to SPIR-V by DXC at build time.
struct Push { float4x4 mvp; };
[[vk::push_constant]] Push pc;

struct VsIn { [[vk::location(0)]] float3 pos : POSITION; [[vk::location(1)]] float4 color : COLOR; };
struct VsOut { float4 pos : SV_Position; float4 color : COLOR; };

VsOut vs_main(VsIn i) {
    VsOut o;
    o.pos = mul(pc.mvp, float4(i.pos, 1));
    o.color = i.color;
    return o;
}

float4 ps_main(VsOut i) : SV_Target { return i.color; }
