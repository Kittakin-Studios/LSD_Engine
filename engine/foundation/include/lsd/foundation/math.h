#pragma once
// Small vector/matrix library. Conventions (same everywhere, shaders included):
// - World is Z-up and right-handed; 1 unit = 1 metre.
// - Mat4 is column-major, column vectors: v' = M * v; element (row r, col c) = m[c * 4 + r].
// - Clip depth is 0..1 (D3D12/Vulkan; the OpenGL backend switches GL to the same range).
#include "lsd/foundation/reflect_macros.h"
#include "lsd/foundation/types.h"

#include <cmath>

namespace lsd {

// Reflected (lsd_reflect generates its table) so components can hold positions and save them.
LSD_STRUCT(Vector)
struct Vec3 {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) f32 x = 0;
    LSD_PROPERTY(Edit) f32 y = 0;
    LSD_PROPERTY(Edit) f32 z = 0;
};

constexpr Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr Vec3 operator*(Vec3 a, f32 s) { return {a.x * s, a.y * s, a.z * s}; }
constexpr Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }
constexpr Vec3 mul(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
constexpr Vec3 vmin(Vec3 a, Vec3 b) { return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z}; }
constexpr Vec3 vmax(Vec3 a, Vec3 b) { return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z}; }
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

// Direction (no translation).
constexpr Vec3 transform_vector(const Mat4& a, Vec3 v) {
    return {a.at(0, 0) * v.x + a.at(0, 1) * v.y + a.at(0, 2) * v.z,
            a.at(1, 0) * v.x + a.at(1, 1) * v.y + a.at(1, 2) * v.z,
            a.at(2, 0) * v.x + a.at(2, 1) * v.y + a.at(2, 2) * v.z};
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
inline Mat4 rotation_z(f32 angle) {
    Mat4 r;
    const f32 c = std::cos(angle), s = std::sin(angle);
    r.at(0, 0) = c;
    r.at(0, 1) = -s;
    r.at(1, 0) = s;
    r.at(1, 1) = c;
    return r;
}

constexpr f32 kPi = 3.14159265358979f;
constexpr f32 radians(f32 deg) { return deg * (kPi / 180.0f); }
constexpr f32 degrees(f32 rad) { return rad * (180.0f / kPi); }

inline Mat4 rotation_x(f32 angle) {
    Mat4 r;
    const f32 c = std::cos(angle), s = std::sin(angle);
    r.at(1, 1) = c;
    r.at(1, 2) = -s;
    r.at(2, 1) = s;
    r.at(2, 2) = c;
    return r;
}

inline Mat4 rotation_y(f32 angle) {
    Mat4 r;
    const f32 c = std::cos(angle), s = std::sin(angle);
    r.at(0, 0) = c;
    r.at(0, 2) = s;
    r.at(2, 0) = -s;
    r.at(2, 2) = c;
    return r;
}

// Euler angles in degrees about X, then Y, then Z (R = Rz * Ry * Rx), the same as Blender's XYZ Euler.
inline Mat4 rotation_euler(Vec3 deg) { return rotation_z(radians(deg.z)) * rotation_y(radians(deg.y)) * rotation_x(radians(deg.x)); }

// Translate * rotate * scale: the world matrix of a transform.
inline Mat4 trs(Vec3 t, Vec3 rotation_degrees, Vec3 s) { return translation(t) * rotation_euler(rotation_degrees) * scaling(s); }

// Inverse of trs(): splits a matrix without shear back into position, Euler degrees and scale.
inline void decompose_trs(const Mat4& m, Vec3& t, Vec3& rotation_degrees, Vec3& s) {
    t = {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    s = {length({m.at(0, 0), m.at(1, 0), m.at(2, 0)}), length({m.at(0, 1), m.at(1, 1), m.at(2, 1)}),
         length({m.at(0, 2), m.at(1, 2), m.at(2, 2)})};
    const f32 r00 = m.at(0, 0) / s.x, r10 = m.at(1, 0) / s.x, r20 = m.at(2, 0) / s.x;
    const f32 r21 = m.at(2, 1) / s.y, r22 = m.at(2, 2) / s.z;
    const f32 sy = -r20 < -1.0f ? -1.0f : -r20 > 1.0f ? 1.0f : -r20;
    rotation_degrees = {degrees(std::atan2(r21, r22)), degrees(std::asin(sy)), degrees(std::atan2(r10, r00))};
    if (std::fabs(sy) > 0.9999f) {  // gimbal lock: put all of the X/Z rotation into Z
        const f32 r01 = m.at(0, 1) / s.y, r11 = m.at(1, 1) / s.y;
        rotation_degrees = {0.0f, degrees(std::asin(sy)), degrees(std::atan2(-r01, r11))};
    }
}

// Inverse of an affine matrix (rotation / scale / shear + translation).
inline Mat4 affine_inverse(const Mat4& m) {
    const f32 a = m.at(0, 0), b = m.at(0, 1), c = m.at(0, 2);
    const f32 d = m.at(1, 0), e = m.at(1, 1), f = m.at(1, 2);
    const f32 g = m.at(2, 0), h = m.at(2, 1), i = m.at(2, 2);
    const f32 A = e * i - f * h, B = f * g - d * i, C = d * h - e * g;
    const f32 det = a * A + b * B + c * C;
    const f32 inv = det != 0.0f ? 1.0f / det : 0.0f;
    Mat4 r;
    r.at(0, 0) = A * inv;                 r.at(0, 1) = (c * h - b * i) * inv;  r.at(0, 2) = (b * f - c * e) * inv;
    r.at(1, 0) = B * inv;                 r.at(1, 1) = (a * i - c * g) * inv;  r.at(1, 2) = (c * d - a * f) * inv;
    r.at(2, 0) = C * inv;                 r.at(2, 1) = (b * g - a * h) * inv;  r.at(2, 2) = (a * e - b * d) * inv;
    const Vec3 t = transform_vector(r, {m.at(0, 3), m.at(1, 3), m.at(2, 3)});
    r.at(0, 3) = -t.x;
    r.at(1, 3) = -t.y;
    r.at(2, 3) = -t.z;
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

// General 4x4 inverse (cofactors); the identity if `a` cannot be inverted.
inline Mat4 inverse(const Mat4& a) {
    const f32* m = a.m;
    f32 inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const f32 det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    Mat4 r;
    if (det == 0.0f) return r;
    for (int i = 0; i < 16; ++i) r.m[i] = inv[i] / det;
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

// Right-handed orthographic projection with 0..1 depth; width and height in world units.
inline Mat4 orthographic(f32 width, f32 height, f32 near_z, f32 far_z) {
    Mat4 r;
    r.at(0, 0) = 2.0f / width;
    r.at(1, 1) = 2.0f / height;
    r.at(2, 2) = 1.0f / (near_z - far_z);
    r.at(2, 3) = near_z / (near_z - far_z);
    return r;
}

} // namespace lsd
