#include "lsd/render/grid.h"

namespace lsd::render {

// 5 mm under z = 0: floors and flat landscapes at 0 hide it cleanly instead of fighting it for the
// same depth (a shimmer of broken lines); the grid still reads as the ground plane.
constexpr f32 kGridZ = -0.005f;

GpuLines create_grid(rhi::Device& device, Allocator& allocator, const GridColors& colors, i32 half_extent) {
    const u32 lines_per_axis = static_cast<u32>(half_extent * 2 + 1);
    const u32 count = lines_per_axis * 2 * 2;  // two directions, two vertices per line
    auto* vertices = static_cast<rhi::LineVertex*>(allocator.allocate(sizeof(rhi::LineVertex) * count, alignof(rhi::LineVertex)));
    const f32 h = static_cast<f32>(half_extent);
    u32 v = 0;
    auto add = [&](f32 x0, f32 y0, f32 x1, f32 y1, const f32 color[4]) {
        vertices[v] = {{x0, y0, kGridZ}, {color[0], color[1], color[2], color[3]}};
        vertices[v + 1] = {{x1, y1, kGridZ}, {color[0], color[1], color[2], color[3]}};
        v += 2;
    };
    for (i32 i = -half_extent; i <= half_extent; ++i) {
        const f32 f = static_cast<f32>(i);
        const f32* along_y = i == 0 ? colors.axis_y : (i % 5 == 0 ? colors.major : colors.minor);  // line x = i runs along Y
        const f32* along_x = i == 0 ? colors.axis_x : (i % 5 == 0 ? colors.major : colors.minor);  // line y = i runs along X
        add(f, -h, f, h, along_y);
        add(-h, f, h, f, along_x);
    }
    GpuLines lines;
    lines.vertices = device.create_buffer(rhi::BufferUsage::Vertex, vertices, sizeof(rhi::LineVertex) * count);
    lines.vertex_count = count;
    allocator.deallocate(vertices);
    return lines;
}

void destroy_lines(rhi::Device& device, GpuLines& lines) {
    device.destroy_buffer(lines.vertices);
    lines = {};
}

} // namespace lsd::render
