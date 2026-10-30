#pragma once
// Curves (Unreal's Curve Float): a value over time from keys, a data asset ("Curve") edited with
// a graph in Details. Use them for fades, damage falloff, difficulty over time, door easing...
#include "lsd/reflect/reflect.h"

namespace lsd::world {

LSD_STRUCT()
struct CurveKey {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "s") f32 time = 0.0f;
    LSD_PROPERTY(Edit) f32 value = 0.0f;
    LSD_PROPERTY(Edit, Widget = "Choice:Smooth,Linear,Constant", Tooltip = "How the curve goes from this key to the next") u32 interp = 0;
};

LSD_STRUCT(Category = "Data")
struct Curve {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Kept in time order") List<CurveKey, 64> keys = {2, {CurveKey{0.0f, 0.0f, 0}, CurveKey{1.0f, 1.0f, 0}}};
};

// Value at `time`: flat before the first key and after the last. Smooth keys use automatic
// tangents (the slope between their neighbours), so the curve never overshoots at its ends.
f32 evaluate(const Curve& curve, f32 time);
void sort_keys(Curve& curve);  // by time, after editing

} // namespace lsd::world
