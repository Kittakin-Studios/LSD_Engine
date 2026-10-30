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

namespace {

// Four cascades split between near and far (Nvidia's practical split: mostly logarithmic), each a
// sphere around its slice of the view so it does not change size as the camera turns, snapped to
// whole texels so shadow edges do not crawl when the camera moves.
void fit_cascades(rhi::LightSetup& s, u32 light_index, Vec3 travel, const LightView& view, f32 distance) {
    const u32 count = 4;
    const f32 near_z = 0.1f, far_z = distance;
    const f32 tile = 0.5f, tile_texels = static_cast<f32>(kShadowAtlasSize) * tile;
    const f32 tan_y = std::tan(view.fov_y * 0.5f), tan_x = tan_y * view.aspect;
    const Vec3 up = std::fabs(travel.z) > 0.99f ? Vec3{1, 0, 0} : Vec3{0, 0, 1};
    const Vec3 side = normalize(cross(travel, up));  // the light view's basis (same as look_at builds)
    const Vec3 lift = cross(side, travel);
    f32 previous = near_z;
    f32 texel0 = 0.0f;
    for (u32 c = 0; c < count; ++c) {
        const f32 fraction = static_cast<f32>(c + 1) / static_cast<f32>(count);
        const f32 uniform = near_z + (far_z - near_z) * fraction;
        const f32 logarithmic = near_z * std::pow(far_z / near_z, fraction);
        const f32 split = uniform + (logarithmic - uniform) * 0.75f;
        const f32 middle = (previous + split) * 0.5f;
        auto corner = [&](f32 depth) {  // distance from the slice centre to a corner at `depth`
            const f32 dx = depth * tan_x, dy = depth * tan_y, dz = depth - middle;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        const f32 radius = std::ceil(std::fmax(corner(previous), corner(split)) * 16.0f) / 16.0f;  // rounded: keeps the texel size steady
        Vec3 centre = view.eye + view.forward * middle;
        const f32 texel = radius * 2.0f / tile_texels;
        const f32 cs = dot(centre, side), cu = dot(centre, lift);
        centre = centre + side * (std::floor(cs / texel) * texel - cs) + lift * (std::floor(cu / texel) * texel - cu);
        const f32 back = 100.0f;  // casters this far towards the light still cast
        const Mat4 light_view = look_at(centre - travel * (radius + back), centre, up);
        const Mat4 vp = orthographic(radius * 2.0f, radius * 2.0f, 0.1f, radius * 2.0f + back) * light_view;
        for (int k = 0; k < 16; ++k) s.shadow_matrices[c][k] = vp.m[k];
        s.shadow_tiles[c][0] = static_cast<f32>(c % 2) * tile;
        s.shadow_tiles[c][1] = static_cast<f32>(c / 2) * tile;
        s.shadow_tiles[c][2] = tile;
        s.shadow_splits[c] = split;
        if (c == 0) texel0 = texel;
        previous = split;
    }
    s.shadow_info[0] = static_cast<f32>(count);
    s.shadow_info[1] = static_cast<f32>(light_index);
    s.shadow_info[2] = 1.0f / static_cast<f32>(kShadowAtlasSize);
    s.shadow_info[3] = texel0 * 1.5f;  // normal offset
}

} // namespace

rhi::LightSetup world_lights(const world::World& w, const LightView& view, bool skip_hidden) {
    const Vec3 eye = view.eye;
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
        l.shadow_tile = -1.0f;
    };
    // Local shadow tiles: a perspective view per spot light, six 90-degree views per point light.
    u32 tiles_used = 0;
    auto add_tile = [&](Vec3 from, Vec3 dir, Vec3 up, f32 fov, f32 range) {
        const u32 t = tiles_used++;
        const Mat4 vp = perspective(fov, 1.0f, 0.05f, range) * look_at(from, from + dir, up);
        for (int k = 0; k < 16; ++k) s.local_matrices[t][k] = vp.m[k];
        s.local_tiles[t][0] = static_cast<f32>(t % 4) * 0.25f;
        s.local_tiles[t][1] = static_cast<f32>(t / 4) * 0.25f;
        s.local_tiles[t][2] = 0.25f;
    };
    const f32 tile_texels = static_cast<f32>(kShadowAtlasSize) * 0.25f;
    for (u32 i = 0; i < w.count(dir_type) && s.count < rhi::kMaxLights; ++i) {
        const world::Entity e = w.owner(dir_type, i);
        if (skip_hidden && w.hidden_in_hierarchy(e)) continue;
        const auto* light = static_cast<const world::DirectionalLight*>(w.at(dir_type, i));
        const Vec3 travel = forward_of(w.world_matrix(e));
        if (light->cast_shadows && s.shadow_info[1] < 0.0f) fit_cascades(s, s.count, travel, view, light->shadow_distance);
        put(rhi::LightType::Directional, {}, travel, light_rgb(light->color, light->use_temperature, light->temperature) * light->intensity, 0, 0, 0);
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
            if (light->cast_shadows && tiles_used + 6 <= rhi::kMaxShadowTiles) {
                rhi::GpuLight& l = s.lights[s.count - 1];
                l.shadow_tile = static_cast<f32>(tiles_used);
                l.shadow_offset = 2.0f / tile_texels * 1.5f;  // texel size per metre for a 90 degree view
                const Vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                for (int f = 0; f < 6; ++f) add_tile(position, dirs[f], f < 4 ? Vec3{0, 0, 1} : Vec3{1, 0, 0}, kPi * 0.5f, light->attenuation_radius);
            }
        } else {
            const auto* light = static_cast<const world::SpotLight*>(w.at(spot_type, candidates[k].index));
            const f32 candela = light->intensity / (4.0f * kPi);
            const f32 outer = light->outer_angle, inner = light->inner_angle < outer ? light->inner_angle : outer;
            const Vec3 dir = forward_of(m);
            put(rhi::LightType::Spot, position, dir, light_rgb(light->color, light->use_temperature, light->temperature) * candela,
                light->attenuation_radius, std::cos(radians(inner)), std::cos(radians(outer)));
            if (light->cast_shadows && tiles_used + 1 <= rhi::kMaxShadowTiles) {
                rhi::GpuLight& l = s.lights[s.count - 1];
                const f32 fov = radians(outer) * 2.0f + 0.1f;  // the cone, plus a little for the filter
                l.shadow_tile = static_cast<f32>(tiles_used);
                l.shadow_offset = 2.0f * std::tan(fov * 0.5f) / tile_texels * 1.5f;
                add_tile(position, dir, std::fabs(dir.z) > 0.99f ? Vec3{1, 0, 0} : Vec3{0, 0, 1}, fov, light->attenuation_radius);
            }
        }
    }
    s.local_tiles[0][3] = static_cast<f32>(tiles_used);  // tiles in use (read by the editor's shadow pass)
    return s;
}

} // namespace lsd::editor
