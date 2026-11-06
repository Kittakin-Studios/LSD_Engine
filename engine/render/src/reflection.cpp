#include "lsd/render/reflection.h"

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

} // namespace

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
                const u32 px = static_cast<u32>(std::fmin((nx * 0.5f + 0.5f) * kReflectionFace, kReflectionFace - 1.0f));
                const u32 py = static_cast<u32>(std::fmin((0.5f - ny * 0.5f) * kReflectionFace, kReflectionFace - 1.0f));
                const u8* in = faces[f] + (static_cast<usize>(py) * kReflectionFace + px) * 4;
                for (int c = 0; c < 4; ++c) out[c] = in[c];
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
