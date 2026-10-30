#pragma once
// Reflection annotations, here so foundation types (Vec3) can be reflected too. Documented in
// lsd/reflect/reflect.h; the annotations compile to nothing and only lsd_reflect_gen reads them.
namespace lsd::reflect { struct TypeInfo; }

#define LSD_STRUCT(...)
#define LSD_PROPERTY(...)
#define LSD_FUNCTION(...)
// Inside the struct body, like Unreal's GENERATED_BODY(). Defined in the generated .gen.cpp.
#define LSD_GENERATED() static const ::lsd::reflect::TypeInfo& static_type();
