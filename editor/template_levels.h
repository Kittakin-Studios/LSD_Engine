#pragma once
// Project templates' levels (Unreal's New Project templates), built from code: no files, no editor
// state, so the tests build them too. templates.h puts them into the editor (picker, files, views).
#include "lsd/foundation/types.h"

namespace lsd::world {
class World;
}

namespace lsd::editor {

// In the picker's order: the five game perspectives first.
enum class Template : u8 {
    ThirdPerson,
    FirstPerson,
    SideScrolling,
    Platformer,
    Orthographic,
    TwoD,
    TwoPointFiveD,
    CharacterCustomization,
    CharacterSelection,
    Empty,
    Count
};

const char* template_name(Template t);
const char* template_description(Template t);

// The 2D template's images, by asset GUID (empty in tests: the level still builds).
struct TemplateArt {
    char tiles[33] = "";
    char hero[33] = "";
    char coin[33] = "";
    char backdrop[33] = "";
    char coin_script[33] = "";    // Coin.lua: collected on touch
    char player_script[33] = "";  // Player2D.lua: the coin count on the HUD (UI_HUD2D.rml)
    // Character Customization / Selection: the body (a UE5-skeleton Skeletal Mesh; empty: a placeholder) and their scripts.
    char body[33] = "";
    char customize_script[33] = "";  // CharacterCustomization.lua: tabs, morph sliders, clothes, save
    char select_script[33] = "";     // CharacterSelection.lua: browse the presets, pick one
};
// Fills `world` with the template's level (cleared first). Pure: no files, no editor state.
void build_template_world(world::World& world, Template t, const TemplateArt& art);
// The 2D art as RGBA pixels (width x height x 4), for build_template / tests.
void template_tileset(u8* rgba);   // 128 x 128: 4 x 4 tiles of 32 px
void template_hero(u8* rgba);      // 128 x 64: 4 x 2 cells of 32 px (idle 0-1, walk 2-5, jump 6, fall 7)
void template_coin(u8* rgba);      // 64 x 16: 4 cells of 16 px, spinning
void template_backdrop(u8* rgba);  // 256 x 128: sky and hills
constexpr u32 kTilesetSize = 128, kHeroWidth = 128, kHeroHeight = 64, kCoinWidth = 64, kCoinHeight = 16, kBackdropWidth = 256, kBackdropHeight = 128;

} // namespace lsd::editor
