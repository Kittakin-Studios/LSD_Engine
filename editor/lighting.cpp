#include "lighting.h"

#include <cmath>

namespace lsd::editor {

namespace {

constexpr f32 kPi = 3.14159265f;

f32 srgb_to_linear(f32 c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

Vec3 light_rgb(const LinearColor& color, bool use_temperature, f32 kelvin) {
    const LinearColor c = use_temperature ? temperature_color(kelvin) : color;
    return {c.r, c.g, c.b};
}

Vec3 forward_of(const Mat4& m) {  // the object's +X axis: where its light points
    const Vec3 x{m.at(0, 0), m.at(1, 0), m.at(2, 0)};
    const f32 len = length(x);
    return len > 0.0f ? x * (1.0f / len) : Vec3{1, 0, 0};
}

} // namespace

LinearColor temperature_color(f32 kelvin) {
    // Tanner Helland's fit of the Planckian locus (sRGB), then to linear and normalised to its brightest channel.
    const f32 t = (kelvin < 1000.0f ? 1000.0f : kelvin > 40000.0f ? 40000.0f : kelvin) / 100.0f;
    f32 r, g, b;
    if (t <= 66.0f) {
        r = 255.0f;
        g = 99.4708025861f * std::log(t) - 161.1195681661f;
        b = t <= 19.0f ? 0.0f : 138.5177312231f * std::log(t - 10.0f) - 305.0447927307f;
    } else {
        r = 329.698727446f * std::pow(t - 60.0f, -0.1332047592f);
        g = 288.1221695283f * std::pow(t - 60.0f, -0.0755148492f);
        b = 255.0f;
    }
    auto channel = [](f32 v) { return srgb_to_linear((v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v) / 255.0f); };
    LinearColor c;
    c.r = channel(r);
    c.g = channel(g);
    c.b = channel(b);
    const f32 top = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
    c.r /= top;
    c.g /= top;
    c.b /= top;
    return c;
}

rhi::LightSetup world_lights(const world::World& w, Vec3 eye, bool skip_hidden) {
    const reflect::TypeInfo& dir_type = reflect::type_of<world::DirectionalLight>();
    const reflect::TypeInfo& point_type = reflect::type_of<world::PointLight>();
    const reflect::TypeInfo& spot_type = reflect::type_of<world::SpotLight>();
    const reflect::TypeInfo& sky_type = reflect::type_of<world::SkyLight>();
    if (!w.count(dir_type) && !w.count(point_type) && !w.count(spot_type) && !w.count(sky_type)) return rhi::default_lights();

    rhi::LightSetup s;
    s.exposure = 1.0f / (1.2f * std::pow(2.0f, w.settings().exposure_ev100));
    // Ambient is given relative to the editor default (1 = as before any light was placed), so it
    // is divided by the exposure the shader multiplies by.
    const rhi::LightSetup defaults = rhi::default_lights();
    Vec3 sky{defaults.sky[0], defaults.sky[1], defaults.sky[2]}, ground{defaults.ground[0], defaults.ground[1], defaults.ground[2]};
    for (u32 i = 0; i < w.count(sky_type); ++i) {
        if (skip_hidden && w.hidden_in_hierarchy(w.owner(sky_type, i))) continue;
        const auto* light = static_cast<const world::SkyLight*>(w.at(sky_type, i));
        sky = Vec3{light->sky_color.r, light->sky_color.g, light->sky_color.b} * light->intensity;
        ground = Vec3{light->ground_color.r, light->ground_color.g, light->ground_color.b} * light->intensity;
        break;
    }
    for (int c = 0; c < 3; ++c) {
        s.sky[c] = (&sky.x)[c] / s.exposure;
        s.ground[c] = (&ground.x)[c] / s.exposure;
    }

    auto put = [&](rhi::LightType type, Vec3 position, Vec3 direction, Vec3 radiance, f32 range, f32 inner_cos, f32 outer_cos) {
        rhi::GpuLight& l = s.lights[s.count++];
        l = rhi::GpuLight{};
        l.type = static_cast<f32>(type);
        for (int c = 0; c < 3; ++c) {
            l.position[c] = (&position.x)[c];
            l.direction[c] = (&direction.x)[c];
            l.color[c] = (&radiance.x)[c];
        }
        l.range = range > 0.01f ? range : 0.01f;
        l.spot_inner_cos = inner_cos;
        l.spot_outer_cos = outer_cos;
    };
    for (u32 i = 0; i < w.count(dir_type) && s.count < rhi::kMaxLights; ++i) {
        const world::Entity e = w.owner(dir_type, i);
        if (skip_hidden && w.hidden_in_hierarchy(e)) continue;
        const auto* light = static_cast<const world::DirectionalLight*>(w.at(dir_type, i));
        put(rhi::LightType::Directional, {}, forward_of(w.world_matrix(e)), light_rgb(light->color, light->use_temperature, light->temperature) * light->intensity, 0, 0, 0);
    }
    // Local lights nearest the camera fill the rest (shortcut: no clustering; 16 per frame is the limit).
    struct Candidate {
        const reflect::TypeInfo* type;
        u32 index;
        f32 distance2;
    };
    Candidate candidates[512];
    u32 n = 0;
    for (const reflect::TypeInfo* type : {&point_type, &spot_type}) {
        for (u32 i = 0; i < w.count(*type) && n < 512; ++i) {
            const world::Entity e = w.owner(*type, i);
            if (skip_hidden && w.hidden_in_hierarchy(e)) continue;
            const Mat4 m = w.world_matrix(e);
            const Vec3 d = Vec3{m.at(0, 3), m.at(1, 3), m.at(2, 3)} - eye;
            candidates[n++] = {type, i, dot(d, d)};
        }
    }
    for (u32 a = 1; a < n; ++a) {  // insertion sort by distance
        const Candidate c = candidates[a];
        u32 b = a;
        while (b > 0 && candidates[b - 1].distance2 > c.distance2) {
            candidates[b] = candidates[b - 1];
            --b;
        }
        candidates[b] = c;
    }
    for (u32 k = 0; k < n && s.count < rhi::kMaxLights; ++k) {
        const world::Entity e = w.owner(*candidates[k].type, candidates[k].index);
        const Mat4 m = w.world_matrix(e);
        const Vec3 position{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
        if (candidates[k].type == &point_type) {
            const auto* light = static_cast<const world::PointLight*>(w.at(point_type, candidates[k].index));
            const f32 candela = light->intensity / (4.0f * kPi);
            put(rhi::LightType::Point, position, {}, light_rgb(light->color, light->use_temperature, light->temperature) * candela, light->attenuation_radius, 0, 0);
        } else {
            const auto* light = static_cast<const world::SpotLight*>(w.at(spot_type, candidates[k].index));
            const f32 candela = light->intensity / (4.0f * kPi);
            const f32 outer = light->outer_angle, inner = light->inner_angle < outer ? light->inner_angle : outer;
            put(rhi::LightType::Spot, position, forward_of(m), light_rgb(light->color, light->use_temperature, light->temperature) * candela,
                light->attenuation_radius, std::cos(radians(inner)), std::cos(radians(outer)));
        }
    }
    return s;
}

} // namespace lsd::editor
