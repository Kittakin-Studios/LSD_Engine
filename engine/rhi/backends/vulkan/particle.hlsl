// Particle sprites for the Vulkan backend (camera-facing quads built on the CPU), as the D3D12 particle shader.
struct Push { float4x4 view_proj; float4 mode; };  // mode.x = 1: into a scene target (post pass)
[[vk::push_constant]] Push pc;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D sprite;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState samp;
struct VsIn { [[vk::location(0)]] float3 pos : POSITION; [[vk::location(1)]] float4 color : COLOR; [[vk::location(2)]] float2 uv : TEXCOORD; };
struct VsOut { float4 pos : SV_Position; float4 color : COLOR; float2 uv : TEXCOORD; };
VsOut vs_main(VsIn i) { VsOut o; o.pos = mul(pc.view_proj, float4(i.pos, 1)); o.color = i.color; o.uv = i.uv; return o; }
float3 to_srgb(float3 c) { c = saturate(c); return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055); }
float4 ps_main(VsOut i) : SV_Target {
    const float4 c = i.color * sprite.Sample(samp, i.uv);
    const float3 lin = max(c.rgb, 0.0);
    return float4(pc.mode.x > 0.5 ? sqrt(lin / (1.0 + lin)) : to_srgb(c.rgb), saturate(c.a));
}
