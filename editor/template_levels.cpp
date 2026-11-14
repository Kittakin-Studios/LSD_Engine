#include "template_levels.h"

#include "lsd/world/components.h"
#include "lsd/world/world.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace lsd::editor {

namespace {

using world::Entity;
constexpr f32 kPi = 3.14159265f;

struct Info {
    const char* name;
    const char* description;
};
const Info kTemplates[] = {
    {"Third Person", "A blockout arena with the player's body in view, the camera on a spring arm over the shoulder."},
    {"First Person", "The same arena seen through the player's eyes: walk, look with the mouse, jump onto ramps and stairs, push boxes."},
    {"Side Scroller", "A 3D level played from the side: run and jump along X, the camera follows side-on."},
    {"Platformer", "Floating platforms to jump across, moving and spinning lifts, a goal at the top; falling respawns you."},
    {"Orthographic", "Top-down / isometric: the arena seen from above at an angle through an orthographic camera that follows the player."},
    {"2D", "Paper2D: a tile map level, an animated player sprite (idle, walk, jump), coins, a backdrop and an orthographic camera."},
    {"2.5D", "3D models in layers (foreground, play plane, background) played on a 2D plane through a side-on perspective camera."},
    {"Character Customization", "A character creator: the project's rigged body on a stage, tabs (Body, Head, Face, Eyes, Nose, Mouth, Ears, Clothes) with morph sliders and clothes per body area, click / slide sounds, saved to the character."},
    {"Character Selection", "Pick a ready-made character: browse the presets you bring (Content/Characters/Presets) on a stage and choose one."},
    {"Empty", "Nothing but a sun and a sky: start from scratch."},
};
static_assert(sizeof kTemplates / sizeof kTemplates[0] == static_cast<usize>(Template::Count), "one entry per template");

// --- level building ---

Entity box(world::World& w, const char* name, Vec3 at, Vec3 size, u32 color, const char* mesh = "Cube", Vec3 rotation = {}) {
    const Entity e = w.create(name);
    world::Transform* t = w.get<world::Transform>(e);
    t->position = at;
    t->scale = size;
    t->rotation = rotation;
    auto* m = w.add<world::StaticMesh>(e);
    std::snprintf(m->mesh, sizeof m->mesh, "%s", mesh);
    m->color = LinearColor::hex(color);
    return e;
}

void sun_and_sky(world::World& w) {
    const Entity sun = w.create("Sun");
    w.get<world::Transform>(sun)->position = {0, 0, 10};
    w.get<world::Transform>(sun)->rotation = {0, 50, 30};
    w.add<world::DirectionalLight>(sun);
    w.add<world::Sky>(w.create("Sky"));
}

// The player: a Character at `feet`; `body` gives it a visible placeholder (third person, side views).
Entity player(world::World& w, Vec3 feet, f32 yaw_degrees, bool body) {
    const Entity e = w.create("Player");
    w.get<world::Transform>(e)->position = feet;
    w.add<world::Character>(e);
    if (body) {
        const Entity b = box(w, "Body", feet + Vec3{0, 0, 0.9f}, {0.6f, 0.45f, 1.8f}, 0xE07A2E);
        w.get<world::StaticMesh>(b)->collision = false;  // the capsule collides, not its look
        const Entity nose = box(w, "Facing", feet + Vec3{0.32f, 0, 1.45f}, {0.12f, 0.3f, 0.12f}, 0x2E2E33);
        w.get<world::StaticMesh>(nose)->collision = false;
        w.set_parent(b, e);
        w.set_parent(nose, e);
    }
    w.get<world::Transform>(e)->rotation = {0, 0, yaw_degrees};  // after parenting: the body and its Facing nose turn with it
    return e;
}

// A Camera child that follows the player side-on (looking along +Y).
void side_camera(world::World& w, Entity player_entity, f32 back, f32 up, bool ortho, f32 size) {
    const Entity cam = w.create("Side Camera");
    const Vec3 feet = w.get<world::Transform>(player_entity)->position;
    w.get<world::Transform>(cam)->position = feet + Vec3{0, -back, up};
    w.get<world::Transform>(cam)->rotation = {0, 0, 90};
    auto* c = w.add<world::Camera>(cam);
    c->auto_activate = true;  // a fixed view that rides along with the player
    c->orthographic = ortho;
    c->ortho_height = size;
    c->fov = ortho ? 60.0f : 40.0f;
    w.set_parent(cam, player_entity);
}

void side_scroller(world::Character& c) {
    c.side_scroller = true;
    c.look_sensitivity = 0.0f;  // the mouse does not turn a side-on player
    c.walk_speed = 6.0f;
    c.jump_speed = 7.5f;
}

void arena(world::World& w) {
    box(w, "Floor", {0, 0, -0.1f}, {40, 40, 0.2f}, 0x8A8A8E);
    for (int side = 0; side < 4; ++side) {  // walls around it
        const f32 x = side < 2 ? (side == 0 ? -20.0f : 20.0f) : 0.0f, y = side >= 2 ? (side == 2 ? -20.0f : 20.0f) : 0.0f;
        box(w, "Wall", {x, y, 1.5f}, side < 2 ? Vec3{0.5f, 40, 3} : Vec3{40, 0.5f, 3}, 0x6E6E74);
    }
    auto* ramp = w.get<world::StaticMesh>(box(w, "Ramp", {6, 6, 1.0f}, {6, 4, 0.3f}, 0xD08A3C, "Cube", {0, -18, 0}));
    ramp->collision_shape = 1;  // the slope itself (a box would be a wall)
    for (int i = 0; i < 6; ++i) box(w, "Step", {-6.0f - static_cast<f32>(i) * 0.8f, 6, 0.15f + static_cast<f32>(i) * 0.3f}, {0.8f, 3, 0.3f + static_cast<f32>(i) * 0.6f}, 0x9C9CA4);
    box(w, "Ledge", {-12, 6, 1.9f}, {4, 6, 0.2f}, 0x9C9CA4);
    for (int i = 0; i < 4; ++i) box(w, "Column", {static_cast<f32>(i) * 4.0f - 6.0f, -8, 2}, {0.8f, 0.8f, 4}, 0x7C7C84);
    for (int i = 0; i < 6; ++i) {  // boxes to push
        const Entity crate = box(w, "Crate", {2.0f + static_cast<f32>(i % 3) * 1.2f, -3.0f - static_cast<f32>(i / 3) * 1.2f, 0.5f}, {1, 1, 1}, 0xC8A060);
        w.add<world::RigidBody>(crate);
    }
}

void build_first_person(world::World& w, bool third) {
    sun_and_sky(w);
    arena(w);
    const Entity p = player(w, {0, 0, 0.05f}, 90, third);
    if (third) {
        auto* c = w.get<world::Character>(p);
        c->camera_distance = 3.5f;
        c->camera_offset = {0, -0.5f, 0.2f};  // over the right shoulder
    }
}

void build_platformer(world::World& w) {
    sun_and_sky(w);
    w.settings().kill_z = -10.0f;  // falling off respawns at the start
    box(w, "Start", {0, 0, -0.25f}, {6, 6, 0.5f}, 0x8A8A8E);
    const Vec3 path[] = {{6, 0, 0.6f}, {10, 3, 1.4f}, {14, 6, 2.2f}, {13, 11, 3.0f}, {8, 13, 3.8f}};
    for (const Vec3& at : path) box(w, "Platform", at, {2.5f, 2.5f, 0.4f}, 0x4FA3D8);
    const Entity lift = box(w, "Lift", {3, 13, 3.8f}, {2.5f, 2.5f, 0.4f}, 0xE0B040);  // up and down
    auto* up = w.add<world::Mover>(lift);
    up->path = 1;
    up->offset = {0, 0, 4};
    up->speed = 1.5f;
    up->pause = 1.0f;
    const Entity spinner = box(w, "Spinner", {-2, 13, 7.8f}, {5, 1.2f, 0.4f}, 0xD05A5A);
    auto* spin = w.add<world::Mover>(spinner);
    spin->path = 0;  // Direction at zero velocity: it only turns
    spin->velocity = {0, 0, 0};
    spin->spin = {0, 0, 40};
    const Entity slider = box(w, "Slider", {-7, 13, 7.8f}, {2.5f, 2.5f, 0.4f}, 0xE0B040);  // side to side
    auto* side = w.add<world::Mover>(slider);
    side->path = 1;
    side->offset = {0, -8, 0};
    side->speed = 2.0f;
    side->pause = 0.5f;
    box(w, "Goal Platform", {-7, 2, 7.8f}, {4, 4, 0.4f}, 0x8A8A8E);
    box(w, "Goal", {-7, 2, 8.6f}, {0.8f, 0.8f, 0.8f}, 0xFFD23C, "Sphere");
    const Entity p = player(w, {0, 0, 0.05f}, 0, true);
    auto* c = w.get<world::Character>(p);
    c->camera_distance = 6.0f;
    c->camera_offset = {0, 0, 1.0f};
    c->jump_speed = 7.0f;
    c->walk_speed = 6.0f;
}

void build_side_scrolling(world::World& w, bool layered) {
    sun_and_sky(w);
    w.settings().kill_z = -10.0f;
    const struct {
        f32 from, to, top;
    } ground[] = {{-4, 18, 0}, {21, 40, 0}, {44, 70, 0}};  // pits between
    for (const auto& g : ground) box(w, "Ground", {(g.from + g.to) * 0.5f, 0, g.top - 1.0f}, {g.to - g.from, 4, 2}, 0x6FA35A);
    const Vec3 platforms[] = {{10, 0, 2.0f}, {14, 0, 3.5f}, {27, 0, 2.2f}, {32, 0, 3.6f}, {50, 0, 2.0f}, {55, 0, 3.4f}, {60, 0, 4.8f}};
    for (const Vec3& at : platforms) box(w, "Platform", at, {3, 4, 0.4f}, 0xB0703C);
    box(w, "Block", {37, 0, 1.0f}, {2, 4, 2}, 0x9C9CA4);
    const Entity lift = box(w, "Lift", {19.5f, 0, -0.2f}, {2.5f, 4, 0.4f}, 0xE0B040);
    auto* m = w.add<world::Mover>(lift);
    m->path = 1;
    m->offset = {0, 0, 3};
    m->speed = 1.5f;
    m->pause = 1.0f;
    box(w, "Goal", {68, 0, 1.0f}, {0.8f, 0.8f, 0.8f}, 0xFFD23C, "Sphere");
    if (layered) {  // 2.5D: scenery in depth layers around the play plane
        for (int i = 0; i < 9; ++i) {
            const f32 x = static_cast<f32>(i) * 9.0f - 2.0f;
            box(w, "Mountain", {x, 26, 3.0f + static_cast<f32>(i % 3) * 2.0f}, {8, 2, 10.0f + static_cast<f32>(i % 3) * 4.0f}, 0x5E6E8C, "Cube", {0, 45, 0});
            box(w, "Tree Trunk", {x + 3.0f, 8, 1.0f}, {0.5f, 0.5f, 3}, 0x6A4A30);
            box(w, "Tree Top", {x + 3.0f, 8, 3.2f}, {2.2f, 2.2f, 2.2f}, 0x3F8A44, "Sphere");
            box(w, "Foreground Rock", {x + 6.0f, -5, 0.2f}, {1.6f, 1.2f, 1.0f}, 0x6E6258, "Sphere");
        }
        box(w, "Backdrop Ground", {33, 16, -1.0f}, {90, 24, 2}, 0x4E7A44);
    }
    const Entity p = player(w, {0, 0, 0.05f}, 0, true);
    side_scroller(*w.get<world::Character>(p));
    side_camera(w, p, layered ? 22.0f : 14.0f, 2.0f, false, 10.0f);
}

// Top-down / isometric: the arena through an orthographic camera riding 45 degrees above and behind.
void build_orthographic(world::World& w) {
    sun_and_sky(w);
    arena(w);
    const Entity p = player(w, {0, 0, 0.05f}, 45, true);
    w.get<world::Character>(p)->look_sensitivity = 0.0f;  // the view stays put; W is up the screen
    const Entity cam = w.create("Ortho Camera");
    w.set_parent(cam, p, false);
    w.get<world::Transform>(cam)->position = {-14, 0, 14};
    w.get<world::Transform>(cam)->rotation = {0, 45, 0};  // looking down at 45 degrees
    auto* c = w.add<world::Camera>(cam);
    c->auto_activate = true;
    c->orthographic = true;
    c->ortho_height = 16.0f;
}

// A stage for showing a character: floor, backdrop, a key and a fill light, and the camera on it.
Entity stage(world::World& w, const char* body_name, const char* body_mesh, const char* script, Vec3 camera, f32 camera_fov) {
    sun_and_sky(w);
    box(w, "Stage", {0, 0, -0.05f}, {3, 3, 0.1f}, 0x3C3838);
    box(w, "Floor", {0, 0, -0.2f}, {30, 30, 0.2f}, 0x242855);
    box(w, "Backdrop", {-3, 0, 3}, {0.2f, 14, 6}, 0x2E2E3A);
    const Vec3 lights[] = {{2.5f, -2.0f, 2.6f}, {2.0f, 2.5f, 1.6f}};
    for (const Vec3& at : lights) {
        const Entity l = w.create("Light");
        w.get<world::Transform>(l)->position = at;
        auto* pl = w.add<world::PointLight>(l);
        pl->intensity = at.y < 0 ? 1600.0f : 700.0f;
        pl->attenuation_radius = 8.0f;
    }
    const Entity body = w.create(body_name);
    auto* mesh = w.add<world::SkeletalMesh>(body);
    std::snprintf(mesh->mesh, sizeof mesh->mesh, "%s", body_mesh);
    if (!body_mesh[0]) {  // no rigged character in the project yet: a stand-in until one is set
        const Entity stand_in = box(w, "Placeholder (set the Skeletal Mesh)", {0, 0, 0.9f}, {0.45f, 0.35f, 1.8f}, 0xB4B4B8);
        w.get<world::StaticMesh>(stand_in)->collision = false;
        w.set_parent(stand_in, body);
    }
    std::snprintf(w.add<world::Script>(body)->script, sizeof(world::Script::script), "%s", script);
    const Entity cam = w.create("Camera");
    w.get<world::Transform>(cam)->position = camera;
    w.get<world::Transform>(cam)->rotation = {0, 4, 180};  // looking back at the body (it faces +X)
    auto* c = w.add<world::Camera>(cam);
    c->auto_activate = true;
    c->fov = camera_fov;
    return body;
}

void build_character_customization(world::World& w, const TemplateArt& art) {
    // Off to the side: the menu covers the left of the screen, the character stands on the right.
    const Entity body = stage(w, "Character", art.body, art.customize_script, {3.6f, -1.0f, 1.05f}, 40.0f);
    w.add<world::Outfit>(body);  // its body areas (Shirt, Pants, Shoes...) are filled on the first frame
}

void build_character_selection(world::World& w, const TemplateArt& art) {
    stage(w, "Character", art.body, art.select_script, {3.8f, 0.0f, 1.05f}, 40.0f);
}

void sprite(world::Sprite& s, const char* texture, u32 columns, u32 rows) {
    std::snprintf(s.texture, sizeof s.texture, "%s", texture);
    s.columns = columns;
    s.rows = rows;
}

void build_2d(world::World& w, const TemplateArt& art) {
    w.settings().kill_z = -6.0f;
    // The level: a tile map (rows from the bottom), tiles = tileset cell + 1, 0 = empty.
    const Entity map_entity = w.create("Level Tiles");
    auto* map = w.add<world::TileMap>(map_entity);
    std::snprintf(map->tileset, sizeof map->tileset, "%s", art.tiles);
    map->tileset_columns = map->tileset_rows = 4;
    map->width = 64;
    map->height = 14;
    map->tiles.count = 0;
    for (u32 i = 0; i < map->width * map->height; ++i) map->tiles.push_back(0);
    auto set = [&](u32 x, u32 y, i32 tile) {
        if (x < map->width && y < map->height) map->tiles[y * map->width + x] = tile;
    };
    for (u32 x = 0; x < map->width; ++x) {
        const bool pit = (x >= 18 && x <= 20) || (x >= 40 && x <= 42);
        if (pit) continue;
        set(x, 0, 2);  // dirt
        set(x, 1, 1);  // grass on top
    }
    const u32 planks[][3] = {{7, 11, 4}, {14, 17, 6}, {24, 28, 4}, {31, 34, 7}, {45, 49, 5}, {52, 55, 8}};
    for (const auto& p : planks) {
        for (u32 x = p[0]; x <= p[1]; ++x) set(x, p[2], 4);  // wood
    }
    for (u32 y = 2; y <= 4; ++y) set(36, y, 3);  // a brick wall to jump over
    for (u32 y = 2; y <= 3; ++y) set(37, y, 3);
    for (u32 y = 0; y < map->height; ++y) set(63, y, 3);  // the far end
    // Backdrop behind everything (further along +Y than the play plane).
    const Entity backdrop = w.create("Backdrop");
    w.get<world::Transform>(backdrop)->position = {32, 6, -3};
    w.get<world::Transform>(backdrop)->scale = {96, 1, 24};
    sprite(*w.add<world::Sprite>(backdrop), art.backdrop, 1, 1);
    w.get<world::Sprite>(backdrop)->pixel_art = false;
    // Coins to collect: flipbooks spinning in place.
    const Vec3 coins[] = {{5.5f, 0, 2.15f}, {6.5f, 0, 2.15f}, {9, 0, 5.5f}, {10, 0, 5.5f}, {15.5f, 0, 7.5f}, {19, 0, 4}, {26, 0, 5.5f}, {32.5f, 0, 8.5f}, {41, 0, 4}, {47, 0, 6.5f}, {53.5f, 0, 9.5f}};
    for (const Vec3& at : coins) {
        const Entity c = w.create("Coin");
        w.get<world::Transform>(c)->position = at;
        w.get<world::Transform>(c)->scale = {0.6f, 1, 0.6f};
        auto* s = w.add<world::Sprite>(c);
        sprite(*s, art.coin, 4, 1);
        s->frame_count = 4;
        s->fps = 8.0f;
        std::snprintf(w.add<world::Script>(c)->script, sizeof(world::Script::script), "%s", art.coin_script);
    }
    // Crates that fall and can be pushed (2D physics: they stay in the X-Z plane).
    for (int i = 0; i < 3; ++i) {
        const Entity crate = w.create("Crate");
        w.get<world::Transform>(crate)->position = {22.0f + static_cast<f32>(i) * 1.05f, 0, 2.0f + static_cast<f32>(i) * 1.1f};
        auto* s = w.add<world::Sprite>(crate);
        sprite(*s, art.tiles, 4, 4);
        s->frame = 2;  // the brick tile
        s->collision = true;
        w.add<world::RigidBody>(crate)->lock_2d = true;
    }
    // The player: an animated sprite on a side-scroller Character, followed by an orthographic camera.
    const Entity p = w.create("Player");
    w.get<world::Transform>(p)->position = {3, 0, 2.05f};
    auto* hero = w.add<world::Sprite>(p);
    sprite(*hero, art.hero, 4, 2);
    hero->frame = 0;
    hero->frame_count = 2;  // idle
    hero->fps = 3.0f;
    hero->walk_frame = 2;
    hero->walk_count = 4;
    hero->jump_frame = 6;
    hero->jump_count = 1;
    std::snprintf(w.add<world::Script>(p)->script, sizeof(world::Script::script), "%s", art.player_script);  // the coin count on the HUD
    auto* c = w.add<world::Character>(p);
    side_scroller(*c);
    c->height = 0.95f;
    c->radius = 0.3f;
    c->eye_height = 0.8f;
    c->jump_speed = 8.0f;
    side_camera(w, p, 10.0f, 2.0f, true, 11.0f);
}

// --- art (drawn in code: no files to ship) ---

struct Canvas {
    u8* px;
    u32 width, height;
    void put(i32 x, i32 y, u32 rgb, u8 a = 255) {
        if (x < 0 || y < 0 || x >= static_cast<i32>(width) || y >= static_cast<i32>(height)) return;
        u8* p = px + (static_cast<usize>(y) * width + static_cast<usize>(x)) * 4;
        p[0] = static_cast<u8>(rgb >> 16);
        p[1] = static_cast<u8>(rgb >> 8);
        p[2] = static_cast<u8>(rgb);
        p[3] = a;
    }
    void rect(i32 x, i32 y, i32 w, i32 h, u32 rgb) {
        for (i32 j = y; j < y + h; ++j) {
            for (i32 i = x; i < x + w; ++i) put(i, j, rgb);
        }
    }
    void disc(f32 cx, f32 cy, f32 rx, f32 ry, u32 rgb) {
        for (i32 j = static_cast<i32>(cy - ry); j <= static_cast<i32>(cy + ry); ++j) {
            for (i32 i = static_cast<i32>(cx - rx); i <= static_cast<i32>(cx + rx); ++i) {
                const f32 dx = (static_cast<f32>(i) + 0.5f - cx) / rx, dy = (static_cast<f32>(j) + 0.5f - cy) / ry;
                if (dx * dx + dy * dy <= 1.0f) put(i, j, rgb);
            }
        }
    }
};

u32 hash(u32 x) {  // stable speckles
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    return x ^ (x >> 16);
}

} // namespace

const char* template_name(Template t) { return kTemplates[static_cast<usize>(t)].name; }
const char* template_description(Template t) { return kTemplates[static_cast<usize>(t)].description; }

void template_tileset(u8* rgba) {
    std::memset(rgba, 0, static_cast<usize>(kTilesetSize) * kTilesetSize * 4);
    Canvas c{rgba, kTilesetSize, kTilesetSize};
    // Image rows run top-down: cell (col, row) starts at (col * 32, row * 32).
    auto speckle = [&](i32 ox, i32 oy, u32 base, u32 dark, u32 seed) {
        c.rect(ox, oy, 32, 32, base);
        for (u32 k = 0; k < 40; ++k) {
            const u32 h = hash(seed * 131 + k);
            c.rect(ox + static_cast<i32>(h % 30), oy + static_cast<i32>((h >> 8) % 30), 2, 2, dark);
        }
    };
    speckle(0, 0, 0x8B5A33, 0x6E4426, 1);  // 0: grass on dirt
    c.rect(0, 0, 32, 9, 0x5DBB45);
    for (i32 x = 0; x < 32; x += 4) c.rect(x, 9, 2, static_cast<i32>(2 + hash(static_cast<u32>(x)) % 3), 0x5DBB45);
    c.rect(0, 0, 32, 2, 0x8CE06A);
    speckle(32, 0, 0x8B5A33, 0x6E4426, 2);  // 1: dirt
    c.rect(64, 0, 32, 32, 0x8E8E96);  // 2: bricks
    for (i32 row = 0; row < 4; ++row) {
        c.rect(64, row * 8 + 7, 32, 1, 0x5E5E66);
        for (i32 x = (row % 2) * 8; x < 32; x += 16) c.rect(64 + x, row * 8, 1, 7, 0x5E5E66);
    }
    c.rect(96, 0, 32, 32, 0xC08A4A);  // 3: wooden planks (a full tile: its collision is the whole square)
    for (i32 y = 0; y < 32; y += 8) {
        c.rect(96, y, 32, 1, 0xE0AE6A);
        c.rect(96, y + 7, 32, 1, 0x8A5E30);
        c.rect(96 + ((y / 8) % 2 ? 8 : 22), y + 1, 1, 6, 0x8A5E30);
    }
    for (u32 k = 4; k < 16; ++k) {  // 4-15: spare colour tiles to paint with
        const u32 colors[] = {0x4FA3D8, 0xE0B040, 0xD05A5A, 0x6FA35A, 0x9C6ACE, 0xE07A2E, 0x3A3A44, 0xEEEEEE, 0x2E8C8C, 0x8C2E5A, 0xB0B040, 0x5A5ACE};
        const i32 ox = static_cast<i32>(k % 4) * 32, oy = static_cast<i32>(k / 4) * 32;
        c.rect(ox, oy, 32, 32, colors[k - 4]);
        c.rect(ox, oy, 32, 2, 0xFFFFFF);
        c.rect(ox, oy + 30, 32, 2, 0x202024);
    }
}

void template_hero(u8* rgba) {
    std::memset(rgba, 0, static_cast<usize>(kHeroWidth) * kHeroHeight * 4);
    Canvas c{rgba, kHeroWidth, kHeroHeight};
    for (u32 cell = 0; cell < 8; ++cell) {
        const f32 ox = static_cast<f32>(cell % 4) * 32.0f, oy = static_cast<f32>(cell / 4) * 32.0f;
        const bool walk = cell >= 2 && cell <= 5, air = cell >= 6;
        const f32 step = walk ? std::sin(static_cast<f32>(cell - 2) * kPi * 0.5f) : 0.0f;
        const f32 bob = cell == 1 ? 1.0f : walk ? std::fabs(step) : 0.0f;
        // Legs (feet at the bottom of the cell: the sprite's pivot), spread while walking or jumping.
        const f32 spread = air ? (cell == 6 ? 5.0f : 3.0f) : step * 5.0f;
        c.rect(static_cast<i32>(ox + 11 + spread), static_cast<i32>(oy + 22 - (air ? 3 : 0)), 4, air ? 7 : 10, 0x2E3A6E);
        c.rect(static_cast<i32>(ox + 17 - spread), static_cast<i32>(oy + 22 - (air ? 3 : 0)), 4, air ? 7 : 10, 0x2E3A6E);
        // Body and head, facing right (+X); Flip X mirrors it.
        c.rect(static_cast<i32>(ox + 9), static_cast<i32>(oy + 12 + bob), 14, 11, 0xE07A2E);
        c.rect(static_cast<i32>(ox + 9), static_cast<i32>(oy + 20 + bob), 14, 2, 0x7A3A14);
        c.disc(ox + 16, oy + 8 + bob, 6.5f, 6.5f, 0xF2C8A0);
        c.rect(static_cast<i32>(ox + 18), static_cast<i32>(oy + 6 + bob), 2, 3, 0x202024);  // the eye
        c.rect(static_cast<i32>(ox + 10), static_cast<i32>(oy + 1 + bob), 12, 3, 0x7A3A14);  // hair
        const f32 arm = air ? -4.0f : step * 3.0f;
        c.rect(static_cast<i32>(ox + 22), static_cast<i32>(oy + 14 + bob + arm), 3, 7, 0xF2C8A0);
    }
}

void template_coin(u8* rgba) {
    std::memset(rgba, 0, static_cast<usize>(kCoinWidth) * kCoinHeight * 4);
    Canvas c{rgba, kCoinWidth, kCoinHeight};
    for (u32 cell = 0; cell < 4; ++cell) {
        const f32 w = 7.0f * std::fabs(std::cos(static_cast<f32>(cell) * kPi * 0.25f)) + 1.0f;
        const f32 cx = static_cast<f32>(cell) * 16.0f + 8.0f;
        c.disc(cx, 8, w, 7, 0xC8941E);
        c.disc(cx, 8, w > 2 ? w - 1.5f : w, 5.5f, 0xFFD23C);
        c.rect(static_cast<i32>(cx - 0.5f), 4, 1, 8, 0xFFF0A0);
    }
}

void template_backdrop(u8* rgba) {
    Canvas c{rgba, kBackdropWidth, kBackdropHeight};
    for (u32 y = 0; y < kBackdropHeight; ++y) {  // sky, lighter toward the horizon
        const f32 t = static_cast<f32>(y) / static_cast<f32>(kBackdropHeight);
        const u32 r = static_cast<u32>(90 + 110 * t), g = static_cast<u32>(160 + 70 * t), b = static_cast<u32>(230 + 20 * t);
        for (u32 x = 0; x < kBackdropWidth; ++x) c.put(static_cast<i32>(x), static_cast<i32>(y), (r << 16) | (g << 8) | b);
    }
    for (u32 x = 0; x < kBackdropWidth; ++x) {  // two rows of hills
        const f32 fx = static_cast<f32>(x);
        const i32 far = static_cast<i32>(78 + 12 * std::sin(fx * 0.05f) + 6 * std::sin(fx * 0.13f));
        const i32 near = static_cast<i32>(96 + 9 * std::sin(fx * 0.08f + 1.0f) + 4 * std::sin(fx * 0.21f));
        for (i32 y = far; y < static_cast<i32>(kBackdropHeight); ++y) c.put(static_cast<i32>(x), y, 0x8FBF9A);
        for (i32 y = near; y < static_cast<i32>(kBackdropHeight); ++y) c.put(static_cast<i32>(x), y, 0x5E9E5A);
    }
    for (u32 k = 0; k < 5; ++k) {  // clouds
        const f32 cx = 20.0f + static_cast<f32>(k) * 52.0f, cy = 18.0f + static_cast<f32>(hash(k) % 20);
        c.disc(cx, cy, 14, 5, 0xFFFFFF);
        c.disc(cx + 8, cy - 3, 9, 5, 0xFFFFFF);
    }
}

void build_template_world(world::World& w, Template t, const TemplateArt& art) {
    w.clear();
    w.settings() = world::WorldSettings{};
    switch (t) {
        case Template::Empty: sun_and_sky(w); break;
        case Template::FirstPerson: build_first_person(w, false); break;
        case Template::ThirdPerson: build_first_person(w, true); break;
        case Template::Platformer: build_platformer(w); break;
        case Template::SideScrolling: build_side_scrolling(w, false); break;
        case Template::TwoPointFiveD: build_side_scrolling(w, true); break;
        case Template::TwoD: build_2d(w, art); break;
        case Template::Orthographic: build_orthographic(w); break;
        case Template::CharacterCustomization: build_character_customization(w, art); break;
        case Template::CharacterSelection: build_character_selection(w, art); break;
        case Template::Count: break;
    }
}

} // namespace lsd::editor
