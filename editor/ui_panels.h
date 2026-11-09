#pragma once
// World UI panels (Unreal's Widget Component in World space): each UiPanel component's document gets
// its own UI context, laid out at the panel's resolution and drawn into a render target that the
// main draw loop puts on the panel's quad (paper2d.h: ui_panel_mesh). In Play, on<event> actions go
// to the scripts and lsd.ui_data reaches the panels too; with the mouse cursor shown, clicks on a
// panel are turned into clicks on its document.
#include "lsd/foundation/math.h"
#include "lsd/rhi/rhi.h"
#include "lsd/ui/ui.h"
#include "lsd/world/world.h"

#include <vector>

union SDL_Event;

namespace lsd::asset {
class AssetRegistry;
}

namespace lsd::editor {

class UiPanels {
public:
    UiPanels() = default;
    UiPanels(const UiPanels&) = delete;
    UiPanels& operator=(const UiPanels&) = delete;
    ~UiPanels() { clear(); }

    // Where the panels' on<event> actions go (Play: the scripts); nullptr: nowhere.
    void set_actions(ui::ActionHandler handler, void* user);
    // Opens, reopens (document or size changed) and closes panels to match the world's UiPanel
    // components; entering or leaving Play opens them all afresh. Once a frame, before render.
    void sync(const world::World& world, const asset::AssetRegistry* registry, rhi::Device& device, bool playing);
    void render(rhi::Device& device);  // every panel's document into its texture (between begin / end frame)
    rhi::TextureHandle texture(world::Entity e) const;  // invalid if that object has no panel (yet)
    // lsd.ui_data for the panels: type 0 number, 1 text, 2 boolean, 3 JSON (as ScriptHost::ui_data).
    void set_data(const char* name, int type, f64 number, const char* text);
    // A mouse event over the view (viewport: x, y, width, height in window pixels; view_proj: what was
    // drawn there): sent to the nearest clickable panel under the cursor, at its document's pixels.
    // shortcut: walls in front of a panel do not stop the click; ray-test them when that matters.
    bool process_event(const SDL_Event& event, const world::World& world, const Mat4& view_proj, const f32 viewport[4]);
    void clear();  // closes every panel (before the device goes)

private:
    struct Panel {
        world::Entity entity;
        char document[33];
        u32 width, height;  // pixels
        ui::UiContext* context;
        rhi::TextureHandle target;
        bool seen;
        bool hovered;
    };
    std::vector<Panel> panels_;  // shortcut: std::vector, a handful of panels per level
    rhi::Device* device_ = nullptr;
    ui::ActionHandler actions_ = nullptr;
    void* actions_user_ = nullptr;
    bool playing_ = false;
    u32 serial_ = 0;  // RmlUi context names must be unique
};

} // namespace lsd::editor
