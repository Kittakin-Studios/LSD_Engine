// Lit meshes for the Vulkan backend: base colour (times its texture, Masked cut-outs) lit by the sun
// (Lambert, its cascaded shadow map) and the sky / ground ambient, as the D3D12 shader's diffuse terms.
// Per draw: push constants (192 bytes; every desktop GPU offers 256). Per view: the Scene block
// (set_lights) in the frame's ring (set 2); the sun's shadow atlas in set 3.
// shortcut: a subset of the D3D12 lit shader (no specular, local lights or their shadows); share its HLSL as the port continues
struct Push {
    float4x4 mvp;
    float4 w0, w1, w2;   // world matrix rows (world positions for shadows)
    float4 n0, n1, n2;   // normal matrix columns; n0.w = 1: unlit (colour only), n1.w the Masked cutoff (0 = off)
    float4 color;        // base colour, linear
    float4 camera;       // xyz (picks the shadow cascade)
};
struct Scene {
    float4 sun_dir;      // where the sun's light travels (xyz), exposure (w)
    float4 sun_color;    // its colour times intensity (xyz), w = 1 when there is a sun
    float4 sky, ground;  // ambient from above / below
    float4 shadow_info;  // cascades, the sun casts (>= 0), texel size, normal offset (LightSetup)
    float4 shadow_split; // far distance of each cascade
    float4x4 shadow_vp[4];
    float4 shadow_tile[4];
};
[[vk::push_constant]] Push pc;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D base_tex;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState base_samp;
[[vk::binding(0, 2)]] StructuredBuffer<Scene> scene;
[[vk::combinedImageSampler]] [[vk::binding(0, 3)]] Texture2D<float> shadow_tex;
[[vk::combinedImageSampler]] [[vk::binding(0, 3)]] SamplerComparisonState shadow_samp;
#if defined(SKINNED) || defined(INSTANCED)
// SKINNED: 3 rows (float4) per joint, its skinning matrix; INSTANCED: 3 rows per copy, its world
// matrix (mvp is then the view-projection). From the frame's ring buffer at this draw's offset.
[[vk::binding(0, 1)]] StructuredBuffer<float4> rows;
#endif

struct VsIn {
    [[vk::location(0)]] float3 pos : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
#ifdef SKINNED
    [[vk::location(3)]] uint4 joints : BLENDINDICES;
    [[vk::location(4)]] float4 weights : BLENDWEIGHT;
#endif
#ifdef INSTANCED
    uint instance : SV_InstanceID;
#endif
};
struct VsOut { float4 pos : SV_Position; float3 normal : NORMAL; float2 uv : TEXCOORD0; float3 wpos : TEXCOORD1; };

VsOut vs_main(VsIn i) {
    VsOut o;
    float3 position = i.pos, normal = i.normal;
#ifdef INSTANCED
    const float4 r0 = rows[i.instance * 3], r1 = rows[i.instance * 3 + 1], r2 = rows[i.instance * 3 + 2];
    const float4 local = float4(position, 1);
    o.wpos = float3(dot(r0, local), dot(r1, local), dot(r2, local));
    o.pos = mul(pc.mvp, float4(o.wpos, 1));
    const float3 c0 = float3(r0.x, r1.x, r2.x), c1 = float3(r0.y, r1.y, r2.y), c2 = float3(r0.z, r1.z, r2.z);
    o.normal = cross(c1, c2) * normal.x + cross(c2, c0) * normal.y + cross(c0, c1) * normal.z;  // cofactor: right under any scale
    o.uv = i.uv;
    return o;
#else
#ifdef SKINNED
    float4 r0 = 0, r1 = 0, r2 = 0;
    [unroll] for (int k = 0; k < 4; ++k) {
        r0 += rows[i.joints[k] * 3] * i.weights[k];
        r1 += rows[i.joints[k] * 3 + 1] * i.weights[k];
        r2 += rows[i.joints[k] * 3 + 2] * i.weights[k];
    }
    position = float3(dot(r0, float4(position, 1)), dot(r1, float4(position, 1)), dot(r2, float4(position, 1)));
    normal = float3(dot(r0.xyz, normal), dot(r1.xyz, normal), dot(r2.xyz, normal));
#endif
    o.pos = mul(pc.mvp, float4(position, 1));
    o.wpos = float3(dot(pc.w0, float4(position, 1)), dot(pc.w1, float4(position, 1)), dot(pc.w2, float4(position, 1)));
    o.normal = pc.n0.xyz * normal.x + pc.n1.xyz * normal.y + pc.n2.xyz * normal.z;
    o.uv = i.uv;
    return o;
#endif
}

float3 to_srgb(float3 c) {
    c = saturate(c);
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

// Sun shadow: the cascade by distance from the camera, pushed out along the normal against acne,
// 3x3 filtered compare taps (as D3D12's sun_shadow).
float sun_shadow(Scene sc, float3 wpos, float3 n) {
    const uint count = (uint)sc.shadow_info.x;
    if (count == 0 || sc.shadow_info.y < 0.0) return 1.0;
    const float dist = length(wpos - pc.camera.xyz);
    uint c = 0;
    while (c + 1 < count && dist > sc.shadow_split[c]) ++c;
    if (dist > sc.shadow_split[count - 1]) return 1.0;
    const float4 s = mul(sc.shadow_vp[c], float4(wpos + n * sc.shadow_info.w * (1.0 + c), 1.0));
    float2 uv = float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || s.z > 1.0) return 1.0;
    uv = sc.shadow_tile[c].xy + uv * sc.shadow_tile[c].z;
    const float2 lo = sc.shadow_tile[c].xy + sc.shadow_info.z, hi = sc.shadow_tile[c].xy + sc.shadow_tile[c].z - sc.shadow_info.z;
    float lit = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) lit += shadow_tex.SampleCmpLevelZero(shadow_samp, clamp(uv + float2(x, y) * sc.shadow_info.z, lo, hi), s.z);
    return lit / 9.0;
}

float4 ps_main(VsOut i, bool front : SV_IsFrontFace) : SV_Target {
    const float4 texel = pc.color * base_tex.Sample(base_samp, i.uv);
    if (pc.n1.w > 0.0 && texel.a < pc.n1.w) discard;  // Masked
    const float3 base = texel.rgb;
    if (pc.n0.w > 0.5) return float4(to_srgb(base), texel.a);
    const Scene sc = scene[0];
    const float3 n = normalize(i.normal) * (front ? 1.0 : -1.0);
    const float3 ambient = lerp(sc.ground.rgb, sc.sky.rgb, n.z * 0.5 + 0.5);
    float3 lit = base * ambient;
    const float ndotl = saturate(dot(n, -sc.sun_dir.xyz));
    if (sc.sun_color.w > 0.5 && ndotl > 0.0) lit += base / 3.14159265 * sc.sun_color.rgb * ndotl * sun_shadow(sc, i.wpos, n);
    return float4(to_srgb(lit * sc.sun_dir.w), texel.a);
}
