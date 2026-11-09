// GPU-driven culling of instanced draws (MeshDraw::gpu_cull), shared by D3D12 (embedded as text,
// compiled at startup) and Vulkan (SPIR-V at build time; OpenGL keeps a GLSL mirror): reset the visible
// counts, test each copy's box against the frustum and (occlusion_cull) the Hi-Z, pick its LOD by its size on
// screen and pack it into that LOD's rows, then one indirect draw's arguments per LOD and batch of 256.
// Layout of `args`: four visible counts (one per LOD), then 5 uints per (LOD, batch).
// HIZ_PASS builds the Hi-Z instead (hiz_down): its own constants and resources; MORPH_PASS mixes morph targets
// (morph_main, Device::morph_vertices).
#ifdef MORPH_PASS
#ifdef __spirv__
#define VK_BIND(b, s) [[vk::binding(b, s)]]
#else
#define VK_BIND(b, s)
#endif
VK_BIND(0, 0) ByteAddressBuffer morph_base : register(t0);      // rhi::SkinnedVertex, 40 bytes each
VK_BIND(1, 0) ByteAddressBuffer morph_deltas : register(t1);    // anim::pack_morphs
VK_BIND(2, 0) ByteAddressBuffer morph_weights : register(t2);   // the vertex count, 3 spare words, then one float per target
VK_BIND(3, 0) RWByteAddressBuffer morph_out : register(u0);
[numthreads(64, 1, 1)]
void morph_main(uint3 id : SV_DispatchThreadID) {
    const uint vertex_count = morph_weights.Load(0);
    const uint v = id.x;
    if (v >= vertex_count) return;
    const uint at = v * 40u;
    float3 p = asfloat(morph_base.Load3(at)), n = asfloat(morph_base.Load3(at + 12u));
    const uint2 range = morph_deltas.Load2(v * 8u);
    for (uint k = 0; k < range.y; ++k) {
        const uint e = vertex_count * 8u + (range.x + k) * 32u;
        const uint4 offset = morph_deltas.Load4(e);
        const float w = asfloat(morph_weights.Load(16u + offset.w * 4u));
        p += asfloat(offset.xyz) * w;
        n += asfloat(morph_deltas.Load3(e + 16u)) * w;
    }
    if (range.y > 0u && dot(n, n) > 1e-12) n = normalize(n);
    morph_out.Store3(at, asuint(p));
    morph_out.Store3(at + 12u, asuint(n));
    morph_out.Store4(at + 24u, morph_base.Load4(at + 24u));  // uv, joints, weights
}
#elif !defined(HIZ_PASS)
#ifdef __spirv__  // Vulkan: the constants as push constants (256 bytes), the buffers by set / binding
#define VK_BIND(b, s) [[vk::binding(b, s)]]
struct CullConstants { float4 planes[6]; float4 box_min; float4 box_max; uint4 info; uint4 index_counts; float4 lod; float4 camera; float4 hiz_vp[4]; };
[[vk::push_constant]] CullConstants cull_pc;
#define planes cull_pc.planes
#define box_min cull_pc.box_min
#define box_max cull_pc.box_max
#define info cull_pc.info
#define index_counts cull_pc.index_counts
#define lod cull_pc.lod
#define camera cull_pc.camera
#define hiz_vp cull_pc.hiz_vp
#else
#define VK_BIND(b, s)
// rhi::CullConstants: info = copies, batches per LOD, flags (1 Hi-Z; LOD count << 1); box_min.w / box_max.w the
// Hi-Z's size; lod = LOD 1..3 screen sizes, distance scale; camera.w the Hi-Z's mip count; hiz_vp its view-projection.
cbuffer Cull : register(b0) { float4 planes[6]; float4 box_min; float4 box_max; uint4 info; uint4 index_counts; float4 lod; float4 camera; float4 hiz_vp[4]; };
#endif
VK_BIND(0, 0) ByteAddressBuffer rows_in : register(t0);
VK_BIND(1, 0) RWByteAddressBuffer rows_out : register(u0);
VK_BIND(2, 0) RWByteAddressBuffer args : register(u1);
VK_BIND(3, 0) Texture2D<float> hiz : register(t1);  // the last frame's depth, the farthest of each 2 x 2 down its mips

static const uint kPerLod = 65536;  // rhi::kMaxCulledInstances: each LOD's rows start this many copies apart

[numthreads(1, 1, 1)] void cull_reset() { args.Store4(0, uint4(0, 0, 0, 0)); }

// Hidden behind what the Hi-Z held where the box was on screen last frame (its nearest point farther than the
// farthest depth over the 2 x 2 Hi-Z texels covering it). Not hidden when any corner was behind the camera or the
// box was off screen then.
bool occluded(float3 wc, float3 we) {
    float2 lo = float2(1.0, 1.0), hi = float2(-1.0, -1.0);
    float nearest = 1.0;
    [unroll] for (uint k = 0; k < 8; ++k) {
        const float3 p = wc + we * float3((k & 1u) ? 1.0 : -1.0, (k & 2u) ? 1.0 : -1.0, (k & 4u) ? 1.0 : -1.0);
        const float4 c = float4(dot(hiz_vp[0].xyz, p) + hiz_vp[0].w, dot(hiz_vp[1].xyz, p) + hiz_vp[1].w, dot(hiz_vp[2].xyz, p) + hiz_vp[2].w,
                                dot(hiz_vp[3].xyz, p) + hiz_vp[3].w);
        if (c.w <= 1e-4) return false;
        const float3 n = c.xyz / c.w;
        lo = min(lo, n.xy);
        hi = max(hi, n.xy);
        nearest = min(nearest, n.z);
    }
    if (any(hi < -1.0) || any(lo > 1.0) || any(lo < -1.0) || any(hi > 1.0)) return false;  // not wholly on screen then
    const float2 size = float2(box_min.w, box_max.w);
    const float2 uv_lo = float2(lo.x * 0.5 + 0.5, 0.5 - hi.y * 0.5), uv_hi = float2(hi.x * 0.5 + 0.5, 0.5 - lo.y * 0.5);  // uv run down
    const float2 span = (uv_hi - uv_lo) * size;
    const uint mip = min(uint(ceil(log2(max(max(span.x, span.y), 1.0)))), uint(camera.w) - 1u);
    const uint2 dims = max(uint2(size) >> mip, uint2(1, 1));
    const uint2 a = min(uint2(uv_lo * float2(dims)), dims - 1u), b = min(uint2(uv_hi * float2(dims)), dims - 1u);
    const float farthest = max(max(hiz.Load(int3(a, mip)), hiz.Load(int3(b.x, a.y, mip))), max(hiz.Load(int3(a.x, b.y, mip)), hiz.Load(int3(b, mip))));
    return nearest > farthest;
}

[numthreads(64, 1, 1)] void cull_main(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    if (i >= info.x) return;
    float4 r0 = asfloat(rows_in.Load4(i * 48)), r1 = asfloat(rows_in.Load4(i * 48 + 16)), r2 = asfloat(rows_in.Load4(i * 48 + 32));
    float3 c = (box_min.xyz + box_max.xyz) * 0.5, e = (box_max.xyz - box_min.xyz) * 0.5;
    float3 wc = float3(dot(r0.xyz, c) + r0.w, dot(r1.xyz, c) + r1.w, dot(r2.xyz, c) + r2.w);
    float3 we = float3(dot(abs(r0.xyz), e), dot(abs(r1.xyz), e), dot(abs(r2.xyz), e));
    [unroll] for (int p = 0; p < 6; ++p) {
        if (dot(planes[p].xyz, wc) + planes[p].w < -dot(abs(planes[p].xyz), we)) return;
    }
    if ((info.z & 1u) && occluded(wc, we)) return;
    // The LOD: bounding radius over distance (times the view distance scale) below LOD k's screen size (as on the CPU).
    const uint lods = (info.z >> 1) & 7u;
    const float size = length(we) * lod.w / max(length(wc - camera.xyz), 1e-3);
    uint l = 0;
    [unroll] for (uint k = 1; k < 4; ++k) {
        if (k < lods && size < lod[k - 1]) l = k;
    }
    uint slot;
    args.InterlockedAdd(l * 4, 1, slot);
    const uint at = (l * kPerLod + slot) * 48;
    rows_out.Store4(at, asuint(r0));
    rows_out.Store4(at + 16, asuint(r1));
    rows_out.Store4(at + 32, asuint(r2));
}

[numthreads(64, 1, 1)] void cull_args(uint3 id : SV_DispatchThreadID) {
    uint b = id.x;  // LOD * batches + batch
    if (b >= info.y * 4u) return;
    const uint l = b / info.y, batch = b % info.y;
    int n = clamp(int(args.Load(l * 4)) - int(batch * 256), 0, 256);
    args.Store(16 + b * 20, index_counts[l]);
    args.Store(20 + b * 20, uint(n));
    args.Store(24 + b * 20, 0);
    args.Store(28 + b * 20, 0);
    args.Store(32 + b * 20, 0);
}

#else
// The Hi-Z: mip 0 the farthest of each 2 x 2 of the scene's depth (src), each next mip the farthest of 2 x 2 of
// the one before (hiz_dst's size in hiz_size.xy, the source's in .zw: odd edges take the extra texel too).
#ifdef __spirv__
#define VK_BIND(b, s) [[vk::binding(b, s)]]
struct HizConstants { uint4 hiz_size; };
[[vk::push_constant]] HizConstants hiz_pc;
#define hiz_size hiz_pc.hiz_size
#else
#define VK_BIND(b, s)
cbuffer Hiz : register(b0) { uint4 hiz_size; };
#endif
VK_BIND(0, 0) Texture2D<float> hiz_src : register(t0);
VK_BIND(1, 0) RWTexture2D<float> hiz_dst : register(u0);
[numthreads(8, 8, 1)] void hiz_down(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= hiz_size.xy)) return;
    const uint2 s = id.xy * 2u, last = hiz_size.zw - 1u;
    float d = max(max(hiz_src.Load(int3(min(s, last), 0)), hiz_src.Load(int3(min(s + uint2(1, 0), last), 0))),
                  max(hiz_src.Load(int3(min(s + uint2(0, 1), last), 0)), hiz_src.Load(int3(min(s + uint2(1, 1), last), 0))));
    if (id.x * 2u + 2u == last.x) d = max(d, hiz_src.Load(int3(min(s + uint2(2, 0), last), 0)));  // odd width: the last column too
    if (id.y * 2u + 2u == last.y) d = max(d, hiz_src.Load(int3(min(s + uint2(0, 2), last), 0)));
    hiz_dst[id.xy] = d;
}
#endif
