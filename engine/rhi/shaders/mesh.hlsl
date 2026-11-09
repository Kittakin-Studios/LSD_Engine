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
    float exposure; uint light_count; float2 depth_proj; float4 sky_color; float4 ground; Light lights[16];
    float4x4 shadow_vp[4]; float4 shadow_tile[4]; float4 shadow_split; float4 shadow_info;
    float4x4 local_vp[16]; float4 local_tile[16];
    float4 fog; float4 fog_color; float4 post; float4 tint; float4 viewport_px; float4 hdr_out;
    float4 probe_origin; float4 probe_cell; uint4 probe_dims; float4 probes[1536];
    float4 decal_info; float4 decals[80];
    float4 reflection_info; float4 reflections[8];
    float4 lightmap_info;
    float4 volumetric;
    float4 reflection_boxes[8];  // box captures: half size, w = fade-in distance (0: a sphere)
    float4 light_profiles[64];   // IES: 16 samples of 1 - profile per light (rhi::LightSetup)
    float4 light_functions[32];  // per light: its image in the decal atlas, then scroll, repeats, on
    float4 light_extra[16];      // per light: x = its shadow mask channel + 1
    float4 normal_view[4];       // the normals pass: the view's rotation (rows 0-2), [3].x = 1 while it draws ([3].y: the velocity pass)
    float4 velocity_view[4];     // the velocity pass: this view's view-projection without jitter
    float4 lsd_globals[16];      // global material parameters (the Global Parameter node)
};
// A light's IES profile at the angle from its axis whose cosine is c (1 without one; asset::light_profile_at).
float light_profile(uint li, float c) {
    const float x = acos(clamp(c, -1.0, 1.0)) / 3.14159265 * 15.0;
    const uint i0 = min((uint)x, 14u), i1 = i0 + 1;
    return 1.0 - lerp(light_profiles[li * 4 + i0 / 4][i0 % 4], light_profiles[li * 4 + i1 / 4][i1 % 4], x - (float)i0);
}
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
    if (length(wpos - cam) > shadow_split[(uint)shadow_info.x - 1]) return 1.0;
    // The first (sharpest) cascade whose map holds the point: by distance alone, points near the screen's corners
    // can fall outside a cascade's sphere and show lit (arcs where its split is).
    uint c = 0;
    float4 s = float4(0.0, 0.0, 2.0, 1.0);
    float2 uv = float2(0.0, 0.0);
    for (; c < (uint)shadow_info.x; ++c) {
        s = mul(shadow_vp[c], float4(wpos + n * shadow_info.w * (1.0 + c), 1.0));
        uv = float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5);
        if (all(uv >= 0.0) && all(uv <= 1.0) && s.z <= 1.0) break;
    }
    if (c >= (uint)shadow_info.x) return 1.0;
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
    if (dist > shadow_split[(uint)shadow_info.x - 1]) return 1.0;
    uint c = 0;  // the first cascade holding the point (as sun_shadow)
    float4 s = float4(0.0, 0.0, 2.0, 1.0);
    float2 uv = float2(0.0, 0.0);
    for (; c < (uint)shadow_info.x; ++c) {
        s = mul(shadow_vp[c], float4(p, 1.0));
        uv = float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5);
        if (all(uv >= 0.0) && all(uv <= 1.0) && s.z <= 1.0) break;
    }
    if (c >= (uint)shadow_info.x) return 1.0;
    const float last = shadow_split[(uint)shadow_info.x - 1];  // faded to lit over the last fifth: no hard edge where the shadows end
    return lerp(shadow_tex.SampleCmpLevelZero(shadow_samp, shadow_tile[c].xy + uv * shadow_tile[c].z, s.z), 1.0, saturate((dist - last * 0.8) / (last * 0.2)));
}
// Volumetric fog: how lit the fog toward wpos is (1 = plain height fog). The sun's light reaching
// points along the view ray (its shadow sampled at each step), shaped by a Henyey-Greenstein phase.
// Sphere lights (Karis 2013, UE4's representative point): a point / spot light with spot.w = radius. `ls` becomes the
// direction to the sphere's point nearest the reflection ray `rv`, the roughness (sa2 = alpha^2) widens by its size,
// and `norm` keeps the energy. shortcut: not exact like the panels' LTC, fit a sphere table when big spheres look off.
// Rect / Disc Lights face dir_range.xyz; a rect's roll follows world up (shortcut: add an axis when lights need to turn).
void area_point(Light L, float3 wpos, float3 rv, float roughness, inout float3 ls, inout float sa2, inout float norm) {
    const float3 to = L.pos_type.xyz - wpos;
    if (L.spot.w <= 0.0) return;
    const float3 c = dot(to, rv) * rv - to;
    const float3 p = to + c * saturate(L.spot.w / max(length(c), 1e-4));
    const float size = L.spot.w;
    ls = normalize(p);
    const float a = roughness * roughness, wide = saturate(a + size / (2.0 * max(length(to), 1e-3)));
    sa2 = wide * wide;
    norm = (a / wide) * (a / wide);
}

// What lights the fog along the view ray (Volumetric): the sun where its shadow lets it through, and the Point
// and Spot Lights near the ray (their glow and cones in the fog; shortcut: unshadowed). Multiplies the fog colour.
// `pixel` (SV_Position.xy) staggers where the steps fall per pixel (interleaved gradient noise): with the same
// distances everywhere, shadow edges show as arcs where each step's sphere meets them.
float3 fog_light(float3 wpos, float3 cam, float2 pixel) {
    const float jitter = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    if (volumetric.x <= 0.0) return float3(1.0, 1.0, 1.0);
    const float3 to = wpos - cam;
    const float len = min(length(to), volumetric.z);
    const float3 dir = to / max(length(to), 1e-4);
    const uint steps = (uint)max(volumetric.w, 1.0);
    const float g = volumetric.y;
    float sun = 1.0;
    if (shadow_info.y >= 0.0 && shadow_info.x >= 0.5) {
        float vis = 0.0;
        for (uint k = 0; k < steps; ++k) vis += sun_visibility(cam + dir * (len * ((float)k + jitter) / (float)steps), cam);
        vis /= (float)steps;
        const float cosv = dot(dir, -lights[(uint)shadow_info.y].dir_range.xyz);
        const float phase = (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * cosv, 1e-3), 1.5);
        sun = lerp(1.0, min(vis * phase, 4.0), volumetric.x);
    }
    float3 local = float3(0.0, 0.0, 0.0);
    for (uint k = 0; k < steps; ++k) {
        const float3 p = cam + dir * (len * ((float)k + jitter) / (float)steps);
        for (uint li = 0; li < light_count; ++li) {
            const Light L = lights[li];
            if (L.pos_type.w < 0.5 || L.pos_type.w > 2.5) continue;  // point and spot lights
            const float3 d = L.pos_type.xyz - p;
            const float dist2 = max(dot(d, d), 1e-2);
            const float r = dist2 / (L.dir_range.w * L.dir_range.w);
            const float window = saturate(1.0 - r * r);
            float atten = window * window / dist2;
            const float3 l = d * rsqrt(dist2);
            if (L.pos_type.w > 1.5) atten *= saturate((dot(-l, L.dir_range.xyz) - L.spot.x) / max(L.color_inner.w - L.spot.x, 1e-4));
            atten *= light_profile(li, dot(-l, L.dir_range.xyz));
            const float cosv = dot(dir, -l);
            local += L.color_inner.xyz * atten * (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * cosv, 1e-3), 1.5);
        }
    }
    return sun + local * (exposure * volumetric.x * 0.25 / (float)steps);
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
    // Area lights (spot.w: a sphere's radius, a panel's half size): a wider filter, up to 4 texels apart (no blocker search)
    const float spread = clamp(L.spot.w * 0.15 / max(L.spot.z / 1.5 * length(d), 1e-5), 1.0, 4.0) * texel;
    float lit = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) lit += local_shadow_tex.SampleCmpLevelZero(shadow_samp, clamp(uv + float2(x, y) * spread, lo, hi), s.z);
    return lit / 9.0;
}
VK_BIND(0, 0) Texture2D base_tex : register(t0);
VK_BIND(1, 0) Texture2D normal_tex : register(t1);
VK_BIND(2, 0) Texture2D orm_tex : register(t2);
VK_BIND(4, 0) Texture2D emissive_tex : register(t8);  // the emissive map (white without one)
// Decals: each one's box (world -> box rows) and its tile in the decal atlas; faded on surfaces that do
// not face along the box's +Z and towards its top and bottom.
VK_BIND(2, 1) Texture2D decal_tex : register(t5);
VK_BIND(3, 1) Texture2D reflection_tex : register(t6);  // reflection captures (lat-long tiles, mipmapped)
VK_BIND(4, 1) Texture2D lightmap_tex : register(t7);    // the level's lightmaps (Build Lighting)
VK_BIND(6, 1) SamplerState decal_samp : register(s2);
// A light function: its image projected along the light's axis `f` (up is world up), toward the pixel `to`; its brightness.
float light_function(uint li, float3 f, float3 to) {
    const float z = dot(to, f);
    if (z <= 1e-3) return 0.0;
    const float3 right = normalize(cross(f, abs(f.z) > 0.99 ? float3(1.0, 0.0, 0.0) : float3(0.0, 0.0, 1.0))), up = cross(right, f);
    const float4 rect = light_functions[li * 2], extra = light_functions[li * 2 + 1];
    const float2 uv = frac(float2(dot(to, right), -dot(to, up)) / z * extra.z + 0.5 + extra.xy);
    return dot(decal_tex.SampleLevel(decal_samp, rect.xy + uv * rect.zw, 0).rgb, float3(0.2126, 0.7152, 0.0722));
}

VK_BIND(7, 1) Texture2D<float> scene_depth_tex : register(t9);  // MeshDraw::scene_depth: the target's depth (white: none)
// Metres of the view from this pixel to what is drawn behind it (far without a depth to read).
float scene_depth_m(float4 pos) {
    const float behind = scene_depth_tex.Load(int3(pos.xy, 0));
    if (depth_proj.y == 0.0 || behind >= 1.0) return 1e4;
    return depth_proj.y / (behind + depth_proj.x) - depth_proj.y / (pos.z + depth_proj.x);
}
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
    float3 c = min(reflection_tex.SampleLevel(decal_samp, atlas, lod).rgb, 0.999);
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

// Rect / Disc Lights, exact (linearly transformed cosines, Heitz et al. 2016): GGX's lobe for a roughness and view
// angle is a clamped cosine bent by a 3x3 matrix, so the light's outline taken through its inverse gives the
// specular share, and the outline itself the diffuse one (Lambert's edge formula). The table: tools/ltc_fit.py.
// ltc table begin
static const float4 ltc_table[256] = {
    float4(49.99845, 0.00000, 49.99845, 0.00000),
    float4(49.99556, 4.72940, 50.44307, -0.09402),
    float4(49.96408, 9.54906, 51.78899, -0.18741),
    float4(49.82514, 14.53214, 54.06319, -0.27878),
    float4(49.43561, 19.71005, 57.29316, -0.36598),
    float4(48.59503, 25.04428, 61.50064, -0.44644),
    float4(47.02467, 30.37299, 66.64056, -0.51540),
    float4(103177828613803.54688, 80979461430970.18750, 839752134714545.37500, -13695019493230.37695),
    float4(40.38907, 39.42377, 78.86979, -0.59161),
    float4(34.85166, 41.83318, 85.06787, -0.58433),
    float4(27.95155, 41.81973, 90.53253, -0.54172),
    float4(20.23764, 38.80371, 94.67432, -0.46573),
    float4(12.61125, 32.64905, 97.22362, -0.36438),
    float4(6.14234, 23.84769, 98.97527, -0.25141),
    float4(0.40701, 3.03324, 25.88229, 0.06612),
    float4(0.50353, 4.12402, 3765.71430, -0.64685),
    float4(49.99845, 0.00000, 49.99845, 0.00000),
    float4(49.99557, 4.72940, 50.44308, -0.09402),
    float4(49.96408, 9.54906, 51.78899, -0.18741),
    float4(49.82514, 14.53214, 54.06319, -0.27878),
    float4(49.43561, 19.71005, 57.29315, -0.36598),
    float4(48.59503, 25.04428, 61.50064, -0.44644),
    float4(47.02467, 30.37299, 66.64056, -0.51540),
    float4(1462107014016350.50000, 1147539562373020.00000, 11899921486665904.00000, -194068685883340.62500),
    float4(40.38903, 39.42373, 78.86971, -0.59161),
    float4(34.85169, 41.83320, 85.06792, -0.58433),
    float4(27.95156, 41.81974, 90.53256, -0.54172),
    float4(20.23762, 38.80367, 94.67421, -0.46573),
    float4(12.61125, 32.64905, 97.22363, -0.36438),
    float4(6.14234, 23.84768, 98.97524, -0.25140),
    float4(0.40701, 3.03324, 25.88229, 0.06612),
    float4(0.50353, 4.12402, 3765.60638, -0.64685),
    float4(28.12226, 0.00000, 28.12226, 0.00000),
    float4(28.12020, 2.66006, 28.37205, -0.09390),
    float4(28.10224, 5.37079, 29.12854, -0.18728),
    float4(28.02428, 8.17341, 30.40733, -0.27868),
    float4(27.80471, 11.08518, 32.22252, -0.36580),
    float4(27.32618, 14.08175, 34.58033, -0.44573),
    float4(26.44962, 17.08129, 37.47754, -0.51500),
    float4(24.96887, 19.88028, 40.79445, -0.56584),
    float4(22.71747, 22.16750, 44.34746, -0.59111),
    float4(19.62563, 23.54585, 47.88086, -0.58520),
    float4(15.74104, 23.53358, 50.94852, -0.54230),
    float4(11.42325, 21.87653, 53.38220, -0.46796),
    float4(520120075636923.25000, 1315529410153189.00000, 10323923084238206.00000, -110093311112985.68750),
    float4(126818424376200.89062, 468360659027535.93750, 9885170994856972.00000, -62836426424616.76562),
    float4(14599409869139.90820, 61788912659507.96875, 6990613597673320.00000, -16454478064671.91211),
    float4(168.93857, 714.62217, 352912.33187, -191.14469),
    float4(12.50408, 0.00000, 12.50408, 0.00000),
    float4(12.50091, 1.18250, 12.61302, -0.09370),
    float4(12.49265, 2.38736, 12.94832, -0.18700),
    float4(12.45816, 3.63284, 13.51558, -0.27831),
    float4(12.35949, 4.92589, 14.31896, -0.36533),
    float4(12.14474, 6.25503, 15.36034, -0.44437),
    float4(11.74822, 7.58064, 16.63243, -0.51236),
    float4(11.09920, 8.82603, 18.11094, -0.56397),
    float4(10.10332, 9.84012, 19.68603, -0.58942),
    float4(8.74705, 10.46463, 21.28177, -0.58537),
    float4(7.05479, 10.50146, 22.74112, -0.54711),
    float4(5.19095, 9.87181, 24.10441, -0.48232),
    float4(3.30970, 8.44315, 25.19123, -0.38891),
    float4(1.70116, 6.39800, 26.65056, -0.28473),
    float4(0.43451, 2.89235, 22.96882, -0.09792),
    float4(0.37645, 2.62593, 290.88852, -0.46596),
    float4(7.04507, 0.00000, 7.04507, 0.00000),
    float4(7.04383, 0.66622, 7.10701, -0.09337),
    float4(7.03984, 1.34498, 7.29551, -0.18647),
    float4(7.02021, 2.04607, 7.61287, -0.27753),
    float4(6.96338, 2.77272, 8.06057, -0.36412),
    float4(6.84321, 3.51918, 8.64209, -0.44248),
    float4(6.62673, 4.26611, 9.35999, -0.51114),
    float4(6.26876, 4.96791, 10.19370, -0.56350),
    float4(5.73185, 5.55468, 11.11186, -0.59314),
    float4(4.98301, 5.91714, 12.03389, -0.59130),
    float4(4.07463, 5.99753, 12.98905, -0.56295),
    float4(3.03724, 5.67294, 13.85566, -0.49940),
    float4(1.94968, 4.81371, 14.37043, -0.39246),
    float4(0.90321, 3.12575, 13.09637, -0.20547),
    float4(0.27867, 1.36782, 11.14508, -0.05136),
    float4(0.31357, 1.46794, 42.75887, -0.19609),
    float4(4.52104, 0.00000, 4.52104, 0.00000),
    float4(4.51869, 0.42708, 4.55934, -0.09173),
    float4(4.51670, 0.86209, 4.67930, -0.18365),
    float4(4.50692, 1.31159, 4.88291, -0.27343),
    float4(4.47315, 1.77678, 5.16804, -0.35721),
    float4(4.39575, 2.25209, 5.53274, -0.43319),
    float4(4.26562, 2.73194, 5.99550, -0.50401),
    float4(4.05689, 3.19203, 6.54859, -0.56201),
    float4(3.71106, 3.55809, 7.11514, -0.58632),
    float4(3.24606, 3.79453, 7.71003, -0.58417),
    float4(2.63765, 3.78796, 8.19525, -0.53742),
    float4(1.90535, 3.40989, 8.32931, -0.42845),
    float4(1.14298, 2.58768, 7.77631, -0.25920),
    float4(0.57971, 1.66351, 7.09996, -0.11682),
    float4(0.28302, 0.94171, 7.35292, -0.04570),
    float4(0.28820, 0.98523, 15.93707, -0.12504),
    float4(3.17014, 0.00000, 3.17014, 0.00000),
    float4(3.16867, 0.29815, 3.19672, -0.08633),
    float4(3.16288, 0.60077, 3.27410, -0.17300),
    float4(3.14701, 0.91015, 3.40252, -0.25521),
    float4(3.10796, 1.22336, 3.57473, -0.32489),
    float4(3.02590, 1.53000, 3.77847, -0.37775),
    float4(2.92374, 1.84409, 4.06616, -0.44191),
    float4(2.75700, 2.12643, 4.38150, -0.48255),
    float4(2.52173, 2.35566, 4.72384, -0.50444),
    float4(2.17983, 2.45418, 4.99751, -0.47897),
    float4(1.72509, 2.33491, 5.07141, -0.39297),
    float4(1.24268, 2.01989, 4.96881, -0.27876),
    float4(0.81478, 1.57588, 4.78823, -0.16732),
    float4(0.51015, 1.12973, 4.79485, -0.09144),
    float4(0.34285, 0.78975, 5.47982, -0.05908),
    float4(0.35313, 0.80524, 8.55580, -0.10858),
    float4(2.36224, 0.00000, 2.36224, 0.00000),
    float4(2.36097, 0.21804, 2.38111, -0.07371),
    float4(2.35282, 0.43766, 2.43083, -0.14506),
    float4(2.33135, 0.65772, 2.50762, -0.20813),
    float4(2.28959, 0.87556, 2.60919, -0.26084),
    float4(2.22373, 1.09079, 2.74419, -0.31344),
    float4(2.12265, 1.28937, 2.89878, -0.35115),
    float4(1.97512, 1.45209, 3.05291, -0.36527),
    float4(1.79783, 1.58258, 3.22615, -0.37306),
    float4(1.55394, 1.61699, 3.33967, -0.33959),
    float4(1.26327, 1.53645, 3.37643, -0.27288),
    float4(0.97663, 1.37311, 3.39421, -0.20417),
    float4(0.72504, 1.14848, 3.42856, -0.14200),
    float4(0.53467, 0.90877, 3.56927, -0.09308),
    float4(0.42342, 0.70955, 4.13269, -0.07559),
    float4(0.41884, 0.71856, 4.80324, -0.09157),
    float4(1.85506, 0.00000, 1.85506, 0.00000),
    float4(1.85173, 0.16212, 1.86593, -0.05375),
    float4(1.84009, 0.32455, 1.89414, -0.10810),
    float4(1.81618, 0.48462, 1.93833, -0.15595),
    float4(1.77763, 0.64174, 2.00077, -0.19997),
    float4(1.72236, 0.79380, 2.08471, -0.24126),
    float4(1.64098, 0.92792, 2.17416, -0.26595),
    float4(1.53193, 1.03575, 2.26206, -0.27315),
    float4(1.39175, 1.10419, 2.33629, -0.26187),
    float4(1.23218, 1.13313, 2.40719, -0.24189),
    float4(1.05841, 1.11232, 2.47152, -0.21346),
    float4(0.88494, 1.04203, 2.53360, -0.17810),
    float4(0.72090, 0.91962, 2.59000, -0.13360),
    float4(0.59867, 0.79244, 2.77215, -0.10854),
    float4(0.51302, 0.65729, 3.12410, -0.09335),
    float4(0.50646, 0.64774, 3.76622, -0.11592),
    float4(1.52321, 0.00000, 1.52321, 0.00000),
    float4(1.51906, 0.12205, 1.52905, -0.03982),
    float4(1.50651, 0.24265, 1.54454, -0.07793),
    float4(1.48513, 0.36171, 1.57098, -0.11568),
    float4(1.45436, 0.47783, 1.60918, -0.15160),
    float4(1.40810, 0.58385, 1.65493, -0.17590),
    float4(1.34520, 0.67594, 1.70587, -0.18913),
    float4(1.27037, 0.75502, 1.76054, -0.19776),
    float4(1.18099, 0.81448, 1.81643, -0.20058),
    float4(1.08051, 0.85028, 1.87128, -0.19628),
    float4(0.97155, 0.85487, 1.91863, -0.18308),
    float4(0.85936, 0.82584, 1.96517, -0.15998),
    float4(0.75451, 0.77046, 2.03509, -0.13696),
    float4(0.67073, 0.69981, 2.17174, -0.12232),
    float4(0.60181, 0.60742, 2.37615, -0.10992),
    float4(0.58001, 0.58330, 2.92190, -0.14357),
    float4(1.29552, 0.00000, 1.29552, 0.00000),
    float4(1.29284, 0.09135, 1.29989, -0.03009),
    float4(1.28489, 0.18203, 1.31113, -0.06013),
    float4(1.26924, 0.26936, 1.32857, -0.08503),
    float4(1.24446, 0.35249, 1.35127, -0.10677),
    float4(1.20987, 0.42852, 1.37818, -0.12265),
    float4(1.16932, 0.50011, 1.41002, -0.14066),
    float4(1.12098, 0.56270, 1.44411, -0.15513),
    float4(1.06258, 0.61029, 1.47746, -0.16051),
    float4(1.00029, 0.64574, 1.51250, -0.16263),
    float4(0.92995, 0.65963, 1.54561, -0.15393),
    float4(0.85933, 0.65671, 1.58218, -0.14488),
    float4(0.79239, 0.63657, 1.63692, -0.13672),
    float4(0.72985, 0.59562, 1.71004, -0.12403),
    float4(0.67424, 0.53542, 1.81668, -0.11580),
    float4(0.64320, 0.50755, 2.00655, -0.13258),
    float4(1.14117, 0.00000, 1.14117, 0.00000),
    float4(1.14050, 0.06623, 1.14515, -0.02161),
    float4(1.13544, 0.13123, 1.15261, -0.04115),
    float4(1.12408, 0.19418, 1.16307, -0.05879),
    float4(1.10754, 0.25514, 1.17702, -0.07692),
    float4(1.08471, 0.31108, 1.19273, -0.09121),
    float4(1.05598, 0.36098, 1.20956, -0.10194),
    float4(1.02505, 0.40750, 1.22930, -0.11463),
    float4(0.98912, 0.44496, 1.24798, -0.12113),
    float4(0.95112, 0.47596, 1.26834, -0.12672),
    float4(0.90454, 0.49084, 1.28625, -0.12141),
    float4(0.86068, 0.50127, 1.31354, -0.12246),
    float4(0.81442, 0.49447, 1.33718, -0.11796),
    float4(0.76872, 0.47566, 1.36896, -0.11333),
    float4(0.72572, 0.44347, 1.40874, -0.11346),
    float4(0.72589, 0.44390, 1.43904, -0.15250),
    float4(1.03765, 0.00000, 1.03765, 0.00000),
    float4(1.03722, 0.04496, 1.03972, -0.01410),
    float4(1.03299, 0.08905, 1.04303, -0.02706),
    float4(1.02542, 0.13217, 1.04782, -0.04013),
    float4(1.01479, 0.17355, 1.05467, -0.05235),
    float4(0.99981, 0.21125, 1.06226, -0.06139),
    float4(0.98398, 0.24846, 1.07309, -0.07407),
    float4(0.96366, 0.27906, 1.08324, -0.07976),
    float4(0.94162, 0.30607, 1.09356, -0.08471),
    float4(0.91709, 0.32900, 1.10356, -0.08931),
    float4(0.88906, 0.34551, 1.11220, -0.09219),
    float4(0.85932, 0.35582, 1.12160, -0.09467),
    float4(0.82640, 0.35769, 1.12937, -0.09395),
    float4(0.78788, 0.34680, 1.12733, -0.09082),
    float4(0.74114, 0.32257, 1.10526, -0.08714),
    float4(0.68570, 0.29200, 1.05814, -0.09767),
    float4(0.96639, 0.00000, 0.96639, 0.00000),
    float4(0.96515, 0.02710, 0.96633, -0.00819),
    float4(0.96246, 0.05412, 0.96734, -0.01697),
    float4(0.95757, 0.07982, 0.96869, -0.02423),
    float4(0.95118, 0.10491, 0.97100, -0.03197),
    float4(0.94358, 0.12918, 0.97463, -0.04007),
    float4(0.93386, 0.15051, 0.97879, -0.04469),
    float4(0.92277, 0.17054, 0.98354, -0.04994),
    float4(0.90888, 0.18746, 0.98778, -0.05255),
    float4(0.89299, 0.20263, 0.99074, -0.05633),
    float4(0.87482, 0.21496, 0.99109, -0.06053),
    float4(0.85335, 0.22227, 0.98754, -0.06226),
    float4(0.82642, 0.22374, 0.98070, -0.05977),
    float4(0.79217, 0.21754, 0.96237, -0.05562),
    float4(0.75026, 0.20713, 0.92221, -0.06093),
    float4(0.68683, 0.18181, 0.84009, -0.04744),
    float4(0.91819, 0.00000, 0.91819, 0.00000),
    float4(0.91668, 0.01224, 0.91712, -0.00325),
    float4(0.91443, 0.02458, 0.91647, -0.00742),
    float4(0.91161, 0.03668, 0.91613, -0.01172),
    float4(0.90851, 0.04818, 0.91635, -0.01540),
    float4(0.90428, 0.05900, 0.91646, -0.01853),
    float4(0.89927, 0.06917, 0.91664, -0.02135),
    float4(0.89288, 0.07828, 0.91637, -0.02333),
    float4(0.88412, 0.08648, 0.91450, -0.02528),
    float4(0.87310, 0.09333, 0.91085, -0.02651),
    float4(0.85979, 0.09898, 0.90471, -0.02786),
    float4(0.84305, 0.10231, 0.89440, -0.02774),
    float4(0.82094, 0.10321, 0.87819, -0.02662),
    float4(0.79002, 0.10056, 0.85328, -0.02290),
    float4(0.74282, 0.09355, 0.80375, -0.02118),
    float4(0.66254, 0.08048, 0.71253, -0.02318),
    float4(0.88675, 0.00000, 0.88675, 0.00000),
    float4(0.88551, 0.00014, 0.88557, 0.00056),
    float4(0.88371, 0.00024, 0.88396, 0.00053),
    float4(0.88162, 0.00045, 0.88202, 0.00012),
    float4(0.87937, 0.00063, 0.87974, -0.00036),
    float4(0.87708, 0.00064, 0.87727, -0.00051),
    float4(0.87330, 0.00049, 0.87342, -0.00021),
    float4(0.86830, 0.00029, 0.86841, 0.00003),
    float4(0.86217, 0.00025, 0.86222, -0.00012),
    float4(0.85364, 0.00011, 0.85375, 0.00008),
    float4(0.84205, -0.00000, 0.84221, 0.00015),
    float4(0.82685, -0.00011, 0.82682, 0.00033),
    float4(0.80542, -0.00024, 0.80493, 0.00080),
    float4(0.77459, -0.00028, 0.77477, 0.00092),
    float4(0.72193, 0.00117, 0.72147, -0.00148),
    float4(0.62626, 0.00541, 0.62715, -0.00835),
};
// ltc table end
float4 ltc_inverse(float roughness, float ndv) {  // (m00, m02, m11, m20), m22 = 1; bilinear over the 16 x 16 table
    const float2 p = float2(sqrt(1.0 - ndv), roughness) * 15.0;
    const float2 c = min(floor(p), 14.0), f = p - c;
    const uint k = (uint)c.y * 16 + (uint)c.x;
    return lerp(lerp(ltc_table[k], ltc_table[k + 1], f.x), lerp(ltc_table[k + 16], ltc_table[k + 17], f.x), f.y);
}
// The cosine-weighted share of the hemisphere (0..1) a polygon covers; corners relative to the point, z along the normal.
float polygon_share(float3 p[8], uint count) {
    float3 c[9];
    uint n = 0;
    for (uint e = 0; e < count; ++e) {  // clipped to the horizon
        const float3 a = p[e], b = p[(e + 1) % count];
        if (a.z >= 0.0) c[n++] = a;
        if ((a.z >= 0.0) != (b.z >= 0.0)) c[n++] = lerp(a, b, a.z / (a.z - b.z));
    }
    float sum = 0.0;
    for (uint k = 0; k < n; ++k) {
        const float3 a = normalize(c[k]), b = normalize(c[(k + 1) % n]);
        const float x = clamp(dot(a, b), -0.9999, 0.9999);
        sum += cross(a, b).z * acos(x) * rsqrt(1.0 - x * x);
    }
    return abs(sum) / (2.0 * PI);
}
// A Rect (type 3: color_inner.w x spot.x metres) or Disc Light's (radius color_inner.w; an octagon of the same area)
// outline seen from wpos in the frame (t1, t2, n). Returns its corner count; `area` its size in square metres.
uint panel_corners(Light L, float3 wpos, float3 t1, float3 t2, float3 n, out float3 p[8], out float area) {
    const float3 ln = L.dir_range.xyz;
    const float3 right = normalize(cross(abs(ln.z) < 0.99 ? float3(0, 0, 1) : float3(1, 0, 0), ln)), up = cross(ln, right);
    const float3 to = L.pos_type.xyz - wpos;
    const bool rect = L.pos_type.w < 3.5;
    const float2 e = float2(L.color_inner.w, L.spot.x) * 0.5;
    area = rect ? 4.0 * e.x * e.y : PI * L.color_inner.w * L.color_inner.w;
    for (uint k = 0; k < 8; ++k) {
        const float angle = k * (PI * 0.25);
        const float2 o = rect ? e * float2(k == 1 || k == 2 ? 1.0 : -1.0, k >= 2 ? 1.0 : -1.0) : float2(cos(angle), sin(angle)) * (L.color_inner.w * 1.0539);
        const float3 q = to + right * o.x + up * o.y;
        p[k] = float3(dot(q, t1), dot(q, t2), dot(q, n));
    }
    return rect ? 4 : 8;
}
float3 to_srgb(float3 c) { c = saturate(c); return lerp(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, step(c, 0.0031308)); }  // no vector ?: (SPIR-V)
// Fog and post-processing on an exposed colour (see rhi::LightSetup): tint, filmic tonemap, height
// fog, saturation, contrast, vignette. `pixel` is SV_Position.xy.
float3 aces(float3 x) { return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)); }
float3 finish(float3 c, float3 wpos, float3 cam, float2 pixel, float3 fog_lit) {
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
float3 scene_out(float3 c, float3 wpos, float3 cam, float2 pixel, float3 fog_lit) {
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
    if (normal_view[3].y > 0.5) {  // the velocity pass (TAA, motion blur): how far this pixel moved on screen since the last frame
        const float4 w4 = float4(i.wpos, 1.0);
        const float4 now = float4(dot(velocity_view[0], w4), dot(velocity_view[1], w4), dot(velocity_view[2], w4), dot(velocity_view[3], w4));
        const float4 was = float4(dot(float4(n0.xyz, mat0.x), w4), dot(float4(n1.xyz, mat0.y), w4), dot(float4(n2.xyz, mat0.z), w4), dot(color, w4));
        const float2 a = now.xy / now.w, b = was.xy / (abs(was.w) > 1e-6 ? was.w : 1e-6);
        const float2 q = round(saturate(float2(a.x - b.x, b.y - a.y) * 0.125 + 0.5) * 65535.0);  // uv moved (y down; ndc / 2), 16 bits over -2..2
        const float2 hi = floor(q / 256.0);
        return max(float4(hi.x, q.x - hi.x * 256.0, hi.y, q.y - hi.y * 256.0), float4(1.0, 0.0, 0.0, 0.0)) / 255.0;  // never all 0 (= nothing drawn)
    }
    float3 n = normalize(i.normal) * (front ? 1.0 : -1.0);
    const float3 geometric_n = n;  // before the normal map: reflections never point into the surface
    if (n0.w > 4.5) {  // Lightmap Density view mode (Unreal's): a checker of lightmap texels, green where lightmapped, grey elsewhere
        float3 c = float3(0.3, 0.3, 0.3);
        if (lightmap_info.y > 0.5 && i.lightmap_uv.x > 0.0) {
            const float2 cell = floor(i.lightmap_uv * lightmap_info.z);
            c = fmod(cell.x + cell.y, 2.0) > 0.5 ? float3(0.15, 0.55, 0.2) : float3(0.08, 0.32, 0.12);
        }
        c *= 0.55 + 0.45 * saturate(dot(n, normalize(float3(0.4, 0.3, 0.85))));
        return float4(hdr_out.x > 0.5 ? hdr_encode(c) : to_srgb(c), 1.0);
    }
    if (n0.w > 3.5) return float4(0.06, 0.025, 0.01, 1.0);  // Overdraw view mode: each layer drawn adds a little
    // n1.w packs: 1 has a normal map, 2 blended (translucent / additive), 4 x the masked cutoff (0..255).
    const bool has_normal_map = fmod(n1.w, 2.0) > 0.5;
    const bool blended = fmod(floor(n1.w * 0.5), 2.0) > 0.5;
    const float cutoff = fmod(floor(n1.w * 0.25), 256.0) / 255.0;
    const bool pixel_art = fmod(floor(n1.w / 1024.0), 2.0) > 0.5;  // 1024: whole texels of the base colour texture
    const bool hair = n1.w > 2047.5;  // 2048: the Hair shading model
#ifdef LSD_MATERIAL
    MaterialIn mi;
    mi.uv = float3(i.uv, 0.0); mi.wpos = i.wpos; mi.normal = n; mi.view = normalize(mat1.yzw - i.wpos); mi.time = n2.w; mi.depth = scene_depth_m(i.pos);
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
    const float3 emissive = float3(mat0.zw, mat1.x) * emissive_tex.Sample(samp, i.uv).rgb;
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
    const float2 ab = env_brdf(ndv, roughness);
    const float3 env_spec = f0 * ab.x + ab.y;  // GGX's whole reflectance (split sum): ambient and the panels' scale
    const float4 ltc = ltc_inverse(roughness, ndv);
    float3 t1 = v - n * dot(n, v);  // the panels' frame: the view in the x-z plane
    t1 = dot(t1, t1) > 1e-8 ? normalize(t1) : normalize(cross(n, abs(n.z) < 0.99 ? float3(0, 0, 1) : float3(1, 0, 0)));
    const float3 t2 = cross(n, t1);
    // Lightmap (Build Lighting): its RGB is this texel's ambient light (sky and bounces), A how much
    // of the sun reached it (baked shadows, sharp beyond the shadow distance too).
    const bool lightmapped = lightmap_info.y > 0.5 && i.lightmap_uv.x > 0.0;
    float4 baked = float4(0, 0, 0, 1), shadow_mask = float4(1, 1, 1, 1);
    if (lightmapped) {
        const float segments = max(lightmap_info.w, 1.0);  // light, then directions, then shadow masks
        const float2 at = i.lightmap_uv * float2(1.0, 1.0 / segments);
        baked = lightmap_tex.SampleLevel(samp, at, 0);
        const float3 c = min(baked.rgb, 0.999);
        baked.rgb = c / (1.0 - c) * lightmap_info.x;
        if (segments > 2.5) shadow_mask = lightmap_tex.SampleLevel(samp, at + float2(0.0, 2.0 / segments), 0);
        if (segments > 1.5) {  // directional: the normal-mapped normal against the one it was baked for
            const float3 d = lightmap_tex.SampleLevel(samp, at + float2(0.0, 1.0 / segments), 0).xyz * 2.0 - 1.0;
            baked.rgb *= (dot(n, d) * 0.5 + 0.5) / max(dot(geometric_n, d) * 0.5 + 0.5, 0.1);
        }
    }
    // Hair (shading model): the strand runs along the texture's V; its direction from how the UVs change across the pixel.
    float3 strand = float3(0.0, 0.0, 1.0);
    if (hair) {
        const float3 dp1 = ddx(i.wpos), dp2 = ddy(i.wpos);
        const float2 duv1 = ddx(i.uv), duv2 = ddy(i.uv);
        const float3 along = dp2 * duv1.x - dp1 * duv2.x;
        const float3 across = along - n * dot(along, n);
        strand = dot(across, across) > 1e-12 ? normalize(across) : strand;
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
            if (L.pos_type.w > 2.5) atten = dot(-to, L.dir_range.xyz) > 0.0 ? window * window : 0.0;  // rect / disc: in front only, the outline does the rest
            else atten *= light_profile(li, dot(-l, L.dir_range.xyz));
            if (light_functions[li * 2 + 1].w > 0.5) atten *= light_function(li, L.dir_range.xyz, -l);
        }
        const float ndl = saturate(dot(n, l));
        const bool panel = L.pos_type.w > 2.5;  // a Rect / Disc Light can light a point its centre is below the horizon of
        if ((ndl <= 0.0 && !panel) || atten <= 0.0) continue;
        if ((float)li == shadow_info.y) atten *= lightmapped ? min(sun_shadow(i.wpos, n, mat1.yzw), baked.a) : sun_shadow(i.wpos, n, mat1.yzw);
        else if (lightmapped && light_extra[li].x > 0.5) atten *= shadow_mask[(uint)light_extra[li].x - 1];  // Stationary: its baked shadow
        else if (L.spot.y >= 0.0) atten *= local_shadow(L, i.wpos, n);
        if (panel) {  // exact: the outline's share of the diffuse and (through the LTC) the specular lobe, times its radiance
            float3 p[8];
            float area;
            const uint count = panel_corners(L, i.wpos, t1, t2, n, p, area);
            const float diffuse_share = polygon_share(p, count);
            for (uint c = 0; c < count; ++c) p[c] = float3(ltc.x * p[c].x + ltc.y * p[c].z, ltc.z * p[c].y, ltc.w * p[c].x + p[c].z);
            const float3 fc = f0 + (1.0 - f0) * pow(1.0 - saturate(dot(v, normalize(v + l))), 5.0);
            lit += ((1.0 - fc) * (1.0 - metallic) * base.rgb * diffuse_share + env_spec * polygon_share(p, count)) * L.color_inner.xyz / max(area, 1e-4) * atten;
            continue;
        }
        if (hair) {  // Kajiya-Kay: a white highlight shifted toward the tip, a tinted one toward the root, soft wrap-around diffuse
            const float3 hh = normalize(v + l);
            const float sharp = lerp(300.0, 20.0, roughness);
            const float3 t1 = normalize(strand + n * 0.12), t2 = normalize(strand - n * 0.12);
            const float d1 = dot(t1, hh), d2 = dot(t2, hh);
            const float s1 = pow(sqrt(saturate(1.0 - d1 * d1)), sharp), s2 = pow(sqrt(saturate(1.0 - d2 * d2)), sharp * 0.25);
            const float wrap = saturate((dot(n, l) + 0.5) / 1.5);
            lit += (base.rgb / PI * wrap + (s1 * 0.2 + s2 * base.rgb * 0.4) * saturate(dot(n, l) + 0.3)) * L.color_inner.xyz * atten;
            continue;
        }
        float3 ls = l;  // specular's direction: a sphere light's nearest point (the centre otherwise)
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
    const float3 environment = reflection_info.y > 0.5 ? capture_sample((uint)reflection_info.z, r, roughness, i.wpos) : sky_radiance(r);  // the Sky's HDRI, or its gradient
    const float3 reflected = capture_reflection(lerp(environment * open_sky, sky, roughness * roughness), r, roughness, i.wpos);
    const float spec_ao = saturate(pow(ndv + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao);
    const float horizon = saturate(1.0 + 1.2 * dot(r, geometric_n));
    const float3 ambient = base.rgb * (1.0 - metallic) * (1.0 - env_spec) * sky * ao + env_spec * reflected * spec_ao * horizon * horizon;
    lit += ambient;
    lit += emissive;
    if (normal_view[3].x > 0.5) {  // the normals pass (SSR / AO): the normal as shaded, in view space; alpha: how much of the light is ambient (AO darkens only that)
        const float total = dot(lit, lum);
        return float4(normalize(float3(dot(normal_view[0].xyz, n), dot(normal_view[1].xyz, n), dot(normal_view[2].xyz, n))) * 0.5 + 0.5,
                      total > 1e-5 ? saturate(dot(ambient, lum) / total) : 1.0);
    }
    return float4(scene_out(lit * exposure, i.wpos, mat1.yzw, i.pos.xy, fog.w > 0.0 ? fog_light(i.wpos, mat1.yzw, i.pos.xy) : float3(1.0, 1.0, 1.0)), hdr_out.x > 0.5 && !blended ? (1.0 - roughness) * (1.0 - roughness) : base.a);  // scene target: how mirror-like (SSR); blended: opacity
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
    if (reflection_info.y > 0.5) c = capture_sample((uint)reflection_info.z, d, 0.0, mat1.yzw) * exposure;  // the Sky's HDRI (its tile, as drawn)
    if (w1.w > 0.0) {
        const float s = dot(d, w0.xyz);
        c += w1.rgb * (smoothstep(w0.w, w0.w + (1.0 - w0.w) * 0.3, s) + 0.25 * pow(saturate(s), 64.0) + 0.06 * pow(saturate(s), 6.0));  // disc + glow + the wide haze around the sun (Mie)
    }
    const float3 far_point = mat1.yzw + d * 5000.0;  // volumetric fog marches its View Distance even with nothing behind it
    return float4(scene_out(c, far_point, mat1.yzw, i.pos.xy, fog.w > 0.0 ? fog_light(far_point, mat1.yzw, i.pos.xy) : float3(1.0, 1.0, 1.0)), 1.0);
}
