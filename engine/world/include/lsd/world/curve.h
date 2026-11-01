#pragma once
// Curves (Unreal's Curve Float / Curve Vector / Curve Linear Color): values over time from keys, data
// assets edited with a graph in Details. Use them for fades, damage falloff, difficulty over time,
// door easing, colour ramps, paths.
#include "lsd/reflect/reflect.h"

namespace lsd::world {

LSD_STRUCT()
struct CurveKey {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "s") f32 time = 0.0f;
    LSD_PROPERTY(Edit) f32 value = 0.0f;
    LSD_PROPERTY(Edit, Widget = "Choice:Smooth,Linear,Constant", Tooltip = "How the curve goes from this key to the next") u32 interp = 0;
    LSD_PROPERTY(Edit, Widget = "Choice:Auto,User", Tooltip = "Smooth keys: Auto takes the slope between the neighbours; User keeps Tangent (drag the key's handle)")
    u32 tangent_mode = 0;
    LSD_PROPERTY(Edit, Tooltip = "Slope through the key (value per second) when the tangent is User") f32 tangent = 0.0f;
};

using CurveKeys = List<CurveKey, 64>;  // reflected fields spell the type out: the generator does not read aliases

LSD_STRUCT(Category = "Data")
struct Curve {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Kept in time order") List<CurveKey, 64> keys = {2, {CurveKey{0.0f, 0.0f, 0}, CurveKey{1.0f, 1.0f, 0}}};
};

// Three curves read together as a vector (positions, scales, wind).
LSD_STRUCT(Category = "Data")
struct CurveVector {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) List<CurveKey, 64> x = {2, {CurveKey{0.0f, 0.0f, 0}, CurveKey{1.0f, 1.0f, 0}}};
    LSD_PROPERTY(Edit) List<CurveKey, 64> y = {2, {CurveKey{0.0f, 0.0f, 0}, CurveKey{1.0f, 1.0f, 0}}};
    LSD_PROPERTY(Edit) List<CurveKey, 64> z = {2, {CurveKey{0.0f, 0.0f, 0}, CurveKey{1.0f, 1.0f, 0}}};
};

// Four curves read together as a colour (fades, day / night ramps, damage flashes).
LSD_STRUCT(Category = "Data")
struct CurveColor {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) List<CurveKey, 64> r = {2, {CurveKey{0.0f, 0.0f, 0}, CurveKey{1.0f, 1.0f, 0}}};
    LSD_PROPERTY(Edit) List<CurveKey, 64> g = {2, {CurveKey{0.0f, 0.0f, 0}, CurveKey{1.0f, 1.0f, 0}}};
    LSD_PROPERTY(Edit) List<CurveKey, 64> b = {2, {CurveKey{0.0f, 0.0f, 0}, CurveKey{1.0f, 1.0f, 0}}};
    LSD_PROPERTY(Edit) List<CurveKey, 64> a = {2, {CurveKey{0.0f, 1.0f, 0}, CurveKey{1.0f, 1.0f, 0}}};
};

// Value at `time`: flat before the first key and after the last. Smooth keys use automatic
// tangents (the slope between their neighbours, so the curve never overshoots at its ends) unless
// their tangent is User.
f32 evaluate(const CurveKeys& keys, f32 time);
void sort_keys(CurveKeys& keys);  // by time, after editing
inline f32 evaluate(const Curve& curve, f32 time) { return evaluate(curve.keys, time); }
inline void sort_keys(Curve& curve) { sort_keys(curve.keys); }

} // namespace lsd::world