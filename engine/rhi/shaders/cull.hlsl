// GPU-driven culling of instanced draws (MeshDraw::gpu_cull), shared by D3D12 (embedded as text,
// compiled at startup) and Vulkan (SPIR-V at build time): reset the visible count, test each copy's
// box against the frustum packing the visible rows, then one indirect draw's arguments per batch of 256.
#ifdef __spirv__  // Vulkan: the constants as push constants (144 bytes), the buffers by set / binding
#define VK_BIND(b, s) [[vk::binding(b, s)]]
struct CullConstants { float4 planes[6]; float4 box_min; float4 box_max; uint4 info; };
[[vk::push_constant]] CullConstants cull_pc;
#define planes cull_pc.planes
#define box_min cull_pc.box_min
#define box_max cull_pc.box_max
#define info cull_pc.info
#else
#define VK_BIND(b, s)
cbuffer Cull : register(b0) { float4 planes[6]; float4 box_min; float4 box_max; uint4 info; };  // info: count, index count, batches
#endif
VK_BIND(0, 0) ByteAddressBuffer rows_in : register(t0);
VK_BIND(1, 0) RWByteAddressBuffer rows_out : register(u0);
VK_BIND(2, 0) RWByteAddressBuffer args : register(u1);  // visible count, 12 bytes free, then 5 uints per batch
[numthreads(1, 1, 1)] void cull_reset() { args.Store(0, 0); }
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
    uint slot;
    args.InterlockedAdd(0, 1, slot);
    rows_out.Store4(slot * 48, asuint(r0));
    rows_out.Store4(slot * 48 + 16, asuint(r1));
    rows_out.Store4(slot * 48 + 32, asuint(r2));
}
[numthreads(64, 1, 1)] void cull_args(uint3 id : SV_DispatchThreadID) {
    uint b = id.x;
    if (b >= info.z) return;
    int n = clamp(int(args.Load(0)) - int(b * 256), 0, 256);
    args.Store(16 + b * 20, info.y);
    args.Store(20 + b * 20, uint(n));
    args.Store(24 + b * 20, 0);
    args.Store(28 + b * 20, 0);
    args.Store(32 + b * 20, 0);
}
