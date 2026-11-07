// Lit meshes for the Vulkan backend: base colour (times its texture, Masked cut-outs) lit by every
// light (Lambert; the sun's cascaded shadows, spot / point shadow tiles) and the sky / ground ambient,
// as the D3D12 shader's diffuse terms. Per draw: push constants (192 bytes; every desktop GPU offers
// 256). Per view: the Scene block (the start of LightSetup) in the frame's ring (set 2); the sun's and
// the local lights' shadow atlases in set 3.
// shortcut: a subset of the D3D12 lit shader (no specular, probes, fog or decals); share its HLSL as the port continues
struct Push {
    float4x4 mvp;
    float4 w0, w1, w2;   // world matrix rows (world positions for shadows)
    float4 n0, n1, n2;   // normal matrix columns; n0.w = 1: unlit (colour only), n1.w the Masked cutoff (0 = off), n2.w roughness
    float4 color;        // base colour, linear
    float4 camera;       // xyz (picks the shadow cascade)
};
// rhi::GpuLight: position + type (0 sun, 1 point, 2 spot), direction + range, colour + spot inner cos,
// spot outer cos + first shadow tile (-1 none) + normal offset per metre.
struct Light { float4 pos_type; float4 dir_range; float4 color_inner; float4 spot; };
struct Scene {  // LightSetup up to local_tiles, then its output (y: the local atlas's texel size)
    float exposure; uint light_count; float2 pad0;
    float4 sky, ground;  // ambient from above / below
    Light lights[16];
    float4x4 shadow_vp[4];
    float4 shadow_tile[4];
    float4 shadow_split; // far distance of each cascade
    float4 shadow_info;  // cascades, the light that casts them (-1 none), texel size, normal offset
    float4x4 local_vp[16];
    float4 local_tile[16];
    float4 output;
};
[[vk::push_constant]] Push pc;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D base_tex;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState base_samp;
[[vk::binding(0, 2)]] StructuredBuffer<Scene> scene;
[[vk::combinedImageSampler]] [[vk::binding(0, 3)]] Texture2D<float> shadow_tex;
[[vk::combinedImageSampler]] [[vk::binding(0, 3)]] SamplerComparisonState shadow_samp;
[[vk::combinedImageSampler]] [[vk::binding(1, 3)]] Texture2D<float> local_shadow_tex;
[[vk::combinedImageSampler]] [[vk::binding(1, 3)]] SamplerComparisonState local_shadow_samp;
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
float sun_shadow(float3 wpos, float3 n) {
    const Scene sc = scene[0];
    const uint count = (uint)sc.shadow_info.x;
    if (count == 0) return 1.0;
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
// Spot / point light shadow: the light's tile (a point light's cube face by the major axis), as D3D12's local_shadow.
float local_shadow(Light L, float3 wpos, float3 n) {
    uint tile = (uint)L.spot.y;
    const float3 d = wpos - L.pos_type.xyz;
    if (L.pos_type.w < 1.5) {
        const float3 a = abs(d);
        tile += a.x >= a.y && a.x >= a.z ? (d.x > 0 ? 0 : 1) : a.y >= a.z ? (d.y > 0 ? 2 : 3) : (d.z > 0 ? 4 : 5);
    }
    float4 s = mul(scene[0].local_vp[tile], float4(wpos + n * (L.spot.z * length(d)), 1.0));
    s.xyz /= s.w;
    float2 uv = float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || s.z > 1.0 || s.z < 0.0) return 1.0;
    const float4 t = scene[0].local_tile[tile];
    uv = t.xy + uv * t.z;
    const float texel = scene[0].output.y > 0.0 ? scene[0].output.y : 1.0 / 4096.0;
    const float2 lo = t.xy + texel, hi = t.xy + t.z - texel;
    float lit = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) lit += local_shadow_tex.SampleCmpLevelZero(local_shadow_samp, clamp(uv + float2(x, y) * texel, lo, hi), s.z);
    return lit / 9.0;
}

// Into a scene target (output.x = 1, for the post pass): exposed colour range-compressed; else display sRGB.
float3 scene_out(float3 c) { return scene[0].output.x > 0.5 ? sqrt(max(c, 0.0) / (1.0 + max(c, 0.0))) : to_srgb(c); }

float4 ps_main(VsOut i, bool front : SV_IsFrontFace) : SV_Target {
    const float4 texel = pc.color * base_tex.Sample(base_samp, i.uv);
    if (pc.n1.w > 0.0 && texel.a < pc.n1.w) discard;  // Masked
    const float3 base = texel.rgb;
    if (pc.n0.w > 0.5) return float4(scene_out(base), texel.a);
    const float3 n = normalize(i.normal) * (front ? 1.0 : -1.0);
    float3 lit = base * lerp(scene[0].ground.rgb, scene[0].sky.rgb, n.z * 0.5 + 0.5);
    const uint count = min(scene[0].light_count, 16u);
    for (uint li = 0; li < count; ++li) {
        const Light L = scene[0].lights[li];
        float3 l;
        float atten = 1.0;
        if (L.pos_type.w < 0.5) {
            l = -L.dir_range.xyz;
        } else {
            const float3 to = L.pos_type.xyz - i.wpos;
            const float dist2 = max(dot(to, to), 1e-4);
            l = to * rsqrt(dist2);
            const float r = dist2 / (L.dir_range.w * L.dir_range.w);  // smooth window to zero at the range (Karis)
            const float window = saturate(1.0 - r * r);
            atten = window * window / dist2;
            if (L.pos_type.w > 1.5) atten *= saturate((dot(-l, L.dir_range.xyz) - L.spot.x) / max(L.color_inner.w - L.spot.x, 1e-4));
        }
        const float ndl = saturate(dot(n, l));
        if (ndl <= 0.0 || atten <= 0.0) continue;
        if ((float)li == scene[0].shadow_info.y) atten *= sun_shadow(i.wpos, n);
        else if (L.spot.y >= 0.0) atten *= local_shadow(L, i.wpos, n);
        lit += base / 3.14159265 * L.color_inner.xyz * ndl * atten;
    }
    const float smooth = (1.0 - pc.n2.w) * (1.0 - pc.n2.w);  // scene target alpha: how mirror-like (SSR reads it)
    return float4(scene_out(lit * scene[0].exposure), scene[0].output.x > 0.5 ? smooth : texel.a);
}
