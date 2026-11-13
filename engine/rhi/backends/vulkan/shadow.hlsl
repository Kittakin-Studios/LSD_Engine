// Shadow casters for the Vulkan backend: depth only into a shadow map tile, as the D3D12 shadow shader.
// SKINNED / INSTANCED read joint or copy matrices from the frame's ring (set 1); ps_main is the Masked
// cut-out (base colour alpha times cut.y below cut.x casts none), used only for Masked casters.
struct Push {
    float4x4 mvp;  // the light's view-projection times the world (INSTANCED: the light's view-projection)
    float4 cut;    // x = cutoff, y = the tint's alpha, zw = the Tiling
};
[[vk::push_constant]] Push pc;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D base_tex;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState base_samp;
#if defined(SKINNED) || defined(INSTANCED)
[[vk::binding(0, 1)]] StructuredBuffer<float4> rows;
#endif

struct VsIn {
    [[vk::location(0)]] float3 pos : POSITION;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
#ifdef SKINNED
    [[vk::location(3)]] uint4 joints : BLENDINDICES;
    [[vk::location(4)]] float4 weights : BLENDWEIGHT;
#endif
#ifdef INSTANCED
    uint instance : SV_InstanceID;
#endif
};
struct VsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VsOut vs_main(VsIn i) {
    VsOut o;
    float4 p = float4(i.pos, 1);
#ifdef INSTANCED
    p = float4(dot(rows[i.instance * 3], p), dot(rows[i.instance * 3 + 1], p), dot(rows[i.instance * 3 + 2], p), 1);
#endif
#ifdef SKINNED
    float4 r0 = 0, r1 = 0, r2 = 0;
    [unroll] for (int k = 0; k < 4; ++k) {
        r0 += rows[i.joints[k] * 3] * i.weights[k];
        r1 += rows[i.joints[k] * 3 + 1] * i.weights[k];
        r2 += rows[i.joints[k] * 3 + 2] * i.weights[k];
    }
    p = float4(dot(r0, p), dot(r1, p), dot(r2, p), 1);
#endif
    o.pos = mul(pc.mvp, p);
    o.uv = i.uv;
    return o;
}

void ps_main(VsOut i) {
    if (base_tex.Sample(base_samp, i.uv * pc.cut.zw).a * pc.cut.y < pc.cut.x) discard;
}
