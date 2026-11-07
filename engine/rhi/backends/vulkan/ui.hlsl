// Game UI (RmlUi) for the Vulkan backend: 2D pixels from the viewport's top-left, premultiplied colour,
// as the D3D12 UI shader.
struct Push { float2 translation; float2 to_clip; };  // to_clip = 2 / viewport size
[[vk::push_constant]] Push pc;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D tex;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState samp;
struct VsIn { [[vk::location(0)]] float2 pos : POSITION; [[vk::location(1)]] float4 color : COLOR; [[vk::location(2)]] float2 uv : TEXCOORD; };
struct VsOut { float4 pos : SV_Position; float4 color : COLOR; float2 uv : TEXCOORD; };
VsOut vs_main(VsIn i) {
    VsOut o;
    const float2 p = (i.pos + pc.translation) * pc.to_clip;
    o.pos = float4(p.x - 1.0, 1.0 - p.y, 0.0, 1.0);
    o.color = i.color;
    o.uv = i.uv;
    return o;
}
float4 ps_main(VsOut i) : SV_Target { return i.color * tex.Sample(samp, i.uv); }
