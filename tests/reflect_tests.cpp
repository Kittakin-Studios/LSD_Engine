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
    CHECK(t.property_count == 8);  // cached_falloff is not reflected

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
    std::strcpy(a.label, "Desk \"A\"\1");

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
    CHECK(std::strcmp(b.label, a.label) == 0);  // quotes and backslash survive escaping

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

static void test_strings_and_binary() {
    // A string longer than the field is truncated and stays terminated.
    const char long_label[] = R"({ "label": "ABCDEFGHIJKLMNOPQRSTUVWXYZ" })";
    PointLight p;
    CHECK(reflect::load_text(p, long_label, sizeof long_label - 1));
    CHECK(std::strlen(p.label) == sizeof p.label - 1);

    PointLight a;
    a.intensity = 42.0f;
    a.color.g = 0.125f;
    a.seed = -7;
    std::strcpy(a.label, "Bin");
    lsd::u8 data[512];
    const lsd::usize size = reflect::save_binary(a, data, sizeof data);
    CHECK(size > 0 && size <= sizeof data);
    PointLight b;
    CHECK(reflect::load_binary(reflect::type_of<PointLight>(), &b, data, size));
    CHECK(b.intensity == 42.0f && b.color.g == 0.125f && b.seed == -7 && std::strcmp(b.label, "Bin") == 0);
    CHECK(!reflect::load_binary(reflect::type_of<PointLight>(), &b, data, size / 2));  // truncated data is rejected
}

static void test_lists() {
    const reflect::Property* colors = reflect::find_property(reflect::type_of<test::Palette>(), "colors");
    CHECK(colors && colors->type == reflect::FieldType::Array && colors->element_type == reflect::FieldType::Struct && colors->capacity == 4);
    CHECK(colors && colors->element_size == sizeof(test::Color) && colors->struct_type == &reflect::type_of<test::Color>());
    const reflect::Property* blend = reflect::find_property(reflect::type_of<test::Palette>(), "blend");
    CHECK(blend && std::strcmp(blend->widget, "Choice:Add,Multiply,Screen") == 0);

    test::Palette a;
    CHECK(colors && colors->items_offset == static_cast<u32>(reinterpret_cast<const char*>(&a.colors.items) - reinterpret_cast<const char*>(&a.colors)));
    const reflect::Property* weights = reflect::find_property(reflect::type_of<test::Palette>(), "weights");
    CHECK(weights && weights->items_offset == static_cast<u32>(reinterpret_cast<const char*>(&a.weights.items) - reinterpret_cast<const char*>(&a.weights)));
    test::Color red;
    red.g = red.b = 0.0f;
    a.colors.push_back(red);
    a.colors.push_back(test::Color{});
    a.weights.push_back(0.25f);
    a.blend = 2;
    char text[1024];
    reflect::save_text(a, text, sizeof text);
    test::Palette b;
    CHECK(reflect::load_text(b, text, std::strlen(text)));
    CHECK(b.colors.count == 2 && b.colors[0].g == 0.0f && b.colors[1].g == 1.0f && b.weights.count == 1 && b.weights[0] == 0.25f && b.blend == 2);

    // Binary round trip.
    u8 blob[512];
    const usize size = reflect::save_binary(a, blob, sizeof blob);
    test::Palette c;
    CHECK(size <= sizeof blob && reflect::load_binary(reflect::type_of<test::Palette>(), &c, blob, size));
    CHECK(c.colors.count == 2 && c.colors[0].r == 1.0f && c.colors[0].g == 0.0f && c.weights.count == 1 && c.weights[0] == 0.25f);

    // More items than the capacity: the extra ones are dropped, the rest load.
    const char overflow[] = "{\"weights\": [1, 2, 3, 4, 5], \"colors\": []}";
    test::Palette d;
    d.colors.push_back(red);
    CHECK(reflect::load_text(d, overflow, sizeof overflow - 1));
    CHECK(d.weights.count == 3 && d.weights[2] == 3.0f && d.colors.count == 0);
    const char bad[] = "{\"weights\": [1, \"x\"]}";
    CHECK(!reflect::load_text(d, bad, sizeof bad - 1));

    // Remove keeps order.
    test::Palette e = a;
    e.colors.push_back(red);
    e.colors.remove(0);
    CHECK(e.colors.count == 2 && e.colors[0].g == 1.0f && e.colors[1].g == 0.0f);
}

int main() {
    test_lists();
    test_type_info();
    test_round_trip();
    test_versioning_and_errors();
    test_strings_and_binary();
    if (g_failures == 0) std::printf("All reflection tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
