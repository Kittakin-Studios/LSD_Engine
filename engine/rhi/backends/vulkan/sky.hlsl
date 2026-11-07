// The sky for the Vulkan backend (rhi::SkyDraw): one triangle over the screen, as the D3D12 sky.
struct Push {
    float4x4 inv_view_proj;
    float4 zenith, horizon, ground;
    float4 sun_direction;  // xyz towards the sun, w = disc size (cosine of its angular radius)
    float4 sun_color;      // w = 0: no disc
    float4 camera;         // w = 1: into a scene target (range-compressed for the post pass)
};
[[vk::push_constant]] Push pc;

struct SkyOut { float4 pos : SV_Position; float3 dir : TEXCOORD0; };

SkyOut vs_main(uint id : SV_VertexID) {
    SkyOut o;
    const float2 p = float2((id << 1) & 2, id & 2) * 2.0 - 1.0;
    o.pos = float4(p, 1.0, 1.0);
    const float4 w = mul(pc.inv_view_proj, float4(p, 1.0, 1.0));
    o.dir = w.xyz / w.w - pc.camera.xyz;
    return o;
}

float3 to_srgb(float3 c) {
    c = saturate(c);
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

float4 ps_main(SkyOut i) : SV_Target {
    const float3 d = normalize(i.dir);
    float3 c = d.z >= 0.0 ? lerp(pc.horizon.rgb, pc.zenith.rgb, pow(saturate(d.z), 0.6)) : lerp(pc.horizon.rgb, pc.ground.rgb, saturate(-d.z * 4.0));
    if (pc.sun_color.w > 0.0) {
        const float s = dot(d, pc.sun_direction.xyz);
        c += pc.sun_color.rgb * (smoothstep(pc.sun_direction.w, pc.sun_direction.w + (1.0 - pc.sun_direction.w) * 0.3, s) + 0.25 * pow(saturate(s), 64.0) + 0.06 * pow(saturate(s), 6.0));
    }
    return float4(pc.camera.w > 0.5 ? sqrt(max(c, 0.0) / (1.0 + max(c, 0.0))) : to_srgb(c), 1.0);
}
