#pragma once
// Editor floor grid as GPU lines: minor line every unit, major every 5, X and Y axes coloured.
#include "lsd/rhi/rhi.h"

namespace lsd::render {

struct GridColors {
    f32 minor[4];
    f32 major[4];
    f32 axis_x[4];
    f32 axis_y[4];
};

struct GpuLines {
    rhi::BufferHandle vertices;
    u32 vertex_count = 0;
};

// Lines from -half_extent to +half_extent units on both axes, at z = 0.
GpuLines create_grid(rhi::Device& device, Allocator& allocator, const GridColors& colors, i32 half_extent = 50);
void destroy_lines(rhi::Device& device, GpuLines& lines);

} // namespace lsd::render
