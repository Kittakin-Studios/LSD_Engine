// Lit meshes (static, SKINNED, INSTANCED; Material Graph code goes in at the MATERIAL marker below) and the sky,
// shared by D3D12 (embedded as text, compiled at startup) and Vulkan (compiled to SPIR-V at build
// time; VK_BIND gives each resource its Vulkan set and binding). The OpenGL backend keeps a GLSL
// mirror in step.
#ifdef __spirv__  // Vulkan: the per-draw constants as push constants (208 bytes), the rest bound by set / binding
#define VK_BIND(b, s) [[vk::binding(b, s)]]
struct DrawConstants {
    float4x4 mvp; float4 n0; float4 n1; float4 n2; float4 w0; float4 w1; float4 w2;
    float4 color; float4 mat0; float4 mat1;
};
[[vk::push_constant]] DrawConstants draw_pc;
#define mvp draw_pc.mvp
#define n0 draw_pc.n0
#define n1 draw_pc.n1
#define n2 draw_pc.n2
#define w0 draw_pc.w0
#define w1 draw_pc.w1
#define w2 draw_pc.w2
#define color draw_pc.color
#define mat0 draw_pc.mat0
#define mat1 draw_pc.mat1
#else
#define VK_BIND(b, s)
cbuffer Draw : register(b0) {
    float4x4 mvp; float4 n0; float4 n1; float4 n2; float4 w0; float4 w1; float4 w2;
    float4 color; float4 mat0; float4 mat1;  // mat0: metallic, roughness, emissive.rg; mat1: emissive.b, camera.xyz
};
#endif
struct Light { float4 pos_type; float4 dir_range; float4 color_inner; float4 spot; };
VK_BIND(0, 2) cbuffer Lights : register(b1) {
    float exposure; uint light_count; float2 pad0; float4 sky_color; float4 ground; Light lights[16];
    float4x4 shadow_vp[4]; float4 shadow_tile[4]; float4 shadow_split; float4 shadow_info;
    float4x4 local_vp[16]; float4 local_tile[16];
    float4 fog; float4 fog_color; float4 post; float4 tint; float4 viewport_px; float4 hdr_out;
    float4 probe_origin; float4 probe_cell; uint4 probe_dims; float4 probes[1536];
    float4 decal_info; float4 decals[80];
    float4 reflection_info; float4 reflections[8];
    float4 lightmap_info;
    float4 volumetric;
    float4 reflection_boxes[8];  // box captures: half size, w = fade-in distance (0: a sphere)
};
// The ambient light reaching a point facing n: the baked probes (trilinear, fading back to the sky
// light over a cell outside the grid), or the sky / ground colours.
float3 probe_light(uint index, float3 n) {
    const float4 basis = float4(0.886227, 1.023328 * n);  // L1 irradiance
    return max(float3(dot(probes[index * 3], basis), dot(probes[index * 3 + 1], basis), dot(probes[index * 3 + 2], basis)), 0.0) * 0.318310;
}
float3 ambient_light(float3 wpos, float3 n) {
    const float3 sky = lerp(ground.rgb, sky_color.rgb, n.z * 0.5 + 0.5);
    if (probe_origin.w < 0.5) return sky;
    const float3 last = float3(probe_dims.xyz) - 1.0;
    float3 g = (wpos - probe_origin.xyz) / probe_cell.xyz;
    const float3 outside = max(max(-g, g - last), 0.0);
    const float fade = saturate(1.0 - max(outside.x, max(outside.y, outside.z)));
    g = clamp(g, 0.0, last);
    const uint3 c = min(uint3(g), probe_dims.xyz - 2);
    const float3 f = g - float3(c);
    float3 baked = 0;
    for (uint k = 0; k < 8; ++k) {
        const uint3 o = uint3(k & 1, (k >> 1) & 1, k >> 2);
        const float3 w3 = lerp(1.0 - f, f, float3(o));
        const uint3 p = c + o;
        baked += probe_light(p.x + p.y * probe_dims.x + p.z * probe_dims.x * probe_dims.y, n) * (w3.x * w3.y * w3.z);
    }
    return lerp(sky, baked, fade);
}

VK_BIND(0, 1) Texture2D<float> shadow_tex : register(t3);
VK_BIND(1, 1) Texture2D<float> local_shadow_tex : register(t4);
VK_BIND(5, 1) SamplerComparisonState shadow_samp : register(s1);
// Sun shadow: the cascade by distance from the camera, pushed out along the normal against acne,
// 3x3 hardware-filtered taps (soft edges).
float sun_shadow(float3 wpos, float3 n, float3 cam) {
    const float dist = length(wpos - cam);
    uint c = 0;
    while (c + 1 < (uint)shadow_info.x && dist > shadow_split[c]) ++c;
    if (dist > shadow_split[(uint)shadow_info.x - 1]) return 1.0;
    const float4 s = mul(shadow_vp[c], float4(wpos + n * shadow_info.w * (1.0 + c), 1.0));
    float2 uv = float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || s.z > 1.0) return 1.0;
    uv = shadow_tile[c].xy + uv * shadow_tile[c].z;
    const float2 lo = shadow_tile[c].xy + shadow_info.z, hi = shadow_tile[c].xy + shadow_tile[c].z - shadow_info.z;
    float lit = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) lit += shadow_tex.SampleCmpLevelZero(shadow_samp, clamp(uv + float2(x, y) * shadow_info.z, lo, hi), s.z);
    return lit / 9.0;
}
// One hardware-filtered tap of the sun's shadow at a point in the air (volumetric fog).
float sun_visibility(float3 p, float3 cam) {
    const float dist = length(p - cam);
    uint c = 0;
    while (c + 1 < (uint)shadow_info.x && dist > shadow_split[c]) ++c;
    if (dist > shadow_split[(uint)shadow_info.x - 1]) return 1.0;
    const float4 s = mul(shadow_vp[c], float4(p, 1.0));
    float2 uv = float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || s.z > 1.0) return 1.0;
    return shadow_tex.SampleCmpLevelZero(shadow_samp, shadow_tile[c].xy + uv * shadow_tile[c].z, s.z);
}
// Volumetric fog: how lit the fog toward wpos is (1 = plain height fog). The sun's light reaching
// points along the view ray (its shadow sampled at each step), shaped by a Henyey-Greenstein phase.
// Area lights (Karis 2013, UE4's representative point): `ls` becomes the direction to the point of the light's shape
// nearest the reflection ray `rv`, the roughness (sa2 = alpha^2) widens by the shape's size, and `norm` keeps the energy.
// Sphere: a point / spot light with spot.w = radius. Rect (type 3): color_inner.w x spot.x metres; disc (type 4): radius
// color_inner.w; both face dir_range.xyz. shortcut: a rect's roll follows world up, add an axis when lights need to turn.
void area_point(Light L, float3 wpos, float3 rv, float roughness, inout float3 ls, inout float sa2, inout float norm) {
    const float3 to = L.pos_type.xyz - wpos;
    float3 p = to;
    float size = 0.0;
    if (L.pos_type.w > 2.5) {
        const float3 ln = L.dir_range.xyz;
        const float3 right = normalize(cross(abs(ln.z) < 0.99 ? float3(0, 0, 1) : float3(1, 0, 0), ln));
        const float3 up = cross(ln, right);
        const float denom = dot(rv, ln);
        const float t = abs(denom) > 1e-4 ? dot(to, ln) / denom : -1.0;
        const float3 on_plane = rv * t - to;
        float2 uv = t > 0.0 ? float2(dot(on_plane, right), dot(on_plane, up)) : float2(0.0, 0.0);
        if (L.pos_type.w < 3.5) {
            const float2 extent = float2(L.color_inner.w, L.spot.x) * 0.5;
            uv = clamp(uv, -extent, extent);
            size = length(extent);
        } else {
            const float len = length(uv);
            if (len > L.color_inner.w) uv *= L.color_inner.w / len;
            size = L.color_inner.w;
        }
        p = to + right * uv.x + up * uv.y;
    } else if (L.spot.w > 0.0) {
        const float3 c = dot(to, rv) * rv - to;
        p = to + c * saturate(L.spot.w / max(length(c), 1e-4));
        size = L.spot.w;
    }
    if (size <= 0.0) return;
    ls = normalize(p);
    const float a = roughness * roughness, wide = saturate(a + size / (2.0 * max(length(to), 1e-3)));
    sa2 = wide * wide;
    norm = (a / wide) * (a / wide);
}

float fog_light(float3 wpos, float3 cam) {
    if (volumetric.x <= 0.0 || shadow_info.y < 0.0 || shadow_info.x < 0.5) return 1.0;
    const float3 to = wpos - cam;
    const float len = min(length(to), volumetric.z);
    const float3 dir = to / max(length(to), 1e-4);
    const uint steps = (uint)max(volumetric.w, 1.0);
    float vis = 0.0;
    for (uint k = 0; k < steps; ++k) vis += sun_visibility(cam + dir * (len * ((float)k + 0.5) / (float)steps), cam);
    vis /= (float)steps;
    const float g = volumetric.y, cosv = dot(dir, -lights[(uint)shadow_info.y].dir_range.xyz);
    const float phase = (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * cosv, 1e-3), 1.5);
    return lerp(1.0, min(vis * phase, 4.0), volumetric.x);
}
// Spot / point light shadow: the light's tile (a point light's cube face by the major axis).
float local_shadow(Light L, float3 wpos, float3 n) {
    uint tile = (uint)L.spot.y;
    const float3 d = wpos - L.pos_type.xyz;
    if (L.pos_type.w < 1.5) {
        const float3 a = abs(d);
        tile += a.x >= a.y && a.x >= a.z ? (d.x > 0 ? 0 : 1) : a.y >= a.z ? (d.y > 0 ? 2 : 3) : (d.z > 0 ? 4 : 5);
    }
    float4 s = mul(local_vp[tile], float4(wpos + n * (L.spot.z * length(d)), 1.0));
    s.xyz /= s.w;
    float2 uv = float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || s.z > 1.0 || s.z < 0.0) return 1.0;
    uv = local_tile[tile].xy + uv * local_tile[tile].z;
    const float texel = hdr_out.y > 0.0 ? hdr_out.y : 1.0 / 4096.0;  // the atlas's texel (its size follows video memory)
    const float2 lo = local_tile[tile].xy + texel, hi = local_tile[tile].xy + local_tile[tile].z - texel;
    float lit = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) lit += local_shadow_tex.SampleCmpLevelZero(shadow_samp, clamp(uv + float2(x, y) * texel, lo, hi), s.z);
    return lit / 9.0;
}
VK_BIND(0, 0) Texture2D base_tex : register(t0);
VK_BIND(1, 0) Texture2D normal_tex : register(t1);
VK_BIND(2, 0) Texture2D orm_tex : register(t2);
// Decals: each one's box (world -> box rows) and its tile in the decal atlas; faded on surfaces that do
// not face along the box's +Z and towards its top and bottom.
VK_BIND(2, 1) Texture2D decal_tex : register(t5);
VK_BIND(3, 1) Texture2D reflection_tex : register(t6);  // reflection captures (lat-long tiles, mipmapped)
VK_BIND(4, 1) Texture2D lightmap_tex : register(t7);    // the level's lightmaps (Build Lighting)
VK_BIND(6, 1) SamplerState decal_samp : register(s2);
float3 apply_decals(float3 c, float3 wpos, float3 n) {
    const uint count = (uint)decal_info.x;
    for (uint d = 0; d < count; ++d) {
        const float4 p = float4(wpos, 1.0);
        const float3 l = float3(dot(decals[d * 5], p), dot(decals[d * 5 + 1], p), dot(decals[d * 5 + 2], p));
        if (any(abs(l) > 0.5)) continue;
        const float facing = dot(n, normalize(decals[d * 5 + 2].xyz));
        const float fade = saturate((facing - 0.1) * 4.0) * saturate((0.5 - abs(l.z)) * 8.0);
        const float4 rect = decals[d * 5 + 3], tint = decals[d * 5 + 4];
        const float2 uv = rect.xy + clamp(float2(l.x + 0.5, 0.5 - l.y), 0.004, 0.996) * rect.zw;
        const float4 t = decal_tex.SampleLevel(decal_samp, uv, 0);
        c = lerp(c, t.rgb * tint.rgb, saturate(t.a * tint.a * fade));
    }
    return c;
}

VK_BIND(3, 0) SamplerState samp : register(s0);
struct VsIn {
    float3 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0;
#ifndef SKINNED
    float2 lightmap_uv : TEXCOORD1;
#endif
#ifdef SKINNED
    uint4 joints : BLENDINDICES; float4 weights : BLENDWEIGHT;
#endif
#ifdef INSTANCED
    uint instance : SV_InstanceID;
#endif
};
struct VsOut { float4 pos : SV_Position; float3 normal : NORMAL; float3 wpos : TEXCOORD1; float2 uv : TEXCOORD0; float2 lightmap_uv : TEXCOORD2; };
#ifdef SKINNED
VK_BIND(1, 2) cbuffer Joints : register(b2) { float4 joint_rows[768]; };  // 3 rows per joint
#endif
#ifdef INSTANCED
VK_BIND(1, 2) cbuffer Instances : register(b2) { float4 instance_rows[768]; };  // 3 rows per instance: its world matrix
#endif
VsOut vs_main(VsIn i) {
    VsOut o;
    float3 position = i.pos, normal = i.normal;
#ifdef INSTANCED
    const float4 r0 = instance_rows[i.instance * 3], r1 = instance_rows[i.instance * 3 + 1], r2 = instance_rows[i.instance * 3 + 2];
    const float4 local = float4(position, 1);
    const float3 world_pos = float3(dot(r0, local), dot(r1, local), dot(r2, local));
    o.pos = mul(mvp, float4(world_pos, 1));  // mvp is the view-projection here
    // Normals by the cofactor matrix (det * inverse transpose): right under any scale, uniform or not.
    const float3 c0 = float3(r0.x, r1.x, r2.x), c1 = float3(r0.y, r1.y, r2.y), c2 = float3(r0.z, r1.z, r2.z);
    o.normal = cross(c1, c2) * normal.x + cross(c2, c0) * normal.y + cross(c0, c1) * normal.z;
    o.wpos = world_pos;
    o.uv = i.uv;
    o.lightmap_uv = 0;  // instances are not lightmapped
    return o;
#endif
#ifdef SKINNED
    float4 r0 = 0, r1 = 0, r2 = 0;
    [unroll] for (int k = 0; k < 4; ++k) {
        r0 += joint_rows[i.joints[k] * 3] * i.weights[k];
        r1 += joint_rows[i.joints[k] * 3 + 1] * i.weights[k];
        r2 += joint_rows[i.joints[k] * 3 + 2] * i.weights[k];
    }
    position = float3(dot(r0, float4(position, 1)), dot(r1, float4(position, 1)), dot(r2, float4(position, 1)));
    normal = float3(dot(r0.xyz, normal), dot(r1.xyz, normal), dot(r2.xyz, normal));
#endif
    o.pos = mul(mvp, float4(position, 1));
    o.normal = n0.xyz * normal.x + n1.xyz * normal.y + n2.xyz * normal.z;
    const float4 p = float4(position, 1);
    o.wpos = float3(dot(w0, p), dot(w1, p), dot(w2, p));
    o.uv = i.uv;
#ifdef SKINNED
    o.lightmap_uv = 0;
#else
    o.lightmap_uv = i.lightmap_uv;
#endif
    return o;
}
//@MATERIAL
static const float PI = 3.14159265;
// Image-based lighting without cubemaps (Unreal's Sky Light reflections): the sky / ground gradient
// seen along a direction, brighter toward the horizon like the drawn sky.
float3 sky_radiance(float3 d) {
    const float h = 1.0 - abs(d.z);
    return lerp(ground.rgb, sky_color.rgb, smoothstep(-0.25, 0.25, d.z)) * (1.0 + 0.35 * h * h * h);
}
// How much a surface at `wpos` is inside capture k (1 inside, fading to 0 at its edge).
float capture_weight(uint k, float3 wpos) {
    if (reflection_boxes[k].w > 0.0) {
        const float3 inside = reflection_boxes[k].xyz - abs(wpos - reflections[k].xyz);
        return saturate(min(min(inside.x, inside.y), inside.z) / reflection_boxes[k].w);
    }
    return saturate((reflections[k].w - length(wpos - reflections[k].xyz)) / max(reflections[k].w * 0.25, 1e-3));
}
// Capture k along r: a box bends r to where it meets the walls; the lat-long tile, blurrier mips for rougher surfaces.
float3 capture_sample(uint k, float3 r, float roughness, float3 wpos) {
    if (reflection_boxes[k].w > 0.0) {
        const float3 safe = r + float3(r.x >= 0.0 ? 1e-5 : -1e-5, r.y >= 0.0 ? 1e-5 : -1e-5, r.z >= 0.0 ? 1e-5 : -1e-5);
        const float3 c = reflections[k].xyz, e = reflection_boxes[k].xyz;
        const float3 far_side = max((c + e - wpos) / safe, (c - e - wpos) / safe);
        const float t = min(min(far_side.x, far_side.y), far_side.z);
        r = normalize(wpos + r * t - c);
    }
    const float lod = roughness * 5.0;
    const float2 margin = 0.5 * exp2(lod) / float2(256.0, 128.0);
    float2 uv = float2(atan2(r.y, r.x) / 6.28318531 + 0.5, acos(clamp(r.z, -1.0, 1.0)) / 3.14159265);
    uv = clamp(uv, margin, 1.0 - margin);
    const float2 atlas = (float2(k % 2, k / 2) + uv) / float2(2.0, 4.0);
    float3 c = min(reflection_tex.SampleLevel(samp, atlas, lod).rgb, 0.999);
    return c * c / (1.0 - c * c) / max(exposure, 1e-6);  // stored like the scene target: exposed, range-compressed
}
// Reflection captures over the sky reflection: the two this point is most inside, blended by how far inside each is
// (no pop where captures overlap); outside every capture, the sky.
float3 capture_reflection(float3 sky_part, float3 r, float roughness, float3 wpos) {
    float near_w = 0.0, next_w = 0.0;
    uint k1 = 0, k2 = 0;
    for (uint k = 0; k < (uint)reflection_info.x && k < 8; ++k) {
        const float w = capture_weight(k, wpos);
        if (w > near_w) { next_w = near_w; k2 = k1; near_w = w; k1 = k; }
        else if (w > next_w) { next_w = w; k2 = k; }
    }
    if (near_w <= 0.0) return sky_part;
    float3 c = capture_sample(k1, r, roughness, wpos);
    if (next_w > 0.0) c = lerp(c, capture_sample(k2, r, roughness, wpos), next_w / (near_w + next_w));
    return lerp(sky_part, c, near_w);
}
// The split-sum environment BRDF (scale, bias for F0), an analytic fit of the usual lookup table.
float2 env_brdf(float ndv, float roughness) {
    const float4 r = roughness * float4(-1.0, -0.0275, -0.572, 0.022) + float4(1.0, 0.0425, 1.04, -0.04);
    const float a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y;
    return float2(-1.04, 1.04) * a004 + r.zw;
}
float3 to_srgb(float3 c) { c = saturate(c); return lerp(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, step(c, 0.0031308)); }  // no vector ?: (SPIR-V)
// Fog and post-processing on an exposed colour (see rhi::LightSetup): tint, filmic tonemap, height
// fog, saturation, contrast, vignette. `pixel` is SV_Position.xy.
float3 aces(float3 x) { return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)); }
float3 finish(float3 c, float3 wpos, float3 cam, float2 pixel, float fog_lit) {
    c *= tint.rgb;
    if (post.w > 0.5) c = aces(c);
    if (fog.w > 0.0) {  // exponential height fog: density falls off with height, integrated along the view ray
        const float d = max(length(wpos - cam) - fog_color.w, 0.0);
        const float h0 = cam.z - fog.z, h1 = wpos.z - fog.z, dh = h1 - h0;
        const float ray = abs(dh * fog.y) > 1e-4 ? (exp(-fog.y * h0) - exp(-fog.y * h1)) / (fog.y * dh) : exp(-fog.y * h0);
        c = lerp(c, fog_color.rgb * fog_lit, min(1.0 - exp(-fog.x * d * ray), fog.w));
    }
    const float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = max(lerp(luma.xxx, c, post.x), 0.0);
    c = max((c - 0.18) * post.y + 0.18, 0.0);
    if (post.z > 0.0 && viewport_px.z > 0.0) {
        const float2 uv = (pixel - viewport_px.xy) / viewport_px.zw - 0.5;
        c *= 1.0 - post.z * smoothstep(0.1, 0.5, dot(uv, uv));
    }
    return c;
}
// Scene targets for the post pass (LightSetup.output): exposed colour, fog applied, range-compressed.
float3 hdr_encode(float3 c) { c = max(c, 0.0); return sqrt(c / (1.0 + c)); }
float3 scene_out(float3 c, float3 wpos, float3 cam, float2 pixel, float fog_lit) {
    if (hdr_out.x > 0.5) {
        if (fog.w > 0.0) {
            const float d = max(length(wpos - cam) - fog_color.w, 0.0);
            const float h0 = cam.z - fog.z, h1 = wpos.z - fog.z, dh = h1 - h0;
            const float ray = abs(dh * fog.y) > 1e-4 ? (exp(-fog.y * h0) - exp(-fog.y * h1)) / (fog.y * dh) : exp(-fog.y * h0);
            c = lerp(c, fog_color.rgb * fog_lit, min(1.0 - exp(-fog.x * d * ray), fog.w));
        }
        return hdr_encode(c);
    }
    return to_srgb(finish(c, wpos, cam, pixel, fog_lit));
}

// Normal map without stored tangents: a tangent frame from screen derivatives.
float3 perturb_normal(float3 n, float3 wpos, float2 uv, float3 tn) {
    const float3 dp1 = ddx(wpos), dp2 = ddy(wpos);
    const float2 duv1 = ddx(uv), duv2 = ddy(uv);
    const float3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    const float3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    const float3 b = dp2perp * duv1.y + dp1perp * duv2.y;
    const float inv = rsqrt(max(max(dot(t, t), dot(b, b)), 1e-12));
    return normalize(t * inv * tn.x + b * inv * tn.y + n * tn.z);
}
float4 ps_main(VsOut i, bool front : SV_IsFrontFace) : SV_Target {
    float3 n = normalize(i.normal) * (front ? 1.0 : -1.0);
    const float3 geometric_n = n;  // before the normal map: reflections never point into the surface
    if (n0.w > 3.5) return float4(0.06, 0.025, 0.01, 1.0);  // Overdraw view mode: each layer drawn adds a little
    // n1.w packs: 1 has a normal map, 2 blended (translucent / additive), 4 x the masked cutoff (0..255).
    const bool has_normal_map = fmod(n1.w, 2.0) > 0.5;
    const bool blended = fmod(floor(n1.w * 0.5), 2.0) > 0.5;
    const float cutoff = fmod(floor(n1.w * 0.25), 256.0) / 255.0;
    const bool pixel_art = n1.w > 1023.5;  // 1024: whole texels of the base colour texture
#ifdef LSD_MATERIAL
    MaterialIn mi;
    mi.uv = float3(i.uv, 0.0); mi.wpos = i.wpos; mi.normal = n; mi.view = normalize(mat1.yzw - i.wpos); mi.time = n2.w;
    const Surface surf = material_main(mi);
    float4 base = float4(surf.base_color * color.rgb, color.a * surf.opacity);
    if (cutoff > 0.0 && base.a < cutoff) discard;  // Masked
    if (n0.w > 0.5 && n0.w < 1.5) return float4(hdr_out.x > 0.5 ? hdr_encode(base.rgb) : to_srgb(base.rgb), base.a);
    n = perturb_normal(n, i.wpos, i.uv, surf.normal);
    const float metallic = saturate(surf.metallic);
    const float roughness = clamp(surf.roughness, 0.04, 1.0);
    const float ao = saturate(surf.ao);
    const float3 emissive = surf.emissive;
#else
    float2 texel_size;
    base_tex.GetDimensions(texel_size.x, texel_size.y);
    float4 base = color * (pixel_art ? base_tex.SampleLevel(samp, (floor(i.uv * texel_size) + 0.5) / texel_size, 0) : base_tex.Sample(samp, i.uv));
    if (cutoff > 0.0 && base.a < cutoff) discard;  // Masked
    if (n0.w > 0.5 && n0.w < 1.5) return float4(hdr_out.x > 0.5 ? hdr_encode(base.rgb) : to_srgb(base.rgb), base.a);
    if (has_normal_map) {  // Z from X and Y: BC5 maps have none, RGBA8 ones give the same
        const float2 nxy = normal_tex.Sample(samp, i.uv).xy * 2.0 - 1.0;
        n = perturb_normal(n, i.wpos, i.uv, float3(nxy, sqrt(saturate(1.0 - dot(nxy, nxy)))));
    }
    const float3 orm = orm_tex.Sample(samp, i.uv).rgb;
    const float metallic = saturate(mat0.x * orm.b);
    const float roughness = clamp(mat0.y * orm.g, 0.04, 1.0);
    const float ao = orm.r;
    const float3 emissive = float3(mat0.zw, mat1.x);
#endif
    if (n0.w > 1.5 && n0.w < 2.5) { const float3 nc = n * 0.5 + 0.5; return float4(hdr_out.x > 0.5 ? hdr_encode(nc) : to_srgb(nc), 1.0); }  // Normals view mode
    base.rgb = apply_decals(base.rgb, i.wpos, n);
    if (n0.w > 2.5) base.rgb = float3(0.5, 0.5, 0.5);  // Lighting Only view mode: every surface a neutral grey
    const float3 v = normalize(mat1.yzw - i.wpos);
    const float ndv = max(dot(n, v), 1e-4);
    const float3 f0 = lerp(float3(0.04, 0.04, 0.04), base.rgb, metallic);
    const float a2 = roughness * roughness * roughness * roughness;
    const float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float3 lit = 0;
    // Lightmap (Build Lighting): its RGB is this texel's ambient light (sky and bounces), A how much
    // of the sun reached it (baked shadows, sharp beyond the shadow distance too).
    const bool lightmapped = lightmap_info.y > 0.5 && i.lightmap_uv.x > 0.0;
    float4 baked = float4(0, 0, 0, 1);
    if (lightmapped) {
        baked = lightmap_tex.SampleLevel(samp, i.lightmap_uv, 0);
        const float3 c = min(baked.rgb, 0.999);
        baked.rgb = c / (1.0 - c) * lightmap_info.x;
    }
    for (uint li = 0; li < light_count; ++li) {
        const Light L = lights[li];
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
            if (L.pos_type.w > 1.5 && L.pos_type.w < 2.5) atten *= saturate((dot(-l, L.dir_range.xyz) - L.spot.x) / max(L.color_inner.w - L.spot.x, 1e-4));
            if (L.pos_type.w > 2.5) atten *= saturate(dot(-l, L.dir_range.xyz));  // rect / disc: lights only in front
        }
        const float ndl = saturate(dot(n, l));
        if (ndl <= 0.0 || atten <= 0.0) continue;
        if ((float)li == shadow_info.y) atten *= lightmapped ? min(sun_shadow(i.wpos, n, mat1.yzw), baked.a) : sun_shadow(i.wpos, n, mat1.yzw);
        else if (L.spot.y >= 0.0) atten *= local_shadow(L, i.wpos, n);
        float3 ls = l;  // specular's direction: an area light's nearest point (the centre otherwise)
        float sa2 = a2, spec_norm = 1.0;
        if (L.pos_type.w > 0.5) area_point(L, i.wpos, reflect(-v, n), roughness, ls, sa2, spec_norm);
        const float ndls = saturate(dot(n, ls));
        const float3 h = normalize(v + ls);
        const float ndh = saturate(dot(n, h)), vdh = saturate(dot(v, h));
        const float d = sa2 / (PI * pow(ndh * ndh * (sa2 - 1.0) + 1.0, 2.0));
        const float g = (ndls / (ndls * (1.0 - k) + k)) * (ndv / (ndv * (1.0 - k) + k));
        const float3 f = f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0);
        const float3 specular = d * g * f / max(4.0 * ndls * ndv, 1e-4);
        const float3 diffuse = (1.0 - f) * (1.0 - metallic) * base.rgb / PI;
        lit += (diffuse * ndl + specular * ndls * spec_norm) * L.color_inner.xyz * atten;
    }
    // Ambient: diffuse from the sky / baked probes; specular from the sky along the reflection,
    // blurred toward the diffuse light as roughness grows, darkened where the probes are darker than
    // the sky (indoors), by AO (specular occlusion) and below the geometric horizon.
    const float3 sky = lightmapped ? baked.rgb : ambient_light(i.wpos, n);
    const float3 r = reflect(-v, n);
    const float3 lum = float3(0.2126, 0.7152, 0.0722);
    const float open_sky = saturate(dot(sky, lum) / max(dot(lerp(ground.rgb, sky_color.rgb, n.z * 0.5 + 0.5), lum), 1e-4));
    const float3 reflected = capture_reflection(lerp(sky_radiance(r) * open_sky, sky, roughness * roughness), r, roughness, i.wpos);
    const float2 ab = env_brdf(ndv, roughness);
    const float3 env_spec = f0 * ab.x + ab.y;
    const float spec_ao = saturate(pow(ndv + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao);
    const float horizon = saturate(1.0 + 1.2 * dot(r, geometric_n));
    lit += base.rgb * (1.0 - metallic) * (1.0 - env_spec) * sky * ao + env_spec * reflected * spec_ao * horizon * horizon;
    lit += emissive;
    return float4(scene_out(lit * exposure, i.wpos, mat1.yzw, i.pos.xy, fog.w > 0.0 ? fog_light(i.wpos, mat1.yzw) : 1.0), hdr_out.x > 0.5 && !blended ? (1.0 - roughness) * (1.0 - roughness) : base.a);  // scene target: how mirror-like (SSR); blended: opacity
}

// The sky (rhi::SkyDraw through the Draw constants: mvp = inverse view-projection, n0 zenith, n1
// horizon, n2 ground, w0 sun direction + size, w1 sun colour, mat1.yzw camera).
struct SkyOut { float4 pos : SV_Position; float3 dir : TEXCOORD0; };
SkyOut vs_sky(uint id : SV_VertexID) {
    SkyOut o;
    const float2 p = float2((id << 1) & 2, id & 2) * 2.0 - 1.0;  // one triangle over the screen
    o.pos = float4(p, 1.0, 1.0);
    const float4 w = mul(mvp, float4(p, 1.0, 1.0));
    o.dir = w.xyz / w.w - mat1.yzw;
    return o;
}
float4 ps_sky(SkyOut i) : SV_Target {
    const float3 d = normalize(i.dir);
    float3 c = d.z >= 0.0 ? lerp(n1.rgb, n0.rgb, pow(saturate(d.z), 0.6)) : lerp(n1.rgb, n2.rgb, saturate(-d.z * 4.0));
    if (w1.w > 0.0) {
        const float s = dot(d, w0.xyz);
        c += w1.rgb * (smoothstep(w0.w, w0.w + (1.0 - w0.w) * 0.3, s) + 0.25 * pow(saturate(s), 64.0) + 0.06 * pow(saturate(s), 6.0));  // disc + glow + the wide haze around the sun (Mie)
    }
    return float4(scene_out(c, mat1.yzw + d * 5000.0, mat1.yzw, i.pos.xy, 1.0), 1.0);
}
