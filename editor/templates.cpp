#include "templates.h"

#include "tips.h"
#include "transactions.h"
#include "lsd/asset/asset_registry.h"
#include "lsd/asset/guid.h"
#include "lsd/asset/texture.h"
#include "lsd/foundation/log.h"
#include "lsd/reflect/reflect.h"
#include "lsd/world/level.h"
#include "lsd/world/world.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace lsd::editor {

constexpr f32 kPi = 3.14159265f;

u32 template_viewports(Template t) {
    switch (t) {
        case Template::SideScrolling: return kViewport2_5D | kViewport3D;
        case Template::TwoD: return kViewport2D;
        case Template::TwoPointFiveD: return kViewport2_5D;
        default: return kViewport3D;
    }
}

namespace {

const char kCoinLua[] = R"(-- A coin (2D template): touching it collects it; the player's script counts it.
function on_update(self, dt)
  local px, py, pz = lsd.position(lsd.player())
  local x, y, z = lsd.position(self.entity)
  local dx, dz = px - x, (pz + 0.5) - (z + 0.3)   -- the player's middle to the coin's middle
  if dx * dx + dz * dz < 0.5 then
    lsd.broadcast("coin_collected")
    lsd.destroy(self.entity)
  end
end
)";

const char kPlayerLua[] = R"(-- The 2D template's player: the coin count on the HUD (UI_HUD2D).
function on_start(self)
  self.coins = 0
  self.hud = lsd.ui_open("UI_HUD2D")
end

function coin_collected(self)
  self.coins = self.coins + 1
  lsd.ui_set_text(self.hud, "coins", tostring(self.coins))
end
)";

const char kHudRml[] = R"(<rml>
<head>
<title>HUD</title>
<style>
body { font-family: sans; width: 100%; height: 100%; }
div { display: block; }
#coin_panel { position: absolute; left: 30px; top: 24px; padding: 8px 18px; border-radius: 8px;
              background-color: rgba(13, 4, 4, 170); color: #FFD23C; font-size: 34px; font-weight: bold; }
</style>
</head>
<body>
<div id="coin_panel">Coins: <span id="coins">0</span></div>
</body>
</rml>
)";

// Writes a starter file into Content/Templates/2D (kept if it is already there: edits survive) and returns its GUID.
bool template_file(EditorState& s, const char* file, u32 width, u32 height, void (*draw)(u8*), const char* text, char out_guid[33]) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(s.registry->content_dir()) / "Templates" / "2D";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path path = dir / file;
    if (!fs::exists(path)) {
        bool ok = false;
        if (draw) {
            std::vector<u8> pixels(static_cast<usize>(width) * height * 4);
            draw(pixels.data());
            ok = asset::write_png(path.string().c_str(), width, height, pixels.data());
        } else if (FILE* f = std::fopen(path.string().c_str(), "wb")) {
            ok = std::fwrite(text, 1, std::strlen(text), f) == std::strlen(text);
            ok = std::fclose(f) == 0 && ok;
        }
        if (!ok) {
            LSD_LOG_ERROR("editor", "Could not write %s", path.string().c_str());
            return false;
        }
    }
    s.registry->scan();
    const std::string want = (fs::path("Templates") / "2D" / file).generic_string();
    for (usize i = 0; i < s.registry->count(); ++i) {
        const asset::AssetRecord& r = s.registry->get(i);
        if (fs::path(r.path).generic_string() == want) {
            asset::to_string(r.guid, out_guid);
            return true;
        }
    }
    return false;
}

} // namespace

void build_template(EditorState& s, Template t, const char* level_name) {
    namespace fs = std::filesystem;
    TemplateArt art;
    if (t == Template::TwoD && s.registry) {
        template_file(s, "T_Tiles.png", kTilesetSize, kTilesetSize, template_tileset, nullptr, art.tiles);
        template_file(s, "T_Hero.png", kHeroWidth, kHeroHeight, template_hero, nullptr, art.hero);
        template_file(s, "T_Coin.png", kCoinWidth, kCoinHeight, template_coin, nullptr, art.coin);
        template_file(s, "T_Backdrop.png", kBackdropWidth, kBackdropHeight, template_backdrop, nullptr, art.backdrop);
        template_file(s, "Coin.lua", 0, 0, nullptr, kCoinLua, art.coin_script);
        template_file(s, "Player2D.lua", 0, 0, nullptr, kPlayerLua, art.player_script);
        char hud[33];
        template_file(s, "UI_HUD2D.rml", 0, 0, nullptr, kHudRml, hud);
    }
    build_template_world(*s.world, t, art);
    // A free name in Content/Levels: Name, Name 2, Name 3...
    const fs::path levels = fs::path(s.registry ? s.registry->content_dir() : "Content") / "Levels";
    std::error_code ec;
    fs::create_directories(levels, ec);
    const std::string base = level_name && level_name[0] ? level_name : template_name(t);
    fs::path path = levels / (base + ".lsdlevel");
    for (int n = 2; fs::exists(path) && n < 1000; ++n) path = levels / (base + " " + std::to_string(n) + ".lsdlevel");
    const std::string level = path.generic_string();
    if (!world::save_level_file(*s.world, level.c_str())) {
        std::snprintf(s.status, sizeof s.status, "Could not save %s", level.c_str());
        LSD_LOG_ERROR("editor", "%s", s.status);
        return;
    }
    std::snprintf(s.open_level_requested, sizeof s.open_level_requested, "%s", level.c_str());  // opened like any level (history, lighting)
    std::snprintf(s.settings.last_level, sizeof s.settings.last_level, "%s", level.c_str());
    // The project's maps, when not set yet: the editor and the packaged game start here.
    if (!s.project.editor_startup_level[0]) std::snprintf(s.project.editor_startup_level, sizeof s.project.editor_startup_level, "%s", level.c_str());
    if (!s.project.game_default_level[0]) std::snprintf(s.project.game_default_level, sizeof s.project.game_default_level, "%s", level.c_str());
    fs::create_directories("Config", ec);
    reflect::save_file(s.project, "Config/project.json");
    // The editor camera near the player, then the template's viewports and Level mode.
    s.camera = EditorCamera{};
    if (t == Template::TwoD) {
        s.camera.view = ViewType::Front;
        s.camera.position = {12, -30, 5};
        s.camera.ortho_height = 14.0f;
    } else if (t == Template::SideScrolling || t == Template::TwoPointFiveD) {
        s.camera.position = {10, -24, 4};
        s.camera.yaw = kPi * 0.5f;
        s.camera.pitch = -0.12f;
        s.camera.pivot_distance = 24.0f;
    } else {
        s.camera.position = {-9, -9, 6};
        s.camera.yaw = kPi * 0.25f;
        s.camera.pitch = -0.4f;
        s.camera.pivot_distance = 12.0f;
    }
    set_viewport_kinds(s, template_viewports(t));
    s.mode = t == Template::TwoD ? EditorMode::Paper2D : EditorMode::Select;
    std::snprintf(s.status, sizeof s.status, "New %s project: %s", template_name(t), level.c_str());
    LSD_LOG_INFO("editor", "%s", s.status);
}

void draw_template_picker(EditorState& s) {
    if (!s.show_template_picker) return;
    static int chosen = static_cast<int>(Template::ThirdPerson);
    static char name[64] = "";
    if (!ImGui::IsPopupOpen("New Project")) ImGui::OpenPopup("New Project");
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(820, 520), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    ImGui::TextWrapped("Pick a template to start from. It makes a level in Content/Levels with everything already set up; "
                       "change anything afterwards. Your saved levels open straight away next time.");
    ImGui::Separator();
    const f32 card_w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3.0f) / 4.0f;
    for (int i = 0; i < static_cast<int>(Template::Count); ++i) {
        if (i % 4) ImGui::SameLine();
        ImGui::PushID(i);
        ImGui::BeginGroup();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##card", chosen == i, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(card_w, 150))) {
            chosen = i;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                build_template(s, static_cast<Template>(i), name);
                s.show_template_picker = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SetItemTooltip("%s", template_description(static_cast<Template>(i)));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(ImVec2(at.x + 6, at.y + 6), ImVec2(at.x + card_w - 6, at.y + 44), chosen == i ? IM_COL32(243, 195, 18, 255) : IM_COL32(60, 56, 56, 255), 4.0f);
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.25f, ImVec2(at.x + 14, at.y + 14), chosen == i ? IM_COL32(20, 20, 20, 255) : IM_COL32(255, 255, 255, 255),
                      template_name(static_cast<Template>(i)));
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(at.x + 12, at.y + 52), IM_COL32(210, 210, 210, 255), template_description(static_cast<Template>(i)), nullptr,
                      card_w - 24);
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("Level name", template_name(static_cast<Template>(chosen)), name, sizeof name);
    ImGui::SetItemTooltip("The new level's file name in Content/Levels (empty: the template's name)");
    if (s.history && s.history->dirty() && s.settings.last_level[0]) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "The open level has unsaved changes: save it first or they are lost.");
    if (with_tip(ImGui::Button("Create", ImVec2(140, 0)), "Makes the level from the chosen template and opens it")) {
        build_template(s, static_cast<Template>(chosen), name);
        s.show_template_picker = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (with_tip(ImGui::Button("Cancel", ImVec2(100, 0)), "Keeps the level that is open now")) {
        s.show_template_picker = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace lsd::editor
