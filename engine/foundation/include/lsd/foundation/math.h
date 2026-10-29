#pragma once
// Small vector/matrix library. Conventions (same everywhere, shaders included):
// - World is Z-up and right-handed; 1 unit = 1 metre.
// - Mat4 is column-major, column vectors: v' = M * v; element (row r, col c) = m[c * 4 + r].
// - Clip depth is 0..1 (D3D12/Vulkan; the OpenGL backend switches GL to the same range).
#include "lsd/foundation/types.h"

#include <cmath>

namespace lsd {

struct Vec3 {
    f32 x = 0, y = 0, z = 0;
};

constexpr Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr Vec3 operator*(Vec3 a, f32 s) { return {a.x * s, a.y * s, a.z * s}; }
constexpr f32 dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline f32 length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(Vec3 a) { return a * (1.0f / length(a)); }

struct Mat4 {
    f32 m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // identity

    constexpr f32& at(int row, int col) { return m[col * 4 + row]; }
    constexpr f32 at(int row, int col) const { return m[col * 4 + row]; }
};

constexpr Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            f32 sum = 0;
            for (int k = 0; k < 4; ++k) sum += a.at(row, k) * b.at(k, c);
            r.at(row, c) = sum;
        }
    }
    return r;
}

constexpr Vec3 transform_point(const Mat4& a, Vec3 p) {
    return {a.at(0, 0) * p.x + a.at(0, 1) * p.y + a.at(0, 2) * p.z + a.at(0, 3),
            a.at(1, 0) * p.x + a.at(1, 1) * p.y + a.at(1, 2) * p.z + a.at(1, 3),
            a.at(2, 0) * p.x + a.at(2, 1) * p.y + a.at(2, 2) * p.z + a.at(2, 3)};
}

constexpr Mat4 translation(Vec3 t) {
    Mat4 r;
    r.at(0, 3) = t.x;
    r.at(1, 3) = t.y;
    r.at(2, 3) = t.z;
    return r;
}

constexpr Mat4 scaling(Vec3 s) {
    Mat4 r;
    r.at(0, 0) = s.x;
    r.at(1, 1) = s.y;
    r.at(2, 2) = s.z;
    return r;
}

// Rotation about +Z (yaw), counter-clockwise seen from above.
inline Mat4 rotation_z(f32 radians) {
    Mat4 r;
    const f32 c = std::cos(radians), s = std::sin(radians);
    r.at(0, 0) = c;
    r.at(0, 1) = -s;
    r.at(1, 0) = s;
    r.at(1, 1) = c;
    return r;
}

// View matrix for a camera at eye looking at target (right-handed).
inline Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up) {
    const Vec3 f = normalize(target - eye);
    const Vec3 s = normalize(cross(f, up));
    const Vec3 u = cross(s, f);
    Mat4 r;
    r.at(0, 0) = s.x;  r.at(0, 1) = s.y;  r.at(0, 2) = s.z;  r.at(0, 3) = -dot(s, eye);
    r.at(1, 0) = u.x;  r.at(1, 1) = u.y;  r.at(1, 2) = u.z;  r.at(1, 3) = -dot(u, eye);
    r.at(2, 0) = -f.x; r.at(2, 1) = -f.y; r.at(2, 2) = -f.z; r.at(2, 3) = dot(f, eye);
    return r;
}

// Right-handed perspective projection with 0..1 depth.
inline Mat4 perspective(f32 fov_y_radians, f32 aspect, f32 near_z, f32 far_z) {
    const f32 h = 1.0f / std::tan(fov_y_radians * 0.5f);
    Mat4 r;
    r.at(0, 0) = h / aspect;
    r.at(1, 1) = h;
    r.at(2, 2) = far_z / (near_z - far_z);
    r.at(2, 3) = near_z * far_z / (near_z - far_z);
    r.at(3, 2) = -1.0f;
    r.at(3, 3) = 0.0f;
    return r;
}

} // namespace lsd
