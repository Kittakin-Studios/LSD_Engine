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

namespace {

// Fog and post-processing on top of the lights (also when the level has no lights).
void add_atmosphere(const world::World& w, Vec3 eye, bool skip_hidden, rhi::LightSetup& s) {
    const reflect::TypeInfo& fog_type = reflect::type_of<world::HeightFog>();
    for (u32 i = 0; i < w.count(fog_type); ++i) {
        if (skip_hidden && w.hidden_in_hierarchy(w.owner(fog_type, i))) continue;
        const auto* fog = static_cast<const world::HeightFog*>(w.at(fog_type, i));
        const Mat4 m = w.world_matrix(w.owner(fog_type, i));
        s.fog[0] = fog->density;
        s.fog[1] = fog->height_falloff;
        s.fog[2] = m.at(2, 3);  // the object's height is where the density is as set
        s.fog[3] = fog->max_opacity;
        s.fog_color[0] = fog->color.r;
        s.fog_color[1] = fog->color.g;
        s.fog_color[2] = fog->color.b;
        s.fog_color[3] = fog->start_distance;
        break;
    }
    const reflect::TypeInfo& post_type = reflect::type_of<world::PostProcessVolume>();
    const world::PostProcessVolume* best = nullptr;
    for (u32 i = 0; i < w.count(post_type); ++i) {
        const world::Entity e = w.owner(post_type, i);
        if (skip_hidden && w.hidden_in_hierarchy(e)) continue;
        const auto* v = static_cast<const world::PostProcessVolume*>(w.at(post_type, i));
        if (!v->unbound) {
            const Vec3 p = transform_point(affine_inverse(w.world_matrix(e)), eye);  // the unit volume: x, y in -0.5..0.5, z in 0..1
            if (p.x < -0.5f || p.x > 0.5f || p.y < -0.5f || p.y > 0.5f || p.z < 0.0f || p.z > 1.0f) continue;
        }
        if (!best || v->priority > best->priority) best = v;
    }
    if (!best) return;
    s.exposure *= std::pow(2.0f, best->exposure_compensation);
    s.post[0] = best->saturation;
    s.post[1] = best->contrast;
    s.post[2] = best->vignette;
    s.post[3] = best->filmic_tonemapper ? 1.0f : 0.0f;
    s.tint[0] = best->tint.r;
    s.tint[1] = best->tint.g;
    s.tint[2] = best->tint.b;
}

rhi::LightSetup lights_only(const world::World& w, const LightView& view, bool skip_hidden);

u32 g_quality = 2;

} // namespace

void set_shadow_quality(u32 quality) { g_quality = quality < 3 ? quality : 3; }

rhi::LightSetup world_lights(const world::World& w, const LightView& view, bool skip_hidden) {
    rhi::LightSetup s = lights_only(w, view, skip_hidden);
    add_atmosphere(w, view.eye, skip_hidden, s);
    return s;
}

bool world_post(const world::World& w, Vec3 eye, bool skip_hidden, rhi::PostDraw& out) {
    const reflect::TypeInfo& post_type = reflect::type_of<world::PostProcessVolume>();
    const world::PostProcessVolume* best = nullptr;
    for (u32 i = 0; i < w.count(post_type); ++i) {
        const world::Entity e = w.owner(post_type, i);
        if (skip_hidden && w.hidden_in_hierarchy(e)) continue;
        const auto* v = static_cast<const world::PostProcessVolume*>(w.at(post_type, i));
        if (!v->unbound) {
            const Vec3 p = transform_point(affine_inverse(w.world_matrix(e)), eye);
            if (p.x < -0.5f || p.x > 0.5f || p.y < -0.5f || p.y > 0.5f || p.z < 0.0f || p.z > 1.0f) continue;
        }
        if (!best || v->priority > best->priority) best = v;
    }
    if (!best) return false;
    out.bloom_intensity = best->bloom_intensity;
    out.bloom_threshold = best->bloom_threshold;
    out.tint[0] = best->tint.r;
    out.tint[1] = best->tint.g;
    out.tint[2] = best->tint.b;
    out.tonemap = best->filmic_tonemapper;
    out.saturation = best->saturation;
    out.contrast = best->contrast;
    out.vignette = best->vignette;
    out.fxaa = best->anti_aliasing;
    return true;
}

bool world_sky(const world::World& w, const Mat4& view_proj, Vec3 eye, bool skip_hidden, rhi::SkyDraw& out) {
    const reflect::TypeInfo& sky_type = reflect::type_of<world::Sky>();
    const world::Sky* sky = nullptr;
    for (u32 i = 0; i < w.count(sky_type) && !sky; ++i) {
        if (!skip_hidden || !w.hidden_in_hierarchy(w.owner(sky_type, i))) sky = static_cast<const world::Sky*>(w.at(sky_type, i));
    }
    if (!sky) return false;
    out = rhi::SkyDraw{};
    // The inverse view-projection (general 4x4 inverse by cofactors).
    const f32* m = view_proj.m;
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
    if (std::fabs(det) < 1e-12f) return false;
    for (int i = 0; i < 16; ++i) out.inv_view_proj[i] = inv[i] / det;
    const f32 b = sky->brightness;
    const LinearColor* colors[3] = {&sky->zenith, &sky->horizon, &sky->ground};
    f32* slots[3] = {out.zenith, out.horizon, out.ground};
    for (int k = 0; k < 3; ++k) {
        slots[k][0] = colors[k]->r * b;
        slots[k][1] = colors[k]->g * b;
        slots[k][2] = colors[k]->b * b;
    }
    out.camera[0] = eye.x;
    out.camera[1] = eye.y;
    out.camera[2] = eye.z;
    const reflect::TypeInfo& dir_type = reflect::type_of<world::DirectionalLight>();
    for (u32 i = 0; i < w.count(dir_type) && sky->sun_disc; ++i) {
        const world::Entity e = w.owner(dir_type, i);
        if (skip_hidden && w.hidden_in_hierarchy(e)) continue;
        const Vec3 toward = -forward_of(w.world_matrix(e));  // the light travels along +X; the sun is the other way
        out.sun_direction[0] = toward.x;
        out.sun_direction[1] = toward.y;
        out.sun_direction[2] = toward.z;
        out.sun_direction[3] = std::cos(radians(sky->sun_size * 0.5f));
        out.sun_color[0] = sky->sun_color.r * 4.0f;  // brighter than the sky around it
        out.sun_color[1] = sky->sun_color.g * 4.0f;
        out.sun_color[2] = sky->sun_color.b * 4.0f;
        out.sun_color[3] = 1.0f;
        break;
    }
    return true;
}

namespace {

rhi::LightSetup lights_only(const world::World& w, const LightView& view, bool skip_hidden) {
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
        static const f32 distance_scale[4] = {0.0f, 0.6f, 1.0f, 1.5f};
        if (light->cast_shadows && g_quality > 0 && s.shadow_info[1] < 0.0f) fit_cascades(s, s.count, travel, view, light->shadow_distance * distance_scale[g_quality]);
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
            if (light->cast_shadows && g_quality >= 2 && tiles_used + 6 <= rhi::kMaxShadowTiles) {
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
            if (light->cast_shadows && g_quality >= 2 && tiles_used + 1 <= rhi::kMaxShadowTiles) {
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

} // namespace

} // namespace lsd::editor
