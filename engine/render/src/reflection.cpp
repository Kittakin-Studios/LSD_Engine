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

// The overhead (within 30 degrees of straight up), horizon (the 15 degrees above it) and below-horizon averages of a
// lat-long image `width` x `height` whose texel(x, y) is its colour, weighted by the area each row covers.
template <class Texel>
void band_averages(u32 width, u32 height, Texel texel, Vec3 averages[3]) {
    Vec3 sum[3] = {};
    f32 weight[3] = {};
    for (u32 y = 0; y < height; ++y) {
        const f32 theta = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(height) * kPi, deg = theta * 180.0f / kPi;
        const int band = deg < 30.0f ? 0 : deg >= 75.0f && deg < 90.0f ? 1 : deg >= 90.0f ? 2 : -1;
        if (band < 0) continue;
        const f32 area = std::sin(theta);  // rows near the poles cover less of the sphere
        for (u32 x = 0; x < width; ++x) sum[band] = sum[band] + texel(x, y) * area;
        weight[band] += area * static_cast<f32>(width);
    }
    for (int b = 0; b < 3; ++b) averages[b] = weight[b] > 0.0f ? sum[b] * (1.0f / weight[b]) : Vec3{};
}

// Earth's atmosphere (the usual published constants), in metres.
constexpr f32 kGround = 6360e3f, kTop = 6460e3f, kEye = 200.0f;
const Vec3 kRayleigh{5.802e-6f, 13.558e-6f, 33.1e-6f};
constexpr f32 kMieScatter = 3.996e-6f, kMieExtinction = 4.40e-6f, kMieG = 0.8f;
const Vec3 kOzone{0.650e-6f, 1.881e-6f, 0.085e-6f};
// What the sky is multiplied by to land where the old fitted colours were (a clear noon zenith of about 0.16 luminance).
constexpr f32 kSkyScale = 11.0f;

Vec3 vexp(Vec3 v) { return {std::exp(v.x), std::exp(v.y), std::exp(v.z)}; }
Vec3 times(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
struct Density { f32 rayleigh, mie, ozone; };
Density density_at(f32 height) {
    return {std::exp(-height / 8000.0f), std::exp(-height / 1200.0f), std::fmax(0.0f, 1.0f - std::fabs(height - 25000.0f) / 15000.0f)};
}
Vec3 extinction(const Density& d) { return kRayleigh * d.rayleigh + Vec3{1, 1, 1} * (kMieExtinction * d.mie) + kOzone * d.ozone; }
// Distance from radius r along cosine mu to the top of the atmosphere; to the ground instead when the ray hits it.
f32 ray_length(f32 r, f32 mu, bool& ground) {
    const f32 below = r * r * (mu * mu - 1.0f) + kGround * kGround;
    ground = mu < 0.0f && below >= 0.0f;
    if (ground) return -r * mu - std::sqrt(below);
    return -r * mu + std::sqrt(std::fmax(r * r * (mu * mu - 1.0f) + kTop * kTop, 0.0f));
}

// Transmittance from a height toward the sun's cosine (0 when the planet is in the way), 32 heights x 64 cosines.
constexpr u32 kTH = 32, kTMu = 64;
const Vec3* transmittance_table() {
    static Vec3 table[kTH * kTMu];
    static bool made = false;
    if (made) return table;
    for (u32 h = 0; h < kTH; ++h) {
        const f32 r = kGround + (kTop - kGround) * static_cast<f32>(h) / (kTH - 1);
        for (u32 m = 0; m < kTMu; ++m) {
            const f32 mu = -1.0f + 2.0f * static_cast<f32>(m) / (kTMu - 1);
            bool ground;
            const f32 span = ray_length(r, mu, ground);
            Vec3 depth{};
            constexpr int kSteps = 40;
            for (int s = 0; s < kSteps; ++s) {
                const f32 t = span * (static_cast<f32>(s) + 0.5f) / kSteps;
                const f32 height = std::sqrt(r * r + t * t + 2.0f * r * mu * t) - kGround;
                depth = depth + extinction(density_at(height)) * (span / kSteps);
            }
            table[h * kTMu + m] = ground ? Vec3{} : vexp(depth * -1.0f);
        }
    }
    made = true;
    return table;
}
Vec3 transmittance(f32 height, f32 mu) {
    const Vec3* table = transmittance_table();
    const f32 fh = std::clamp(height / (kTop - kGround), 0.0f, 1.0f) * (kTH - 1), fm = std::clamp((mu + 1.0f) * 0.5f, 0.0f, 1.0f) * (kTMu - 1);
    const u32 h0 = std::min(static_cast<u32>(fh), kTH - 2), m0 = std::min(static_cast<u32>(fm), kTMu - 2);
    const f32 th = fh - static_cast<f32>(h0), tm = fm - static_cast<f32>(m0);
    auto at = [&](u32 h, u32 m) { return table[h * kTMu + m]; };
    const Vec3 a = at(h0, m0) * (1.0f - tm) + at(h0, m0 + 1) * tm, b = at(h0 + 1, m0) * (1.0f - tm) + at(h0 + 1, m0 + 1) * tm;
    return a * (1.0f - th) + b * th;
}

// The sky's radiance along unit `d` from just above the ground, the sun toward `sun`.
Vec3 sky_radiance(Vec3 d, Vec3 sun) {
    const f32 r = kGround + kEye, mu = d.z;
    bool ground;
    const f32 span = ray_length(r, mu, ground);
    const f32 nu = dot(d, sun);
    const f32 phase_r = 3.0f / (16.0f * kPi) * (1.0f + nu * nu);
    const f32 g2 = kMieG * kMieG;
    const f32 phase_m = 3.0f / (8.0f * kPi) * (1.0f - g2) * (1.0f + nu * nu) / ((2.0f + g2) * std::pow(1.0f + g2 - 2.0f * kMieG * nu, 1.5f));
    constexpr int kSteps = 24;
    const f32 dt = span / kSteps;
    Vec3 light{}, view{1, 1, 1};
    for (int s = 0; s < kSteps; ++s) {
        const f32 t = dt * (static_cast<f32>(s) + 0.5f);
        const Vec3 p{d.x * t, d.y * t, r + d.z * t};
        const f32 pr = length(p);
        const Density den = density_at(pr - kGround);
        const Vec3 step = vexp(extinction(den) * -dt);
        const Vec3 scatter = kRayleigh * (den.rayleigh * phase_r) + Vec3{1, 1, 1} * (kMieScatter * den.mie * phase_m);
        const Vec3 sun_light = transmittance(pr - kGround, dot(p * (1.0f / pr), sun));
        // the light scattered in over this step, integrated against the extinction (Hillaire's energy-conserving form)
        const Vec3 ext = extinction(den);
        const Vec3 gain{ext.x > 0 ? (1.0f - step.x) / ext.x : dt, ext.y > 0 ? (1.0f - step.y) / ext.y : dt, ext.z > 0 ? (1.0f - step.z) / ext.z : dt};
        light = light + times(times(view, times(scatter, sun_light)), gain);
        view = times(view, step);
    }
    if (ground) {  // the ground at the end of the ray (a dark 0.08 albedo, near the old fitted ground), lit by the sun through the air
        const Vec3 p{d.x * span, d.y * span, r + d.z * span};
        const Vec3 up = p * (1.0f / length(p));
        const f32 cos_sun = dot(up, sun);
        light = light + times(view, transmittance(0.0f, cos_sun)) * (0.08f / kPi * std::fmax(cos_sun, 0.0f));
    }
    return light;
}

} // namespace

void atmosphere_to_tile(Vec3 sun, f32 scale, u32 index, u8* atlas, Vec3 averages[3]) {
    // The sky-view table: 64 elevations (straight up to straight down) by 33 azimuths from the sun's (0 .. pi; the sky
    // is mirror-symmetric about the sun's vertical plane).
    constexpr u32 kRows = 64, kCols = 33;
    static Vec3 view[kRows * kCols];
    const f32 sun_azimuth = std::atan2(sun.y, sun.x);
    const f32 sun_horizontal = std::sqrt(std::fmax(1.0f - sun.z * sun.z, 0.0f));
    for (u32 y = 0; y < kRows; ++y) {
        const f32 theta = (static_cast<f32>(y) + 0.5f) / kRows * kPi;
        for (u32 x = 0; x < kCols; ++x) {
            const f32 phi = static_cast<f32>(x) / (kCols - 1) * kPi;  // from the sun's azimuth
            const Vec3 d{std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta)};
            const Vec3 s{sun_horizontal, 0.0f, sun.z};  // the sun at azimuth 0 in this frame
            // a faint night sky (airglow, starlight) so the dark is not black
            const Vec3 night = d.z >= 0.0f ? Vec3{0.002f, 0.004f, 0.014f} * d.z + Vec3{0.01f, 0.014f, 0.03f} * (1.0f - d.z) : Vec3{0.003f, 0.003f, 0.004f};
            view[y * kCols + x] = sky_radiance(d, s) * kSkyScale + night;
        }
    }
    auto sample = [&](f32 theta, f32 phi) {  // the table at a direction (theta from straight up, phi azimuth)
        f32 rel = std::fabs(std::remainder(phi - sun_azimuth, 2.0f * kPi));
        const f32 fy = std::clamp(theta / kPi * kRows - 0.5f, 0.0f, kRows - 1.0f), fx = rel / kPi * (kCols - 1);
        const u32 y0 = std::min(static_cast<u32>(fy), kRows - 2), x0 = std::min(static_cast<u32>(fx), kCols - 2);
        const f32 ty = fy - static_cast<f32>(y0), tx = fx - static_cast<f32>(x0);
        auto at = [&](u32 yy, u32 xx) { return view[yy * kCols + xx]; };
        const Vec3 a = at(y0, x0) * (1.0f - tx) + at(y0, x0 + 1) * tx, b = at(y0 + 1, x0) * (1.0f - tx) + at(y0 + 1, x0 + 1) * tx;
        return a * (1.0f - ty) + b * ty;
    };
    auto tile_texel = [&](u32 x, u32 y) {
        return sample((static_cast<f32>(y) + 0.5f) / kReflectionTileH * kPi, ((static_cast<f32>(x) + 0.5f) / kReflectionTileW * 2.0f - 1.0f) * kPi);
    };
    const u32 col = index % 2, row = index / 2;
    for (u32 y = 0; y < kReflectionTileH; ++y) {
        for (u32 x = 0; x < kReflectionTileW; ++x) {
            const Vec3 c = tile_texel(x, y) * scale;
            u8* out = atlas + ((static_cast<usize>(row) * kReflectionTileH + y) * kReflectionAtlas + col * kReflectionTileW + x) * 4;
            out[0] = encode(c.x);
            out[1] = encode(c.y);
            out[2] = encode(c.z);
            out[3] = 255;
        }
    }
    band_averages(kReflectionTileW, kReflectionTileH, tile_texel, averages);
}

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
    band_averages(width, height, [&](u32 x, u32 y) { return Vec3{texel(x, y, 0), texel(x, y, 1), texel(x, y, 2)}; }, averages);
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
