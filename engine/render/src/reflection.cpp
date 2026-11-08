#include "lsd/render/reflection.h"

#include <algorithm>
#include <cmath>

namespace lsd::render {

namespace {

constexpr f32 kPi = 3.14159265358979f;

f32 decode(u8 e) {
    const f32 x = std::fmin(static_cast<f32>(e) / 255.0f, 0.999f), s = x * x;
    return s / (1.0f - s);
}
u8 encode(f32 c) {
    const f32 x = std::sqrt(c / (1.0f + c));
    return static_cast<u8>(std::lround(std::fmin(std::fmax(x, 0.0f), 1.0f) * 255.0f));
}

f32 from_half(u16 h) {
    const u32 exponent = (h >> 10) & 0x1Fu, mantissa = h & 0x3FFu;
    const f32 m = static_cast<f32>(mantissa) / 1024.0f;
    const f32 v = exponent == 0 ? std::ldexp(m, -14) : exponent == 31 ? 65504.0f : std::ldexp(1.0f + m, static_cast<int>(exponent) - 15);
    return (h & 0x8000u) ? -v : v;
}

} // namespace

void hdri_to_tile(const u16* half_rgba, u32 width, u32 height, f32 scale, u32 index, u8* atlas, Vec3 averages[3]) {
    auto texel = [&](u32 x, u32 y, int c) { return std::fmax(from_half(half_rgba[(static_cast<usize>(y) * width + x) * 4 + static_cast<usize>(c)]), 0.0f); };
    const u32 col = index % 2, row = index / 2;
    for (u32 y = 0; y < kReflectionTileH; ++y) {
        const f32 fy = std::clamp((static_cast<f32>(y) + 0.5f) / kReflectionTileH * static_cast<f32>(height) - 0.5f, 0.0f, static_cast<f32>(height - 1));
        const u32 y0 = static_cast<u32>(fy), y1 = std::min(y0 + 1, height - 1);
        for (u32 x = 0; x < kReflectionTileW; ++x) {
            const f32 fx = (static_cast<f32>(x) + 0.5f) / kReflectionTileW * static_cast<f32>(width) - 0.5f;
            const u32 x0 = fx < 0.0f ? width - 1 : static_cast<u32>(fx) % width, x1 = (x0 + 1) % width;  // wraps around
            const f32 tx = fx - std::floor(fx), ty = fy - static_cast<f32>(y0);
            u8* out = atlas + ((static_cast<usize>(row) * kReflectionTileH + y) * kReflectionAtlas + col * kReflectionTileW + x) * 4;
            for (int c = 0; c < 3; ++c) {
                const f32 top = texel(x0, y0, c) + (texel(x1, y0, c) - texel(x0, y0, c)) * tx;
                const f32 bottom = texel(x0, y1, c) + (texel(x1, y1, c) - texel(x0, y1, c)) * tx;
                out[c] = encode((top + (bottom - top) * ty) * scale);
            }
            out[3] = 255;
        }
    }
    Vec3 sum[3] = {};
    f32 weight[3] = {};
    for (u32 y = 0; y < height; ++y) {
        const f32 theta = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(height) * kPi, deg = theta * 180.0f / kPi;
        const int band = deg < 30.0f ? 0 : deg >= 75.0f && deg < 90.0f ? 1 : deg >= 90.0f ? 2 : -1;
        if (band < 0) continue;
        const f32 area = std::sin(theta);  // rows near the poles cover less of the sphere
        for (u32 x = 0; x < width; ++x) sum[band] = sum[band] + Vec3{texel(x, y, 0), texel(x, y, 1), texel(x, y, 2)} * area;
        weight[band] += area * static_cast<f32>(width);
    }
    for (int b = 0; b < 3; ++b) averages[b] = weight[b] > 0.0f ? sum[b] * (1.0f / weight[b]) : Vec3{};
}

void reflection_views(Vec3 eye, Mat4 out[6]) {
    const Vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const Vec3 ups[6] = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}};  // never along the view
    const Mat4 proj = perspective(kPi * 0.5f, 1.0f, 0.05f, 1000.0f);
    for (int f = 0; f < 6; ++f) out[f] = proj * look_at(eye, eye + dirs[f], ups[f]);
}

void unwrap_reflection(const u8* const faces[6], const Mat4 views[6], u32 index, u8* atlas) {
    const u32 col = index % 2, row = index / 2;
    for (u32 y = 0; y < kReflectionTileH; ++y) {
        const f32 theta = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kReflectionTileH) * kPi;
        for (u32 x = 0; x < kReflectionTileW; ++x) {
            const f32 phi = ((static_cast<f32>(x) + 0.5f) / static_cast<f32>(kReflectionTileW) * 2.0f - 1.0f) * kPi;
            const Vec3 d{std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta)};
            u8* out = atlas + ((static_cast<usize>(row) * kReflectionTileH + y) * kReflectionAtlas + col * kReflectionTileW + x) * 4;
            for (int f = 0; f < 6; ++f) {  // the face this direction goes through (its view sees it inside the square)
                const Mat4& m = views[f];
                const f32 w = m.at(3, 0) * d.x + m.at(3, 1) * d.y + m.at(3, 2) * d.z + m.at(3, 3);
                if (w <= 1e-6f) continue;
                const f32 nx = (m.at(0, 0) * d.x + m.at(0, 1) * d.y + m.at(0, 2) * d.z + m.at(0, 3)) / w;
                const f32 ny = (m.at(1, 0) * d.x + m.at(1, 1) * d.y + m.at(1, 2) * d.z + m.at(1, 3)) / w;
                if (std::fabs(nx) > 1.0f || std::fabs(ny) > 1.0f) continue;
                // Bilinear between the face's four nearest texels (nearest stepped along hard edges: the horizon).
                const f32 fx = std::clamp((nx * 0.5f + 0.5f) * kReflectionFace - 0.5f, 0.0f, kReflectionFace - 1.0f);
                const f32 fy = std::clamp((0.5f - ny * 0.5f) * kReflectionFace - 0.5f, 0.0f, kReflectionFace - 1.0f);
                const u32 x0 = static_cast<u32>(fx), y0 = static_cast<u32>(fy);
                const u32 x1 = std::min(x0 + 1, kReflectionFace - 1), y1 = std::min(y0 + 1, kReflectionFace - 1);
                const f32 tx = fx - static_cast<f32>(x0), ty = fy - static_cast<f32>(y0);
                auto texel = [&](u32 px, u32 py, int c) { return static_cast<f32>(faces[f][(static_cast<usize>(py) * kReflectionFace + px) * 4 + static_cast<usize>(c)]); };
                for (int c = 0; c < 4; ++c) {
                    const f32 top = texel(x0, y0, c) + (texel(x1, y0, c) - texel(x0, y0, c)) * tx;
                    const f32 bottom = texel(x0, y1, c) + (texel(x1, y1, c) - texel(x0, y1, c)) * tx;
                    out[c] = static_cast<u8>(std::lround(top + (bottom - top) * ty));
                }
                break;
            }
        }
    }
}

void reflection_mips(const u8* atlas, std::vector<std::vector<u8>>& levels) {
    levels.clear();
    levels.reserve(kReflectionMips);  // `src` points into the last level: no reallocation
    const u8* src = atlas;
    u32 size = kReflectionAtlas;
    for (u32 level = 1; level < kReflectionMips; ++level) {
        const u32 half = size / 2;
        std::vector<u8> next(static_cast<usize>(half) * half * 4);
        for (u32 y = 0; y < half; ++y) {
            for (u32 x = 0; x < half; ++x) {
                for (int c = 0; c < 4; ++c) {
                    f32 sum = 0.0f;
                    for (u32 k = 0; k < 4; ++k) sum += decode(src[((static_cast<usize>(y) * 2 + k / 2) * size + x * 2 + k % 2) * 4 + c]);
                    next[(static_cast<usize>(y) * half + x) * 4 + c] = c == 3 ? 255 : encode(sum * 0.25f);
                }
            }
        }
        levels.push_back(std::move(next));
        src = levels.back().data();
        size = half;
    }
}

} // namespace lsd::render
