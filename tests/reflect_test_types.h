#pragma once
// Annotated types used by reflect_tests.cpp; lsd_reflect_gen generates reflect_test_types.gen.cpp.
#include "lsd/reflect/reflect.h"

namespace lsd::test {

LSD_STRUCT()
struct Color {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) f32 r = 1.0f;
    LSD_PROPERTY(Edit) f32 g = 1.0f;
    LSD_PROPERTY(Edit) f32 b = 1.0f;
};

LSD_STRUCT(Component, Category = "Lights")
struct PointLight {
    LSD_GENERATED()

    LSD_PROPERTY(Edit, Category = "Light", Units = "lumens", Min = 0)
    f32 intensity = 800.0f;

    LSD_PROPERTY(Edit, Category = "Light", Units = "kelvin", Min = 1000, Max = 40000)
    f32 temperature = 6500.0f;

    LSD_PROPERTY(Edit, Category = "Light", Tooltip = "Radius where light reaches zero")
    f64 attenuation_radius = 1000.0;

    LSD_PROPERTY(Edit, Category = "Light")
    Color color;

    LSD_PROPERTY(Visible) u32 light_id = 7;
    LSD_PROPERTY(Save) i64 seed = -42;
    LSD_PROPERTY(Edit, EditorOnly) bool show_radius_gizmo = true;
    LSD_PROPERTY(Edit) char label[16] = "Lamp";

    f32 cached_falloff = 0.0f;  // not reflected, not saved

    LSD_FUNCTION(Callable, Category = "Light")
    void set_intensity(f32 lumens) { intensity = lumens; }
};

// Lists and widget hints.
LSD_STRUCT()
struct Palette {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) List<Color, 4> colors;
    LSD_PROPERTY(Edit, Min = 0) List<f32, 3> weights;
    LSD_PROPERTY(Edit, Widget = "Choice:Add,Multiply,Screen") u32 blend = 1;
};

} // namespace lsd::test
