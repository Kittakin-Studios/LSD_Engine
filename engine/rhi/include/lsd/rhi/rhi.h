#pragma once
// Render Hardware Interface: the only graphics API the rest of LSD_Engine talks to.
// Backends (Direct3D 12, Vulkan, OpenGL 4.6) implement Device behind this interface.
#include "lsd/foundation/allocator.h"
#include "lsd/foundation/handle.h"
#include "lsd/foundation/types.h"

struct SDL_Window;

namespace lsd::rhi {

enum class Backend : u8 { Auto, D3D12, Vulkan, OpenGL };
enum class Vendor : u8 { Unknown, Nvidia, Amd, Intel, Other };
enum class AdapterType : u8 { Discrete, Integrated, Software };

struct AdapterInfo {
    char name[128] = {};
    Vendor vendor = Vendor::Unknown;
    AdapterType type = AdapterType::Discrete;
    u32 index = 0;
    u64 dedicated_vram_bytes = 0;   // 0 on integrated GPUs
    u64 shared_memory_bytes = 0;    // system RAM the GPU may use
    bool supports_raytracing = false;
    bool supports_mesh_shaders = false;
};

// Live memory budget reported by the driver (D3D12: QueryVideoMemoryInfo, Vulkan: VK_EXT_memory_budget).
struct MemoryBudget {
    u64 budget_bytes = 0;  // what the OS says we may use right now
    u64 usage_bytes = 0;   // what we currently use
};

enum class QualityPreset : u8 { Low, Medium, High, Epic };

// ---- Resources ----

struct BufferTag {};
using BufferHandle = Handle<BufferTag>;
struct TextureTag {};
using TextureHandle = Handle<TextureTag>;
enum class BufferUsage : u8 { Vertex, Index };
// RGBA8 colour data: sRGB for colour textures (base colour, emissive), linear for data (normals, masks).
enum class TextureFormat : u8 { RGBA8, RGBA8_sRGB };
constexpr u32 kMaxMips = 16;

// Vertex formats of the built-in pipelines.
struct MeshVertex {
    f32 position[3];
    f32 normal[3];
    f32 uv[2];
};
struct LineVertex {
    f32 position[3];
    f32 color[4];
};

// One lit mesh draw, shaded physically based (metal / roughness). Indices are u32. Matrices are
// column-major (see lsd/foundation/math.h).
struct MeshDraw {
    BufferHandle vertices;
    BufferHandle indices;
    u32 index_count = 0;
    f32 mvp[16];            // projection * view * world
    f32 normal_matrix[12];  // inverse transpose of the world 3x3, as three float4 columns (w unused)
    f32 world[12];          // world matrix rows 0..2 (3x4), for world positions
    f32 camera_position[3] = {};
    f32 color[4];           // base colour, linear (multiplies the base colour texture)
    f32 metallic = 0.0f;
    f32 roughness = 0.5f;
    f32 emissive[3] = {};   // linear, added on top (already times its strength)
    // Invalid handles use the defaults: white base colour, flat normal, white occlusion /
    // roughness / metallic (glTF packing: R occlusion, G roughness, B metallic; G and B multiply the values above).
    TextureHandle base_color_texture;
    TextureHandle normal_texture;
    TextureHandle orm_texture;
    bool unlit = false;      // colour only, no lighting (Unlit view mode)
    bool wireframe = false;  // triangle edges only (Wireframe view mode)
};

// Lights for the lit-mesh pipeline, in physical units already converted to radiance by the caller:
// colour = linear colour x intensity (lux for directional; candela, i.e. lumens / 4 pi, for point
// and spot). The shader multiplies everything by `exposure` and encodes to sRGB.
enum class LightType : u32 { Directional = 0, Point = 1, Spot = 2 };
constexpr u32 kMaxLights = 16;
struct GpuLight {
    f32 position[3];
    f32 type;          // LightType as a float (the shader reads it from a float4)
    f32 direction[3];  // where the light travels (directional, spot)
    f32 range;         // point / spot: no light beyond this (smooth falloff)
    f32 color[3];
    f32 spot_inner_cos;
    f32 spot_outer_cos;
    f32 pad[3];
};
struct LightSetup {
    f32 exposure = 1.0f;
    u32 count = 0;
    f32 pad0[2] = {};
    f32 sky[4] = {0.30f, 0.36f, 0.45f, 0.0f};    // ambient from above (radiance, before exposure)
    f32 ground[4] = {0.16f, 0.15f, 0.14f, 0.0f}; // ambient from below
    GpuLight lights[kMaxLights] = {};
    // Sun shadows: cascades (view-projection of each, 0..1 depth) and where each sits in the shadow
    // map atlas (x, y offset and size as fractions, top-left origin). info: cascade count, index of
    // the shadowed light (-1 = none), texel size (1 / atlas size), normal offset in metres.
    f32 shadow_matrices[4][16] = {};
    f32 shadow_tiles[4][4] = {};
    f32 shadow_splits[4] = {};  // far distance of each cascade from the camera
    f32 shadow_info[4] = {0, -1, 0, 0};
};
static_assert(sizeof(GpuLight) == 64 && sizeof(LightSetup) == 48 + kMaxLights * 64 + 256 + 64 + 16 + 16, "matches the shader's light buffer");

// The look before any light is placed (and of thumbnails): one sun, sky and ground ambient.
LightSetup default_lights();

struct DeviceDesc {
    Backend backend = Backend::Auto;
    i32 adapter_override = -1;  // -1 = pick automatically (-gpu=<index> on the command line)
    bool enable_validation = false;
};

class Device {
public:
    virtual ~Device() = default;
    virtual Backend backend() const = 0;
    virtual const AdapterInfo& adapter() const = 0;
    virtual MemoryBudget query_memory_budget() const = 0;

    // Swapchain for an SDL window, created with the flags the backend needs (SDL_WINDOW_OPENGL /
    // SDL_WINDOW_VULKAN). One per device for now. The OpenGL backend creates its context here.
    virtual bool create_swapchain(SDL_Window* window, u32 width, u32 height) = 0;
    virtual void resize_swapchain(u32 width, u32 height) = 0;

    // shortcut: built-in pipelines (lit mesh, lines, test triangle) instead of user pipelines and
    // command lists; replace when the material graph needs custom shaders (milestone 5).

    // Immutable GPU buffer filled with data at creation.
    virtual BufferHandle create_buffer(BufferUsage usage, const void* data, u64 size) = 0;
    virtual void destroy_buffer(BufferHandle buffer) = 0;  // safe while the GPU may still use it

    // Sampled texture with its mip chain (mips[0] full size, each next one half, rows tightly packed).
    // Needs init_debug_ui() first on D3D12 (its view lives in the UI's descriptor heap).
    virtual TextureHandle create_texture(u32 width, u32 height, u32 mip_count, TextureFormat format, const void* const* mips) = 0;

    // Offscreen colour target (RGBA8, same format as the window) with its own depth buffer.
    virtual TextureHandle create_render_target(u32 width, u32 height) = 0;
    virtual void destroy_texture(TextureHandle texture) = 0;  // safe while the GPU may still use it

    virtual void begin_frame(const f32 clear_color[4]) = 0;  // clears colour and depth
    // Following draws go to `target`, cleared to clear_color, with the viewport covering it; an
    // invalid handle switches back to the window (not cleared again). Call between begin/end_frame.
    virtual void set_render_target(TextureHandle target, const f32 clear_color[4]) = 0;
    // Pixel rectangle that following draws render into (default: the whole window each frame).
    virtual void set_viewport(f32 x, f32 y, f32 width, f32 height) = 0;
    // Lights for the following draw_mesh calls (until the next set_lights; begin_frame resets to
    // default_lights()). Call between begin_frame and end_frame.
    virtual void set_lights(const LightSetup& lights) = 0;

    // Shadow maps: a square depth texture. Between begin_shadow_pass and end_shadow_pass, draws go
    // into it (set_viewport picks a tile of an atlas) with only their depth; end_shadow_pass returns
    // to the window. set_shadow_map binds one for the following draw_mesh calls (lights say which
    // light uses it); begin_frame unbinds it.
    virtual TextureHandle create_shadow_map(u32 size) = 0;
    virtual void begin_shadow_pass(TextureHandle shadow_map) = 0;
    virtual void draw_shadow_caster(BufferHandle vertices, BufferHandle indices, u32 index_count, const f32 light_mvp[16]) = 0;
    virtual void end_shadow_pass() = 0;
    virtual void set_shadow_map(TextureHandle shadow_map) = 0;
    virtual void draw_mesh(const MeshDraw& draw) = 0;
    // Line list (2 vertices per line), depth-tested, unlit.
    virtual void draw_lines(BufferHandle vertices, u32 vertex_count, const f32 mvp[16]) = 0;
    virtual void draw_test_triangle(f32 scale, f32 rotation_radians) = 0;
    virtual void end_frame() = 0;  // presents

    // Saves the current frame (call before end_frame) as a 24-bit BMP. Debug aid; OpenGL only for now.
    virtual bool capture_frame(const char* bmp_path) { (void)bmp_path; return false; }

    // Debug UI (Dear ImGui): each backend owns its ImGui renderer. Create the ImGui context and
    // platform backend first, call init_debug_ui() after create_swapchain(), then each frame call
    // debug_ui_new_frame() before ImGui::NewFrame() and render_debug_ui() between begin/end_frame.
    virtual bool init_debug_ui() = 0;
    virtual void debug_ui_new_frame() = 0;
    virtual void render_debug_ui() = 0;
    // ImTextureID for showing a render target or texture with ImGui::Image (0 if unsupported).
    virtual u64 debug_ui_texture(TextureHandle texture) = 0;
    // True when render targets come out upside down in ImGui (OpenGL): flip the UVs.
    virtual bool render_targets_flipped() const { return false; }
};

// Creates a device for desc.backend (Auto = platform default), selecting the GPU with
// select_adapter(). Returns nullptr and logs why if the backend is unavailable.
Device* create_device(const DeviceDesc& desc, Allocator& allocator);
void destroy_device(Device* device, Allocator& allocator);

// Scores an adapter for automatic selection: discrete beats integrated, then VRAM and features.
u64 score_adapter(const AdapterInfo& adapter);

// Returns the index into `adapters` of the best adapter, honouring desc.adapter_override.
i32 select_adapter(const AdapterInfo* adapters, u32 count, const DeviceDesc& desc);

// Picks a starting quality preset from the chosen adapter and system RAM.
QualityPreset recommend_preset(const AdapterInfo& adapter, u64 system_ram_bytes);

const char* to_string(Backend backend);
const char* to_string(QualityPreset preset);

} // namespace lsd::rhi
