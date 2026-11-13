// Direct3D 12 backend: adapter enumeration, live memory budget, swapchain, depth buffer,
// immutable buffers, built-in lit-mesh / line / test-triangle pipelines and ImGui.
#include "lsd/rhi/rhi.h"
#include "lsd/rhi/shader_cache.h"
#include "lsd/foundation/log.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_dx12.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <utility>
#include <vector>
#include <string>
#include <new>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace lsd::rhi {

namespace {

constexpr u32 kBackBufferCount = 2;
constexpr u32 kMaxAdapters = 16;
constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
// Render targets (scene, post chain, previews) are half float: the scene's range-compressed colour keeps 10 bits of
// mantissa instead of 8 (no banding in dark gradients). Only what draws to the window uses kBackBufferFormat.
constexpr DXGI_FORMAT kTargetFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr u32 kDebugUiDescriptors = 4096;  // ImGui font atlas, render targets and textures (the meshes sample them too)
constexpr u32 kMaxTextures = 2048;  // handles kMaxTargets.. (render targets use 0..kMaxTargets-1)
constexpr u32 kMaxTargets = 256;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
constexpr u32 kMaxBuffers = 4096;
constexpr u32 kRootConstants = 52;  // one root-constant block shared by every built-in pipeline (+ 5 texture tables + lights and joints CBVs: 61 of 64 DWORDs)
constexpr u32 kLightSlot = static_cast<u32>((sizeof(lsd::rhi::LightSetup) + 255) & ~static_cast<size_t>(255));  // rounded up to the CBV alignment
constexpr u32 kMaxShadowMaps = 8;    // handles kMaxTargets + kMaxTextures..
constexpr u32 kLightSlots = 64;      // set_lights calls per frame before the ring wraps
constexpr u32 kJointRingBytes = 4 * 1024 * 1024;  // skinning matrices per frame (about 340 characters at 256 joints)

// shortcut: runtime HLSL compile via d3dcompiler_47 (ships with Windows); move to DXC + the data
// compiler once shaders become assets.
constexpr char kTriangleHlsl[] = R"(
struct VsOut { float4 pos : SV_Position; float3 color : COLOR; };
cbuffer Params : register(b0) { float scale; float angle; float aspect; };
VsOut vs_main(uint id : SV_VertexID) {
    const float2 pos[3]   = { float2(0.0, 0.6), float2(0.55, -0.5), float2(-0.55, -0.5) };
    const float3 color[3] = { float3(1, 0.2, 0.2), float3(0.2, 1, 0.2), float3(0.2, 0.4, 1) };
    VsOut o;
    float s, c;
    sincos(angle, s, c);
    const float2 p = mul(float2x2(c, -s, s, c), pos[id] * scale);
    o.pos = float4(p.x / aspect, p.y, 0, 1);
    o.color = color[id];
    return o;
}
float4 ps_main(VsOut i) : SV_Target { return float4(i.color, 1); }
)";

// Lit mesh, physically based (metal / roughness, GGX): one sun plus sky / ground ambient. Constants
// match rhi::MeshDraw; n0.w carries the unlit flag, n1.w "has a normal map". Output is encoded to
// sRGB by hand (the swapchain is UNORM so the UI keeps its exact colours).
#include "mesh_hlsl.h"  // kMeshHlsl: engine/rhi/shaders/mesh.hlsl

#include "material_prelude_hlsl.h"  // kMaterialPreludeHlsl: engine/rhi/shaders/material_prelude.hlsl
#include "cull_hlsl.h"  // kCullHlsl: engine/rhi/shaders/cull.hlsl
#include "gpu_particles_hlsl.h"  // kGpuParticlesHlsl: engine/rhi/shaders/gpu_particles.hlsl

// Shadow casters: depth only (SKINNED: skinned first).
constexpr char kShadowHlsl[] = R"(
cbuffer Draw : register(b0) { float4x4 mvp; float4 cut; };  // cut (Masked casters): x the alpha cutoff, y the tint's alpha, zw the Tiling
#ifdef CUTOUT
Texture2D base_tex : register(t0);
SamplerState samp : register(s0);
struct ShadowOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
#ifdef SKINNED
cbuffer Joints : register(b2) { float4 joint_rows[768]; };
ShadowOut vs_main(float3 pos : POSITION, float2 uv : TEXCOORD0, uint4 joints : BLENDINDICES, float4 weights : BLENDWEIGHT) {
    float4 r0 = 0, r1 = 0, r2 = 0;
    [unroll] for (int k = 0; k < 4; ++k) {
        r0 += joint_rows[joints[k] * 3] * weights[k];
        r1 += joint_rows[joints[k] * 3 + 1] * weights[k];
        r2 += joint_rows[joints[k] * 3 + 2] * weights[k];
    }
    const float4 p = float4(pos, 1);
    ShadowOut o;
    o.pos = mul(mvp, float4(dot(r0, p), dot(r1, p), dot(r2, p), 1));
    o.uv = uv;
    return o;
}
#else
ShadowOut vs_main(float3 pos : POSITION, float2 uv : TEXCOORD0) {
    ShadowOut o;
    o.pos = mul(mvp, float4(pos, 1));
    o.uv = uv;
    return o;
}
#endif
void ps_cutout(ShadowOut i) { if (base_tex.Sample(samp, i.uv * cut.zw).a * cut.y < cut.x) discard; }
#elif defined(SKINNED)
cbuffer Joints : register(b2) { float4 joint_rows[768]; };
float4 vs_main(float3 pos : POSITION, uint4 joints : BLENDINDICES, float4 weights : BLENDWEIGHT) : SV_Position {
    float4 r0 = 0, r1 = 0, r2 = 0;
    [unroll] for (int k = 0; k < 4; ++k) {
        r0 += joint_rows[joints[k] * 3] * weights[k];
        r1 += joint_rows[joints[k] * 3 + 1] * weights[k];
        r2 += joint_rows[joints[k] * 3 + 2] * weights[k];
    }
    const float4 p = float4(pos, 1);
    return mul(mvp, float4(dot(r0, p), dot(r1, p), dot(r2, p), 1));
}
#elif defined(INSTANCED)
cbuffer Instances : register(b2) { float4 instance_rows[768]; };  // 3 rows per copy: its world matrix
float4 vs_main(float3 pos : POSITION, uint instance : SV_InstanceID) : SV_Position {
    const float4 p = float4(pos, 1);
    const float4 r0 = instance_rows[instance * 3], r1 = instance_rows[instance * 3 + 1], r2 = instance_rows[instance * 3 + 2];
    return mul(mvp, float4(dot(r0, p), dot(r1, p), dot(r2, p), 1));
}
#else
float4 vs_main(float3 pos : POSITION) : SV_Position { return mul(mvp, float4(pos, 1)); }
#endif
)";

constexpr char kLinesHlsl[] = R"(
cbuffer Draw : register(b0) { float4x4 mvp; };
struct VsIn { float3 pos : POSITION; float4 color : COLOR; };
struct VsOut { float4 pos : SV_Position; float4 color : COLOR; };
VsOut vs_main(VsIn i) { VsOut o; o.pos = mul(mvp, float4(i.pos, 1)); o.color = i.color; return o; }
float4 ps_main(VsOut i) : SV_Target { return i.color; }
)";

constexpr char kParticleHlsl[] = R"(
cbuffer Draw : register(b0) { float4x4 view_proj; float4 mode; float4 soft; };  // mode.x = 1: into a scene target (post pass); soft: see draw_particles
Texture2D sprite : register(t0);
Texture2D<float> scene_depth : register(t1);  // the target's depth (soft particles)
float view_distance(float d) { return soft.w > 0.5 ? (soft.z - d) / soft.y : soft.z / (d + soft.y); }
SamplerState linear_wrap : register(s0);
struct VsIn { float3 pos : POSITION; float4 color : COLOR; float2 uv : TEXCOORD; };
struct VsOut { float4 pos : SV_Position; float4 color : COLOR; float2 uv : TEXCOORD; };
VsOut vs_main(VsIn i) { VsOut o; o.pos = mul(view_proj, float4(i.pos, 1)); o.color = i.color; o.uv = i.uv; return o; }
float3 to_srgb(float3 c) { c = saturate(c); return lerp(c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055, step(0.0031308, c)); }
float4 ps_main(VsOut i) : SV_Target {
    float4 c = i.color * sprite.Sample(linear_wrap, i.uv);
    if (soft.x > 0.0) c.a *= saturate((view_distance(scene_depth.Load(int3(i.pos.xy, 0))) - view_distance(i.pos.z)) / soft.x);  // fades into surfaces
    const float3 lin = max(c.rgb, 0.0);
    return float4(mode.x > 0.5 ? sqrt(lin / (1.0 + lin)) : to_srgb(c.rgb), saturate(c.a));
}
)";

const D3D12_INPUT_ELEMENT_DESC kMeshLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},  // the lightmap UV
};
const D3D12_INPUT_ELEMENT_DESC kSkinnedLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"BLENDWEIGHT", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
};
const D3D_SHADER_MACRO kSkinnedDefines[] = {{"SKINNED", "1"}, {nullptr, nullptr}};
const D3D_SHADER_MACRO kInstancedDefines[] = {{"INSTANCED", "1"}, {nullptr, nullptr}};
const D3D_SHADER_MACRO kCutoutDefines[] = {{"CUTOUT", "1"}, {nullptr, nullptr}};
const D3D_SHADER_MACRO kSkinnedCutoutDefines[] = {{"CUTOUT", "1"}, {"SKINNED", "1"}, {nullptr, nullptr}};

constexpr char kUiHlsl[] = R"(
cbuffer Draw : register(b0) { float2 translation; float2 to_clip; };  // to_clip = 2 / viewport size
Texture2D tex : register(t0);
SamplerState samp : register(s0);
struct VsIn { float2 pos : POSITION; float4 color : COLOR; float2 uv : TEXCOORD; };
struct VsOut { float4 pos : SV_Position; float4 color : COLOR; float2 uv : TEXCOORD; };
VsOut vs_main(VsIn i) {
    VsOut o;
    const float2 p = (i.pos + translation) * to_clip;
    o.pos = float4(p.x - 1.0, 1.0 - p.y, 0.0, 1.0);
    o.color = i.color;
    o.uv = i.uv;
    return o;
}
float4 ps_main(VsOut i) : SV_Target { return i.color * tex.Sample(samp, i.uv); }
)";

#include "post_hlsl.h"  // kPostHlsl: engine/rhi/shaders/post.hlsl

std::atomic<u32> g_shader_compiles{0}, g_shader_cache_hits{0};  // the start-up line: shaders compiled, read back, and the time compiling took
std::atomic<u64> g_shader_compile_ns{0};
// The compiler and its flags, part of each cached shader's key (a change here compiles every shader again).
constexpr char kShaderCompilerId[] = "d3dcompiler_47 flags 0 0";

bool device_removed(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

const char* removed_reason_name(HRESULT hr) {
    switch (hr) {
        case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG: the GPU stopped responding (a timeout or a GPU page fault)";
        case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET: the GPU was reset";
        case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED: the driver crashed or was updated, or the GPU was removed";
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR: the driver failed";
        case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL: the GPU was sent invalid work";
        default: return "unknown reason";
    }
}

// Calls that failed because the device was removed: the first is logged, the rest only counted (the log
// used to fill with one line per resource until the editor was closed).
u32 g_removed_failures = 0;

bool check(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) return true;
    if (device_removed(hr)) {
        if (g_removed_failures++ == 0) LSD_LOG_ERROR("d3d12", "%s failed: the GPU device was removed (HRESULT 0x%08X)", what, static_cast<unsigned>(hr));
        return false;
    }
    LSD_LOG_ERROR("d3d12", "%s failed (HRESULT 0x%08X)", what, static_cast<unsigned>(hr));
    return false;
}

Vendor vendor_from_id(UINT id) {
    switch (id) {
        case 0x10DE: return Vendor::Nvidia;
        case 0x1002: return Vendor::Amd;
        case 0x8086: return Vendor::Intel;
        default:     return Vendor::Other;
    }
}

const char* to_string(AdapterType type) {
    switch (type) {
        case AdapterType::Discrete:   return "discrete";
        case AdapterType::Integrated: return "integrated";
        case AdapterType::Software:   return "software";
    }
    return "?";
}

class D3D12Device final : public Device {
    struct MaterialShader {
        ComPtr<ID3D12PipelineState> mesh, skinned;
        ComPtr<ID3D12PipelineState> blended[2][2];  // [static, skinned][translucent, additive]
        u32 generation = 0;
    };
    static constexpr u32 kMaxMaterialShaders = 256;

public:
    ~D3D12Device() override {
        if (g_removed_failures > 1) LSD_LOG_WARN("d3d12", "%u more calls failed after the GPU device was removed", g_removed_failures - 1);
        if (queue_ && fence_ && fence_event_) wait_for_gpu();
        if (debug_ui_) ImGui_ImplDX12_Shutdown();
        if (fence_event_) CloseHandle(fence_event_);
    }

    bool init(const DeviceDesc& desc) {
        UINT factory_flags = 0;
        if (desc.enable_validation) {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
                debug->EnableDebugLayer();
                factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
                LSD_LOG_INFO("d3d12", "Debug layer enabled.");
            } else {
                LSD_LOG_WARN("d3d12", "Debug layer unavailable (install 'Graphics Tools' in Windows optional features).");
            }
        }
        if (desc.enable_validation || desc.gpu_debug) {  // DRED: what the GPU was doing, and the address it faulted on, if it fails
            ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
                dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred_ = true;
                LSD_LOG_INFO("d3d12", "GPU crash data (DRED) on: breadcrumbs and page faults are logged if the device is removed.");
            } else {
                LSD_LOG_WARN("d3d12", "GPU crash data (DRED) unavailable.");
            }
        }
        if (!check(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory_)), "CreateDXGIFactory2")) return false;

        // Enumerate every adapter that can run D3D12 at feature level 12.0.
        AdapterInfo infos[kMaxAdapters];
        ComPtr<IDXGIAdapter1> adapters[kMaxAdapters];
        u32 count = 0;
        ComPtr<IDXGIAdapter1> candidate;
        for (UINT i = 0; count < kMaxAdapters && factory_->EnumAdapters1(i, &candidate) != DXGI_ERROR_NOT_FOUND; ++i) {
            ComPtr<ID3D12Device> probe;
            if (FAILED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&probe)))) continue;
            fill_adapter_info(candidate.Get(), probe.Get(), count, infos[count]);
            adapters[count] = candidate;
            ++count;
        }
        if (count == 0) {
            LSD_LOG_ERROR("d3d12", "No adapter supports Direct3D 12 feature level 12.0.");
            return false;
        }

        if (desc.adapter_override >= static_cast<i32>(count)) {
            LSD_LOG_WARN("d3d12", "-gpu=%d is out of range (%u adapters); selecting automatically.", desc.adapter_override, count);
        }
        const i32 chosen = select_adapter(infos, count, desc);
        for (u32 i = 0; i < count; ++i) {
            const AdapterInfo& a = infos[i];
            LSD_LOG_INFO("d3d12", "%s GPU %u: %s (%s, %llu MiB VRAM, %llu MiB shared, RT %s, mesh shaders %s)",
                         static_cast<i32>(i) == chosen ? "*" : " ", i, a.name, to_string(a.type),
                         static_cast<unsigned long long>(a.dedicated_vram_bytes / MiB),
                         static_cast<unsigned long long>(a.shared_memory_bytes / MiB),
                         a.supports_raytracing ? "yes" : "no", a.supports_mesh_shaders ? "yes" : "no");
        }
        info_ = infos[chosen];
        if (!check(adapters[chosen].As(&adapter_), "IDXGIAdapter3 query")) return false;
        if (!check(D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_)), "D3D12CreateDevice")) return false;
        if (desc.enable_validation) forward_debug_messages();

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (!check(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_)), "CreateCommandQueue")) return false;
        for (u32 i = 0; i < kBackBufferCount; ++i) {
            if (!check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators_[i])), "CreateCommandAllocator")) return false;
        }
        if (!check(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(), nullptr, IID_PPV_ARGS(&list_)), "CreateCommandList")) return false;
        list_->Close();
        if (!check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence")) return false;
        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fence_event_) return false;

        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = kBackBufferCount;
        if (!check(device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap_)), "CreateDescriptorHeap")) return false;
        rtv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heap_desc.NumDescriptors = 1;
        if (!check(device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dsv_heap_)), "CreateDescriptorHeap (DSV)")) return false;
        heap_desc.NumDescriptors = kMaxTargets * 2;  // each target's DSV, then its read-only one (soft particles)
        if (!check(device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&target_dsv_heap_)), "CreateDescriptorHeap (target DSV)")) return false;
        heap_desc.NumDescriptors = kMaxTargets;
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (!check(device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&target_rtv_heap_)), "CreateDescriptorHeap (target RTV)")) return false;
        dsv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

        const u64 started = SDL_GetTicksNS();
        if (!create_pipelines()) return false;
        if (!create_culling()) {  // optional: gpu_cull draws then draw every instance
            cull_signature_.Reset();
            LSD_LOG_WARN("d3d12", "GPU culling is unavailable on this device");
        }
        if (!create_morphs()) morph_pso_.Reset();  // optional: morphs then mix on the CPU
        if (!create_gpu_particles()) gpu_particle_add_.Reset();  // optional: GPU emitters then run on the CPU
        LSD_LOG_INFO("d3d12", "Built-in pipelines: %.0f ms (%u shaders compiled in %.0f ms, %u read from the shader cache)", static_cast<double>(SDL_GetTicksNS() - started) * 1e-6,
                     g_shader_compiles.load(), static_cast<double>(g_shader_compile_ns.load()) * 1e-6, g_shader_cache_hits.load());
        return true;
    }

    Backend backend() const override { return Backend::D3D12; }
    bool supports_gpu_culling() const override { return cull_signature_ != nullptr; }
    bool supports_gpu_morphs() const override { return morph_pso_ != nullptr; }
    BufferHandle create_morph_output(u32 vertex_count) override {
        u32 slot = 0;
        while (slot < kMaxBuffers && buffers_[slot].resource) ++slot;
        if (slot == kMaxBuffers || vertex_count == 0) return {};
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = static_cast<u64>(vertex_count) * sizeof(SkinnedVertex);
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Buffer& b = buffers_[slot];  // COMMON: promoted to UAV by the morph pass, decays back after each frame
        if (!check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                    IID_PPV_ARGS(&b.resource)), "CreateCommittedResource (morph output)")) return {};
        b.size = desc.Width;
        b.usage = BufferUsage::Vertex;
        return BufferHandle::make(slot, b.generation);
    }
    void morph_vertices(BufferHandle out, BufferHandle base, BufferHandle morphs, u32 vertex_count, const f32* weights, u32 weight_count) override {
        MorphJob* job = nullptr;  // one job per output: a later change this frame replaces it
        for (MorphJob& j : pending_morphs_) {
            if (j.out.value == out.value) job = &j;
        }
        if (!job) job = &pending_morphs_.emplace_back();
        morph_job(*job, out, base, morphs, vertex_count, weights, weight_count);
    }
    bool culls_shadow_instances() const override { return cull_signature_ != nullptr; }
    const AdapterInfo& adapter() const override { return info_; }

    MemoryBudget query_memory_budget() const override {
        DXGI_QUERY_VIDEO_MEMORY_INFO mem{};
        adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mem);
        return MemoryBudget{mem.Budget, mem.CurrentUsage};
    }

    bool create_swapchain(SDL_Window* window, u32 width, u32 height) override {
        DXGI_SWAP_CHAIN_DESC1 sc{};
        sc.Width = width;
        sc.Height = height;
        sc.Format = kBackBufferFormat;
        sc.SampleDesc.Count = 1;
        sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc.BufferCount = kBackBufferCount;
        sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        const HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        ComPtr<IDXGISwapChain1> swapchain1;
        if (!check(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sc, nullptr, nullptr, &swapchain1), "CreateSwapChainForHwnd")) return false;
        factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        if (!check(swapchain1.As(&swapchain_), "IDXGISwapChain3 query")) return false;
        width_ = width;
        height_ = height;
        create_back_buffer_views();
        return create_depth_buffer();
    }

    void resize_swapchain(u32 width, u32 height) override {
        if (!swapchain_ || width == 0 || height == 0 || (width == width_ && height == height_)) return;
        wait_for_gpu();
        for (auto& buffer : back_buffers_) buffer.Reset();
        if (!check(swapchain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers")) return;
        width_ = width;
        height_ = height;
        create_back_buffer_views();
        create_depth_buffer();
    }

    BufferHandle create_buffer(BufferUsage usage, const void* data, u64 size) override {
        u32 slot = 0;
        while (slot < kMaxBuffers && buffers_[slot].resource) ++slot;
        if (slot == kMaxBuffers || size == 0 || !data) {
            LSD_LOG_ERROR("d3d12", "create_buffer failed: table full (%u) or empty data.", kMaxBuffers);
            return {};
        }
        // shortcut: buffers live in the upload heap (CPU-visible); move static meshes to the default
        // heap with a copy queue when scenes get large.
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Buffer& b = buffers_[slot];
        if (!check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&b.resource)), "CreateCommittedResource (buffer)")) return {};
        void* mapped = nullptr;
        const D3D12_RANGE no_read{0, 0};
        if (!check(b.resource->Map(0, &no_read, &mapped), "Map (buffer)")) {
            b.resource.Reset();
            return {};
        }
        std::memcpy(mapped, data, size);
        b.resource->Unmap(0, nullptr);
        b.size = size;
        b.usage = usage;
        return BufferHandle::make(slot, b.generation);
    }

    void destroy_buffer(BufferHandle handle) override {
        Buffer* b = lookup(handle);
        if (!b) return;
        retire(std::move(b->resource));  // the frame being recorded may use it (buffers made per frame, water)
        ++b->generation;
    }

    bool supports_bc() const override { return true; }  // every D3D12 device
    TextureHandle create_texture(u32 width, u32 height, u32 mip_count, TextureFormat format, const void* const* mips) override {
        if (!srv_heap_) {
            LSD_LOG_ERROR("d3d12", "create_texture needs init_debug_ui() first (its view lives in the UI heap).");
            return {};
        }
        u32 slot = 0;
        while (slot < kMaxTextures && textures_[slot].resource) ++slot;
        if (slot == kMaxTextures || width == 0 || height == 0 || mip_count == 0 || mip_count > kMaxMips) {
            LSD_LOG_ERROR("d3d12", "create_texture failed: table full (%u), zero size or bad mip count.", kMaxTextures);
            return {};
        }
        Texture& t = textures_[slot];
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = static_cast<UINT16>(mip_count);
        switch (format) {
            case TextureFormat::RGBA8_sRGB: desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; break;
            case TextureFormat::BC1: desc.Format = DXGI_FORMAT_BC1_UNORM; break;
            case TextureFormat::BC1_sRGB: desc.Format = DXGI_FORMAT_BC1_UNORM_SRGB; break;
            case TextureFormat::BC3: desc.Format = DXGI_FORMAT_BC3_UNORM; break;
            case TextureFormat::BC3_sRGB: desc.Format = DXGI_FORMAT_BC3_UNORM_SRGB; break;
            case TextureFormat::BC5: desc.Format = DXGI_FORMAT_BC5_UNORM; break;
            case TextureFormat::RGBA16F: desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
            default: desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
        }
        if (is_block_compressed(format) && (width % 4 || height % 4)) {
            LSD_LOG_ERROR("d3d12", "create_texture: a BC texture must be a multiple of 4 wide and high (%u x %u)", width, height);
            return {};
        }
        desc.SampleDesc.Count = 1;
        if (!check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&t.resource)), "CreateCommittedResource (texture)")) return {};
        // Every mip through one upload buffer, copied on a separate list and waited for.
        // shortcut: synchronous upload (a stall per texture); move to a copy queue + streaming later.
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[kMaxMips];
        UINT rows[kMaxMips];
        UINT64 row_bytes[kMaxMips];
        UINT64 total = 0;
        device_->GetCopyableFootprints(&desc, 0, mip_count, 0, layouts, rows, row_bytes, &total);
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer_desc{};
        buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer_desc.Width = total;
        buffer_desc.Height = 1;
        buffer_desc.DepthOrArraySize = 1;
        buffer_desc.MipLevels = 1;
        buffer_desc.SampleDesc.Count = 1;
        buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> upload;
        if (!check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&upload)), "CreateCommittedResource (texture upload)")) {
            t.resource.Reset();
            return {};
        }
        u8* mapped = nullptr;
        const D3D12_RANGE no_read{0, 0};
        upload->Map(0, &no_read, reinterpret_cast<void**>(&mapped));
        for (u32 m = 0; m < mip_count; ++m) {
            const u8* source = static_cast<const u8*>(mips[m]);
            for (UINT r = 0; r < rows[m]; ++r) std::memcpy(mapped + layouts[m].Offset + r * layouts[m].Footprint.RowPitch, source + r * row_bytes[m], row_bytes[m]);
        }
        upload->Unmap(0, nullptr);
        if (!upload_list_) {
            device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&upload_allocator_));
            device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, upload_allocator_.Get(), nullptr, IID_PPV_ARGS(&upload_list_));
        } else {
            upload_allocator_->Reset();
            upload_list_->Reset(upload_allocator_.Get(), nullptr);
        }
        for (u32 m = 0; m < mip_count; ++m) {
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = t.resource.Get();
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.SubresourceIndex = m;
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = upload.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint = layouts[m];
            upload_list_->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = t.resource.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        upload_list_->ResourceBarrier(1, &barrier);
        upload_list_->Close();
        ID3D12CommandList* lists[] = {upload_list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        wait_for_gpu();
        alloc_srv(t.srv_cpu, t.srv_gpu);
        device_->CreateShaderResourceView(t.resource.Get(), nullptr, t.srv_cpu);
        return TextureHandle::make(kMaxTargets + slot, t.generation);
    }

    TextureHandle create_render_target(u32 width, u32 height) override {
        if (!srv_heap_) {
            LSD_LOG_ERROR("d3d12", "create_render_target needs init_debug_ui() first (its view lives in the UI heap).");
            return {};
        }
        u32 slot = 0;
        while (slot < kMaxTargets && targets_[slot].color) ++slot;
        if (slot == kMaxTargets || width == 0 || height == 0) {
            LSD_LOG_ERROR("d3d12", "create_render_target failed: table full (%u) or zero size.", kMaxTargets);
            return {};
        }
        Target& t = targets_[slot];
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = kTargetFormat;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        // No optimized clear colour: targets are cleared to varying colours (theme background).
        if (!check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                    IID_PPV_ARGS(&t.color)), "CreateCommittedResource (render target)")) return {};
        desc.Format = DXGI_FORMAT_R32_TYPELESS;  // written as depth, read as a float by the post pass
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = kDepthFormat;
        clear.DepthStencil.Depth = 1.0f;
        if (!check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                                    IID_PPV_ARGS(&t.depth)), "CreateCommittedResource (target depth)")) {
            t.color.Reset();
            return {};
        }
        device_->CreateRenderTargetView(t.color.Get(), nullptr, target_rtv(slot));
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
        dsv_desc.Format = kDepthFormat;
        dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device_->CreateDepthStencilView(t.depth.Get(), &dsv_desc, target_dsv(slot));
        dsv_desc.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        device_->CreateDepthStencilView(t.depth.Get(), &dsv_desc, target_dsv(slot + kMaxTargets));
        alloc_srv(t.srv_cpu, t.srv_gpu);
        device_->CreateShaderResourceView(t.color.Get(), nullptr, t.srv_cpu);
        alloc_srv(t.depth_srv_cpu, t.depth_srv_gpu);
        D3D12_SHADER_RESOURCE_VIEW_DESC depth_srv{};
        depth_srv.Format = DXGI_FORMAT_R32_FLOAT;
        depth_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depth_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depth_srv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(t.depth.Get(), &depth_srv, t.depth_srv_cpu);
        t.width = width;
        t.height = height;
        return TextureHandle::make(slot, t.generation);
    }

    void destroy_texture(TextureHandle handle) override {
        // Released, and the view's descriptor freed, once the frame being recorded is done: RmlUi frees a font
        // atlas in the middle of drawing a document whose earlier text this frame's commands still read (the
        // GPU then faulted on the freed texture and the device was removed: voice chat's "talking" text).
        if (ShadowMap* shadow = lookup_shadow(handle)) {
            retire_srv(shadow->srv_cpu);
            retire(std::move(shadow->resource));
            ++shadow->generation;
            return;
        }
        if (Texture* texture = lookup_texture(handle)) {
            retire_srv(texture->srv_cpu);
            retire(std::move(texture->resource));
            ++texture->generation;
            return;
        }
        Target* t = lookup(handle);
        if (!t) return;
        retire_srv(t->srv_cpu);
        retire_srv(t->depth_srv_cpu);
        retire(std::move(t->color));
        retire(std::move(t->depth));
        ++t->generation;
    }

    void set_render_target(TextureHandle handle, const f32 clear_color[4]) override {
        if (current_target_ >= 0) {
            transition(targets_[current_target_].color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            current_target_ = -1;
        }
        const Target* t = lookup(handle);
        if (!t) {  // back to the window
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(frame_);
            const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
            list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            set_viewport(0.0f, 0.0f, static_cast<f32>(width_), static_cast<f32>(height_));
            return;
        }
        const u32 slot = handle.index();
        transition(t->color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        current_target_ = static_cast<i32>(slot);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = target_rtv(slot);
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = target_dsv(slot);
        list_->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
        list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        set_viewport(0.0f, 0.0f, static_cast<f32>(t->width), static_cast<f32>(t->height));
    }

    u64 debug_ui_texture(TextureHandle handle) override {
        if (const Texture* texture = lookup_texture(handle)) return texture->srv_gpu.ptr;
        const Target* t = lookup(handle);
        return t ? t->srv_gpu.ptr : 0;
    }

    void set_viewport(f32 x, f32 y, f32 width, f32 height) override {
        viewport_[0] = x;
        viewport_[1] = y;
        viewport_[2] = width;
        viewport_[3] = height;
        const D3D12_VIEWPORT viewport{x, y, width, height, 0.0f, 1.0f};
        const D3D12_RECT scissor{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + width), static_cast<LONG>(y + height)};
        list_->RSSetViewports(1, &viewport);
        list_->RSSetScissorRects(1, &scissor);
    }

    // Copies skinning rows into this frame's ring and returns their GPU address (0 if full).
    D3D12_GPU_VIRTUAL_ADDRESS upload_joints(const f32* rows, u32 count) {
        return upload_constants(rows, (count < kMaxJoints ? count : kMaxJoints) * 48);
    }
    // Any constants (skinning rows, material parameters) into this frame's ring; 0 if full.
    D3D12_GPU_VIRTUAL_ADDRESS upload_constants(const void* rows, u32 bytes) {
        const u32 aligned = (bytes + 255u) & ~255u;
        if (joint_offset_ + aligned > kJointRingBytes) {
            LSD_LOG_WARN("d3d12", "Too many skinned draws this frame: some are skipped");
            return 0;
        }
        std::memcpy(joint_mapped_[frame_] + joint_offset_, rows, bytes);
        const D3D12_GPU_VIRTUAL_ADDRESS address = joint_buffers_[frame_]->GetGPUVirtualAddress() + joint_offset_;
        joint_offset_ += aligned;
        return address;
    }

    void draw_mesh(const MeshDraw& draw) override {
        const Buffer* vb = lookup(draw.vertices);
        const Buffer* ib = lookup(draw.indices);
        if (!vb || !ib) return;
        // GPU culling first (compute), the draw below reads what it keeps.
        const bool can_cull = draw.gpu_cull && !(draw.joint_rows && draw.joint_count) && draw.instance_rows && draw.instance_count;
        const u32 cull_batches = (draw.instance_count + kMaxJoints - 1) / kMaxJoints;
        const bool culled = can_cull && run_culling(draw, cull_batches);
        f32 constants[kRootConstants];
        std::memcpy(constants, draw.mvp, sizeof draw.mvp);
        std::memcpy(constants + 16, draw.normal_matrix, sizeof draw.normal_matrix);
        std::memcpy(constants + 28, draw.world, sizeof draw.world);
        std::memcpy(constants + 40, draw.color, sizeof draw.color);
        constants[19] = draw.unlit || draw.wireframe ? 1.0f : static_cast<f32>(draw.debug_view);
        const Texture* normal = lookup_texture(draw.normal_texture);
        const f32 cutoff = draw.alpha_cutoff < 0.0f ? 0.0f : draw.alpha_cutoff > 1.0f ? 1.0f : draw.alpha_cutoff;
        constants[23] = (normal ? 1.0f : 0.0f) + (draw.blend ? 2.0f : 0.0f) + 4.0f * std::round(cutoff * 255.0f) + (draw.pixel_art ? 1024.0f : 0.0f) + (draw.hair ? 2048.0f : 0.0f) + 4096.0f * std::round((draw.specular < 0.0f ? 0.0f : draw.specular > 1.0f ? 1.0f : draw.specular) * 255.0f);  // see n1.w in the shader
        constants[44] = draw.metallic;
        constants[45] = draw.roughness;
        constants[46] = draw.emissive[0];
        constants[47] = draw.emissive[1];
        constants[48] = draw.emissive[2];
        std::memcpy(constants + 49, draw.camera_position, sizeof draw.camera_position);
        const MaterialShader* material = draw.wireframe ? nullptr : lookup_material(draw.material_shader);
        constants[27] = material ? draw.time : pack_uv_scale(draw.uv_scale);  // the normal matrix's spare w: a graph's Time, else the Tiling
        const Texture* base = lookup_texture(draw.base_color_texture);
        const Texture* orm = lookup_texture(draw.orm_texture);
        const Texture* emissive = lookup_texture(draw.emissive_texture);
        ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
        list_->SetDescriptorHeaps(1, heaps);
        list_->SetGraphicsRootSignature(root_signature_.Get());
        list_->SetGraphicsRoot32BitConstants(0, kRootConstants, constants, 0);
        const Target* base_target = base ? nullptr : lookup(draw.base_color_texture);  // a render target (a world UI panel)
        list_->SetGraphicsRootDescriptorTable(1, base ? base->srv_gpu : base_target ? base_target->srv_gpu : textures_[white_].srv_gpu);
        list_->SetGraphicsRootDescriptorTable(2, (normal ? normal : &textures_[flat_normal_])->srv_gpu);
        list_->SetGraphicsRootDescriptorTable(3, (orm ? orm : &textures_[white_])->srv_gpu);
        list_->SetGraphicsRootConstantBufferView(4, current_lights_);
        if (scene_dirty_) write_scene_table();
        list_->SetGraphicsRootDescriptorTable(5, scene_table_);  // shadows, decals, reflection captures, lightmaps
        list_->SetGraphicsRootDescriptorTable(6, (emissive ? emissive : &textures_[white_])->srv_gpu);
        const bool skinned = draw.joint_rows && draw.joint_count;
        const bool instanced = !skinned && draw.instance_rows && draw.instance_count;
        const u32 blend = draw.wireframe || draw.blend > 3 ? 0u : draw.blend;  // 1..3: a blended variant
        // Water's depth tint: the target's depth read by the shader and tested read-only at once (as soft particles).
        const Target* depth_target = draw.scene_depth && material && blend == 1 && !instanced && current_target_ >= 0 ? &targets_[current_target_] : nullptr;
        list_->SetGraphicsRootDescriptorTable(9, depth_target ? depth_target->depth_srv_gpu : textures_[white_].srv_gpu);
        if (instanced) {  // batches of kMaxJoints instances through the joint ring
            list_->SetPipelineState(draw.wireframe ? instanced_wire_pipeline_.Get() : blend ? blend_pipelines_[2][blend - 1].Get() : instanced_pipeline_.Get());
            list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const D3D12_VERTEX_BUFFER_VIEW vbv{vb->resource->GetGPUVirtualAddress(), static_cast<UINT>(vb->size), sizeof(MeshVertex)};
            const D3D12_INDEX_BUFFER_VIEW ibv{ib->resource->GetGPUVirtualAddress(), static_cast<UINT>(ib->size), DXGI_FORMAT_R32_UINT};
            list_->IASetVertexBuffers(0, 1, &vbv);
            list_->IASetIndexBuffer(&ibv);
            if (culled) {  // each LOD and batch's visible rows as the instance constants, its count from the GPU
                draw_culled(draw, cull_batches);
                return;
            }
            for (u32 first = 0; first < draw.instance_count; first += kMaxJoints) {
                const u32 n = draw.instance_count - first < kMaxJoints ? draw.instance_count - first : kMaxJoints;
                const D3D12_GPU_VIRTUAL_ADDRESS rows = upload_joints(draw.instance_rows + first * 12, n);
                if (!rows) return;
                list_->SetGraphicsRootConstantBufferView(7, rows);
                list_->DrawIndexedInstanced(draw.index_count, n, 0, 0, 0);
            }
            return;
        }
        if (material) {  // its parameters: 16 float4, unset ones 0 (through the joint ring: 4 rows of 48 bytes = 192 + room)
            f32 values[kMaxMaterialParams * 4] = {};
            const u32 count = draw.material_param_count < kMaxMaterialParams ? draw.material_param_count : kMaxMaterialParams;
            if (draw.material_params) std::memcpy(values, draw.material_params, count * 16);
            const D3D12_GPU_VIRTUAL_ADDRESS address = upload_constants(values, sizeof values);
            if (!address) return;
            list_->SetGraphicsRootConstantBufferView(8, address);
        }
        if (skinned) {
            const D3D12_GPU_VIRTUAL_ADDRESS joints = upload_joints(draw.joint_rows, draw.joint_count);
            if (!joints) return;
            list_->SetGraphicsRootConstantBufferView(7, joints);
            list_->SetPipelineState(draw.wireframe                    ? skinned_wire_pipeline_.Get()
                                    : blend && material && blend < 3 ? material->blended[1][blend - 1].Get()
                                    : blend                          ? blend_pipelines_[1][blend - 1].Get()
                                    : material                       ? material->skinned.Get()
                                                                     : skinned_pipeline_.Get());
        } else {
            list_->SetPipelineState(draw.wireframe                    ? mesh_wire_pipeline_.Get()
                                    : blend && material && blend < 3 ? material->blended[0][blend - 1].Get()
                                    : blend                          ? blend_pipelines_[0][blend - 1].Get()
                                    : material                       ? material->mesh.Get()
                                                                     : mesh_pipeline_.Get());
        }
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VERTEX_BUFFER_VIEW vbv{vb->resource->GetGPUVirtualAddress(), static_cast<UINT>(vb->size), skinned ? sizeof(SkinnedVertex) : sizeof(MeshVertex)};
        const D3D12_INDEX_BUFFER_VIEW ibv{ib->resource->GetGPUVirtualAddress(), static_cast<UINT>(ib->size), DXGI_FORMAT_R32_UINT};
        list_->IASetVertexBuffers(0, 1, &vbv);
        list_->IASetIndexBuffer(&ibv);
        const u32 target_slot = static_cast<u32>(current_target_);
        if (depth_target) {
            transition(depth_target->depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = target_rtv(target_slot), dsv = target_dsv(target_slot + kMaxTargets);
            list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        }
        list_->DrawIndexedInstanced(draw.index_count, 1, 0, 0, 0);
        if (depth_target) {
            transition(depth_target->depth.Get(), D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = target_rtv(target_slot), dsv = target_dsv(target_slot);
            list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        }
    }

    MaterialShader* lookup_material(MaterialShaderHandle handle) {
        if (!handle.valid() || handle.index() >= kMaxMaterialShaders) return nullptr;
        MaterialShader& m = material_shaders_[handle.index()];
        return m.mesh && (m.generation & MaterialShaderHandle::kGenerationMask) == handle.generation() ? &m : nullptr;
    }

    MaterialShaderHandle create_material_shader(const char* code) override {
        u32 slot = 0;
        while (slot < kMaxMaterialShaders && material_shaders_[slot].mesh) ++slot;
        if (slot == kMaxMaterialShaders || !code) return {};
        std::string source = kMeshHlsl;
        source.insert(source.find("//@MATERIAL"), std::string(kMaterialPreludeHlsl) + code);
        project_shaders_ = true;  // the project's: DerivedData/Shaders
        const ShaderJob jobs[] = {{source.c_str(), source.size(), "vs_main", "vs_5_0", nullptr}, {source.c_str(), source.size(), "ps_main", "ps_5_0", nullptr},
                                  {source.c_str(), source.size(), "vs_main", "vs_5_0", kSkinnedDefines}, {source.c_str(), source.size(), "ps_main", "ps_5_0", kSkinnedDefines}};
        precompile(jobs, std::size(jobs));
        MaterialShader& m = material_shaders_[slot];
        bool ok = create_pipeline(source.c_str(), source.size() + 1, {kMeshLayout, 4}, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, true, m.mesh) &&
                  create_pipeline(source.c_str(), source.size() + 1, {kSkinnedLayout, 5}, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, true, m.skinned, false, kSkinnedDefines);
        for (u32 b = 0; b < 2 && ok; ++b) {  // translucent and additive variants
            ok = create_pipeline(source.c_str(), source.size() + 1, {kMeshLayout, 4}, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, true, m.blended[0][b], false, nullptr, b + 1) &&
                 create_pipeline(source.c_str(), source.size() + 1, {kSkinnedLayout, 5}, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, true, m.blended[1][b], false, kSkinnedDefines, b + 1);
        }
        project_shaders_ = false;
        if (!ok) {
            m.mesh.Reset();
            m.skinned.Reset();
            for (auto& kind : m.blended) {
                for (auto& p : kind) p.Reset();
            }
            return {};
        }
        return MaterialShaderHandle::make(slot, m.generation & MaterialShaderHandle::kGenerationMask);
    }

    static constexpr u32 kMaxPostShaders = 16;
    struct PostShader {
        ComPtr<ID3D12PipelineState> pso, pso_window;  // into a target, to the window
        u32 generation = 0;
    };
    PostShaderHandle create_post_shader(const char* code) override {
        u32 slot = 0;
        while (slot < kMaxPostShaders && post_shaders_[slot].pso) ++slot;
        if (slot == kMaxPostShaders || !code) return {};
        std::string source = std::string("#define LSD_POST_MATERIAL 1\n") + kPostHlsl;
        source.insert(source.find("//@POST"), std::string(code) + "\n");
        PostShader& s = post_shaders_[slot];
        ComPtr<ID3DBlob> vs, ps;
        project_shaders_ = true;
        const bool compiled = compile(source.c_str(), source.size(), "vs_full", "vs_5_0", vs) && compile(source.c_str(), source.size(), "ps_composite", "ps_5_0", ps);
        project_shaders_ = false;
        if (!compiled ||
            !make_post_pso(vs.Get(), ps.Get(), false, s.pso) || !make_post_pso(vs.Get(), ps.Get(), false, s.pso_window, kBackBufferFormat)) {
            s.pso.Reset();
            return {};
        }
        return PostShaderHandle::make(slot, s.generation & PostShaderHandle::kGenerationMask);
    }
    PostShader* lookup_post(PostShaderHandle handle) {
        if (!handle.valid() || handle.index() >= kMaxPostShaders) return nullptr;
        PostShader& s = post_shaders_[handle.index()];
        return s.pso && (s.generation & PostShaderHandle::kGenerationMask) == handle.generation() ? &s : nullptr;
    }
    void destroy_post_shader(PostShaderHandle handle) override {
        PostShader* s = lookup_post(handle);
        if (!s) return;
        wait_for_gpu();  // shortcut: stalls; fine for edits in the editor
        s->pso.Reset();
        s->pso_window.Reset();
        ++s->generation;
    }

    void destroy_material_shader(MaterialShaderHandle handle) override {
        MaterialShader* m = lookup_material(handle);
        if (!m) return;
        wait_for_gpu();  // shortcut: stalls; fine for edits in the editor, defer when materials churn
        m->mesh.Reset();
        m->skinned.Reset();
        for (auto& kind : m->blended) {
            for (auto& p : kind) p.Reset();
        }
        ++m->generation;
    }

    void set_lights(const LightSetup& lights) override {
        // Each call takes the next slot of this frame's buffer, so earlier draws keep their lights.
        if (light_offset_ + kLightSlot > kLightSlot * kLightSlots) light_offset_ = 0;
        std::memcpy(light_mapped_[frame_] + light_offset_, &lights, sizeof lights);
        hdr_output_ = lights.output[0] > 0.5f;
        std::memcpy(light_mapped_[frame_] + light_offset_ + offsetof(LightSetup, viewport), viewport_, sizeof viewport_);
        current_lights_ = light_buffers_[frame_]->GetGPUVirtualAddress() + light_offset_;
        light_offset_ += kLightSlot;
    }

    TextureHandle create_shadow_map(u32 size) override {
        if (!srv_heap_) {
            LSD_LOG_ERROR("d3d12", "create_shadow_map needs init_debug_ui() first (its view lives in the UI heap).");
            return {};
        }
        u32 slot = 0;
        while (slot < kMaxShadowMaps && shadow_maps_[slot].resource) ++slot;
        if (slot == kMaxShadowMaps || size == 0) return {};
        ShadowMap& m = shadow_maps_[slot];
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = size;
        desc.Height = size;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32_TYPELESS;  // written as depth, read as a float
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = kDepthFormat;
        clear.DepthStencil.Depth = 1.0f;
        if (!check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                                                    IID_PPV_ARGS(&m.resource)), "CreateCommittedResource (shadow map)")) return {};
        if (!shadow_dsv_heap_) {
            D3D12_DESCRIPTOR_HEAP_DESC dsv{};
            dsv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            dsv.NumDescriptors = kMaxShadowMaps;
            device_->CreateDescriptorHeap(&dsv, IID_PPV_ARGS(&shadow_dsv_heap_));
        }
        m.dsv = shadow_dsv_heap_->GetCPUDescriptorHandleForHeapStart();
        m.dsv.ptr += static_cast<SIZE_T>(slot) * dsv_size_;
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
        dsv_desc.Format = kDepthFormat;
        dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device_->CreateDepthStencilView(m.resource.Get(), &dsv_desc, m.dsv);
        alloc_srv(m.srv_cpu, m.srv_gpu);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(m.resource.Get(), &srv, m.srv_cpu);
        m.size = size;
        return TextureHandle::make(kMaxTargets + kMaxTextures + slot, m.generation);
    }

    void begin_shadow_pass(TextureHandle handle, bool clear) override {
        ShadowMap* m = lookup_shadow(handle);
        if (!m) return;
        if (current_target_ >= 0) set_render_target({}, nullptr);
        transition(m->resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        if (clear) list_->ClearDepthStencilView(m->dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        list_->OMSetRenderTargets(0, nullptr, FALSE, &m->dsv);
        set_viewport(0.0f, 0.0f, static_cast<f32>(m->size), static_cast<f32>(m->size));
        shadow_pass_ = static_cast<i32>(handle.index() - kMaxTargets - kMaxTextures);
    }

    void clear_shadow_viewport() override {
        if (shadow_pass_ < 0) return;
        const D3D12_RECT rect{static_cast<LONG>(viewport_[0]), static_cast<LONG>(viewport_[1]), static_cast<LONG>(viewport_[0] + viewport_[2]),
                              static_cast<LONG>(viewport_[1] + viewport_[3])};
        list_->ClearDepthStencilView(shadow_maps_[shadow_pass_].dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 1, &rect);
    }

    void draw_shadow_caster(BufferHandle vertices, BufferHandle indices, u32 index_count, const f32 light_mvp[16], const f32* joint_rows,
                            u32 joint_count, const ShadowCutout* cutout) override {
        const Buffer* vb = lookup(vertices);
        const Buffer* ib = lookup(indices);
        if (!vb || !ib || shadow_pass_ < 0) return;
        const bool skinned = joint_rows && joint_count;
        const Texture* cut_tex = cutout ? lookup_texture(cutout->texture) : nullptr;
        list_->SetGraphicsRootSignature(root_signature_.Get());
        list_->SetGraphicsRoot32BitConstants(0, 16, light_mvp, 0);
        if (cut_tex) {  // Masked: the texture's alpha decides which pixels cast
            const f32 cut[4] = {cutout->cutoff, cutout->alpha, cutout->uv_scale[0], cutout->uv_scale[1]};
            list_->SetGraphicsRoot32BitConstants(0, 4, cut, 16);
            ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
            list_->SetDescriptorHeaps(1, heaps);
            list_->SetGraphicsRootDescriptorTable(1, cut_tex->srv_gpu);
        }
        if (skinned) {
            const D3D12_GPU_VIRTUAL_ADDRESS joints = upload_joints(joint_rows, joint_count);
            if (!joints) return;
            list_->SetGraphicsRootConstantBufferView(7, joints);
        }
        list_->SetPipelineState(cut_tex ? (skinned ? skinned_cutout_shadow_pipeline_.Get() : cutout_shadow_pipeline_.Get())
                                        : skinned ? skinned_shadow_pipeline_.Get() : shadow_pipeline_.Get());
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VERTEX_BUFFER_VIEW vbv{vb->resource->GetGPUVirtualAddress(), static_cast<UINT>(vb->size), skinned ? sizeof(SkinnedVertex) : sizeof(MeshVertex)};
        const D3D12_INDEX_BUFFER_VIEW ibv{ib->resource->GetGPUVirtualAddress(), static_cast<UINT>(ib->size), DXGI_FORMAT_R32_UINT};
        list_->IASetVertexBuffers(0, 1, &vbv);
        list_->IASetIndexBuffer(&ibv);
        list_->DrawIndexedInstanced(index_count, 1, 0, 0, 0);
    }

    void draw_shadow_instances(BufferHandle vertices, BufferHandle indices, u32 index_count, const f32 view_proj[16], const f32* rows, u32 count,
                               const f32* bounds_min, const f32* bounds_max) override {
        const Buffer* vb = lookup(vertices);
        const Buffer* ib = lookup(indices);
        if (!vb || !ib || shadow_pass_ < 0 || !rows || !count) return;
        MeshDraw cull;  // with bounds: culled against the light's view on the GPU
        cull.indices = indices;
        cull.index_count = index_count;
        std::memcpy(cull.mvp, view_proj, sizeof cull.mvp);
        cull.instance_rows = rows;
        cull.instance_count = count;
        if (bounds_min && bounds_max) {
            std::memcpy(cull.local_bounds_min, bounds_min, sizeof cull.local_bounds_min);
            std::memcpy(cull.local_bounds_max, bounds_max, sizeof cull.local_bounds_max);
        }
        const u32 batches = (count + kMaxJoints - 1) / kMaxJoints;
        const bool culled = bounds_min && bounds_max && run_culling(cull, batches);
        list_->SetGraphicsRootSignature(root_signature_.Get());
        list_->SetGraphicsRoot32BitConstants(0, 16, view_proj, 0);
        list_->SetPipelineState(instanced_shadow_pipeline_.Get());
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VERTEX_BUFFER_VIEW vbv{vb->resource->GetGPUVirtualAddress(), static_cast<UINT>(vb->size), sizeof(MeshVertex)};
        const D3D12_INDEX_BUFFER_VIEW ibv{ib->resource->GetGPUVirtualAddress(), static_cast<UINT>(ib->size), DXGI_FORMAT_R32_UINT};
        list_->IASetVertexBuffers(0, 1, &vbv);
        list_->IASetIndexBuffer(&ibv);
        if (culled) {
            draw_culled(cull, batches);
            return;
        }
        for (u32 first = 0; first < count; first += kMaxJoints) {  // the rows' buffer holds 256 copies a call
            const u32 n = count - first < kMaxJoints ? count - first : kMaxJoints;
            const D3D12_GPU_VIRTUAL_ADDRESS buffer = upload_joints(rows + first * 12, n);
            if (!buffer) return;
            list_->SetGraphicsRootConstantBufferView(7, buffer);
            list_->DrawIndexedInstanced(index_count, n, 0, 0, 0);
        }
    }

    void end_shadow_pass() override {
        if (shadow_pass_ < 0) return;
        transition(shadow_maps_[shadow_pass_].resource.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        shadow_pass_ = -1;
        set_render_target({}, nullptr);  // back to the window
    }

    // t3..t7 share one table of five neighbouring descriptors. A change writes the five into the ring's next
    // entry (never one a frame still on the GPU may read), at the next mesh draw.
    // shortcut: kSceneRing entries; over ~20 shadow / atlas changes a frame (views) would reuse one in flight, more entries then
    void set_scene_resource(u32 which, ID3D12Resource* resource, bool shadow) {
        if (scene_resources_[which] == resource) return;
        scene_resources_[which] = resource;
        if (which < 2) scene_shadow_[which] = shadow;
        scene_dirty_ = true;
    }
    void set_scene_texture(u32 which, TextureHandle atlas) {
        const Texture* t = lookup_texture(atlas);
        set_scene_resource(which, t ? t->resource.Get() : nullptr, false);
    }
    void write_scene_table() {
        scene_at_ = (scene_at_ + 1) % kSceneRing;
        D3D12_CPU_DESCRIPTOR_HANDLE at = scene_cpu_;
        at.ptr += static_cast<SIZE_T>(scene_at_) * 5 * srv_size_;
        scene_table_ = scene_gpu_;
        scene_table_.ptr += static_cast<UINT64>(scene_at_) * 5 * srv_size_;
        for (u32 k = 0; k < 5; ++k, at.ptr += srv_size_) {
            ID3D12Resource* resource = scene_resources_[k];
            D3D12_SHADER_RESOURCE_VIEW_DESC depth{};  // shadow atlases: their depth as R32_FLOAT
            depth.Format = DXGI_FORMAT_R32_FLOAT;
            depth.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            depth.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            depth.Texture2D.MipLevels = 1;
            const bool shadow = resource && k < 2 && scene_shadow_[k];
            device_->CreateShaderResourceView(resource ? resource : textures_[white_].resource.Get(), shadow ? &depth : nullptr, at);
        }
        scene_dirty_ = false;
    }
    void set_decal_atlas(TextureHandle atlas) override { set_scene_texture(2, atlas); }
    void set_reflection_atlas(TextureHandle atlas) override { set_scene_texture(3, atlas); }
    void set_lightmap_atlas(TextureHandle atlas) override { set_scene_texture(4, atlas); }

    void set_shadow_map(TextureHandle sun, TextureHandle local) override {
        const ShadowMap* s = lookup_shadow(sun);  // white without one: the shader only samples them when a light says so
        const ShadowMap* l = lookup_shadow(local);
        set_scene_resource(0, s ? s->resource.Get() : nullptr, s != nullptr);
        set_scene_resource(1, l ? l->resource.Get() : nullptr, l != nullptr);
    }

    void draw_lines(BufferHandle vertices, u32 vertex_count, const f32 mvp[16]) override {
        const Buffer* vb = lookup(vertices);
        if (!vb) return;
        list_->SetGraphicsRootSignature(root_signature_.Get());
        list_->SetGraphicsRoot32BitConstants(0, 16, mvp, 0);
        list_->SetPipelineState(line_pipeline_.Get());
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        const D3D12_VERTEX_BUFFER_VIEW vbv{vb->resource->GetGPUVirtualAddress(), static_cast<UINT>(vb->size), sizeof(LineVertex)};
        list_->IASetVertexBuffers(0, 1, &vbv);
        list_->DrawInstanced(vertex_count, 1, 0, 0);
    }

    void draw_particles(const ParticleVertex* vertices, u32 vertex_count, const f32 view_proj[16], TextureHandle texture, bool additive, const f32* soft) override {
        const u32 bytes = vertex_count * static_cast<u32>(sizeof(ParticleVertex));
        if (!vertex_count || particle_offset_ + bytes > kParticleRingBytes) {
            if (vertex_count) LSD_LOG_WARN("d3d12", "Too many particles this frame: some are skipped");
            return;
        }
        std::memcpy(particle_mapped_[frame_] + particle_offset_, vertices, bytes);
        const D3D12_VERTEX_BUFFER_VIEW vbv{particle_buffers_[frame_]->GetGPUVirtualAddress() + particle_offset_, bytes, sizeof(ParticleVertex)};
        particle_offset_ += (bytes + 255u) & ~255u;
        draw_sprites(&vbv, vertex_count, view_proj, texture, additive, soft, nullptr);
    }

    bool supports_gpu_particles() const override { return gpu_particle_add_ != nullptr; }
    BufferHandle create_particle_state(u32 capacity) override {
        u32 slot = 0;
        while (slot < kMaxBuffers && buffers_[slot].resource) ++slot;
        if (slot == kMaxBuffers || capacity == 0 || capacity > kMaxGpuParticles) return {};
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = static_cast<u64>(capacity) * 48;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Buffer& b = buffers_[slot];  // committed resources start zeroed: every particle dead
        if (!check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                    IID_PPV_ARGS(&b.resource)), "CreateCommittedResource (particles)")) return {};
        b.size = desc.Width;
        b.usage = BufferUsage::Storage;
        b.read_frame = 0;
        return BufferHandle::make(slot, b.generation);
    }
    void simulate_particles(BufferHandle state, const GpuParticles& params) override {
        Buffer* b = lookup(state);
        if (!b || !gpu_particle_sim_) return;
        const D3D12_GPU_VIRTUAL_ADDRESS constants = upload_constants(&params, sizeof params);
        if (!constants) return;
        if (b->read_frame == frame_number_) transition(b->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list_->SetComputeRootSignature(gpu_particle_sim_root_.Get());
        list_->SetComputeRootConstantBufferView(0, constants);
        list_->SetComputeRootUnorderedAccessView(1, b->resource->GetGPUVirtualAddress());
        list_->SetPipelineState(gpu_particle_sim_.Get());
        list_->Dispatch((params.spawn[2] + 63) / 64, 1, 1);
        transition(b->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        b->read_frame = frame_number_;
    }
    void draw_gpu_particles(BufferHandle state, const GpuParticles& params, const f32 view_proj[16], TextureHandle texture, bool additive, const f32* soft) override {
        Buffer* b = lookup(state);
        if (!b || !gpu_particle_add_ || !params.spawn[2]) return;
        const D3D12_GPU_VIRTUAL_ADDRESS constants = upload_constants(&params, sizeof params);
        if (!constants) return;
        b->read_frame = frame_number_;  // promoted to a shader resource by this draw if not simulated this frame
        const D3D12_GPU_VIRTUAL_ADDRESS gpu[2] = {constants, b->resource->GetGPUVirtualAddress()};
        draw_sprites(nullptr, params.spawn[2] * 6, view_proj, texture, additive, soft, gpu);
    }

    // Sprites (draw_particles from a vertex buffer; draw_gpu_particles built in the vertex shader from `gpu`: its
    // parameters and particle state): the texture, the depth for soft particles, the blend.
    void draw_sprites(const D3D12_VERTEX_BUFFER_VIEW* vbv, u32 vertex_count, const f32 view_proj[16], TextureHandle texture, bool additive, const f32* soft,
                      const D3D12_GPU_VIRTUAL_ADDRESS* gpu) {
        const Texture* t = lookup_texture(texture);
        ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
        list_->SetDescriptorHeaps(1, heaps);
        list_->SetGraphicsRootSignature(gpu ? gpu_sprite_root_.Get() : root_signature_.Get());
        f32 particle_constants[24] = {};
        std::memcpy(particle_constants, view_proj, 16 * sizeof(f32));
        particle_constants[16] = hdr_output_ ? 1.0f : 0.0f;
        const Target* target = current_target_ >= 0 ? &targets_[current_target_] : nullptr;
        const bool soft_fade = soft && soft[0] > 0.0f && target;
        if (soft_fade) std::memcpy(particle_constants + 20, soft, 4 * sizeof(f32));
        list_->SetGraphicsRoot32BitConstants(0, 24, particle_constants, 0);
        list_->SetGraphicsRootDescriptorTable(1, (t ? t : &textures_[white_])->srv_gpu);
        list_->SetGraphicsRootDescriptorTable(2, soft_fade ? target->depth_srv_gpu : textures_[white_].srv_gpu);
        if (soft_fade) {  // the depth read by the shader and tested read-only at once
            transition(target->depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = target_rtv(static_cast<u32>(current_target_));
            const D3D12_CPU_DESCRIPTOR_HANDLE dsv = target_dsv(static_cast<u32>(current_target_) + kMaxTargets);
            list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        }
        if (gpu) {
            list_->SetGraphicsRootConstantBufferView(3, gpu[0]);
            list_->SetGraphicsRootShaderResourceView(4, gpu[1]);
            list_->SetPipelineState(additive ? gpu_particle_add_.Get() : gpu_particle_alpha_.Get());
        } else {
            list_->SetPipelineState(additive ? particle_add_pipeline_.Get() : particle_alpha_pipeline_.Get());
        }
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (vbv) list_->IASetVertexBuffers(0, 1, vbv);
        list_->DrawInstanced(vertex_count, 1, 0, 0);
        if (soft_fade) {
            transition(target->depth.Get(), D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = target_rtv(static_cast<u32>(current_target_));
            const D3D12_CPU_DESCRIPTOR_HANDLE dsv = target_dsv(static_cast<u32>(current_target_));
            list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        }
    }

    void draw_sky(const SkyDraw& sky) override {
        f32 constants[kRootConstants] = {};
        std::memcpy(constants, sky.inv_view_proj, sizeof sky.inv_view_proj);
        std::memcpy(constants + 16, sky.zenith, 16);
        std::memcpy(constants + 20, sky.horizon, 16);
        std::memcpy(constants + 24, sky.ground, 16);
        std::memcpy(constants + 28, sky.sun_direction, 16);
        std::memcpy(constants + 32, sky.sun_color, 16);
        std::memcpy(constants + 49, sky.camera, 12);
        ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
        list_->SetDescriptorHeaps(1, heaps);
        list_->SetGraphicsRootSignature(root_signature_.Get());
        list_->SetGraphicsRoot32BitConstants(0, kRootConstants, constants, 0);
        list_->SetGraphicsRootConstantBufferView(4, current_lights_);
        if (scene_dirty_) write_scene_table();
        list_->SetGraphicsRootDescriptorTable(5, scene_table_);  // the reflection atlas: an HDRI's tile
        list_->SetPipelineState(sky_pipeline_.Get());
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list_->DrawInstanced(3, 1, 0, 0);
    }

    void draw_ui(const UiDraw& d) override {
        const u32 vbytes = d.vertex_count * static_cast<u32>(sizeof(UiVertex));
        const u32 ibytes = d.index_count * 4u;
        const u32 valigned = (vbytes + 255u) & ~255u;
        if (!d.vertex_count || !d.index_count || particle_offset_ + valigned + ibytes > kParticleRingBytes) return;
        std::memcpy(particle_mapped_[frame_] + particle_offset_, d.vertices, vbytes);
        std::memcpy(particle_mapped_[frame_] + particle_offset_ + valigned, d.indices, ibytes);
        const D3D12_GPU_VIRTUAL_ADDRESS base = particle_buffers_[frame_]->GetGPUVirtualAddress() + particle_offset_;
        const D3D12_VERTEX_BUFFER_VIEW vbv{base, vbytes, sizeof(UiVertex)};
        const D3D12_INDEX_BUFFER_VIEW ibv{base + valigned, ibytes, DXGI_FORMAT_R32_UINT};
        particle_offset_ += (valigned + ibytes + 255u) & ~255u;
        const f32 constants[4] = {d.translation[0], d.translation[1], 2.0f / d.viewport[2], 2.0f / d.viewport[3]};
        const Texture* t = lookup_texture(d.texture);
        ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
        list_->SetDescriptorHeaps(1, heaps);
        list_->SetGraphicsRootSignature(root_signature_.Get());
        list_->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
        list_->SetGraphicsRootDescriptorTable(1, (t ? t : &textures_[white_])->srv_gpu);
        list_->SetPipelineState(current_target_ >= 0 ? ui_target_pipeline_.Get() : ui_pipeline_.Get());
        const D3D12_VIEWPORT viewport{d.viewport[0], d.viewport[1], d.viewport[2], d.viewport[3], 0.0f, 1.0f};
        list_->RSSetViewports(1, &viewport);
        D3D12_RECT scissor{static_cast<LONG>(d.viewport[0]), static_cast<LONG>(d.viewport[1]), static_cast<LONG>(d.viewport[0] + d.viewport[2]),
                           static_cast<LONG>(d.viewport[1] + d.viewport[3])};
        if (d.scissor[2] > 0) {
            scissor.left = static_cast<LONG>(d.viewport[0]) + d.scissor[0];
            scissor.top = static_cast<LONG>(d.viewport[1]) + d.scissor[1];
            scissor.right = scissor.left + d.scissor[2];
            scissor.bottom = scissor.top + d.scissor[3];
        }
        list_->RSSetScissorRects(1, &scissor);
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list_->IASetVertexBuffers(0, 1, &vbv);
        list_->IASetIndexBuffer(&ibv);
        list_->DrawIndexedInstanced(d.index_count, 1, 0, 0, 0);
        set_viewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);  // back to the caller's rectangle
    }

    // Post-processing: the bloom chain and the FXAA input, one set per scene size (a few kept).
    struct PostChain {
        u32 width = 0, height = 0;
        TextureHandle down[5], up[4], ldr;
        TextureHandle ao, lit, dof, motion, taa[2], velocity;  // occlusion, scene with it, depth of field, motion blur, TAA output / history, velocity resolved
        TextureHandle ssr;                   // the scene with reflections
        TextureHandle exposure[2], exposed;  // eye adaptation: this / last frame's exposure (1 x 1), the scene with it
        u32 exposure_index = 0;
        bool exposure_valid = false;
        u32 taa_index = 0;
        bool history_valid = false;  // shortcut: one TAA history per size; two same-size views would share it
    };
    PostChain& post_chain(u32 width, u32 height) {
        for (PostChain& c : post_chains_) {
            if (c.width == width && c.height == height) return c;
        }
        PostChain& c = post_chains_[post_chain_next_++ % 4];
        for (TextureHandle& t : c.down) destroy_texture(t);
        for (TextureHandle& t : c.up) destroy_texture(t);
        for (TextureHandle& t : c.taa) destroy_texture(t);
        destroy_texture(c.ao);
        destroy_texture(c.lit);
        destroy_texture(c.dof);
        destroy_texture(c.motion);
        destroy_texture(c.velocity);
        destroy_texture(c.ssr);
        destroy_texture(c.ldr);
        for (TextureHandle& t : c.exposure) destroy_texture(t);
        destroy_texture(c.exposed);
        c = PostChain{};
        c.width = width;
        c.height = height;
        for (u32 i = 0; i < 5; ++i) {
            const u32 w = (width >> (i + 1)) ? (width >> (i + 1)) : 1u, h = (height >> (i + 1)) ? (height >> (i + 1)) : 1u;
            c.down[i] = create_render_target(w, h);
            if (i < 4) c.up[i] = create_render_target(w, h);
        }
        c.ldr = create_render_target(width, height);
        c.ao = create_render_target(width, height);  // shortcut: full-size effect targets even when unused; make them lazily if memory gets tight
        c.lit = create_render_target(width, height);
        c.dof = create_render_target(width, height);
        c.motion = create_render_target(width, height);
        c.velocity = create_render_target(width, height);
        c.ssr = create_render_target(width, height);
        c.taa[0] = create_render_target(width, height);
        c.taa[1] = create_render_target(width, height);
        c.exposure[0] = create_render_target(1, 1);
        c.exposure[1] = create_render_target(1, 1);
        c.exposed = create_render_target(width, height);
        return c;
    }
    // One full-screen pass into `target` (invalid: the window, in `window_rect`).
    void post_pass(ID3D12PipelineState* pso, TextureHandle target, const f32* window_rect, TextureHandle in0, TextureHandle in1, TextureHandle depth_of,
                   const f32 constants[48], bool color2 = false) {  // color2: depth_of's colour, not its depth
        const f32 black[4] = {0, 0, 0, 1};
        if (target.valid()) {
            set_render_target(target, black);
        } else {
            set_render_target({}, nullptr);
            set_viewport(window_rect[0], window_rect[1], window_rect[2], window_rect[3]);
        }
        const Target* a = lookup(in0);
        const Target* b = lookup(in1);
        ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
        list_->SetDescriptorHeaps(1, heaps);
        list_->SetGraphicsRootSignature(root_signature_.Get());
        const Target* d = lookup(depth_of);
        list_->SetGraphicsRoot32BitConstants(0, 48, constants, 0);
        list_->SetGraphicsRootDescriptorTable(1, (a ? a->srv_gpu : textures_[white_].srv_gpu));
        list_->SetGraphicsRootDescriptorTable(2, (b ? b->srv_gpu : textures_[white_].srv_gpu));
        const Texture* plain = d ? nullptr : lookup_texture(depth_of);  // not a target: an ordinary texture (the Color Grading LUT)
        list_->SetGraphicsRootDescriptorTable(3, (d ? (color2 ? d->srv_gpu : d->depth_srv_gpu) : plain ? plain->srv_gpu : textures_[white_].srv_gpu));
        list_->SetPipelineState(pso);
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list_->DrawInstanced(3, 1, 0, 0);
    }

    static u8 half_to_byte(u16 h) {  // 0..1 (clamped) to 0..255
        const u32 exponent = (h >> 10) & 0x1Fu, mantissa = h & 0x3FFu;
        if (h & 0x8000u) return 0;
        const f32 v = exponent == 0 ? std::ldexp(static_cast<f32>(mantissa) / 1024.0f, -14) : exponent == 31 ? 1.0f : std::ldexp(1.0f + static_cast<f32>(mantissa) / 1024.0f, static_cast<int>(exponent) - 15);
        return static_cast<u8>(std::lround(std::fmin(v, 1.0f) * 255.0f));
    }

    bool read_render_target(TextureHandle handle, u8* rgba) override {
        Target* t = lookup(handle);
        if (!t) return false;
        wait_for_gpu();
        D3D12_RESOURCE_DESC desc = t->color->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows = 0;
        UINT64 row_bytes = 0, total = 0;
        device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_bytes, &total);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = total;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        if (!check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)),
                   "CreateCommittedResource (readback)")) return false;
        if (!upload_list_) {
            device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&upload_allocator_));
            device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, upload_allocator_.Get(), nullptr, IID_PPV_ARGS(&upload_list_));
        } else {
            upload_allocator_->Reset();
            upload_list_->Reset(upload_allocator_.Get(), nullptr);
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = t->color.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        upload_list_->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = t->color.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = footprint;
        upload_list_->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        upload_list_->ResourceBarrier(1, &barrier);
        upload_list_->Close();
        ID3D12CommandList* lists[] = {upload_list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        wait_for_gpu();
        u8* mapped = nullptr;
        if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
        for (u32 y = 0; y < t->height; ++y) {  // half floats to bytes
            const u16* row = reinterpret_cast<const u16*>(mapped + footprint.Offset + y * footprint.Footprint.RowPitch);
            u8* out = rgba + static_cast<usize>(y) * t->width * 4;
            for (u32 i = 0; i < t->width * 4; ++i) out[i] = half_to_byte(row[i]);
        }
        const D3D12_RANGE nothing{0, 0};
        readback->Unmap(0, &nothing);
        return true;
    }

    void draw_post(const PostDraw& p) override {
        Target* scene = lookup(p.scene);
        if (!scene) return;
        if (p.build_hiz) build_hiz(*scene, p.hiz_view_proj);  // the next frame's occlusion test
        PostChain& c = post_chain(scene->width, scene->height);
        auto size = [&](TextureHandle t, f32* out) {
            const Target* target = lookup(t);
            out[0] = target ? 1.0f / static_cast<f32>(target->width) : 0.0f;
            out[1] = target ? 1.0f / static_cast<f32>(target->height) : 0.0f;
        };
        const bool depth_effects = p.proj[0] != 0.0f;
        const bool ao = depth_effects && (p.ao_intensity > 0.0f || p.contact_shadow[3] > 0.0f);
        const bool dof = depth_effects && p.dof_focal_distance > 0.0f;
        const bool motion = depth_effects && p.motion_blur > 0.0f && !p.taa_reset;
        const bool taa = depth_effects && p.taa;
        const bool ssr = depth_effects && p.ssr_intensity > 0.0f;
        if (!taa) c.history_valid = false;
        f32 k[48] = {};  // the constants every pass reads (see the shaders' u_post / cbuffer Post)
        for (int i = 0; i < 4; ++i) k[16 + i] = p.proj[i];
        k[20] = p.ao_radius > 0.01f ? p.ao_radius : 0.01f;
        k[21] = p.ao_intensity;
        k[22] = p.ssr_intensity;
        k[23] = p.ssr_max_distance > 0.5f ? p.ssr_max_distance : 0.5f;
        k[24] = p.dof_focal_distance;
        k[25] = p.dof_focal_region;
        k[26] = p.dof_max_blur;
        k[27] = p.dof_transition;
        k[28] = 0.1f;  // TAA: how much of this frame goes in
        k[29] = (p.taa_reset || !c.history_valid) ? 1.0f : 0.0f;
        k[30] = p.motion_blur;
        k[31] = p.motion_blur_max;
        for (int r = 0; r < 4; ++r) {
            for (int col = 0; col < 4; ++col) k[32 + r * 4 + col] = p.reproject[col * 4 + r];  // rows
        }
        size(p.scene, k);
        k[2] = lookup(p.normals) ? 1.0f : 0.0f;  // SSR / AO: normals from the normals pass (t1)
        // The scene's depth is read by the passes below: a shader resource until they are done.
        const bool read_depth = ao || dof || taa || ssr || motion;
        if (read_depth) transition(scene->depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        TextureHandle src = p.scene;
        if (ssr) {  // first: it reads the scene's alpha (reflectivity)
            post_pass(post_ssr_.Get(), c.ssr, nullptr, p.scene, p.normals, p.scene, k);
            src = c.ssr;
        }
        if (ao) {
            f32 ka[48];  // the AO pass reads no reprojection: its first row carries the contact shadow ray
            std::memcpy(ka, k, sizeof ka);
            std::memcpy(ka + 32, p.contact_shadow, sizeof p.contact_shadow);
            post_pass(post_ao_.Get(), c.ao, nullptr, {}, p.normals, p.scene, ka);
            post_pass(post_ao_apply_.Get(), c.lit, nullptr, src, c.ao, p.scene, k);
            src = c.lit;
        }
        // The velocity pass (objects' own movement): resolved with the camera's where nothing was drawn, for TAA and motion blur.
        const bool vel = (taa || motion) && lookup(p.velocity);
        f32 kv[48];
        std::memcpy(kv, k, sizeof kv);
        kv[3] = 1.0f;  // texel.w: they read it
        if (vel) post_pass(post_velocity_.Get(), c.velocity, nullptr, {}, p.velocity, p.scene, k);
        if (taa) {
            const TextureHandle out = c.taa[c.taa_index], history = c.taa[c.taa_index ^ 1u];
            post_pass(post_taa_.Get(), out, nullptr, src, history, vel ? c.velocity : p.scene, vel ? kv : k, vel);
            c.taa_index ^= 1u;
            c.history_valid = true;
            src = out;
        }
        if (dof) {
            post_pass(post_dof_.Get(), c.dof, nullptr, src, {}, p.scene, k);
            src = c.dof;
        }
        if (motion) {
            post_pass(post_motion_.Get(), c.motion, nullptr, src, vel ? c.velocity : TextureHandle{}, p.scene, vel ? kv : k);
            src = c.motion;
        }
        if (p.auto_exposure) {  // eye adaptation: meter into 1 x 1 (eased from the last frame's), then apply it
            f32 e[48] = {};
            e[4] = p.exposure_min_ev;
            e[5] = p.exposure_max_ev;
            e[6] = p.exposure_compensation;
            e[7] = c.exposure_valid && p.delta_seconds > 0.0f ? 0.0f : 1.0f;  // 1: jump to the target
            e[8] = p.delta_seconds * p.exposure_speed_up;
            e[9] = p.delta_seconds * p.exposure_speed_down;
            const TextureHandle now = c.exposure[c.exposure_index], last = c.exposure[c.exposure_index ^ 1u];
            post_pass(post_meter_.Get(), now, nullptr, src, last, {}, e);
            post_pass(post_expose_.Get(), c.exposed, nullptr, src, now, {}, e);
            c.exposure_index ^= 1u;
            c.exposure_valid = true;
            src = c.exposed;
        } else {
            c.exposure_valid = false;
        }
        const bool bloom = p.bloom_intensity > 0.0f;
        if (bloom) {
            k[4] = p.bloom_threshold;
            size(src, k);
            post_pass(post_prefilter_.Get(), c.down[0], nullptr, src, {}, {}, k);
            for (u32 i = 1; i < 5; ++i) {
                size(c.down[i - 1], k);
                post_pass(post_down_.Get(), c.down[i], nullptr, c.down[i - 1], {}, {}, k);
            }
            for (i32 i = 3; i >= 0; --i) {  // up[i] = down[i] + blur(up[i + 1]); the lowest uses down[4]
                const TextureHandle below = i == 3 ? c.down[4] : c.up[i + 1];
                size(c.down[i], k);
                size(below, k + 2);
                post_pass(post_up_.Get(), c.up[i], nullptr, c.down[i], below, {}, k);
            }
        }
        f32 g[48] = {};
        g[5] = bloom ? p.bloom_intensity : 0.0f;
        g[8] = p.tint[0];
        g[9] = p.tint[1];
        g[10] = p.tint[2];
        g[11] = p.tonemap ? 1.0f : 0.0f;
        g[12] = p.saturation;
        g[13] = p.contrast;
        g[14] = p.vignette;
        g[15] = p.color_lut.valid() ? p.lut_intensity : 0.0f;
        post_color_vision(p, g);
        const bool fxaa = p.fxaa && !taa;
        const PostShader* custom = lookup_post(p.post_shader);
        if (custom) {  // a post-process material: the scene's texel, its time and parameters (proj's row: unused here)
            size(src, g);
            g[16] = p.post_time;
            for (int n = 0; n < 3; ++n) g[17 + n] = p.post_params[n];
        }
        const bool to_window = !fxaa && !lookup(p.output);
        post_pass(custom ? (to_window ? custom->pso_window : custom->pso).Get() : (to_window ? post_composite_window_ : post_composite_).Get(), fxaa ? c.ldr : p.output,
                  p.viewport, src, bloom ? c.up[0] : TextureHandle{}, p.color_lut, g);
        if (fxaa) {
            f32 f[48] = {};
            size(c.ldr, f);
            post_pass((lookup(p.output) ? post_fxaa_ : post_fxaa_window_).Get(), p.output, p.viewport, c.ldr, {}, {}, f);
        }
        set_render_target({}, nullptr);  // the last pass's target back to a shader resource before the depth moves
        if (read_depth) transition(scene->depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        set_viewport(p.viewport[0], p.viewport[1], p.viewport[2], p.viewport[3]);
    }

    void draw_outline(const MeshDraw* draws, u32 count, const f32 color[4], const f32 viewport[4]) override {
        const u32 w = static_cast<u32>(viewport[2]), h = static_cast<u32>(viewport[3]);
        if (!count || !w || !h) return;
        if (outline_size_[0] != w || outline_size_[1] != h) {  // the mask: one per viewport size, kept
            destroy_texture(outline_mask_);
            outline_mask_ = create_render_target(w, h);
            outline_size_[0] = w;
            outline_size_[1] = h;
        }
        const f32 clear[4] = {0, 0, 0, 0};
        set_render_target(outline_mask_, clear);
        for (u32 i = 0; i < count; ++i) {  // each as plain white: only where it covers matters
            MeshDraw d = draws[i];
            d.unlit = true;
            d.wireframe = false;
            d.debug_view = 0;
            d.blend = 0;
            d.alpha_cutoff = 0.0f;
            d.material_shader = {};
            d.base_color_texture = {};
            d.color[0] = d.color[1] = d.color[2] = d.color[3] = 1.0f;
            draw_mesh(d);
        }
        f32 k[48] = {};
        k[0] = 1.0f / static_cast<f32>(w);
        k[1] = 1.0f / static_cast<f32>(h);
        for (int c = 0; c < 4; ++c) k[8 + c] = color[c];
        post_pass(post_outline_.Get(), {}, viewport, outline_mask_, {}, {}, k);
        set_render_target({}, nullptr);  // the mask back to a shader resource
        set_viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    }

    void begin_frame(const f32 clear_color[4]) override {
        frame_ = swapchain_->GetCurrentBackBufferIndex();
        wait_for_fence(frame_fence_values_[frame_]);
        retired_[frame_].clear();  // that frame is done on the GPU: what it was the last to use goes
        for (const D3D12_CPU_DESCRIPTOR_HANDLE cpu : retired_srvs_[frame_]) free_srv(cpu);
        retired_srvs_[frame_].clear();
        if (timestamps_set_[frame_] && timestamp_readback_ && timestamp_frequency_) {  // this slot's last frame has finished: its timings
            const D3D12_RANGE range{sizeof(u64) * frame_ * kGpuTimestamps, sizeof(u64) * (frame_ + 1) * kGpuTimestamps};
            u64* ticks = nullptr;
            if (SUCCEEDED(timestamp_readback_->Map(0, &range, reinterpret_cast<void**>(&ticks)))) {
                const u64* t = ticks + frame_ * kGpuTimestamps;
                for (u32 i = 0; i < kGpuTimestamps; ++i) {
                    gpu_ms_[i] = (timestamps_set_[frame_] >> i) & 1u ? static_cast<f64>(t[i] - t[0]) * 1000.0 / static_cast<f64>(timestamp_frequency_) : -1.0;
                }
                const D3D12_RANGE nothing{0, 0};
                timestamp_readback_->Unmap(0, &nothing);
                have_timings_ = true;
            }
            timestamps_set_[frame_] = 0;
        }
        allocators_[frame_]->Reset();
        list_->Reset(allocators_[frame_].Get(), nullptr);

        transition(back_buffers_[frame_].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(frame_);
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
        list_->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
        list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<f32>(width_), static_cast<f32>(height_), 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        list_->RSSetViewports(1, &viewport);
        list_->RSSetScissorRects(1, &scissor);
        light_offset_ = 0;
        joint_offset_ = 0;
        particle_offset_ = 0;
        ++frame_number_;
        run_morphs();
        set_lights(default_lights());
        set_shadow_map({}, {});
    }

    void draw_test_triangle(f32 scale, f32 rotation_radians) override {
        const f32 params[3] = {scale, rotation_radians, height_ ? static_cast<f32>(width_) / static_cast<f32>(height_) : 1.0f};
        list_->SetGraphicsRootSignature(root_signature_.Get());
        list_->SetGraphicsRoot32BitConstants(0, 3, params, 0);
        list_->SetPipelineState(triangle_pipeline_.Get());
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list_->DrawInstanced(3, 1, 0, 0);
    }

    bool init_debug_ui() override {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = kDebugUiDescriptors;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (!check(device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&srv_heap_)), "CreateDescriptorHeap (debug UI)")) return false;
        srv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        ImGui_ImplDX12_InitInfo init;
        init.Device = device_.Get();
        init.CommandQueue = queue_.Get();
        init.NumFramesInFlight = kBackBufferCount;
        init.RTVFormat = kBackBufferFormat;
        init.DSVFormat = kDepthFormat;  // ImGui draws while the depth buffer is bound
        init.UserData = this;
        init.SrvDescriptorHeap = srv_heap_.Get();
        init.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
            static_cast<D3D12Device*>(info->UserData)->alloc_srv(*cpu, *gpu);
        };
        init.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
            static_cast<D3D12Device*>(info->UserData)->free_srv(cpu);
        };
        debug_ui_ = ImGui_ImplDX12_Init(&init);
        // Defaults for meshes without textures: white (colour, occlusion / roughness / metallic) and a flat normal.
        const u32 white = 0xFFFFFFFFu, flat = 0xFFFF8080u;  // RGBA bytes, little-endian
        const void* white_mip[1] = {&white};
        const void* flat_mip[1] = {&flat};
        white_ = create_texture(1, 1, 1, TextureFormat::RGBA8, white_mip).index() - kMaxTargets;
        {  // the t3..t7 ring: kSceneRing x 5 neighbouring descriptors, white until set
            constexpr u32 n = kSceneRing * 5;
            u32 slot = 0;
            auto free_run = [&](u32 at) {
                for (u32 k = 0; k < n; ++k) {
                    if (srv_used_[at + k]) return false;
                }
                return true;
            };
            while (slot + n < kDebugUiDescriptors && !free_run(slot)) ++slot;
            for (u32 k = 0; k < n; ++k) srv_used_[slot + k] = true;
            scene_cpu_ = srv_heap_->GetCPUDescriptorHandleForHeapStart();
            scene_gpu_ = srv_heap_->GetGPUDescriptorHandleForHeapStart();
            scene_cpu_.ptr += static_cast<SIZE_T>(slot) * srv_size_;
            scene_gpu_.ptr += static_cast<UINT64>(slot) * srv_size_;
            scene_dirty_ = true;
        }
        flat_normal_ = create_texture(1, 1, 1, TextureFormat::RGBA8, flat_mip).index() - kMaxTargets;
        return debug_ui_;
    }

    void debug_ui_new_frame() override { ImGui_ImplDX12_NewFrame(); }

    void render_debug_ui() override {
        if (current_target_ >= 0) set_render_target({}, nullptr);  // the UI always draws to the window
        ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
        list_->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list_.Get());
    }

    void gpu_timestamp(u32 slot) override {
        if (slot >= kGpuTimestamps) return;
        if (!timestamp_heap_) {  // made on first use: frames that never ask cost nothing
            D3D12_QUERY_HEAP_DESC desc{};
            desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            desc.Count = kBackBufferCount * kGpuTimestamps;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = sizeof(u64) * kBackBufferCount * kGpuTimestamps;
            buffer.Height = 1;
            buffer.DepthOrArraySize = 1;
            buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(device_->CreateQueryHeap(&desc, IID_PPV_ARGS(&timestamp_heap_))) ||
                FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&timestamp_readback_)))) {
                timestamp_heap_.Reset();
                return;
            }
            queue_->GetTimestampFrequency(&timestamp_frequency_);
        }
        list_->EndQuery(timestamp_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame_ * kGpuTimestamps + slot);
        timestamps_set_[frame_] |= 1u << slot;
    }

    bool gpu_timings(f64 out_ms[kGpuTimestamps]) override {
        if (!have_timings_) return false;
        for (u32 i = 0; i < kGpuTimestamps; ++i) out_ms[i] = gpu_ms_[i];
        return true;
    }

    // The screenshot command: the back buffer is copied out at end_frame and written as a BMP once the frame is done.
    bool capture_frame(const char* bmp_path) override {
        std::snprintf(capture_path_, sizeof capture_path_, "%s", bmp_path);
        return true;
    }

    void end_frame() override {
        if (current_target_ >= 0) set_render_target({}, nullptr);
        ComPtr<ID3D12Resource> capture;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        if (capture_path_[0]) {  // back buffer -> a readback buffer
            const D3D12_RESOURCE_DESC desc = back_buffers_[frame_]->GetDesc();
            UINT64 total = 0;
            device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = total;
            buffer.Height = 1;
            buffer.DepthOrArraySize = 1;
            buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&capture)),
                      "CreateCommittedResource (capture)")) {
                transition(back_buffers_[frame_].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION from{}, to{};
                from.pResource = back_buffers_[frame_].Get();
                from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                to.pResource = capture.Get();
                to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                to.PlacedFootprint = footprint;
                list_->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                transition(back_buffers_[frame_].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
            }
        }
        if (!capture) transition(back_buffers_[frame_].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        if (timestamps_set_[frame_]) {  // this frame's timestamps into the readback buffer, read when the frame is done
            u32 last = 0;
            for (u32 i = 0; i < kGpuTimestamps; ++i) last = (timestamps_set_[frame_] >> i) & 1u ? i : last;
            for (u32 i = 0; i <= last; ++i) {  // every slot up to the last is resolved: fill the unset ones
                if (!((timestamps_set_[frame_] >> i) & 1u)) list_->EndQuery(timestamp_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame_ * kGpuTimestamps + i);
            }
            list_->ResolveQueryData(timestamp_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame_ * kGpuTimestamps, last + 1, timestamp_readback_.Get(),
                                    sizeof(u64) * frame_ * kGpuTimestamps);
        }
        list_->Close();
        ID3D12CommandList* lists[] = {list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        if (capture) write_capture(capture.Get(), footprint);
        const HRESULT presented = swapchain_->Present(1, 0);  // vsync
        queue_->Signal(fence_.Get(), ++fence_value_);
        frame_fence_values_[frame_] = fence_value_;
        if (!lost_[0] && (device_removed(presented) || g_removed_failures || FAILED(device_->GetDeviceRemovedReason()))) report_device_removed();
    }

    const char* device_lost() const override { return lost_[0] ? lost_ : nullptr; }

    void remove_device() override {
        ComPtr<ID3D12Device5> device5;
        if (SUCCEEDED(device_.As(&device5))) device5->RemoveDevice();
    }

    // Waits for the frame, then writes the copied back buffer (RGBA rows, top-down) as a 24-bit BMP (BGR, bottom-up).
    void write_capture(ID3D12Resource* capture, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint) {
        wait_for_gpu();
        const u32 w = footprint.Footprint.Width, h = footprint.Footprint.Height, row_bytes = (w * 3 + 3) & ~3u;
        u8* mapped = nullptr;
        bool ok = false;
        if (SUCCEEDED(capture->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
            u8 header[54] = {'B', 'M'};
            auto put32 = [&](int at, u32 v) { for (int i = 0; i < 4; ++i) header[at + i] = static_cast<u8>(v >> (8 * i)); };
            put32(2, 54 + row_bytes * h);
            put32(10, 54);
            put32(14, 40);
            put32(18, w);
            put32(22, h);
            header[26] = 1;   // planes
            header[28] = 24;  // bits per pixel
            put32(34, row_bytes * h);
            if (FILE* file = std::fopen(capture_path_, "wb")) {
                ok = std::fwrite(header, 1, sizeof header, file) == sizeof header;
                std::vector<u8> row(static_cast<usize>(row_bytes), 0);
                for (u32 y = 0; ok && y < h; ++y) {
                    const u8* src = mapped + footprint.Offset + static_cast<usize>(h - 1 - y) * footprint.Footprint.RowPitch;
                    for (u32 x = 0; x < w; ++x) {
                        row[x * 3] = src[x * 4 + 2];
                        row[x * 3 + 1] = src[x * 4 + 1];
                        row[x * 3 + 2] = src[x * 4];
                    }
                    ok = std::fwrite(row.data(), 1, row_bytes, file) == row_bytes;
                }
                ok = std::fclose(file) == 0 && ok;
            }
            const D3D12_RANGE nothing{0, 0};
            capture->Unmap(0, &nothing);
        }
        LSD_LOG_INFO("d3d12", ok ? "Saved %s" : "Could not write %s", capture_path_);
        capture_path_[0] = '\0';
    }

private:
    static void fill_adapter_info(IDXGIAdapter1* adapter, ID3D12Device* probe, u32 index, AdapterInfo& out) {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, out.name, sizeof out.name - 1, nullptr, nullptr);
        out.index = index;
        out.vendor = vendor_from_id(d.VendorId);
        out.shared_memory_bytes = d.SharedSystemMemory;

        D3D12_FEATURE_DATA_ARCHITECTURE arch{};
        probe->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof arch);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) out.type = AdapterType::Software;
        else if (arch.UMA) out.type = AdapterType::Integrated;
        else out.type = AdapterType::Discrete;
        // Integrated GPUs report a small carve-out as "dedicated"; the real pool is shared RAM.
        out.dedicated_vram_bytes = out.type == AdapterType::Discrete ? d.DedicatedVideoMemory : 0;

        D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
        if (SUCCEEDED(probe->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5))) {
            out.supports_raytracing = o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0;
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
        if (SUCCEEDED(probe->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof o7))) {
            out.supports_mesh_shaders = o7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1;
        }
    }

    // The device is gone (once): why, and with DRED what the GPU last did and the address it faulted on.
    void report_device_removed() {
        const HRESULT reason = device_->GetDeviceRemovedReason();
        std::snprintf(lost_, sizeof lost_, "%s", removed_reason_name(reason));
        LSD_LOG_ERROR("d3d12", "The GPU device was removed (reason 0x%08X, %s)", static_cast<unsigned>(reason), lost_);
        ComPtr<ID3D12DeviceRemovedExtendedData> dred;
        if (!dred_ || FAILED(device_.As(&dred))) {
            LSD_LOG_ERROR("d3d12", "Run with -gpu-debug to log what the GPU was doing when it failed (DRED).");
            return;
        }
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs))) {
            for (const D3D12_AUTO_BREADCRUMB_NODE* n = crumbs.pHeadAutoBreadcrumbNode; n; n = n->pNext) {
                const u32 done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
                if (done >= n->BreadcrumbCount) continue;  // that list finished: not the culprit
                LSD_LOG_ERROR("d3d12", "DRED: command list '%s' stopped after %u of %u GPU operations; the ones around it:",
                              n->pCommandListDebugNameA ? n->pCommandListDebugNameA : "?", done, n->BreadcrumbCount);
                const u32 from = done > 8 ? done - 8 : 0, to = done + 4 < n->BreadcrumbCount ? done + 4 : n->BreadcrumbCount;
                for (u32 i = from; i < to; ++i) {
                    LSD_LOG_ERROR("d3d12", "DRED:   %s%u %s", i == done ? "-> " : "   ", i, breadcrumb_name(n->pCommandHistory[i]));
                }
            }
        }
        D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault)) && fault.PageFaultVA) {
            LSD_LOG_ERROR("d3d12", "DRED: GPU page fault at 0x%llX", static_cast<unsigned long long>(fault.PageFaultVA));
            for (const D3D12_DRED_ALLOCATION_NODE* a = fault.pHeadExistingAllocationNode; a; a = a->pNext) {
                LSD_LOG_ERROR("d3d12", "DRED:   in a live allocation '%s' (type %d)", a->ObjectNameA ? a->ObjectNameA : "?", static_cast<int>(a->AllocationType));
            }
            for (const D3D12_DRED_ALLOCATION_NODE* a = fault.pHeadRecentFreedAllocationNode; a; a = a->pNext) {
                LSD_LOG_ERROR("d3d12", "DRED:   in a recently freed allocation '%s' (type %d): used after it was released",
                              a->ObjectNameA ? a->ObjectNameA : "?", static_cast<int>(a->AllocationType));
            }
        }
    }

    static const char* breadcrumb_name(D3D12_AUTO_BREADCRUMB_OP op) {
        switch (op) {
            case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SetMarker";
            case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
            case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
            case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
            case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
            case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "ExecuteIndirect";
            case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
            case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
            case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
            case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
            case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return "ResolveSubresource";
            case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "ClearRenderTargetView";
            case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "ClearUnorderedAccessView";
            case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "ClearDepthStencilView";
            case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
            case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE: return "ExecuteBundle";
            case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "Present";
            case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return "ResolveQueryData";
            default: return "other";
        }
    }

    // Sends debug-layer warnings and errors to our log (by default they only reach a debugger).
    void forward_debug_messages() {
        ComPtr<ID3D12InfoQueue1> queue;
        if (FAILED(device_.As(&queue))) {
            LSD_LOG_WARN("d3d12", "Debug messages only visible in a debugger (ID3D12InfoQueue1 unavailable).");
            return;
        }
        DWORD cookie = 0;
        queue->RegisterMessageCallback(
            [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR text, void*) {
                // Render targets are cleared to theme colours on purpose, so no fixed optimised clear value.
                if (id == D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE) return;
                if (severity <= D3D12_MESSAGE_SEVERITY_ERROR) LSD_LOG_ERROR("d3d12", "%s", text);
                else if (severity == D3D12_MESSAGE_SEVERITY_WARNING) LSD_LOG_WARN("d3d12", "%s", text);
            },
            D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
    }

    // Free-list of slots in the debug UI descriptor heap.
    void alloc_srv(D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu) {
        u32 slot = 0;
        while (slot < kDebugUiDescriptors && srv_used_[slot]) ++slot;
        if (slot == kDebugUiDescriptors) {
            LSD_LOG_ERROR("d3d12", "Debug UI descriptor heap is full (%u).", kDebugUiDescriptors);
            slot = 0;
        }
        srv_used_[slot] = true;
        cpu = srv_heap_->GetCPUDescriptorHandleForHeapStart();
        gpu = srv_heap_->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(slot) * srv_size_;
        gpu.ptr += static_cast<UINT64>(slot) * srv_size_;
    }

    void free_srv(D3D12_CPU_DESCRIPTOR_HANDLE cpu) {
        const SIZE_T slot = (cpu.ptr - srv_heap_->GetCPUDescriptorHandleForHeapStart().ptr) / srv_size_;
        if (slot < kDebugUiDescriptors) srv_used_[slot] = false;
    }

    struct Buffer {
        ComPtr<ID3D12Resource> resource;
        u64 size = 0;
        BufferUsage usage = BufferUsage::Vertex;
        u32 generation = 0;
        u64 read_frame = 0;  // GPU particle state: the frame it is a shader resource in (it decays to COMMON after each)
    };

    struct Target {
        ComPtr<ID3D12Resource> color;
        ComPtr<ID3D12Resource> depth;
        D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu{};
        D3D12_CPU_DESCRIPTOR_HANDLE depth_srv_cpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE depth_srv_gpu{};
        u32 width = 0;
        u32 height = 0;
        u32 generation = 0;
    };

    struct Texture {
        ComPtr<ID3D12Resource> resource;
        D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu{};
        u32 generation = 0;
    };

    Texture* lookup_texture(TextureHandle handle) {
        if (!handle.valid() || handle.index() < kMaxTargets || handle.index() >= kMaxTargets + kMaxTextures) return nullptr;
        Texture& t = textures_[handle.index() - kMaxTargets];
        if (!t.resource || (t.generation & TextureHandle::kGenerationMask) != handle.generation()) return nullptr;
        return &t;
    }

    struct ShadowMap {
        ComPtr<ID3D12Resource> resource;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu{};
        u32 size = 0;
        u32 generation = 0;
    };

    ShadowMap* lookup_shadow(TextureHandle handle) {
        const u32 first = kMaxTargets + kMaxTextures;
        if (!handle.valid() || handle.index() < first || handle.index() >= first + kMaxShadowMaps) return nullptr;
        ShadowMap& m = shadow_maps_[handle.index() - first];
        if (!m.resource || (m.generation & TextureHandle::kGenerationMask) != handle.generation()) return nullptr;
        return &m;
    }

    Target* lookup(TextureHandle handle) {
        if (!handle.valid() || handle.index() >= kMaxTargets) return nullptr;
        Target& t = targets_[handle.index()];
        if (!t.color || (t.generation & TextureHandle::kGenerationMask) != handle.generation()) return nullptr;
        return &t;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE target_rtv(u32 slot) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = target_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(slot) * rtv_size_;
        return h;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE target_dsv(u32 slot) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = target_dsv_heap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(slot) * dsv_size_;
        return h;
    }

    Buffer* lookup(BufferHandle handle) {
        if (!handle.valid() || handle.index() >= kMaxBuffers) return nullptr;
        Buffer& b = buffers_[handle.index()];
        if (!b.resource || (b.generation & BufferHandle::kGenerationMask) != handle.generation()) return nullptr;
        return &b;
    }

    bool create_depth_buffer() {
        depth_.Reset();
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width_;
        desc.Height = height_;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = kDepthFormat;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = kDepthFormat;
        clear.DepthStencil.Depth = 1.0f;
        if (!check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                                    IID_PPV_ARGS(&depth_)), "CreateCommittedResource (depth)")) return false;
        device_->CreateDepthStencilView(depth_.Get(), nullptr, dsv_heap_->GetCPUDescriptorHandleForHeapStart());
        return true;
    }

    bool create_pipelines() {
        // Root constants, then one table per mesh texture (t0 base colour, t1 normal, t2 occlusion / roughness / metallic).
        // 63 of 64 DWORDs: 52 constants, 4 tables, 3 root CBVs (2 each).
        D3D12_ROOT_PARAMETER params[10]{};
        D3D12_DESCRIPTOR_RANGE scene_range{};  // t3 / t4: the sun's and the local lights' shadow atlases, t5 decals, t6 reflection captures, t7 lightmaps
        scene_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        scene_range.NumDescriptors = 5;
        scene_range.BaseShaderRegister = 3;
        params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[5].DescriptorTable.NumDescriptorRanges = 1;
        params[5].DescriptorTable.pDescriptorRanges = &scene_range;
        params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_DESCRIPTOR_RANGE emissive_range{};  // t8: the emissive map
        emissive_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        emissive_range.NumDescriptors = 1;
        emissive_range.BaseShaderRegister = 8;
        params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[6].DescriptorTable.NumDescriptorRanges = 1;
        params[6].DescriptorTable.pDescriptorRanges = &emissive_range;
        params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // b3: material graph parameters (63 of 64 DWORDs)
        params[8].Descriptor.ShaderRegister = 3;
        params[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // b2: skinning matrices
        params[7].Descriptor.ShaderRegister = 2;
        params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // b1: lights
        params[4].Descriptor.ShaderRegister = 1;
        params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = kRootConstants;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_DESCRIPTOR_RANGE ranges[3]{};
        for (UINT i = 0; i < 3; ++i) {
            ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[i].NumDescriptors = 1;
            ranges[i].BaseShaderRegister = i;
            params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
            params[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
            params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_ANISOTROPIC;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MaxAnisotropy = 8;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC samplers[3] = {sampler, {}, {}};
        samplers[2].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;  // s2: post-process taps, clamped at the edges
        samplers[2].AddressU = samplers[2].AddressV = samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[2].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[2].ShaderRegister = 2;
        samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;  // s1: 2x2 filtered depth compare
        samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[1].ShaderRegister = 1;
        samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_DESCRIPTOR_RANGE depth_range{};  // t9: the target's depth for MeshDraw::scene_depth (64 of 64 DWORDs)
        depth_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        depth_range.NumDescriptors = 1;
        depth_range.BaseShaderRegister = 9;
        params[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[9].DescriptorTable.NumDescriptorRanges = 1;
        params[9].DescriptorTable.pDescriptorRanges = &depth_range;
        params[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rs_desc{};
        rs_desc.NumParameters = 10;
        rs_desc.pParameters = params;
        rs_desc.NumStaticSamplers = 3;
        rs_desc.pStaticSamplers = samplers;
        rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> rs_blob, error;
        if (!check(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &error), "D3D12SerializeRootSignature")) return false;
        if (!check(device_->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&root_signature_)), "CreateRootSignature")) return false;
        // Light buffers: one persistently mapped upload buffer per frame in flight.
        for (u32 i = 0; i < kBackBufferCount; ++i) {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = kLightSlot * kLightSlots;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (!check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&light_buffers_[i])), "CreateCommittedResource (lights)")) return false;
            const D3D12_RANGE no_read{0, 0};
            light_buffers_[i]->Map(0, &no_read, reinterpret_cast<void**>(&light_mapped_[i]));
            desc.Width = kJointRingBytes;
            if (!check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&joint_buffers_[i])), "CreateCommittedResource (joints)")) return false;
            joint_buffers_[i]->Map(0, &no_read, reinterpret_cast<void**>(&joint_mapped_[i]));
            desc.Width = kParticleRingBytes;
            if (!check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&particle_buffers_[i])), "CreateCommittedResource (particles)")) return false;
            particle_buffers_[i]->Map(0, &no_read, reinterpret_cast<void**>(&particle_mapped_[i]));
        }

        {
            const usize n = sizeof kMeshHlsl - 1;
            const ShaderJob slow[] = {{kMeshHlsl, n, "vs_main", "vs_5_0", nullptr},           {kMeshHlsl, n, "ps_main", "ps_5_0", nullptr},
                                      {kMeshHlsl, n, "vs_main", "vs_5_0", kSkinnedDefines},   {kMeshHlsl, n, "ps_main", "ps_5_0", kSkinnedDefines},
                                      {kMeshHlsl, n, "vs_main", "vs_5_0", kInstancedDefines}, {kMeshHlsl, n, "ps_main", "ps_5_0", kInstancedDefines},
                                      {kMeshHlsl, n, "vs_sky", "vs_5_0", nullptr},            {kMeshHlsl, n, "ps_sky", "ps_5_0", nullptr}};
            precompile(slow, std::size(slow));
        }
        const D3D12_INPUT_ELEMENT_DESC* mesh_layout = kMeshLayout;
        const D3D12_INPUT_ELEMENT_DESC* skinned_layout = kSkinnedLayout;
        const D3D_SHADER_MACRO* skinned_defines = kSkinnedDefines;
        static const D3D12_INPUT_ELEMENT_DESC line_layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        // Blended variants: [static, skinned, instanced] x [translucent, additive, overdraw].
        const D3D12_INPUT_LAYOUT_DESC blend_layouts[3] = {{mesh_layout, 4}, {skinned_layout, 5}, {mesh_layout, 4}};
        const D3D_SHADER_MACRO* blend_defines[3] = {nullptr, skinned_defines, kInstancedDefines};
        for (u32 kind = 0; kind < 3; ++kind) {
            for (u32 b = 0; b < 3; ++b) {
                if (!create_pipeline(kMeshHlsl, sizeof kMeshHlsl, blend_layouts[kind], D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, true, blend_pipelines_[kind][b], false,
                                     blend_defines[kind], b + 1)) {
                    return false;
                }
            }
        }
        pso_format_ = kBackBufferFormat;  // the test triangle draws to the window; everything else into targets
        const bool triangle = create_pipeline(kTriangleHlsl, sizeof kTriangleHlsl, {nullptr, 0}, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, false, triangle_pipeline_);
        pso_format_ = kTargetFormat;
        return triangle &&
               create_pipeline(kMeshHlsl, sizeof kMeshHlsl, {mesh_layout, 4}, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, true, mesh_pipeline_) &&
               create_pipeline(kMeshHlsl, sizeof kMeshHlsl, {mesh_layout, 4}, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, true, mesh_wire_pipeline_, true) &&
               create_pipeline(kMeshHlsl, sizeof kMeshHlsl, {mesh_layout, 4}, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, true, instanced_pipeline_, false, kInstancedDefines) &&
               create_pipeline(kMeshHlsl, sizeof kMeshHlsl, {mesh_layout, 4}, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, true, instanced_wire_pipeline_, true, kInstancedDefines) &&
               create_pipeline(kLinesHlsl, sizeof kLinesHlsl, {line_layout, 2}, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE, true, line_pipeline_) &&
               create_pipeline(kMeshHlsl, sizeof kMeshHlsl, {skinned_layout, 5}, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, true, skinned_pipeline_, false, skinned_defines) &&
               create_pipeline(kMeshHlsl, sizeof kMeshHlsl, {skinned_layout, 5}, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, true, skinned_wire_pipeline_, true, skinned_defines) &&
               create_shadow_pipeline({mesh_layout, 4}, nullptr, shadow_pipeline_) &&
               create_shadow_pipeline({skinned_layout, 5}, skinned_defines, skinned_shadow_pipeline_) &&
               create_shadow_pipeline({mesh_layout, 4}, kInstancedDefines, instanced_shadow_pipeline_) &&
               create_shadow_pipeline({mesh_layout, 4}, kCutoutDefines, cutout_shadow_pipeline_, true) &&
               create_shadow_pipeline({skinned_layout, 5}, kSkinnedCutoutDefines, skinned_cutout_shadow_pipeline_, true) &&
               create_particle_pipeline(false, particle_alpha_pipeline_) && create_particle_pipeline(true, particle_add_pipeline_) && create_sky_pipeline() &&
               create_ui_pipeline() && create_post_pipelines();
    }

    // A post pass's pipeline: a full-screen triangle into the colour target; `over` blends it onto what is there.
    bool make_post_pso(ID3DBlob* vs, ID3DBlob* ps, bool over, ComPtr<ID3D12PipelineState>& out, DXGI_FORMAT format = kTargetFormat) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = root_signature_.Get();
        pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        if (over) {  // painted over the image (the outline)
            D3D12_RENDER_TARGET_BLEND_DESC& b = pso.BlendState.RenderTarget[0];
            b.BlendEnable = TRUE;
            b.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            b.BlendOp = D3D12_BLEND_OP_ADD;
            b.SrcBlendAlpha = D3D12_BLEND_ONE;
            b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.DepthStencilState.DepthEnable = FALSE;
        pso.DSVFormat = kDepthFormat;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = format;
        pso.SampleDesc.Count = 1;
        return check(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(out.GetAddressOf())), "CreateGraphicsPipelineState (post)");
    }

    bool create_post_pipelines() {
        ComPtr<ID3DBlob> vs;
        if (!compile(kPostHlsl, sizeof kPostHlsl - 1, "vs_full", "vs_5_0", vs, nullptr)) return false;
        struct Entry {
            const char* name;
            ComPtr<ID3D12PipelineState>* out;
        };
        const Entry entries[] = {{"ps_prefilter", &post_prefilter_}, {"ps_down", &post_down_}, {"ps_up", &post_up_}, {"ps_composite", &post_composite_},
                                 {"ps_fxaa", &post_fxaa_}, {"ps_ssr", &post_ssr_}, {"ps_ao", &post_ao_}, {"ps_ao_apply", &post_ao_apply_}, {"ps_taa", &post_taa_}, {"ps_dof", &post_dof_}, {"ps_motion", &post_motion_},
                                 {"ps_meter", &post_meter_}, {"ps_expose", &post_expose_}, {"ps_outline", &post_outline_}, {"ps_velocity", &post_velocity_}};
        for (const Entry& e : entries) {
            ComPtr<ID3DBlob> ps;
            if (!compile(kPostHlsl, sizeof kPostHlsl - 1, e.name, "ps_5_0", ps, nullptr)) return false;
            const bool outline = std::strcmp(e.name, "ps_outline") == 0;  // painted over the window
            if (!make_post_pso(vs.Get(), ps.Get(), outline, *e.out, outline ? kBackBufferFormat : kTargetFormat)) return false;
            // The last passes write into a target or straight to the window: one of each.
            if (std::strcmp(e.name, "ps_composite") == 0 && !make_post_pso(vs.Get(), ps.Get(), false, post_composite_window_, kBackBufferFormat)) return false;
            if (std::strcmp(e.name, "ps_fxaa") == 0 && !make_post_pso(vs.Get(), ps.Get(), false, post_fxaa_window_, kBackBufferFormat)) return false;
        }
        return true;
    }

    bool create_ui_pipeline() {
        static const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        ComPtr<ID3DBlob> vs, ps;
        if (!compile(kUiHlsl, sizeof kUiHlsl - 1, "vs_main", "vs_5_0", vs, nullptr) || !compile(kUiHlsl, sizeof kUiHlsl - 1, "ps_main", "ps_5_0", ps, nullptr)) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = root_signature_.Get();
        pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso.InputLayout = {layout, 3};
        D3D12_RENDER_TARGET_BLEND_DESC& blend = pso.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;  // premultiplied alpha
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.DepthStencilState.DepthEnable = FALSE;
        pso.DSVFormat = kDepthFormat;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = kBackBufferFormat;
        pso.SampleDesc.Count = 1;
        if (!check(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&ui_pipeline_)), "CreateGraphicsPipelineState (ui)")) return false;
        pso.RTVFormats[0] = kTargetFormat;  // into a render target (the UI window's preview)
        return check(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&ui_target_pipeline_)), "CreateGraphicsPipelineState (ui, target)");
    }

    bool create_sky_pipeline() {
        ComPtr<ID3DBlob> vs, ps;
        if (!compile(kMeshHlsl, sizeof kMeshHlsl - 1, "vs_sky", "vs_5_0", vs, nullptr) || !compile(kMeshHlsl, sizeof kMeshHlsl - 1, "ps_sky", "ps_5_0", ps, nullptr)) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = root_signature_.Get();
        pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.DepthStencilState.DepthEnable = FALSE;  // behind everything: drawn first, depth untouched
        pso.DSVFormat = kDepthFormat;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = kTargetFormat;
        pso.SampleDesc.Count = 1;
        return check(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&sky_pipeline_)), "CreateGraphicsPipelineState (sky)");
    }

    bool create_particle_pipeline(bool additive, ComPtr<ID3D12PipelineState>& out, bool gpu = false) {
        static const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        ComPtr<ID3DBlob> vs, ps;
        static const D3D_SHADER_MACRO draw_defines[] = {{"PARTICLE_DRAW", "1"}, {nullptr, nullptr}};
        if (!(gpu ? compile(kGpuParticlesHlsl, sizeof kGpuParticlesHlsl - 1, "gpu_particle_vs", "vs_5_0", vs, draw_defines)
                  : compile(kParticleHlsl, sizeof kParticleHlsl - 1, "vs_main", "vs_5_0", vs, nullptr)) ||
            !compile(kParticleHlsl, sizeof kParticleHlsl - 1, "ps_main", "ps_5_0", ps, nullptr)) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = gpu ? gpu_sprite_root_.Get() : root_signature_.Get();
        pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso.InputLayout = {gpu ? nullptr : layout, gpu ? 0u : 3u};
        D3D12_RENDER_TARGET_BLEND_DESC& blend = pso.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend = additive ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.DepthStencilState.DepthEnable = TRUE;
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;  // sprites never hide each other
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pso.DSVFormat = kDepthFormat;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = kTargetFormat;
        pso.SampleDesc.Count = 1;
        return check(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&out)), "CreateGraphicsPipelineState (particles)");
    }

    bool create_pipeline(const char* source, usize source_size, D3D12_INPUT_LAYOUT_DESC layout, D3D12_PRIMITIVE_TOPOLOGY_TYPE topology,
                         bool depth_test, ComPtr<ID3D12PipelineState>& out, bool wireframe = false, const D3D_SHADER_MACRO* defines = nullptr, u32 blend = 0) {
        ComPtr<ID3DBlob> vs, ps;
        if (!compile(source, source_size - 1, "vs_main", "vs_5_0", vs, defines) || !compile(source, source_size - 1, "ps_main", "ps_5_0", ps, defines)) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = root_signature_.Get();
        pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso.InputLayout = layout;
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        if (blend) {  // 1 translucent (over), 2 / 3 additive (3: no depth test)
            D3D12_RENDER_TARGET_BLEND_DESC& b = pso.BlendState.RenderTarget[0];
            b.BlendEnable = TRUE;
            b.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            b.DestBlend = blend == 1 ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ONE;
            b.BlendOp = D3D12_BLEND_OP_ADD;
            // The scene target's alpha is the surface's mirror-likeness (SSR): translucent surfaces cover it by their
            // opacity (no floor reflection through opaque glass), additive ones leave it.
            b.SrcBlendAlpha = D3D12_BLEND_ZERO;
            b.DestBlendAlpha = blend == 1 ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ONE;
            b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;  // shortcut: no culling until imported winding is verified
        pso.RasterizerState.FrontCounterClockwise = TRUE;  // as in OpenGL: SV_IsFrontFace must agree for two-sided lighting
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.DepthStencilState.DepthEnable = depth_test && blend != 3;
        pso.DepthStencilState.DepthWriteMask = depth_test && !blend ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;  // blended: tested, not written
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pso.DSVFormat = kDepthFormat;
        pso.PrimitiveTopologyType = topology;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = pso_format_;
        pso.SampleDesc.Count = 1;
        return check(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&out)), "CreateGraphicsPipelineState");
    }

    bool create_shadow_pipeline(D3D12_INPUT_LAYOUT_DESC layout, const D3D_SHADER_MACRO* defines, ComPtr<ID3D12PipelineState>& out, bool cutout = false) {
        ComPtr<ID3DBlob> vs, ps;
        if (!compile(kShadowHlsl, sizeof kShadowHlsl - 1, "vs_main", "vs_5_0", vs, defines)) return false;
        if (cutout && !compile(kShadowHlsl, sizeof kShadowHlsl - 1, "ps_cutout", "ps_5_0", ps, defines)) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = root_signature_.Get();
        pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        if (cutout) pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso.InputLayout = layout;
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = FALSE;  // casters behind the light's near plane still cast (pancaking)
        pso.RasterizerState.SlopeScaledDepthBias = 2.0f;  // against shadow acne on slopes; the shader adds a normal offset
        pso.DepthStencilState.DepthEnable = TRUE;
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pso.DSVFormat = kDepthFormat;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 0;
        pso.SampleDesc.Count = 1;
        return check(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&out)), "CreateGraphicsPipelineState (shadow)");
    }

    // GPU-driven culling (MeshDraw::gpu_cull): reset the visible count, test every instance's box
    // against the frustum (as rhi::instance_visible) packing the visible rows, then one indirect draw
    // per batch of 256 (the instanced shader's constant buffer holds 256). Optional: if it fails to
    // build, gpu_cull draws simply draw every instance.
    bool create_culling() {
        D3D12_ROOT_PARAMETER params[5]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[3].Descriptor.ShaderRegister = 1;
        static const D3D12_DESCRIPTOR_RANGE hiz_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};  // t1: the Hi-Z
        params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[4].DescriptorTable = {1, &hiz_range};
        for (D3D12_ROOT_PARAMETER& p : params) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 5;
        rs.pParameters = params;
        ComPtr<ID3DBlob> blob, error;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
            FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&cull_root_)))) return false;
        const char* entries[3] = {"cull_reset", "cull_main", "cull_args"};
        for (u32 k = 0; k < 3; ++k) {
            ComPtr<ID3DBlob> cs;
            if (!compile(kCullHlsl, sizeof kCullHlsl - 1, entries[k], "cs_5_0", cs)) return false;
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = cull_root_.Get();
            pso.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
            if (FAILED(device_->CreateComputePipelineState(&pso, IID_PPV_ARGS(&cull_pso_[k])))) return false;
        }
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        desc.Width = 4 * static_cast<u64>(kMaxCulledInstances) * 48;  // each LOD's rows; buffers start in COMMON and promote to UAV on first use
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&cull_out_)))) return false;
        desc.Width = 16 + 4 * (kMaxCulledInstances / kMaxJoints) * 20;
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&cull_args_)))) return false;
        D3D12_INDIRECT_ARGUMENT_DESC argument{};
        argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC signature{};
        signature.ByteStride = 20;
        signature.NumArgumentDescs = 1;
        signature.pArgumentDescs = &argument;
        if (FAILED(device_->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&cull_signature_)))) return false;
        // The Hi-Z builder (optional: without it no occlusion test): 4 constants, its source (t0) and destination (u0).
        D3D12_ROOT_PARAMETER hiz[3]{};
        hiz[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        hiz[0].Constants.Num32BitValues = 4;
        static const D3D12_DESCRIPTOR_RANGE src{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
        static const D3D12_DESCRIPTOR_RANGE dst{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
        hiz[1].ParameterType = hiz[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        hiz[1].DescriptorTable = {1, &src};
        hiz[2].DescriptorTable = {1, &dst};
        for (D3D12_ROOT_PARAMETER& p : hiz) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC hrs{};
        hrs.NumParameters = 3;
        hrs.pParameters = hiz;
        ComPtr<ID3DBlob> hiz_blob, hiz_error, hiz_cs;
        static const D3D_SHADER_MACRO hiz_defines[] = {{"HIZ_PASS", "1"}, {nullptr, nullptr}};
        if (SUCCEEDED(D3D12SerializeRootSignature(&hrs, D3D_ROOT_SIGNATURE_VERSION_1, &hiz_blob, &hiz_error)) &&
            SUCCEEDED(device_->CreateRootSignature(0, hiz_blob->GetBufferPointer(), hiz_blob->GetBufferSize(), IID_PPV_ARGS(&hiz_root_))) &&
            compile(kCullHlsl, sizeof kCullHlsl - 1, "hiz_down", "cs_5_0", hiz_cs, hiz_defines)) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = hiz_root_.Get();
            pso.CS = {hiz_cs->GetBufferPointer(), hiz_cs->GetBufferSize()};
            if (FAILED(device_->CreateComputePipelineState(&pso, IID_PPV_ARGS(&hiz_pso_)))) hiz_root_.Reset();
        }
        return true;
    }

    // The Hi-Z (PostDraw::build_hiz) from a scene target's depth: half its size, halving to 1 x 1, each texel the farthest
    // of 2 x 2. Between builds every mip is a non-pixel shader resource (the cull pass reads it).
    void build_hiz(const Target& scene, const f32 view_proj[16]) {
        if (!hiz_pso_) return;
        const u32 w = scene.width > 1 ? (scene.width + 1) / 2 : 1, h = scene.height > 1 ? (scene.height + 1) / 2 : 1;
        u32 mips = 1;
        while ((std::max(w, h) >> mips) > 0 && mips < kMaxHizMips) ++mips;
        if (!hiz_ || hiz_size_[0] != w || hiz_size_[1] != h) {
            wait_for_gpu();
            if (hiz_) {
                free_srv(hiz_all_cpu_);
                for (u32 m = 0; m < hiz_mips_; ++m) {
                    free_srv(hiz_srv_cpu_[m]);
                    free_srv(hiz_uav_cpu_[m]);
                }
                hiz_.Reset();
            }
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = w;
            desc.Height = h;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = static_cast<UINT16>(mips);
            desc.Format = DXGI_FORMAT_R32_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (!check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&hiz_)),
                       "CreateCommittedResource (Hi-Z)")) return;
            alloc_srv(hiz_all_cpu_, hiz_all_gpu_);
            device_->CreateShaderResourceView(hiz_.Get(), nullptr, hiz_all_cpu_);
            for (u32 m = 0; m < mips; ++m) {
                D3D12_SHADER_RESOURCE_VIEW_DESC one{};
                one.Format = DXGI_FORMAT_R32_FLOAT;
                one.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                one.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                one.Texture2D.MostDetailedMip = m;
                one.Texture2D.MipLevels = 1;
                alloc_srv(hiz_srv_cpu_[m], hiz_srv_gpu_[m]);
                device_->CreateShaderResourceView(hiz_.Get(), &one, hiz_srv_cpu_[m]);
                D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
                uav.Format = DXGI_FORMAT_R32_FLOAT;
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uav.Texture2D.MipSlice = m;
                alloc_srv(hiz_uav_cpu_[m], hiz_uav_gpu_[m]);
                device_->CreateUnorderedAccessView(hiz_.Get(), nullptr, &uav, hiz_uav_cpu_[m]);
            }
            hiz_size_[0] = w;
            hiz_size_[1] = h;
            hiz_mips_ = mips;
        }
        auto sub = [&](u32 m, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = hiz_.Get();
            b.Transition.Subresource = m;
            b.Transition.StateBefore = from;
            b.Transition.StateAfter = to;
            list_->ResourceBarrier(1, &b);
        };
        ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
        list_->SetDescriptorHeaps(1, heaps);
        transition(scene.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list_->SetComputeRootSignature(hiz_root_.Get());
        list_->SetPipelineState(hiz_pso_.Get());
        u32 sw = scene.width, sh = scene.height;
        for (u32 m = 0; m < hiz_mips_; ++m) {  // mip m written, then readable for the next
            const u32 dw = std::max(hiz_size_[0] >> m, 1u), dh = std::max(hiz_size_[1] >> m, 1u);
            const u32 size[4] = {dw, dh, sw, sh};
            sub(m, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list_->SetComputeRoot32BitConstants(0, 4, size, 0);
            list_->SetComputeRootDescriptorTable(1, m ? hiz_srv_gpu_[m - 1] : scene.depth_srv_gpu);
            list_->SetComputeRootDescriptorTable(2, hiz_uav_gpu_[m]);
            list_->Dispatch((dw + 7) / 8, (dh + 7) / 8, 1);
            sub(m, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            sw = dw;
            sh = dh;
        }
        transition(scene.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        std::memcpy(hiz_view_proj_, view_proj, sizeof hiz_view_proj_);
        hiz_valid_ = true;
    }

    void uav_barrier(ID3D12Resource* resource) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = resource;
        list_->ResourceBarrier(1, &b);
    }

    // The culled copies, per LOD (its own index buffer) and batch: rows at (LOD * kMaxCulledInstances + batch * kMaxJoints)
    // * 48 in cull_out_, arguments at 16 + (LOD * batches + batch) * 20; the vertex buffer and pipeline are bound.
    void draw_culled(const MeshDraw& draw, u32 batches) {
        CullConstants c;
        cull_constants(draw, batches, nullptr, hiz_size_, 0, c);
        const u32 lods = (c.info[2] >> 1) & 7u;
        for (u32 l = 0; l < lods; ++l) {
            const Buffer* lib = lookup(l ? draw.lod_indices[l - 1] : draw.indices);
            if (!lib) continue;
            const D3D12_INDEX_BUFFER_VIEW ibv{lib->resource->GetGPUVirtualAddress(), static_cast<UINT>(lib->size), DXGI_FORMAT_R32_UINT};
            list_->IASetIndexBuffer(&ibv);
            for (u32 b = 0; b < batches; ++b) {
                list_->SetGraphicsRootConstantBufferView(7, cull_out_->GetGPUVirtualAddress() + (static_cast<u64>(l) * kMaxCulledInstances + static_cast<u64>(b) * kMaxJoints) * 48);
                list_->ExecuteIndirect(cull_signature_.Get(), 1, cull_args_.Get(), 16 + (static_cast<u64>(l) * batches + b) * 20, nullptr, 0);
            }
        }
        transition(cull_out_.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(cull_args_.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // Morph targets (cull.hlsl's morph_main): base vertices (t0), packed targets (t1), the weights (t2) into the output (u0).
    bool create_morphs() {
        D3D12_ROOT_PARAMETER params[4]{};
        for (u32 k = 0; k < 3; ++k) {
            params[k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            params[k].Descriptor.ShaderRegister = k;
        }
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        for (D3D12_ROOT_PARAMETER& p : params) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 4;
        rs.pParameters = params;
        ComPtr<ID3DBlob> blob, error, cs;
        static const D3D_SHADER_MACRO defines[] = {{"MORPH_PASS", "1"}, {nullptr, nullptr}};
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
            FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&morph_root_))) ||
            !compile(kCullHlsl, sizeof kCullHlsl - 1, "morph_main", "cs_5_0", cs, defines)) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = morph_root_.Get();
        pso.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        return SUCCEEDED(device_->CreateComputePipelineState(&pso, IID_PPV_ARGS(&morph_pso_)));
    }
    // GPU particles: the compute pass (b0 parameters, u0 particles) and the sprites, whose root signature is the
    // particle shader's (b0 constants, t0 sprite, t1 depth) plus the parameters (b2) and the particles (t2) for the vertex shader.
    bool create_gpu_particles() {
        D3D12_ROOT_PARAMETER sim[2]{};
        sim[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        sim[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        for (D3D12_ROOT_PARAMETER& p : sim) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC sim_desc{};
        sim_desc.NumParameters = 2;
        sim_desc.pParameters = sim;
        ComPtr<ID3DBlob> blob, error, cs;
        static const D3D_SHADER_MACRO sim_defines[] = {{"PARTICLE_SIM", "1"}, {nullptr, nullptr}};
        if (FAILED(D3D12SerializeRootSignature(&sim_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
            FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&gpu_particle_sim_root_))) ||
            !compile(kGpuParticlesHlsl, sizeof kGpuParticlesHlsl - 1, "gpu_particle_sim", "cs_5_0", cs, sim_defines)) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = gpu_particle_sim_root_.Get();
        pso.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        if (FAILED(device_->CreateComputePipelineState(&pso, IID_PPV_ARGS(&gpu_particle_sim_)))) return false;
        D3D12_ROOT_PARAMETER params[5]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 24;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        for (UINT i = 0; i < 2; ++i) {
            ranges[i] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, i, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
            params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[1 + i].DescriptorTable = {1, &ranges[i]};
            params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[3].Descriptor.ShaderRegister = 2;
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[4].Descriptor.ShaderRegister = 2;
        params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_STATIC_SAMPLER_DESC sampler{};  // s0, as the particles' linear_wrap
        sampler.Filter = D3D12_FILTER_ANISOTROPIC;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MaxAnisotropy = 8;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = 5;
        desc.pParameters = params;
        desc.NumStaticSamplers = 1;
        desc.pStaticSamplers = &sampler;
        ComPtr<ID3DBlob> sprite_blob;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sprite_blob, &error)) ||
            FAILED(device_->CreateRootSignature(0, sprite_blob->GetBufferPointer(), sprite_blob->GetBufferSize(), IID_PPV_ARGS(&gpu_sprite_root_)))) return false;
        return create_particle_pipeline(false, gpu_particle_alpha_, true) && create_particle_pipeline(true, gpu_particle_add_, true);
    }

    // The morphs queued since the last frame, at its start (outside any pass); each output ends ready to draw.
    void run_morphs() {
        for (const MorphJob& j : pending_morphs_) {
            const Buffer* out = lookup(j.out);
            const Buffer* base = lookup(j.base);
            const Buffer* morphs = lookup(j.morphs);
            if (!morph_pso_ || !out || !base || !morphs) continue;
            const D3D12_GPU_VIRTUAL_ADDRESS weights = upload_constants(j.words, j.word_count * 4);
            if (!weights) break;
            list_->SetComputeRootSignature(morph_root_.Get());
            list_->SetComputeRootShaderResourceView(0, base->resource->GetGPUVirtualAddress());
            list_->SetComputeRootShaderResourceView(1, morphs->resource->GetGPUVirtualAddress());
            list_->SetComputeRootShaderResourceView(2, weights);
            list_->SetComputeRootUnorderedAccessView(3, out->resource->GetGPUVirtualAddress());
            list_->SetPipelineState(morph_pso_.Get());
            list_->Dispatch((j.vertex_count + 63) / 64, 1, 1);
            transition(out->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        }
        pending_morphs_.clear();
    }

    // The compute passes for a gpu_cull draw; false (nothing recorded) if it cannot run: draw all.
    bool run_culling(const MeshDraw& draw, u32 batches) {
        if (!cull_signature_ || draw.instance_count > kMaxCulledInstances) return false;
        CullConstants cull;
        cull_constants(draw, batches, hiz_valid_ ? hiz_view_proj_ : nullptr, hiz_size_, hiz_mips_, cull);
        const D3D12_GPU_VIRTUAL_ADDRESS constants = upload_constants(&cull, sizeof cull);
        const D3D12_GPU_VIRTUAL_ADDRESS rows = constants ? upload_constants(draw.instance_rows, draw.instance_count * 48) : 0;
        if (!rows) return false;
        list_->SetComputeRootSignature(cull_root_.Get());
        list_->SetComputeRootConstantBufferView(0, constants);
        list_->SetComputeRootShaderResourceView(1, rows);
        list_->SetComputeRootUnorderedAccessView(2, cull_out_->GetGPUVirtualAddress());
        list_->SetComputeRootUnorderedAccessView(3, cull_args_->GetGPUVirtualAddress());
        ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
        list_->SetDescriptorHeaps(1, heaps);
        list_->SetComputeRootDescriptorTable(4, hiz_valid_ ? hiz_all_gpu_ : textures_[white_].srv_gpu);  // read only when the constants say so
        list_->SetPipelineState(cull_pso_[0].Get());
        list_->Dispatch(1, 1, 1);
        uav_barrier(cull_args_.Get());
        list_->SetPipelineState(cull_pso_[1].Get());
        list_->Dispatch((draw.instance_count + 63) / 64, 1, 1);
        uav_barrier(cull_args_.Get());
        list_->SetPipelineState(cull_pso_[2].Get());
        list_->Dispatch((batches * 4 + 63) / 64, 1, 1);
        transition(cull_out_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        transition(cull_args_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        return true;
    }

    // From the shader cache when it has it, else compiled and stored there (project_shaders_: the project's folder).
    bool compile(const char* source, usize length, const char* entry, const char* target, ComPtr<ID3DBlob>& out,
                 const D3D_SHADER_MACRO* defines = nullptr) const {
        std::string macros;
        for (const D3D_SHADER_MACRO* d = defines; d && d->Name; ++d) macros.append(d->Name).append("=").append(d->Definition ? d->Definition : "").append(";");
        const u64 key = shader_cache_key({source, length}, entry, target, macros, kShaderCompilerId);
        std::vector<u8> cached;
        if (find_cached_shader(key, cached) && SUCCEEDED(D3DCreateBlob(cached.size(), &out))) {
            std::memcpy(out->GetBufferPointer(), cached.data(), cached.size());
            ++g_shader_cache_hits;
            return true;
        }
        ComPtr<ID3DBlob> errors;
        const u64 started = SDL_GetTicksNS();
        const HRESULT hr = D3DCompile(source, length, "builtin.hlsl", defines, nullptr, entry, target, 0, 0, &out, &errors);
        if (FAILED(hr)) {
            LSD_LOG_ERROR("d3d12", "Shader %s failed: %s", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
            return false;
        }
        ++g_shader_compiles;
        g_shader_compile_ns += SDL_GetTicksNS() - started;
        store_cached_shader(shader_cache_folder(project_shaders_), key, out->GetBufferPointer(), out->GetBufferSize());
        return true;
    }

    // The slow shaders (FXC takes over a second for the lit mesh's pixel shader) compiled side by side into the
    // shader cache; the pipelines built after them read them back. A cached one costs a file read.
    struct ShaderJob {
        const char* source;
        usize length;
        const char* entry;
        const char* target;
        const D3D_SHADER_MACRO* defines;
    };
    void precompile(const ShaderJob* jobs, usize count) const {
        std::vector<std::thread> threads;
        threads.reserve(count);
        for (usize i = 0; i < count; ++i) {
            threads.emplace_back([this, job = jobs[i]] {
                ComPtr<ID3DBlob> blob;
                compile(job.source, job.length, job.entry, job.target, blob, job.defines);
            });
        }
        for (std::thread& t : threads) t.join();
    }

    void create_back_buffer_views() {
        for (u32 i = 0; i < kBackBufferCount; ++i) {
            swapchain_->GetBuffer(i, IID_PPV_ARGS(&back_buffers_[i]));
            device_->CreateRenderTargetView(back_buffers_[i].Get(), nullptr, rtv_handle(i));
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle(u32 i) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * rtv_size_;
        return h;
    }

    void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        list_->ResourceBarrier(1, &b);
    }

    void retire(ComPtr<ID3D12Resource>&& resource) {
        if (resource) retired_[frame_].push_back(std::move(resource));
    }
    void retire_srv(D3D12_CPU_DESCRIPTOR_HANDLE cpu) {
        if (cpu.ptr) retired_srvs_[frame_].push_back(cpu);
    }

    void wait_for_fence(u64 value) {
        if (fence_->GetCompletedValue() >= value) return;
        fence_->SetEventOnCompletion(value, fence_event_);
        WaitForSingleObject(fence_event_, INFINITE);
    }

    void wait_for_gpu() {
        queue_->Signal(fence_.Get(), ++fence_value_);
        wait_for_fence(fence_value_);
    }

    AdapterInfo info_{};
    ComPtr<IDXGIFactory6> factory_;
    ComPtr<IDXGIAdapter3> adapter_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12CommandAllocator> allocators_[kBackBufferCount];
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_ = nullptr;
    u64 fence_value_ = 0;
    u64 frame_fence_values_[kBackBufferCount] = {};
    std::vector<ComPtr<ID3D12Resource>> retired_[kBackBufferCount];       // destroyed while that frame was recorded
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> retired_srvs_[kBackBufferCount];  // their views' slots in the UI heap
    TextureHandle outline_mask_;  // the selection outline's mask, sized to the last viewport
    u32 outline_size_[2] = {};
    ComPtr<ID3D12QueryHeap> timestamp_heap_;  // GPU timings: kGpuTimestamps per frame in flight
    ComPtr<ID3D12Resource> timestamp_readback_;
    u64 timestamp_frequency_ = 0;
    u32 timestamps_set_[kBackBufferCount] = {};
    f64 gpu_ms_[kGpuTimestamps] = {};
    bool have_timings_ = false;
    ComPtr<IDXGISwapChain3> swapchain_;
    ComPtr<ID3D12Resource> back_buffers_[kBackBufferCount];
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    u32 rtv_size_ = 0;
    u32 frame_ = 0;
    u32 width_ = 0;
    u32 height_ = 0;
    char capture_path_[260] = "";  // capture_frame: written at the next end_frame
    ComPtr<ID3D12DescriptorHeap> dsv_heap_;
    ComPtr<ID3D12Resource> depth_;
    ComPtr<ID3D12RootSignature> root_signature_;
    ComPtr<ID3D12PipelineState> triangle_pipeline_;
    ComPtr<ID3D12PipelineState> mesh_pipeline_;
    ComPtr<ID3D12PipelineState> blend_pipelines_[3][3];  // [static, skinned, instanced][translucent, additive, overdraw]
    ComPtr<ID3D12PipelineState> mesh_wire_pipeline_;
    ComPtr<ID3D12PipelineState> instanced_pipeline_, instanced_wire_pipeline_;
    static constexpr u32 kSceneRing = 64;
    D3D12_CPU_DESCRIPTOR_HANDLE scene_cpu_{};  // the ring's start: entries of five (t3 / t4 shadows, t5 decals, t6 reflections, t7 lightmaps)
    D3D12_GPU_DESCRIPTOR_HANDLE scene_gpu_{};
    D3D12_GPU_DESCRIPTOR_HANDLE scene_table_{};  // the entry in use
    ID3D12Resource* scene_resources_[5] = {};    // null: white
    bool scene_shadow_[2] = {};
    bool scene_dirty_ = true;
    u32 scene_at_ = 0;
    ComPtr<ID3D12PipelineState> line_pipeline_;
    ComPtr<ID3D12PipelineState> shadow_pipeline_;
    ComPtr<ID3D12PipelineState> cutout_shadow_pipeline_, skinned_cutout_shadow_pipeline_;  // Masked casters
    ComPtr<ID3D12PipelineState> skinned_pipeline_;
    ComPtr<ID3D12PipelineState> skinned_wire_pipeline_;
    ComPtr<ID3D12PipelineState> skinned_shadow_pipeline_;
    ComPtr<ID3D12PipelineState> instanced_shadow_pipeline_;
    ComPtr<ID3D12Resource> joint_buffers_[kBackBufferCount];
    ComPtr<ID3D12RootSignature> cull_root_;  // GPU culling (create_culling)
    ComPtr<ID3D12PipelineState> cull_pso_[3];
    ComPtr<ID3D12Resource> cull_out_, cull_args_;
    static constexpr u32 kMaxHizMips = 14;
    ComPtr<ID3D12RootSignature> hiz_root_;
    ComPtr<ID3D12PipelineState> hiz_pso_;
    ComPtr<ID3D12Resource> hiz_;  // R32F; every mip a non-pixel shader resource between builds
    D3D12_CPU_DESCRIPTOR_HANDLE hiz_all_cpu_{}, hiz_srv_cpu_[kMaxHizMips]{}, hiz_uav_cpu_[kMaxHizMips]{};
    D3D12_GPU_DESCRIPTOR_HANDLE hiz_all_gpu_{}, hiz_srv_gpu_[kMaxHizMips]{}, hiz_uav_gpu_[kMaxHizMips]{};
    u32 hiz_size_[2] = {}, hiz_mips_ = 0;
    f32 hiz_view_proj_[16] = {};
    bool hiz_valid_ = false;
    ComPtr<ID3D12CommandSignature> cull_signature_;
    ComPtr<ID3D12RootSignature> gpu_particle_sim_root_, gpu_sprite_root_;  // GPU particles (create_gpu_particles)
    ComPtr<ID3D12PipelineState> gpu_particle_sim_, gpu_particle_alpha_, gpu_particle_add_;
    u64 frame_number_ = 0;
    ComPtr<ID3D12RootSignature> morph_root_;  // morph targets (create_morphs)
    ComPtr<ID3D12PipelineState> morph_pso_;
    std::vector<MorphJob> pending_morphs_;
    u8* joint_mapped_[kBackBufferCount] = {};
    u32 joint_offset_ = 0;
    ComPtr<ID3D12Resource> particle_buffers_[kBackBufferCount];
    u8* particle_mapped_[kBackBufferCount] = {};
    u32 particle_offset_ = 0;
    ComPtr<ID3D12PipelineState> particle_alpha_pipeline_, particle_add_pipeline_;
    ComPtr<ID3D12PipelineState> sky_pipeline_;
    ComPtr<ID3D12PipelineState> ui_pipeline_;
    PostShader post_shaders_[kMaxPostShaders];
    ComPtr<ID3D12PipelineState> post_motion_, post_prefilter_, post_down_, post_up_, post_composite_, post_fxaa_, post_ao_, post_ao_apply_, post_taa_, post_dof_, post_ssr_, post_meter_, post_expose_, post_outline_, post_velocity_;
    ComPtr<ID3D12PipelineState> post_composite_window_, post_fxaa_window_, ui_target_pipeline_;  // the window's format (kBackBufferFormat)
    DXGI_FORMAT pso_format_ = kTargetFormat;  // what create_pipeline builds for
    bool project_shaders_ = false;  // compile() stores into the project's shader cache (Material Graphs, post materials), else the user's
    PostChain post_chains_[4];
    u32 post_chain_next_ = 0;
    bool hdr_output_ = false;  // the current LightSetup draws into a scene target
    f32 viewport_[4] = {};  // the last set_viewport, for the shaders' vignette
    MaterialShader material_shaders_[kMaxMaterialShaders];
    ShadowMap shadow_maps_[kMaxShadowMaps];
    ComPtr<ID3D12DescriptorHeap> shadow_dsv_heap_;
    i32 shadow_pass_ = -1;
    Buffer buffers_[kMaxBuffers];
    ComPtr<ID3D12DescriptorHeap> target_rtv_heap_;
    ComPtr<ID3D12DescriptorHeap> target_dsv_heap_;
    u32 dsv_size_ = 0;
    Target targets_[kMaxTargets];
    Texture textures_[kMaxTextures];
    u32 white_ = 0, flat_normal_ = 0;  // default textures (slots in textures_)
    ComPtr<ID3D12Resource> light_buffers_[kBackBufferCount];
    u8* light_mapped_[kBackBufferCount] = {};
    u32 light_offset_ = 0;
    D3D12_GPU_VIRTUAL_ADDRESS current_lights_ = 0;
    ComPtr<ID3D12CommandAllocator> upload_allocator_;
    ComPtr<ID3D12GraphicsCommandList> upload_list_;
    i32 current_target_ = -1;  // slot being rendered to, -1 = the window
    ComPtr<ID3D12DescriptorHeap> srv_heap_;
    u32 srv_size_ = 0;
    bool srv_used_[kDebugUiDescriptors] = {};
    bool debug_ui_ = false;
    bool dred_ = false;    // DRED on (-gpu-debug / -validation)
    char lost_[128] = "";  // why the device was removed; empty while it works
};

} // namespace

Device* create_d3d12_device(const DeviceDesc& desc, Allocator& allocator) {
    void* memory = allocator.allocate(sizeof(D3D12Device), alignof(D3D12Device));
    auto* device = new (memory) D3D12Device();
    if (!device->init(desc)) {
        destroy_device(device, allocator);
        return nullptr;
    }
    return device;
}

} // namespace lsd::rhi
