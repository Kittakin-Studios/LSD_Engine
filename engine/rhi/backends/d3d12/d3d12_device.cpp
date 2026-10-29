// Direct3D 12 backend: adapter enumeration, live memory budget, swapchain and a test triangle.
#include "lsd/rhi/rhi.h"
#include "lsd/foundation/log.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <new>

using Microsoft::WRL::ComPtr;

namespace lsd::rhi {

namespace {

constexpr u32 kBackBufferCount = 2;
constexpr u32 kMaxAdapters = 16;
constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

// shortcut: runtime HLSL compile via d3dcompiler_47 (ships with Windows); move to DXC + the data
// compiler once shaders become assets.
constexpr char kTriangleHlsl[] = R"(
struct VsOut { float4 pos : SV_Position; float3 color : COLOR; };
VsOut vs_main(uint id : SV_VertexID) {
    const float2 pos[3]   = { float2(0.0, 0.6), float2(0.55, -0.5), float2(-0.55, -0.5) };
    const float3 color[3] = { float3(1, 0.2, 0.2), float3(0.2, 1, 0.2), float3(0.2, 0.4, 1) };
    VsOut o;
    o.pos = float4(pos[id], 0, 1);
    o.color = color[id];
    return o;
}
float4 ps_main(VsOut i) : SV_Target { return float4(i.color, 1); }
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

        return create_triangle_pipeline();
    }

    Backend backend() const override { return Backend::D3D12; }
    const AdapterInfo& adapter() const override { return info_; }

    MemoryBudget query_memory_budget() const override {
        DXGI_QUERY_VIDEO_MEMORY_INFO mem{};
        adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mem);
        return MemoryBudget{mem.Budget, mem.CurrentUsage};
    }

    bool create_swapchain(void* native_window, u32 width, u32 height) override {
        DXGI_SWAP_CHAIN_DESC1 sc{};
        sc.Width = width;
        sc.Height = height;
        sc.Format = kBackBufferFormat;
        sc.SampleDesc.Count = 1;
        sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc.BufferCount = kBackBufferCount;
        sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        const HWND hwnd = static_cast<HWND>(native_window);
        ComPtr<IDXGISwapChain1> swapchain1;
        if (!check(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sc, nullptr, nullptr, &swapchain1), "CreateSwapChainForHwnd")) return false;
        factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        if (!check(swapchain1.As(&swapchain_), "IDXGISwapChain3 query")) return false;
        width_ = width;
        height_ = height;
        create_back_buffer_views();
        return true;
    }

    void resize_swapchain(u32 width, u32 height) override {
        if (!swapchain_ || width == 0 || height == 0 || (width == width_ && height == height_)) return;
        wait_for_gpu();
        for (auto& buffer : back_buffers_) buffer.Reset();
        if (!check(swapchain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers")) return;
        width_ = width;
        height_ = height;
        create_back_buffer_views();
    }

    void begin_frame(const f32 clear_color[4]) override {
        frame_ = swapchain_->GetCurrentBackBufferIndex();
        wait_for_fence(frame_fence_values_[frame_]);
        allocators_[frame_]->Reset();
        list_->Reset(allocators_[frame_].Get(), nullptr);

        transition(back_buffers_[frame_].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(frame_);
        list_->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
        list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<f32>(width_), static_cast<f32>(height_), 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        list_->RSSetViewports(1, &viewport);
        list_->RSSetScissorRects(1, &scissor);
    }

    void draw_test_triangle() override {
        list_->SetGraphicsRootSignature(root_signature_.Get());
        list_->SetPipelineState(pipeline_.Get());
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list_->DrawInstanced(3, 1, 0, 0);
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

    bool create_triangle_pipeline() {
        D3D12_ROOT_SIGNATURE_DESC rs_desc{};
        ComPtr<ID3DBlob> rs_blob, error;
        if (!check(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &error), "D3D12SerializeRootSignature")) return false;
        if (!check(device_->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&root_signature_)), "CreateRootSignature")) return false;

        ComPtr<ID3DBlob> vs, ps;
        if (!compile("vs_main", "vs_5_0", vs) || !compile("ps_main", "ps_5_0", ps)) return false;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = root_signature_.Get();
        pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = kBackBufferFormat;
        pso.SampleDesc.Count = 1;
        return check(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline_)), "CreateGraphicsPipelineState");
    }

    static bool compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(kTriangleHlsl, sizeof kTriangleHlsl - 1, "triangle.hlsl", nullptr, nullptr, entry, target, 0, 0, &out, &errors);
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
    ComPtr<ID3D12RootSignature> root_signature_;
    ComPtr<ID3D12PipelineState> pipeline_;
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
