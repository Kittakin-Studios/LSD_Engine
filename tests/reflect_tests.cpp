// Reflection tests: generated type tables and text save/load round trips.
#include "reflect_test_types.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); ++g_failures; } } while (0)

using namespace lsd;
using lsd::test::PointLight;

static void test_type_info() {
    const reflect::TypeInfo& t = reflect::type_of<PointLight>();
    CHECK(std::strcmp(t.name, "PointLight") == 0);
    CHECK(t.id == LSD_ID("lsd::test::PointLight"));
    CHECK(t.size == sizeof(PointLight));
    CHECK(t.flags & reflect::kComponent);
    CHECK(std::strcmp(t.category, "Lights") == 0);
    CHECK(t.property_count == 7);  // cached_falloff is not reflected

    const reflect::Property* temp = reflect::find_property(t, "temperature");
    CHECK(temp && temp->type == reflect::FieldType::F32);
    CHECK(temp && temp->offset == offsetof(PointLight, temperature));
    CHECK(temp && temp->has_min && temp->min == 1000.0 && temp->has_max && temp->max == 40000.0);
    CHECK(temp && std::strcmp(temp->units, "kelvin") == 0);

    const reflect::Property* color = reflect::find_property(t, "color");
    CHECK(color && color->type == reflect::FieldType::Struct && color->struct_type == &reflect::type_of<lsd::test::Color>());
    CHECK(reflect::find_property(t, "show_radius_gizmo")->flags & reflect::kEditorOnly);
    CHECK(reflect::find_property(t, "cached_falloff") == nullptr);
}

static void test_round_trip() {
    PointLight a;
    a.intensity = 1234.5f;
    a.temperature = 0.1f;
    a.attenuation_radius = 1.0 / 3.0;
    a.color = {0.25f, 0.5f, 0.75f};
    a.light_id = 4000000000u;
    a.seed = -9000000000000000000ll;
    a.show_radius_gizmo = false;
    a.cached_falloff = 99.0f;

    char text[1024];
    const usize length = reflect::save_text(a, text, sizeof text);
    CHECK(length < sizeof text);
    CHECK(std::strstr(text, "\"cached_falloff\"") == nullptr);

    PointLight b;
    CHECK(reflect::load_text(b, text, length));
    CHECK(b.intensity == a.intensity && b.temperature == a.temperature);
    CHECK(b.attenuation_radius == a.attenuation_radius);
    CHECK(b.color.r == 0.25f && b.color.g == 0.5f && b.color.b == 0.75f);
    CHECK(b.light_id == a.light_id && b.seed == a.seed);
    CHECK(b.show_radius_gizmo == false);
    CHECK(b.cached_falloff == 0.0f);

    char small[8];
    CHECK(reflect::save_text(a, small, sizeof small) == length);  // reports the size it needs
    CHECK(std::strlen(small) == sizeof small - 1);
}

static void test_versioning_and_errors() {
    // Missing keys keep defaults; unknown keys (removed properties) are skipped.
    const char old_file[] = R"({ "intensity": 5, "removed": { "x": [1, 2, "a"] }, "color": { "g": 0 } })";
    PointLight p;
    CHECK(reflect::load_text(p, old_file, sizeof old_file - 1));
    CHECK(p.intensity == 5.0f && p.temperature == 6500.0f);
    CHECK(p.color.r == 1.0f && p.color.g == 0.0f);

    const char* bad[] = {
        R"({ "intensity": "loud" })",
        R"({ "light_id": -1 })",
        R"({ "light_id": 5000000000 })",
        R"({ "intensity": 1 )",
        R"({ "intensity": 1 } trailing)",
    };
    for (const char* text : bad) {
        PointLight q;
        CHECK(!reflect::load_text(q, text, std::strlen(text)));
    }
}

int main() {
    test_type_info();
    test_round_trip();
    test_versioning_and_errors();
    if (g_failures == 0) std::printf("All reflection tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
