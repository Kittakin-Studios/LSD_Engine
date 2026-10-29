// Direct3D 12 backend: adapter enumeration, live memory budget, swapchain, depth buffer,
// immutable buffers, built-in lit-mesh / line / test-triangle pipelines and ImGui.
#include "lsd/rhi/rhi.h"
#include "lsd/foundation/log.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_dx12.h>

#include <cstring>
#include <new>

using Microsoft::WRL::ComPtr;

namespace lsd::rhi {

namespace {

constexpr u32 kBackBufferCount = 2;
constexpr u32 kMaxAdapters = 16;
constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr u32 kDebugUiDescriptors = 64;  // ImGui font atlas + user textures
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
constexpr u32 kMaxBuffers = 4096;
constexpr u32 kRootConstants = 32;  // one root-constant block shared by every built-in pipeline

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

// Lit mesh: one directional light plus sky/ground ambient. Constants match rhi::MeshDraw.
constexpr char kMeshHlsl[] = R"(
cbuffer Draw : register(b0) { float4x4 mvp; float4 rot0; float4 rot1; float4 rot2; float4 color; };
struct VsIn { float3 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct VsOut { float4 pos : SV_Position; float3 normal : NORMAL; };
VsOut vs_main(VsIn i) {
    VsOut o;
    o.pos = mul(mvp, float4(i.pos, 1));
    o.normal = rot0.xyz * i.normal.x + rot1.xyz * i.normal.y + rot2.xyz * i.normal.z;
    return o;
}
float4 ps_main(VsOut i) : SV_Target {
    const float3 n = normalize(i.normal);
    const float3 sun = normalize(float3(-0.5, -0.35, 0.8));
    const float ambient = 0.25 + 0.1 * n.z;
    return float4(color.rgb * (ambient + 0.85 * saturate(dot(n, sun))), color.a);
}
)";

constexpr char kLinesHlsl[] = R"(
cbuffer Draw : register(b0) { float4x4 mvp; };
struct VsIn { float3 pos : POSITION; float4 color : COLOR; };
struct VsOut { float4 pos : SV_Position; float4 color : COLOR; };
VsOut vs_main(VsIn i) { VsOut o; o.pos = mul(mvp, float4(i.pos, 1)); o.color = i.color; return o; }
float4 ps_main(VsOut i) : SV_Target { return i.color; }
)";

bool check(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) return true;
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
public:
    ~D3D12Device() override {
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

        return create_pipelines();
    }

    Backend backend() const override { return Backend::D3D12; }
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
        wait_for_gpu();  // shortcut: stalls; switch to deferred release when buffers churn per frame
        b->resource.Reset();
        ++b->generation;
    }

    void set_viewport(f32 x, f32 y, f32 width, f32 height) override {
        const D3D12_VIEWPORT viewport{x, y, width, height, 0.0f, 1.0f};
        const D3D12_RECT scissor{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + width), static_cast<LONG>(y + height)};
        list_->RSSetViewports(1, &viewport);
        list_->RSSetScissorRects(1, &scissor);
    }

    void draw_mesh(const MeshDraw& draw) override {
        const Buffer* vb = lookup(draw.vertices);
        const Buffer* ib = lookup(draw.indices);
        if (!vb || !ib) return;
        f32 constants[kRootConstants];
        std::memcpy(constants, draw.mvp, sizeof draw.mvp);
        std::memcpy(constants + 16, draw.world_rotation, sizeof draw.world_rotation);
        std::memcpy(constants + 28, draw.color, sizeof draw.color);
        list_->SetGraphicsRootSignature(root_signature_.Get());
        list_->SetGraphicsRoot32BitConstants(0, kRootConstants, constants, 0);
        list_->SetPipelineState(mesh_pipeline_.Get());
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VERTEX_BUFFER_VIEW vbv{vb->resource->GetGPUVirtualAddress(), static_cast<UINT>(vb->size), sizeof(MeshVertex)};
        const D3D12_INDEX_BUFFER_VIEW ibv{ib->resource->GetGPUVirtualAddress(), static_cast<UINT>(ib->size), DXGI_FORMAT_R32_UINT};
        list_->IASetVertexBuffers(0, 1, &vbv);
        list_->IASetIndexBuffer(&ibv);
        list_->DrawIndexedInstanced(draw.index_count, 1, 0, 0, 0);
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

    void begin_frame(const f32 clear_color[4]) override {
        frame_ = swapchain_->GetCurrentBackBufferIndex();
        wait_for_fence(frame_fence_values_[frame_]);
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
        return debug_ui_;
    }

    void debug_ui_new_frame() override { ImGui_ImplDX12_NewFrame(); }

    void render_debug_ui() override {
        ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
        list_->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list_.Get());
    }

    void end_frame() override {
        transition(back_buffers_[frame_].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        list_->Close();
        ID3D12CommandList* lists[] = {list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        swapchain_->Present(1, 0);  // vsync
        queue_->Signal(fence_.Get(), ++fence_value_);
        frame_fence_values_[frame_] = fence_value_;
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

    // Sends debug-layer warnings and errors to our log (by default they only reach a debugger).
    void forward_debug_messages() {
        ComPtr<ID3D12InfoQueue1> queue;
        if (FAILED(device_.As(&queue))) {
            LSD_LOG_WARN("d3d12", "Debug messages only visible in a debugger (ID3D12InfoQueue1 unavailable).");
            return;
        }
        DWORD cookie = 0;
        queue->RegisterMessageCallback(
            [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID, LPCSTR text, void*) {
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
    };

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
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants.Num32BitValues = kRootConstants;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rs_desc{};
        rs_desc.NumParameters = 1;
        rs_desc.pParameters = &param;
        rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> rs_blob, error;
        if (!check(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &error), "D3D12SerializeRootSignature")) return false;
        if (!check(device_->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&root_signature_)), "CreateRootSignature")) return false;

        static const D3D12_INPUT_ELEMENT_DESC mesh_layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        static const D3D12_INPUT_ELEMENT_DESC line_layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        return create_pipeline(kTriangleHlsl, sizeof kTriangleHlsl, {nullptr, 0}, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, false, triangle_pipeline_) &&
               create_pipeline(kMeshHlsl, sizeof kMeshHlsl, {mesh_layout, 3}, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, true, mesh_pipeline_) &&
               create_pipeline(kLinesHlsl, sizeof kLinesHlsl, {line_layout, 2}, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE, true, line_pipeline_);
    }

    bool create_pipeline(const char* source, usize source_size, D3D12_INPUT_LAYOUT_DESC layout, D3D12_PRIMITIVE_TOPOLOGY_TYPE topology,
                         bool depth_test, ComPtr<ID3D12PipelineState>& out) {
        ComPtr<ID3DBlob> vs, ps;
        if (!compile(source, source_size - 1, "vs_main", "vs_5_0", vs) || !compile(source, source_size - 1, "ps_main", "ps_5_0", ps)) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = root_signature_.Get();
        pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso.InputLayout = layout;
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;  // shortcut: no culling until imported winding is verified
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.DepthStencilState.DepthEnable = depth_test;
        pso.DepthStencilState.DepthWriteMask = depth_test ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pso.DSVFormat = kDepthFormat;
        pso.PrimitiveTopologyType = topology;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = kBackBufferFormat;
        pso.SampleDesc.Count = 1;
        return check(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&out)), "CreateGraphicsPipelineState");
    }

    static bool compile(const char* source, usize length, const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(source, length, "builtin.hlsl", nullptr, nullptr, entry, target, 0, 0, &out, &errors);
        if (FAILED(hr)) {
            LSD_LOG_ERROR("d3d12", "Shader %s failed: %s", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
            return false;
        }
        return true;
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
    ComPtr<IDXGISwapChain3> swapchain_;
    ComPtr<ID3D12Resource> back_buffers_[kBackBufferCount];
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    u32 rtv_size_ = 0;
    u32 frame_ = 0;
    u32 width_ = 0;
    u32 height_ = 0;
    ComPtr<ID3D12DescriptorHeap> dsv_heap_;
    ComPtr<ID3D12Resource> depth_;
    ComPtr<ID3D12RootSignature> root_signature_;
    ComPtr<ID3D12PipelineState> triangle_pipeline_;
    ComPtr<ID3D12PipelineState> mesh_pipeline_;
    ComPtr<ID3D12PipelineState> line_pipeline_;
    Buffer buffers_[kMaxBuffers];
    ComPtr<ID3D12DescriptorHeap> srv_heap_;
    u32 srv_size_ = 0;
    bool srv_used_[kDebugUiDescriptors] = {};
    bool debug_ui_ = false;
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
