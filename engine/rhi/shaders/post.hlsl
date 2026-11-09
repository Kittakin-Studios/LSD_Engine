// The post-process passes, shared by D3D12 (embedded as text, compiled at startup) and Vulkan
// (compiled to SPIR-V at build time; textures t0-t2 at bindings 0-2, the sampler at 18).
// The OpenGL backend keeps a GLSL mirror in step.
// Post-process passes over a full-screen triangle. Scene colours arrive range-compressed:
// e = sqrt(c / (1 + c)), so c = e^2 / (1 - e^2).
#ifdef __spirv__  // Vulkan (DXC to SPIR-V): the same constants as push constants
struct PostConstants { float4 texel; float4 bloom; float4 tint_tm; float4 grade; float4 proj; float4 ao_p; float4 dof; float4 taa; float4 reproject[4]; };
[[vk::push_constant]] PostConstants post_pc;
#define texel post_pc.texel
#define bloom post_pc.bloom
#define tint_tm post_pc.tint_tm
#define grade post_pc.grade
#define proj post_pc.proj
#define ao_p post_pc.ao_p
#define dof post_pc.dof
#define taa post_pc.taa
#define reproject post_pc.reproject
#else
cbuffer Post : register(b0) { float4 texel; float4 bloom; float4 tint_tm; float4 grade; float4 proj; float4 ao_p; float4 dof; float4 taa; float4 reproject[4]; };
#endif
Texture2D t0 : register(t0);
Texture2D t1 : register(t1);
Texture2D t2 : register(t2);  // the scene's depth
SamplerState clamp_linear : register(s2);
struct VsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VsOut vs_full(uint id : SV_VertexID) {
    VsOut o;
    const float2 p = float2((id << 1) & 2, id & 2) * 2.0 - 1.0;
    o.pos = float4(p, 0.0, 1.0);
    o.uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    return o;
}
float3 decode(float3 e) { e = min(e, 0.999); const float3 s = e * e; return s / (1.0 - s); }
float3 encode(float3 c) { c = max(c, 0.0); return sqrt(c / (1.0 + c)); }
float3 tap(Texture2D t, float2 uv) { return decode(t.SampleLevel(clamp_linear, uv, 0).rgb); }
float3 box4(Texture2D t, float2 uv, float2 px) {  // 4 bilinear taps: a 4x4 texel average
    return 0.25 * (tap(t, uv + px * float2(-1, -1)) + tap(t, uv + px * float2(1, -1)) + tap(t, uv + px * float2(-1, 1)) + tap(t, uv + px * float2(1, 1)));
}
float2 to_ndc(float2 uv) { return float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0); }
float2 to_uv(float2 ndc) { return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5); }
float depth_at(float2 uv) { return t2.SampleLevel(clamp_linear, uv, 0).r; }
float view_distance(float d) { return proj.w / (d + proj.z); }
float3 view_pos(float2 uv) {
    const float z = view_distance(depth_at(uv));
    const float2 ndc = to_ndc(uv);
    return float3(ndc.x * z / proj.x, ndc.y * z / proj.y, -z);
}
float coc(float z) { return saturate((abs(z - dof.x) - dof.y) / max(dof.w, 1e-3)) * dof.z; }  // blur radius in pixels
// Scalable ambient obscurance from depth alone; the normal from the nearer neighbour on each axis.
float4 ps_ao(VsOut i) : SV_Target {
    if (depth_at(i.uv) >= 0.99999) return 1.0;
    const float3 p = view_pos(i.uv);
    const float2 px = texel.xy;
    const float3 l = view_pos(i.uv - float2(px.x, 0)), r = view_pos(i.uv + float2(px.x, 0));
    const float3 b = view_pos(i.uv - float2(0, px.y)), t = view_pos(i.uv + float2(0, px.y));
    const float3 dx = abs(r.z - p.z) < abs(p.z - l.z) ? r - p : p - l;
    const float3 dy = abs(t.z - p.z) < abs(p.z - b.z) ? t - p : p - b;
    float3 n = normalize(cross(dx, dy));
    if (dot(n, p) > 0.0) n = -n;  // facing the camera
    if (texel.z > 0.5) n = normalize(t1.SampleLevel(clamp_linear, i.uv, 0).xyz * 2.0 - 1.0);  // the normals pass's (smooth, as shaded)
    const float radius = ao_p.x;
    const float2 scale = float2(proj.x, proj.y) * 0.5 * radius / -p.z;
    // A 4 x 4 Bayer pattern: the 4 x 4 blur in ps_ao_apply sees every rotation once, so the noise averages out exactly.
    const uint2 q = uint2(i.pos.xy) & 3u;
    const uint xy = q.x ^ q.y;
    const float noise = (float(((xy & 1u) << 3) | ((q.y & 1u) << 2) | (((xy >> 1) & 1u) << 1) | (q.y >> 1)) + 0.5) / 16.0;
    float sum = 0.0;
    for (int k = 0; k < 12; ++k) {
        const float a = (k + noise) * 2.39996323;
        const float2 offset = float2(cos(a), sin(a)) * sqrt((k + 0.5) / 12.0) * scale;
        const float3 v = view_pos(i.uv + offset) - p;
        const float vv = dot(v, v);
        const float f = max(radius * radius - vv, 0.0);
        // Samples within ~17 degrees of the surface's plane count for nothing: the normal is the flat triangle's, and on a
        // low-poly mesh shaded smooth those flip in and out triangle by triangle (facets, a stepped band on a sphere).
        sum += f * f * f * max((dot(v, n) - 0.3 * sqrt(vv) - 0.01 * radius) / (0.7 * (vv + 0.01)), 0.0);
    }
    float ao = max(0.0, 1.0 - sum * ao_p.y * 5.0 / (12.0 * pow(radius, 6.0)));
    if (texel.z > 0.5) ao = 1.0 - (1.0 - ao) * t1.SampleLevel(clamp_linear, i.uv, 0).a;  // the normals pass: only the ambient share darkens (direct light stays)
    // Contact shadows (reproject[0]: toward the sun in view space, w = length): a short ray through the
    // depth; a point behind what the camera sees there, but not far behind (thickness), is in shadow.
    if (reproject[0].w > 0.0 && dot(n, reproject[0].xyz) > 0.05) {  // faces the sun (else the sun does not light it anyway)
        [loop] for (int s = 0; s < 12; ++s) {
            const float3 q = p + reproject[0].xyz * (reproject[0].w * (s + noise) / 12.0);
            if (q.z > -0.05) break;
            const float2 uv = to_uv(float2(q.x * proj.x / -q.z, q.y * proj.y / -q.z));
            if (any(uv < 0.0) || any(uv > 1.0)) break;
            const float behind = -q.z - view_distance(depth_at(uv));
            if (behind > 0.008 * -q.z + 0.02 && behind < 0.25) {
                // shortcut: darkens the shaded colour (ambient too, and where the shadow map already shades);
                // the lit shader should take it off the sun's light only: needs a depth pre-pass.
                ao *= 0.6;
                break;
            }
        }
    }
    return float4(ao, ao, ao, 1.0);
}
// The occlusion blurred over 4 x 4 pixels at a similar depth (hides its noise), times the scene.
float4 ps_ao_apply(VsOut i) : SV_Target {
    const float z = view_distance(depth_at(i.uv));
    float total = 0.0, weight = 0.0;
    for (int y = -2; y <= 1; ++y) {
        for (int x = -2; x <= 1; ++x) {
            const float2 uv = i.uv + float2(x, y) * texel.xy;
            const float w = abs(view_distance(depth_at(uv)) - z) < 0.05 * z ? 1.0 : 0.0;
            total += t1.SampleLevel(clamp_linear, uv, 0).r * w;
            weight += w;
        }
    }
    const float ao = weight > 0.0 ? total / weight : 1.0;
    return float4(encode(tap(t0, i.uv) * ao), 1.0);
}
// The velocity target's movement in uv (LightSetup::velocity_view's encoding), and back.
float2 velocity_at(Texture2D t, float2 uv) {
    const float4 e = round(t.SampleLevel(clamp_linear, uv, 0) * 255.0);  // half-float targets: the bytes, exactly
    return (float2(e.x * 256.0 + e.y, e.z * 256.0 + e.w) / 65535.0 - 0.5) * 4.0;
}
float4 velocity_out(float2 move) { const float2 q = round(saturate(move * 0.25 + 0.5) * 65535.0); const float2 hi = floor(q / 256.0); return float4(hi.x, q.x - hi.x * 256.0, hi.y, q.y - hi.y * 256.0) / 255.0; }
// The velocity pass's movement (t1) where a mesh was drawn; elsewhere (sky, particles) the camera's, from the depth.
float4 ps_velocity(VsOut i) : SV_Target {
    const float4 e = t1.SampleLevel(clamp_linear, i.uv, 0);
    if (any(e > 0.0)) return e;
    const float4 here = float4(to_ndc(i.uv), depth_at(i.uv), 1.0);
    const float4 prev = float4(dot(reproject[0], here), dot(reproject[1], here), dot(reproject[2], here), dot(reproject[3], here));
    return velocity_out(prev.w > 0.0 ? i.uv - to_uv(prev.xy / prev.w) : float2(0.0, 0.0));
}
// This frame blended into the last one where this pixel was, clamped to this frame's neighbours. texel.w: where it was
// comes from the resolved velocity (t2) instead of the depth.
float4 ps_taa(VsOut i) : SV_Target {
    const float3 cur = t0.SampleLevel(clamp_linear, i.uv, 0).rgb;
    float3 lo = cur, hi = cur;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const float3 s = t0.SampleLevel(clamp_linear, i.uv + float2(x, y) * texel.xy, 0).rgb;
            lo = min(lo, s);
            hi = max(hi, s);
        }
    }
    const bool moved = texel.w > 0.5;
    const float4 here = float4(to_ndc(i.uv), moved ? 0.5 : depth_at(i.uv), 1.0);
    const float4 prev = float4(dot(reproject[0], here), dot(reproject[1], here), dot(reproject[2], here), dot(reproject[3], here));
    const float2 prev_uv = moved ? i.uv - velocity_at(t2, i.uv) : to_uv(prev.xy / prev.w);
    const bool outside = taa.y > 0.5 || (!moved && prev.w <= 0.0) || any(prev_uv < 0.0) || any(prev_uv > 1.0);
    const float3 history = clamp(t1.SampleLevel(clamp_linear, prev_uv, 0).rgb, lo, hi);
    return float4(outside ? cur : lerp(history, cur, taa.x), 1.0);
}
// Motion blur (taa.z amount, taa.w longest smear in uv): where this pixel was last frame, as TAA finds
// it, gives its movement on screen; 8 taps along it, centred on the pixel.
float4 ps_motion(VsOut i) : SV_Target {
    const float4 here = float4(to_ndc(i.uv), depth_at(i.uv), 1.0);
    const float4 prev = float4(dot(reproject[0], here), dot(reproject[1], here), dot(reproject[2], here), dot(reproject[3], here));
    float2 move = texel.w > 0.5 ? velocity_at(t1, i.uv) * taa.z : prev.w > 0.0 ? (i.uv - to_uv(prev.xy / prev.w)) * taa.z : float2(0.0, 0.0);  // texel.w: the velocity (t1)
    if (texel.w > 0.5) {  // a moving object smears over what is behind it too: the fastest nearby movement whose smear reaches here
        [unroll] for (int k = 0; k < 16; ++k) {
            const float a = k * 0.785398;  // 8 directions, at 4 and 12 pixels
            const float2 o = float2(cos(a), sin(a)) * (k < 8 ? 4.0 : 12.0) * texel.xy;
            const float2 m = velocity_at(t1, i.uv + o) * taa.z;
            if (dot(m, m) > dot(move, move) && length(m) * 0.5 >= length(o)) move = m;
        }
    }
    const float span = length(move);
    if (span > taa.w) move *= taa.w / span;
    if (span * (1.0 / texel.x) < 0.5) return float4(encode(tap(t0, i.uv)), 1.0);
    float3 sum = 0.0;
    const float jitter = frac(52.9829189 * frac(dot(i.pos.xy, float2(0.06711056, 0.00583715))));  // staggered per pixel: long smears show noise, not steps
    [unroll] for (int k = 0; k < 8; ++k) sum += tap(t0, i.uv + move * ((k + jitter) / 8.0 - 0.5));
    return float4(encode(sum / 8.0), 1.0);
}
// Gather over a disc as wide as this pixel's blur; a sample only counts if its own blur reaches here
// (a sharp object in front does not smear into the background behind it). Bokeh: bright samples weigh
// more, so out-of-focus lights open into discs (Unreal's Cinematic DOF) instead of fading into the blur.
// Samples brighter than 8 are scaled down first (one hot pixel would print the sample pattern as dots).
float3 tame(float3 c) { const float l = dot(c, float3(0.2126, 0.7152, 0.0722)); return l > 8.0 ? c * (8.0 / l) : c; }
float bokeh_weight(float3 c) { return 1.0 + min(4.0 * max(dot(c, float3(0.2126, 0.7152, 0.0722)) - 1.0, 0.0), 4.0); }
float4 ps_dof(VsOut i) : SV_Target {
    const float radius = coc(view_distance(depth_at(i.uv)));
    const float3 centre = tame(tap(t0, i.uv));
    float weight = bokeh_weight(centre);
    float3 sum = centre * weight;
    if (radius > 0.5) {
        for (int k = 0; k < 40; ++k) {
            const float a = k * 2.39996323;
            const float r = sqrt((k + 0.5) / 40.0) * radius;
            const float2 uv = i.uv + float2(cos(a), sin(a)) * r * texel.xy;
            const float3 c = tame(box4(t0, uv, texel.xy * max(0.5, radius * 0.15)));  // a footprint as wide as the gap between samples: a disc, not dots
            const float w = saturate(coc(view_distance(depth_at(uv))) - r + 1.0) * bokeh_weight(c);
            sum += c * w;
            weight += w;
        }
    }
    return float4(encode(sum / weight), 1.0);
}
// Screen-space reflections: the view ray mirrored about the surface, marched through the depth buffer;
// where it passes just behind a surface, that pixel's colour is reflected. The mesh pass wrote how
// mirror-like each pixel is into alpha (smooth = 1).
float4 ps_ssr(VsOut i) : SV_Target {
    const float4 scene = t0.SampleLevel(clamp_linear, i.uv, 0);
    float3 c = decode(scene.rgb);
    if (scene.a < 0.02 || depth_at(i.uv) >= 0.99999) return float4(encode(c), 1.0);
    const float3 p = view_pos(i.uv);
    const float2 px = texel.xy;
    const float3 l = view_pos(i.uv - float2(px.x, 0)), r = view_pos(i.uv + float2(px.x, 0));
    const float3 b = view_pos(i.uv - float2(0, px.y)), t = view_pos(i.uv + float2(0, px.y));
    const float3 dx = abs(r.z - p.z) < abs(p.z - l.z) ? r - p : p - l;
    const float3 dy = abs(t.z - p.z) < abs(p.z - b.z) ? t - p : p - b;
    float3 n = normalize(cross(dx, dy));
    if (dot(n, p) > 0.0) n = -n;
    if (texel.z > 0.5) n = normalize(t1.SampleLevel(clamp_linear, i.uv, 0).xyz * 2.0 - 1.0);  // the normals pass's (smooth, as shaded)
    const float3 ray = reflect(normalize(p), n);
    if (ray.z > 0.4) return float4(encode(c), 1.0);  // back towards the camera: nothing on screen to show
    const float reach = ao_p.w;
    const float noise = frac(52.9829189 * frac(dot(i.pos.xy, float2(0.06711056, 0.00583715))));  // hides the steps (TAA smooths it)
    float before = 0.0, was_behind = -1.0;
    for (int k = 1; k <= 32; ++k) {
        const float f = (k - 1 + noise) / 32.0;
        const float along = reach * f * f + 0.05;
        const float3 q = p + ray * along;
        if (q.z > -0.05) break;
        const float2 uv = to_uv(float2(q.x * proj.x / -q.z, q.y * proj.y / -q.z));
        if (any(uv < 0.0) || any(uv > 1.0)) break;
        const float behind = -q.z - view_distance(depth_at(uv));
        if (behind > 0.02 && was_behind <= 0.02 && behind < 1.0 + along * 0.15) {  // crossed into a surface: refine
            float lo = before, hi = along;  // bisect to the surface
            float2 hit = uv;
            for (int s = 0; s < 4; ++s) {
                const float mid = (lo + hi) * 0.5;
                const float3 m = p + ray * mid;
                const float2 muv = to_uv(float2(m.x * proj.x / -m.z, m.y * proj.y / -m.z));
                if (-m.z - view_distance(depth_at(muv)) > 0.0) { hi = mid; hit = muv; } else { lo = mid; }
            }
            const float2 edge = min(hit, 1.0 - hit);
            const float fade = saturate(min(edge.x, edge.y) * 10.0) * (1.0 - f) * saturate((0.4 - ray.z) * 2.5);
            c = lerp(c, tap(t0, hit), saturate(scene.a * ao_p.z * fade));
            break;
        }
        before = along;
        was_behind = behind;
    }
    return float4(encode(c), 1.0);
}
float4 ps_prefilter(VsOut i) : SV_Target {  // bright parts only, with a soft knee
    const float3 c = box4(t0, i.uv, texel.xy);
    const float brightness = max(c.r, max(c.g, c.b));
    const float knee = bloom.x * 0.5;
    float soft = clamp(brightness - bloom.x + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-5);
    const float keep = max(soft, brightness - bloom.x) / max(brightness, 1e-5);
    return float4(encode(c * keep), 1.0);
}
float4 ps_down(VsOut i) : SV_Target { return float4(encode(box4(t0, i.uv, texel.xy)), 1.0); }
float4 ps_up(VsOut i) : SV_Target {  // this level plus the blurred (tent) level below it
    const float2 px = texel.zw;
    float3 c = tap(t1, i.uv) * 4.0;
    c += (tap(t1, i.uv + float2(-px.x, 0)) + tap(t1, i.uv + float2(px.x, 0)) + tap(t1, i.uv + float2(0, -px.y)) + tap(t1, i.uv + float2(0, px.y))) * 2.0;
    c += tap(t1, i.uv + px * float2(-1, -1)) + tap(t1, i.uv + px * float2(1, -1)) + tap(t1, i.uv + px * float2(-1, 1)) + tap(t1, i.uv + px * float2(1, 1));
    return float4(encode(tap(t0, i.uv) + c / 16.0), 1.0);
}
float3 aces(float3 x) { return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)); }
float3 to_srgb(float3 c) { c = saturate(c); return lerp(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, step(c, 0.0031308)); }  // no vector ?: (SPIR-V)
// Eye adaptation (see the GLSL): metered log average -> eased exposure in 16 bits (r high, g low).
float4 ps_meter(VsOut i) : SV_Target {
    float sum = 0.0, weights = 0.0;
    [loop] for (int y = 0; y < 32; ++y) {
        [loop] for (int x = 0; x < 32; ++x) {
            const float2 uv = (float2(x, y) + 0.5) / 32.0;
            const float w = 1.0 - 0.75 * min(length(uv - 0.5) * 1.4142, 1.0);
            sum += log2(max(dot(tap(t0, uv), float3(0.2126, 0.7152, 0.0722)), 1e-4)) * w;
            weights += w;
        }
    }
    const float target = clamp(log2(0.18) - sum / weights + bloom.z, bloom.x, bloom.y);
    const float4 prev = t1.SampleLevel(clamp_linear, float2(0.5, 0.5), 0);
    const float last = (prev.r + prev.g / 255.0) * 32.0 - 16.0;
    const float ev = bloom.w > 0.5 ? target : lerp(last, target, 1.0 - exp(-(target > last ? tint_tm.x : tint_tm.y)));
    const float stored = saturate((ev + 16.0) / 32.0);
    const float high = floor(stored * 255.0) / 255.0;
    return float4(high, (stored - high) * 255.0, 0.0, 1.0);
}
float4 ps_expose(VsOut i) : SV_Target {
    const float4 e = t1.SampleLevel(clamp_linear, float2(0.5, 0.5), 0);
    return float4(encode(tap(t0, i.uv) * exp2((e.r + e.g / 255.0) * 32.0 - 16.0)), 1.0);
}
// The selection outline (see the GLSL): empty here in the mask, something within 2 pixels; tint_tm = colour.
float4 ps_outline(VsOut i) : SV_Target {
    const float here = t0.SampleLevel(clamp_linear, i.uv, 0).r;
    float around = 0.0;
    [unroll] for (int y = -2; y <= 2; ++y) {
        [unroll] for (int x = -2; x <= 2; ++x) around = max(around, t0.SampleLevel(clamp_linear, i.uv + float2(x, y) * texel.xy, 0).r);
    }
    return float4(tint_tm.rgb, saturate(around * 4.0) * (1.0 - saturate(here * 4.0)) * tint_tm.a);
}
float3 lut(float3 s) {  // a 256 x 16 strip: 16 tiles of 16 x 16 (red across, green down), blue picks the tile
    const float b = saturate(s.b) * 15.0, b0 = floor(b);
    const float2 inner = (saturate(s.rg) * 15.0 + 0.5) / float2(256.0, 16.0);
    const float3 lo = t2.SampleLevel(clamp_linear, inner + float2(b0 / 16.0, 0.0), 0).rgb;
    const float3 hi = t2.SampleLevel(clamp_linear, inner + float2(min(b0 + 1.0, 15.0) / 16.0, 0.0), 0).rgb;
    return lerp(lo, hi, b - b0);
}
#ifdef LSD_POST_MATERIAL  // a post-process material: the backend defines this and puts its code at the POST marker
struct PostIn { float2 uv; float3 color; float time; float4 params; };
float3 scene_color(float2 uv) { return tap(t0, uv); }  // the scene, linear and exposed, before tonemapping
float2 texel_size() { return texel.xy; }
#endif
//@POST
float4 ps_composite(VsOut i) : SV_Target {
    float3 c = tap(t0, i.uv);
    if (bloom.y > 0.0) c += tap(t1, i.uv) * bloom.y;
#ifdef LSD_POST_MATERIAL
    {  // its time and parameters ride in proj (unused by this pass)
        PostIn pi;
        pi.uv = i.uv;
        pi.color = c;
        pi.time = proj.x;
        pi.params = float4(proj.yzw, 0.0);
        c = max(post_main(pi), 0.0);
    }
#endif
    c *= tint_tm.rgb;
    if (tint_tm.w > 0.5) c = aces(c);
    const float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = max(lerp(luma.xxx, c, grade.x), 0.0);
    c = max((c - 0.18) * grade.y + 0.18, 0.0);
    const float2 v = i.uv - 0.5;
    c *= 1.0 - grade.z * smoothstep(0.1, 0.5, dot(v, v));
    float3 s = to_srgb(c);
    if (grade.w > 0.0) s = lerp(s, lut(s), grade.w);
    return float4(s, dot(s, float3(0.299, 0.587, 0.114)));  // luma in alpha for FXAA
}
float4 ps_fxaa(VsOut i) : SV_Target {  // FXAA: blur only across edges, along their direction
    const float2 px = texel.xy;
    const float3 m = t0.SampleLevel(clamp_linear, i.uv, 0).rgb;
    const float l_nw = t0.SampleLevel(clamp_linear, i.uv + px * float2(-1, -1), 0).a, l_ne = t0.SampleLevel(clamp_linear, i.uv + px * float2(1, -1), 0).a;
    const float l_sw = t0.SampleLevel(clamp_linear, i.uv + px * float2(-1, 1), 0).a, l_se = t0.SampleLevel(clamp_linear, i.uv + px * float2(1, 1), 0).a;
    const float l_m = t0.SampleLevel(clamp_linear, i.uv, 0).a;
    const float l_min = min(l_m, min(min(l_nw, l_ne), min(l_sw, l_se))), l_max = max(l_m, max(max(l_nw, l_ne), max(l_sw, l_se)));
    float2 dir = float2(-((l_nw + l_ne) - (l_sw + l_se)), (l_nw + l_sw) - (l_ne + l_se));
    const float reduce = max((l_nw + l_ne + l_sw + l_se) * 0.03125, 1.0 / 128.0);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), -8.0, 8.0) * px;
    const float3 a = 0.5 * (t0.SampleLevel(clamp_linear, i.uv + dir * (1.0 / 3.0 - 0.5), 0).rgb + t0.SampleLevel(clamp_linear, i.uv + dir * (2.0 / 3.0 - 0.5), 0).rgb);
    const float3 b = a * 0.5 + 0.25 * (t0.SampleLevel(clamp_linear, i.uv - dir * 0.5, 0).rgb + t0.SampleLevel(clamp_linear, i.uv + dir * 0.5, 0).rgb);
    const float l_b = dot(b, float3(0.299, 0.587, 0.114));
    return float4((l_b < l_min || l_b > l_max) ? a : b, 1.0);
}
