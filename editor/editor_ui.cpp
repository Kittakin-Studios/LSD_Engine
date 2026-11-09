#include "loc_gather.h"
#include "editor_ui.h"
#include "templates.h"
#include "tips.h"
#include "graph_editor.h"
#include "lighting.h"
#include "material_cache.h"
#include "animation_player.h"
#include "skinned_cache.h"
#include "sound_player.h"
#include "table_editor.h"
#include "anim_graph_editor.h"
#include "flipbook_editor.h"
#include "particle_editor.h"
#include "ui_editor.h"
#include "packaging.h"

#include "lsd/net/join_code.h"
#include "lsd/net/net.h"
#include "lsd/script/material_graph.h"

#include "mesh_cache.h"
#include "debug_draw.h"
#include "references.h"
#include "remote_console.h"
#include "game_module.h"
#include "keymap.h"
#include "editor_modes.h"
#include "paper2d.h"
#include "workspaces.h"
#include "lua_debugger.h"
#include "mesh_editor.h"
#include "mesh_materials.h"
#include "assistant.h"
#include "prefab_editor.h"
#include "timeline_editor.h"
#include "source_control.h"
#include "thumbnails.h"
#include "transactions.h"

#include "lsd/asset/data_asset.h"
#include "lsd/asset/plugins.h"
#include "lsd/core/console.h"
#include "lsd/input/input.h"
#include "lsd/debug_ui/details_panel.h"
#include "lsd/foundation/log.h"
#include "lsd/foundation/system_info.h"
#include "lsd/world/components.h"
#include "lsd/world/level.h"
#include "lsd/world/prefab.h"
#include "lsd/world/spline.h"
#include "lsd/world/curve.h"
#include "lsd/render/material.h"
#include "lsd/asset/mesh_import.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder API
#include <ImGuizmo.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace lsd::editor {

namespace {

bool asset_selected(const EditorState& s, asset::Guid g);
extern ReferenceIndex g_references;

// A Lua or JavaScript script's `properties = { name = value, ... }` (`{ name: value }`) defaults, read from its text: (name, value as text).
// shortcut: a small scanner for numbers, true / false and quoted strings (no nesting, no expressions).
std::vector<std::pair<std::string, std::string>> script_property_defaults(const char* path) {
    std::vector<std::pair<std::string, std::string>> out;
    std::ifstream file(path, std::ios::binary);
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();
    size_t at = text.find("properties");
    while (at != std::string::npos && at > 0 && (std::isalnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_' || text[at - 1] == '.')) at = text.find("properties", at + 1);
    if (at == std::string::npos) return out;
    at = text.find('{', at);
    const size_t end = at == std::string::npos ? at : text.find('}', at);
    if (end == std::string::npos) return out;
    size_t i = at + 1;
    auto skip = [&] {  // spaces, separators and comments (Lua `--`, JavaScript `//`) to the end of their line
        for (;;) {
            while (i < end && (std::isspace(static_cast<unsigned char>(text[i])) || text[i] == ',' || text[i] == ';')) ++i;
            if (i + 1 >= end || !((text[i] == '-' && text[i + 1] == '-') || (text[i] == '/' && text[i + 1] == '/'))) return;
            const size_t eol = text.find('\n', i);
            i = eol == std::string::npos || eol > end ? end : eol;
        }
    };
    for (skip(); i < end; skip()) {
        size_t name_end = i;
        while (name_end < end && (std::isalnum(static_cast<unsigned char>(text[name_end])) || text[name_end] == '_')) ++name_end;
        if (name_end == i) break;
        const std::string name = text.substr(i, name_end - i);
        i = text.find_first_of("=:", name_end);  // Lua `speed = 5`, JavaScript `speed: 5`
        if (i == std::string::npos || i >= end) break;
        ++i;
        while (i < end && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        std::string value;
        if (i < end && (text[i] == '"' || text[i] == '\'')) {
            const char quote = text[i++];
            const size_t close = text.find(quote, i);
            if (close == std::string::npos || close > end) break;
            value = text.substr(i, close - i);
            i = close + 1;
        } else {
            const size_t start = i;
            while (i < end && text[i] != ',' && text[i] != ';' && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
            value = text.substr(start, i - start);
        }
        out.emplace_back(name, value);
    }
    return out;
}

// Instanced Static Mesh > Scatter: copies at random over a circle (random turn and size), replacing the list.
void draw_scatter(world::InstancedStaticMesh& ism) {
    static int count = 100;
    static float radius = 10.0f, min_scale = 0.8f, max_scale = 1.2f;
    ImGui::SeparatorText("Scatter");
    ImGui::DragInt("Count", &count, 1.0f, 1, static_cast<int>(ism.instances.capacity()));
    with_tip(ImGui::DragFloat("Radius (m)", &radius, 0.1f, 0.1f, 1000.0f), "Radius around the object in which Scatter places copies");
    ImGui::DragFloatRange2("Scale", &min_scale, &max_scale, 0.01f, 0.05f, 10.0f);
    if (with_tip(ImGui::Button("Scatter"), "Places Count copies at random inside Radius, on the ground")) {  // the edit is undoable like any Details change
        u32 seed = 0x9E3779B9u ^ static_cast<u32>(ism.instances.count * 7919u + static_cast<u32>(count));
        auto random = [&seed] {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            return static_cast<f32>(seed & 0xFFFFFF) / static_cast<f32>(0xFFFFFF);
        };
        ism.instances.count = 0;
        for (int k = 0; k < count && ism.instances.count < ism.instances.capacity(); ++k) {
            const f32 angle = random() * 6.2831853f, distance = std::sqrt(random()) * radius;  // even over the disc
            world::MeshInstance in;
            in.position = {std::cos(angle) * distance, std::sin(angle) * distance, 0.0f};
            in.rotation = {0.0f, 0.0f, random() * 360.0f};
            const f32 size = min_scale + (max_scale - min_scale) * random();
            in.scale = {size, size, size};
            ism.instances.push_back(in);
        }
    }
    ImGui::SameLine();
    if (with_tip(ImGui::Button("Clear"), "Removes every copy")) ism.instances.count = 0;
}

// Per-object values over a Lua or C / C++ script's defaults (name, default as text); only changed
// values are kept in `overrides`.
void draw_property_overrides(const std::vector<std::pair<std::string, std::string>>& defaults, List<world::ScriptProperty, 16>& overrides);

// Details for a Script's exposed properties: the script's defaults, changed per object.
void draw_script_properties(EditorState& s, world::Script& script) {
    asset::Guid guid;
    const asset::AssetRecord* r = asset::parse_guid(script.script, guid) ? s.registry->find(guid) : nullptr;
    if (!r || r->type != asset::AssetType::Script) return;
    char path[300];
    s.registry->source_path(*r, path, sizeof path);
    const auto defaults = script_property_defaults(path);
    if (defaults.empty()) return;
    ImGui::SeparatorText("Script properties");
    draw_property_overrides(defaults, script.properties);
}

// The same for a Native Script: the defaults come from a fresh state of the C / C++ script.
void draw_native_properties(world::NativeScript& native) {
    const lsd_behaviour* b = game_module::module().find(native.script);
    if (!b || b->property_count == 0) return;
    std::vector<std::pair<std::string, std::string>> defaults;
    for (int i = 0; i < b->property_count; ++i) {
        char text[64];
        script::native_property_default(*b, b->properties[i], text, sizeof text);
        defaults.emplace_back(b->properties[i].name, text);
    }
    ImGui::SeparatorText("Script properties");
    draw_property_overrides(defaults, native.properties);
}

void draw_property_overrides(const std::vector<std::pair<std::string, std::string>>& defaults, List<world::ScriptProperty, 16>& overrides) {
    for (const auto& [name, fallback] : defaults) {
        world::ScriptProperty* own = nullptr;
        for (world::ScriptProperty& p : overrides) {
            if (name == p.name) own = &p;
        }
        const std::string current = own ? own->value : fallback;
        std::string changed;
        bool edited_now = false;
        ImGui::PushID(name.c_str());
        if (fallback == "true" || fallback == "false") {
            bool on = current == "true";
            if ((edited_now = ImGui::Checkbox(name.c_str(), &on))) changed = on ? "true" : "false";
        } else {
            char* stop = nullptr;
            const double number = std::strtod(fallback.c_str(), &stop);
            if (!fallback.empty() && stop && *stop == '\0') {
                float v = static_cast<float>(std::strtod(current.c_str(), nullptr));
                if ((edited_now = ImGui::DragFloat(name.c_str(), &v, 0.1f))) {
                    char text[64];
                    std::snprintf(text, sizeof text, "%g", static_cast<double>(v));
                    changed = text;
                }
                (void)number;
            } else {
                char text[64];
                std::snprintf(text, sizeof text, "%s", current.c_str());
                if ((edited_now = ImGui::InputText(name.c_str(), text, sizeof text))) changed = text;
            }
        }
        if (own) {
            ImGui::SameLine();
            if (with_tip(ImGui::SmallButton("Reset"), "Back to the value the script itself sets")) {  // back to the script's default
                changed = fallback;
                edited_now = true;
            }
        }
        ImGui::PopID();
        if (!edited_now) continue;
        if (changed == fallback) {  // the default again: no override
            for (u32 k = 0; k < overrides.count; ++k) {
                if (name == overrides[k].name) overrides.remove(k);
            }
        } else if (own) {
            std::snprintf(own->value, sizeof own->value, "%s", changed.c_str());
        } else if (overrides.count < overrides.capacity()) {
            world::ScriptProperty p;
            std::snprintf(p.name, sizeof p.name, "%s", name.c_str());
            std::snprintf(p.value, sizeof p.value, "%s", changed.c_str());
            overrides.push_back(p);
        }
    }
}

// Unreal's Keep Simulation Changes: the selected objects are copied back into the level on Stop.
void keep_simulation_changes(EditorState& s) {
    u32 added = 0;
    for (u32 i = 0; i < s.selection_count + 1; ++i) {
        const world::Entity e = i < s.selection_count ? s.selection[i] : s.selected;
        if (!s.world->alive(e)) continue;
        bool known = false;
        for (u32 k = 0; k < s.kept_count; ++k) known = known || s.kept[k] == e;
        if (!known && s.kept_count < EditorState::kMaxKept) {
            s.kept[s.kept_count++] = e;
            ++added;
        }
    }
    std::snprintf(s.status, sizeof s.status, "%u object%s will keep their Play changes when you stop", s.kept_count, s.kept_count == 1 ? "" : "s");
    (void)added;
}
void find_in_content_browser(EditorState& s, asset::Guid guid);
void note_moves(const asset::AssetRegistry& registry, const char* path_prefix, asset::Guid guid);

using world::Entity;

constexpr char kThemePath[] = "Config/editor_theme.json";
constexpr char kContentDir[] = "Content";
constexpr char kLevelsFolder[] = "Content/Levels";
constexpr char kAssetPayload[] = "LSD_ASSET";    // drag-and-drop payload: an asset::Guid
constexpr char kPlacePayload[] = "LSD_PLACE";    // a PlaceItem index
constexpr char kEntityPayload[] = "LSD_ENTITY";  // an Entity (Outliner drag to parent)
constexpr char kOutlinerWindow[] = "Outliner";
constexpr char kDetailsWindow[] = "Details";
constexpr char kContentWindow[] = "Content Browser";

ImU32 u32_color(const LinearColor& c) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a)); }
ImVec4 vec4(const LinearColor& c) { return ImVec4(c.r, c.g, c.b, c.a); }

// Place panel items (Unreal's Place Actors: basic shapes and gameplay helpers).
struct PlaceItem {
    const char* label;
    const char* mesh;       // basic shape, or nullptr
    const char* component;  // extra component type name, or nullptr
};
constexpr PlaceItem kPlaceItems[] = {
    {"Cube", "Cube", nullptr}, {"Sphere", "Sphere", nullptr}, {"Plane", "Plane", nullptr},
    {"Player Start", nullptr, "PlayerStart"}, {"Character", nullptr, "Character"}, {"Blocking Volume", nullptr, "BlockingVolume"},
    {"Kill Volume", nullptr, "KillVolume"}, {"Trigger Volume", nullptr, "TriggerVolume"}, {"Spline", nullptr, "Spline"}, {"Sound Emitter", nullptr, "SoundEmitter"}, {"Particle Effect", nullptr, "ParticleEffect"}, {"Sky", nullptr, "Sky"}, {"Height Fog", nullptr, "HeightFog"},
    {"Post Process Volume", nullptr, "PostProcessVolume"}, {"Lightmass Importance Volume", nullptr, "LightmassImportanceVolume"}, {"Decal", nullptr, "Decal"}, {"Nav Mesh Bounds", nullptr, "NavMeshBounds"}, {"Nav Modifier Volume", nullptr, "NavModifierVolume"}, {"Nav Link", nullptr, "NavLink"}, {"Cull Distance Volume", nullptr, "CullDistanceVolume"}, {"Level Streaming Volume", nullptr, "LevelStreamingVolume"}, {"Water Body", nullptr, "WaterBody"}, {"Water Volume", nullptr, "WaterVolume"}, {"Nav Agent", "Cube", "NavAgent"}, {"AI Controller", nullptr, "AIController"}, {"Audio Volume", nullptr, "AudioVolume"},
    {"Directional Light", nullptr, "DirectionalLight"}, {"Point Light", nullptr, "PointLight"}, {"Spot Light", nullptr, "SpotLight"}, {"Rect Light", nullptr, "RectLight"}, {"Sky Light", nullptr, "SkyLight"}, {"Reflection Capture", nullptr, "ReflectionCapture"}, {"Camera", nullptr, "Camera"}, {"Empty", nullptr, nullptr},
    // Movement prefabs, cubes as placeholders: swap the mesh in Details.
    {"Sub Level", nullptr, "SubLevel"}, {"Drivable Vehicle", "Cube", "Vehicle"}, {"Transport (Ride)", "Cube", "Transport"}, {"Moving Mesh (Mover)", "Cube", "Mover"},
};

// Volumes are the unit cube scaled by the transform (see components.h).
constexpr Vec3 kVolumeLo{-0.5f, -0.5f, 0.0f}, kVolumeHi{0.5f, 0.5f, 1.0f};

bool is_volume(const world::World& w, world::Entity e) {
    const auto* post = w.get<world::PostProcessVolume>(e);
    return w.get<world::BlockingVolume>(e) || w.get<world::KillVolume>(e) || w.get<world::TriggerVolume>(e) || w.get<world::AudioVolume>(e) || (post && !post->unbound) ||
           w.get<world::NavMeshBounds>(e) || w.get<world::NavModifierVolume>(e) || w.get<world::CullDistanceVolume>(e) || w.get<world::LevelStreamingVolume>(e) || w.get<world::WaterVolume>(e) || w.get<world::LightmassImportanceVolume>(e) || w.get<world::Decal>(e) || w.get<world::PcgVolume>(e);
}

// UI-only state for the confirmation dialogs.
char g_confirm_open[260] = "";  // level to open after "discard changes?"
bool g_confirm_new = false;
bool g_open_confirm = false;
bool g_open_save_as = false;
char g_save_as_name[96] = "";
bool g_was_using_gizmo[4] = {};  // per viewport
bool g_alt_duplicated[4] = {};
bool g_gizmo_busy[4] = {};
// Shift+drag with the move gizmo: the camera follows the object. The gizmo keeps reading the mouse
// through the camera as it was when the follow began, or the move would feed on itself.
// shortcut: releasing Shift mid-drag makes the object jump by how far the camera went; hold it to the end
bool g_shift_follow[4] = {};
Vec3 g_follow_camera[4], g_follow_pivot[4];  // the mouse is on (or dragging) the gizmo: Alt+LMB duplicates instead of orbiting
int g_request_active = -1;       // viewport clicked this frame

// The data asset shown in Details (loaded on selection, written back on Save).
u8* g_data = nullptr;  // the selected data asset, in g_data_storage (grown to its type's size)
std::vector<u8> g_data_storage;
std::vector<u8> g_data_original;  // the selected data asset as it was when selected (Revert)
// A buffer of at least `size` bytes for a copy of a reflected struct (operator new: 16-byte aligned on x64).
u8* grow(std::vector<u8>& storage, usize size) {
    if (storage.size() < size) storage.resize(size);
    return storage.data();
}
const reflect::TypeInfo* g_data_type = nullptr;
asset::Guid g_data_guid;
bool g_data_dirty = false;
char* g_listening_key = nullptr;  // Key widget waiting for a key press

// ---- Output Log storage (filled from any thread through the log sink) ----

struct LogLine {
    log::Level level;
    char channel[16];
    char text[240];
};
constexpr u32 kLogLines = 2000;
LogLine g_log[kLogLines];
u32 g_log_next = 0, g_log_count = 0;
bool g_log_scroll = false;
std::mutex g_log_mutex;

// Message Log: warnings and errors only, kept apart so they are not lost in the Output Log.
constexpr u32 kMessages = 300;
LogLine g_messages[kMessages];
u32 g_messages_next = 0, g_messages_count = 0, g_unread_warnings = 0, g_unread_errors = 0;

void log_sink(void*, log::Level level, const char* channel, const char* message) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (level >= log::Level::Warning) {
        LogLine& m = g_messages[g_messages_next];
        m.level = level;
        std::snprintf(m.channel, sizeof m.channel, "%s", channel);
        std::snprintf(m.text, sizeof m.text, "%s", message);
        g_messages_next = (g_messages_next + 1) % kMessages;
        if (g_messages_count < kMessages) ++g_messages_count;
        ++(level >= log::Level::Error ? g_unread_errors : g_unread_warnings);
    }
    LogLine& line = g_log[g_log_next];
    line.level = level;
    std::snprintf(line.channel, sizeof line.channel, "%s", channel);
    std::snprintf(line.text, sizeof line.text, "%s", message);
    g_log_next = (g_log_next + 1) % kLogLines;
    if (g_log_count < kLogLines) ++g_log_count;
    g_log_scroll = true;
}

void draw_stats(EditorState& s, ImVec2 pos, ImVec2 size);

// ---- Edits ----

// Names the edit in progress; it becomes an undo step once the mouse and widgets are idle.
void edited(EditorState& s, const char* label) { std::snprintf(s.pending_edit, sizeof s.pending_edit, "%s", label); }

// Replaces the selection with `e` (or clears it).
void select(EditorState& s, Entity e) {
    if (e != s.selected) {
        s.spline_point = -1;
        s.joint_pivot = false;
        s.pivot_offset = {};  // each object starts on its own origin
    }
    s.selected = e;
    s.selection_count = 0;
    if (e.valid()) {
        s.selection[s.selection_count++] = e;
        s.selected_asset = {};
    }
}

// Ctrl/Shift+click: adds `e` (it becomes the primary) or, with toggle, removes it if already selected.
void add_to_selection(EditorState& s, Entity e, bool toggle) {
    if (!e.valid()) return;
    for (u32 i = 0; i < s.selection_count; ++i) {
        if (s.selection[i] != e) continue;
        if (!toggle) return;
        s.selection[i] = s.selection[--s.selection_count];
        s.selected = s.selection_count ? s.selection[s.selection_count - 1] : Entity{};
        return;
    }
    if (s.selection_count == EditorState::kMaxSelection) return;
    s.selection[s.selection_count++] = e;
    s.selected = e;
    s.selected_asset = {};
}

// Drops dead entities (after undo or delete) and keeps the primary valid.
void prune_selection(EditorState& s) {
    u32 kept = 0;
    for (u32 i = 0; i < s.selection_count; ++i) {
        if (s.world->alive(s.selection[i])) s.selection[kept++] = s.selection[i];
    }
    s.selection_count = kept;
    if (!s.world->alive(s.selected) || !s.is_selected(s.selected)) s.selected = kept ? s.selection[kept - 1] : Entity{};
}

// Selected entities with no selected ancestor: operations on these move children along exactly once.
u32 selection_roots(const EditorState& s, Entity* out) {
    u32 n = 0;
    for (u32 i = 0; i < s.selection_count; ++i) {
        bool nested = false;
        for (Entity p = s.world->parent(s.selection[i]); p.valid() && !nested; p = s.world->parent(p)) nested = s.is_selected(p);
        if (!nested && s.world->alive(s.selection[i])) out[n++] = s.selection[i];
    }
    return n;
}

Entity duplicate(world::World& w, Entity source, Entity parent) { return world::copy_entity(w, source, w, parent); }

// Ctrl+C: the selection (with children) as level text on the clipboard, so it pastes into any level
// or a text editor.
void copy_selection(EditorState& s) {
    if (!s.world->alive(s.selected)) return;
    world::World clip(default_allocator());
    Entity roots[EditorState::kMaxSelection];
    const u32 count = selection_roots(s, roots);
    for (u32 i = 0; i < count; ++i) world::copy_entity(*s.world, roots[i], clip, {});
    Array<char> text(default_allocator());
    world::save_level(clip, text);
    text.push_back('\0');
    ImGui::SetClipboardText(text.data());
    std::snprintf(s.status, sizeof s.status, count > 1 ? "Copied %u objects" : "Copied %u object", count);
}

void paste(EditorState& s) {
    const char* text = ImGui::GetClipboardText();
    if (!text || !std::strstr(text, "\"lsd_level\"")) return;
    world::World clip(default_allocator());
    if (!world::load_level(clip, text, std::strlen(text))) {
        std::snprintf(s.status, sizeof s.status, "The clipboard does not hold objects");
        return;
    }
    select(s, {});
    for (u32 slot = 0; slot < clip.slot_count(); ++slot) {
        const Entity e = clip.entity_at_slot(slot);
        if (e.valid() && !clip.parent(e).valid()) add_to_selection(s, world::copy_entity(clip, e, *s.world, {}), false);
    }
    if (s.selection_count) edited(s, "Paste");
}

Entity place(EditorState& s, const char* name, const char* mesh, const char* component, Vec3 position) {
    char unique[64];
    s.world->unique_name(name, unique, sizeof unique);
    const Entity e = s.world->create(unique);
    s.world->get<world::Transform>(e)->position = position;
    if (mesh) std::snprintf(s.world->add<world::StaticMesh>(e)->mesh, sizeof(world::StaticMesh::mesh), "%s", mesh);
    if (const reflect::TypeInfo* type = component ? world::find_component_type(component) : nullptr) s.world->add(e, *type);
    if (is_volume(*s.world, e)) s.world->get<world::Transform>(e)->scale = {4, 4, 3};  // a useful size to start
    if (s.world->get<world::DirectionalLight>(e)) s.world->get<world::Transform>(e)->rotation = {0, 50, 30};  // aimed down at the scene
    if (s.world->get<world::SpotLight>(e)) {
        s.world->get<world::Transform>(e)->rotation = {0, 90, 0};  // pointing straight down from up here
        s.world->get<world::Transform>(e)->position.z += 3.0f;
    }
    if (s.world->get<world::PointLight>(e)) s.world->get<world::Transform>(e)->position.z += 1.5f;
    // Movement prefabs: a car-sized box, a long platform that shuttles (a train or ferry to ride), a box that rises and falls.
    if (s.world->get<world::Vehicle>(e)) {
        s.world->get<world::Transform>(e)->scale = {4.0f, 2.0f, 1.2f};
        s.world->get<world::StaticMesh>(e)->color = LinearColor::hex(0xC0392B);
    }
    if (s.world->get<world::Transport>(e)) {
        s.world->get<world::Transform>(e)->scale = {8.0f, 3.0f, 0.5f};
        s.world->get<world::StaticMesh>(e)->color = LinearColor::hex(0x2E86C1);
        auto* mover = s.world->add<world::Mover>(e);
        mover->offset = {20.0f, 0.0f, 0.0f};
        mover->pause = 3.0f;
    }
    if (s.world->get<world::Mover>(e) && !s.world->get<world::Transport>(e)) {
        s.world->get<world::Transform>(e)->scale = {2.0f, 2.0f, 0.5f};
        s.world->get<world::StaticMesh>(e)->color = LinearColor::hex(0x27AE60);
    }
    select(s, e);
    char label[96];
    std::snprintf(label, sizeof label, "Place %s", unique);
    edited(s, label);
    return e;
}


// ---- Prefabs ----

// A property reverted from the Details right-click menu, applied next frame (rebuilding the instance
// while the panel draws would pull its components out from under it).
struct RevertRequest {
    Entity object;
    char component[32];
    char property[32];
    bool pending = false;
};
RevertRequest g_revert;
const reflect::TypeInfo* g_details_type = nullptr;  // the component the Details panel is drawing

bool prefab_path(const EditorState& s, const char* guid_text, char* path, usize capacity) {
    asset::Guid guid;
    const asset::AssetRecord* r = asset::parse_guid(guid_text, guid) ? s.registry->find(guid) : nullptr;
    if (!r || r->type != asset::AssetType::Prefab) return false;
    s.registry->source_path(*r, path, capacity);
    return true;
}

// Sub Levels in the editor: shown (loaded) or not as each one asks, and reloaded when its level file
// changes. Play streams them itself (PlaySession).
// shortcut: Sub Levels inside a Sub Level's contents are not loaded (no nesting); add a depth limit when needed.
void update_sublevels(EditorState& s) {
    struct Shown {
        char guid[33];
        i64 time;
    };
    static std::unordered_map<u32, Shown> shown;
    const reflect::TypeInfo& type = reflect::type_of<world::SubLevel>();
    for (u32 i = 0; i < s.world->count(type); ++i) {
        const Entity e = s.world->owner(type, i);
        const auto* sub = static_cast<const world::SubLevel*>(s.world->at(type, i));
        bool nested = false;
        for (Entity p = s.world->parent(e); p.valid() && !nested; p = s.world->parent(p)) nested = s.world->transient(p);
        if (nested) continue;
        asset::Guid guid;
        const asset::AssetRecord* r = sub->show_in_editor && asset::parse_guid(sub->level, guid) ? s.registry->find(guid) : nullptr;
        if (r && r->type != asset::AssetType::Level) r = nullptr;
        const bool loaded = world::sublevel_loaded(*s.world, e);
        if (!r) {
            if (loaded) world::unload_sublevel(*s.world, e);
            shown.erase(e.value);
            continue;
        }
        const auto it = shown.find(e.value);
        const bool current = loaded && it != shown.end() && std::strcmp(it->second.guid, sub->level) == 0 && it->second.time == r->source_time;
        if (current) continue;
        char path[300];
        s.registry->source_path(*r, path, sizeof path);
        if (world::load_sublevel(*s.world, e, path)) {
            Shown entry{};
            std::snprintf(entry.guid, sizeof entry.guid, "%s", sub->level);
            entry.time = r->source_time;
            shown[e.value] = entry;
        } else {
            shown.erase(e.value);
        }
    }
}

// The prefab instance an object belongs to: itself or its nearest ancestor with a PrefabInstance.
Entity prefab_root(const world::World& w, Entity e) {
    for (; e.valid(); e = w.parent(e)) {
        if (w.get<world::PrefabInstance>(e)) return e;
    }
    return {};
}

// Brings the instances of one prefab (every prefab if `guid_text` is null) in line with the asset,
// except `skip`. Returns how many were updated.
u32 refresh_instances(EditorState& s, const char* guid_text, Entity skip) {
    const reflect::TypeInfo& type = reflect::type_of<world::PrefabInstance>();
    Array<Entity> instances(default_allocator());  // collected first: updating changes the pools
    for (u32 i = 0; i < s.world->count(type); ++i) instances.push_back(s.world->owner(type, i));
    u32 updated = 0;
    for (const Entity e : instances) {
        const auto* instance = s.world->get<world::PrefabInstance>(e);  // null if an earlier update removed it (nested)
        if (e == skip || !instance || (guid_text && std::strcmp(instance->prefab, guid_text) != 0)) continue;
        char path[300];
        if (prefab_path(s, instance->prefab, path, sizeof path) && world::update_instance_from_file(*s.world, e, path)) ++updated;
    }
    prune_selection(s);
    return updated;
}

// After an edit: every prefab instance compared with its prefab file, its added / removed components
// and objects listed as structural overrides (prefab files read once per GUID).
void record_prefab_structure(EditorState& s) {
    const reflect::TypeInfo& type = reflect::type_of<world::PrefabInstance>();
    std::map<std::string, std::string> texts;
    for (u32 i = 0; i < s.world->count(type); ++i) {
        const Entity e = s.world->owner(type, i);
        const auto* instance = static_cast<const world::PrefabInstance*>(s.world->at(type, i));
        auto it = texts.find(instance->prefab);
        if (it == texts.end()) {
            char path[300];
            std::string text;
            if (prefab_path(s, instance->prefab, path, sizeof path)) {
                std::ifstream file(path, std::ios::binary);
                text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            }
            it = texts.emplace(instance->prefab, std::move(text)).first;
        }
        if (!it->second.empty() && !world::record_structure_overrides(*s.world, e, it->second.data(), it->second.size())) {
            std::snprintf(s.status, sizeof s.status, "%s holds 32 overrides already: some added or removed parts are not kept", s.world->name(e));
        }
    }
}

// Marks a changed property of an object inside a prefab instance as overridden. The root's own
// Transform and PrefabInstance are never part of the prefab, so they need no override.
void note_override(EditorState& s, Entity object, const reflect::TypeInfo& type, const reflect::Property& p) {
    const Entity root = prefab_root(*s.world, object);
    if (!root.valid() || &type == &reflect::type_of<world::PrefabInstance>() || (object == root && &type == &reflect::type_of<world::Transform>())) return;
    char path[64];
    if (world::instance_path(*s.world, root, object, path, sizeof path) && !world::set_override(*s.world, root, path, type.name, p.name, true)) {
        std::snprintf(s.status, sizeof s.status, "%s holds 32 overrides already: this change is not kept when the prefab changes", s.world->name(root));
    }
}

void note_changed_overrides(EditorState& s, Entity object, const reflect::TypeInfo& type, const u8* before, const u8* after) {
    for (u32 i = 0; i < type.property_count; ++i) {
        const reflect::Property& p = type.properties[i];
        if (std::memcmp(before + p.offset, after + p.offset, p.size) != 0) note_override(s, object, type, p);
    }
}

bool details_is_overridden(void* user, const reflect::Property& p) {
    const EditorState& s = *static_cast<const EditorState*>(user);
    const Entity root = g_details_type ? prefab_root(*s.world, s.selected) : Entity{};
    char path[64];
    return root.valid() && world::instance_path(*s.world, root, s.selected, path, sizeof path) && world::is_overridden(*s.world, root, path, g_details_type->name, p.name);
}

void details_revert(void* user, const reflect::Property& p) {
    const EditorState& s = *static_cast<const EditorState*>(user);
    if (!g_details_type) return;
    g_revert.object = s.selected;
    std::snprintf(g_revert.component, sizeof g_revert.component, "%s", g_details_type->name);
    std::snprintf(g_revert.property, sizeof g_revert.property, "%s", p.name);
    g_revert.pending = true;
}

Entity place_prefab(EditorState& s, const asset::AssetRecord& r, Vec3 position) {
    const Entity e = place(s, r.name, nullptr, "PrefabInstance", position);
    auto* instance = s.world->get<world::PrefabInstance>(e);
    asset::to_string(r.guid, instance->prefab);
    char path[300];
    s.registry->source_path(r, path, sizeof path);
    if (!world::update_instance_from_file(*s.world, e, path, true)) std::snprintf(s.status, sizeof s.status, "Could not read prefab %s", r.path);
    return e;
}

std::string new_asset_path(EditorState& s, const char* base, const char* extension);
const asset::AssetRecord* record_at(EditorState& s, const std::string& path);

// Outliner > Create Prefab: saves the object and its children as a prefab in the open Content
// Browser folder; the object becomes its first instance.
void create_prefab(EditorState& s, Entity root) {
    const std::string path = new_asset_path(s, s.world->name(root), world::kPrefabExtension);
    if (!world::save_prefab_file(*s.world, root, path.c_str())) {
        std::snprintf(s.status, sizeof s.status, "Could not save %s", path.c_str());
        return;
    }
    s.registry->scan();
    if (const asset::AssetRecord* r = record_at(s, path)) {
        asset::to_string(r->guid, s.world->add<world::PrefabInstance>(root)->prefab);
        edited(s, "Create Prefab");
    }
    std::snprintf(s.status, sizeof s.status, "Created prefab %s", path.c_str());
    LSD_LOG_INFO("editor", "%s", s.status);
}

void delete_selected(EditorState& s) {
    if (!s.world->alive(s.selected)) return;
    char label[96];
    if (s.selection_count > 1) std::snprintf(label, sizeof label, "Delete %u objects", s.selection_count);
    else std::snprintf(label, sizeof label, "Delete %s", s.world->name(s.selected));
    Entity roots[EditorState::kMaxSelection];
    const u32 count = selection_roots(s, roots);
    for (u32 i = 0; i < count; ++i) s.world->destroy(roots[i]);
    select(s, {});
    edited(s, label);
}

// Ctrl+D: copies every selected object in place and selects the copies.
void duplicate_selected(EditorState& s) {
    Entity roots[EditorState::kMaxSelection];
    const u32 count = selection_roots(s, roots);
    if (!count) return;
    select(s, {});
    for (u32 i = 0; i < count; ++i) add_to_selection(s, duplicate(*s.world, roots[i], s.world->parent(roots[i])), false);
    edited(s, "Duplicate");
}

// Ctrl+G: a new empty "Group" at the selection's base centre, holding the selected objects.
void group_selection(EditorState& s);
void ungroup_selection(EditorState& s);

// Box of an entity's mesh in its own space; a small box around the origin when it has none.
void local_bounds(EditorState& s, Entity e, Vec3& local_lo, Vec3& local_hi) {
    local_lo = {-0.25f, -0.25f, 0.0f};
    local_hi = {0.25f, 0.25f, 1.8f};
    if (is_volume(*s.world, e)) {
        local_lo = kVolumeLo;
        local_hi = kVolumeHi;
    }
    if (const auto* mesh = s.world->get<world::StaticMesh>(e)) {
        if (const CachedMesh* cached = s.meshes->get(mesh->mesh)) {
            local_lo = cached->bounds_min;
            local_hi = cached->bounds_max;
        }
    }
    if (const auto* skeletal = s.world->get<world::SkeletalMesh>(e); skeletal && s.skinned) {
        if (const SkinnedEntry* model = s.skinned->get(skeletal->mesh)) {  // the rest pose, turned by its Facing
            local_lo = model->model->bounds_min;
            local_hi = model->model->bounds_max;
            const f32 yaw = facing_yaw(*skeletal, model);
            if (yaw != 0.0f) {
                const Mat4 turn = rotation_z(radians(yaw));
                Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
                for (int c = 0; c < 8; ++c) {
                    const Vec3 p = transform_point(turn, {c & 1 ? local_hi.x : local_lo.x, c & 2 ? local_hi.y : local_lo.y, c & 4 ? local_hi.z : local_lo.z});
                    lo = {std::fmin(lo.x, p.x), std::fmin(lo.y, p.y), std::fmin(lo.z, p.z)};
                    hi = {std::fmax(hi.x, p.x), std::fmax(hi.y, p.y), std::fmax(hi.z, p.z)};
                }
                local_lo = lo;
                local_hi = hi;
            }
        }
    }
}

// World-space box of an entity's mesh (local_bounds through its transform).
void world_bounds(EditorState& s, Entity e, Vec3& lo, Vec3& hi) {
    const Mat4 m = s.world->world_matrix(e);
    Vec3 local_lo, local_hi;
    local_bounds(s, e, local_lo, local_hi);
    lo = {1e30f, 1e30f, 1e30f};
    hi = {-1e30f, -1e30f, -1e30f};
    for (int c = 0; c < 8; ++c) {
        const Vec3 p = transform_point(m, {(c & 1) ? local_hi.x : local_lo.x, (c & 2) ? local_hi.y : local_lo.y, (c & 4) ? local_hi.z : local_lo.z});
        lo = vmin(lo, p);
        hi = vmax(hi, p);
    }
}

// ---- Picking ----

struct Ray {
    Vec3 origin, dir;
};

Ray mouse_ray(const EditorCamera& cam, ImVec2 pos, ImVec2 size, ImVec2 mouse) {
    const Mat4 view = camera_view(cam);
    const Vec3 right{view.at(0, 0), view.at(0, 1), view.at(0, 2)};
    const Vec3 up{view.at(1, 0), view.at(1, 1), view.at(1, 2)};
    const Vec3 back{view.at(2, 0), view.at(2, 1), view.at(2, 2)};
    const f32 sx = (mouse.x - pos.x) / size.x * 2.0f - 1.0f, sy = 1.0f - (mouse.y - pos.y) / size.y * 2.0f;
    if (cam.view != ViewType::Perspective) {  // parallel rays from the view plane
        const f32 half_h = cam.ortho_height * 0.5f, half_w = half_h * size.x / size.y;
        return {cam.position + back * 500.0f + right * (sx * half_w) + up * (sy * half_h), -back};
    }
    const f32 tan_half = std::tan(cam.fov_y * 0.5f);
    const f32 nx = sx * tan_half * (size.x / size.y);
    const f32 ny = sy * tan_half;
    return {cam.position, normalize(right * nx + up * ny - back)};
}

// Slab test against a local box; t is in the ray's units.
bool ray_box(Vec3 o, Vec3 d, Vec3 lo, Vec3 hi, f32& t) {
    f32 t0 = 0.0f, t1 = 1e30f;
    for (int a = 0; a < 3; ++a) {
        const f32 oa = a == 0 ? o.x : a == 1 ? o.y : o.z, da = a == 0 ? d.x : a == 1 ? d.y : d.z;
        f32 la = a == 0 ? lo.x : a == 1 ? lo.y : lo.z, ha = a == 0 ? hi.x : a == 1 ? hi.y : hi.z;
        if (ha - la < 1e-3f) { la -= 0.01f; ha += 0.01f; }  // flat planes
        if (std::fabs(da) < 1e-9f) {
            if (oa < la || oa > ha) return false;
            continue;
        }
        f32 n = (la - oa) / da, f = (ha - oa) / da;
        if (n > f) { const f32 x = n; n = f; f = x; }
        t0 = n > t0 ? n : t0;
        t1 = f < t1 ? f : t1;
        if (t0 > t1) return false;
    }
    t = t0;
    return true;
}

// Nearest triangle along a (local) ray (Moller-Trumbore, both faces); false if none.
bool ray_triangles(Vec3 o, Vec3 d, const MeshCache::Triangles& mesh, f32& t) {
    t = 1e30f;
    for (usize i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const Vec3 a = mesh.points[mesh.indices[i]], b = mesh.points[mesh.indices[i + 1]], c = mesh.points[mesh.indices[i + 2]];
        const Vec3 e1 = b - a, e2 = c - a, p = cross(d, e2);
        const f32 det = dot(e1, p);
        if (std::fabs(det) < 1e-9f) continue;
        const f32 inv = 1.0f / det;
        const Vec3 s = o - a;
        const f32 u = dot(s, p) * inv;
        if (u < 0.0f || u > 1.0f) continue;
        const Vec3 q = cross(s, e1);
        const f32 v = dot(d, q) * inv;
        if (v < 0.0f || u + v > 1.0f) continue;
        const f32 hit = dot(e2, q) * inv;
        if (hit > 0.0f && hit < t) t = hit;
    }
    return t < 1e30f;
}

// Nearest mesh under the ray: its bounding box first, then its triangles (exact picking and
// placement: through the hole of an arch, onto a ramp's slope).
// Hidden objects are never hit; with `pickable`, locked ones are skipped too (viewport clicks).
bool raycast(EditorState& s, const Ray& ray, Entity ignore, Entity& hit, f32& hit_t, bool pickable = false) {
    const reflect::TypeInfo& type = reflect::type_of<world::StaticMesh>();
    hit_t = 1e30f;
    for (u32 i = 0; i < s.world->count(type); ++i) {
        const Entity e = s.world->owner(type, i);
        if (e == ignore || s.world->hidden_in_hierarchy(e) || (pickable && s.world->locked(e))) continue;
        const char* key = static_cast<const world::StaticMesh*>(s.world->at(type, i))->mesh;
        const CachedMesh* cached = s.meshes->get(key);
        if (!cached) continue;
        const Mat4 inv = affine_inverse(s.world->world_matrix(e));
        const Vec3 o = transform_point(inv, ray.origin), d = transform_vector(inv, ray.dir);
        f32 t;
        if (!ray_box(o, d, cached->bounds_min, cached->bounds_max, t) || t >= hit_t) continue;
        if (const MeshCache::Triangles* triangles = s.meshes->triangles(key); triangles && !triangles->indices.empty() && !ray_triangles(o, d, *triangles, t)) continue;
        if (t < hit_t) {
            hit_t = t;
            hit = e;
        }
    }
    // 2D: sprites and tile maps by their flat boxes.
    for (const reflect::TypeInfo* flat : {&reflect::type_of<world::Sprite>(), &reflect::type_of<world::TileMap>(), &reflect::type_of<world::UiPanel>()}) {
        for (u32 i = 0; i < s.world->count(*flat); ++i) {
            const Entity e = s.world->owner(*flat, i);
            if (e == ignore || s.world->hidden_in_hierarchy(e) || (pickable && s.world->locked(e))) continue;
            Vec3 lo, hi;
            if (flat == &reflect::type_of<world::Sprite>()) sprite_bounds(lo, hi);
            else if (flat == &reflect::type_of<world::UiPanel>()) ui_panel_bounds(*static_cast<const world::UiPanel*>(s.world->at(*flat, i)), lo, hi);
            else tile_map_bounds(*static_cast<const world::TileMap*>(s.world->at(*flat, i)), lo, hi);
            const Mat4 inv = affine_inverse(s.world->world_matrix(e));
            f32 t;
            if (ray_box(transform_point(inv, ray.origin), transform_vector(inv, ray.dir), lo, hi, t) && t < hit_t) {
                hit_t = t;
                hit = e;
            }
        }
    }
    const reflect::TypeInfo& skinned_type = reflect::type_of<world::SkeletalMesh>();
    for (u32 i = 0; i < s.world->count(skinned_type) && s.skinned; ++i) {  // rigged meshes by their rest-pose box
        const Entity e = s.world->owner(skinned_type, i);
        if (e == ignore || s.world->hidden_in_hierarchy(e) || (pickable && s.world->locked(e))) continue;
        const auto* component = static_cast<const world::SkeletalMesh*>(s.world->at(skinned_type, i));
        const SkinnedEntry* model = s.skinned->get(component->mesh);
        if (!model) continue;
        const Mat4 inv = affine_inverse(skeletal_matrix(*s.world, e, *component, model));
        f32 t;
        if (ray_box(transform_point(inv, ray.origin), transform_vector(inv, ray.dir), model->model->bounds_min, model->model->bounds_max, t) && t < hit_t) {
            hit_t = t;
            hit = e;
        }
    }
    return hit_t < 1e30f;
}

// World point to window pixels; false if behind the camera.
bool project(const Mat4& view_proj, Vec3 p, ImVec2 pos, ImVec2 size, ImVec2& out) {
    const f32 x = view_proj.at(0, 0) * p.x + view_proj.at(0, 1) * p.y + view_proj.at(0, 2) * p.z + view_proj.at(0, 3);
    const f32 y = view_proj.at(1, 0) * p.x + view_proj.at(1, 1) * p.y + view_proj.at(1, 2) * p.z + view_proj.at(1, 3);
    const f32 w = view_proj.at(3, 0) * p.x + view_proj.at(3, 1) * p.y + view_proj.at(3, 2) * p.z + view_proj.at(3, 3);
    if (w < 0.01f) return false;
    out = ImVec2(pos.x + (x / w * 0.5f + 0.5f) * size.x, pos.y + (0.5f - y / w * 0.5f) * size.y);
    return true;
}

// Where a drop or new object lands: on the surface under the mouse, else the floor (z = 0).
Vec3 drop_point(EditorState& s, const Ray& ray) {
    Entity hit;
    f32 t;
    if (raycast(s, ray, {}, hit, t)) return ray.origin + ray.dir * t;
    if (ray.dir.z < -1e-4f) return ray.origin + ray.dir * (-ray.origin.z / ray.dir.z);
    return ray.origin + ray.dir * 10.0f;
}

// End key: drops the selection onto what is below it (or the floor), like Unreal.
void drop_to_floor(EditorState& s) {
    Entity roots[EditorState::kMaxSelection];
    const u32 count = selection_roots(s, roots);
    for (u32 i = 0; i < count; ++i) {
        const Entity e = roots[i];
        Vec3 lo, hi;
        world_bounds(s, e, lo, hi);
        const Vec3 center{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, lo.z};
        Entity hit;
        f32 t;
        f32 floor_z = 0.0f;
        if (raycast(s, {center + Vec3{0, 0, 0.01f}, {0, 0, -1}}, e, hit, t)) floor_z = center.z + 0.01f - t;
        else if (center.z < 0.0f) continue;  // below the floor with nothing under it: leave it
        Mat4 m = s.world->world_matrix(e);
        m.at(2, 3) += floor_z - lo.z;
        s.world->set_world_matrix(e, m);
    }
    if (count) edited(s, "Drop to Floor");
}

void group_selection(EditorState& s) {
    Entity roots[EditorState::kMaxSelection];
    const u32 count = selection_roots(s, roots);
    if (count < 1) return;
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (u32 i = 0; i < count; ++i) {
        Vec3 a, b;
        world_bounds(s, roots[i], a, b);
        lo = vmin(lo, a);
        hi = vmax(hi, b);
    }
    char name[64];
    s.world->unique_name("Group", name, sizeof name);
    const Entity group = s.world->create(name);
    s.world->get<world::Transform>(group)->position = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, lo.z};
    const Entity parent = s.world->parent(roots[0]);  // the group goes where the first object was
    s.world->set_parent(group, parent);
    for (u32 i = 0; i < count; ++i) s.world->set_parent(roots[i], group);
    select(s, group);
    edited(s, "Group");
}

// Shift+G: removes selected groups (entities with only a Transform), keeping their children in place.
void ungroup_selection(EditorState& s) {
    u32 type_count = 0;
    const reflect::TypeInfo* const* types = world::component_types(type_count);
    Entity roots[EditorState::kMaxSelection];
    const u32 count = selection_roots(s, roots);
    bool any = false;
    for (u32 i = 0; i < count; ++i) {
        bool plain = true;
        for (u32 t = 1; t < type_count; ++t) plain = plain && !s.world->get(roots[i], *types[t]);
        if (!plain) continue;
        const Entity parent = s.world->parent(roots[i]);
        for (u32 slot = 0; slot < s.world->slot_count(); ++slot) {
            const Entity child = s.world->entity_at_slot(slot);
            if (child.valid() && s.world->parent(child) == roots[i]) s.world->set_parent(child, parent);
        }
        s.world->destroy(roots[i]);
        any = true;
    }
    if (any) {
        select(s, {});
        edited(s, "Ungroup");
    }
}

void focus_selected(EditorState& s) {
    if (!s.world->alive(s.selected)) return;
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (u32 i = 0; i < s.selection_count; ++i) {
        Vec3 a, b;
        world_bounds(s, s.selection[i], a, b);
        lo = vmin(lo, a);
        hi = vmax(hi, b);
    }
    const Vec3 center = (lo + hi) * 0.5f;
    const f32 radius = length(hi - lo) * 0.5f;
    if (s.camera.view != ViewType::Perspective) {
        s.camera.position = center;
        s.camera.ortho_height = radius * 2.5f + 1.0f;
        return;
    }
    s.camera.pivot_distance = radius * 2.5f + 1.0f;
    s.camera.position = center - camera_forward(s.camera) * s.camera.pivot_distance;
}

// ---- Layout ----

constexpr char kLayoutDir[] = "Saved/Layouts";


// ---- Workspaces (Blender's workspace tabs) ----

constexpr char kWorkspaceDir[] = "Saved/Layouts/Workspaces";
constexpr char kEditorAreaWindow[] = "Editors###WorkspaceEditorArea";  // holds a workspace's editor area open

// A built-in workspace: how it splits the screen around the viewport (which keeps the remaining,
// central node), and what its editor area is for. `editor_area` returns that node, 0 for none.
struct WorkspaceDef {
    const char* name;
    const char* tooltip;
    const char* hint;  // shown in the empty editor area
    bool lua_debugger;
    ImGuiID (*build)(ImGuiID root);
    bool multiplayer = false;  // shows the Multiplayer panel
};

ImGuiID dock_panels(ImGuiID right, f32 outliner_share, ImGuiID content) {
    ImGuiID top = 0, bottom = 0;
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, outliner_share, &top, &bottom);
    ImGui::DockBuilderDockWindow(kOutlinerWindow, top);
    ImGui::DockBuilderDockWindow(kDetailsWindow, bottom);
    ImGui::DockBuilderDockWindow(kContentWindow, content);
    return 0;
}

ImGuiID build_layout_workspace(ImGuiID root) {  // the original arrangement
    ImGuiID right = 0, left = 0, content = 0, viewport = 0;
    ImGui::DockBuilderSplitNode(root, ImGuiDir_Right, 0.22f, &right, &left);
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.30f, &content, &viewport);
    return dock_panels(right, 0.45f, content);
}

ImGuiID build_side_editor_workspace(ImGuiID root) {  // Modeling, Effects: the editor beside the viewport
    ImGuiID right = 0, left = 0, content = 0, rest = 0, area = 0, viewport = 0;
    ImGui::DockBuilderSplitNode(root, ImGuiDir_Right, 0.20f, &right, &left);
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.25f, &content, &rest);
    ImGui::DockBuilderSplitNode(rest, ImGuiDir_Left, 0.45f, &area, &viewport);
    dock_panels(right, 0.40f, content);
    return area;
}

ImGuiID build_shading_workspace(ImGuiID root) {  // Blender's Shading: browser left, node editor under the viewport
    ImGuiID right = 0, left = 0, content = 0, rest = 0, area = 0, viewport = 0;
    ImGui::DockBuilderSplitNode(root, ImGuiDir_Right, 0.20f, &right, &left);
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Left, 0.20f, &content, &rest);
    ImGui::DockBuilderSplitNode(rest, ImGuiDir_Down, 0.48f, &area, &viewport);
    dock_panels(right, 0.40f, content);
    return area;
}

ImGuiID build_bottom_editor_workspace(ImGuiID root) {  // Animation, UI: the editor along the bottom
    ImGuiID right = 0, left = 0, bottom = 0, viewport = 0, content = 0, area = 0;
    ImGui::DockBuilderSplitNode(root, ImGuiDir_Right, 0.20f, &right, &left);
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.42f, &bottom, &viewport);
    ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Left, 0.28f, &content, &area);
    dock_panels(right, 0.40f, content);
    return area;
}

ImGuiID build_scripting_workspace(ImGuiID root) {  // Blender's Scripting: a big editor, a small viewport
    ImGuiID right = 0, left = 0, bottom = 0, viewport = 0, content = 0, area = 0;
    ImGui::DockBuilderSplitNode(root, ImGuiDir_Right, 0.24f, &right, &left);
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.58f, &bottom, &viewport);
    ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Left, 0.26f, &content, &area);
    dock_panels(right, 0.40f, content);
    ImGui::DockBuilderDockWindow("Lua Debugger", area);
    return area;
}

ImGuiID build_particles_workspace(ImGuiID root) {  // Niagara's layout: a big Particle Editor, a smaller viewport beside it
    ImGuiID right = 0, left = 0, content = 0, rest = 0, area = 0, viewport = 0;
    ImGui::DockBuilderSplitNode(root, ImGuiDir_Right, 0.22f, &right, &left);
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.24f, &content, &rest);
    ImGui::DockBuilderSplitNode(rest, ImGuiDir_Left, 0.64f, &area, &viewport);
    dock_panels(right, 0.35f, content);
    return area;
}

constexpr char kMultiplayerWindow[] = "Multiplayer###MultiplayerPanel";

ImGuiID build_multiplayer_workspace(ImGuiID root) {  // the viewport, the Multiplayer panel under it beside the Content Browser
    ImGuiID right = 0, left = 0, bottom = 0, viewport = 0, content = 0, panel = 0;
    ImGui::DockBuilderSplitNode(root, ImGuiDir_Right, 0.20f, &right, &left);
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.40f, &bottom, &viewport);
    ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Left, 0.30f, &content, &panel);
    dock_panels(right, 0.40f, content);
    ImGui::DockBuilderDockWindow(kMultiplayerWindow, panel);
    return 0;
}

constexpr WorkspaceDef kWorkspaces[] = {
    {"Layout", "The level: viewport, Outliner, Details, Content Browser", nullptr, false, build_layout_workspace},
    {"Modeling", "Meshes and prefabs: their editors beside the viewport", "Double-click a mesh or a prefab in the Content Browser to edit it here.", false, build_side_editor_workspace},
    {"Shading", "Materials: the Material Graph under the viewport", "Double-click a Material Graph in the Content Browser to edit it here.", false, build_shading_workspace},
    {"Animation", "Timelines (Sequencer) and animation graphs along the bottom", "Double-click a Timeline or an Animation Graph to edit it here.", false, build_bottom_editor_workspace},
    {"Scripting", "Visual scripts, the Lua Debugger and a small viewport", "Double-click a visual script to edit it here (Lua opens in your code editor).", true, build_scripting_workspace},
    {"UI", "Game UI documents (HTML / CSS) along the bottom", "Double-click a UI document (.rml) to edit it here.", false, build_bottom_editor_workspace},
    {"Effects", "Particle systems (Niagara) and sound cues beside the viewport", "Double-click a particle system or a sound cue to edit it here.", false, build_side_editor_workspace},
    {"Particles", "The Particle Editor (Niagara) big, with its preview and Emitter Stack, beside a small viewport", "Double-click a Particle System to edit it here (it opens this workspace by itself).", false, build_particles_workspace},
    {"Multiplayer", "Host, join and playtest multiplayer: settings, join codes, LAN games, players and traffic", nullptr, false, build_multiplayer_workspace, true},
};

const WorkspaceDef* find_workspace_def(const EditorSettings& set, const char* name) {
    for (const WorkspaceDef& w : kWorkspaces) {
        if (std::strcmp(w.name, name) == 0) return &w;
    }
    for (const CustomWorkspace& c : set.workspaces) {  // a user's workspace has its base's editor area
        if (std::strcmp(c.name, name) == 0) return find_workspace_def(set, c.base);
    }
    return nullptr;
}

std::string workspace_file(const char* name) { return std::string(kWorkspaceDir) + "/" + name + ".ini"; }

void build_workspace(const WorkspaceDef& w, ImGuiID dockspace, ImVec2 size) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_DockSpace) | ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::DockBuilderSetNodeSize(dockspace, size);
    if (const ImGuiID area = w.build(dockspace)) ImGui::DockBuilderDockWindow(kEditorAreaWindow, area);
    ImGui::DockBuilderFinish(dockspace);
}

// Saves the active workspace's arrangement, then shows `name`'s: as it was left, or built fresh.
void switch_workspace(EditorState& s, const char* name, ImGuiID dockspace, ImVec2 size) {
    std::error_code ec;
    std::filesystem::create_directories(kWorkspaceDir, ec);
    ImGui::SaveIniSettingsToDisk(workspace_file(s.settings.workspace).c_str());
    std::snprintf(s.settings.workspace, sizeof s.settings.workspace, "%s", name);
    const WorkspaceDef* def = find_workspace_def(s.settings, name);
    s.show_debugger = def && def->lua_debugger;
    s.show_multiplayer = def && def->multiplayer;
    const std::string file = workspace_file(name);
    if (std::filesystem::exists(file, ec)) apply_layout_file(file.c_str(), s.status, sizeof s.status);
    else if (def) build_workspace(*def, dockspace, size);
}

// The tab strip under the menu bar.
void draw_workspace_tabs(EditorState& s) {
    const f32 height = ImGui::GetFrameHeight() + 6.0f;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 3.0f));
    if (ImGui::BeginViewportSideBar("##workspace_tabs", ImGui::GetMainViewport(), ImGuiDir_Up, height, flags)) {
        if (ImGui::BeginMenuBar()) {
            // The tab bar keeps its own selection: when the workspace changed elsewhere (start, console,
            // +), select its tab and ignore what the bar reports this frame.
            static char selected[32] = "";
            const bool sync = std::strcmp(selected, s.settings.workspace) != 0;
            if (ImGui::BeginTabBar("##workspaces", ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_NoTooltip)) {
                auto tab = [&](const char* name, const char* tooltip, bool custom) {
                    const bool active = std::strcmp(s.settings.workspace, name) == 0;
                    const ImGuiTabItemFlags tab_flags = active && sync ? ImGuiTabItemFlags_SetSelected : 0;
                    if (ImGui::BeginTabItem(name, nullptr, tab_flags)) {
                        if (!active && !sync && !s.workspace_request[0]) std::snprintf(s.workspace_request, sizeof s.workspace_request, "%s", name);
                        ImGui::EndTabItem();
                    }
                    if (tooltip) ImGui::SetItemTooltip("%s", tooltip);
                    if (ImGui::BeginPopupContextItem()) {
                        if (with_tip(ImGui::MenuItem("Reset to Default"), "Puts this workspace's panels back where they started")) {  // forget its saved arrangement
                            std::error_code ec;
                            std::filesystem::remove(workspace_file(name), ec);
                            if (active) s.reset_layout = true;
                        }
                        if (custom && !active && with_tip(ImGui::MenuItem("Delete"), "Removes this workspace tab")) {
                            for (u32 k = 0; k < s.settings.workspaces.count; ++k) {
                                if (std::strcmp(s.settings.workspaces[k].name, name) == 0) s.settings.workspaces.remove(k);
                            }
                            std::error_code ec;
                            std::filesystem::remove(workspace_file(name), ec);
                        }
                        ImGui::EndPopup();
                    }
                };
                for (const WorkspaceDef& w : kWorkspaces) tab(w.name, w.tooltip, false);
                for (u32 k = 0; k < s.settings.workspaces.count; ++k) tab(s.settings.workspaces[k].name, "Your workspace (right-click: Reset, Delete)", true);
                if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) ImGui::OpenPopup("New Workspace");
                ImGui::SetItemTooltip("New workspace: a copy of this one's arrangement, under a new name");
                if (ImGui::BeginPopup("New Workspace")) {
                    static char name[32] = "My Workspace";
                    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                    const bool enter = with_tip(ImGui::InputText("Name", name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue), "Name of the new workspace tab");
                    const bool taken = find_workspace_def(s.settings, name) != nullptr || std::strcmp(name, s.settings.workspace) == 0;
                    if ((with_tip(ImGui::Button("Create"), "Makes the workspace from the current arrangement of panels") || enter) && asset::valid_name(name) && !taken && s.settings.workspaces.count < s.settings.workspaces.capacity()) {
                        const WorkspaceDef* base = find_workspace_def(s.settings, s.settings.workspace);
                        CustomWorkspace c;
                        std::snprintf(c.name, sizeof c.name, "%s", name);
                        std::snprintf(c.base, sizeof c.base, "%s", base ? base->name : "Layout");
                        s.settings.workspaces.push_back(c);
                        std::error_code ec;
                        std::filesystem::create_directories(kWorkspaceDir, ec);
                        ImGui::SaveIniSettingsToDisk(workspace_file(name).c_str());  // starts as this arrangement
                        std::snprintf(s.workspace_request, sizeof s.workspace_request, "%s", name);
                        ImGui::CloseCurrentPopup();
                    }
                    if (taken) ImGui::TextDisabled("That name is taken");
                    ImGui::EndPopup();
                }
                ImGui::EndTabBar();
            }
            if (sync) std::snprintf(selected, sizeof selected, "%s", s.settings.workspace);
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

// The Multiplayer workspace's panel (Unreal: the multiplayer options of Play, Session Frontend's
// players and traffic): the project's multiplayer settings, Play as host or join a code, the host's
// join codes and the session while playing.
void draw_multiplayer_panel(EditorState& s) {
    if (!s.show_multiplayer) return;
    if (!ImGui::Begin(kMultiplayerWindow, &s.show_multiplayer)) {
        ImGui::End();
        return;
    }
    ProjectSettings& pr = s.project;
    const EditorState::NetView& v = s.net;
    if (ImGui::BeginTable("##mp", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
        ImGui::TableNextColumn();  // ---- settings
        ImGui::SeparatorText("Project (Config/project.json)");
        with_tip(ImGui::Checkbox("Multiplayer", &pr.multiplayer), "Off: a single-player game, nothing networked runs");
        ImGui::BeginDisabled(!pr.multiplayer || s.playing);
        int players = static_cast<int>(pr.play_players), port = static_cast<int>(pr.port), max_players = static_cast<int>(pr.max_players);
        ImGui::SetNextItemWidth(120.0f);
        if (with_tip(ImGui::SliderInt("Players in Play", &players, 1, 8), "Play hosts here and opens a game window for each other player")) pr.play_players = static_cast<u32>(players);
        ImGui::SetNextItemWidth(120.0f);
        if (with_tip(ImGui::InputInt("Port", &port, 0), "The UDP port the server listens on (testers' routers forward this one)")) pr.port = static_cast<u32>(std::clamp(port, 1024, 65535));
        ImGui::SetNextItemWidth(120.0f);
        if (with_tip(ImGui::SliderInt("Max Players", &max_players, 2, 64), "How many players one server takes, the host included")) pr.max_players = static_cast<u32>(max_players);
        ImGui::EndDisabled();
        if (with_tip(ImGui::Button("Save"), "Writes the Project Settings (everyone on the team gets them)"))
            std::snprintf(s.status, sizeof s.status, reflect::save_file(pr, "Config/project.json") ? "Saved %s" : "Could not save %s", "Config/project.json");
        ImGui::SameLine();
        if (with_tip(ImGui::Button("All Settings..."), "Project Settings > Multiplayer, Chat and Voice")) s.show_project_settings = true;

        ImGui::TableNextColumn();  // ---- play / join
        ImGui::SeparatorText(s.playing ? "Session" : "Play");
        if (!pr.multiplayer) {
            ImGui::TextWrapped("Tick Multiplayer to host and join.");
        } else if (!s.playing) {
            if (with_tip(ImGui::Button("Play as Host"), "Play hosts the game here (Alt+P); the other Players in Play join it")) s.play_requested = true;
            static char code[64] = "";
            static char why[96] = "";
            ImGui::SetNextItemWidth(160.0f);
            const bool enter = ImGui::InputTextWithHint("##code", "join code or address", code, sizeof code, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (with_tip(ImGui::Button("Play and Join"), "Play as a client of that game (a tester's or another PC's)") || enter) {
                char address[64];
                if (net::parse_join_code(code, static_cast<u16>(pr.port), address, sizeof address)) {
                    std::snprintf(s.net_join, sizeof s.net_join, "%s", address);
                    s.play_requested = true;
                    why[0] = '\0';
                } else {
                    std::snprintf(why, sizeof why, "Not a join code (10 letters and digits) or an address");
                }
            }
            if (why[0]) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", why);
            static net::LanGame games[16];
            static u32 game_count = 0;
            static bool searched = false;
            if (with_tip(ImGui::Button("Find LAN Games"), "Games hosting on this network (half a second)")) {
                game_count = net::NetSession::find_lan_games(static_cast<u16>(pr.port), 0.5f, games, 16);
                searched = true;
            }
            for (u32 k = 0; k < game_count; ++k) {
                ImGui::PushID(static_cast<int>(k));
                ImGui::Text("%s  %s  %u/%u", games[k].name, games[k].address, games[k].players, games[k].max_players);
                ImGui::SameLine();
                if (ImGui::SmallButton("Join")) {
                    std::snprintf(s.net_join, sizeof s.net_join, "%s", games[k].address);
                    s.play_requested = true;
                }
                ImGui::PopID();
            }
            if (searched && !game_count) ImGui::TextDisabled("No games on this network");
        } else {
            static const char* const modes[] = {"Single player", "Server (host)", "Client"};
            ImGui::Text("%s, player %u%s", modes[v.mode], v.player, v.mode == 2 && !v.connected ? " (connecting)" : "");
            if (v.mode != 0) {
                ImGui::Text("Players %u   Replicated %u", v.players, v.replicated);
                ImGui::Text("Ping %.0f ms   In %.1f KB/s   Out %.1f KB/s", v.ping_ms, v.in_kbs, v.out_kbs);
            }
        }

        ImGui::TableNextColumn();  // ---- join codes
        ImGui::SeparatorText("Join Codes");
        if (!s.playing || v.mode != 1) {
            ImGui::TextWrapped("Play as Host, then give testers a code. They paste it into Join (this panel, or the game's Multiplayer menu: Content/Multiplayer).");
        } else {
            if (with_tip(ImGui::Button(v.codes[0] ? "Refresh" : "Show Join Codes"), "Lists this PC's codes; the first time it asks the router to forward the port (UPnP)")) s.net_codes_wanted = true;
            for (const char* line = v.codes; *line;) {  // "upnp|state|message|code" then "lan|address|code"
                const char* end = std::strchr(line, '\n');
                const std::string row(line, end ? static_cast<usize>(end - line) : std::strlen(line));
                line = end ? end + 1 : line + row.size();
                const usize a = row.find('|'), b = row.find('|', a + 1), c = row.find('|', b + 1);
                if (a == std::string::npos || b == std::string::npos) continue;
                const std::string kind = row.substr(0, a);
                std::string label, code;
                if (kind == "upnp" && c != std::string::npos) {
                    const std::string st = row.substr(a + 1, b - a - 1);
                    code = row.substr(c + 1);
                    label = "Internet";
                    if (code.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.9f, 0.3f, 1.0f));
                        ImGui::TextWrapped("Internet: %s", st == "working" ? "asking the router..." : st == "failed" ? row.substr(b + 1, c - b - 1).c_str() : "-");
                        ImGui::PopStyleColor();
                        continue;
                    }
                } else if (kind == "lan") {
                    const std::string address = row.substr(a + 1, b - a - 1);
                    code = row.substr(b + 1);
                    unsigned x = 0, y = 0;
                    const bool tailscale = std::sscanf(address.c_str(), "%u.%u", &x, &y) == 2 && x == 100 && (y & 0xC0u) == 64u;
                    label = (tailscale ? "Tailscale " : "LAN ") + address;
                } else {
                    continue;
                }
                ImGui::PushID(label.c_str());
                ImGui::Text("%s:", label.c_str());
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.1f, 1.0f), "%s", code.c_str());
                ImGui::SameLine();
                if (with_tip(ImGui::SmallButton("Copy"), "Copies the code to send to testers")) ImGui::SetClipboardText(code.c_str());
                ImGui::PopID();
            }
        }
        ImGui::TextDisabled("Guide: docs/Multiplayer.md");
        ImGui::EndTable();
    }
    ImGui::End();
}

// A workspace's editor area while no asset editor is open in it (Blender shows an empty editor too).
void draw_editor_area(EditorState& s) {
    const WorkspaceDef* def = find_workspace_def(s.settings, s.settings.workspace);
    if (!def || !def->hint) return;
    if (ImGui::Begin(kEditorAreaWindow, nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", def->hint);
        ImGui::TextDisabled("Editors opened in this workspace dock here as tabs.");
    }
    ImGui::End();
}

ImGuiID g_editor_area = 0;  // this frame's editor area dock node, 0 for none

// ---- Panel tabs (Unreal's document tabs): the Level, then asset editors opened outside an editor area ----

// Back / forward (Mouse 4 / Mouse 5, like a web browser) through places the user moved between: a
// Content Browser folder, an editor tab. track() each frame records changes; go() moves and returns
// where to.
struct NavHistory {
    std::vector<std::string> back, forward;  // shortcut: std in editor-only code, a few dozen entries
    std::string now;
    bool started = false;
    void track(const std::string& place) {
        if (!started) {
            now = place;
            started = true;
        } else if (place != now) {
            back.push_back(now);
            if (back.size() > 64) back.erase(back.begin());
            forward.clear();
            now = place;
        }
    }
    bool go(bool to_back, std::string& out) {
        std::vector<std::string>& from = to_back ? back : forward;
        std::vector<std::string>& to = to_back ? forward : back;
        if (from.empty()) return false;
        to.push_back(now);
        now = from.back();
        from.pop_back();
        out = now;
        return true;
    }
};
NavHistory g_folder_history, g_tab_history;
bool g_content_hovered = false;  // the mouse is over the Content Browser (its back / forward win)

constexpr char kLevelTab[] = "###LevelTab";
struct PanelTab {
    std::string id;     // the window's ### part (stable while its title shows a name or a *)
    std::string label;  // the window's title
    bool seen;          // its editor drew this frame; the tab goes when it stops
};
std::vector<PanelTab> g_tabs;  // shortcut: std in editor-only code, a handful of tabs
std::string g_active_tab = kLevelTab;
std::string g_select_tab;  // select this tab in the bar (it keeps its own selection)
std::string g_close_tab;   // closed with its x: the editor sees *open turn false on its next draw
std::string g_focused_tab;  // the editor tab last given focus

std::string window_id(const char* title) {
    const char* hash = std::strstr(title, "###");
    return hash ? hash : title;
}

void draw_panel_tabs(EditorState& s) {
    std::erase_if(g_tabs, [](const PanelTab& t) { return !t.seen; });
    for (PanelTab& t : g_tabs) t.seen = false;
    const bool active_alive = std::any_of(g_tabs.begin(), g_tabs.end(), [](const PanelTab& t) { return t.id == g_active_tab; });
    if (g_active_tab != kLevelTab && !active_alive) g_select_tab = kLevelTab;  // its editor closed
    if (s.playing && !s.ejected && g_active_tab != kLevelTab) g_select_tab = kLevelTab;  // Play shows the level
    const f32 height = ImGui::GetFrameHeight() + 6.0f;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 3.0f));
    if (ImGui::BeginViewportSideBar("##panel_tabs", ImGui::GetMainViewport(), ImGuiDir_Up, height, flags)) {
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginTabBar("##panels", ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_Reorderable)) {
                auto tab = [&](const std::string& id, const char* label, bool* open) {
                    const ImGuiTabItemFlags tab_flags = g_select_tab == id ? ImGuiTabItemFlags_SetSelected : 0;
                    if (ImGui::BeginTabItem(label, open, tab_flags)) {
                        if (g_select_tab.empty()) g_active_tab = id;
                        ImGui::EndTabItem();
                    }
                };
                char level_label[64];  // the mode shows in the tab; the space at the end is for the arrow
                std::snprintf(level_label, sizeof level_label, "%s%s     ###LevelTab", s.mode == EditorMode::Select ? "Level" : "Level: ", s.mode == EditorMode::Select ? "" : mode_name(s.mode));
                tab(kLevelTab, level_label, nullptr);
                const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
                {  // a small down arrow: the tab opens the mode list
                    const f32 cx = hi.x - 14.0f, cy = (lo.y + hi.y) * 0.5f;
                    ImGui::GetWindowDrawList()->AddTriangleFilled(ImVec2(cx - 4.0f, cy - 2.0f), ImVec2(cx + 4.0f, cy - 2.0f), ImVec2(cx, cy + 3.0f), ImGui::GetColorU32(ImGuiCol_Text));
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && g_active_tab == kLevelTab) {
                    ImGui::SetNextWindowPos(ImVec2(lo.x, hi.y));
                    ImGui::OpenPopup("##level_modes");
                }
                ImGui::SetItemTooltip("The level and its modes (click again for the list): Select, 3D Modeling, Foliage, Landscape, Animation Sequence, PCG, VFX");
                if (ImGui::BeginPopup("##level_modes")) {
                    draw_mode_menu(s);
                    ImGui::EndPopup();
                }
                for (const PanelTab& t : g_tabs) {
                    bool open = true;
                    tab(t.id, t.label.c_str(), &open);
                    if (!open) g_close_tab = t.id;
                }
                ImGui::EndTabBar();
            }
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    if (!g_select_tab.empty()) {
        g_active_tab = g_select_tab;
        g_select_tab.clear();
    }
    if (g_active_tab == kLevelTab) g_focused_tab.clear();
    // Mouse 4 / Mouse 5 anywhere but the Content Browser: back / forward through the tabs (closed ones skipped).
    g_tab_history.track(g_active_tab);
    const bool back = ImGui::IsMouseClicked(3), forward = ImGui::IsMouseClicked(4);
    if ((back || forward) && !g_content_hovered && !(s.playing && !s.ejected)) {
        std::string to;
        while (g_tab_history.go(back, to)) {
            const bool alive = to == kLevelTab || std::any_of(g_tabs.begin(), g_tabs.end(), [&](const PanelTab& t) { return t.id == to; });
            if (alive) {
                g_select_tab = to;
                break;
            }
        }
    }
}

// Panels draw their own themed tab bars, so the dock nodes hide theirs.
bool begin_panel(const char* name) {
    ImGuiWindowClass window_class;
    window_class.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
    ImGui::SetNextWindowClass(&window_class);
    return ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoCollapse);
}

// One themed tab. Text turns dark on the active tab so it stays readable on bright colours.
bool themed_tab(const char* label, int index, int& active, const LinearColor& on, const LinearColor& off, const LinearColor& on_text, bool select = false) {
    const bool is_active = active == index;
    ImGui::PushStyleColor(ImGuiCol_Tab, vec4(off));
    ImGui::PushStyleColor(ImGuiCol_TabHovered, vec4(on));
    ImGui::PushStyleColor(ImGuiCol_TabSelected, vec4(on));
    ImGui::PushStyleColor(ImGuiCol_TabDimmed, vec4(off));
    ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, vec4(on));
    if (is_active) ImGui::PushStyleColor(ImGuiCol_Text, vec4(on_text));
    const bool open = ImGui::BeginTabItem(label, nullptr, select ? ImGuiTabItemFlags_SetSelected : 0);
    if (is_active) ImGui::PopStyleColor();
    ImGui::PopStyleColor(5);
    if (open) active = index;
    return open;
}

bool toggle_button(const char* label, bool on) {
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    const bool clicked = ImGui::Button(label);
    if (on) ImGui::PopStyleColor();
    return clicked;
}

const char* place_tip(const char* label) {  // what each Place item is, for its tooltip
    if (std::strcmp(label, "Cube") == 0) return "A 1 m box: blockout, walls, platforms";
    if (std::strcmp(label, "Sphere") == 0) return "A 1 m ball";
    if (std::strcmp(label, "Plane") == 0) return "A flat 1 m square: floors, ground";
    if (std::strcmp(label, "Player Start") == 0) return "Where the player appears when Play starts (without a Character)";
    if (std::strcmp(label, "Character") == 0) return "A walking player: capsule, WASD / mouse controls, jump, crouch; add a Skeletal Mesh and Camera as children";
    if (std::strcmp(label, "Blocking Volume") == 0) return "An invisible wall the player and physics cannot pass";
    if (std::strcmp(label, "Kill Volume") == 0) return "The player dies (respawns) on entering it: pits, lava";
    if (std::strcmp(label, "Trigger Volume") == 0) return "Sends overlap events to scripts when something enters or leaves it";
    if (std::strcmp(label, "Spline") == 0) return "A curve you shape with points: paths, rails, routes for movers and AI";
    if (std::strcmp(label, "Sound Emitter") == 0) return "Plays a sound from this spot, louder when near";
    if (std::strcmp(label, "Particle Effect") == 0) return "Plays a Particle System here: fire, smoke, sparks";
    if (std::strcmp(label, "Sky") == 0) return "The sky gradient and sun disc (follows the Directional Light)";
    if (std::strcmp(label, "Height Fog") == 0) return "Fog that thickens lower down and with distance";
    if (std::strcmp(label, "Post Process Volume") == 0) return "The look inside its box (or everywhere): exposure, bloom, colour grading, depth of field";
    if (std::strcmp(label, "Lightmass Importance Volume") == 0) return "Where Build Lighting bakes light probes in detail";
    if (std::strcmp(label, "Decal") == 0) return "Projects an image onto surfaces in its box: stains, signs, cracks";
    if (std::strcmp(label, "Nav Mesh Bounds") == 0) return "Where AI navigation is built (press P to see it)";
    if (std::strcmp(label, "Nav Modifier Volume") == 0) return "AI paths go round its box";
    if (std::strcmp(label, "Cull Distance Volume") == 0) return "Hides small meshes inside it when far away (faster frames)";
    if (std::strcmp(label, "Water Volume") == 0) return "Water the Character swims in";
    if (std::strcmp(label, "Water Body") == 0) return "A lake, ocean or river with waves (Kind in Details); swum in and floated on in Play";
    if (std::strcmp(label, "Nav Agent") == 0) return "An AI walker that paths around the level (Move To, Behavior Trees)";
    if (std::strcmp(label, "AI Controller") == 0) return "An AI with a Behavior Tree, sight and hearing";
    if (std::strcmp(label, "Audio Volume") == 0) return "Reverb inside its box: rooms, caves, halls";
    if (std::strcmp(label, "Directional Light") == 0) return "Sunlight: lights everything from one direction, casts shadows; turn it to set the time of day";
    if (std::strcmp(label, "Point Light") == 0) return "A bulb: light in every direction from a point";
    if (std::strcmp(label, "Spot Light") == 0) return "A cone of light: torches, street lamps, stage lights";
    if (std::strcmp(label, "Sky Light") == 0) return "Soft ambient light from the sky and ground";
    if (std::strcmp(label, "Camera") == 0) return "A camera to look through: cutscenes, fixed views, or the player's view as a Character's child";
    if (std::strcmp(label, "Empty") == 0) return "An object with only a transform: a parent to group others, a script holder, a marker";
    if (std::strcmp(label, "Sub Level") == 0) return "Places another level here; it can stream in and out by distance";
    if (std::strcmp(label, "Drivable Vehicle") == 0) return "A car the player enters (E) and drives with WASD, Shift boost, Ctrl brake";
    if (std::strcmp(label, "Transport (Ride)") == 0) return "A train, lift or boat the player rides along its route without driving";
    if (std::strcmp(label, "Moving Mesh (Mover)") == 0) return "A mesh that moves by itself: a direction, back and forth, or along a spline; on a trigger or a timer";
    return "";
}

// ---- Viewport ----

} // namespace

void set_view(EditorState& s, ViewType view) {
    EditorCamera& cam = s.camera;
    if (cam.view == view) return;
    if (cam.view == ViewType::Perspective) {
        s.perspective_camera = cam;
        const Vec3 pivot = cam.position + camera_forward(cam) * cam.pivot_distance;
        cam.view = view;
        cam.position = pivot;
        cam.ortho_height = cam.pivot_distance * 1.2f;
    } else if (view == ViewType::Perspective) {
        cam = s.perspective_camera;
    } else {
        cam.view = view;
    }
}

namespace {

void update_camera(EditorState& s, bool hovered, int index) {
    ImGuiIO& io = ImGui::GetIO();
    EditorCamera& cam = s.camera;
    static bool looking_by_view[4], panning_by_view[4], orbiting_by_view[4];  // a drag stays in the viewport it started in
    bool& looking = looking_by_view[index];
    bool& panning = panning_by_view[index];
    bool& orbiting = orbiting_by_view[index];
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) looking = true;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) panning = true;
    if (hovered && io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !g_gizmo_busy[index]) orbiting = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) looking = false;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) panning = false;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) orbiting = false;

    const Mat4 view = camera_view(cam);
    const Vec3 right{view.at(0, 0), view.at(0, 1), view.at(0, 2)};
    const Vec3 up{view.at(1, 0), view.at(1, 1), view.at(1, 2)};
    if (cam.view != ViewType::Perspective) {  // ortho: RMB / MMB drag pans, wheel zooms
        if (looking || panning) {
            const f32 height = s.viewport_rects[index][3];
            const f32 k = cam.ortho_height / (height > 1.0f ? height : 1.0f);
            cam.position = cam.position - right * (io.MouseDelta.x * k) + up * (io.MouseDelta.y * k);
        } else if (hovered && io.MouseWheel != 0.0f) {
            cam.ortho_height *= std::pow(0.88f, io.MouseWheel);
            cam.ortho_height = cam.ortho_height < 0.5f ? 0.5f : cam.ortho_height > 2000.0f ? 2000.0f : cam.ortho_height;
        }
        return;
    }
    auto clamp_pitch = [&] { cam.pitch = cam.pitch < -1.55f ? -1.55f : cam.pitch > 1.55f ? 1.55f : cam.pitch; };

    if (looking) {  // RMB: look around; WASD / QE fly; wheel changes speed (Unreal controls)
        cam.yaw -= io.MouseDelta.x * 0.005f;
        cam.pitch -= io.MouseDelta.y * 0.005f;
        clamp_pitch();
        const Vec3 fwd = camera_forward(cam);
        Vec3 move = fwd * static_cast<f32>(ImGui::IsKeyDown(ImGuiKey_W) - ImGui::IsKeyDown(ImGuiKey_S)) +
                    right * static_cast<f32>(ImGui::IsKeyDown(ImGuiKey_D) - ImGui::IsKeyDown(ImGuiKey_A)) +
                    Vec3{0, 0, 1} * static_cast<f32>(ImGui::IsKeyDown(ImGuiKey_E) - ImGui::IsKeyDown(ImGuiKey_Q));
        f32& speed = s.settings.camera_speed;
        if (io.MouseWheel != 0.0f) speed = speed * std::pow(1.25f, io.MouseWheel);
        speed = speed < 0.1f ? 0.1f : speed > 100.0f ? 100.0f : speed;
        if (length(move) > 0.0f) cam.position = cam.position + normalize(move) * (speed * (io.KeyShift ? 3.0f : 1.0f) * io.DeltaTime);
    } else if (panning) {
        const f32 k = cam.pivot_distance * 0.0016f;
        cam.position = cam.position - right * (io.MouseDelta.x * k) + up * (io.MouseDelta.y * k);
    } else if (orbiting) {
        const Vec3 pivot = cam.position + camera_forward(cam) * cam.pivot_distance;
        cam.yaw -= io.MouseDelta.x * 0.005f;
        cam.pitch -= io.MouseDelta.y * 0.005f;
        clamp_pitch();
        cam.position = pivot - camera_forward(cam) * cam.pivot_distance;
    } else if (hovered && io.MouseWheel != 0.0f) {  // dolly toward the pivot
        const f32 step = (cam.pivot_distance > 5.0f ? cam.pivot_distance : 5.0f) * 0.12f * io.MouseWheel;
        cam.position = cam.position + camera_forward(cam) * step;
        cam.pivot_distance = cam.pivot_distance - step > 0.5f ? cam.pivot_distance - step : 0.5f;
    }
}

// Gizmo for the selection (ImGuizmo). Alt+drag duplicates first, like Unreal.
// Returns true while the mouse is over or dragging this viewport's gizmo.
// A Spline's points in world space.
u32 spline_world_points(const world::World& w, Entity e, Vec3* out) {
    Vec3 tangents[32];
    bool closed = false;
    return world::spline_world_curve(w, e, out, tangents, closed);
}

// Where the tangent handle of point `k` sits (world): a third of its tangent ahead, like a Bezier control point.
Vec3 spline_handle(const world::World& w, Entity e, u32 k) {
    Vec3 points[32], tangents[32];
    bool closed = false;
    const u32 n = world::spline_world_curve(w, e, points, tangents, closed);
    if (k >= n) return {};
    return points[k] + world::spline_tangent(points, n, closed, static_cast<f32>(k), tangents) * (1.0f / 3.0f);
}

// Is the mouse on the selected point's tangent handle?
bool spline_handle_at(EditorState& s, const Mat4& view_proj, ImVec2 pos, ImVec2 size, ImVec2 mouse) {
    if (s.spline_point < 0 || !s.world->alive(s.selected) || !s.world->get<world::Spline>(s.selected)) return false;
    ImVec2 p;
    return project(view_proj, spline_handle(*s.world, s.selected, static_cast<u32>(s.spline_point)), pos, size, p) &&
           (p.x - mouse.x) * (p.x - mouse.x) + (p.y - mouse.y) * (p.y - mouse.y) < 64.0f;
}

// The selected Spline's point under the mouse, or -1.
i32 spline_point_at(EditorState& s, const Mat4& view_proj, ImVec2 pos, ImVec2 size, ImVec2 mouse) {
    Vec3 points[32];
    const u32 n = s.world->alive(s.selected) ? spline_world_points(*s.world, s.selected, points) : 0;
    for (u32 i = 0; i < n; ++i) {
        ImVec2 p;
        if (project(view_proj, points[i], pos, size, p) && (p.x - mouse.x) * (p.x - mouse.x) + (p.y - mouse.y) * (p.y - mouse.y) < 81.0f) return static_cast<i32>(i);
    }
    return -1;
}

// Moves the selected Spline point with a move gizmo; true while the gizmo has the mouse.
bool draw_spline_point_gizmo(EditorState& s, const Mat4& view, const Mat4& proj, ImVec2 pos, ImVec2 size, int index) {
    auto* spline = s.world->get<world::Spline>(s.selected);
    if (!spline || s.spline_point < 0 || static_cast<u32>(s.spline_point) >= spline->points.count) {
        s.spline_point = -1;
        return false;
    }
    const Mat4 world = s.world->world_matrix(s.selected);
    const u32 k = static_cast<u32>(s.spline_point);
    // Alt-drag the point: a copy is added after it and dragged instead (Unreal's Alt-drag to add points).
    if (!s.spline_handle && ImGui::GetIO().KeyAlt && ImGuizmo::IsOver() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && spline->points.count < 32) {
        const Vec3 copy = spline->points[k];
        spline->points.push_back(copy);
        for (u32 i = spline->points.count - 1; i > k + 1; --i) spline->points[i] = spline->points[i - 1];
        spline->points[k + 1] = copy;
        if (spline->tangents.count > k + 1) {  // keep tangents lined up with their points
            spline->tangents.push_back(Vec3{});
            for (u32 i = spline->tangents.count - 1; i > k + 1; --i) spline->tangents[i] = spline->tangents[i - 1];
            spline->tangents[k + 1] = Vec3{};
        }
        s.spline_point = static_cast<i32>(k + 1);
        edited(s, "Add Spline Point");
        return true;
    }
    Mat4 m = translation(s.spline_handle ? spline_handle(*s.world, s.selected, k) : transform_point(world, spline->points[k]));
    const f32 snap[3] = {s.settings.snap_move, s.settings.snap_move, s.settings.snap_move};
    ImGuizmo::SetOrthographic(s.camera.view != ViewType::Perspective);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(pos.x, pos.y, size.x, size.y);
    ImGui::PushID(index + 100);
    if (ImGuizmo::Manipulate(view.m, proj.m, ImGuizmo::TRANSLATE, ImGuizmo::WORLD, m.m, nullptr, s.settings.snap && !s.spline_handle ? snap : nullptr)) {
        const Vec3 local = transform_point(affine_inverse(world), {m.at(0, 3), m.at(1, 3), m.at(2, 3)});
        if (s.spline_handle) {  // the handle is a third of the tangent ahead of its point
            while (spline->tangents.count <= k) spline->tangents.push_back(Vec3{});
            spline->tangents[k] = (local - spline->points[k]) * 3.0f;
            edited(s, "Move Spline Tangent");
        } else {
            spline->points[k] = local;
            edited(s, "Move Spline Point");
        }
    }
    const bool busy = ImGuizmo::IsUsing() || ImGuizmo::IsOver();
    ImGui::PopID();
    return busy;
}

// Is the mouse on the selected object's Physics Joint pivot marker?
bool joint_pivot_at(EditorState& s, const Mat4& view_proj, ImVec2 pos, ImVec2 size, ImVec2 mouse) {
    const auto* joint = s.world->alive(s.selected) ? s.world->get<world::PhysicsJoint>(s.selected) : nullptr;
    ImVec2 p;
    return joint && project(view_proj, transform_point(s.world->world_matrix(s.selected), joint->pivot), pos, size, p) &&
           (p.x - mouse.x) * (p.x - mouse.x) + (p.y - mouse.y) * (p.y - mouse.y) < 81.0f;
}

// Moves the selected object's Physics Joint pivot with a move gizmo (Unreal's constraint frame widget).
bool draw_joint_pivot_gizmo(EditorState& s, const Mat4& view, const Mat4& proj, ImVec2 pos, ImVec2 size, int index) {
    auto* joint = s.world->get<world::PhysicsJoint>(s.selected);
    if (!joint) {
        s.joint_pivot = false;
        return false;
    }
    const Mat4 world = s.world->world_matrix(s.selected);
    Mat4 m = translation(transform_point(world, joint->pivot));
    const f32 snap[3] = {s.settings.snap_move, s.settings.snap_move, s.settings.snap_move};
    ImGuizmo::SetOrthographic(s.camera.view != ViewType::Perspective);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(pos.x, pos.y, size.x, size.y);
    ImGui::PushID(index + 200);
    if (ImGuizmo::Manipulate(view.m, proj.m, ImGuizmo::TRANSLATE, ImGuizmo::WORLD, m.m, nullptr, s.settings.snap ? snap : nullptr)) {
        joint->pivot = transform_point(affine_inverse(world), {m.at(0, 3), m.at(1, 3), m.at(2, 3)});
        edited(s, "Move Joint Pivot");
    }
    const bool busy = ImGuizmo::IsUsing() || ImGuizmo::IsOver();
    ImGui::PopID();
    return busy;
}

bool draw_gizmo(EditorState& s, const Mat4& camera_view_matrix, const Mat4& proj, ImVec2 pos, ImVec2 size, int index) {
    Mat4 view = camera_view_matrix;
    if (g_shift_follow[index]) {
        EditorCamera start = s.camera;
        start.position = g_follow_camera[index];
        view = camera_view(start);
    }
    const bool editable = s.world->alive(s.selected) && !s.world->locked(s.selected) && !s.game_view && !(s.playing && !s.ejected);
    if (s.spline_point >= 0 && editable) return draw_spline_point_gizmo(s, view, proj, pos, size, index);
    if (s.joint_pivot && editable) return draw_joint_pivot_gizmo(s, view, proj, pos, size, index);
    bool& was_using = g_was_using_gizmo[index];
    bool& alt_duplicated = g_alt_duplicated[index];
    if (!s.world->alive(s.selected) || s.world->locked(s.selected) || s.game_view || (s.playing && !s.ejected)) {
        was_using = false;
        return false;
    }
    ImGuizmo::SetOrthographic(s.camera.view != ViewType::Perspective);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(pos.x, pos.y, size.x, size.y);
    const ImGuizmo::OPERATION op = s.gizmo == GizmoMode::Translate ? ImGuizmo::TRANSLATE
                                 : s.gizmo == GizmoMode::Rotate    ? ImGuizmo::ROTATE
                                                                   : ImGuizmo::SCALE;
    const f32 step = s.gizmo == GizmoMode::Translate ? s.settings.snap_move : s.gizmo == GizmoMode::Rotate ? s.settings.snap_angle : s.settings.snap_scale;
    const f32 snap[3] = {step, step, step};
    // The gizmo sits on the pivot: the object's origin plus the pivot offset (Unreal's pivot; Alt+V or the Pivot button moves it).
    const Mat4 object = s.world->world_matrix(s.selected);
    const Mat4 before = object * translation(s.pivot_offset);
    Mat4 m = before;
    const bool moving_pivot = s.pivot_edit;
    ImGui::PushID(index);  // one gizmo per viewport, each with its own drag state
    if (moving_pivot) {  // only the pivot moves: translate, nothing else changes
        const bool moved = ImGuizmo::Manipulate(view.m, proj.m, ImGuizmo::TRANSLATE, ImGuizmo::WORLD, m.m, nullptr, s.settings.snap ? snap : nullptr);
        if (moved) s.pivot_offset = transform_point(affine_inverse(object), {m.at(0, 3), m.at(1, 3), m.at(2, 3)});
        const bool busy = ImGuizmo::IsUsing() || ImGuizmo::IsOver();
        ImGui::PopID();
        was_using = false;
        return busy;
    }
    const bool changed = ImGuizmo::Manipulate(view.m, proj.m, op, s.gizmo_local ? ImGuizmo::LOCAL : ImGuizmo::WORLD, m.m, nullptr,
                                              s.settings.snap ? snap : nullptr);
    const bool using_now = ImGuizmo::IsUsing();
    const bool over = ImGuizmo::IsOver();
    ImGui::PopID();
    if (using_now && !was_using) alt_duplicated = false;
    if (changed && s.gizmo == GizmoMode::Translate) {
        const Mat4 view_proj = proj * view;
        ImVec2 at;
        Vec3 target{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
        bool snapped = false;
        if (ImGui::IsKeyDown(ImGuiKey_V) && project(view_proj, target, pos, size, at)) {
            // Vertex snap (Unreal's V): the pivot jumps to the nearest corner of another mesh on screen.
            f32 best = 24.0f * 24.0f;
            const reflect::TypeInfo& type = reflect::type_of<world::StaticMesh>();
            for (u32 i = 0; i < s.world->count(type); ++i) {
                const Entity e = s.world->owner(type, i);
                if (s.is_selected(e) || s.world->hidden_in_hierarchy(e)) continue;
                const MeshCache::Triangles* mesh = s.meshes->triangles(static_cast<const world::StaticMesh*>(s.world->at(type, i))->mesh);
                if (!mesh) continue;
                const Mat4 w = s.world->world_matrix(e);
                for (const Vec3& local : mesh->points) {
                    const Vec3 p = transform_point(w, local);
                    ImVec2 q;
                    if (!project(view_proj, p, pos, size, q)) continue;
                    const f32 d = (q.x - at.x) * (q.x - at.x) + (q.y - at.y) * (q.y - at.y);
                    if (d < best) {
                        best = d;
                        target = p;
                        snapped = true;
                    }
                }
            }
        } else if (s.settings.snap_to_surface && project(view_proj, target, pos, size, at)) {
            // Surface snap: the pivot follows the surface under it, seen from the camera.
            Entity hit;
            f32 t;
            const Ray ray = mouse_ray(s.camera, pos, size, at);
            if (raycast(s, ray, s.selected, hit, t) && !s.is_selected(hit)) {
                target = ray.origin + ray.dir * t;
                snapped = true;
            }
        }
        if (snapped) {
            m.at(0, 3) = target.x;
            m.at(1, 3) = target.y;
            m.at(2, 3) = target.z;
        }
    }
    if (changed) {
        if (ImGui::GetIO().KeyAlt && !alt_duplicated) {
            duplicate_selected(s);  // copies are made from the untouched originals; they get the move
            alt_duplicated = true;
        }
        // Every selected object gets the same change, about the pivot.
        const Mat4 delta = m * affine_inverse(before);
        if (ImGui::GetIO().KeyShift && s.gizmo == GizmoMode::Translate) {  // Shift+drag: the camera comes along (Unreal)
            if (!g_shift_follow[index]) {
                g_shift_follow[index] = true;
                g_follow_camera[index] = s.camera.position;
                g_follow_pivot[index] = {before.at(0, 3), before.at(1, 3), before.at(2, 3)};
            }
            s.camera.position = g_follow_camera[index] + Vec3{m.at(0, 3), m.at(1, 3), m.at(2, 3)} - g_follow_pivot[index];
        }
        Entity roots[EditorState::kMaxSelection];
        const u32 count = selection_roots(s, roots);
        for (u32 i = 0; i < count; ++i) {
            const world::Transform moved_before = *s.world->get<world::Transform>(roots[i]);
            if (roots[i] != s.selected) s.world->set_world_matrix(roots[i], delta * s.world->world_matrix(roots[i]));
            else s.world->set_world_matrix(s.selected, m * translation(-s.pivot_offset));  // the pivot stays where the gizmo left it
            note_changed_overrides(s, roots[i], reflect::type_of<world::Transform>(), reinterpret_cast<const u8*>(&moved_before),
                                   reinterpret_cast<const u8*>(s.world->get<world::Transform>(roots[i])));
        }
        edited(s, alt_duplicated ? "Duplicate" : s.gizmo == GizmoMode::Translate ? "Move" : s.gizmo == GizmoMode::Rotate ? "Rotate" : "Scale");
    }
    if (!using_now || !ImGui::GetIO().KeyShift) g_shift_follow[index] = false;
    was_using = using_now;
    return using_now || over;
}

// A small glyph telling what an object without a mesh is (Unreal's sprite icons), centred on `at`.
void draw_type_icon(const world::World& w, Entity e, ImDrawList* dl, ImVec2 at, ImU32 color) {
    const f32 x = at.x, y = at.y;
    if (w.get<world::PointLight>(e) || w.get<world::SpotLight>(e) || w.get<world::RectLight>(e) || w.get<world::DirectionalLight>(e) || w.get<world::SkyLight>(e)) {
        dl->AddCircleFilled(at, 4.0f, color);  // a sun: disc and rays
        for (int k = 0; k < 8; ++k) {
            const f32 a = static_cast<f32>(k) * 0.785398f;
            dl->AddLine(ImVec2(x + std::cos(a) * 6.5f, y + std::sin(a) * 6.5f), ImVec2(x + std::cos(a) * 10.0f, y + std::sin(a) * 10.0f), color, 1.5f);
        }
    } else if (w.get<world::Camera>(e)) {  // a film camera: body and lens
        dl->AddRect(ImVec2(x - 8, y - 5), ImVec2(x + 3, y + 5), color, 1.0f, 0, 2.0f);
        dl->AddTriangle(ImVec2(x + 3, y), ImVec2(x + 9, y - 5), ImVec2(x + 9, y + 5), color, 2.0f);
    } else if (w.get<world::SoundEmitter>(e)) {  // a speaker with sound waves
        dl->AddRectFilled(ImVec2(x - 8, y - 3), ImVec2(x - 4, y + 3), color);
        dl->AddTriangleFilled(ImVec2(x - 4, y - 3), ImVec2(x + 1, y - 7), ImVec2(x + 1, y + 7), color);
        dl->PathArcTo(at, 6.0f, -0.7f, 0.7f, 6);
        dl->PathStroke(color, 0, 1.5f);
        dl->PathArcTo(at, 10.0f, -0.7f, 0.7f, 8);
        dl->PathStroke(color, 0, 1.5f);
    } else if (w.get<world::ParticleEffect>(e)) {  // sparks
        dl->AddCircleFilled(ImVec2(x - 4, y + 3), 2.5f, color);
        dl->AddCircleFilled(ImVec2(x + 4, y + 2), 2.0f, color);
        dl->AddCircleFilled(ImVec2(x, y - 5), 3.0f, color);
    } else {
        dl->AddCircle(at, 7.0f, color, 12, 2.0f);  // an empty
    }
}

// Icons for entities without a mesh (Player Start, empties), drawn over the 3D view.
void draw_icons(EditorState& s, ImDrawList* dl, const Mat4& view_proj, ImVec2 pos, ImVec2 size) {
    const EditorTheme& t = s.theme;
    // Splines: the curve, and the points of the selected one (click a point to move it).
    const reflect::TypeInfo& spline_type = reflect::type_of<world::Spline>();
    for (u32 i = 0; i < s.world->count(spline_type); ++i) {
        const Entity e = s.world->owner(spline_type, i);
        if (s.world->hidden_in_hierarchy(e)) continue;
        const auto* spline = static_cast<const world::Spline*>(s.world->at(spline_type, i));
        Vec3 points[32], tangents[32];
        bool closed = false;
        const u32 n = world::spline_world_curve(*s.world, e, points, tangents, closed);
        const bool selected = s.is_selected(e);
        const ImU32 color = selected ? u32_color(t.details_tab_active) : IM_COL32(120, 200, 255, 220);
        const u32 samples = world::spline_segments(n, spline->closed) * 12;
        ImVec2 previous;
        bool have_previous = false;
        for (u32 k = 0; k <= samples; ++k) {
            ImVec2 p;
            const bool visible = project(view_proj, world::spline_point(points, n, spline->closed, static_cast<f32>(k) / 12.0f, tangents), pos, size, p);
            if (visible && have_previous) dl->AddLine(previous, p, color, 2.0f);
            previous = p;
            have_previous = visible;
        }
        if (!selected) continue;
        for (u32 k = 0; k < n; ++k) {
            ImVec2 p;
            if (!project(view_proj, points[k], pos, size, p)) continue;
            const bool active = s.selected == e && s.spline_point == static_cast<i32>(k);
            if (active) dl->AddRectFilled(ImVec2(p.x - 6, p.y - 6), ImVec2(p.x + 6, p.y + 6), IM_COL32(255, 255, 255, 255));
            dl->AddRect(ImVec2(p.x - 6, p.y - 6), ImVec2(p.x + 6, p.y + 6), color, 0.0f, 0, 2.0f);
            ImVec2 h;  // the selected point's tangent handle: a line out to a dot
            if (active && project(view_proj, spline_handle(*s.world, e, k), pos, size, h)) {
                dl->AddLine(p, h, IM_COL32(255, 255, 255, 200), 1.5f);
                dl->AddCircleFilled(h, 5.0f, s.spline_handle ? IM_COL32(255, 255, 255, 255) : color);
            }
        }
    }
    // Lights: directional and spot lights show where they point; a selected point / spot light its range.
    for (const reflect::TypeInfo* type : {&reflect::type_of<world::DirectionalLight>(), &reflect::type_of<world::SpotLight>(), &reflect::type_of<world::PointLight>()}) {
        for (u32 i = 0; i < s.world->count(*type); ++i) {
            const Entity e = s.world->owner(*type, i);
            if (s.world->hidden_in_hierarchy(e)) continue;
            const Mat4 m = s.world->world_matrix(e);
            const Vec3 origin{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
            Vec3 forward{m.at(0, 0), m.at(1, 0), m.at(2, 0)};
            const f32 len = length(forward);
            forward = len > 0.0f ? forward * (1.0f / len) : Vec3{1, 0, 0};
            ImVec2 a, b;
            const ImU32 color = s.is_selected(e) ? u32_color(t.details_tab_active) : IM_COL32(255, 220, 90, 230);
            if (!project(view_proj, origin, pos, size, a)) continue;
            if (type != &reflect::type_of<world::PointLight>() && project(view_proj, origin + forward * 1.5f, pos, size, b)) {
                dl->AddLine(a, b, color, 2.0f);
                dl->AddCircleFilled(b, 3.0f, color);
            }
            if (s.is_selected(e) && type != &reflect::type_of<world::DirectionalLight>()) {  // attenuation radius as a ring in the view plane
                const f32 radius = type == &reflect::type_of<world::PointLight>() ? s.world->get<world::PointLight>(e)->attenuation_radius
                                                                                 : s.world->get<world::SpotLight>(e)->attenuation_radius;
                const Vec3 side = cross(camera_forward(s.camera), Vec3{0, 0, 1});
                const f32 side_len = length(side);
                if (project(view_proj, origin + (side_len > 0.01f ? side * (radius / side_len) : Vec3{radius, 0, 0}), pos, size, b)) {
                    const f32 r = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
                    dl->AddCircle(a, r, IM_COL32(255, 220, 90, 120), 48, 1.0f);
                }
            }
        }
    }
    // Rect Lights: their panel (or disc) outlined, with a short line out of the lit side.
    const reflect::TypeInfo& rect_type = reflect::type_of<world::RectLight>();
    for (u32 i = 0; i < s.world->count(rect_type); ++i) {
        const Entity e = s.world->owner(rect_type, i);
        if (s.world->hidden_in_hierarchy(e)) continue;
        const auto* light = static_cast<const world::RectLight*>(s.world->at(rect_type, i));
        const Mat4 m = s.world->world_matrix(e);
        const ImU32 color = s.is_selected(e) ? u32_color(t.details_tab_active) : IM_COL32(255, 220, 90, 230);
        const f32 hw = light->source_width * 0.5f, hh = (light->shape == 1 ? light->source_width : light->source_height) * 0.5f;
        constexpr int kSides = 24;
        const int sides = light->shape == 1 ? kSides : 4;
        ImVec2 ring[kSides + 1];
        bool ok = true;
        for (int k = 0; k <= sides && ok; ++k) {
            const f32 a = static_cast<f32>(k) / static_cast<f32>(sides) * 6.2831853f + (sides == 4 ? 0.785398f : 0.0f);
            const f32 u = sides == 4 ? (std::cos(a) > 0 ? hw : -hw) : std::cos(a) * hw, v = sides == 4 ? (std::sin(a) > 0 ? hh : -hh) : std::sin(a) * hh;
            ok = project(view_proj, transform_point(without_scale(m), {0, u, v}), pos, size, ring[k]);
        }
        if (!ok) continue;
        for (int k = 0; k < sides; ++k) dl->AddLine(ring[k], ring[k + 1], color, 1.5f);
        ImVec2 a, b;
        if (project(view_proj, transform_point(without_scale(m), {0, 0, 0}), pos, size, a) && project(view_proj, transform_point(without_scale(m), {0.6f, 0, 0}), pos, size, b))
            dl->AddLine(a, b, color, 2.0f);
    }
    // A selected AI Controller's perception (Unreal's AI debugger view): the sight cone flat at the
    // object's height out to sight range, and the hearing range as a ring.
    const reflect::TypeInfo& ai_type = reflect::type_of<world::AIController>();
    for (u32 i = 0; i < s.world->count(ai_type); ++i) {
        const Entity e = s.world->owner(ai_type, i);
        if (!s.is_selected(e) || s.world->hidden_in_hierarchy(e)) continue;
        const auto* ai = static_cast<const world::AIController*>(s.world->at(ai_type, i));
        const Mat4 m = s.world->world_matrix(e);
        const Vec3 origin{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
        const f32 yaw = std::atan2(m.at(1, 0), m.at(0, 0));
        auto ring_point = [&](f32 angle, f32 radius) { return origin + Vec3{std::cos(angle), std::sin(angle), 0.0f} * radius; };
        auto polyline = [&](auto point_at, u32 segments, ImU32 color) {
            ImVec2 a, b;
            bool has_a = project(view_proj, point_at(0u), pos, size, a);
            for (u32 k = 1; k <= segments; ++k) {
                const bool has_b = project(view_proj, point_at(k), pos, size, b);
                if (has_a && has_b) dl->AddLine(a, b, color, 1.5f);
                a = b;
                has_a = has_b;
            }
        };
        constexpr u32 kSegments = 48;
        const f32 half = radians(std::fmin(ai->sight_angle, 360.0f)) * 0.5f;
        const ImU32 sight = IM_COL32(90, 230, 120, 220), hearing = IM_COL32(110, 170, 255, 160);
        // Origin, along the left edge, round the arc, back along the right edge.
        polyline([&](u32 k) {
            if (k == 0 || k == kSegments + 2) return origin;
            return ring_point(yaw - half + 2.0f * half * static_cast<f32>(k - 1) / kSegments, ai->sight_range);
        }, kSegments + 2, sight);
        polyline([&](u32 k) { return ring_point(6.2831853f * static_cast<f32>(k) / kSegments, ai->hearing_range); }, kSegments, hearing);
    }
    // Physics Joints: a ring at the pivot and a line along the axis.
    const reflect::TypeInfo& joint_type = reflect::type_of<world::PhysicsJoint>();
    for (u32 i = 0; i < s.world->count(joint_type); ++i) {
        const Entity e = s.world->owner(joint_type, i);
        if (s.world->hidden_in_hierarchy(e)) continue;
        const auto* joint = static_cast<const world::PhysicsJoint*>(s.world->at(joint_type, i));
        const Mat4 m = s.world->world_matrix(e);
        const Vec3 pivot = transform_point(m, joint->pivot);
        const Vec3 axis = transform_vector(m, joint->axis);
        const f32 axis_length = length(axis);
        ImVec2 a, b;
        if (!project(view_proj, pivot, pos, size, a)) continue;
        const ImU32 color = IM_COL32(255, 230, 40, 255);
        dl->AddCircle(a, 7.0f, color, 16, 2.0f);
        if (s.joint_pivot && e == s.selected) dl->AddCircleFilled(a, 4.0f, color);  // the gizmo is on it
        if (joint->type != 0 && joint->type != 2 && axis_length > 0.0f && project(view_proj, pivot + axis * (0.75f / axis_length), pos, size, b)) {
            dl->AddLine(a, b, color, 2.0f);
        }
    }
    for (u32 slot = 0; slot < s.world->slot_count(); ++slot) {
        const Entity e = s.world->entity_at_slot(slot);
        if (!e.valid() || s.world->get<world::StaticMesh>(e) || s.world->get<world::SkeletalMesh>(e) || s.world->hidden_in_hierarchy(e)) continue;
        const Mat4 m = s.world->world_matrix(e);
        const Vec3 origin{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
        if (is_volume(*s.world, e)) {  // wire box: orange blocks, red kills, green triggers
            const ImU32 color = s.is_selected(e) ? u32_color(t.details_tab_active)
                                : s.world->get<world::KillVolume>(e)    ? IM_COL32(255, 70, 60, 255)
                                : s.world->get<world::TriggerVolume>(e) ? IM_COL32(90, 230, 120, 255)
                                : s.world->get<world::AudioVolume>(e)   ? IM_COL32(190, 140, 255, 255)
                                                                         : IM_COL32(255, 160, 40, 255);
            ImVec2 corner[8];
            bool visible = true;
            for (int c = 0; c < 8 && visible; ++c) {
                const Vec3 p{(c & 1) ? kVolumeHi.x : kVolumeLo.x, (c & 2) ? kVolumeHi.y : kVolumeLo.y, (c & 4) ? kVolumeHi.z : kVolumeLo.z};
                visible = project(view_proj, transform_point(m, p), pos, size, corner[c]);
            }
            if (!visible) continue;
            static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (const auto& edge : edges) dl->AddLine(corner[edge[0]], corner[edge[1]], color, 1.5f);
            dl->AddText(ImVec2(corner[4].x + 4, corner[4].y - 16), color, s.world->name(e));
            continue;
        }
        ImVec2 base, head, tip;
        const bool start = s.world->get<world::PlayerStart>(e) || s.world->get<world::Character>(e);
        if (!project(view_proj, origin, pos, size, base)) continue;
        const ImU32 color = u32_color(s.is_selected(e) ? t.details_tab_active : start ? t.text_blueprint : t.text_component);
        if (start && project(view_proj, origin + Vec3{0, 0, 1.8f}, pos, size, head)) {
            // A standing capsule from feet to head, plus an arrow for the facing direction.
            const f32 radius = std::fabs(head.y - base.y) * 0.17f + 2.0f;
            dl->AddLine(ImVec2(base.x - radius, base.y), ImVec2(head.x - radius, head.y), color, 2.0f);
            dl->AddLine(ImVec2(base.x + radius, base.y), ImVec2(head.x + radius, head.y), color, 2.0f);
            dl->AddCircle(head, radius, color, 16, 2.0f);
            dl->AddCircle(base, radius, color, 16, 2.0f);
            const Vec3 facing = normalize(transform_vector(m, {1, 0, 0}));
            if (project(view_proj, origin + Vec3{0, 0, 0.9f} + facing * 0.8f, pos, size, tip)) {
                ImVec2 mid;
                project(view_proj, origin + Vec3{0, 0, 0.9f}, pos, size, mid);
                dl->AddLine(mid, tip, color, 2.5f);
                dl->AddCircleFilled(tip, 4.0f, color);
            }
            dl->AddText(ImVec2(head.x + radius + 4, head.y - 8), color, s.world->name(e));
        } else {
            draw_type_icon(*s.world, e, dl, base, color);
            dl->AddText(ImVec2(base.x + 12, base.y - 8), color, s.world->name(e));
        }
    }
}

// Click picking: meshes by their boxes, icons by screen distance.
Entity pick(EditorState& s, const Mat4& view_proj, ImVec2 pos, ImVec2 size, ImVec2 mouse) {
    Entity hit;
    f32 t;
    const Ray ray = mouse_ray(s.camera, pos, size, mouse);
    raycast(s, ray, {}, hit, t, true);
    for (u32 slot = 0; slot < s.world->slot_count(); ++slot) {
        const Entity e = s.world->entity_at_slot(slot);
        if (!e.valid() || s.world->get<world::StaticMesh>(e) || s.world->get<world::SkeletalMesh>(e) || s.world->hidden_in_hierarchy(e) || s.world->locked(e)) continue;
        if (is_volume(*s.world, e)) {  // picked like a mesh, by its box
            const Mat4 inv = affine_inverse(s.world->world_matrix(e));
            f32 vt;
            if (ray_box(transform_point(inv, ray.origin), transform_vector(inv, ray.dir), kVolumeLo, kVolumeHi, vt) && vt < t) {
                hit = e;
                t = vt;
            }
            continue;
        }
        const Mat4 m = s.world->world_matrix(e);
        ImVec2 base, head;
        const Vec3 origin{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
        if (!project(view_proj, origin, pos, size, base)) continue;
        const bool standing = s.world->get<world::PlayerStart>(e) || s.world->get<world::Character>(e);
        if (!project(view_proj, origin + Vec3{0, 0, standing ? 1.8f : 0.0f}, pos, size, head)) head = base;
        const f32 lo = (head.y < base.y ? head.y : base.y) - 10.0f, hi = (head.y > base.y ? head.y : base.y) + 10.0f;
        const f32 depth = length(origin - s.camera.position);
        if (std::fabs(mouse.x - base.x) < 14.0f && mouse.y > lo && mouse.y < hi && depth < t) {
            hit = e;
            t = depth;
        }
    }
    return hit;
}

// Switches between 1, 2 (1x2) and 4 (2x2) viewports. The first time, the extra views are framed around the camera's pivot.
void set_panes(EditorState& s, u32 panes) {
    if (panes != 2 && panes != 4) panes = 1;
    if (panes == s.panes) return;
    if (panes > 1 && s.views[1].view == ViewType::Perspective) {
        const Vec3 pivot = s.camera.position + camera_forward(s.camera) * s.camera.pivot_distance;
        const ViewType types[3] = {ViewType::Top, ViewType::Front, ViewType::Right};
        for (int i = 1; i < 4; ++i) {
            s.views[i].view = types[i - 1];
            s.views[i].position = pivot;
            s.views[i].ortho_height = 24.0f;
        }
        s.views[0] = s.camera;
    }
    if (s.active_view >= static_cast<int>(panes)) {  // its pane is gone: show that camera in the first
        s.views[0] = s.camera;
        s.active_view = 0;
    }
    s.panes = panes;
}

} // namespace

void set_viewport_kinds(EditorState& s, u32 kinds) {
    kinds &= kViewport2D | kViewport2_5D | kViewport3D;
    if (!kinds) kinds = kViewport3D;
    const Vec3 pivot = s.camera.position + camera_forward(s.camera) * s.camera.pivot_distance;
    EditorCamera cams[4];
    u32 n = 0;
    if (kinds & kViewport2D) {  // side-on, no perspective
        EditorCamera& c = cams[n++];
        c.view = ViewType::Front;
        c.position = {pivot.x, pivot.y - 30.0f, pivot.z};
        c.ortho_height = 14.0f;
    }
    if (kinds & kViewport2_5D) {  // side-on with a long lens: depth reads as layers
        EditorCamera& c = cams[n++];
        c.position = {pivot.x, pivot.y - 24.0f, pivot.z + 3.0f};
        c.yaw = 1.5707963f;
        c.pitch = -0.12f;
        c.fov_y = 0.6f;
        c.pivot_distance = 24.0f;
    }
    if (kinds & kViewport3D) {
        EditorCamera& c = cams[n++];
        c.position = pivot + Vec3{-9.0f, -9.0f, 6.0f};
        c.yaw = 0.7853982f;
        c.pitch = -0.4f;
        c.pivot_distance = 13.0f;
    }
    if (n == 3) {  // the fourth pane looks down
        cams[3].view = ViewType::Top;
        cams[3].position = pivot;
        cams[3].ortho_height = 24.0f;
    }
    for (u32 i = 0; i < 4; ++i) s.views[i] = cams[i < n || n == 3 ? i : 0];
    s.camera = s.views[0];
    s.active_view = 0;
    s.panes = n == 1 ? 1u : n == 2 ? 2u : 4u;
    s.settings.viewport_kinds = kinds;
}

namespace {

void draw_viewport(EditorState& s, int index, ImVec2 pos, ImVec2 size) {
    if (size.x < 8 || size.y < 8) return;
    const EditorTheme& t = s.theme;
    ImGuiIO& io = ImGui::GetIO();
    const bool active = index == s.active_view;
    f32* rect = s.viewport_rects[index];
    rect[0] = pos.x;
    rect[1] = pos.y;
    rect[2] = size.x;
    rect[3] = size.y;
    char window_name[32];
    const Mat4 view = camera_view(s.camera);
    const Mat4 proj = camera_projection(s.camera, size.x / size.y);
    const Mat4 view_proj = proj * view;

    // Invisible window covering the viewport: owns its mouse input, the gizmo and asset drops.
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    std::snprintf(window_name, sizeof window_name, "##ViewportInput%d", index);
    ImGui::Begin(window_name, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar();
    // ImGuizmo only grabs a click when no widget is hovered: over the gizmo (last frame), the viewport
    // leaves its click surface out so LMB reaches the arrows.
    bool hovered;
    if (g_gizmo_busy[index] && !(s.playing && !s.ejected)) {
        ImGui::Dummy(size);
        hovered = ImGui::IsWindowHovered();
    } else {
        ImGui::InvisibleButton("##surface", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        hovered = ImGui::IsItemHovered();
    }
    if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                    ImGui::IsMouseClicked(ImGuiMouseButton_Middle))) {
        g_request_active = index;
    }
    if (s.panes > 1 && active) {  // gold frame on the active viewport
        ImGui::GetForegroundDrawList()->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), u32_color(t.details_tab_active), 0.0f, 0, 2.0f);
    }

    if (s.playing && !s.ejected) {
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) s.mouse_captured = true;  // click back in to resume control
        ImGui::End();
        char text[200];
        std::snprintf(text, sizeof text, "%s%s   Esc: stop   Pause: pause   F8: eject%s", s.paused ? "Paused ( ] steps one frame)" : "Playing",
                      s.mouse_captured ? "" : " (mouse free: click to control)", s.mouse_captured ? "   Shift+F1: free the mouse" : "");
        ImDrawList* play_list = ImGui::GetBackgroundDrawList();
        if (debug_draw::count()) {  // scripts' debug shapes, through the player's camera
            play_list->PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), true);
            debug_draw::draw(play_list, s.drawn_view_proj[index], pos, size);
            play_list->PopClipRect();
        }
        play_list->AddText(ImVec2(pos.x + 12, pos.y + 10), u32_color(t.details_tab_active), text);
        draw_stats(s, pos, size);
        return;
    }
    if (s.playing) {  // ejected: edit-style camera and selection; the player is drawn as a marker
        ImVec2 feet, head;
        if (project(view_proj, s.player_feet, pos, size, feet) && project(view_proj, s.player_feet + Vec3{0, 0, 1.8f}, pos, size, head)) {
            ImGui::GetBackgroundDrawList()->AddLine(feet, head, IM_COL32(80, 255, 120, 255), 6.0f);
            ImGui::GetBackgroundDrawList()->AddText(ImVec2(head.x + 6, head.y - 8), IM_COL32(80, 255, 120, 255), "Player");
        }
        ImGui::GetBackgroundDrawList()->AddText(ImVec2(pos.x + 12, pos.y + 70), u32_color(t.details_tab_active),
                                                "Ejected: the game keeps running   F8: possess the player again   Esc: stop");
    }

    const bool gizmo_busy = draw_gizmo(s, view, proj, pos, size, index);
    g_gizmo_busy[index] = gizmo_busy;
    const bool brushing = mode_brush(s, view_proj, pos, size, hovered && !gizmo_busy);  // Foliage / Landscape paint instead of selecting
    // Click selects; Ctrl+click toggles, Shift+click adds; a drag from empty space draws a selection box.
    static bool marquee_by_view[4];
    static ImVec2 marquee_start_by_view[4];
    bool& marquee = marquee_by_view[index];
    ImVec2& marquee_start = marquee_start_by_view[index];
    const i32 clicked_point = ImGui::IsItemClicked(ImGuiMouseButton_Left) && !io.KeyAlt && !gizmo_busy ? spline_point_at(s, view_proj, pos, size, io.MousePos) : -1;
    const bool clicked_pivot = clicked_point < 0 && ImGui::IsItemClicked(ImGuiMouseButton_Left) && !io.KeyAlt && !gizmo_busy && joint_pivot_at(s, view_proj, pos, size, io.MousePos);
    const bool clicked_handle = clicked_point < 0 && !clicked_pivot && ImGui::IsItemClicked(ImGuiMouseButton_Left) && !io.KeyAlt && !gizmo_busy &&
                                spline_handle_at(s, view_proj, pos, size, io.MousePos);
    if (brushing) {
        marquee = false;
    } else if (clicked_point >= 0 || clicked_handle) {  // the gizmo now moves that point, or its tangent handle
        if (clicked_point >= 0) s.spline_point = clicked_point;
        s.spline_handle = clicked_handle;
    } else if (clicked_pivot) {
        s.joint_pivot = true;  // the gizmo now moves the joint's pivot
    } else if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !io.KeyAlt && !gizmo_busy) {
        const Entity hit = pick(s, view_proj, pos, size, io.MousePos);
        s.joint_pivot = s.spline_handle = false;  // a click elsewhere: the gizmo goes back to the object
        if (io.KeyCtrl) add_to_selection(s, hit, true);
        else if (io.KeyShift) add_to_selection(s, hit, false);
        else select(s, hit);
        marquee = !hit.valid();
        marquee_start = io.MousePos;
    }
    if (marquee) {
        const ImVec2 a(marquee_start.x < io.MousePos.x ? marquee_start.x : io.MousePos.x, marquee_start.y < io.MousePos.y ? marquee_start.y : io.MousePos.y);
        const ImVec2 b(marquee_start.x > io.MousePos.x ? marquee_start.x : io.MousePos.x, marquee_start.y > io.MousePos.y ? marquee_start.y : io.MousePos.y);
        const bool dragged = b.x - a.x > 4.0f || b.y - a.y > 4.0f;
        if (dragged) {
            ImGui::GetForegroundDrawList()->AddRectFilled(a, b, IM_COL32(243, 195, 18, 40));
            ImGui::GetForegroundDrawList()->AddRect(a, b, u32_color(t.details_tab_active), 0.0f, 0, 1.5f);
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            marquee = false;
            // Objects whose bounds centre lands in the box.
            for (u32 slot = 0; dragged && slot < s.world->slot_count(); ++slot) {
                const Entity e = s.world->entity_at_slot(slot);
                if (!e.valid() || s.world->hidden_in_hierarchy(e) || s.world->locked(e)) continue;
                Vec3 lo, hi;
                world_bounds(s, e, lo, hi);
                ImVec2 p;
                if (project(view_proj, (lo + hi) * 0.5f, pos, size, p) && p.x >= a.x && p.x <= b.x && p.y >= a.y && p.y <= b.y) add_to_selection(s, e, false);
            }
        }
    }

    if (ImGui::BeginDragDropTarget()) {
        const Ray ray = mouse_ray(s.camera, pos, size, io.MousePos);
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            asset::Guid guid;
            std::memcpy(&guid, payload->Data, sizeof guid);
            const asset::AssetRecord* record = s.registry->find(guid);
            char key[33];
            asset::to_string(guid, key);
            if (record && record->type == asset::AssetType::Mesh && s.skinned && s.skinned->get(key)) {  // rigged: a Skeletal Mesh
                const Entity e = place(s, record->name, nullptr, "SkeletalMesh", drop_point(s, ray));
                std::snprintf(s.world->get<world::SkeletalMesh>(e)->mesh, sizeof(world::SkeletalMesh::mesh), "%s", key);
            } else if (record && record->type == asset::AssetType::Mesh) {
                place(s, record->name, key, nullptr, drop_point(s, ray));
            } else if (record && record->type == asset::AssetType::Prefab) {
                place_prefab(s, *record, drop_point(s, ray));
            } else if (record && asset_matches(*record, "Material")) {
                // Onto the object under the mouse, like dropping a material on an actor in Unreal.
                Entity hit;
                f32 hit_t = 0.0f;
                world::StaticMesh* mesh = raycast(s, ray, {}, hit, hit_t, true) ? s.world->get<world::StaticMesh>(hit) : nullptr;
                if (mesh) {
                    asset::to_string(guid, mesh->material);
                    select(s, hit);
                    char label[96];
                    std::snprintf(label, sizeof label, "Apply %s", record->name);
                    edited(s, label);
                } else {
                    std::snprintf(s.status, sizeof s.status, "Drop a material onto an object with a Static Mesh");
                }
            } else if (record && asset_matches(*record, "ParticleSystem")) {  // a Particle Effect, like dropping a Niagara System
                const Entity e = place(s, record->name, nullptr, "ParticleEffect", drop_point(s, ray));
                asset::to_string(guid, s.world->get<world::ParticleEffect>(e)->system);
            } else if (record && (record->type == asset::AssetType::Sound || record->type == asset::AssetType::Cue)) {  // a Sound Emitter, like dropping a sound in Unreal
                const Entity e = place(s, record->name, nullptr, "SoundEmitter", drop_point(s, ray) + Vec3{0, 0, 1});
                asset::to_string(guid, s.world->get<world::SoundEmitter>(e)->sound);
            }
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kPlacePayload)) {
            const PlaceItem& item = kPlaceItems[*static_cast<const int*>(payload->Data)];
            place(s, item.label, item.mesh, item.component, drop_point(s, ray));
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::End();
    update_camera(s, hovered, index);

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), true);
    if (!s.game_view) draw_icons(s, dl, view_proj, pos, size);
    if (debug_draw::count()) debug_draw::draw(dl, s.drawn_view_proj[index], pos, size);  // scripts' debug shapes, also in Play
    const bool show_collision = s.show_collision || s.view_mode == ViewMode::Collision;
    if ((s.show_bounds || show_collision) && !s.game_view) {  // Unreal's Show > Bounds / Collision
        // A box through `m`: its 8 corners projected, 12 edges drawn when all are in front of the camera.
        auto box = [&](const Mat4& m, Vec3 lo, Vec3 hi, ImU32 color) {
            ImVec2 corner[8];
            for (int c = 0; c < 8; ++c) {
                if (!project(view_proj, transform_point(m, {(c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z}), pos, size, corner[c])) return;
            }
            static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (const auto& edge : edges) dl->AddLine(corner[edge[0]], corner[edge[1]], color, 1.0f);
        };
        for (u32 slot = 0; slot < s.world->slot_count(); ++slot) {
            const Entity e = s.world->entity_at_slot(slot);
            if (!e.valid() || s.world->hidden_in_hierarchy(e)) continue;
            const auto* mesh = s.world->get<world::StaticMesh>(e);
            const bool has_mesh = mesh || s.world->get<world::SkeletalMesh>(e);
            if (s.show_bounds && has_mesh) {
                Vec3 lo, hi;
                world_bounds(s, e, lo, hi);
                box(Mat4{}, lo, hi, IM_COL32(255, 220, 60, 200));
            }
            // shortcut: rigid-body spheres and capsules are drawn as their fitted box; draw the real shapes if that misleads.
            const bool blocks = (mesh && mesh->collision) || s.world->get<world::BlockingVolume>(e) || s.world->get<world::RigidBody>(e);
            if (show_collision && blocks) {
                Vec3 lo, hi;
                local_bounds(s, e, lo, hi);
                box(s.world->world_matrix(e), lo, hi, IM_COL32(80, 220, 255, 230));
            }
        }
    }
    // Axis gizmo, bottom-left: world axes as seen from the camera.
    if (s.show_axis_gizmo && !s.game_view) {
        const Vec3 right{view.at(0, 0), view.at(0, 1), view.at(0, 2)};
        const Vec3 up{view.at(1, 0), view.at(1, 1), view.at(1, 2)};
        const ImVec2 origin(pos.x + 48, pos.y + size.y - 48);
        const struct { Vec3 dir; const LinearColor* color; const char* label; } axes[3] = {
            {{1, 0, 0}, &t.axis_x, "X"}, {{0, 1, 0}, &t.axis_y, "Y"}, {{0, 0, 1}, &t.axis_z, "Z"}};
        for (const auto& a : axes) {
            const ImVec2 tip(origin.x + dot(a.dir, right) * 32.0f, origin.y - dot(a.dir, up) * 32.0f);
            dl->AddLine(origin, tip, u32_color(*a.color), 2.5f);
            dl->AddText(ImVec2(tip.x + 3, tip.y - 8), u32_color(*a.color), a.label);
        }
    }
    dl->PopClipRect();

    // Toolbar overlay, top-left of the viewport.
    ImGui::SetNextWindowPos(ImVec2(pos.x + 8, pos.y + 8));
    ImGui::SetNextWindowBgAlpha(0.65f);
    const ImGuiWindowFlags overlay = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                     ImGuiWindowFlags_NoMove;
    std::snprintf(window_name, sizeof window_name, "##ViewportToolbar%d", index);
    if (ImGui::Begin(window_name, nullptr, overlay)) {
        static const char* const kViews[] = {"Perspective", "Top", "Front", "Right"};
        static const char* const kViewKeys[] = {"Alt+G", "Alt+J", "Alt+H", "Alt+K"};
        static const char* const kModes[] = {"Lit", "Unlit", "Wireframe", "Normals", "Lighting Only", "Collision", "Overdraw", "Lightmap Density"};
        static const char* const kModeKeys[] = {"Alt+4", "Alt+3", "Alt+2", "", "Alt+6", "Alt+C", "Alt+0", ""};
        if (with_tip(ImGui::Button(kViews[static_cast<int>(s.camera.view)]), "Camera view: Perspective, or flat Top / Front / Right views (keys in the menu)")) ImGui::OpenPopup("ViewMenu");
        if (ImGui::BeginPopup("ViewMenu")) {
            for (int v = 0; v < 4; ++v) {
                if (ImGui::MenuItem(kViews[v], kViewKeys[v], static_cast<int>(s.camera.view) == v)) set_view(s, static_cast<ViewType>(v));
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (with_tip(ImGui::Button(kModes[static_cast<int>(s.view_mode)]), "View mode: Lit, Unlit, Wireframe, Normals, Lighting Only, Collision shapes, Overdraw (how often each pixel is drawn), Lightmap Density (the baked lightmap's texels)")) ImGui::OpenPopup("ModeMenu");
        if (ImGui::BeginPopup("ModeMenu")) {
            for (int m = 0; m < 8; ++m) {
                if (ImGui::MenuItem(kModes[m], kModeKeys[m], static_cast<int>(s.view_mode) == m)) s.view_mode = static_cast<ViewMode>(m);
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(s.panes == 4 ? "2x2" : s.panes == 2 ? "1x2" : "1x1")) set_panes(s, s.panes == 1 ? 2 : s.panes == 2 ? 4 : 1);
        ImGui::SetItemTooltip("Viewport layout: one; perspective + top side by side; or perspective + top + front + right");
        if (s.panes > 1 && !active) {  // the other viewports keep a short toolbar
            ImGui::End();
            return;
        }
        ImGui::SameLine();
        if (with_tip(ImGui::Button("Show"), "What the viewport draws: grid, gizmo, navmesh, bounds, collision, game view, stats")) ImGui::OpenPopup("ShowMenu");
        if (ImGui::BeginPopup("ShowMenu")) {
            with_tip(ImGui::Checkbox("Grid", &s.show_grid), "The floor grid");
            with_tip(ImGui::Checkbox("Axis gizmo", &s.show_axis_gizmo), "The axis arrows in the corner");
            with_tip(ImGui::Checkbox("Navigation (P)", &s.show_navigation), "Where AI can walk, in green");
            with_tip(ImGui::Checkbox("Bounds", &s.show_bounds), "Boxes around every mesh");
            with_tip(ImGui::Checkbox("Collision", &s.show_collision), "The shapes physics uses");
            with_tip(ImGui::Checkbox("Game view (G)", &s.game_view), "Hides editor-only things (icons, volumes, gizmos) to see what the player sees");
            with_tip(ImGui::Checkbox("Stats (stat fps)", &s.show_stats), "Frame rate, draw calls and triangles in the corner");
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (with_tip(toggle_button("Grid", s.show_grid), "Shows the floor grid")) s.show_grid = !s.show_grid;
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (toggle_button("Move", s.gizmo == GizmoMode::Translate)) s.gizmo = GizmoMode::Translate;
        ImGui::SetItemTooltip("Move (W): drag an arrow to move along it, a square between two to move in that plane");
        ImGui::SameLine();
        if (toggle_button("Rotate", s.gizmo == GizmoMode::Rotate)) s.gizmo = GizmoMode::Rotate;
        ImGui::SetItemTooltip("Rotate (E): drag a ring to turn around that axis");
        ImGui::SameLine();
        if (toggle_button("Scale", s.gizmo == GizmoMode::Scale)) s.gizmo = GizmoMode::Scale;
        ImGui::SetItemTooltip("Scale (R): drag a handle to resize along it, the centre to resize evenly");
        ImGui::SameLine();
        if (ImGui::Button(s.gizmo_local ? "Local" : "World")) s.gizmo_local = !s.gizmo_local;
        ImGui::SetItemTooltip("Gizmo space (Ctrl+`)");
        ImGui::SameLine();
        if (toggle_button("Surface", s.settings.snap_to_surface)) s.settings.snap_to_surface = !s.settings.snap_to_surface;
        ImGui::SetItemTooltip("Surface snapping: moved objects slide over what is under them. Hold V while moving: snap to the nearest vertex");
        ImGui::SameLine();
        if (toggle_button("Pivot", s.pivot_edit)) s.pivot_edit = !s.pivot_edit;
        ImGui::SetItemTooltip("Alt+V: the gizmo moves the pivot the object turns and scales about (right-click: reset)");
        if (ImGui::BeginPopupContextItem("pivot_menu")) {
            if (with_tip(ImGui::MenuItem("Reset Pivot"), "Puts the pivot back at the object's origin")) s.pivot_offset = {};
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (with_tip(toggle_button("Snap", s.settings.snap), "Moves, turns and scales go in steps (the box next to it sets the step)")) s.settings.snap = !s.settings.snap;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        f32* step = s.gizmo == GizmoMode::Translate ? &s.settings.snap_move : s.gizmo == GizmoMode::Rotate ? &s.settings.snap_angle : &s.settings.snap_scale;
        with_tip(ImGui::DragFloat("##snap", step, 0.05f, 0.01f, 360.0f, s.gizmo == GizmoMode::Rotate ? "%.0f deg" : s.gizmo == GizmoMode::Translate ? "%.2f m" : "%.2f"), "Snap step for the current gizmo (drag to change)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::SliderFloat("##speed", &s.settings.camera_speed, 0.5f, 50.0f, "speed %.1f", ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("Camera speed (RMB + wheel)");
        if (s.panes == 1) {
            ImGui::SameLine();
            ImGui::TextDisabled("RMB: look + WASD/QE fly  MMB: pan  Alt+LMB: orbit  F: focus");
        }
    }
    ImGui::End();
    draw_stats(s, pos, size);
}

// ---- Outliner ----

struct OutlineRow {
    Entity entity;
    int depth;
    const char* folder = nullptr;  // set on a folder's header row (then `entity` is invalid)
};

// Each object's children in slot order, gathered in one pass a frame: the walk below is linear in the
// number of objects (scanning every slot for every parent was quadratic: 25 ms with 3000 objects).
using ChildLists = std::unordered_map<u32, std::vector<Entity>>;

void collect_rows(const ChildLists& children, Entity parent, int depth, OutlineRow* rows, int& count, int max) {
    const auto it = children.find(parent.value);
    if (it == children.end()) return;
    for (const Entity e : it->second) {
        if (count >= max) return;
        rows[count++] = {e, depth};
        collect_rows(children, e, depth + 1, rows, count, max);
    }
}

std::set<std::string> g_closed_folders;  // Outliner folders folded shut (this session)
char g_renaming_folder[64] = "";         // the folder whose name is being edited in place
char g_folder_name[64] = "";

// Hierarchy tab: folders (A-Z) with their root objects inside, then the objects in no folder.
void collect_hierarchy(const world::World& w, OutlineRow* rows, int& count, int max) {
    static ChildLists children;
    for (auto& [parent, list] : children) list.clear();  // keeps the lists' memory frame to frame
    for (u32 slot = 0; slot < w.slot_count(); ++slot) {
        const Entity e = w.entity_at_slot(slot);
        if (e.valid() && w.parent(e).valid()) children[w.parent(e).value].push_back(e);
    }
    std::map<std::string, const char*> folders;  // name -> a record's copy, stable for this frame
    for (u32 slot = 0; slot < w.slot_count(); ++slot) {
        const Entity e = w.entity_at_slot(slot);
        if (e.valid() && !w.parent(e).valid() && w.folder(e)[0]) folders.emplace(w.folder(e), w.folder(e));
    }
    for (const auto& [name, label] : folders) {
        if (count >= max) return;
        rows[count++] = {{}, 0, label};
        if (g_closed_folders.count(name)) continue;
        for (u32 slot = 0; slot < w.slot_count() && count < max; ++slot) {
            const Entity e = w.entity_at_slot(slot);
            if (!e.valid() || w.parent(e).valid() || name != w.folder(e)) continue;
            rows[count++] = {e, 1};
            collect_rows(children, e, 2, rows, count, max);
        }
    }
    for (u32 slot = 0; slot < w.slot_count() && count < max; ++slot) {
        const Entity e = w.entity_at_slot(slot);
        if (!e.valid() || w.parent(e).valid() || w.folder(e)[0]) continue;
        rows[count++] = {e, 0};
        collect_rows(children, e, 1, rows, count, max);
    }
}

// Puts the selected objects (their top-most selected ancestors) into `folder` ("" = out of folders).
// Children follow their parent, so a selected child is detached first, like Unreal.
void move_selection_to_folder(EditorState& s, const char* folder) {
    Entity roots[EditorState::kMaxSelection];
    const u32 n = selection_roots(s, roots);
    for (u32 i = 0; i < n; ++i) {
        if (s.world->parent(roots[i]).valid()) s.world->set_parent(roots[i], {});
        s.world->set_folder(roots[i], folder);
    }
    char label[96];
    std::snprintf(label, sizeof label, folder[0] ? "Move to folder %s" : "Remove from folder%s", folder);
    edited(s, label);
}

// Renames a folder: every object in it moves to the new name.
void rename_folder(EditorState& s, const char* from, const char* to) {
    char old_name[64];
    std::snprintf(old_name, sizeof old_name, "%s", from);  // `from` may point into a record we rewrite
    for (u32 slot = 0; slot < s.world->slot_count(); ++slot) {
        const Entity e = s.world->entity_at_slot(slot);
        if (e.valid() && std::strcmp(s.world->folder(e), old_name) == 0) s.world->set_folder(e, to);
    }
    if (g_closed_folders.erase(old_name) && to[0]) g_closed_folders.insert(to);
    edited(s, to[0] ? "Rename folder" : "Delete folder");
}

// One folder header row: click folds it, drop objects on it to file them, right-click to rename or delete.
void draw_folder_row(EditorState& s, const char* folder, f32 row_height, ImDrawList* dl) {
    const EditorTheme& t = s.theme;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool closed = g_closed_folders.count(folder) != 0;
    const ImU32 color = u32_color(t.details_tab_active);
    const f32 cy = p.y + row_height * 0.5f;  // a folder glyph: tab and body, filled when folded shut
    dl->AddRectFilled(ImVec2(p.x + 4, cy - 6), ImVec2(p.x + 10, cy - 4), color);
    if (closed) dl->AddRectFilled(ImVec2(p.x + 4, cy - 4), ImVec2(p.x + 18, cy + 5), color, 1.0f);
    else dl->AddRect(ImVec2(p.x + 4, cy - 4), ImVec2(p.x + 18, cy + 5), color, 1.0f, 0, 1.5f);
    ImGui::PushID(folder);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 24.0f);
    if (std::strcmp(g_renaming_folder, folder) == 0) {
        if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-60.0f);
        if (ImGui::InputText("##rename", g_folder_name, sizeof g_folder_name, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            if (g_folder_name[0] && std::strcmp(g_folder_name, folder) != 0) rename_folder(s, folder, g_folder_name);
            g_renaming_folder[0] = '\0';
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            g_renaming_folder[0] = '\0';
        }
        ImGui::PopID();
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, vec4(t.details_tab_active));
    if (ImGui::Selectable(folder, false, ImGuiSelectableFlags_SpanAvailWidth, ImVec2(0, row_height - ImGui::GetStyle().ItemSpacing.y))) {
        if (closed) g_closed_folders.erase(folder);
        else g_closed_folders.insert(folder);
    }
    ImGui::PopStyleColor();
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            Entity dropped;
            std::memcpy(&dropped, payload->Data, sizeof dropped);
            if (!s.is_selected(dropped)) select(s, dropped);
            char name[64];
            std::snprintf(name, sizeof name, "%s", folder);
            move_selection_to_folder(s, name);
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("folder_row_menu")) {
        if (with_tip(ImGui::MenuItem("Rename"), "Renames the object (F2)")) {
            std::snprintf(g_renaming_folder, sizeof g_renaming_folder, "%s", folder);
            std::snprintf(g_folder_name, sizeof g_folder_name, "%s", folder);
        }
        if (ImGui::MenuItem("Delete Folder")) rename_folder(s, folder, "");  // its objects stay, out of any folder
        ImGui::SetItemTooltip("Removes the folder; the objects in it stay in the level");
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

bool contains_ignore_case(const char* text, const char* needle) {
    for (; *text; ++text) {
        const char* a = text;
        const char* b = needle;
        while (*a && *b && std::tolower(static_cast<unsigned char>(*a)) == std::tolower(static_cast<unsigned char>(*b))) { ++a; ++b; }
        if (!*b) return true;
    }
    return !*needle;
}

void draw_rows(EditorState& s, const OutlineRow* rows, int row_count, bool tree) {
    const EditorTheme& t = s.theme;
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, vec4(t.outline_scrollbar));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, vec4(t.outline_row_b));
    ImGui::BeginChild(tree ? "HierarchyRows" : "OutlineRows", ImVec2(0, 0), ImGuiChildFlags_None);
    const f32 row_height = ImGui::GetTextLineHeight() + 10.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const f32 left = ImGui::GetWindowPos().x;
    const f32 right = left + ImGui::GetWindowWidth();
    int visible = static_cast<int>(ImGui::GetWindowHeight() / row_height) + 1;
    visible = visible > row_count ? visible : row_count;
    ImGuiListClipper clipper;  // only the rows on screen are built: thousands of objects stay cheap
    clipper.Begin(visible, row_height);
    while (clipper.Step())
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(ImVec2(left, p.y), ImVec2(right, p.y + row_height), u32_color((i & 1) ? t.outline_row_b : t.outline_row_a));
        if (i >= row_count) { ImGui::Dummy(ImVec2(0, row_height - ImGui::GetStyle().ItemSpacing.y)); continue; }
        const OutlineRow& row = rows[i];
        if (row.folder) {
            draw_folder_row(s, row.folder, row_height, dl);
            continue;
        }
        const Entity e = row.entity;
        const LinearColor& text = s.world->get<world::StaticMesh>(e) ? t.text_mesh : s.world->get<world::PlayerStart>(e) ? t.text_blueprint : t.text_component;
        const int depth = tree ? row.depth : 0;
        if (depth) {  // child hook drawn as lines: the UI font has no arrow glyph
            const f32 hx = p.x + static_cast<f32>(depth) * 22.0f - 14.0f;
            const f32 mid = p.y + row_height * 0.5f;
            dl->AddLine(ImVec2(hx, p.y + 4.0f), ImVec2(hx, mid), u32_color(text), 1.5f);
            dl->AddLine(ImVec2(hx, mid), ImVec2(hx + 9.0f, mid), u32_color(text), 1.5f);
        }
        ImGui::PushID(static_cast<int>(e.value));
        const bool hidden = s.world->hidden(e), locked = s.world->locked(e);
        ImVec4 text_color = vec4(text);
        if (hidden) text_color.w = 0.45f;
        ImGui::PushStyleColor(ImGuiCol_Text, text_color);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + static_cast<f32>(depth) * 22.0f);
        ImGui::SetNextItemAllowOverlap();  // the eye and lock buttons sit on top of the row
        if (ImGui::Selectable(s.world->name(e), s.is_selected(e), static_cast<ImGuiSelectableFlags>(ImGuiSelectableFlags_SpanAvailWidth) | ImGuiSelectableFlags_AllowDoubleClick,
                              ImVec2(0, row_height - ImGui::GetStyle().ItemSpacing.y))) {
            const ImGuiIO& io = ImGui::GetIO();
            if (io.KeyCtrl) {
                add_to_selection(s, e, true);
            } else if (io.KeyShift && s.selected.valid()) {  // range from the primary to this row
                int from = -1;
                for (int r = 0; r < row_count; ++r) from = rows[r].entity == s.selected ? r : from;
                if (from < 0) from = i;
                const int lo = from < i ? from : i, hi = from < i ? i : from;
                for (int r = lo; r <= hi; ++r) {
                    if (rows[r].entity.valid()) add_to_selection(s, rows[r].entity, false);  // folder rows have none
                }
                add_to_selection(s, e, false);
            } else {
                select(s, e);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) focus_selected(s);
            }
        }
        ImGui::PopStyleColor();
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kEntityPayload, &e, sizeof e);
            ImGui::Text("Attach %s to...", s.world->name(e));
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload)) {
                Entity child;
                std::memcpy(&child, payload->Data, sizeof child);
                if (child != e && s.world->set_parent(child, e)) edited(s, "Attach");
                else std::snprintf(s.status, sizeof s.status, "Cannot attach an object to itself or its own child");
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem("row_menu")) {
            if (!s.is_selected(e)) select(s, e);
            if (ImGui::MenuItem("Focus", command_text(Command::Focus))) focus_selected(s);
            if (s.playing && ImGui::MenuItem("Keep Simulation Changes", command_text(Command::KeepSimulation))) keep_simulation_changes(s);
            if (s.playing) ImGui::SetItemTooltip("When Play stops, the selected objects keep what they have now (where physics left them...)");
            asset::Guid used;  // the object's mesh (or script, sound) asset
            i32 part = -1;  // a part of a mesh file finds the file
            if (const auto* m = s.world->get<world::StaticMesh>(e)) asset::parse_mesh_key(m->mesh, used, part);
            if (const auto* m = s.world->get<world::SkeletalMesh>(e); m && !used.valid()) asset::parse_guid(m->mesh, used);
            if (const auto* m = s.world->get<world::Script>(e); m && !used.valid()) asset::parse_guid(m->script, used);
            if (const auto* m = s.world->get<world::SoundEmitter>(e); m && !used.valid()) asset::parse_guid(m->sound, used);
            if (with_tip(ImGui::MenuItem("Find in Content Browser", command_text(Command::FindInContentBrowser), false, used.valid()), "Shows the asset this object uses in the Content Browser")) find_in_content_browser(s, used);
            if (with_tip(ImGui::MenuItem("Duplicate", command_text(Command::Duplicate)), "Copies the selection in place")) duplicate_selected(s);
            if (with_tip(ImGui::MenuItem("Detach", nullptr, false, s.world->parent(e).valid()), "Unparents it: it stays where it is but no longer moves with its parent")) {
                s.world->set_parent(e, {});
                edited(s, "Detach");
            }
            if (with_tip(ImGui::MenuItem("Delete", command_text(Command::Delete)), "Removes the selection from the level (undoable)")) delete_selected(s);
            if (with_tip(ImGui::BeginMenu("Move to Folder", !s.playing), "Puts the selection in an Outliner folder (folders only organise; they do not move objects)")) {
                std::set<std::string> folders;
                for (u32 slot = 0; slot < s.world->slot_count(); ++slot) {
                    const Entity other = s.world->entity_at_slot(slot);
                    if (other.valid() && s.world->folder(other)[0]) folders.insert(s.world->folder(other));
                }
                for (const std::string& f : folders) {
                    if (ImGui::MenuItem(f.c_str())) move_selection_to_folder(s, f.c_str());
                }
                if (!folders.empty()) ImGui::Separator();
                if (with_tip(ImGui::MenuItem("New Folder"), "Makes a new Outliner folder for the selection")) {
                    char name[64] = "NewFolder";
                    for (int n = 2; folders.count(name); ++n) std::snprintf(name, sizeof name, "NewFolder%d", n);
                    move_selection_to_folder(s, name);
                    std::snprintf(g_renaming_folder, sizeof g_renaming_folder, "%s", name);  // name it at once, like Unreal
                    std::snprintf(g_folder_name, sizeof g_folder_name, "%s", name);
                }
                if (s.world->folder(e)[0] && with_tip(ImGui::MenuItem("Remove from Folder"), "Takes the selection out of its folder")) move_selection_to_folder(s, "");
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Create Prefab", nullptr, false, !s.playing)) create_prefab(s, e);
            ImGui::SetItemTooltip("Saves this object and its children as a prefab in the open Content Browser folder");
            ImGui::EndPopup();
        }
        {  // after the row's menus (they attach to the Selectable): eye (hide in editor) and lock (no viewport selection or gizmo), like Unreal's Outliner
            const ImVec2 after = ImGui::GetCursorScreenPos();
            const f32 cy = p.y + row_height * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(right - 52.0f, p.y + 2.0f));
            if (ImGui::InvisibleButton("eye", ImVec2(22.0f, row_height - 6.0f))) {
                s.world->set_hidden(e, !hidden);
                edited(s, hidden ? "Show" : "Hide");
            }
            ImGui::SetItemTooltip(hidden ? "Hidden in the editor (still plays). Click to show" : "Hide in the editor");
            const ImU32 icon = hidden ? IM_COL32(140, 140, 140, 200) : u32_color(text);
            const ImVec2 eye(right - 41.0f, cy);
            dl->AddEllipse(eye, ImVec2(8.0f, 4.5f), icon, 0.0f, 16, 1.5f);
            if (!hidden) dl->AddCircleFilled(eye, 2.5f, icon);
            else dl->AddLine(ImVec2(eye.x - 8, eye.y + 5), ImVec2(eye.x + 8, eye.y - 5), icon, 1.5f);
            ImGui::SetCursorScreenPos(ImVec2(right - 28.0f, p.y + 2.0f));
            if (ImGui::InvisibleButton("lock", ImVec2(22.0f, row_height - 6.0f))) {
                s.world->set_locked(e, !locked);
                edited(s, locked ? "Unlock" : "Lock");
            }
            ImGui::SetItemTooltip(locked ? "Locked: not selectable in the viewport, no gizmo. Click to unlock" : "Lock");
            const ImU32 lock_color = locked ? u32_color(t.details_tab_active) : IM_COL32(140, 140, 140, 120);
            const ImVec2 body(right - 17.0f, cy + 1.0f);
            if (locked) dl->AddRectFilled(ImVec2(body.x - 5, body.y - 1), ImVec2(body.x + 5, body.y + 6), lock_color, 1.5f);
            else dl->AddRect(ImVec2(body.x - 5, body.y - 1), ImVec2(body.x + 5, body.y + 6), lock_color, 1.5f, 0, 1.5f);
            dl->PathArcTo(ImVec2(body.x, body.y - 2), 3.5f, 3.14159f, 6.28318f, 8);
            dl->PathStroke(lock_color, 0, 1.5f);
            // Back to where the next row starts: a zero-size item ending exactly there keeps ImGui's layout valid.
            ImGui::SetCursorScreenPos(ImVec2(after.x, after.y - ImGui::GetStyle().ItemSpacing.y));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    // Dropping on the empty area below the rows detaches.
    if (tree && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            Entity child;
            std::memcpy(&child, payload->Data, sizeof child);
            if (s.world->parent(child).valid() && s.world->set_parent(child, {})) edited(s, "Detach");
            else if (s.world->folder(child)[0]) {  // a filed root object: out of its folder
                s.world->set_folder(child, "");
                edited(s, "Remove from folder");
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::PopStyleColor(2);
}

void draw_outliner(EditorState& s) {
    const EditorTheme& t = s.theme;
    static int active_tab = 1;
    static char filter[64] = "";
    static OutlineRow rows[4096];
    if (!begin_panel(kOutlinerWindow)) { ImGui::End(); return; }
    static bool first_frame = true;  // ImGui opens the first tab; start on the one active_tab names
    const int wanted = first_frame ? active_tab : -1;  // read before the tabs below overwrite active_tab
    first_frame = false;
    if (ImGui::BeginTabBar("OutlinerTabs")) {
        if (themed_tab("Outline", 0, active_tab, t.outline_tab, t.panel_background, LinearColor::hex(0xFFFFFF), wanted == 0)) {
            // Type filter (Unreal's Outliner filter by class): component types the level uses.
            static const reflect::TypeInfo* type_filter = nullptr;
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::BeginCombo("##type", type_filter ? type_filter->name : "All types")) {
                if (with_tip(ImGui::Selectable("All types", !type_filter), "Lists every kind of asset")) type_filter = nullptr;
                u32 n_types = 0;
                const reflect::TypeInfo* const* types = world::component_types(n_types);
                for (u32 i = 0; i < n_types; ++i) {
                    if (std::strcmp(types[i]->name, "Transform") == 0 || !s.world->count(*types[i])) continue;
                    if (ImGui::Selectable(types[i]->name, type_filter == types[i])) type_filter = types[i];
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##filter", "Search objects", filter, sizeof filter);
            int count = 0;
            for (u32 slot = 0; slot < s.world->slot_count() && count < 4096; ++slot) {
                const Entity e = s.world->entity_at_slot(slot);
                if (!e.valid() || !contains_ignore_case(s.world->name(e), filter)) continue;
                if (type_filter && !s.world->get(e, *type_filter)) continue;
                rows[count++] = {e, 0};
            }
            draw_rows(s, rows, count, false);
            ImGui::EndTabItem();
        }
        if (themed_tab("Hierarchy", 1, active_tab, t.outline_tab, t.panel_background, LinearColor::hex(0xFFFFFF), wanted == 1)) {
            int count = 0;
            collect_hierarchy(*s.world, rows, count, 4096);
            draw_rows(s, rows, count, true);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ---- Details ----

// ---- Asset references and keys in Details ----

void start_rename(EditorState& s, asset::Guid guid, const char* folder, const char* current);
void toggle_preview(EditorState& s, const asset::AssetRecord& r);

} // namespace

bool asset_matches(const asset::AssetRecord& r, const char* type) {
    if (std::strcmp(type, "Mesh") == 0) return r.type == asset::AssetType::Mesh;
    if (std::strcmp(type, "Level") == 0) return r.type == asset::AssetType::Level;
    if (std::strcmp(type, "Texture") == 0) return r.type == asset::AssetType::Texture;
    if (std::strcmp(type, "LightProfile") == 0) return r.type == asset::AssetType::LightProfile;
    if (std::strcmp(type, "PcgGraph") == 0) return r.type == asset::AssetType::PcgGraph;
    if (std::strcmp(type, "Prefab") == 0) return r.type == asset::AssetType::Prefab;
    if (std::strcmp(type, "BehaviorTree") == 0) return r.type == asset::AssetType::BehaviorTree;
    if (std::strcmp(type, "UI") == 0) return r.type == asset::AssetType::UiDocument;
    if (std::strcmp(type, "Script") == 0) return r.type == asset::AssetType::Script || r.type == asset::AssetType::Graph;  // Lua or visual
    if (std::strcmp(type, "Sound") == 0) return r.type == asset::AssetType::Sound || r.type == asset::AssetType::Cue;  // a wave or a cue
    if (std::strcmp(type, "Material") == 0) {  // a material or an instance of one
        return r.type == asset::AssetType::MaterialGraph ||
               (r.type == asset::AssetType::Data && (std::strcmp(r.data_type, "Material") == 0 || std::strcmp(r.data_type, "MaterialInstance") == 0));
    }
    return r.type == asset::AssetType::Data && std::strcmp(r.data_type, type) == 0;
}

namespace {

// Combo of the assets of one type ("None" clears it); also accepts an asset dragged from the Content Browser.
bool asset_picker(EditorState& s, const char* type, char* guid_text, usize capacity) {
    bool changed = false;
    asset::Guid guid;
    const asset::AssetRecord* current = asset::parse_guid(guid_text, guid) ? s.registry->find(guid) : nullptr;
    if (ImGui::BeginCombo("##value", current ? current->name : guid_text[0] ? "(missing asset)" : "None")) {
        if (with_tip(ImGui::Selectable("None", !guid_text[0]), "No asset")) {
            guid_text[0] = '\0';
            changed = true;
        }
        for (usize i = 0; i < s.registry->count(); ++i) {
            const asset::AssetRecord& r = s.registry->get(i);
            if (!asset_matches(r, type)) continue;
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(r.name, current == &r)) {
                asset::to_string(r.guid, guid_text);
                changed = true;
            }
            ImGui::SetItemTooltip("Content/%s", r.path);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s asset (drag one here from the Content Browser)", type);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            std::memcpy(&guid, payload->Data, sizeof guid);
            const asset::AssetRecord* r = s.registry->find(guid);
            if (r && asset_matches(*r, type) && capacity >= 33) {
                asset::to_string(guid, guid_text);
                changed = true;
            } else {
                std::snprintf(s.status, sizeof s.status, "That is not a %s asset", type);
            }
        }
        ImGui::EndDragDropTarget();
    }
    return changed;
}

// Key names in a combo, plus Listen: the next key pressed is used (Escape cancels).
bool key_picker(char* key, usize capacity) {
    bool changed = false;
    if (g_listening_key == key) {
        with_tip(ImGui::Button("Press a key... (Esc cancels)", ImVec2(-1.0f, 0)), "Press the key, mouse button or gamepad button to use; Esc keeps the old one");
        int count = 0;
        const bool* keys = SDL_GetKeyboardState(&count);
        for (int k = 0; k < count; ++k) {
            if (!keys[k]) continue;
            if (k != SDL_SCANCODE_ESCAPE) {
                std::snprintf(key, capacity, "%s", SDL_GetScancodeName(static_cast<SDL_Scancode>(k)));
                changed = true;
            }
            g_listening_key = nullptr;
            break;
        }
        const char* mouse[3] = {"Left Mouse Button", "Right Mouse Button", "Middle Mouse Button"};
        for (int b = 0; b < 3 && g_listening_key && !ImGui::IsItemHovered(); ++b) {
            if (!ImGui::IsMouseClicked(b)) continue;
            std::snprintf(key, capacity, "%s", mouse[b]);
            changed = true;
            g_listening_key = nullptr;
        }
        return changed;
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70.0f);
    if (ImGui::BeginCombo("##value", key[0] ? key : "None", ImGuiComboFlags_HeightLarge)) {
        u32 count = 0;
        const char* const* names = input::key_names(count);
        for (u32 i = 0; i < count; ++i) {
            if (ImGui::Selectable(names[i], std::strcmp(names[i], key) == 0)) {
                std::snprintf(key, capacity, "%s", names[i]);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Listen", ImVec2(-1.0f, 0))) g_listening_key = key;
    ImGui::SetItemTooltip("Press the key, mouse button or gamepad button to use");
    return changed;
}

// True if the comma-separated list holds exactly `tag` (not just a child or parent of it).
bool has_exact_tag(const char* tags, const char* tag) {
    const usize n = std::strlen(tag);
    for (const char* p = tags; *p;) {
        while (*p == ' ' || *p == ',') ++p;
        const char* end = p;
        while (*end && *end != ',') ++end;
        const char* last = end;
        while (last > p && last[-1] == ' ') --last;
        if (static_cast<usize>(last - p) == n && std::strncmp(p, tag, n) == 0) return true;
        p = end;
    }
    return false;
}

// Gameplay tags: the text (typed freely), plus a "+" menu of the project's tags not on it yet.
bool tag_picker(EditorState& s, char* tags, usize capacity) {
    bool changed = false;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28.0f);
    changed |= ImGui::InputText("##tags", tags, capacity);
    ImGui::SameLine();
    if (ImGui::Button("+", ImVec2(-1.0f, 0))) ImGui::OpenPopup("tag_menu");
    ImGui::SetItemTooltip("Add a tag from Project Settings > Gameplay Tags");
    if (ImGui::BeginPopup("tag_menu")) {
        u32 offered = 0;
        for (const GameplayTagDef& def : s.project.gameplay_tags) {
            if (!def.tag[0] || has_exact_tag(tags, def.tag)) continue;  // already there
            ++offered;
            if (ImGui::MenuItem(def.tag)) {
                const usize used = std::strlen(tags);
                std::snprintf(tags + used, capacity - used, "%s%s", used ? ", " : "", def.tag);
                changed = true;
            }
            if (def.comment[0]) ImGui::SetItemTooltip("%s", def.comment);
        }
        if (!offered) ImGui::TextDisabled(s.project.gameplay_tags.count ? "Every project tag is on it" : "No tags yet: add them in Project Settings > Gameplay Tags");
        ImGui::EndPopup();
    }
    return changed;
}

bool details_widget(void* user, const reflect::Property& p, u8* field, bool& changed) {
    auto& s = *static_cast<EditorState*>(user);
    if (p.type != reflect::FieldType::String) return false;
    if (std::strcmp(p.widget, "Tags") == 0) {
        changed |= tag_picker(s, reinterpret_cast<char*>(field), p.size);
        return true;
    }
    if (std::strcmp(p.widget, "NativeScript") == 0) {  // a C / C++ script from the game code
        char* value = reinterpret_cast<char*>(field);
        const script::NativeModule& m = game_module::module();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##value", value[0] ? value : "None")) {
            if (with_tip(ImGui::Selectable("None", !value[0]), "No value")) {
                value[0] = '\0';
                changed = true;
            }
            for (u32 i = 0; i < m.count; ++i) {
                if (ImGui::Selectable(m.names[i], std::strcmp(value, m.names[i]) == 0)) {
                    std::snprintf(value, p.size, "%s", m.names[i]);
                    changed = true;
                }
            }
            if (m.count == 0) ImGui::TextDisabled(game_module::loaded() ? "The game code has no scripts" : "No C / C++ scripts yet: Tools > New C / C++ Script");
            ImGui::EndCombo();
        }
        if (value[0] && !m.find(value)) ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "Not in the compiled game code (Ctrl+Alt+F11?)");
        return true;
    }
    if (std::strncmp(p.widget, "Asset:", 6) == 0) {
        changed |= asset_picker(s, p.widget + 6, reinterpret_cast<char*>(field), p.size);
        return true;
    }
    if (std::strcmp(p.widget, "Key") == 0) {
        changed |= key_picker(reinterpret_cast<char*>(field), p.size);
        return true;
    }
    return false;
}

// ---- Data assets ----

void load_selected_data(EditorState& s, const asset::AssetRecord& r) {
    g_data_type = asset::find_data_type(r.data_type);
    g_data_guid = r.guid;
    g_data_dirty = false;
    if (!g_data_type) return;
    g_data = grow(g_data_storage, g_data_type->size);
    std::memcpy(g_data, g_data_type->defaults, g_data_type->size);
    char path[300];
    s.registry->source_path(r, path, sizeof path);
    if (!asset::load_data_asset(path, *g_data_type, g_data)) std::snprintf(s.status, sizeof s.status, "Could not read %s", r.path);
    g_data_original.assign(g_data, g_data + g_data_type->size);  // what Revert goes back to
}

// Graph of one curve's keys: drag keys, double-click to add one, right-click a key to delete it; the
// selected key shows a tangent handle (dragging it makes the tangent User). `others` are drawn faint
// behind it (the other channels of a vector or colour curve).
bool draw_curve_editor(world::CurveKeys& keys, const world::CurveKeys* const* others = nullptr, u32 other_count = 0, ImU32 color = IM_COL32(240, 200, 60, 255)) {
    static f32 view[4] = {0, 1, 0, 1};  // time min / max, value min / max (kept while dragging)
    static i32 dragging = -1, selected = -1;
    static bool handle = false;  // dragging the selected key's tangent handle
    bool changed = false;
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 220.0f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("curve_canvas", size, ImGuiButtonFlags_MouseButtonLeft);
    const bool canvas_hovered = ImGui::IsItemHovered();
    if (dragging < 0 && !handle && keys.count) {  // fit the keys (and the other channels)
        view[0] = view[1] = keys[0].time;
        view[2] = view[3] = keys[0].value;
        auto fit = [&](const world::CurveKeys& list) {
            for (const world::CurveKey& k : list) {
                view[0] = k.time < view[0] ? k.time : view[0];
                view[1] = k.time > view[1] ? k.time : view[1];
                view[2] = k.value < view[2] ? k.value : view[2];
                view[3] = k.value > view[3] ? k.value : view[3];
            }
        };
        fit(keys);
        for (u32 o = 0; o < other_count; ++o) fit(*others[o]);
        const f32 pad_t = (view[1] - view[0]) * 0.1f + 0.1f, pad_v = (view[3] - view[2]) * 0.15f + 0.1f;
        view[0] -= pad_t;
        view[1] += pad_t;
        view[2] -= pad_v;
        view[3] += pad_v;
    }
    auto to_screen = [&](f32 t, f32 v) {
        return ImVec2(at.x + (t - view[0]) / (view[1] - view[0]) * size.x, at.y + size.y - (v - view[2]) / (view[3] - view[2]) * size.y);
    };
    auto to_curve = [&](ImVec2 p, f32& t, f32& v) {
        t = view[0] + (p.x - at.x) / size.x * (view[1] - view[0]);
        v = view[2] + (at.y + size.y - p.y) / size.y * (view[3] - view[2]);
    };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(at, ImVec2(at.x + size.x, at.y + size.y), IM_COL32(24, 24, 28, 255));
    const ImVec2 zero_a = to_screen(view[0], 0.0f), zero_b = to_screen(view[1], 0.0f);
    dl->AddLine(zero_a, zero_b, IM_COL32(70, 70, 80, 255));
    char label[64];
    std::snprintf(label, sizeof label, "%.3g", static_cast<double>(view[3]));
    dl->AddText(ImVec2(at.x + 4, at.y + 2), IM_COL32(150, 150, 160, 255), label);
    std::snprintf(label, sizeof label, "%.3g", static_cast<double>(view[2]));
    dl->AddText(ImVec2(at.x + 4, at.y + size.y - 16), IM_COL32(150, 150, 160, 255), label);
    std::snprintf(label, sizeof label, "%.3g s", static_cast<double>(view[1]));
    dl->AddText(ImVec2(at.x + size.x - 60, at.y + size.y - 16), IM_COL32(150, 150, 160, 255), label);
    auto plot = [&](const world::CurveKeys& list, ImU32 line, f32 thickness) {
        ImVec2 previous;
        for (int i = 0; i <= 120; ++i) {
            const f32 t = view[0] + (view[1] - view[0]) * static_cast<f32>(i) / 120.0f;
            const ImVec2 p = to_screen(t, world::evaluate(list, t));
            if (i) dl->AddLine(previous, p, line, thickness);
            previous = p;
        }
    };
    for (u32 o = 0; o < other_count; ++o) plot(*others[o], IM_COL32(150, 150, 160, 90), 1.5f);
    plot(keys, color, 2.0f);
    const ImGuiIO& io = ImGui::GetIO();
    if (selected >= static_cast<i32>(keys.count)) selected = -1;
    // The selected key's tangent handle: 40 pixels along its slope.
    ImVec2 handle_at;
    bool handle_hot = false;
    if (selected >= 0 && keys[static_cast<u32>(selected)].interp == 0) {
        const world::CurveKey& k = keys[static_cast<u32>(selected)];
        const f32 slope = k.tangent_mode == 1 ? k.tangent
                          : (static_cast<u32>(selected) == 0 || static_cast<u32>(selected) + 1 >= keys.count)
                              ? 0.0f
                              : (keys[static_cast<u32>(selected) + 1].value - keys[static_cast<u32>(selected) - 1].value) /
                                    std::fmax(keys[static_cast<u32>(selected) + 1].time - keys[static_cast<u32>(selected) - 1].time, 1e-4f);
        const ImVec2 p = to_screen(k.time, k.value);
        const f32 step = (view[1] - view[0]) * 0.05f;
        const ImVec2 q = to_screen(k.time + step, k.value + slope * step);
        const f32 len = std::sqrt((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y));
        handle_at = len > 0.0f ? ImVec2(p.x + (q.x - p.x) / len * 40.0f, p.y + (q.y - p.y) / len * 40.0f) : ImVec2(p.x + 40.0f, p.y);
        dl->AddLine(ImVec2(2 * p.x - handle_at.x, 2 * p.y - handle_at.y), handle_at, IM_COL32(200, 200, 210, 160), 1.0f);
        handle_hot = (io.MousePos.x - handle_at.x) * (io.MousePos.x - handle_at.x) + (io.MousePos.y - handle_at.y) * (io.MousePos.y - handle_at.y) < 49.0f;
        dl->AddCircleFilled(handle_at, handle_hot || handle ? 5.0f : 4.0f, k.tangent_mode == 1 ? IM_COL32(255, 160, 60, 255) : IM_COL32(200, 200, 210, 255));
    }
    i32 hovered_key = -1;
    for (u32 i = 0; i < keys.count; ++i) {
        const ImVec2 p = to_screen(keys[i].time, keys[i].value);
        const bool hot = (io.MousePos.x - p.x) * (io.MousePos.x - p.x) + (io.MousePos.y - p.y) * (io.MousePos.y - p.y) < 64.0f;
        if (hot) hovered_key = static_cast<i32>(i);
        dl->AddCircleFilled(p, hot || dragging == static_cast<i32>(i) ? 6.0f : 4.5f, selected == static_cast<i32>(i) ? IM_COL32(255, 160, 60, 255) : IM_COL32(255, 255, 255, 255));
    }
    if (canvas_hovered && hovered_key >= 0) {
        ImGui::SetTooltip("%.3f s  =  %.3f", static_cast<double>(keys[static_cast<u32>(hovered_key)].time), static_cast<double>(keys[static_cast<u32>(hovered_key)].value));
    }
    if (ImGui::IsItemActivated()) {
        if (handle_hot) {
            handle = true;
        } else if (hovered_key >= 0) {
            dragging = selected = hovered_key;
        } else {
            selected = -1;
        }
    }
    if (handle && selected >= 0) {
        world::CurveKey& k = keys[static_cast<u32>(selected)];
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            f32 t, v;
            to_curve(io.MousePos, t, v);
            if (std::fabs(t - k.time) > 1e-5f) {  // the slope from the key to the mouse
                k.tangent = (v - k.value) / (t - k.time);
                k.tangent_mode = 1;
                changed = true;
            }
        } else {
            handle = false;
        }
    }
    if (dragging >= 0 && static_cast<u32>(dragging) < keys.count) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            to_curve(io.MousePos, keys[static_cast<u32>(dragging)].time, keys[static_cast<u32>(dragging)].value);
            changed = true;
        } else {
            dragging = selected = -1;  // sorting may move it: select again with a click
            world::sort_keys(keys);
            changed = true;
        }
    }
    if (canvas_hovered && hovered_key < 0 && !handle_hot && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && keys.count < keys.capacity()) {
        world::CurveKey key;
        to_curve(io.MousePos, key.time, key.value);
        keys.push_back(key);
        world::sort_keys(keys);
        changed = true;
    }
    if (canvas_hovered && hovered_key >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && keys.count > 1) {
        keys.remove(static_cast<u32>(hovered_key));
        selected = -1;
        changed = true;
    }
    ImGui::TextDisabled("Drag keys. Double-click: add a key. Right-click a key: delete it. Click a key, then drag its handle: its tangent.");
    return changed;
}

// A Curve Vector / Curve Color: pick the channel to edit; the others show faint behind it.
bool draw_multi_curve_editor(world::CurveKeys* const* channels, const char* const* names, const ImU32* colors, u32 count) {
    static u32 channel = 0;
    if (channel >= count) channel = 0;
    for (u32 c = 0; c < count; ++c) {
        if (c) ImGui::SameLine();
        if (ImGui::RadioButton(names[c], channel == c)) channel = c;
    }
    const world::CurveKeys* others[3];
    u32 n = 0;
    for (u32 c = 0; c < count && n < 3; ++c) {
        if (c != channel) others[n++] = channels[c];
    }
    return draw_curve_editor(*channels[channel], others, n, colors[channel]);
}

} // namespace

namespace {

// An instance of a Material Graph: a button adding the graph's parameters (at their defaults) to its list.
bool fill_graph_parameters(EditorState& s, render::MaterialInstance& instance) {
    asset::Guid guid;
    const asset::AssetRecord* parent = asset::parse_guid(instance.parent, guid) ? s.registry->find(guid) : nullptr;
    if (!parent || parent->type != asset::AssetType::MaterialGraph) return false;
    if (!ImGui::Button("Add the graph's parameters")) {
        ImGui::SetItemTooltip("Lists the parent graph's Scalar / Vector Parameters below (the base settings above are not used by graphs)");
        return false;
    }
    char path[300];
    s.registry->source_path(*parent, path, sizeof path);
    script::Graph graph(default_allocator());
    script::MaterialCode code(default_allocator());
    char error[256] = "";
    if (!script::load_graph_file(graph, path) || !script::compile_material_graph(graph, code, error, sizeof error)) {
        std::snprintf(s.status, sizeof s.status, "%s: %s", parent->name, error[0] ? error : "could not read the graph");
        return false;
    }
    bool added = false;
    for (u32 i = 0; i < code.param_count; ++i) {
        bool present = false;
        for (const render::MaterialParameter& p : instance.parameters) present = present || std::strcmp(p.name, code.params[i].name) == 0;
        if (present || instance.parameters.count == instance.parameters.capacity()) continue;
        render::MaterialParameter& p = instance.parameters[instance.parameters.count++];
        std::snprintf(p.name, sizeof p.name, "%s", code.params[i].name);
        p.value = {code.params[i].value[0], code.params[i].value[1], code.params[i].value[2]};
        added = true;
    }
    std::snprintf(s.status, sizeof s.status, added ? "Added %s's parameters" : "%s has no other parameters", parent->name);
    return added;
}

} // namespace

void* edited_data_asset(const asset::Guid& guid, const reflect::TypeInfo& type) {
    return g_data_type == &type && g_data_guid == guid ? g_data : nullptr;
}

void mark_data_asset_edited() { g_data_dirty = true; }

namespace {

void copy_changed(const reflect::TypeInfo& type, const u8* before, const u8* after, u8* target);

// Bulk editing (Unreal's Property Matrix / multi-asset Details): the other selected assets like `r`
// (same type, and same data type for data assets).
std::vector<asset::AssetRecord> same_kind_selected(const EditorState& s, const asset::AssetRecord& r) {
    std::vector<asset::AssetRecord> out;
    for (u32 k = 0; k < s.asset_selection_count; ++k) {
        const asset::AssetRecord* o = s.registry->find(s.asset_selection[k]);
        if (o && !(o->guid == r.guid) && o->type == r.type && std::strcmp(o->data_type, r.data_type) == 0) out.push_back(*o);
    }
    return out;
}

void draw_data_asset_details(EditorState& s, const asset::AssetRecord& r) {
    const EditorTheme& t = s.theme;
    if (!(g_data_guid == r.guid)) load_selected_data(s, r);
    ImGui::Text("%s%s", r.name, g_data_dirty ? " *" : "");
    ImGui::TextDisabled("%s data asset  |  Content/%s", r.data_type[0] ? r.data_type : "Unknown", r.path);
    if (!g_data_type) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "The type '%s' is not registered in this editor.", r.data_type);
        return;
    }
    char path[300];
    s.registry->source_path(r, path, sizeof path);
    // Live editing: changes are written as you make them (at most ten times a second, so a drag does
    // not write every frame) and everything using the asset reloads it at once, in Play too.
    static u64 last_write = 0;
    if (g_data_dirty && SDL_GetTicks() - last_write >= 100) {
        if (asset::save_data_asset(path, *g_data_type, g_data)) {
            g_data_dirty = false;
            s.reload_assets_now = true;
        }
        last_write = SDL_GetTicks();
    }
    const bool changed = g_data_original.size() == g_data_type->size && std::memcmp(g_data_original.data(), g_data, g_data_type->size) != 0;
    ImGui::BeginDisabled(!changed);
    if (with_tip(ImGui::Button("Revert"), "Back to how it was when you selected it (edits apply live as you make them)")) {
        std::memcpy(g_data, g_data_original.data(), g_data_type->size);
        g_data_dirty = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Live: edits apply as you make them");
    const std::vector<asset::AssetRecord> others = same_kind_selected(s, r);
    if (!others.empty()) ImGui::TextDisabled("%zu %s assets selected: a change applies to all of them", others.size() + 1, r.data_type);
    ImGui::BeginChild("DataRows");
    const DetailsStyle style{{u32_color(t.details_row_a), u32_color(t.details_row_b)}, nullptr, details_widget, &s};
    std::vector<u8> before(g_data, g_data + g_data_type->size);  // what changes this frame goes to the others
    if (g_data_type == &reflect::type_of<world::Curve>() && draw_curve_editor(reinterpret_cast<world::Curve*>(g_data)->keys)) g_data_dirty = true;
        if (g_data_type == &reflect::type_of<world::CurveVector>()) {
            auto* v = reinterpret_cast<world::CurveVector*>(g_data);
            world::CurveKeys* channels[3] = {&v->x, &v->y, &v->z};
            const char* names[3] = {"X", "Y", "Z"};
            const ImU32 colors[3] = {IM_COL32(230, 70, 60, 255), IM_COL32(90, 210, 80, 255), IM_COL32(70, 130, 240, 255)};
            if (draw_multi_curve_editor(channels, names, colors, 3)) g_data_dirty = true;
        }
        if (g_data_type == &reflect::type_of<world::CurveColor>()) {
            auto* c = reinterpret_cast<world::CurveColor*>(g_data);
            world::CurveKeys* channels[4] = {&c->r, &c->g, &c->b, &c->a};
            const char* names[4] = {"R", "G", "B", "A"};
            const ImU32 colors[4] = {IM_COL32(230, 70, 60, 255), IM_COL32(90, 210, 80, 255), IM_COL32(70, 130, 240, 255), IM_COL32(220, 220, 220, 255)};
            if (draw_multi_curve_editor(channels, names, colors, 4)) g_data_dirty = true;
        }
    if (g_data_type == &reflect::type_of<render::MaterialInstance>() && fill_graph_parameters(s, *reinterpret_cast<render::MaterialInstance*>(g_data))) {
        g_data_dirty = true;
        std::memcpy(before.data(), g_data, before.size());  // its parent's parameters, not an edit to share
    }
    if (draw_properties(*g_data_type, g_data, &style)) g_data_dirty = true;
    ImGui::EndChild();
    if (!others.empty() && std::memcmp(before.data(), g_data, before.size()) != 0) {
        // shortcut: loads and writes every other asset on each changed frame (a drag too); batch it if selections get big.
        std::vector<u8> other(g_data_type->size);
        for (const asset::AssetRecord& o : others) {
            char other_path[300];
            s.registry->source_path(o, other_path, sizeof other_path);
            std::memcpy(other.data(), g_data_type->defaults, other.size());
            if (!asset::load_data_asset(other_path, *g_data_type, other.data())) continue;
            copy_changed(*g_data_type, before.data(), static_cast<const u8*>(g_data), other.data());
            asset::save_data_asset(other_path, *g_data_type, other.data());
        }
        s.reload_assets_now = true;
    }
}

// Path of a new file in the current folder: Content/<folder>/<base><extension>, numbered if taken.
std::string new_asset_path(EditorState& s, const char* base, const char* extension = asset::kDataAssetExtension) {
    const std::filesystem::path folder = std::filesystem::path(kContentDir) / s.content_folder;
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    std::filesystem::path path = folder / (std::string(base) + extension);
    for (int n = 2; std::filesystem::exists(path, error); ++n) path = folder / (std::string(base) + std::to_string(n) + extension);
    return path.generic_string();
}

const asset::AssetRecord* record_at(EditorState& s, const std::string& path) {
    const std::string relative = std::filesystem::relative(path, kContentDir).generic_string();
    for (usize i = 0; i < s.registry->count(); ++i) {
        if (relative == s.registry->get(i).path) return &s.registry->get(i);
    }
    return nullptr;
}

void create_data_asset(EditorState& s, const reflect::TypeInfo& type) {
    char base[64];
    std::snprintf(base, sizeof base, "New%s", type.name);
    const std::string path = new_asset_path(s, base);
    if (!asset::save_data_asset(path.c_str(), type, type.defaults)) return;
    s.registry->scan();
    if (const asset::AssetRecord* r = record_at(s, path)) {
        select(s, {});
        s.selected_asset = r->guid;
        load_selected_data(s, *r);
        start_rename(s, r->guid, "", r->name);
    }
}

// Mesh > Create Prefab from Parts (Unreal's import with Combine Meshes off): a prefab with one child
// per mesh node and material of the file ("<guid>#<k>" meshes in their node's space, placed by the
// node's transform: pivots stay where the artist put them), and a Material per material of the file
// (Import Materials and Textures: its maps found and copied into Content).
// The file's node tree is kept: a part sits under the part of its parent node (a door leaf under its frame keeps its
// pivot); nodes without a mesh become empty groups.
// shortcut: a group without a mesh sits at its parent's pivot (its own is not read); read it when artists need group pivots
void make_parts_prefab(EditorState& s, const asset::AssetRecord& mesh) {
    char source[300], error[256];
    s.registry->source_path(mesh, source, sizeof source);
    Array<asset::MeshPart> parts(default_allocator());
    asset::AssetMeta meta;
    s.registry->load_meta(mesh, meta);
    if (!asset::list_mesh_parts(source, parts, error, sizeof error) || parts.empty()) {
        std::snprintf(s.status, sizeof s.status, "Could not read the parts of %s: %s", mesh.path, error[0] ? error : "no triangles");
        return;
    }
    char guid[33];
    asset::to_string(mesh.guid, guid);
    MaterialImportReport report;
    char imported[256];
    import_mesh_materials(*s.registry, mesh, "", false, report, imported, sizeof imported);
    const std::vector<std::pair<std::string, std::string>>& materials = report.materials;  // file material name -> our Material's GUID
    world::World w(default_allocator());
    const Entity root = w.create(mesh.name);
    // Parents first (shorter paths), so a node's own part exists before its children look for it.
    std::vector<usize> order(parts.size());
    for (usize k = 0; k < order.size(); ++k) order[k] = k;
    std::stable_sort(order.begin(), order.end(), [&](usize a, usize b) {
        return std::count(parts[a].node_path, parts[a].node_path + std::strlen(parts[a].node_path), '/') <
               std::count(parts[b].node_path, parts[b].node_path + std::strlen(parts[b].node_path), '/');
    });
    std::vector<std::pair<std::string, Entity>> nodes;  // node path -> its object (a part, or an empty group)
    auto node_of = [&](const std::string& path) -> Entity {
        Entity parent = root;
        usize from = 0;
        while (from < path.size()) {
            const usize slash = path.find('/', from);
            const std::string prefix = path.substr(0, slash == std::string::npos ? path.size() : slash);
            Entity found;
            for (const auto& [p, e] : nodes) found = p == prefix ? e : found;
            if (!found.valid()) {  // a group with no mesh of its own
                found = w.create(prefix.substr(prefix.rfind('/') == std::string::npos ? 0 : prefix.rfind('/') + 1).c_str());
                w.set_parent(found, parent, false);
                nodes.emplace_back(prefix, found);
            }
            parent = found;
            from = slash == std::string::npos ? path.size() : slash + 1;
        }
        return parent;
    };
    for (const usize k : order) {
        const asset::MeshPart& p = parts[k];
        const std::string path = p.node_path;
        const usize slash = path.rfind('/');
        const Entity parent = slash == std::string::npos ? root : node_of(path.substr(0, slash));
        const Entity child = w.create(p.name);
        w.set_parent(child, root, false);
        auto* transform = w.get<world::Transform>(child);
        transform->position = p.position * meta.import_scale;  // the part's mesh is scaled by it too
        transform->rotation = p.rotation;
        transform->scale = p.scale;
        if (parent != root) w.set_parent(child, parent, true);  // under its parent node, where it stands
        bool known = path.empty();
        for (const auto& n : nodes) known = known || n.first == path;
        if (!known) nodes.emplace_back(path, child);  // the node's first part stands for the node
        auto* sm = w.add<world::StaticMesh>(child);
        std::snprintf(sm->mesh, sizeof sm->mesh, "%s#%zu", guid, k);
        sm->color = LinearColor::hex(0xFFFFFF);  // the material's own colour
        for (const auto& [name, id] : materials) {
            if (p.material[0] && name == p.material) std::snprintf(sm->material, sizeof sm->material, "%s", id.c_str());
        }
    }
    const std::string path = new_asset_path(s, (std::string("BP_") + mesh.name).c_str(), world::kPrefabExtension);
    if (!world::save_prefab_file(w, root, path.c_str())) {
        std::snprintf(s.status, sizeof s.status, "Could not save %s", path.c_str());
        return;
    }
    s.registry->scan();
    if (const asset::AssetRecord* r = record_at(s, path)) find_in_content_browser(s, r->guid);
    std::snprintf(s.status, sizeof s.status, "Created %s: %zu parts, %zu materials", path.c_str(), parts.size(), materials.size());
    LSD_LOG_INFO("editor", "%s", s.status);
}

constexpr char kScriptTemplate[] = R"(-- Runs on every object whose Script component points at this file (in Play).
-- self.entity is the object, self.name its name; keep your own state in self.
-- The engine API is the lsd table: see engine/script/include/lsd/script/script.h.

function on_start(self)
  lsd.log("Hello from " .. self.name)
end

function on_update(self, dt)
  -- spin around Z, 90 degrees a second
  local x, y, z = lsd.rotation(self.entity)
  lsd.set_rotation(self.entity, x, y, z + 90 * dt)
end
)";

constexpr char kJsScriptTemplate[] = R"(// Runs on every object whose Script component points at this file (in Play).
// self.entity is the object, self.name its name; keep your own state in self.
// The engine API is the lsd object, the same as Lua's: see engine/script/include/lsd/script/script.h.
// Several results come back as an array: const [x, y, z] = lsd.position(self.entity).
// Events are top-level function declarations (not const / let arrow functions).

// Shown in each object's Details (Instance Editable).
var properties = { speed: 90 };

function on_start(self) {
  lsd.log("Hello from " + self.name);
}

function on_update(self, dt) {
  // spin around Z, self.speed degrees a second
  const [x, y, z] = lsd.rotation(self.entity);
  lsd.set_rotation(self.entity, x, y, z + self.speed * dt);
}
)";

void create_script(EditorState& s, bool javascript) {
    const std::string path = new_asset_path(s, "NewScript", javascript ? ".js" : ".lua");
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return;
    std::fputs(javascript ? kJsScriptTemplate : kScriptTemplate, file);
    std::fclose(file);
    s.registry->scan();
    if (const asset::AssetRecord* r = record_at(s, path)) {
        select(s, {});
        s.selected_asset = r->guid;
        start_rename(s, r->guid, "", r->name);
    }
}

// A ready-made input setup like Unreal's templates: three actions and a context wired to them.
void create_default_input(EditorState& s) {
    struct ActionSpec { const char* name; u32 value_type; const char* description; };
    const ActionSpec specs[3] = {{"IA_Move", 2, "Walk: WASD or the left stick"}, {"IA_Look", 2, "Turn the camera: mouse or the right stick"},
                                 {"IA_Jump", 0, "Space or the bottom face button"}};
    char guids[3][33] = {};
    for (int i = 0; i < 3; ++i) {
        input::InputAction action;
        action.value_type = specs[i].value_type;
        std::snprintf(action.description, sizeof action.description, "%s", specs[i].description);
        const std::string path = new_asset_path(s, specs[i].name);
        asset::save_data_asset(path.c_str(), reflect::type_of<input::InputAction>(), &action);
        s.registry->scan();  // writes the .meta with the new GUID
        if (const asset::AssetRecord* r = record_at(s, path)) asset::to_string(r->guid, guids[i]);
    }
    input::InputMappingContext imc;
    auto add = [&](int action, const char* key, u32 axis, f32 scale) {
        input::KeyMapping m;
        std::snprintf(m.action, sizeof m.action, "%s", guids[action]);
        std::snprintf(m.key, sizeof m.key, "%s", key);
        m.axis = axis;
        m.scale = scale;
        imc.mappings.push_back(m);
    };
    add(0, "W", 1, 1.0f);
    add(0, "S", 1, -1.0f);
    add(0, "D", 0, 1.0f);
    add(0, "A", 0, -1.0f);
    add(0, "Gamepad Left Stick", 0, 1.0f);
    add(1, "Mouse XY", 0, 1.0f);
    add(1, "Gamepad Right Stick", 0, 12.0f);  // sticks read -1..1 per frame; this feels close to the mouse
    add(2, "Space", 0, 1.0f);
    add(2, "Gamepad Face Bottom", 0, 1.0f);
    const std::string path = new_asset_path(s, "IMC_Default");
    asset::save_data_asset(path.c_str(), reflect::type_of<input::InputMappingContext>(), &imc);
    s.registry->scan();
    std::snprintf(s.status, sizeof s.status, "Created IA_Move, IA_Look, IA_Jump and IMC_Default");
}

// Mesh slot for a StaticMesh: basic shapes and mesh assets in a combo; also accepts a dragged asset.
void draw_mesh_slot(EditorState& s, world::StaticMesh& mesh) {
    const char* shown = mesh.mesh;
    asset::Guid guid;
    i32 part = -1;
    char part_label[80];
    if (asset::parse_mesh_key(mesh.mesh, guid, part)) {
        const asset::AssetRecord* r = s.registry->find(guid);
        shown = r ? r->name : "(missing asset)";
        if (r && part >= 0) {
            std::snprintf(part_label, sizeof part_label, "%s (part %d)", r->name, part);
            shown = part_label;
        }
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Mesh");
    ImGui::SameLine(160.0f);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##mesh_slot", shown)) {
        for (const char* shape : {"Cube", "Sphere", "Plane"}) {
            if (ImGui::Selectable(shape, std::strcmp(mesh.mesh, shape) == 0)) {
                std::snprintf(mesh.mesh, sizeof mesh.mesh, "%s", shape);
                edited(s, "Change Mesh");
            }
        }
        ImGui::Separator();
        for (usize i = 0; i < s.registry->count(); ++i) {
            const asset::AssetRecord& r = s.registry->get(i);
            if (r.type != asset::AssetType::Mesh) continue;
            char key[33];
            asset::to_string(r.guid, key);
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(r.name, std::strcmp(mesh.mesh, key) == 0)) {
                std::snprintf(mesh.mesh, sizeof mesh.mesh, "%s", key);
                edited(s, "Change Mesh");
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            std::memcpy(&guid, payload->Data, sizeof guid);
            const asset::AssetRecord* r = s.registry->find(guid);
            if (r && r->type == asset::AssetType::Mesh) {
                asset::to_string(guid, mesh.mesh);
                edited(s, "Change Mesh");
            }
        }
        ImGui::EndDragDropTarget();
    }
}

// Skeletal Mesh > Materials (Unreal's Materials list on a mesh component): a row per material slot
// of the mesh, showing the material it is drawn with; picking another overrides that slot on this
// object only (Material Overrides), the arrow goes back to the mesh's own.
void draw_material_slots(EditorState& s, world::SkeletalMesh& mesh) {
    const SkinnedEntry* model = s.skinned ? s.skinned->get(mesh.mesh) : nullptr;
    if (!model || !model->section_count) return;
    if (!ImGui::TreeNodeEx("Materials", ImGuiTreeNodeFlags_DefaultOpen, "Materials (%u slots)", model->section_count)) return;
    if (mesh.material[0]) ImGui::TextDisabled("Material above is set: it covers every slot (clear it to use these)");
    u32 k = 0;
    for (const anim::MeshSection& section : model->model->sections) {
        if (!section.index_count || k >= model->section_count) continue;
        const u32 slot = k++;
        world::MaterialOverride* own = nullptr;
        u32 own_index = 0;
        for (u32 i = 0; i < mesh.material_overrides.count; ++i) {
            if (std::strcmp(mesh.material_overrides[i].slot, section.material) == 0) own = &mesh.material_overrides[i], own_index = i;
        }
        char value[33];
        std::snprintf(value, sizeof value, "%s", own && own->material[0] ? own->material : model->sections[slot].slot_material);
        ImGui::PushID(static_cast<int>(slot));
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%u %s", slot, section.material[0] ? section.material : "(no material)");
        ImGui::SetItemTooltip("Element %u: the triangles using \"%s\" in the file", slot, section.material);
        ImGui::SameLine(160.0f);
        const bool overridden = own && own->material[0];
        ImGui::SetNextItemWidth(overridden ? -28.0f : -1.0f);
        if (asset_picker(s, "Material", value, sizeof value)) {
            if (std::strcmp(value, model->sections[slot].slot_material) == 0) {
                if (own) mesh.material_overrides.remove(own_index);  // back to the mesh's own
            } else if (own) {
                std::snprintf(own->material, sizeof own->material, "%s", value);
            } else {
                world::MaterialOverride o;
                std::snprintf(o.slot, sizeof o.slot, "%s", section.material);
                std::snprintf(o.material, sizeof o.material, "%s", value);
                if (!mesh.material_overrides.push_back(o)) std::snprintf(s.status, sizeof s.status, "Too many material overrides on one object");
            }
            edited(s, "Change Material Slot");
        }
        if (overridden) {
            ImGui::SameLine();
            if (with_tip(ImGui::SmallButton("<"), "Back to the mesh's own material for this slot")) {
                mesh.material_overrides.remove(own_index);
                edited(s, "Reset Material Slot");
            }
        }
        ImGui::PopID();
    }
    ImGui::TreePop();
}

// Skeletal Mesh extras above its properties: the animation file's clips by name (picks Clip), and
// while an Animation Graph runs, the state it is in (Unreal's live Anim Blueprint debugging).
void draw_skeletal_readout(EditorState& s, Entity e, world::SkeletalMesh& mesh) {
    if (!s.skinned) return;
    char motion[160];
    if (s.animations && s.animations->motion_readout(e, motion, sizeof motion)) {  // the Character's Motion Matching picks the clips
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Motion Matching: %s", motion);
        // The pose-search debugger: what else was near, and why (pose vs path cost). Lower wins.
        if (ImGui::TreeNode("Pose search candidates")) {
            char lines[6][96];
            const u32 n = s.animations->motion_candidates(e, lines, 6);
            for (u32 k = 0; k < n; ++k) ImGui::Text("%u. %s", k + 1, lines[k]);
            if (!n) ImGui::TextDisabled("(none yet)");
            ImGui::TreePop();
        }
        return;
    }
    if (mesh.anim_graph[0] && s.animations) {
        const char* from = "";
        const char* state = s.animations->graph_state(*s.skinned, mesh, e, &from);
        if (state[0] && from[0]) ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "State: %s (blending from %s)", state, from);
        else if (state[0]) ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "State: %s", state);
        else ImGui::TextDisabled("State: (not running yet)");
        return;  // the graph picks the clips
    }
    const ClipSet* set = s.skinned->clips(mesh.mesh, mesh.animation);
    if (!set || set->count == 0) {
        ImGui::TextDisabled("No animations in the animation file");
        return;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Clip");
    ImGui::SameLine(160.0f);
    ImGui::SetNextItemWidth(-1.0f);
    char shown[96];
    if (mesh.clip < set->count) std::snprintf(shown, sizeof shown, "%u: %s (%.2f s)", mesh.clip, set->clips[mesh.clip].name, static_cast<f64>(set->clips[mesh.clip].duration));
    else std::snprintf(shown, sizeof shown, "%u: (no such clip)", mesh.clip);
    if (ImGui::BeginCombo("##clip_list", shown)) {
        for (u32 c = 0; c < set->count; ++c) {
            char label[96];
            std::snprintf(label, sizeof label, "%u: %s (%.2f s)", c, set->clips[c].name, static_cast<f64>(set->clips[c].duration));
            if (ImGui::Selectable(label, c == mesh.clip)) {
                mesh.clip = c;
                edited(s, "Change Clip");
            }
        }
        ImGui::EndCombo();
    }
}

// Copies each leaf value that differs between `before` and `after` into `target` (same type).
void copy_changed(const reflect::TypeInfo& type, const u8* before, const u8* after, u8* target) {
    for (u32 i = 0; i < type.property_count; ++i) {
        const reflect::Property& p = type.properties[i];
        if (p.type == reflect::FieldType::Struct) {
            copy_changed(*p.struct_type, before + p.offset, after + p.offset, target + p.offset);
        } else if (std::memcmp(before + p.offset, after + p.offset, p.size) != 0) {
            std::memcpy(target + p.offset, after + p.offset, p.size);
        }
    }
}

void draw_entity_details(EditorState& s) {
    EditorTheme& t = s.theme;
    world::World& w = *s.world;
    const Entity e = s.selected;
    char name[64];
    std::snprintf(name, sizeof name, "%s", w.name(e));
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##name", name, sizeof name)) {
        w.set_name(e, name);
        edited(s, "Rename");
    }
    if (const Entity parent = w.parent(e); parent.valid()) ImGui::TextDisabled("Attached to %s", w.name(parent));
    if (s.selection_count > 1) ImGui::TextDisabled("%u objects selected: changes apply to all that have the component", s.selection_count);
    if (g_revert.pending) {  // Revert to Prefab from a property's menu last frame
        g_revert.pending = false;
        const Entity root = w.alive(g_revert.object) ? prefab_root(w, g_revert.object) : Entity{};
        char object_path[64], file[300];
        if (root.valid() && world::instance_path(w, root, g_revert.object, object_path, sizeof object_path) &&
            prefab_path(s, w.get<world::PrefabInstance>(root)->prefab, file, sizeof file)) {
            world::set_override(w, root, object_path, g_revert.component, g_revert.property, false);
            world::update_instance_from_file(w, root, file);
            prune_selection(s);
            // The object itself may be new (children are rebuilt): select it again by path.
            const Entity again = world::find_in_instance(w, root, object_path);
            select(s, again.valid() ? again : root);
            edited(s, "Revert to Prefab");
            return;
        }
    }
    if (const Entity root = prefab_root(w, e); root.valid()) {  // like the Blueprint bar on an actor: apply or revert
        char guid[33];
        std::snprintf(guid, sizeof guid, "%s", w.get<world::PrefabInstance>(root)->prefab);
        const u32 override_count = w.get<world::PrefabInstance>(root)->overrides.count;
        char path[300];
        if (!prefab_path(s, guid, path, sizeof path)) {
            ImGui::TextColored(ImVec4(1.0f, 0.76f, 0.4f, 1.0f), "Prefab asset missing: pick one in PrefabInstance");
        } else {
            if (root == e) ImGui::TextDisabled("Prefab instance, %u override%s (gold *)", override_count, override_count == 1 ? "" : "s");
            else ImGui::TextDisabled("Part of prefab instance %s, %u override%s", w.name(root), override_count, override_count == 1 ? "" : "s");
            if (with_tip(ImGui::Button("Apply to Prefab"), "Writes this copy's changes into the prefab: every copy in every level updates") && !s.playing) {
                world::save_prefab_file(w, root, path);
                w.get<world::PrefabInstance>(root)->overrides.count = 0;  // its values are the prefab's now
                const u32 others = refresh_instances(s, guid, root);
                std::snprintf(s.status, sizeof s.status, "Saved the prefab; updated %u other instance%s", others, others == 1 ? "" : "s");
                edited(s, "Apply Prefab");
            }
            ImGui::SetItemTooltip("Saves this instance's children and components to the prefab and updates every other instance in the level");
            ImGui::SameLine();
            if (with_tip(ImGui::Button("Revert"), "Throws away this copy's changes: back to the prefab") && !s.playing) {
                w.get<world::PrefabInstance>(root)->overrides.count = 0;
                world::update_instance_from_file(w, root, path);
                prune_selection(s);
                edited(s, "Revert Prefab");
            }
            ImGui::SetItemTooltip("Throws away every change to this instance (except its placement) and reloads it from the prefab. One value: right-click it > Revert to Prefab");
            ImGui::SameLine();
            if (with_tip(ImGui::Button("Browse"), "Shows the prefab in the Content Browser")) {
                select(s, {});
                asset::Guid id;
                if (asset::parse_guid(guid, id)) s.selected_asset = id;
            }
            ImGui::SetItemTooltip("Selects the prefab in the Content Browser");
        }
        if (!s.world->alive(s.selected)) return;  // reverting removed the selected child
    }

    static char filter[64] = "";
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##details_search", "Search details", filter, sizeof filter);
    ImGui::BeginChild("DetailsRows");
    DetailsStyle style{{u32_color(t.details_row_a), u32_color(t.details_row_b)}, filter, details_widget, &s};
    style.is_overridden = details_is_overridden;
    style.revert = details_revert;
    u32 type_count = 0;
    const reflect::TypeInfo* const* types = world::component_types(type_count);
    for (u32 i = 0; i < type_count; ++i) {
        void* component = w.get(e, *types[i]);
        if (!component) continue;
        ImGui::PushID(types[i]->name);
        bool keep = true;
        if (ImGui::CollapsingHeader(types[i]->name, i == 0 ? nullptr : &keep, ImGuiTreeNodeFlags_DefaultOpen)) {
            static std::vector<u8> before_storage;  // as big as the biggest component seen (game code can add big ones)
            u8* before = grow(before_storage, types[i]->size);
            g_details_type = types[i];
            std::memcpy(before, component, types[i]->size);
            if (types[i] == &reflect::type_of<world::StaticMesh>()) draw_mesh_slot(s, *static_cast<world::StaticMesh*>(component));
            if (types[i] == &reflect::type_of<world::SkeletalMesh>()) {
                draw_skeletal_readout(s, e, *static_cast<world::SkeletalMesh*>(component));
                draw_material_slots(s, *static_cast<world::SkeletalMesh*>(component));
            }
            if (types[i] == &reflect::type_of<world::Spline>()) {  // point tools; points are moved in the viewport
                auto& points = static_cast<world::Spline*>(component)->points;
                const bool has_point = s.spline_point >= 0 && static_cast<u32>(s.spline_point) < points.count;
                if (with_tip(ImGui::Button("Add Point"), "Adds a point after the selected one (drag points in the viewport)") && points.count < points.capacity()) {
                    // After the selected point (or at the end), continuing the curve's direction.
                    const u32 after = has_point ? static_cast<u32>(s.spline_point) : points.count - 1;
                    const Vec3 a = points.count ? points[after] : Vec3{};
                    const Vec3 b = after > 0 ? points[after - 1] : a - Vec3{2, 0, 0};
                    const Vec3 next = after + 1 < points.count ? (a + points[after + 1]) * 0.5f : a + (a - b);
                    for (u32 k = points.count; k > after + 1; --k) points[k] = points[k - 1];
                    points[after + 1] = next;
                    ++points.count;
                    auto& tangents = static_cast<world::Spline*>(component)->tangents;
                    if (tangents.count > after + 1 && tangents.count < tangents.capacity()) {  // keep them lined up: the new point's is automatic
                        for (u32 k = tangents.count; k > after + 1; --k) tangents[k] = tangents[k - 1];
                        tangents[after + 1] = Vec3{};
                        ++tangents.count;
                    }
                    s.spline_point = static_cast<i32>(after + 1);
                    edited(s, "Add Spline Point");
                }
                ImGui::SameLine();
                if (with_tip(ImGui::Button("Remove Point"), "Removes the selected point") && has_point && points.count > 2) {
                    points.remove(static_cast<u32>(s.spline_point));
                    static_cast<world::Spline*>(component)->tangents.remove(static_cast<u32>(s.spline_point));  // no-op past its end
                    s.spline_point = -1;
                    edited(s, "Remove Spline Point");
                }
                ImGui::SameLine();
                ImGui::TextDisabled(has_point ? "Point %d selected" : "Click a point in the viewport", s.spline_point);
            }
            draw_properties(*types[i], component, &style);
            if (types[i] == &reflect::type_of<world::Script>()) draw_script_properties(s, *static_cast<world::Script*>(component));
            if (types[i] == &reflect::type_of<world::NativeScript>()) draw_native_properties(*static_cast<world::NativeScript*>(component));
            if (types[i] == &reflect::type_of<world::InstancedStaticMesh>()) draw_scatter(*static_cast<world::InstancedStaticMesh*>(component));
            g_details_type = nullptr;
            if (std::memcmp(before, component, types[i]->size) != 0) {
                note_changed_overrides(s, e, *types[i], before, static_cast<const u8*>(component));
                // Multi-edit: only the values that changed go to the other selected objects.
                for (u32 k = 0; k < s.selection_count; ++k) {
                    if (s.selection[k] == e) continue;
                    if (void* other = w.get(s.selection[k], *types[i])) copy_changed(*types[i], before, static_cast<const u8*>(component), static_cast<u8*>(other));
                }
                char label[96];
                std::snprintf(label, sizeof label, "Edit %s", types[i]->name);
                edited(s, label);
            }
        }
        if (!keep) {
            w.remove(e, *types[i]);
            edited(s, "Remove Component");
        }
        ImGui::PopID();
    }
    ImGui::Spacing();
    if (with_tip(ImGui::Button("+ Add Component", ImVec2(-1.0f, 0)), "Adds a component: a mesh, light, script, physics, sound... (Unreal's Add Component)")) ImGui::OpenPopup("AddComponent");
    if (ImGui::BeginPopup("AddComponent")) {
        for (u32 i = 1; i < type_count; ++i) {
            if (!w.get(e, *types[i]) && with_tip(ImGui::MenuItem(types[i]->name), types[i]->tooltip)) {
                w.add(e, *types[i]);
                edited(s, "Add Component");
            }
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();
}

void draw_details_panel(EditorState& s) {
    EditorTheme& t = s.theme;
    if (!begin_panel(kDetailsWindow)) { ImGui::End(); return; }
    if (ImGui::BeginTabBar("DetailsTabs")) {
        const asset::AssetRecord* selected = s.registry && s.selected_asset.valid() ? s.registry->find(s.selected_asset) : nullptr;
        if (themed_tab("Details", 0, s.details_tab, t.details_tab_active, t.details_tab, t.text_on_bright)) {
            if (s.world->alive(s.selected)) {
                draw_entity_details(s);
            } else if (selected && selected->type == asset::AssetType::Data) {
                draw_data_asset_details(s, *selected);
            } else if (selected) {
                ImGui::Text("%s", selected->name);
                ImGui::TextDisabled("%s  |  Content/%s", asset::to_string(selected->type), selected->path);
                const std::vector<asset::AssetRecord> others = same_kind_selected(s, *selected);
                if (with_tip(ImGui::Button("Save"), "Writes the import settings and reimports the asset; with several assets of this type selected, the settings you changed go to all of them")) {
                    // The settings changed since the file was read go to the other selected assets too.
                    asset::AssetMeta before;
                    s.registry->load_meta(*selected, before);
                    u32 also = 0;
                    for (const asset::AssetRecord& o : others) {
                        asset::AssetMeta meta;
                        if (!s.registry->load_meta(o, meta)) continue;
                        copy_changed(reflect::type_of<asset::AssetMeta>(), reinterpret_cast<const u8*>(&before), reinterpret_cast<const u8*>(&s.selected_meta), reinterpret_cast<u8*>(&meta));
                        also += s.registry->save_meta(o, meta) ? 1u : 0u;
                    }
                    // The data compiler sees the new settings and rebuilds; placed copies update live.
                    if (s.registry->save_meta(*selected, s.selected_meta)) std::snprintf(s.status, sizeof s.status, "Saved %s.meta and %u more", selected->name, also);
                    else std::snprintf(s.status, sizeof s.status, "Could not save %s.meta", selected->name);
                }
                ImGui::SameLine();
                if (with_tip(ImGui::Button("Revert"), "Throws away unsaved changes to the settings")) s.registry->load_meta(*selected, s.selected_meta);
                if (selected->type == asset::AssetType::Sound || selected->type == asset::AssetType::Cue) {
                    ImGui::SameLine();
                    if (ImGui::Button(s.sounds && s.sounds->audio()->playing(s.preview_voice) ? "Stop" : "Play")) toggle_preview(s, *selected);
                }
                if (!others.empty()) ImGui::TextDisabled("%zu %s assets selected: Save applies the changed settings to all", others.size() + 1, asset::to_string(selected->type));
                if (s.status[0]) ImGui::TextDisabled("%s", s.status);
                ImGui::BeginChild("DetailsRows");
                DetailsStyle style{{u32_color(t.details_row_a), u32_color(t.details_row_b)}, nullptr, details_widget, &s};
                style.categories = selected->type == asset::AssetType::Mesh ? "Asset,Import,Animation"
                                 : selected->type == asset::AssetType::Sound || selected->type == asset::AssetType::Cue ? "Asset,Sound"
                                 : selected->type == asset::AssetType::Texture ? "Asset,Texture" : "Asset";
                draw_details(reflect::type_of<asset::AssetMeta>(), &s.selected_meta, &style);
                ImGui::EndChild();
            } else {
                ImGui::TextDisabled("Select an object in the viewport or the Outliner,\nor an asset in the Content Browser.");
            }
            ImGui::EndTabItem();
        }
        if (themed_tab("World Partition", 1, s.details_tab, t.details_tab_active, t.details_tab, t.text_on_bright)) {
            ImGui::TextDisabled("World Partition (level streaming grid) arrives in a later milestone.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ---- Content Browser ----

std::string join_folder(const char* parent, const char* child) {
    return parent[0] ? std::string(parent) + "/" + child : std::string(child);
}

// Accepts an asset dragged onto the last item (folder tile or tree node) and moves it there.
void folder_drop_target(EditorState& s, const char* folder) {
    if (!ImGui::BeginDragDropTarget()) return;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
        asset::Guid guid;
        std::memcpy(&guid, payload->Data, sizeof guid);
        char error[200] = "";
        if (const asset::AssetRecord* r = s.registry->find(guid)) {
            if (std::strcmp(r->folder, folder) == 0) {
                // already there
            } else {
                note_moves(*s.registry, "", r->guid);  // its path in scripts: Fix Up Redirectors
                if (s.registry->move_asset(*r, folder, error, sizeof error)) std::snprintf(s.status, sizeof s.status, "Moved to Content/%s", folder);
                else std::snprintf(s.status, sizeof s.status, "Move failed: %s", error);
            }
        }
    }
    ImGui::EndDragDropTarget();
}

// Opens a file in the program Windows / the desktop associates with it (VS Code for .lua, usually).
void open_external(EditorState& s, const asset::AssetRecord& r) {
    char path[300];
    s.registry->source_path(r, path, sizeof path);
    std::error_code error;
    SDL_OpenURL(("file:///" + std::filesystem::absolute(path, error).generic_string()).c_str());
}

// Content Browser preview: plays the sound with its settings (not spatialised), or stops it.
void toggle_preview(EditorState& s, const asset::AssetRecord& r) {
    if (!s.sounds) return;
    audio::AudioSystem& a = *s.sounds->audio();
    if (a.playing(s.preview_voice)) {
        a.stop(s.preview_voice);
        s.preview_voice = 0;
        return;
    }
    char guid[33];
    asset::to_string(r.guid, guid);
    s.preview_voice = s.sounds->play(guid, false, {});
}

void show_in_explorer(const char* folder) {
    std::error_code error;
    const std::string path = std::filesystem::absolute(std::filesystem::path(kContentDir) / folder, error).generic_string();
    SDL_OpenURL(("file:///" + path).c_str());
}

void start_rename(EditorState& s, asset::Guid guid, const char* folder, const char* current) {
    s.rename_asset = guid;
    std::snprintf(s.rename_folder, sizeof s.rename_folder, "%s", folder);
    std::snprintf(s.rename_buffer, sizeof s.rename_buffer, "%s", current);
    s.open_rename = true;
}

// Opens a level, asking first if the current one has unsaved changes.
void request_open(EditorState& s, const char* path);

bool open_record(EditorState& s, const asset::AssetRecord& r) {
    if (r.type == asset::AssetType::Level) {
        char path[300];
        s.registry->source_path(r, path, sizeof path);
        request_open(s, path);
    } else if (r.type == asset::AssetType::Script) {
        open_external(s, r);
    } else if (r.type == asset::AssetType::MaterialGraph && std::filesystem::path(r.path).extension() == script::kShaderExtension) {
        open_external(s, r);  // a shader asset: code, in the code editor (saving it recompiles)
    } else if (r.type == asset::AssetType::Graph || r.type == asset::AssetType::Cue || r.type == asset::AssetType::MaterialGraph || r.type == asset::AssetType::BehaviorTree ||
               r.type == asset::AssetType::PcgGraph) {
        open_graph(s, r);
    } else if (r.type == asset::AssetType::Table) {
        open_table(s, r);
    } else if (r.type == asset::AssetType::Sound) {
        toggle_preview(s, r);
    } else if (s.particle_editor && asset_matches(r, "ParticleSystem")) {
        s.particle_editor->open(s, r);
    } else if (asset_matches(r, "Timeline")) {
        open_timeline_editor(s, r);
    } else if (asset_matches(r, "AnimGraph")) {
        open_anim_graph_editor(s, r);
    } else if (asset_matches(r, "Flipbook")) {
        open_flipbook_editor(s, r);
    } else if (r.type == asset::AssetType::Mesh) {
        open_mesh_editor(s, r);
    } else if (r.type == asset::AssetType::Prefab) {
        open_prefab_editor(s, r);
    } else if (r.type == asset::AssetType::UiDocument || r.type == asset::AssetType::UiStyle) {
        open_ui_editor(s, r);
    } else {
        return false;
    }
    return true;
}

void request_open(EditorState& s, const char* path) {
    if (s.history->dirty()) {
        std::snprintf(g_confirm_open, sizeof g_confirm_open, "%s", path);
        g_confirm_new = false;
        g_open_confirm = true;
    } else {
        std::snprintf(s.open_level_requested, sizeof s.open_level_requested, "%s", path);
    }
}

void request_new(EditorState& s) {
    if (s.history->dirty()) {
        g_confirm_open[0] = '\0';
        g_confirm_new = true;
        g_open_confirm = true;
    } else {
        s.new_level_requested = true;
    }
}

void request_save(EditorState& s, bool save_as) {
    if (!save_as && s.level_path[0]) {
        s.save_requested = true;
        return;
    }
    std::snprintf(g_save_as_name, sizeof g_save_as_name, "%s", s.level_path[0] ? std::filesystem::path(s.level_path).stem().string().c_str() : "NewLevel");
    g_open_save_as = true;
}

// Window > Audio Mixer (Unreal's submix graph with meters): every Sound Class from Master down, the
// Submix effects it runs through on the way to its parent, and how loud it is right now.
void draw_audio_mixer(EditorState& s) {
    if (!s.show_audio_mixer) return;
    ImGui::SetNextWindowSize(ImVec2(560, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Audio Mixer", &s.show_audio_mixer)) {
        ImGui::End();
        return;
    }
    audio::AudioSystem* a = s.sounds ? s.sounds->audio() : nullptr;
    if (!a || !a->class_count()) {
        ImGui::TextDisabled("No audio (started with -nosound, or no device).");
        ImGui::End();
        return;
    }
    static f32 shown[audio::AudioSystem::kMaxClasses] = {};  // the meters fall back slowly, like a VU's
    static const char* const kEffects[] = {"Reverb", "Low Pass", "High Pass", "EQ Band", "Echo", "Compressor"};
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Each Sound Class, the Submix effects it goes through, and its level (after its volume, before its effects).");
    ImGui::PopStyleColor();
    const u32 count = a->class_count();
    auto row = [&](auto&& self, u32 c, int depth) -> void {
        const f32 peak = a->take_class_peak(c);
        shown[c] = std::fmax(peak, shown[c] * 0.9f);
        ImGui::Indent(static_cast<f32>(depth) * 16.0f);
        ImGui::TextUnformatted(a->class_name(c));
        ImGui::SameLine(200.0f);
        const f32 db = 20.0f * std::log10(std::fmax(shown[c], 1e-4f));
        const f32 fill = std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);  // -60 dB .. 0 dB
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, db > -1.0f ? ImVec4(0.9f, 0.25f, 0.2f, 1) : db > -12.0f ? ImVec4(0.95f, 0.8f, 0.2f, 1) : ImVec4(0.35f, 0.85f, 0.35f, 1));
        char label[32];
        std::snprintf(label, sizeof label, shown[c] > 1e-4f ? "%.0f dB" : "-inf", static_cast<f64>(db));
        ImGui::ProgressBar(fill, ImVec2(160.0f, 0.0f), label);
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Peak level of %s: green is comfortable, yellow loud, red at or over full scale (clipping)", a->class_name(c));
        if (const u32 effects = a->class_effect_count(c)) {
            std::string chain = "-> ";
            for (u32 k = 0; k < effects; ++k) {
                const u32 type = a->class_effect_type(c, k);
                chain += type < 6 ? kEffects[type] : "?";
                chain += " -> ";
            }
            chain += a->class_name(a->class_parent(c));
            ImGui::SameLine();
            ImGui::TextDisabled("%s", chain.c_str());
        }
        ImGui::Unindent(static_cast<f32>(depth) * 16.0f);
        for (u32 k = 1; k < count; ++k) {
            if (k != c && a->class_parent(k) == c) self(self, k, depth + 1);
        }
    };
    row(row, 0, 0);
    ImGui::End();
}

// Window > Memory (Unreal's memreport / stat memory): every allocator, the process and the GPU.
void draw_memory_window(EditorState& s) {
    if (!s.show_memory) return;
    ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Memory", &s.show_memory)) {
        ImGui::End();
        return;
    }
    const f64 mib = 1024.0 * 1024.0;
    u64 process = 0, process_peak = 0;
    process_memory(process, process_peak);
    if (process) ImGui::Text("Process (RAM in use): %.1f MiB, peak %.1f MiB", static_cast<f64>(process) / mib, static_cast<f64>(process_peak) / mib);
    if (s.device) {
        const rhi::MemoryBudget gpu = s.device->query_memory_budget();
        ImGui::Text("Video memory: %.1f MiB used of a %.1f MiB budget", static_cast<f64>(gpu.usage_bytes) / mib, static_cast<f64>(gpu.budget_bytes) / mib);
    }
    AllocatorStats stats[128];
    const u32 n = allocator_stats(stats, 128);
    if (ImGui::BeginTable("allocators", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Allocator");
        ImGui::TableSetupColumn("In use (KiB)", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending);
        ImGui::TableSetupColumn("Peak / capacity (KiB)");
        ImGui::TableSetupColumn("Blocks");
        ImGui::TableHeadersRow();
        u32 order[128];
        for (u32 i = 0; i < n; ++i) order[i] = i;
        const ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs();
        const int column = sort && sort->SpecsCount ? sort->Specs[0].ColumnIndex : 1;
        const bool descending = !sort || !sort->SpecsCount || sort->Specs[0].SortDirection == ImGuiSortDirection_Descending;
        std::sort(order, order + n, [&](u32 a, u32 b) {
            const AllocatorStats &x = stats[a], &y = stats[b];
            const bool less = column == 0 ? std::strcmp(x.name, y.name) < 0 : column == 2 ? x.peak < y.peak : column == 3 ? x.allocations < y.allocations : x.bytes < y.bytes;
            const bool greater = column == 0 ? std::strcmp(x.name, y.name) > 0 : column == 2 ? x.peak > y.peak : column == 3 ? x.allocations > y.allocations : x.bytes > y.bytes;
            return descending ? greater : less;
        });
        for (u32 k = 0; k < n; ++k) {
            const AllocatorStats& a = stats[order[k]];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s%s", a.name, a.arena ? " (arena)" : "");
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", static_cast<f64>(a.bytes) / 1024.0);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", static_cast<f64>(a.peak) / 1024.0);
            ImGui::TableNextColumn();
            ImGui::Text("%zu", a.allocations);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// Rename and delete confirmation dialogs; actions happen on OK.
void draw_file_dialogs(EditorState& s) {
    if (s.open_rename) ImGui::OpenPopup("Rename");
    if (s.open_delete) ImGui::OpenPopup("Delete");
    s.open_rename = s.open_delete = false;

    if (ImGui::BeginPopupModal("Rename", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##name", s.rename_buffer, sizeof s.rename_buffer, ImGuiInputTextFlags_EnterReturnsTrue);
        const bool ok = asset::valid_name(s.rename_buffer);
        if (!ok) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Not a valid name");
        if ((with_tip(ImGui::Button("Rename"), "Renames it (references keep working: they use its ID)") || enter) && ok) {
            char error[200] = "";
            bool done = false;
            if (const asset::AssetRecord* r = s.rename_asset.valid() ? s.registry->find(s.rename_asset) : nullptr) {
                char old_path[300];
                s.registry->source_path(*r, old_path, sizeof old_path);
                note_moves(*s.registry, "", r->guid);  // names of it in scripts: Fix Up Redirectors
                done = s.registry->rename_asset(*r, s.rename_buffer, error, sizeof error);
                // Renaming the open level: keep saving to the new file.
                if (done && std::filesystem::path(old_path) == std::filesystem::path(s.level_path)) {
                    if (const asset::AssetRecord* renamed = s.registry->find(s.rename_asset)) s.registry->source_path(*renamed, s.level_path, sizeof s.level_path);
                }
            } else if (s.rename_folder[0]) {
                const std::filesystem::path from = std::filesystem::path(kContentDir) / s.rename_folder;
                const std::filesystem::path to = from.parent_path() / s.rename_buffer;
                std::error_code ec;
                if (std::filesystem::exists(to, ec)) std::snprintf(error, sizeof error, "%s already exists", s.rename_buffer);
                else {
                    note_moves(*s.registry, s.rename_folder, {});
                    std::filesystem::rename(from, to, ec);
                }
                if (ec) std::snprintf(error, sizeof error, "%s", ec.message().c_str());
                done = !error[0];
                if (done) {
                    s.registry->scan();  // assets inside keep their GUIDs: their .meta files moved too
                    s.content_folder[0] = '\0';
                }
            }
            std::snprintf(s.status, sizeof s.status, done ? "Renamed to %s" : "Rename failed: %s", done ? s.rename_buffer : error);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (with_tip(ImGui::Button("Cancel"), "Keeps the old name") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const asset::AssetRecord* r = s.delete_asset.valid() ? s.registry->find(s.delete_asset) : nullptr;
        const bool many = r && s.asset_selection_count > 1 && asset_selected(s, s.delete_asset);
        if (many) ImGui::Text("Delete the %u selected assets?\nObjects using them in levels will show nothing.", s.asset_selection_count);
        else if (r) ImGui::Text("Delete Content/%s?\nObjects using it in levels will show nothing.", r->path);
        if (r && ImGui::IsWindowAppearing()) g_references.build(*s.registry);
        if (r) {  // what still uses it (Unreal warns the same way)
            std::vector<std::string> users;
            for (u32 k = 0; k < (many ? s.asset_selection_count : 1u); ++k) {
                char g[33];
                asset::to_string(many ? s.asset_selection[k] : s.delete_asset, g);
                for (std::string& u : g_references.used_by(g)) {
                    asset::Guid ug;
                    asset::parse_guid(u.c_str(), ug);
                    bool doomed = false;  // assets being deleted together do not count
                    for (u32 j = 0; j < s.asset_selection_count && many; ++j) doomed = doomed || s.asset_selection[j] == ug;
                    if (!doomed) users.push_back(std::move(u));
                }
            }
            if (!users.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Still used by %zu asset%s:", users.size(), users.size() == 1 ? "" : "s");
                for (size_t k = 0; k < users.size() && k < 8; ++k) {
                    asset::Guid ug;
                    asset::parse_guid(users[k].c_str(), ug);
                    const asset::AssetRecord* user = s.registry->find(ug);
                    ImGui::BulletText("%s", user ? user->path : users[k].c_str());
                }
                if (users.size() > 8) ImGui::BulletText("... and %zu more", users.size() - 8);
            }
        }
        else ImGui::Text("Delete the empty folder Content/%s?", s.delete_folder);
        if (with_tip(ImGui::Button("Delete"), "Deletes for good (the file goes to nowhere, not the Recycle Bin)")) {
            char error[200] = "";
            bool done = false;
            if (many) {
                u32 deleted = 0;
                const u32 n = s.asset_selection_count;
                asset::Guid doomed[EditorState::kMaxAssetSelection];
                std::memcpy(doomed, s.asset_selection, sizeof(asset::Guid) * n);
                for (u32 k = 0; k < n; ++k) {
                    if (const asset::AssetRecord* each = s.registry->find(doomed[k])) deleted += s.registry->delete_asset(*each, error, sizeof error) ? 1u : 0u;
                }
                done = deleted == n;
                if (done) std::snprintf(error, sizeof error, "%u assets", deleted);
                s.asset_selection_count = 0;
                s.selected_asset = {};
            } else if (r) {
                const std::string name = r->name;
                done = s.registry->delete_asset(*r, error, sizeof error);
                if (done) std::snprintf(error, sizeof error, "%s", name.c_str());
                if (s.selected_asset == s.delete_asset) s.selected_asset = {};
            } else {
                std::error_code ec;
                done = std::filesystem::remove(std::filesystem::path(kContentDir) / s.delete_folder, ec);  // fails unless empty
                std::snprintf(error, sizeof error, "%s", done ? s.delete_folder : "folder is not empty");
            }
            std::snprintf(s.status, sizeof s.status, done ? "Deleted %s" : "Delete failed: %s", error);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (with_tip(ImGui::Button("Cancel"), "Keeps it") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// Draws the sub-folders of `dir` as tree nodes; clicking one selects it. Returns false if none.
// shortcut: reads the disk every frame; move to the registry's index when folders get many.
// `root` is the folder the tree starts in and `prefix` its path in the registry ("" for Content, "Plugins/<Name>" for a plugin's Content).
bool draw_folder_tree(EditorState& s, const std::filesystem::path& dir, ImGuiTreeNodeFlags flags, const std::filesystem::path& root = kContentDir,
                      const std::string& prefix = "") {
    std::error_code error;
    bool any = false;
    for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
        if (!entry.is_directory(error)) continue;
        any = true;
        const std::string name = entry.path().filename().string();
        const std::string inside = std::filesystem::relative(entry.path(), root, error).generic_string();
        const std::string relative = prefix.empty() ? inside : prefix + "/" + inside;
        const bool selected = relative == s.content_folder;
        const bool open = ImGui::TreeNodeEx(name.c_str(), flags | (selected ? ImGuiTreeNodeFlags_Selected : 0));
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            std::snprintf(s.content_folder, sizeof s.content_folder, "%s", relative.c_str());
            s.search[0] = '\0';
        }
        folder_drop_target(s, relative.c_str());
        if (open) {
            draw_folder_tree(s, entry.path(), flags, root, prefix);
            ImGui::TreePop();
        }
    }
    return any;
}

// ---- Content Browser extras: selection of several assets, duplicate, reimport, favourites, collections ----

int strcmp_nocase(const char* a, const char* b) {
    for (;; ++a, ++b) {
        const int x = std::tolower(static_cast<unsigned char>(*a)), y = std::tolower(static_cast<unsigned char>(*b));
        if (x != y || !x) return x - y;
    }
}

bool asset_selected(const EditorState& s, asset::Guid g) {
    for (u32 i = 0; i < s.asset_selection_count; ++i) {
        if (s.asset_selection[i] == g) return true;
    }
    return false;
}

void select_asset(EditorState& s, const asset::AssetRecord& r, bool add) {
    if (!add) s.asset_selection_count = 0;
    if (!asset_selected(s, r.guid) && s.asset_selection_count < EditorState::kMaxAssetSelection) s.asset_selection[s.asset_selection_count++] = r.guid;
    s.selected_asset = r.guid;
}

void deselect_asset(EditorState& s, asset::Guid g) {
    for (u32 i = 0; i < s.asset_selection_count; ++i) {
        if (s.asset_selection[i] != g) continue;
        s.asset_selection[i] = s.asset_selection[--s.asset_selection_count];
        break;
    }
    if (s.selected_asset == g) s.selected_asset = s.asset_selection_count ? s.asset_selection[s.asset_selection_count - 1] : asset::Guid{};
}

// Shows an asset in the Content Browser: its folder, selected (Unreal's Find in Content Browser, Ctrl+B).
void find_in_content_browser(EditorState& s, asset::Guid guid) {
    const asset::AssetRecord* r = s.registry ? s.registry->find(guid) : nullptr;
    if (!r) {
        std::snprintf(s.status, sizeof s.status, "That asset is not in Content/");
        return;
    }
    std::snprintf(s.content_folder, sizeof s.content_folder, "%s", r->folder);
    s.search[0] = '\0';
    s.active_collection[0] = '\0';
    s.type_filter = 0;
    select_asset(s, *r, false);
    s.show_content_tab = true;
}

// A copy next to the original ("Chair_Copy"), with its own GUID and the same import settings.
void duplicate_asset(EditorState& s, const asset::AssetRecord& r) {
    char source[300];
    s.registry->source_path(r, source, sizeof source);
    const std::filesystem::path from(source);
    std::filesystem::path to = from.parent_path() / (std::string(r.name) + "_Copy" + from.extension().string());
    std::error_code error;
    for (int n = 2; std::filesystem::exists(to, error); ++n) to = from.parent_path() / (std::string(r.name) + "_Copy" + std::to_string(n) + from.extension().string());
    std::filesystem::copy_file(from, to, error);
    if (error) {
        std::snprintf(s.status, sizeof s.status, "Duplicate failed: %s", error.message().c_str());
        return;
    }
    std::ifstream meta_in(from.string() + ".meta", std::ios::binary);
    if (meta_in) {  // same settings, new identity
        std::stringstream text;
        text << meta_in.rdbuf();
        std::string meta = text.str();
        char old_guid[33], fresh[33];
        asset::to_string(r.guid, old_guid);
        asset::to_string(asset::new_guid(), fresh);
        const size_t at = meta.find(old_guid);
        if (at != std::string::npos) meta.replace(at, 32, fresh);
        std::ofstream(to.string() + ".meta", std::ios::binary) << meta;
    }
    s.registry->scan();
    if (const asset::AssetRecord* copy = record_at(s, to.generic_string())) select_asset(s, *copy, false);
    std::snprintf(s.status, sizeof s.status, "Duplicated %s as %s", r.name, to.stem().string().c_str());
}

// Reads the source file again (Unreal's Reimport): it counts as changed, so it compiles again.
void reimport_asset(EditorState& s, const asset::AssetRecord& r) {
    char source[300];
    s.registry->source_path(r, source, sizeof source);
    std::error_code error;
    std::filesystem::last_write_time(source, std::filesystem::file_time_type::clock::now(), error);
    s.registry->scan();
    std::snprintf(s.status, sizeof s.status, error ? "Reimport failed: %s" : "Reimporting %s", error ? error.message().c_str() : r.name);
}

bool is_favourite(const EditorState& s, const char* folder) {
    for (const FavouriteFolder& f : s.settings.favourites) {
        if (std::strcmp(f.path, folder) == 0) return true;
    }
    return false;
}

void toggle_favourite(EditorState& s, const char* folder) {
    auto& list = s.settings.favourites;
    for (u32 i = 0; i < list.count; ++i) {
        if (std::strcmp(list[i].path, folder) == 0) {
            list.remove(i);
            return;
        }
    }
    FavouriteFolder f;
    std::snprintf(f.path, sizeof f.path, "%s", folder);
    list.push_back(f);
}

bool in_collection(const EditorState& s, const char* collection, asset::Guid g) {
    char guid[33];
    asset::to_string(g, guid);
    for (const CollectionEntry& e : s.settings.collections) {
        if (std::strcmp(e.collection, collection) == 0 && std::strcmp(e.guid, guid) == 0) return true;
    }
    return false;
}

void add_to_collection(EditorState& s, const char* collection, asset::Guid g) {
    if (in_collection(s, collection, g)) return;
    CollectionEntry e;
    std::snprintf(e.collection, sizeof e.collection, "%s", collection);
    asset::to_string(g, e.guid);
    if (!s.settings.collections.push_back(e)) std::snprintf(s.status, sizeof s.status, "Collections are full (512 entries)");
}

void remove_from_collection(EditorState& s, const char* collection, asset::Guid g) {
    char guid[33];
    asset::to_string(g, guid);
    auto& list = s.settings.collections;
    for (u32 i = 0; i < list.count; ++i) {
        if (std::strcmp(list[i].collection, collection) == 0 && std::strcmp(list[i].guid, guid) == 0) {
            list.remove(i);
            return;
        }
    }
}

// Distinct collection names, in first-seen order.
u32 collection_names(const EditorState& s, const char* out[64]) {
    u32 n = 0;
    for (const CollectionEntry& e : s.settings.collections) {
        bool seen = false;
        for (u32 k = 0; k < n && !seen; ++k) seen = std::strcmp(out[k], e.collection) == 0;
        if (!seen && n < 64) out[n++] = e.collection;
    }
    return n;
}

// ---- Reference Viewer ----
ReferenceIndex g_references;
std::string g_reference_target;  // the asset shown in the middle
bool g_reference_open = false;

void open_reference_viewer(EditorState& s, asset::Guid guid) {
    g_references.build(*s.registry);
    char text[33];
    asset::to_string(guid, text);
    g_reference_target = text;
    g_reference_open = true;
}

// An asset row: its name and type (or "Missing"), click to centre the viewer on it.
void reference_row(EditorState& s, const std::string& guid, bool missing_ok) {
    asset::Guid g;
    asset::parse_guid(guid.c_str(), g);
    const asset::AssetRecord* r = s.registry->find(g);
    ImGui::PushID(guid.c_str());
    if (!r) {
        if (!missing_ok) {
            ImGui::PopID();
            return;
        }
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "Missing %s", guid.c_str());
        ImGui::SameLine();
        const asset::AssetRecord* pick = s.selected_asset.valid() ? s.registry->find(s.selected_asset) : nullptr;
        ImGui::BeginDisabled(!pick);
        if (with_tip(ImGui::SmallButton("Replace with selected"), "Points this broken reference at the asset selected in the Content Browser")) {  // fix a broken reference with the Content Browser's selection
            char to[33];
            asset::to_string(pick->guid, to);
            const u32 n = replace_references(*s.registry, guid, to);
            std::snprintf(s.status, sizeof s.status, "Replaced a missing asset with %s in %u file%s (reopen the level to see it)", pick->name, n, n == 1 ? "" : "s");
            s.registry->scan();
            g_references.build(*s.registry);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Points every reference to this missing asset at the asset selected in the Content Browser");
    } else {
        char label[160];
        const char* type = r->type == asset::AssetType::Data && r->data_type[0] ? r->data_type : asset::to_string(r->type);
        std::snprintf(label, sizeof label, "%s  (%s)", r->name, type);
        if (ImGui::Selectable(label, false, ImGuiSelectableFlags_AllowDoubleClick)) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) find_in_content_browser(s, r->guid);
            else g_reference_target = guid;
        }
        ImGui::SetItemTooltip("Content/%s\nClick: show its references. Double-click: find it in the Content Browser", r->path);
    }
    ImGui::PopID();
}

void draw_reference_viewer(EditorState& s) {
    if (!g_reference_open || !s.registry) return;
    ImGui::SetNextWindowSize(ImVec2(820, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Reference Viewer", &g_reference_open)) {
        ImGui::End();
        return;
    }
    asset::Guid target;
    asset::parse_guid(g_reference_target.c_str(), target);
    const asset::AssetRecord* r = s.registry->find(target);
    if (with_tip(ImGui::Button("Refresh"), "Looks again for what uses what")) g_references.build(*s.registry);
    ImGui::SameLine();
    if (!r) {
        ImGui::TextDisabled("The asset is gone.");
        ImGui::End();
        return;
    }
    ImGui::Text("%s  -  Content/%s", r->name, r->path);
    const auto& uses = g_references.uses(g_reference_target);
    const auto used_by = g_references.used_by(g_reference_target);
    const asset::AssetRecord* pick = s.selected_asset.valid() && s.selected_asset != target ? s.registry->find(s.selected_asset) : nullptr;
    ImGui::BeginDisabled(!pick || used_by.empty());
    char replace[200];
    std::snprintf(replace, sizeof replace, "Replace its %zu reference%s with %s", used_by.size(), used_by.size() == 1 ? "" : "s", pick ? pick->name : "the selected asset");
    if (ImGui::Button(replace) && pick) {  // Unreal's Replace References / Consolidate
        char to[33];
        asset::to_string(pick->guid, to);
        const u32 n = replace_references(*s.registry, g_reference_target, to);
        std::snprintf(s.status, sizeof s.status, "%s now used instead of %s in %u file%s (reopen the level to see it)", pick->name, r->name, n, n == 1 ? "" : "s");
        s.registry->scan();
        g_references.build(*s.registry);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Every asset that uses this one uses the Content Browser's selected asset instead (then this one can be deleted)");
    ImGui::Separator();
    if (ImGui::BeginTable("refs", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
        char heading[64];
        std::snprintf(heading, sizeof heading, "Used by (%zu)", used_by.size());
        ImGui::TableSetupColumn(heading);
        std::snprintf(heading, sizeof heading, "Uses (%zu)", uses.size());
        ImGui::TableSetupColumn(heading);
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        for (const std::string& g : used_by) reference_row(s, g, false);
        if (used_by.empty()) ImGui::TextDisabled("Nothing: safe to delete");
        ImGui::TableNextColumn();
        for (const std::string& g : uses) reference_row(s, g, true);
        ImGui::EndTable();
    }
    ImGui::End();
}

// ---- Size Map, Asset Audit, Fix Up Redirectors ----

std::string size_text(u64 bytes) {
    char out[32];
    if (bytes >= 1048576) std::snprintf(out, sizeof out, "%.1f MB", static_cast<f64>(bytes) / 1048576.0);
    else std::snprintf(out, sizeof out, "%.1f KB", static_cast<f64>(bytes) / 1024.0);
    return out;
}

std::string g_size_map_title;               // the asset or folder measured
std::vector<std::string> g_size_map_roots;  // its GUIDs
std::vector<SizeEntry> g_size_map;
bool g_size_map_open = false;

// An asset name, a folder (relative to Content/) or "" for everything; false if neither matches.
bool open_size_map_for(EditorState& s, const char* name_or_folder) {
    g_size_map_roots.clear();
    const std::string folder = name_or_folder;
    for (usize i = 0; i < s.registry->count(); ++i) {
        const asset::AssetRecord& r = s.registry->get(i);
        const bool in_folder = folder.empty() || std::strncmp(r.path, (folder + "/").c_str(), folder.size() + 1) == 0;
        if (std::strcmp(r.name, name_or_folder) != 0 && !in_folder) continue;
        char g[33];
        asset::to_string(r.guid, g);
        g_size_map_roots.emplace_back(g);
    }
    if (g_size_map_roots.empty()) return false;
    g_size_map_title = folder.empty() ? "Content" : folder;
    g_size_map = size_map(*s.registry, g_size_map_roots);
    g_size_map_open = true;
    return true;
}

void open_size_map_for_selection(EditorState& s) {
    g_size_map_roots.clear();
    for (u32 k = 0; k < s.asset_selection_count; ++k) {
        char g[33];
        asset::to_string(s.asset_selection[k], g);
        g_size_map_roots.emplace_back(g);
    }
    const asset::AssetRecord* first = s.asset_selection_count ? s.registry->find(s.asset_selection[0]) : nullptr;
    if (!first) return;
    g_size_map_title = s.asset_selection_count > 1 ? std::to_string(s.asset_selection_count) + " assets" : std::string(first->name);
    g_size_map = size_map(*s.registry, g_size_map_roots);
    g_size_map_open = true;
}

// Sorted bars (Unreal's Size Map draws a treemap; bars read the same sizes).
// shortcut: file sizes of the sources, not of the cooked data; add DerivedData when packages get big.
void draw_size_map(EditorState& s) {
    if (!g_size_map_open || !s.registry) return;
    ImGui::SetNextWindowSize(ImVec2(640, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Size Map", &g_size_map_open)) {
        ImGui::End();
        return;
    }
    if (with_tip(ImGui::Button("Refresh"), "Measures again (after adding or changing assets)")) g_size_map = size_map(*s.registry, g_size_map_roots);
    u64 total = 0;
    for (const SizeEntry& e : g_size_map) total += e.bytes;
    ImGui::SameLine();
    ImGui::Text("%s: %s in %zu asset%s (it and everything it uses)", g_size_map_title.c_str(), size_text(total).c_str(), g_size_map.size(), g_size_map.size() == 1 ? "" : "s");
    ImGui::Separator();
    ImGui::BeginChild("bars");
    const u64 biggest = g_size_map.empty() ? 1 : (g_size_map.front().bytes ? g_size_map.front().bytes : 1);
    const ImU32 bar = (u32_color(s.theme.content_tab) & ~IM_COL32_A_MASK) | (120u << IM_COL32_A_SHIFT);  // see-through: the label stays readable
    for (const SizeEntry& e : g_size_map) {
        if (e.record >= s.registry->count()) break;  // the registry rescanned: Refresh
        const asset::AssetRecord& r = s.registry->get(e.record);
        ImGui::PushID(static_cast<int>(e.record));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const f32 width = ImGui::GetContentRegionAvail().x, height = ImGui::GetTextLineHeightWithSpacing();
        const bool clicked = ImGui::InvisibleButton("row", ImVec2(width > 1.0f ? width : 1.0f, height));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const f32 fraction = static_cast<f32>(static_cast<f64>(e.bytes) / static_cast<f64>(biggest));
        dl->AddRectFilled(at, ImVec2(at.x + width * fraction, at.y + height - 2.0f), bar, 2.0f);
        const f64 share = total ? 100.0 * static_cast<f64>(e.bytes) / static_cast<f64>(total) : 0.0;
        char label[200];
        std::snprintf(label, sizeof label, "%s  %s  (%.1f%%)  %s", r.name, size_text(e.bytes).c_str(), share, r.type == asset::AssetType::Data && r.data_type[0] ? r.data_type : asset::to_string(r.type));
        dl->AddText(ImVec2(at.x + 4.0f, at.y + 1.0f), ImGui::GetColorU32(ImGuiCol_Text), label);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Content/%s\nClick: find it in the Content Browser", r.path);
        if (clicked) find_in_content_browser(s, r.guid);
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::End();
}

std::vector<AuditRow> g_audit;
bool g_audit_open = false;
char g_audit_folder[128] = "";
int g_audit_type = 0;  // 0 = all, else AssetType + 0
bool g_audit_unused_only = false;

void refresh_audit(EditorState& s) {
    g_references.build(*s.registry);
    static char settings[65536];  // the project settings as text: what the game starts from (as cooking does)
    const usize n = reflect::save_text(reflect::type_of<ProjectSettings>(), &s.project, settings, sizeof settings);
    const std::vector<std::string> cooked = cook_set(*s.registry, std::string(settings, n < sizeof settings ? n : sizeof settings - 1));
    g_audit = audit_assets(*s.registry, g_references, cooked, "DerivedData");
}

void open_asset_audit_window(EditorState& s) {
    refresh_audit(s);
    g_audit_open = true;
}

// A sortable table of every asset (Unreal's Asset Audit).
// shortcut: compiled size counts DerivedData/<guid>* only (textures cache by content hash, not counted); key them by GUID if that matters.
void draw_asset_audit(EditorState& s) {
    if (!g_audit_open || !s.registry) return;
    ImGui::SetNextWindowSize(ImVec2(900, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Asset Audit", &g_audit_open)) {
        ImGui::End();
        return;
    }
    if (with_tip(ImGui::Button("Refresh"), "Reads sizes and references again")) refresh_audit(s);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##folder", "Folder (e.g. Props)", g_audit_folder, sizeof g_audit_folder);
    ImGui::SetItemTooltip("Only assets under this folder of Content/");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    const char* type_name = g_audit_type ? asset::to_string(static_cast<asset::AssetType>(g_audit_type)) : "All types";
    if (ImGui::BeginCombo("##type", type_name)) {
        if (ImGui::Selectable("All types", g_audit_type == 0)) g_audit_type = 0;
        for (int k = 1; k <= static_cast<int>(asset::AssetType::UiStyle); ++k) {
            if (ImGui::Selectable(asset::to_string(static_cast<asset::AssetType>(k)), g_audit_type == k)) g_audit_type = k;
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Only assets of this type");
    ImGui::SameLine();
    with_tip(ImGui::Checkbox("Unused only", &g_audit_unused_only), "Only assets nothing uses: no asset names them and the game does not reach them (Cook Used Assets Only leaves them out)");
    std::vector<const AuditRow*> rows;
    u64 disk = 0, derived = 0;
    u32 unused = 0;
    const usize prefix = std::strlen(g_audit_folder);
    for (const AuditRow& row : g_audit) {
        if (row.record >= s.registry->count()) continue;  // rescanned since: Refresh
        const asset::AssetRecord& r = s.registry->get(row.record);
        if (prefix && (std::strncmp(r.path, g_audit_folder, prefix) != 0 || (r.path[prefix] != '/' && r.path[prefix] != '\0'))) continue;
        if (g_audit_type && static_cast<int>(r.type) != g_audit_type) continue;
        if (g_audit_unused_only && !row.unused) continue;
        rows.push_back(&row);
        disk += row.disk;
        derived += row.derived;
        unused += row.unused ? 1u : 0u;
    }
    ImGui::Text("%zu assets: %s on disk, %s compiled, %u unused", rows.size(), size_text(disk).c_str(), size_text(derived).c_str(), unused);
    const ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV;
    if (ImGui::BeginTable("audit", 6, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort);
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Path");
        ImGui::TableSetupColumn("Disk", ImGuiTableColumnFlags_PreferSortDescending);
        ImGui::TableSetupColumn("Compiled", ImGuiTableColumnFlags_PreferSortDescending);
        ImGui::TableSetupColumn("Used by", ImGuiTableColumnFlags_PreferSortDescending);
        ImGui::TableHeadersRow();
        if (const ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs(); sort && sort->SpecsCount) {
            const int column = sort->Specs[0].ColumnIndex;
            const bool up = sort->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            const asset::AssetRegistry& reg = *s.registry;
            std::stable_sort(rows.begin(), rows.end(), [&](const AuditRow* a, const AuditRow* b) {
                const asset::AssetRecord &ra = reg.get(a->record), &rb = reg.get(b->record);
                int c = 0;
                if (column == 0) c = strcmp_nocase(ra.name, rb.name);
                else if (column == 1) c = std::strcmp(asset::to_string(ra.type), asset::to_string(rb.type));
                else if (column == 2) c = strcmp_nocase(ra.path, rb.path);
                else {
                    const u64 x = column == 3 ? a->disk : column == 4 ? a->derived : a->users, y = column == 3 ? b->disk : column == 4 ? b->derived : b->users;
                    c = x < y ? -1 : x > y ? 1 : 0;
                }
                return up ? c < 0 : c > 0;
            });
        }
        for (const AuditRow* row : rows) {
            const asset::AssetRecord& r = s.registry->get(row->record);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(row->record));
            if (ImGui::Selectable(r.name, false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) find_in_content_browser(s, r.guid);
            ImGui::SetItemTooltip(row->unused ? "Unused: nothing names it, the game does not reach it\nDouble-click: find it in the Content Browser" : "Double-click: find it in the Content Browser");
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.type == asset::AssetType::Data && r.data_type[0] ? r.data_type : asset::to_string(r.type));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.path);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(size_text(row->disk).c_str());
            ImGui::TableNextColumn();
            if (row->derived) ImGui::TextUnformatted(size_text(row->derived).c_str());
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            if (row->unused) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "0 (unused)");
            else ImGui::Text("%u", row->users);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// Renames and moves since the last fix-up, kept in Saved/ so a restart does not lose them.
// shortcut: one PC's list; a teammate who pulls a rename runs the fix-up on the renamer's PC (or renames
// back and forth); share it in Config/ if teams rename a lot.
constexpr char kRedirectsPath[] = "Saved/AssetRedirects.txt";
std::vector<Redirect> g_redirects;
bool g_redirects_loaded = false;

void load_redirects() {
    if (g_redirects_loaded) return;
    g_redirects_loaded = true;
    std::ifstream in(kRedirectsPath);
    std::string line;
    while (std::getline(in, line)) {  // "<guid> <old path>"
        Redirect r;
        if (line.size() > 33 && asset::parse_guid(line.substr(0, 32).c_str(), r.guid)) g_redirects.push_back({line.substr(33), r.guid});
    }
}

void save_redirects() {
    std::error_code error;
    std::filesystem::create_directories("Saved", error);
    std::ofstream out(kRedirectsPath, std::ios::trunc);
    for (const Redirect& r : g_redirects) {
        char g[33];
        asset::to_string(r.guid, g);
        out << g << ' ' << r.from << '\n';
    }
}

// Before a rename or move: the asset `guid`, or every asset under the folder `path_prefix`.
void note_moves(const asset::AssetRegistry& registry, const char* path_prefix, asset::Guid guid) {
    load_redirects();
    const usize n = std::strlen(path_prefix);
    for (usize i = 0; i < registry.count(); ++i) {
        const asset::AssetRecord& r = registry.get(i);
        if (guid.valid() ? r.guid == guid : n && std::strncmp(r.path, path_prefix, n) == 0 && r.path[n] == '/') g_redirects.push_back({r.path, r.guid});
    }
    save_redirects();
}

// Rewrites the names and paths of renamed / moved assets in scripts, UI, text assets and the
// project settings; returns how many files changed.
u32 fix_up_asset_redirectors(EditorState& s) {
    load_redirects();
    u32 files = fix_up_redirectors(*s.registry, g_redirects);
    static char settings[65536];
    const usize n = reflect::save_text(reflect::type_of<ProjectSettings>(), &s.project, settings, sizeof settings);
    std::string text(settings, n < sizeof settings ? n : sizeof settings - 1);
    if (fix_up_text(*s.registry, g_redirects, text) && reflect::load_text(reflect::type_of<ProjectSettings>(), &s.project, text.data(), text.size())) {
        reflect::save_file(s.project, "Config/project.json");  // e.g. Game Default Level after a level moved
        ++files;
    }
    const usize fixed = g_redirects.size();
    g_redirects.clear();
    save_redirects();
    g_data_guid = {};  // the selected data asset reads its fixed text again
    s.reload_assets_now = true;
    std::snprintf(s.status, sizeof s.status, "Fix Up Redirectors: %zu rename%s / move%s, %u file%s updated", fixed, fixed == 1 ? "" : "s", fixed == 1 ? "" : "s", files, files == 1 ? "" : "s");
    LSD_LOG_INFO("editor", "%s", s.status);
    return files;
}

char g_new_collection[48] = "";
bool g_open_new_collection = false;

// Asset tiles for the current folder or search; meshes drag into the viewport, levels open on double-click.
void draw_asset_grid(EditorState& s) {
    const EditorTheme& t = s.theme;
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##search", "Search assets", s.search, sizeof s.search);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::Combo("##type", &s.type_filter, "All types\0Meshes\0Levels\0Data\0Prefabs\0Scripts\0Visual Scripts\0Data Tables\0Sounds\0Textures\0");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    int sort_by = static_cast<int>(s.settings.sort_by);
    if (ImGui::Combo("##sort", &sort_by, "Sort: Name\0Sort: Type\0Sort: Modified\0Sort: Size\0")) s.settings.sort_by = static_cast<u32>(sort_by);
    ImGui::SameLine();
    if (ImGui::Button(s.settings.list_view ? "Tiles" : "List")) s.settings.list_view = !s.settings.list_view;
    if (!s.settings.list_view) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::SliderFloat("##tile", &s.settings.tile_size, 48.0f, 256.0f, "%.0f px");
    }
    ImGui::SameLine();
    if (s.active_collection[0]) ImGui::TextDisabled("Collection: %s", s.active_collection);
    else ImGui::TextDisabled(s.search[0] ? "Results in all folders" : "Content/%s", s.content_folder);
    ImGui::Separator();
    if (!s.registry) return;

    static usize hits[1024];
    const asset::AssetType type = s.type_filter == 1 ? asset::AssetType::Mesh : s.type_filter == 2 ? asset::AssetType::Level
                                : s.type_filter == 3 ? asset::AssetType::Data : s.type_filter == 4 ? asset::AssetType::Prefab
                                : s.type_filter == 5 ? asset::AssetType::Script
                                : s.type_filter == 6 ? asset::AssetType::Graph
                                : s.type_filter == 7 ? asset::AssetType::Table
                                : s.type_filter == 8 ? asset::AssetType::Sound
                                : s.type_filter == 9 ? asset::AssetType::Texture : asset::AssetType::Unknown;
    usize count = 0;
    if (s.active_collection[0]) {  // a collection: its assets wherever they are, filtered by the search and type
        for (usize i = 0; i < s.registry->count() && count < 1024; ++i) {
            const asset::AssetRecord& r = s.registry->get(i);
            const bool name_ok = !s.search[0] || std::strstr(r.name, s.search) != nullptr;
            if (in_collection(s, s.active_collection, r.guid) && name_ok && (type == asset::AssetType::Unknown || r.type == type)) hits[count++] = i;
        }
    } else {
        count = s.registry->search(s.search, type, s.content_folder, hits, 1024);
    }
    // The registry as `hits` saw it: menus below (New..., Delete, Duplicate, Reimport) rescan, and the
    // tiles stop drawing for this frame when they did.
    const u32 generation = s.registry->generation();
    std::sort(hits, hits + count, [&](usize a, usize b) {
        const asset::AssetRecord &x = s.registry->get(a), &y = s.registry->get(b);
        switch (s.settings.sort_by) {
            case 1: if (x.type != y.type) return x.type < y.type; break;
            case 2: if (x.source_time != y.source_time) return x.source_time > y.source_time; break;  // newest first
            case 3: if (x.source_size != y.source_size) return x.source_size > y.source_size; break;  // biggest first
            default: break;
        }
        return strcmp_nocase(x.name, y.name) < 0;
    });

    // Sub-folders first (not while searching), like Unreal; double-click opens one.
    // shortcut: listed from disk every frame, like the folder tree.
    static char folders[64][96];
    usize folder_count = 0;
    if (!s.search[0] && !s.active_collection[0]) {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(kContentDir) / s.content_folder, error)) {
            if (folder_count < 64 && entry.is_directory(error)) {
                std::snprintf(folders[folder_count++], sizeof folders[0], "%s", entry.path().filename().string().c_str());
            }
        }
    }
    // Right-click on empty space: New Folder.
    if (ImGui::BeginPopupContextWindow("grid_menu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (with_tip(ImGui::MenuItem("New Folder"), "Makes a folder here")) {
            std::error_code error;
            const std::filesystem::path base = std::filesystem::path(kContentDir) / s.content_folder;
            std::filesystem::path dir = base / "NewFolder";
            for (int n = 2; std::filesystem::exists(dir, error); ++n) dir = base / ("NewFolder" + std::to_string(n));
            std::filesystem::create_directories(dir, error);
            start_rename(s, {}, join_folder(s.content_folder, dir.filename().string().c_str()).c_str(), dir.filename().string().c_str());
        }
        if (with_tip(ImGui::BeginMenu("New Data Asset"), "Makes a data asset (Material, Input Action, Sound Class, Curve, Game Mode...) here")) {
            u32 type_count = 0;
            const reflect::TypeInfo* const* types = asset::data_types(type_count);
            for (u32 i = 0; i < type_count; ++i) {
                if (ImGui::MenuItem(types[i]->name)) create_data_asset(s, *types[i]);
            }
            ImGui::EndMenu();
        }
        if (with_tip(ImGui::MenuItem("New Visual Script"), "Makes a Blueprint-style visual script here")) {
            const std::string path = create_graph_file((std::filesystem::path(kContentDir) / s.content_folder).generic_string().c_str(), "NewVisualScript");
            s.registry->scan();
            if (const asset::AssetRecord* r = path.empty() ? nullptr : record_at(s, path)) {
                select(s, {});
                s.selected_asset = r->guid;
                start_rename(s, r->guid, "", r->name);
            }
        }
        ImGui::SetItemTooltip("A node graph (like a Blueprint event graph); double-click to edit, pick it in a Script component");
        if (with_tip(ImGui::MenuItem("New Data Table"), "Makes a Data Table (rows and columns, edited like a spreadsheet) here")) {
            const std::string path = create_table_file((std::filesystem::path(kContentDir) / s.content_folder).generic_string().c_str(), "DT_New");
            s.registry->scan();
            if (const asset::AssetRecord* r = path.empty() ? nullptr : record_at(s, path)) {
                select(s, {});
                s.selected_asset = r->guid;
                start_rename(s, r->guid, "", r->name);
            }
        }
        ImGui::SetItemTooltip("A spreadsheet (.csv) of rows and columns; read it with lsd.table() or Get Table Value. Dropping a .csv here imports one too");
        if (ImGui::MenuItem("New Material")) create_data_asset(s, reflect::type_of<render::Material>());
        ImGui::SetItemTooltip("Base colour, metallic, roughness, normal / ORM maps, emissive. Drag it onto an object in the viewport");
        if (with_tip(ImGui::MenuItem("New Material Graph"), "Makes a node-based material (Unreal's Material Editor) here")) {
            const std::string path = create_material_graph_file((std::filesystem::path(kContentDir) / s.content_folder).generic_string().c_str(), "M_New");
            s.registry->scan();
            if (const asset::AssetRecord* r = path.empty() ? nullptr : record_at(s, path)) {
                select(s, {});
                s.selected_asset = r->guid;
                start_rename(s, r->guid, "", r->name);
            }
        }
        ImGui::SetItemTooltip("A material built from nodes (textures, maths, Time, Fresnel...) like Unreal's Material Editor. Double-click to edit; drag onto an object");
        if (with_tip(ImGui::MenuItem("New Shader"), "Makes a material written in HLSL code (Unreal's Custom node, as a whole material) here")) {
            namespace fs = std::filesystem;
            const fs::path dir = fs::path(kContentDir) / s.content_folder;
            fs::path file = dir / "SH_New.hlsl";
            for (int n = 2; fs::exists(file) && n < 100; ++n) file = dir / ("SH_New" + std::to_string(n) + ".hlsl");
            static const char kStarter[] =
                "// A shader asset: a material written in code (the function a Material Graph compiles to).\n"
                "// Declare textures and parameters on lines like these (they become lsd_tex0.. and lsd_params[0..]):\n"
                "// param: Tint 1, 0.45, 0.2\n"
                "// param: Pulse 2\n"
                "// (textures: // texture: T_Rock color   - color, normal or data; blend: // blend: translucent)\n"
                "Surface material_main(MaterialIn mi) {\n"
                "    Surface s;\n"
                "    float glow = 0.5 + 0.5 * sin(mi.time * lsd_params[1].x);\n"
                "    s.base_color = lsd_params[0].xyz;\n"
                "    s.metallic = 0.0;\n"
                "    s.roughness = 0.4;\n"
                "    s.emissive = lsd_params[0].xyz * glow;\n"
                "    s.normal = float3(0.0, 0.0, 1.0);\n"
                "    s.ao = 1.0;\n"
                "    s.opacity = 1.0;\n"
                "    return s;\n"
                "}\n";
            std::error_code ec;
            fs::create_directories(dir, ec);
            if (FILE* f = std::fopen(file.string().c_str(), "wb")) {
                std::fwrite(kStarter, 1, sizeof kStarter - 1, f);
                std::fclose(f);
            }
            s.registry->scan();
            if (const asset::AssetRecord* r = record_at(s, file.generic_string())) {
                select(s, {});
                s.selected_asset = r->guid;
                start_rename(s, r->guid, "", r->name);
            }
        }
        if (with_tip(ImGui::MenuItem("New Post Process Material"), "Makes a post-process material (code run on the whole image) here; pick it on a Post Process Volume")) {
            namespace fs = std::filesystem;
            const fs::path dir = fs::path(kContentDir) / s.content_folder;
            fs::path file = dir / "PP_New.hlsl";
            for (int n = 2; fs::exists(file) && n < 100; ++n) file = dir / ("PP_New" + std::to_string(n) + ".hlsl");
            static const char kStarter[] =
                "// A post-process material (Unreal's Post Process Material): runs on every pixel of the image before tonemapping.\n"
                "// pi.color is the scene there (linear), pi.uv its place (0..1), pi.time seconds, pi.params the volume's Post Parameters;\n"
                "// scene_color(uv) reads the scene anywhere, texel_size() is one pixel in uv.\n"
                "float3 post_main(PostIn pi) {\n"
                "    float grey = dot(pi.color, float3(0.2126, 0.7152, 0.0722));\n"
                "    float3 sepia = grey * float3(1.07, 0.74, 0.43);\n"
                "    return lerp(pi.color, sepia, saturate(pi.params.x));  // Post Parameters X: how much\n"
                "}\n";
            std::error_code ec;
            fs::create_directories(dir, ec);
            if (FILE* f = std::fopen(file.string().c_str(), "wb")) {
                std::fwrite(kStarter, 1, sizeof kStarter - 1, f);
                std::fclose(f);
            }
            s.registry->scan();
            if (const asset::AssetRecord* r = record_at(s, file.generic_string())) {
                select(s, {});
                s.selected_asset = r->guid;
                start_rename(s, r->guid, "", r->name);
            }
        }
        if (with_tip(ImGui::MenuItem("New UI Document"), "Makes an HTML / CSS game UI document (Unreal's Widget Blueprint) here")) {
            const std::string path = create_ui_document((std::filesystem::path(kContentDir) / s.content_folder).generic_string().c_str(), "UI_New");
            s.registry->scan();
            if (const asset::AssetRecord* r = path.empty() ? nullptr : record_at(s, path)) {
                select(s, {});
                s.selected_asset = r->guid;
                start_rename(s, r->guid, "", r->name);
            }
        }
        ImGui::SetItemTooltip("Game UI in HTML / CSS (Unreal's Widget Blueprint): a HUD or a menu. Open it from Lua (lsd.ui_open) or as the Game Mode's HUD");
        if (with_tip(ImGui::MenuItem("New Behavior Tree"), "Makes an AI Behavior Tree here")) {
            const std::string path = create_behavior_tree_file((std::filesystem::path(kContentDir) / s.content_folder).generic_string().c_str(), "BT_New");
            s.registry->scan();
            if (const asset::AssetRecord* r = path.empty() ? nullptr : record_at(s, path)) {
                select(s, {});
                s.selected_asset = r->guid;
                start_rename(s, r->guid, "", r->name);
            }
        }
        ImGui::SetItemTooltip("AI decisions as a tree (Selector, Sequence, Move To, Wait, conditions...). Pick it on an AI Controller");
        if (with_tip(ImGui::MenuItem("New Sound Cue"), "Makes a Sound Cue (sounds mixed by nodes: random, modulate, concatenate) here")) {
            const std::string path = create_cue_file((std::filesystem::path(kContentDir) / s.content_folder).generic_string().c_str(), "SC_New");
            s.registry->scan();
            if (const asset::AssetRecord* r = path.empty() ? nullptr : record_at(s, path)) {
                select(s, {});
                s.selected_asset = r->guid;
                start_rename(s, r->guid, "", r->name);
            }
        }
        ImGui::SetItemTooltip("A node graph that combines sounds: Random, Modulator, Mixer, Delay, Looping, Switch... Double-click to edit");
        if (with_tip(ImGui::MenuItem("New PCG Graph"), "Makes a PCG Graph (a PCG Volume's rules as nodes) here")) {
            const std::string path = create_pcg_file((std::filesystem::path(kContentDir) / s.content_folder).generic_string().c_str(), "PCG_New");
            s.registry->scan();
            if (const asset::AssetRecord* r = path.empty() ? nullptr : record_at(s, path)) {
                select(s, {});
                s.selected_asset = r->guid;
                start_rename(s, r->guid, "", r->name);
            }
        }
        if (ImGui::MenuItem("New Lua Script")) create_script(s, false);
        ImGui::SetItemTooltip("A .lua file with on_start / on_update; add a Script component to an object and pick it");
        if (ImGui::MenuItem("New JavaScript Script")) create_script(s, true);
        ImGui::SetItemTooltip("A .js file with on_start / on_update and the same lsd API as Lua; add a Script component to an object and pick it");
        if (ImGui::MenuItem("New Input Setup (Move, Look, Jump)")) create_default_input(s);
        ImGui::SetItemTooltip("Creates IA_Move, IA_Look, IA_Jump and IMC_Default mapped to WASD, mouse, Space and a gamepad");
        if (with_tip(ImGui::MenuItem("Show in Explorer"), "Opens this folder in Windows Explorer")) show_in_explorer(s.content_folder);
        ImGui::EndPopup();
    }
    if (s.selected_asset.valid() && ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_F2)) {
        if (const asset::AssetRecord* r = s.registry->find(s.selected_asset)) start_rename(s, r->guid, "", r->name);
    }
    draw_file_dialogs(s);
    if (g_open_new_collection) ImGui::OpenPopup("New Collection");
    g_open_new_collection = false;
    if (ImGui::BeginPopupModal("New Collection", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = with_tip(ImGui::InputText("Name", g_new_collection, sizeof g_new_collection, ImGuiInputTextFlags_EnterReturnsTrue), "Name of the new collection");
        if ((with_tip(ImGui::Button("Create"), "Makes the collection and adds the selected assets") || enter) && g_new_collection[0]) {
            for (u32 k = 0; k < s.asset_selection_count; ++k) add_to_collection(s, g_new_collection, s.asset_selection[k]);
            std::snprintf(s.active_collection, sizeof s.active_collection, "%s", g_new_collection);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (with_tip(ImGui::Button("Cancel"), "Keeps things as they are") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (count == 0 && folder_count == 0) {
        ImGui::TextDisabled(s.search[0] ? "No assets match." : "No assets here. Drag .fbx, .gltf or .glb files from Explorer onto the editor, or right-click for New Folder.");
        return;
    }

    const bool list = s.settings.list_view;
    const f32 kTile = s.settings.tile_size < 48.0f ? 48.0f : s.settings.tile_size;
    const f32 spacing = ImGui::GetStyle().ItemSpacing.x;
    const int columns = list ? 1
                        : static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (kTile + spacing)) > 0
                            ? static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (kTile + spacing))
                            : 1;
    const f32 row_width = ImGui::GetContentRegionAvail().x;
    const f32 row_height = ImGui::GetTextLineHeightWithSpacing();
    if (list) {  // column headings
        const ImVec2 h = ImGui::GetCursorScreenPos();
        ImDrawList* hl = ImGui::GetWindowDrawList();
        const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        hl->AddText(ImVec2(h.x + 4, h.y), dim, "Name");
        hl->AddText(ImVec2(h.x + row_width * 0.35f, h.y), dim, "Type");
        hl->AddText(ImVec2(h.x + row_width * 0.55f, h.y), dim, "Size");
        hl->AddText(ImVec2(h.x + row_width * 0.68f, h.y), dim, "Folder");
        ImGui::Dummy(ImVec2(row_width, row_height));
    }
    usize tile = 0;
    for (usize f = 0; f < folder_count; ++f, ++tile) {
        ImGui::PushID(folders[f]);
        if (tile % static_cast<usize>(columns) != 0) ImGui::SameLine();
        ImGui::BeginGroup();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Selectable("##folder", false, ImGuiSelectableFlags_AllowDoubleClick, list ? ImVec2(row_width, row_height) : ImVec2(kTile, kTile + ImGui::GetTextLineHeightWithSpacing()));
        const std::string folder_path = join_folder(s.content_folder, folders[f]);
        folder_drop_target(s, folder_path.c_str());
        if (ImGui::BeginPopupContextItem("folder_menu")) {
            if (ImGui::MenuItem(is_favourite(s, folder_path.c_str()) ? "Remove from Favourites" : "Add to Favourites")) toggle_favourite(s, folder_path.c_str());
            if (with_tip(ImGui::MenuItem("Rename"), "Renames the folder (assets inside keep working)")) start_rename(s, {}, folder_path.c_str(), folders[f]);
            if (with_tip(ImGui::MenuItem("Delete..."), "Deletes the folder (only when it is empty)")) {
                s.delete_asset = {};
                std::snprintf(s.delete_folder, sizeof s.delete_folder, "%s", folder_path.c_str());
                s.open_delete = true;
            }
            if (with_tip(ImGui::MenuItem("Size Map..."), "How much disk the folder's assets take with everything they use (Unreal's Size Map)")) open_size_map_for(s, folder_path.c_str());
            if (with_tip(ImGui::MenuItem("Fix Up Redirectors"), "Updates names and paths of renamed or moved assets in scripts, UI and text assets (GUID references never need it)")) fix_up_asset_redirectors(s);
            if (with_tip(ImGui::MenuItem("Show in Explorer"), "Opens this folder in Windows Explorer")) show_in_explorer(folder_path.c_str());
            ImGui::EndPopup();
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            std::snprintf(s.content_folder, sizeof s.content_folder, "%s", folder_path.c_str());
        }
        if (list) {
            ImDrawList* rl = ImGui::GetWindowDrawList();
            rl->AddRectFilled(ImVec2(p.x + 4, p.y + 4), ImVec2(p.x + 16, p.y + row_height - 4), u32_color(t.folder_tree_text), 2.0f);
            rl->AddText(ImVec2(p.x + 22, p.y), ImGui::GetColorU32(ImGuiCol_Text), folders[f]);
            rl->AddText(ImVec2(p.x + row_width * 0.35f, p.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), "Folder");
            ImGui::EndGroup();
            ImGui::PopID();
            continue;
        }
        // Folder shape: tab on top of a body, in the tree's folder colours.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(p.x + 12, p.y + 22), ImVec2(p.x + 46, p.y + 32), u32_color(t.folder_tree_text), 3.0f);
        dl->AddRectFilled(ImVec2(p.x + 12, p.y + 30), ImVec2(p.x + kTile - 12, p.y + kTile - 18), u32_color(t.folder_tree_text), 3.0f);
        dl->PushClipRect(ImVec2(p.x, p.y), ImVec2(p.x + kTile, p.y + kTile + ImGui::GetTextLineHeightWithSpacing()), true);
        dl->AddText(ImVec2(p.x + 4, p.y + kTile), ImGui::GetColorU32(ImGuiCol_Text), folders[f]);
        dl->PopClipRect();
        ImGui::EndGroup();
        ImGui::PopID();
    }
    for (usize n = 0; n < count; ++n, ++tile) {
        // A tile's menu can rescan (Duplicate, Create Material, Reimport...): the rest of `hits` is
        // stale then, so stop and draw the new list next frame. `r` is a copy for the same reason.
        if (s.registry->generation() != generation) break;
        const asset::AssetRecord r = s.registry->get(hits[n]);
        const bool level = r.type == asset::AssetType::Level;
        ImGui::PushID(static_cast<int>(hits[n]));
        if (tile % static_cast<usize>(columns) != 0) ImGui::SameLine();
        ImGui::BeginGroup();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool selected = r.guid == s.selected_asset || asset_selected(s, r.guid);
        if (ImGui::Selectable("##tile", selected, ImGuiSelectableFlags_AllowDoubleClick, list ? ImVec2(row_width, row_height) : ImVec2(kTile, kTile + ImGui::GetTextLineHeightWithSpacing()))) {
            select(s, {});
            if (ImGui::GetIO().KeyCtrl && asset_selected(s, r.guid)) {  // Ctrl-click a selected one: unselect it
                deselect_asset(s, r.guid);
            } else if (ImGui::GetIO().KeyShift && s.selected_asset.valid()) {  // Shift: everything between the last click and this
                usize from = count;
                for (usize k = 0; k < count; ++k) {
                    if (s.registry->get(hits[k]).guid == s.selected_asset) from = k;
                }
                const usize lo = from < n ? from : n, hi = from < n ? n : from;
                if (from == count) select_asset(s, r, false);
                for (usize k = lo; from != count && k <= hi; ++k) select_asset(s, s.registry->get(hits[k]), true);
            } else {
                select_asset(s, r, ImGui::GetIO().KeyCtrl);
            }
            s.selected_asset = r.guid;
            s.selected_meta = asset::AssetMeta{};
            s.registry->load_meta(r, s.selected_meta);
            s.status[0] = '\0';
            if (r.type == asset::AssetType::Data) load_selected_data(s, r);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) open_record(s, r);
        }
        if (ImGui::BeginPopupContextItem("asset_menu")) {
            if (level && with_tip(ImGui::MenuItem("Open Level"), "Opens this level in the editor")) {
                char path[300];
                s.registry->source_path(r, path, sizeof path);
                request_open(s, path);
            }
            if (r.type == asset::AssetType::Script && with_tip(ImGui::MenuItem("Edit Script"), "Opens the script in your code editor (VS Code)")) open_external(s, r);
            if (r.type == asset::AssetType::Graph && with_tip(ImGui::MenuItem("Edit Visual Script"), "Opens the visual script's node graph")) open_graph(s, r);
            if (r.type == asset::AssetType::Table && with_tip(ImGui::MenuItem("Edit Data Table"), "Opens the table's rows and columns")) open_table(s, r);
            if (s.particle_editor && asset_matches(r, "ParticleSystem") && with_tip(ImGui::MenuItem("Open Particle Editor"), "Opens the Particle Editor with a preview")) s.particle_editor->open(s, r);
            if ((r.type == asset::AssetType::Sound || r.type == asset::AssetType::Cue) && with_tip(ImGui::MenuItem("Play / Stop"), "Plays the sound (or stops it)")) toggle_preview(s, r);
            if (r.type == asset::AssetType::Cue && with_tip(ImGui::MenuItem("Edit Sound Cue"), "Opens the Sound Cue's node graph")) open_graph(s, r);
            if (r.type == asset::AssetType::MaterialGraph && with_tip(ImGui::MenuItem("Edit Material Graph"), "Opens the material's node graph")) open_graph(s, r);
            if (r.type == asset::AssetType::BehaviorTree && with_tip(ImGui::MenuItem("Edit Behavior Tree"), "Opens the tree's node graph")) open_graph(s, r);
            if ((r.type == asset::AssetType::UiDocument || r.type == asset::AssetType::UiStyle) && with_tip(ImGui::MenuItem("Edit UI"), "Opens the UI window: designer canvas and code")) open_ui_editor(s, r);
            if (r.type == asset::AssetType::Data && std::strcmp(r.data_type, "Material") == 0 && with_tip(ImGui::MenuItem("Create Material Instance"), "Makes a Material Instance: a copy whose values you override without touching this material")) {
                render::MaterialInstance instance;
                asset::to_string(r.guid, instance.parent);
                char base[64];
                std::snprintf(base, sizeof base, "MI_%s", r.name[0] == 'M' && r.name[1] == '_' ? r.name + 2 : r.name);
                const std::string path = new_asset_path(s, base);
                if (asset::save_data_asset(path.c_str(), reflect::type_of<render::MaterialInstance>(), &instance)) {
                    s.registry->scan();
                    if (const asset::AssetRecord* created = record_at(s, path)) {
                        select(s, {});
                        s.selected_asset = created->guid;
                        load_selected_data(s, *created);
                    }
                }
            }
            if (r.type == asset::AssetType::Texture && with_tip(ImGui::MenuItem("Create Material"), "Makes a Material that uses this texture")) {  // like Unreal: a material using this texture
                render::Material m;
                asset::to_string(r.guid, m.base_color_texture);
                char base[64];
                std::snprintf(base, sizeof base, "M_%s", r.name);
                const std::string path = new_asset_path(s, base);
                if (asset::save_data_asset(path.c_str(), reflect::type_of<render::Material>(), &m)) {
                    s.registry->scan();
                    if (const asset::AssetRecord* created = record_at(s, path)) {
                        select(s, {});
                        s.selected_asset = created->guid;
                        load_selected_data(s, *created);
                    }
                }
            }
            if (r.type == asset::AssetType::Table && with_tip(ImGui::MenuItem("Open in External Program"), "Opens the table's .csv in Excel or your spreadsheet program")) open_external(s, r);
            if (!asset_selected(s, r.guid)) select_asset(s, r, false);  // right-click acts on what is selected
            if (asset_matches(r, "Timeline") && with_tip(ImGui::MenuItem("Edit Timeline"), "Opens the Timeline editor (tracks and keys)")) open_timeline_editor(s, r);
            if (r.type == asset::AssetType::Mesh && ImGui::MenuItem("Open Mesh Editor")) open_mesh_editor(s, r);
            if (r.type == asset::AssetType::Mesh && ImGui::MenuItem("Create Prefab from Parts")) make_parts_prefab(s, r);
            if (r.type == asset::AssetType::Mesh) ImGui::SetItemTooltip("One object per mesh and material in the file, with Materials made from the file's (Unreal: Combine Meshes off)");
            if (r.type == asset::AssetType::Mesh &&
                with_tip(ImGui::MenuItem("Import Materials and Textures"),
                         "A Material per material of the file, with its texture maps copied into Textures/ (metallic / roughness packed into ORM, opacity into the colour's alpha), set on the mesh's Material Slots (Unreal's Import Materials / Import Textures)")) {
                queue_mesh_materials(r, "", false);  // stepped by the frame loop, with a progress bar
            }
            if (r.type == asset::AssetType::Prefab && with_tip(ImGui::MenuItem("Edit Prefab"), "Opens the Prefab Editor")) open_prefab_editor(s, r);
            if (with_tip(ImGui::MenuItem("Reference Viewer..."), "Shows what this asset uses and what uses it")) open_reference_viewer(s, r.guid);
            if (with_tip(ImGui::MenuItem("Size Map..."), "How much disk the selected assets take with everything they use, biggest first (Unreal's Size Map)")) open_size_map_for_selection(s);
            if (with_tip(ImGui::MenuItem("Duplicate"), "Makes a copy of the asset next to it")) duplicate_asset(s, r);
            if ((r.type == asset::AssetType::Mesh || r.type == asset::AssetType::Texture || r.type == asset::AssetType::Sound) && with_tip(ImGui::MenuItem("Reimport"), "Reads the source file again (after editing it in Blender, Photoshop...)")) {
                for (u32 k = 0; k < s.asset_selection_count; ++k) {
                    if (const asset::AssetRecord* each = s.registry->find(s.asset_selection[k])) reimport_asset(s, *each);
                }
            }
            if (with_tip(ImGui::BeginMenu("Add to Collection"), "Adds the selected assets to a collection (a group across folders)")) {
                const char* names[64];
                const u32 n_names = collection_names(s, names);
                for (u32 c = 0; c < n_names; ++c) {
                    if (ImGui::MenuItem(names[c])) {
                        for (u32 k = 0; k < s.asset_selection_count; ++k) add_to_collection(s, names[c], s.asset_selection[k]);
                    }
                }
                if (n_names) ImGui::Separator();
                if (with_tip(ImGui::MenuItem("New Collection..."), "Makes a new collection for the selected assets")) {
                    std::snprintf(g_new_collection, sizeof g_new_collection, "Collection%u", n_names + 1);
                    g_open_new_collection = true;
                }
                ImGui::EndMenu();
            }
            if (s.active_collection[0] && with_tip(ImGui::MenuItem("Remove from Collection"), "Takes the selected assets out of this collection (they are not deleted)")) {
                for (u32 k = 0; k < s.asset_selection_count; ++k) remove_from_collection(s, s.active_collection, s.asset_selection[k]);
            }
            source_control::asset_menu(r.path);
            if (with_tip(ImGui::MenuItem("Rename", "F2"), "Renames the asset (references keep working: they use its ID)")) start_rename(s, r.guid, "", r.name);
            if (ImGui::MenuItem(s.asset_selection_count > 1 ? "Delete Selected..." : "Delete...")) {
                s.delete_asset = r.guid;
                s.open_delete = true;
            }
            if (with_tip(ImGui::MenuItem("Show in Explorer"), "Opens the asset's folder in Windows Explorer")) show_in_explorer(r.folder);
            ImGui::EndPopup();
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kAssetPayload, &r.guid, sizeof r.guid);
            ImGui::Text(level ? "%s (drop on a folder to move it)" : "Place %s", r.name);
            ImGui::EndDragDropSource();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(level ? "%s\nContent/%s\nDouble-click to open" : "%s\nContent/%s", r.name, r.path);
        if (list) {  // one row: name, type, size, folder
            ImDrawList* rl = ImGui::GetWindowDrawList();
            const char* type_name = r.type == asset::AssetType::Data && r.data_type[0] ? r.data_type : asset::to_string(r.type);
            char size_text[32];
            std::snprintf(size_text, sizeof size_text, r.source_size >= 1048576 ? "%.1f MB" : "%.1f KB", static_cast<f64>(r.source_size) / (r.source_size >= 1048576 ? 1048576.0 : 1024.0));
            rl->AddText(ImVec2(p.x + 4, p.y), ImGui::GetColorU32(ImGuiCol_Text), r.name);
            rl->AddText(ImVec2(p.x + row_width * 0.35f, p.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), type_name);
            rl->AddText(ImVec2(p.x + row_width * 0.55f, p.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), size_text);
            rl->AddText(ImVec2(p.x + row_width * 0.68f, p.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), r.folder[0] ? r.folder : "(Content)");
            if (const char code = source_control::status(r.path)) {
                const char badge[2] = {code, 0};
                rl->AddText(ImVec2(p.x + row_width - 18, p.y), code == 'A' ? IM_COL32(155, 225, 31, 255) : IM_COL32(243, 195, 18, 255), badge);
            }
            ImGui::EndGroup();
            ImGui::PopID();
            continue;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 lo(p.x + 6, p.y + 6), hi(p.x + kTile - 6, p.y + kTile - 6);
        u64 thumbnail = 0;
        if (r.type == asset::AssetType::Texture && s.textures && s.device) {  // the image itself
            char key[33];
            asset::to_string(r.guid, key);
            thumbnail = s.device->debug_ui_texture(s.textures->get(key, true));
        } else if (s.thumbnails && ThumbnailCache::has_thumbnail(r)) {
            thumbnail = s.thumbnails->get(r.guid);
        }
        // Code assets: a dark tile with what they are in large white letters.
        const char* code_label = r.type == asset::AssetType::BehaviorTree ? "BT" : r.type == asset::AssetType::Script ? (std::filesystem::path(r.path).extension() == ".js" ? ".JS" : ".Lua") : nullptr;
        if (thumbnail) {
            const bool flip = s.thumbnails->flipped();
            dl->AddImage(ImTextureRef(static_cast<ImTextureID>(thumbnail)), lo, hi, ImVec2(0, flip ? 1.0f : 0.0f), ImVec2(1, flip ? 0.0f : 1.0f));
        } else if (code_label) {
            dl->AddRectFilled(lo, hi, IM_COL32(22, 22, 26, 255), 4.0f);
            const f32 font_size = (hi.y - lo.y) * 0.32f;
            const ImVec2 text = ImGui::GetFont()->CalcTextSizeA(font_size, FLT_MAX, 0.0f, code_label);
            dl->AddText(ImGui::GetFont(), font_size, ImVec2((lo.x + hi.x - text.x) * 0.5f, (lo.y + hi.y - text.y) * 0.5f), IM_COL32(255, 255, 255, 255), code_label);
        } else {  // still rendering (or no thumbnail for this type): type badge
            dl->AddRectFilled(lo, hi, u32_color(level ? t.details_tab : t.outline_row_b), 4.0f);
            const char* badge = r.type == asset::AssetType::Data && r.data_type[0] ? r.data_type : asset::to_string(r.type);
            const ImVec2 badge_size = ImGui::CalcTextSize(badge);
            dl->PushClipRect(lo, hi, true);  // long type names ("InputMappingContext") stay inside the tile
            dl->AddText(ImVec2(p.x + (badge_size.x < kTile - 12.0f ? (kTile - badge_size.x) * 0.5f : 8.0f), p.y + (kTile - badge_size.y) * 0.5f),
                        u32_color(level ? t.text_on_bright : t.text_mesh), badge);
            dl->PopClipRect();
        }
        dl->AddRect(lo, hi, u32_color(selected ? t.details_tab_active : t.text_mesh), 4.0f, 0, 2.0f);
        if (const char code = source_control::status(r.path)) {  // Git: M changed, A new, R renamed
            const ImVec2 c(hi.x - 9, lo.y + 9);
            dl->AddCircleFilled(c, 8.0f, code == 'A' ? IM_COL32(155, 225, 31, 255) : code == 'R' ? IM_COL32(89, 108, 193, 255) : IM_COL32(243, 195, 18, 255));
            const char text[2] = {code, 0};
            const ImVec2 size = ImGui::CalcTextSize(text);
            dl->AddText(ImVec2(c.x - size.x * 0.5f, c.y - size.y * 0.5f), IM_COL32(0, 0, 0, 255), text);
        }
        bool lock_mine = false;
        if (const std::string owner = source_control::locked_by(r.path, lock_mine); !owner.empty()) {  // Git LFS lock: green yours, red a teammate's
            const ImVec2 c(lo.x + 9, lo.y + 9);
            dl->AddCircleFilled(c, 8.0f, lock_mine ? IM_COL32(155, 225, 31, 255) : IM_COL32(220, 60, 50, 255));
            const ImVec2 size = ImGui::CalcTextSize("L");
            dl->AddText(ImVec2(c.x - size.x * 0.5f, c.y - size.y * 0.5f), IM_COL32(0, 0, 0, 255), "L");
            if (ImGui::IsMouseHoveringRect(ImVec2(c.x - 8, c.y - 8), ImVec2(c.x + 8, c.y + 8))) {
                if (lock_mine) ImGui::SetTooltip("Locked by you (Git LFS)");
                else ImGui::SetTooltip("Locked by %s (Git LFS)", owner.c_str());
            }
        }
        dl->PushClipRect(ImVec2(p.x, p.y), ImVec2(p.x + kTile, p.y + kTile + ImGui::GetTextLineHeightWithSpacing()), true);
        dl->AddText(ImVec2(p.x + 4, p.y + kTile), ImGui::GetColorU32(ImGuiCol_Text), r.name);
        dl->PopClipRect();
        ImGui::EndGroup();
        ImGui::PopID();
    }
}

// Place tab: basic shapes and gameplay helpers, dragged into the viewport or double-clicked.
void draw_place_panel(EditorState& s) {
    const EditorTheme& t = s.theme;
    constexpr f32 kTile = 96.0f;
    const f32 spacing = ImGui::GetStyle().ItemSpacing.x;
    const int fit = static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (kTile + spacing));
    const int columns = fit > 0 ? fit : 1;
    for (int i = 0; i < static_cast<int>(sizeof kPlaceItems / sizeof kPlaceItems[0]); ++i) {
        const PlaceItem& item = kPlaceItems[i];
        ImGui::PushID(i);
        if (i % columns != 0) ImGui::SameLine();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##place", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(kTile, kTile + ImGui::GetTextLineHeightWithSpacing())) &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            const Vec3 ahead = s.camera.position + camera_forward(s.camera) * s.camera.pivot_distance;
            place(s, item.label, item.mesh, item.component, {ahead.x, ahead.y, 0.0f});
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kPlacePayload, &i, sizeof i);
            ImGui::Text("Place %s", item.label);
            ImGui::EndDragDropSource();
        }
        ImGui::SetItemTooltip("%s%sDrag into the viewport, or double-click to place in front of the camera", place_tip(item.label), place_tip(item.label)[0] ? "\n" : "");
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c(p.x + kTile * 0.5f, p.y + kTile * 0.5f);
        const bool start = item.component && (std::strcmp(item.component, "PlayerStart") == 0 || std::strcmp(item.component, "Character") == 0);
        const bool volume = item.component && !start;
        const ImU32 color = u32_color(item.mesh ? t.text_mesh : start ? t.text_blueprint : volume ? t.text_vfx : t.text_component);
        dl->AddRect(ImVec2(p.x + 6, p.y + 6), ImVec2(p.x + kTile - 6, p.y + kTile - 6), color, 4.0f, 0, 2.0f);
        if (i == 0) dl->AddRect(ImVec2(c.x - 18, c.y - 18), ImVec2(c.x + 18, c.y + 18), color, 0.0f, 0, 2.5f);
        else if (i == 1) dl->AddCircle(c, 20.0f, color, 24, 2.5f);
        else if (i == 2) dl->AddQuad(ImVec2(c.x - 26, c.y + 10), ImVec2(c.x - 8, c.y - 12), ImVec2(c.x + 26, c.y - 12), ImVec2(c.x + 8, c.y + 10), color, 2.5f);
        else if (volume) {
            dl->AddRect(ImVec2(c.x - 22, c.y - 14), ImVec2(c.x + 22, c.y + 18), color, 0.0f, 0, 1.5f);
            dl->AddLine(ImVec2(c.x - 22, c.y - 14), ImVec2(c.x - 12, c.y - 24), color, 1.5f);
            dl->AddLine(ImVec2(c.x + 22, c.y - 14), ImVec2(c.x + 32, c.y - 24), color, 1.5f);
            dl->AddLine(ImVec2(c.x - 12, c.y - 24), ImVec2(c.x + 32, c.y - 24), color, 1.5f);
        } else if (start) {
            dl->AddCircle(ImVec2(c.x, c.y - 16), 7.0f, color, 12, 2.5f);
            dl->AddLine(ImVec2(c.x - 7, c.y - 8), ImVec2(c.x - 7, c.y + 22), color, 2.5f);
            dl->AddLine(ImVec2(c.x + 7, c.y - 8), ImVec2(c.x + 7, c.y + 22), color, 2.5f);
        } else {
            dl->AddCircle(c, 8.0f, color, 12, 2.5f);
        }
        dl->AddText(ImVec2(p.x + 4, p.y + kTile), ImGui::GetColorU32(ImGuiCol_Text), item.label);
        ImGui::PopID();
    }
}

// Console input: Enter runs the line; Tab completes; Up / Down walk the history.
char g_history[32][256];
int g_history_count = 0, g_history_pos = -1;

void remember_command(const char* line) {
    if (g_history_count == 32) {
        std::memmove(g_history[0], g_history[1], sizeof g_history[0] * 31);
        --g_history_count;
    }
    std::snprintf(g_history[g_history_count++], sizeof g_history[0], "%s", line);
    g_history_pos = -1;
}

int console_callback(ImGuiInputTextCallbackData* data) {
    int& history_pos = g_history_pos;
    const int history_count = g_history_count;
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        const char* names[32];
        const u32 n = console::complete(data->Buf, names, 32);
        if (n == 1) {
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, names[0]);
            data->InsertChars(data->CursorPos, " ");
        } else if (n > 1) {
            for (u32 i = 0; i < n; ++i) LSD_LOG_INFO("console", "  %s   %s", names[i], console::help(names[i]));
        }
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory && history_count > 0) {
        if (data->EventKey == ImGuiKey_UpArrow) history_pos = history_pos < 0 ? history_count - 1 : (history_pos > 0 ? history_pos - 1 : 0);
        else if (history_pos >= 0) history_pos = history_pos + 1 < history_count ? history_pos + 1 : -1;
        data->DeleteChars(0, data->BufTextLen);
        if (history_pos >= 0) data->InsertChars(0, g_history[history_pos]);
    }
    return 0;
}

void draw_output_log(EditorState& s) {
    const EditorTheme& t = s.theme;
    static char filter[64] = "";
    static bool show_info = true, show_warnings = true, show_errors = true;
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##log_filter", "Filter", filter, sizeof filter);
    ImGui::SameLine();
    with_tip(ImGui::Checkbox("Messages", &show_info), "Shows information lines");
    ImGui::SameLine();
    with_tip(ImGui::Checkbox("Warnings", &show_warnings), "Shows warnings (yellow)");
    ImGui::SameLine();
    with_tip(ImGui::Checkbox("Errors", &show_errors), "Shows errors (red)");
    ImGui::SameLine();
    if (with_tip(ImGui::Button("Clear"), "Empties the log")) clear_output_log();

    const f32 input_height = ImGui::GetFrameHeightWithSpacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, vec4(t.content_area));
    ImGui::BeginChild("LogLines", ImVec2(0, -input_height), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        const u32 first = (g_log_next + kLogLines - g_log_count) % kLogLines;
        for (u32 i = 0; i < g_log_count; ++i) {
            const LogLine& line = g_log[(first + i) % kLogLines];
            const bool error = line.level >= log::Level::Error, warning = line.level == log::Level::Warning;
            if ((error && !show_errors) || (warning && !show_warnings) || (!error && !warning && !show_info)) continue;
            if (filter[0] && !contains_ignore_case(line.text, filter) && !contains_ignore_case(line.channel, filter)) continue;
            const ImVec4 color = error ? ImVec4(1.0f, 0.42f, 0.38f, 1) : warning ? ImVec4(1.0f, 0.82f, 0.35f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGui::TextColored(color, "[%s] %s", line.channel, line.text);
        }
        if (g_log_scroll) ImGui::SetScrollHereY(1.0f);
        g_log_scroll = false;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    static char command[256] = "";
    ImGui::SetNextItemWidth(-1.0f);
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory;
    if (ImGui::InputTextWithHint("##console", "Console: type a command (help, stat fps, r.Grid 0...), Tab completes", command, sizeof command, flags,
                                 console_callback)) {
        if (command[0]) {
            LSD_LOG_INFO("console", "> %s", command);
            remember_command(command);
            console::execute(command);
        }
        command[0] = '\0';
        ImGui::SetKeyboardFocusHere(-1);  // keep typing
    }
}

void draw_stats(EditorState& s, ImVec2 pos, ImVec2 size) {
    if (s.show_audio_stats && s.sounds) {  // `stat audio`: Unreal's au.Debug.Sounds
        const audio::AudioSystem& a = *s.sounds->audio();
        char line[160];
        std::snprintf(line, sizeof line, "Audio: %u voice%s, %u mix%s%s", a.voice_count(), a.voice_count() == 1 ? "" : "s", a.mix_count(),
                      a.mix_count() == 1 ? "" : "es", a.has_device() ? "" : "  (no device: silent)");
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        f32 y = pos.y + size.y - 24.0f - static_cast<f32>(a.voice_count()) * 16.0f;
        fg->AddRectFilled(ImVec2(pos.x + 6, y - 4), ImVec2(pos.x + 520, pos.y + size.y - 4), IM_COL32(0, 0, 0, 150), 4.0f);  // readable on any background
        fg->AddText(ImVec2(pos.x + 12, y), IM_COL32(120, 230, 255, 255), line);
        for (u32 i = 0; i < a.voice_count(); ++i) {
            const audio::VoiceInfo v = a.voice(i);
            // Columns placed in pixels: the UI font is proportional, so padding with spaces does not line up.
            y += 16.0f;
            fg->AddText(ImVec2(pos.x + 20, y), IM_COL32(200, 240, 255, 255), v.name);
            fg->AddText(ImVec2(pos.x + 200, y), IM_COL32(200, 240, 255, 255), a.class_name(v.sound_class));
            if (v.spatial && v.lowpass < 19000.0f) {
                std::snprintf(line, sizeof line, "vol %.2f   %.1f m   muffled %.0f Hz", static_cast<f64>(v.volume), static_cast<f64>(v.distance), static_cast<f64>(v.lowpass));
            } else if (v.spatial) std::snprintf(line, sizeof line, "vol %.2f   %.1f m", static_cast<f64>(v.volume), static_cast<f64>(v.distance));
            else std::snprintf(line, sizeof line, "vol %.2f   2D", static_cast<f64>(v.volume));
            fg->AddText(ImVec2(pos.x + 340, y), IM_COL32(200, 240, 255, 255), line);
        }
    }
    if (!s.show_stats) return;
    static f32 smoothed = 0.016f;
    smoothed = smoothed * 0.95f + ImGui::GetIO().DeltaTime * 0.05f;
    static f32 game = 0, render = 0, ui = 0;
    game = game * 0.95f + s.stat_game_ms * 0.05f;
    render = render * 0.95f + s.stat_render_ms * 0.05f;
    ui = ui * 0.95f + s.stat_ui_ms * 0.05f;
    char text[320];
    std::snprintf(text, sizeof text, "%.0f fps   %.2f ms\ngame %.2f   render %.2f   ui %.2f ms (CPU)\n%u draws   %u triangles   %u culled\nengine memory %.1f MB",
                  static_cast<f64>(1.0f / smoothed), static_cast<f64>(smoothed * 1000.0f), static_cast<f64>(game), static_cast<f64>(render), static_cast<f64>(ui),
                  s.stat_draws, s.stat_triangles, s.stat_culled, static_cast<f64>(default_allocator().allocated_bytes()) / (1024.0 * 1024.0));
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    ImGui::GetForegroundDrawList()->AddText(ImVec2(pos.x + size.x - text_size.x - 14.0f, pos.y + 12.0f), u32_color(s.theme.axis_y), text);
}

void draw_content_browser(EditorState& s) {
    const EditorTheme& t = s.theme;
    static int active_tab = 0;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
    const bool open = begin_panel(kContentWindow);
    ImGui::PopStyleVar();
    g_content_hovered = open && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (!open) { ImGui::End(); return; }
    // Mouse 4 / Mouse 5 over the Content Browser: the previous / next folder (Unreal's history arrows).
    g_folder_history.track(s.content_folder);
    if (g_content_hovered && (ImGui::IsMouseClicked(3) || ImGui::IsMouseClicked(4))) {
        std::string to;
        if (g_folder_history.go(ImGui::IsMouseClicked(3), to)) std::snprintf(s.content_folder, sizeof s.content_folder, "%s", to.c_str());
    }

    // Blue bar behind the tab row.
    const ImVec2 wp = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddRectFilled(wp, ImVec2(wp.x + ImGui::GetWindowWidth(), wp.y + ImGui::GetFrameHeight() + 8.0f), u32_color(t.content_bar));
    if (ImGui::BeginTabBar("ContentTabs")) {
        const bool to_content = s.show_content_tab;  // Find in Content Browser
        s.show_content_tab = false;
        if (themed_tab("Content", 0, active_tab, t.content_tab, t.content_bar, t.text_on_bright, to_content)) {
            // Folder tree: black, lime border, yellow text and lines.
            ImGui::PushStyleColor(ImGuiCol_ChildBg, vec4(t.folder_tree_background));
            ImGui::PushStyleColor(ImGuiCol_Border, vec4(t.folder_tree_border));
            ImGui::PushStyleColor(ImGuiCol_TreeLines, vec4(t.folder_tree_text));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 2.0f);
            ImGui::BeginChild("FolderTree", ImVec2(200, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
            const ImGuiTreeNodeFlags node = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_DrawLinesToNodes | ImGuiTreeNodeFlags_SpanAvailWidth;
            ImGui::PushStyleColor(ImGuiCol_Text, vec4(t.folder_tree_border));
            if (s.settings.favourites.count && ImGui::TreeNodeEx("Favourites", node)) {
                for (u32 i = 0; i < s.settings.favourites.count; ++i) {
                    const char* path = s.settings.favourites[i].path;
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Selectable(path[0] ? path : "Content", !s.active_collection[0] && std::strcmp(path, s.content_folder) == 0)) {
                        std::snprintf(s.content_folder, sizeof s.content_folder, "%s", path);
                        s.search[0] = s.active_collection[0] = '\0';
                    }
                    if (ImGui::BeginPopupContextItem("favourite_menu")) {
                        if (with_tip(ImGui::MenuItem("Remove from Favourites"), "Unpins this folder from the top of the tree")) toggle_favourite(s, path);
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            const char* collections[64];
            const u32 collection_count = collection_names(s, collections);
            if (collection_count && ImGui::TreeNodeEx("Collections", node)) {
                for (u32 i = 0; i < collection_count; ++i) {
                    if (ImGui::Selectable(collections[i], std::strcmp(collections[i], s.active_collection) == 0)) {
                        std::snprintf(s.active_collection, sizeof s.active_collection, "%s", collections[i]);
                        s.search[0] = '\0';
                    }
                }
                ImGui::TreePop();
            }
            const bool root = ImGui::TreeNodeEx("Content", node | (s.content_folder[0] || s.active_collection[0] ? 0 : ImGuiTreeNodeFlags_Selected));
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                s.content_folder[0] = '\0';
                s.search[0] = '\0';
                s.active_collection[0] = '\0';
            }
            folder_drop_target(s, "");
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, vec4(t.folder_tree_text));
            if (root) {
                if (!draw_folder_tree(s, kContentDir, node)) ImGui::TextDisabled("(no folders)");
                ImGui::TreePop();
            }
            for (const asset::AssetMount& m : s.registry->mounts()) {  // each enabled plugin's Content/, under Plugins/<Name>
                if (!m.prefix[0]) continue;
                const bool open = ImGui::TreeNodeEx(m.prefix, node | (s.content_folder == std::string(m.prefix) ? ImGuiTreeNodeFlags_Selected : 0));
                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                    std::snprintf(s.content_folder, sizeof s.content_folder, "%s", m.prefix);
                    s.search[0] = s.active_collection[0] = '\0';
                }
                if (open) {
                    draw_folder_tree(s, m.dir, node, m.dir, m.prefix);
                    ImGui::TreePop();
                }
            }
            ImGui::PopStyleColor();
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);

            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, vec4(t.content_area));
            ImGui::BeginChild("Assets", ImVec2(0, 0), ImGuiChildFlags_None);
            draw_asset_grid(s);
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::EndTabItem();
        }
        if (themed_tab("Place", 1, active_tab, t.content_tab, t.content_bar, t.text_on_bright)) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, vec4(t.content_area));
            ImGui::BeginChild("PlaceItems", ImVec2(0, 0), ImGuiChildFlags_None);
            draw_place_panel(s);
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::EndTabItem();
        }
        if (themed_tab("Output Log", 2, active_tab, t.content_tab, t.content_bar, t.text_on_bright)) {
            draw_output_log(s);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ---- Menus and windows ----

// Keeps File > Recent Levels in step with the level being edited (opened, saved as, or new then saved).
static void note_recent_level(EditorState& s) {
    auto& recent = s.settings.recent_levels;
    if (!s.level_path[0] || (recent.count && std::strcmp(recent[0].path, s.level_path) == 0)) return;
    for (u32 i = 0; i < recent.count; ++i) {
        if (std::strcmp(recent[i].path, s.level_path) == 0) recent.remove(i--);
    }
    if (recent.count == recent.capacity()) recent.remove(recent.count - 1);
    RecentLevel entry;
    std::snprintf(entry.path, sizeof entry.path, "%s", s.level_path);
    recent.push_back(entry);
    for (u32 i = recent.count - 1; i > 0; --i) std::swap(recent[i], recent[i - 1]);  // newest first
}

void draw_menu_bar(EditorState& s) {
    note_recent_level(s);
    if (!ImGui::BeginMainMenuBar()) return;
    if (with_tip(ImGui::BeginMenu("File"), "Levels: new, open, save; validating and packaging")) {
        if (with_tip(ImGui::MenuItem("New Level", command_text(Command::NewLevel), false, !s.playing), "Starts an empty level")) request_new(s);
        if (with_tip(ImGui::MenuItem("New from Template...", nullptr, false, !s.playing),
                     "First Person, Third Person, Platformer, Side Scrolling, 2D, 2.5D or Empty: a ready level to start from (Unreal's New Project templates)")) {
            s.show_template_picker = true;
        }
        if (with_tip(ImGui::BeginMenu("Open Level", !s.playing), "Opens one of the project's levels")) {
            bool any = false;
            for (usize i = 0; i < s.registry->count(); ++i) {
                const asset::AssetRecord& r = s.registry->get(i);
                if (r.type != asset::AssetType::Level) continue;
                any = true;
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::MenuItem(r.name, r.folder)) {
                    char path[300];
                    s.registry->source_path(r, path, sizeof path);
                    request_open(s, path);
                }
                ImGui::PopID();
            }
            if (!any) ImGui::TextDisabled("No levels yet: File > Save Level As");
            ImGui::EndMenu();
        }
        if (with_tip(ImGui::BeginMenu("Recent Levels", !s.playing && s.settings.recent_levels.count > 0), "Levels opened recently")) {
            for (u32 i = 0; i < s.settings.recent_levels.count; ++i) {
                const char* path = s.settings.recent_levels[i].path;
                ImGui::PushID(static_cast<int>(i));
                const bool exists = std::filesystem::exists(path);
                if (ImGui::MenuItem(std::filesystem::path(path).stem().string().c_str(), path, false, exists)) request_open(s, path);
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (with_tip(ImGui::MenuItem("Save Level", command_text(Command::Save), false, !s.playing), "Writes the level to disk")) request_save(s, false);
        if (with_tip(ImGui::MenuItem("Save Level As...", command_text(Command::SaveAs), false, !s.playing), "Writes the level under a new name")) request_save(s, true);
        ImGui::Separator();
        if (with_tip(ImGui::MenuItem("Validate Assets"), "Checks every asset for problems (missing files, broken references) and lists them in the Message Log")) {
            const u32 n = validate_assets(s.project, *s.registry);
            std::snprintf(s.status, sizeof s.status, "Asset validation: %u problem%s (Message Log)", n, n == 1 ? "" : "s");
            if (n) s.show_message_log = true;
        }
        ImGui::SetItemTooltip("Missing references, oversize textures, name prefixes (Project Settings > Validation)");
        if (ImGui::MenuItem("Import...", nullptr, false, !s.playing)) s.import_requested = true;
        ImGui::SetItemTooltip("Pick files (several at once) to copy into the Content Browser's folder; dropping files or a whole folder on the editor works too");
        if (ImGui::MenuItem("Package Project", nullptr, false, !s.playing)) s.package_requested = true;
        ImGui::SetItemTooltip("Build/<Project Name>/: the game in a folder that runs on another PC (opens Project Settings > Game Default Level)");
        with_tip(ImGui::MenuItem("Package DLC...", nullptr, &s.show_package_dlc), "Packages one content folder or plugin (or what changed since an earlier package) into Build/DLC/<Name>/, which the shipped game mounts from its DLC/ folder");
        ImGui::EndMenu();
    }
    if (with_tip(ImGui::BeginMenu("Edit"), "Undo, copy and paste, grouping; Editor Preferences and Project Settings")) {
        if (with_tip(ImGui::MenuItem("Undo", command_text(Command::Undo), false, s.history->can_undo() && !s.playing), "Takes back the last change")) s.history->undo(*s.world);
        if (with_tip(ImGui::MenuItem("Redo", command_text(Command::Redo), false, s.history->can_redo() && !s.playing), "Does again what Undo took back")) s.history->redo(*s.world);
        ImGui::Separator();
        const bool has = s.world->alive(s.selected) && !s.playing;
        if (with_tip(ImGui::MenuItem("Duplicate", command_text(Command::Duplicate), false, has), "Copies the selection in place")) duplicate_selected(s);
        if (with_tip(ImGui::MenuItem("Group", command_text(Command::Group), false, has), "Puts the selection under a new parent object so they move together")) group_selection(s);
        if (with_tip(ImGui::MenuItem("Ungroup", command_text(Command::Ungroup), false, has), "Takes the selection out of its group")) ungroup_selection(s);
        if (with_tip(ImGui::MenuItem("Select All", command_text(Command::SelectAll), false, !s.playing), "Selects every object in the level")) {
            for (u32 slot = 0; slot < s.world->slot_count(); ++slot) add_to_selection(s, s.world->entity_at_slot(slot), false);
        }
        if (with_tip(ImGui::MenuItem("Cut", command_text(Command::Cut), false, has), "Copies the selection and removes it")) {
            copy_selection(s);
            delete_selected(s);
        }
        if (with_tip(ImGui::MenuItem("Copy", command_text(Command::Copy), false, has), "Copies the selection (paste it in this or another level)")) copy_selection(s);
        if (with_tip(ImGui::MenuItem("Paste", command_text(Command::Paste), false, !s.playing), "Puts copied objects into the level")) paste(s);
        if (with_tip(ImGui::MenuItem("Delete", command_text(Command::Delete), false, has), "Removes the selection")) delete_selected(s);
        if (with_tip(ImGui::MenuItem("Drop to Floor", command_text(Command::DropToFloor), false, has), "Moves the selection down until it rests on what is below")) drop_to_floor(s);
        ImGui::Separator();
        with_tip(ImGui::MenuItem("Plugins", nullptr, &s.show_plugins), "Plugins/<Name> folders with their own Content and game code: turn them on or off, make a new one");
        with_tip(ImGui::MenuItem("Editor Preferences", nullptr, &s.show_preferences), "Your own editor settings: camera speed, snapping, autosave, theme, plugins");
        with_tip(ImGui::MenuItem("Project Settings", nullptr, &s.show_project_settings), "The game's settings, shared with the team: maps, multiplayer, languages, platform");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        if (ImGui::MenuItem("Smoke Test Level (5 s)", nullptr, false, !s.playing)) s.smoke_test_seconds = 5.0f;
        ImGui::SetItemTooltip("Plays this level for 5 seconds and reports any errors it logs (Unreal's automation smoke test)");
        if (ImGui::MenuItem("Run Unit Tests")) s.run_unit_tests = true;
        ImGui::SetItemTooltip("Runs the engine's tests (ctest) in the background; the result goes to the Output Log");
        if (with_tip(ImGui::MenuItem("Source Control Changes"), "Every changed asset (Git): tick and submit them together, or revert one (Unreal's View Changelists)")) source_control::open_changes();
        if (with_tip(ImGui::MenuItem("Gather Text"), "Localization (Unreal's Gather Text): adds every key the project shows (lsd.text in scripts, Localized Text nodes, UI document texts) that the string table lacks")) {
            bool changed = false;
            gather_project_text(s.project, *s.registry, s.status, sizeof s.status, changed);
            if (changed) reflect::save_file(s.project, "Config/project.json");  // the new string table
        }
        ImGui::Separator();
        if (with_tip(ImGui::MenuItem("Asset Audit"), "Every asset in a sortable table: type, disk size, compiled size, how many use it, unused ones flagged (Unreal's Asset Audit)")) open_asset_audit_window(s);
        if (with_tip(ImGui::MenuItem("Size Map"), "How much disk all of Content/ takes, biggest assets first (Unreal's Size Map); right-click an asset or folder for its own")) open_size_map_for(s, "");
        if (with_tip(ImGui::MenuItem("Fix Up Redirectors"), "Updates names and paths of renamed or moved assets in scripts, UI, text assets and the project settings (Unreal's Fix Up Redirectors)")) fix_up_asset_redirectors(s);
        ImGui::Separator();
        if (ImGui::MenuItem("Compile Game Code", command_text(Command::CompileGameCode), false, !game_module::compiling())) game_module::compile();
        ImGui::SetItemTooltip("Builds the C / C++ game code in Source/ into Binaries/Game.dll; it reloads by itself, also while playing (Unreal's Compile / Live Coding)");
        if (with_tip(ImGui::MenuItem("New C / C++ Script..."), "Makes a C or C++ script or component in Source/ and opens it in VS Code")) s.new_script_popup = true;
        if (with_tip(ImGui::MenuItem("Set Up Debugging (VS Code)"), "Writes VS Code launch settings so F5 debugs the game code")) {
            game_module::write_debug_config(s.status, sizeof s.status);
            LSD_LOG_INFO("game", "%s", s.status);
        }
        ImGui::SetItemTooltip("Writes .vscode/launch.json: F5 starts the editor under the debugger, breakpoints in Source/ stop");
        ImGui::SetItemTooltip("Writes Source/<Name>.cpp or .c from a template (Unreal's Tools > New C++ Class)");
        ImGui::Separator();
        ImGui::MenuItem("Lua Debugger", nullptr, &s.show_debugger);
        ImGui::SetItemTooltip("Breakpoints, stepping, call stack, locals and watch for Lua scripts (visual scripts: breakpoints on nodes)");
        if (s.settings.assistant_enabled) {
            ImGui::MenuItem("LSD Assistant", nullptr, &s.show_assistant);
            ImGui::SetItemTooltip("Chat with your own AI model that knows LSD_Engine: questions, plans, scripts it can write into the project");
        }
        ImGui::MenuItem("Remote Console", nullptr, &s.show_remote_console);
        ImGui::SetItemTooltip("Sends console commands to a game running on this PC with -remote");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Build")) {
        if (ImGui::MenuItem("Build Lighting", nullptr, false, !s.playing && s.build_lighting_progress < 0.0f)) s.build_lighting_requested = true;
        if (with_tip(ImGui::MenuItem("Build Reflection Captures"), "Captures what each Reflection Capture sees again (after moving things)")) s.capture_reflections = true;
        ImGui::SetItemTooltip("Bakes light probes (sky, and light bounced off the level) into <level>_BuiltData.lsdlight; save the level first");
        if (ImGui::MenuItem("Build Navigation", command_text(Command::ToggleNavigation))) s.show_navigation = true;  // the navmesh rebuilds while it is shown
        if (with_tip(ImGui::MenuItem("Build Release Engine"), "Builds the Release engine Package Project copies (it runs on PCs without Visual Studio), from the engine's source with CMake; the menu bar shows its progress")) {
            start_release_build(s.status, sizeof s.status);
            LSD_LOG_INFO("editor", "%s", s.status);
        }
        ImGui::SetItemTooltip("Builds and shows the navmesh (it keeps rebuilding while shown)");
        if (with_tip(ImGui::MenuItem("Validate Assets"), "Checks every asset for problems and lists them in the Message Log")) {
            const u32 n = validate_assets(s.project, *s.registry);
            std::snprintf(s.status, sizeof s.status, "Asset validation: %u problem%s (Message Log)", n, n == 1 ? "" : "s");
            if (n) s.show_message_log = true;
        }
        if (with_tip(ImGui::MenuItem("Package Project", nullptr, false, !s.playing), "Builds the shippable game into Build/<Project Name>/")) s.package_requested = true;
        ImGui::EndMenu();
    }
    if (s.user_settings && with_tip(ImGui::BeginMenu("Settings"), "Graphics quality (Low to Epic)")) {  // Unreal's toolbar Settings > Engine Scalability
        static const char* kQuality[] = {"Low", "Medium", "High", "Epic"};
        ImGui::TextDisabled("Engine Scalability");
        if (with_tip(ImGui::MenuItem("Auto-detect"), "Picks the quality for this PC again: the recommendation for its graphics card, then the project's Device Profiles"))
            s.auto_detect_quality = true;
        for (u32 q = 0; q < 4; ++q) {
            if (ImGui::MenuItem(kQuality[q], nullptr, s.user_settings->quality == q)) {
                s.user_settings->quality = q;
                s.user_settings_changed = true;
            }
        }
        ImGui::Separator();
        struct Group {
            const char* name;
            u32* value;
            const char* tip;
        } groups[] = {{"Shadows", &s.user_settings->shadow_quality, "Low none, Medium the sun's only and nearer, High as set, Epic further"},
                      {"Post Processing", &s.user_settings->post_quality, "Low: bloom, tonemap, grading, FXAA. Medium: + ambient occlusion. High / Epic: everything the volume sets"},
                      {"View Distance", &s.user_settings->view_distance_quality, "Draw and cull distances (and LOD switches) times 0.5, 0.75, 1 or 1.5"},
                      {"Effects", &s.user_settings->effects_quality, "Particles spawned times 0.25, 0.5 or 1"}};
        for (const Group& g : groups) {
            char label[64];
            std::snprintf(label, sizeof label, "%s: %s###%s", g.name, *g.value ? kQuality[*g.value - 1 < 3 ? *g.value - 1 : 3] : "Overall", g.name);
            if (with_tip(ImGui::BeginMenu(label), g.tip)) {
                if (ImGui::MenuItem("Overall", nullptr, *g.value == 0)) {
                    *g.value = 0;
                    s.user_settings_changed = true;
                }
                for (u32 q = 0; q < 4; ++q) {
                    if (ImGui::MenuItem(kQuality[q], nullptr, *g.value == q + 1)) {
                        *g.value = q + 1;
                        s.user_settings_changed = true;
                    }
                }
                ImGui::EndMenu();
            }
        }
        ImGui::EndMenu();
    }
    if (with_tip(ImGui::BeginMenu("Window"), "Panels to show: Undo History, World Settings, Message Log, Memory, Layout")) {
        with_tip(ImGui::MenuItem("Undo History", nullptr, &s.show_history), "Every change you made; click one to go back to it");
        with_tip(ImGui::MenuItem("World Settings", nullptr, &s.show_world_settings), "This level's settings: gravity, Kill Z, Game Mode, Level Script, navigation");
        with_tip(ImGui::MenuItem("Message Log", nullptr, &s.show_message_log), "Warnings and errors from imports, validation and packaging");
        with_tip(ImGui::MenuItem("Memory", nullptr, &s.show_memory), "Memory used: RAM, video memory and each allocator");
        with_tip(ImGui::MenuItem("Audio Mixer", nullptr, &s.show_audio_mixer), "The Sound Classes with their Submix effects and live level meters (Unreal's submix graph)");
        if (with_tip(ImGui::BeginMenu("Viewports"), "2D, 2.5D and 3D views of the level, side by side (a 2D game previewed in 3D)")) {
            const u32 kinds = s.settings.viewport_kinds ? s.settings.viewport_kinds : kViewport3D;
            const struct {
                const char* name;
                u32 bit;
                const char* tip;
            } items[] = {{"2D Viewport", kViewport2D, "Orthographic, looking along +Y: how a side-on 2D game is seen"},
                         {"2.5D Viewport", kViewport2_5D, "Perspective from the side with a long lens: 3D layers played on a 2D plane"},
                         {"3D Viewport", kViewport3D, "The free perspective view: walk around a 2D or 2.5D level to check its depth"}};
            for (const auto& item : items) {
                bool on = (kinds & item.bit) != 0;
                if (with_tip(ImGui::MenuItem(item.name, nullptr, &on), item.tip)) set_viewport_kinds(s, on ? kinds | item.bit : kinds & ~item.bit);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (with_tip(ImGui::BeginMenu("Layout"), "Save and load panel arrangements")) {  // Unreal's Window > Load Layout / Save Layout
            static char name[64] = "My Layout";
            ImGui::SetNextItemWidth(160.0f);
            ImGui::InputText("##layout_name", name, sizeof name);
            ImGui::SameLine();
            if (with_tip(ImGui::Button("Save Layout As"), "Saves the current arrangement of panels under this name") && asset::valid_name(name)) {
                std::error_code ec;
                std::filesystem::create_directories(kLayoutDir, ec);
                const std::string path = std::string(kLayoutDir) + "/" + name + ".ini";
                ImGui::SaveIniSettingsToDisk(path.c_str());
                std::snprintf(s.status, sizeof s.status, "Saved layout %s", name);
            }
            ImGui::Separator();
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(kLayoutDir, ec)) {
                if (entry.path().extension() != ".ini") continue;
                if (ImGui::MenuItem(entry.path().stem().string().c_str())) std::snprintf(s.load_layout, sizeof s.load_layout, "%s", entry.path().generic_string().c_str());
            }
            if (with_tip(ImGui::MenuItem("Reset to Default"), "Puts every panel back where it started")) s.reset_layout = true;
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    // Play / Stop, like Unreal's main toolbar.
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.5f - 40.0f);
    if (!s.playing) {
        ImGui::PushStyleColor(ImGuiCol_Text, vec4(s.theme.axis_y));
        if (ImGui::MenuItem("Play", command_text(Command::Play))) s.play_requested = true;
        if (ImGui::MenuItem("Simulate", command_text(Command::Simulate))) s.play_requested = s.simulate_requested = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Runs the level with the editor camera: physics, scripts and AI, nobody possessed (F8 possesses)");
        if (!s.play_window_open) {
            if (ImGui::MenuItem("New Window", command_text(Command::PlayWindow))) s.play_window_requested = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Plays the level as it is now in a window of its own, as the packaged game would (Unreal's New Editor Window); the editor stays free");
        } else if (with_tip(ImGui::MenuItem("Close Window"), "Closes the game window that Play opened")) {
            s.close_play_window = true;
        }
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, vec4(s.theme.axis_x));
        if (with_tip(ImGui::MenuItem("Stop", command_text(Command::StopPlay)), "Stops playing (Esc)")) s.stop_requested = true;
        ImGui::PopStyleColor();
        if (ImGui::MenuItem(s.paused ? "Resume" : "Pause", command_text(Command::Pause))) s.paused = !s.paused;
        if (s.paused && with_tip(ImGui::MenuItem("Step", command_text(Command::StepFrame)), "Advances the paused game one frame")) s.step_requested = true;
        if (ImGui::MenuItem(s.ejected ? "Possess" : "Eject", command_text(Command::Eject))) {
            s.ejected = !s.ejected;
            s.mouse_captured = !s.ejected;
        }
    }
    if (g_unread_errors || g_unread_warnings) {
        char badge[64];
        std::snprintf(badge, sizeof badge, "%u error%s, %u warning%s", g_unread_errors, g_unread_errors == 1 ? "" : "s", g_unread_warnings,
                      g_unread_warnings == 1 ? "" : "s");
        ImGui::PushStyleColor(ImGuiCol_Text, g_unread_errors ? ImVec4(1.0f, 0.42f, 0.38f, 1) : ImVec4(1.0f, 0.82f, 0.35f, 1));
        if (ImGui::MenuItem(badge)) s.show_message_log = true;
        ImGui::PopStyleColor();
    }
    const f32 progress = s.material_import_progress >= 0.0f ? s.material_import_progress : s.build_lighting_progress;
    if (progress >= 0.0f) {  // long jobs: Import Materials and Textures, Build Lighting
        ImGui::SetNextItemWidth(260.0f);
        ImGui::ProgressBar(progress, ImVec2(260.0f, 0.0f), s.material_import_progress >= 0.0f ? s.material_import_label : "Building lighting");
        if (s.material_import_progress >= 0.0f) ImGui::SetItemTooltip("%s", s.material_import_label);
    }
    const char* level = s.level_path[0] ? s.level_path : "Untitled level";
    char right[320];
    std::snprintf(right, sizeof right, "%s%s   %s", level, s.history->dirty() ? " *" : "", s.status);
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::CalcTextSize(right).x - 16.0f);
    ImGui::TextDisabled("%s", right);
    ImGui::EndMainMenuBar();
}

void draw_level_dialogs(EditorState& s) {
    static bool asked_recovery = false;
    if (s.recovery_path[0] && !asked_recovery) {
        ImGui::OpenPopup("Restore Autosave");
        asked_recovery = true;
    }
    if (ImGui::BeginPopupModal("Restore Autosave", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("The editor did not close properly last time.\nAn autosave newer than the level was found:\n  %s", s.recovery_path);
        if (with_tip(ImGui::Button("Restore"), "Opens the autosave the editor made before it closed unexpectedly")) {
            s.restore_requested = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (with_tip(ImGui::Button("Ignore"), "Keeps the last saved level and drops the autosave")) {
            s.recovery_path[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (s.quit_requested) ImGui::OpenPopup("Quit");
    s.quit_requested = false;
    if (ImGui::BeginPopupModal("Quit", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save changes to %s before closing?", s.level_path[0] ? s.level_path : "the untitled level");
        if (with_tip(ImGui::Button("Save"), "Saves the level, then continues")) {
            s.quit_after_save = true;
            request_save(s, false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (with_tip(ImGui::Button("Don't Save"), "Continues and loses the unsaved changes")) {
            s.quit_confirmed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (with_tip(ImGui::Button("Cancel"), "Goes back to editing") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (g_open_confirm) ImGui::OpenPopup("Unsaved Changes");
    if (g_open_save_as) ImGui::OpenPopup("Save Level As");
    g_open_confirm = g_open_save_as = false;

    if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("The current level has unsaved changes. Discard them?");
        if (with_tip(ImGui::Button("Discard"), "Loses the unsaved changes")) {
            if (g_confirm_new) s.new_level_requested = true;
            else std::snprintf(s.open_level_requested, sizeof s.open_level_requested, "%s", g_confirm_open);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (with_tip(ImGui::Button("Cancel"), "Goes back to editing") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Save Level As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Saved to %s/<name>%s", kLevelsFolder, world::kLevelExtension);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##level_name", g_save_as_name, sizeof g_save_as_name, ImGuiInputTextFlags_EnterReturnsTrue);
        const bool ok = asset::valid_name(g_save_as_name);
        if (!ok) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Not a valid name");
        if ((with_tip(ImGui::Button("Save"), "Saves the level under this name in Content/Levels") || enter) && ok) {
            std::snprintf(s.save_as_requested, sizeof s.save_as_requested, "%s/%s%s", kLevelsFolder, g_save_as_name, world::kLevelExtension);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (with_tip(ImGui::Button("Cancel"), "Does not save") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            s.quit_after_save = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void draw_history_window(EditorState& s) {
    if (!s.show_history) return;
    ImGui::SetNextWindowSize(ImVec2(320, 400), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Undo History", &s.show_history)) {
        for (u32 i = 0; i < s.history->count(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const bool current = i == s.history->current();
            if (i > s.history->current()) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (ImGui::Selectable(s.history->label(i), current) && !s.playing) s.history->jump_to(*s.world, i);
            if (i > s.history->current()) ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
    ImGui::End();
}

// The asset a log message is about: a GUID in it, else the longest Content path it mentions.
asset::Guid asset_in_message(const EditorState& s, const char* text) {
    asset::Guid guid;
    for (const char* p = text; *p; ++p) {  // 32 hex digits in a row
        int n = 0;
        while (n < 32 && std::isxdigit(static_cast<unsigned char>(p[n]))) ++n;
        if (n == 32 && !std::isxdigit(static_cast<unsigned char>(p[32])) && asset::parse_guid(std::string(p, 32).c_str(), guid) && s.registry->find(guid)) return guid;
    }
    usize best = 0;
    guid = {};
    for (usize i = 0; i < s.registry->count(); ++i) {
        const asset::AssetRecord& r = s.registry->get(i);
        const usize len = std::strlen(r.path);
        if (len > best && std::strstr(text, r.path)) {
            best = len;
            guid = r.guid;
        }
    }
    return guid;
}

void draw_message_log(EditorState& s) {
    if (!s.show_message_log) return;
    ImGui::SetNextWindowSize(ImVec2(620, 300), ImGuiCond_FirstUseEver);
    asset::Guid clicked;
    if (ImGui::Begin("Message Log", &s.show_message_log)) {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        g_unread_errors = g_unread_warnings = 0;  // seen
        if (with_tip(ImGui::Button("Clear"), "Empties the list")) g_messages_count = 0;
        ImGui::SameLine();
        ImGui::TextDisabled("Warnings and errors from imports, saves and the engine (all lines are in the Output Log)");
        ImGui::Separator();
        if (g_messages_count == 0) ImGui::TextDisabled("No messages.");
        const u32 first = (g_messages_next + kMessages - g_messages_count) % kMessages;
        for (u32 i = g_messages_count; i-- > 0;) {  // newest first
            const LogLine& m = g_messages[(first + i) % kMessages];
            const bool error = m.level >= log::Level::Error;
            ImGui::PushID(static_cast<int>(i));
            // The whole row is clickable: it shows the asset the message names (Unreal's Message Log links).
            const ImVec2 row = ImGui::GetCursorPos();
            const f32 height = ImGui::CalcTextSize(m.text, nullptr, false, ImGui::GetContentRegionAvail().x - 160.0f).y;
            if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, height))) clicked = asset_in_message(s, m.text);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click: show the asset this message names in the Content Browser");
            ImGui::SetCursorPos(row);
            ImGui::TextColored(error ? ImVec4(1.0f, 0.42f, 0.38f, 1) : ImVec4(1.0f, 0.82f, 0.35f, 1), "%s", error ? "Error" : "Warning");
            ImGui::SameLine(80.0f);
            ImGui::TextDisabled("[%s]", m.channel);
            ImGui::SameLine();
            ImGui::TextWrapped("%s", m.text);
            ImGui::PopID();
        }
    }
    ImGui::End();
    if (clicked.valid()) find_in_content_browser(s, clicked);  // after the log lock: it may log
}

void draw_project_settings(EditorState& s) {
    if (!s.show_project_settings) return;
    ImGui::SetNextWindowSize(ImVec2(520, 300), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Project Settings", &s.show_project_settings)) {
        constexpr char kProjectPath[] = "Config/project.json";
        ImGui::TextDisabled("%s (shared with the team: commit it)", kProjectPath);
        if (with_tip(ImGui::Button("Save"), "Writes the Project Settings to Config/project.json")) std::snprintf(s.status, sizeof s.status, reflect::save_file(s.project, kProjectPath) ? "Saved %s" : "Could not save %s", kProjectPath);
        ImGui::SameLine();
        if (with_tip(ImGui::Button("Use Current Level as Game Default"), "The packaged game starts in the level open now") && s.level_path[0]) std::snprintf(s.project.game_default_level, sizeof s.project.game_default_level, "%s", s.level_path);
        const DetailsStyle style{{u32_color(s.theme.details_row_a), u32_color(s.theme.details_row_b)}};
        draw_details(reflect::type_of<ProjectSettings>(), &s.project, &style);
    }
    ImGui::End();
}

void draw_world_settings_window(EditorState& s) {
    if (!s.show_world_settings) return;
    ImGui::SetNextWindowSize(ImVec2(380, 200), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("World Settings", &s.show_world_settings)) {
        const DetailsStyle style{{u32_color(s.theme.details_row_a), u32_color(s.theme.details_row_b)}, "", details_widget, &s};  // asset pickers
        if (draw_properties(reflect::type_of<world::WorldSettings>(), &s.world->settings(), &style)) edited(s, "Edit World Settings");
    }
    ImGui::End();
}

// ---- Plugins (Editor Preferences > Plugins, like Unreal's Edit > Plugins) ----

// The Claude MCP bridge sits next to the editor exe; Claude Code starts it and it talks to us.
std::string mcp_bridge_path() {
    std::string dir = SDL_GetBasePath() ? SDL_GetBasePath() : "";
    for (char& c : dir) {
        if (c == '\\') c = '/';
    }
#ifdef _WIN32
    return dir + "lsd_mcp.exe";
#else
    return dir + "lsd_mcp";
#endif
}

void draw_claude_mcp(EditorState& s) {
    EditorSettings& set = s.settings;
    ImGui::TextWrapped("Lets Claude Code work in this editor through MCP (Model Context Protocol): run console commands "
                       "and Lua, list the level's objects and the assets, read the Output Log. Scripts Claude writes into "
                       "Content/ reload by themselves.");
    ImGui::Spacing();
    with_tip(ImGui::Checkbox("Enabled", &set.mcp_enabled), "Lets Claude Code drive the editor (place objects, run commands) through the MCP bridge");
    ImGui::SameLine(0, 24);
    int port = static_cast<int>(set.mcp_port);
    ImGui::SetNextItemWidth(90);
    if (ImGui::InputInt("Port", &port, 0) && port > 0 && port < 65536) set.mcp_port = static_cast<u32>(port);
    if (!set.mcp_enabled) ImGui::TextDisabled("Off");
    else if (!remote::listening()) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "Port %u is in use: pick another one", set.mcp_port);
    else if (remote::client_connected()) ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "Claude is connected (127.0.0.1:%u)", set.mcp_port);
    else ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "Waiting for Claude Code on 127.0.0.1:%u", set.mcp_port);

    ImGui::SeparatorText("Connect Claude Code (once)");
    const std::string bridge = mcp_bridge_path();
    const std::string port_text = std::to_string(set.mcp_port);
    const std::string command = "claude mcp add lsd-engine -- \"" + bridge + "\" --port " + port_text;
    ImGui::TextWrapped("1. Copy this command and run it in a terminal (or ask Claude Code to run it):");
    ImGui::TextDisabled("%s", command.c_str());
    if (with_tip(ImGui::Button("Copy Command"), "Copies the command that adds the editor to Claude Code")) {
        ImGui::SetClipboardText(command.c_str());
        std::snprintf(s.status, sizeof s.status, "Copied the claude mcp add command");
    }
    ImGui::SameLine();
    if (with_tip(ImGui::Button("Write .mcp.json"), "Writes .mcp.json in the project so Claude Code finds the editor by itself")) {  // project-scoped server: Claude Code asks once to trust it
        std::error_code error;
        if (std::filesystem::exists(".mcp.json", error)) {
            std::snprintf(s.status, sizeof s.status, "A .mcp.json is already there: use Copy Command instead (it is left as it is)");
        } else {
            std::ofstream file(".mcp.json", std::ios::binary);
            file << "{\n  \"mcpServers\": {\n    \"lsd-engine\": {\n      \"command\": \"" << bridge << "\",\n      \"args\": [\"--port\", \"" << port_text
                 << "\"]\n    }\n  }\n}\n";
            std::snprintf(s.status, sizeof s.status, file ? "Wrote .mcp.json in the project folder" : "Could not write .mcp.json");
        }
    }
    ImGui::SetItemTooltip("Writes the server into this project's .mcp.json, so Claude Code started in this folder finds it");
    ImGui::TextWrapped("2. Keep this plugin enabled and the editor open, then start Claude Code in the project folder. "
                       "/mcp in Claude Code shows lsd-engine as connected.");

    ImGui::SeparatorText("What Claude can do");
    ImGui::BulletText("console: any console command (play, stop, save, open, graph.Open, validate, HighResShot...)");
    ImGui::BulletText("lua: run Lua in the game while playing");
    ImGui::BulletText("list_objects / list_assets: the level and Content/");
    ImGui::BulletText("read_log: the latest Output Log lines (errors after Play)");
    ImGui::TextDisabled("Only programs on this PC can connect (127.0.0.1). Disable it when you are not using it.");
}

struct Plugin {
    const char* name;
    const char* summary;
    bool EditorSettings::*enabled;
    void (*draw)(EditorState& s);
};
constexpr Plugin kPlugins[] = {
    {"Claude MCP", "Code with Claude Code directly in the editor", &EditorSettings::mcp_enabled, draw_claude_mcp},
    {"LSD Assistant", "An AI game studio in the editor (your own model)", &EditorSettings::assistant_enabled, assistant::draw_settings},
};

void draw_plugins(EditorState& s) {
    static int selected = -1;
    ImGui::BeginChild("##plugin_list", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 2.2f), ImGuiChildFlags_Borders);
    for (int i = 0; i < static_cast<int>(std::size(kPlugins)); ++i) {
        const Plugin& p = kPlugins[i];
        ImGui::PushID(i);
        ImGui::Checkbox("##on", &(s.settings.*p.enabled));
        ImGui::SameLine();
        if (ImGui::Selectable(p.name, selected == i)) selected = selected == i ? -1 : i;
        ImGui::SameLine(200);
        ImGui::TextDisabled("%s", p.summary);
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (selected < 0) {
        ImGui::TextDisabled("Click a plugin for its settings.");
        return;
    }
    ImGui::SeparatorText(kPlugins[selected].name);
    kPlugins[selected].draw(s);
}

// Edit > Plugins (Unreal's Plugins window): the Plugins/<Name>/plugin.json folders, an Enabled checkbox each, New Plugin.
void draw_plugins_window(EditorState& s) {
    if (!s.show_plugins) return;
    static std::vector<asset::PluginInfo> list;
    static double refreshed = -10.0;
    static char new_name[48] = "";
    if (ImGui::GetTime() - refreshed > 1.0) {
        list = asset::discover_plugins(kPluginsDir);
        refreshed = ImGui::GetTime();
    }
    ImGui::SetNextWindowSize(ImVec2(520, 340), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Plugins", &s.show_plugins)) {
        if (list.empty()) ImGui::TextDisabled("No plugins. New Plugin makes Plugins/<Name>/ with plugin.json and Content/.");
        for (const asset::PluginInfo& p : list) {
            ImGui::PushID(p.descriptor.name);
            bool on = p.descriptor.enabled;
            if (with_tip(ImGui::Checkbox("##enabled", &on), "Enabled: its Content shows under Plugins/<Name>/ and ships in packages; its Source/ builds into the game code at the next Compile Game Code")) {
                asset::set_plugin_enabled(kPluginsDir, p.descriptor.name, on);
                mount_extras(*s.registry);
                s.registry->scan();
                refreshed = -10.0;
                std::snprintf(s.status, sizeof s.status, "%s %s (Compile Game Code applies its C / C++ files)", p.descriptor.name, on ? "enabled" : "disabled");
            }
            ImGui::SameLine();
            ImGui::Text("%s  v%s", p.descriptor.name, p.descriptor.version);
            if (p.descriptor.description[0]) ImGui::TextDisabled("    %s", p.descriptor.description);
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::SetNextItemWidth(200);
        ImGui::InputText("##new_plugin", new_name, sizeof new_name);
        ImGui::SetItemTooltip("The new plugin's name (also its folder name)");
        ImGui::SameLine();
        if (with_tip(ImGui::Button("New Plugin"), "Makes Plugins/<Name>/ with plugin.json and an empty Content/")) {
            std::string why;
            if (asset::create_plugin(kPluginsDir, new_name, why)) {
                mount_extras(*s.registry);
                s.registry->scan();
                refreshed = -10.0;
                std::snprintf(s.status, sizeof s.status, "Made Plugins/%s", new_name);
                new_name[0] = '\0';
            } else {
                std::snprintf(s.status, sizeof s.status, "New Plugin: %s", why.c_str());
            }
        }
        ImGui::TextDisabled("Plugins/<Name>/Content is found by the Content Browser; Plugins/<Name>/Source/*.cpp joins Game.dll.");
    }
    ImGui::End();
}

// File > Package DLC (Unreal's patch .pak / chunking).
void draw_package_dlc_window(EditorState& s) {
    if (!s.show_package_dlc) return;
    static char folder[256] = "";
    static char base[256] = "";
    ImGui::SetNextWindowSize(ImVec2(520, 220), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Package DLC", &s.show_package_dlc)) {
        ImGui::InputText("Folder", folder, sizeof folder);
        ImGui::SetItemTooltip("The content folder or plugin to package, as the Content Browser shows it: Levels/Episode2 or Plugins/MyPlugin (empty = everything, with a base manifest)");
        if (with_tip(ImGui::SmallButton("Use the open folder"), "Fills Folder with the Content Browser's current folder")) std::snprintf(folder, sizeof folder, "%s", s.content_folder);
        ImGui::InputText("Base manifest", base, sizeof base);
        ImGui::SetItemTooltip("For a patch: the manifest.lsdmanifest of the package (or earlier DLC) players have; only new and changed assets are packaged. Empty = a whole DLC");
        if (with_tip(ImGui::Button("Package DLC"), "Writes Build/DLC/<Name>/ (Content/ and manifest.lsdmanifest); copy it into the shipped game's DLC/ folder")) {
            if (package_dlc(*s.registry, folder, base, s.status, sizeof s.status)) LSD_LOG_INFO("editor", "%s", s.status);
            else LSD_LOG_ERROR("editor", "Package DLC: %s", s.status);
        }
        ImGui::TextWrapped("%s", s.status);
    }
    ImGui::End();
}

void draw_preferences_window(EditorState& s) {
    if (!s.show_preferences) return;
    EditorTheme& t = s.theme;
    ImGui::SetNextWindowSize(ImVec2(460, 600), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Editor Preferences", &s.show_preferences)) {
        const DetailsStyle style{{u32_color(t.details_row_a), u32_color(t.details_row_b)}};
        if (ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_DefaultOpen)) draw_details(reflect::type_of<EditorSettings>(), &s.settings, &style);
        if (ImGui::CollapsingHeader("Theme", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("%s", kThemePath);
            if (with_tip(ImGui::Button("Save Theme"), "Writes these colours to Config/editor_theme.json")) std::snprintf(s.status, sizeof s.status, reflect::save_file(t, kThemePath) ? "Saved %s" : "Could not save %s", kThemePath);
            ImGui::SameLine();
            if (with_tip(ImGui::Button("Reload"), "Reads the theme file again")) {
                reflect::load_file(t, kThemePath);
                apply_theme(t);
            }
            ImGui::SameLine();
            if (with_tip(ImGui::Button("Reset"), "Back to the original colours")) {
                t = EditorTheme{};
                apply_theme(t);
            }
            if (draw_details(reflect::type_of<EditorTheme>(), &t, &style)) apply_theme(t);
        }
        if (ImGui::CollapsingHeader("Keyboard Shortcuts")) draw_keymap(s.settings);
        if (ImGui::CollapsingHeader("Plugins", ImGuiTreeNodeFlags_DefaultOpen)) draw_plugins(s);
    }
    ImGui::End();
}

void handle_shortcuts(EditorState& s) {
    if (keymap_capturing()) return;  // Preferences is reading a new key
    ImGuiIO& io = ImGui::GetIO();
    using C = Command;
    if (command_pressed(C::CompileGameCode)) game_module::compile();  // also while playing
    if (s.playing) {
        if (command_pressed(C::StopPlay)) s.stop_requested = true;
        if (command_pressed(C::ReleaseMouse)) s.mouse_captured = false;
        if (command_pressed(C::Pause)) s.paused = !s.paused;
        if (s.paused && command_pressed(C::StepFrame, true)) s.step_requested = true;
        if (command_pressed(C::Eject)) {
            s.ejected = !s.ejected;
            s.mouse_captured = !s.ejected;
        }
        if (!s.ejected) return;
        if (!io.WantTextInput && s.world->alive(s.selected) && command_pressed(C::Focus)) focus_selected(s);
        if (!io.WantTextInput && s.world->alive(s.selected) && command_pressed(C::KeepSimulation)) keep_simulation_changes(s);
        return;
    }
    // Asset editor windows have their own Ctrl+S and Delete: level shortcuts stay out of them.
    if (const ImGuiWindow* focused = GImGui->NavWindow; focused && focused->RootWindow) {
        for (const char* id : {"###VisualScript", "###DataTable", "###UiEditor", "###Timeline", "###StateMachine", "###MeshEditor", "###ParticleEditor", "###PrefabEditor"}) {
            if (std::strstr(focused->RootWindow->Name, id)) return;
        }
    }
    if (command_pressed(C::Play)) s.play_requested = true;
    if (command_pressed(C::PlayWindow) && !s.play_window_open) s.play_window_requested = true;
    if (command_pressed(C::Simulate)) s.play_requested = s.simulate_requested = true;
    if (io.WantTextInput) return;
    if (command_pressed(C::ViewPerspective)) set_view(s, ViewType::Perspective);  // Unreal's viewport keys
    if (command_pressed(C::ViewTop)) set_view(s, ViewType::Top);
    if (command_pressed(C::ViewFront)) set_view(s, ViewType::Front);
    if (command_pressed(C::ViewRight)) set_view(s, ViewType::Right);
    if (command_pressed(C::Lit)) s.view_mode = ViewMode::Lit;
    if (command_pressed(C::Unlit)) s.view_mode = ViewMode::Unlit;
    if (command_pressed(C::Wireframe)) s.view_mode = ViewMode::Wireframe;
    if (command_pressed(C::LightingOnly)) s.view_mode = ViewMode::LightingOnly;
    if (command_pressed(C::Overdraw)) s.view_mode = ViewMode::Overdraw;
    if (command_pressed(C::Collision)) s.view_mode = s.view_mode == ViewMode::Collision ? ViewMode::Lit : ViewMode::Collision;
    if (command_pressed(C::PivotEdit)) s.pivot_edit = !s.pivot_edit;
    if (command_pressed(C::Undo, true)) s.history->undo(*s.world);
    if (command_pressed(C::Redo, true) || command_pressed(C::RedoAlt, true)) s.history->redo(*s.world);
    if (command_pressed(C::Save)) request_save(s, false);
    if (command_pressed(C::SaveAs)) request_save(s, true);
    if (command_pressed(C::NewLevel)) request_new(s);
    if (command_pressed(C::LocalGizmo)) s.gizmo_local = !s.gizmo_local;
    const bool has = s.world->alive(s.selected);
    if (has && command_pressed(C::Copy)) copy_selection(s);
    if (has && command_pressed(C::Cut)) {
        copy_selection(s);
        delete_selected(s);
    }
    if (command_pressed(C::Paste)) paste(s);
    if (has && command_pressed(C::Duplicate)) duplicate_selected(s);
    if (has && command_pressed(C::FindInContentBrowser)) {  // the selected object's asset
        asset::Guid used;
        i32 part = -1;
        if (const auto* m = s.world->get<world::StaticMesh>(s.selected)) asset::parse_mesh_key(m->mesh, used, part);
        if (const auto* m = s.world->get<world::SkeletalMesh>(s.selected); m && !used.valid()) asset::parse_guid(m->mesh, used);
        if (const auto* m = s.world->get<world::Script>(s.selected); m && !used.valid()) asset::parse_guid(m->script, used);
        if (used.valid()) find_in_content_browser(s, used);
    }
    if (has && command_pressed(C::Group)) group_selection(s);
    if (has && command_pressed(C::Ungroup)) ungroup_selection(s);
    if (command_pressed(C::SelectAll)) {
        for (u32 slot = 0; slot < s.world->slot_count(); ++slot) add_to_selection(s, s.world->entity_at_slot(slot), false);
    }
    const bool flying = ImGui::IsMouseDown(ImGuiMouseButton_Right);  // WASD/QE move the camera then
    if (flying) return;
    for (int n = 0; n < 10; ++n) {  // camera bookmarks: Ctrl+0-9 sets, 0-9 jumps
        if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + n)) || io.KeyAlt || io.KeyShift) continue;
        if (io.KeyCtrl) {
            s.bookmarks[n] = s.camera;
            s.bookmark_set[n] = true;
            std::snprintf(s.status, sizeof s.status, "Camera bookmark %d set", n);
        } else if (s.bookmark_set[n]) {
            s.camera = s.bookmarks[n];
        }
    }
    if (command_pressed(C::ToggleNavigation)) s.show_navigation = !s.show_navigation;
    if (command_pressed(C::Translate)) s.gizmo = GizmoMode::Translate;
    if (command_pressed(C::Rotate)) s.gizmo = GizmoMode::Rotate;
    if (command_pressed(C::Scale)) s.gizmo = GizmoMode::Scale;
    if (command_pressed(C::GameView)) s.game_view = !s.game_view;
    if (has && command_pressed(C::Focus)) focus_selected(s);
    if (has && command_pressed(C::DropToFloor)) drop_to_floor(s);
    if (has && command_pressed(C::Delete)) delete_selected(s);
    if (command_pressed(C::Deselect)) select(s, {});
}

} // namespace

void set_layout(EditorState& s, u32 panes) { set_panes(s, panes); }

void create_input_setup(EditorState& s) { create_default_input(s); }

u32 refresh_prefabs(EditorState& s) { return refresh_instances(s, nullptr, {}); }

bool select_asset(EditorState& s, const char* name) {
    usize hit = 0;
    if (s.registry->search(name, asset::AssetType::Unknown, nullptr, &hit, 1) != 1) return false;
    const asset::AssetRecord& r = s.registry->get(hit);
    select(s, {});
    s.selected_asset = r.guid;
    s.selected_meta = asset::AssetMeta{};
    s.registry->load_meta(r, s.selected_meta);  // as a click in the Content Browser does
    if (r.type == asset::AssetType::Data) load_selected_data(s, r);
    return true;
}

void install_output_log() { log::set_sink(log_sink, nullptr); }

void clear_output_log() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_count = 0;
}

std::string recent_log(u32 count) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    const u32 n = count < g_log_count ? count : g_log_count;
    std::string out;
    for (u32 i = 0; i < n; ++i) {
        const LogLine& line = g_log[(g_log_next + kLogLines - n + i) % kLogLines];
        out += std::string(line.level >= log::Level::Error ? "ERROR " : line.level >= log::Level::Warning ? "WARN " : "") + "[" + line.channel + "] " + line.text + "\n";
    }
    return out.empty() ? "(the Output Log is empty)" : out;
}

void apply_theme(const EditorTheme& t) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);
    style.FontSizeBase = t.font_size;
    style.WindowRounding = 0.0f;
    style.TabRounding = 2.0f;
    style.WindowBorderSize = 0.0f;
    style.TreeLinesSize = 2.0f;
    style.Colors[ImGuiCol_WindowBg] = vec4(t.panel_background);
    style.Colors[ImGuiCol_ChildBg] = vec4(t.panel_background);
    style.Colors[ImGuiCol_DockingEmptyBg] = vec4(t.viewport_background);
    style.Colors[ImGuiCol_ScrollbarGrab] = vec4(t.outline_scrollbar);
    style.Colors[ImGuiCol_Header] = vec4(t.details_tab);
    style.Colors[ImGuiCol_HeaderHovered] = vec4(t.details_row_a);
    style.Colors[ImGuiCol_CheckMark] = vec4(t.details_tab_active);
    style.Colors[ImGuiCol_SliderGrab] = vec4(t.details_tab_active);
    style.Colors[ImGuiCol_SliderGrabActive] = vec4(t.content_tab);
    style.Colors[ImGuiCol_SeparatorHovered] = vec4(t.details_tab_active);
    style.Colors[ImGuiCol_SeparatorActive] = vec4(t.content_tab);
    style.Colors[ImGuiCol_DockingPreview] = vec4(t.details_tab_active);
}

void draw_game(EditorState& s) {
    const ImGuiIO& io = ImGui::GetIO();
    s.viewport_count = 1;
    s.viewport_rects[0][0] = 0.0f;
    s.viewport_rects[0][1] = 0.0f;
    s.viewport_rects[0][2] = io.DisplaySize.x;
    s.viewport_rects[0][3] = io.DisplaySize.y;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) s.stop_requested = true;  // ends the game
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) s.mouse_captured = true;
}

void draw_editor(EditorState& s) {
    ImGuizmo::BeginFrame();
    draw_menu_bar(s);
    draw_panel_tabs(s);
    const bool level_tab = g_active_tab == kLevelTab;
    if (level_tab) draw_workspace_tabs(s);  // the workspaces belong to the Level tab
    const ImGuiViewport* main = ImGui::GetMainViewport();
    const ImGuiID dockspace = ImHashStr("EditorDockspace");
    if (s.workspace_request[0]) {
        if (std::strcmp(s.workspace_request, s.settings.workspace) != 0) switch_workspace(s, s.workspace_request, dockspace, main->WorkSize);
        s.workspace_request[0] = '\0';
    }
    const ImGuiDockNode* existing = ImGui::DockBuilderGetNode(dockspace);
    if (!existing || !existing->IsSplitNode() || s.reset_layout) {
        const WorkspaceDef* def = find_workspace_def(s.settings, s.settings.workspace);
        build_workspace(def ? *def : kWorkspaces[0], dockspace, main->WorkSize);
    }
    s.reset_layout = false;
    ImGui::DockSpaceOverViewport(dockspace, main, ImGuiDockNodeFlags_PassthruCentralNode);

    u64 lap = SDL_GetTicksNS();
    auto took = [&](int part) {  // stat dump's UI breakdown
        const u64 now = SDL_GetTicksNS();
        s.stat_panel_ms[part] = static_cast<f32>(static_cast<f64>(now - lap) * 1e-6);
        lap = now;
    };
    draw_outliner(s);
    took(0);
    draw_details_panel(s);
    took(1);
    draw_content_browser(s);
    took(2);
    draw_editor_area(s);
    took(3);
    if (level_tab) draw_mode_panel(s);
    {  // asset editors opened from now on dock where the editor area is (begin_editor)
        const WorkspaceDef* def = find_workspace_def(s.settings, s.settings.workspace);
        const ImGuiWindow* area = def && def->hint ? ImGui::FindWindowByName(kEditorAreaWindow) : nullptr;
        g_editor_area = area && area->DockNode ? area->DockNode->ID : 0;
    }
    draw_history_window(s);
    draw_preferences_window(s);
    draw_plugins_window(s);
    draw_package_dlc_window(s);
    draw_world_settings_window(s);
    draw_message_log(s);
    draw_project_settings(s);
    draw_multiplayer_panel(s);
    draw_level_dialogs(s);
    draw_graph_editor(s);
    draw_table_editor(s);
    if (s.particle_editor) s.particle_editor->draw(s);
    source_control::draw();
    remote::draw_window(&s.show_remote_console);
    if (s.new_script_popup) {  // Tools > New C / C++ Script
        ImGui::OpenPopup("New C / C++ Script");
        s.new_script_popup = false;
    }
    if (ImGui::BeginPopupModal("New C / C++ Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char name[64] = "Spinner";
        static int language = 0;  // 0 C++, 1 C
        static int kind = 0;      // 0 a script, 1 a component
        ImGui::TextUnformatted("Language:");
        ImGui::SameLine();
        with_tip(ImGui::RadioButton("C++", &language, 0), "A C++ file using lsd/game.hpp (classes, LSD_NODE, LSD_COMPONENT)");
        ImGui::SameLine();
        with_tip(ImGui::RadioButton("C", &language, 1), "A plain C file using lsd_game.h");
        ImGui::TextUnformatted("Make:    ");
        ImGui::SameLine();
        with_tip(ImGui::RadioButton("Script", &kind, 0), "Behaviour for an object: on_start, on_update, events (Native Script component)");
        ImGui::SameLine();
        with_tip(ImGui::RadioButton("Component", &kind, 1), "A new component type with fields shown in Details");
        ImGui::TextDisabled("Writes Source/<Name>%s, then compiles the game code.", language ? ".c" : ".cpp");
        if (kind == 0) ImGui::TextDisabled("It runs on objects with a Native Script component, like a Lua script on a Script component.");
        else ImGui::TextDisabled("Data of your own: Details > Add Component lists it, levels save it, scripts and nodes read it.");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = with_tip(ImGui::InputText("Name", name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue), "Name of the new script (letters, digits, _)");
        if (with_tip(ImGui::Button("Create"), "Makes the file in Source/ and opens it in VS Code") || enter) {
            if (game_module::new_script(name, language == 1, s.status, sizeof s.status, kind == 1)) {
                LSD_LOG_INFO("game", "%s", s.status);
                game_module::compile();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (with_tip(ImGui::Button("Cancel"), "Does not make anything")) ImGui::CloseCurrentPopup();
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", s.status);
        ImGui::EndPopup();
    }
    draw_reference_viewer(s);
    draw_size_map(s);
    draw_asset_audit(s);
    draw_template_picker(s);
    draw_timeline_editor(s);
    draw_anim_graph_editor(s);
    draw_flipbook_editor(s);
    draw_mesh_editor(s);
    draw_prefab_editor(s);
    debugger::draw_window(s, &s.show_debugger);
    assistant::draw_window(s, &s.show_assistant);
    draw_memory_window(s);
    draw_audio_mixer(s);
    draw_ui_editor(s);
    took(4);
    handle_shortcuts(s);

    // The central node is left empty and transparent: the 3D viewport shows through it.
    s.viewport_count = 0;  // an editor's tab covers the level: nothing renders behind it
    const ImGuiDockNode* central = level_tab ? ImGui::DockBuilderGetCentralNode(dockspace) : nullptr;
    if (central) {
        const u32 panes = s.playing && !s.ejected ? 1u : s.panes;  // the game plays in one big viewport
        s.viewport_count = panes;
        g_request_active = -1;
        if (panes == 1) {
            draw_viewport(s, s.active_view, central->Pos, central->Size);
            s.viewport_count = 1;
            if (s.active_view != 0) {  // single view always uses slot 0
                s.viewport_rects[0][0] = central->Pos.x;
                s.viewport_rects[0][1] = central->Pos.y;
                s.viewport_rects[0][2] = central->Size.x;
                s.viewport_rects[0][3] = central->Size.y;
            }
        } else {
            const f32 gap = 2.0f;
            const f32 rows = panes == 4 ? 2.0f : 1.0f;
            const ImVec2 half((central->Size.x - gap) * 0.5f, (central->Size.y - gap * (rows - 1.0f)) / rows);
            for (int i = 0; i < static_cast<int>(panes); ++i) {
                const ImVec2 at(central->Pos.x + static_cast<f32>(i % 2) * (half.x + gap), central->Pos.y + static_cast<f32>(i / 2) * (half.y + gap));
                if (i != s.active_view) std::swap(s.camera, s.views[i]);  // draw_viewport works on s.camera
                draw_viewport(s, i, at, half);
                if (i != s.active_view) std::swap(s.camera, s.views[i]);
            }
            if (g_request_active >= 0 && g_request_active != s.active_view) {
                s.views[s.active_view] = s.camera;
                s.camera = s.views[g_request_active];
                s.active_view = g_request_active;
            }
        }
    }

    prune_selection(s);  // undo or delete may have removed selected objects
    update_pcg(s);       // before the undo step below, so it holds the rebuilt copies
    if (!s.playing) update_sublevels(s);
    // One undo step per finished edit: wait until drags and text fields are released.
    if (s.pending_edit[0] && (s.playing || (!ImGui::IsAnyItemActive() && !ImGuizmo::IsUsing() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)))) {
        if (!s.playing) {
            record_prefab_structure(s);  // components / objects added or removed inside instances: kept as overrides
            s.history->commit(*s.world, s.pending_edit);  // Play changes are thrown away on Stop
        }
        s.pending_edit[0] = '\0';
    }
}

Vec3 camera_forward(const EditorCamera& cam) {
    switch (cam.view) {
        case ViewType::Top: return {0, 0, -1};
        case ViewType::Front: return {0, 1, 0};   // from -Y toward +Y, like Blender's front view
        case ViewType::Right: return {-1, 0, 0};  // from +X toward -X
        case ViewType::Perspective: break;
    }
    const f32 cp = std::cos(cam.pitch);
    return {cp * std::cos(cam.yaw), cp * std::sin(cam.yaw), std::sin(cam.pitch)};
}

Mat4 camera_view(const EditorCamera& cam) {
    if (cam.view == ViewType::Perspective) return look_at(cam.position, cam.position + camera_forward(cam), Vec3{0, 0, 1});
    const Vec3 up = cam.view == ViewType::Top ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
    return look_at(cam.position - camera_forward(cam) * 500.0f, cam.position, up);
}

Mat4 camera_projection(const EditorCamera& cam, f32 aspect) {
    if (cam.view != ViewType::Perspective) return orthographic(cam.ortho_height * aspect, cam.ortho_height, 0.1f, 2000.0f);
    return perspective(cam.fov_y, aspect, 0.05f, 1000.0f);
}

void load_theme(EditorTheme& theme) { reflect::load_file(theme, kThemePath); }

void make_default_level(world::World& w) {
    w.clear();
    const Entity floor = w.create("Floor");
    world::Transform* t = w.get<world::Transform>(floor);
    t->position = {0, 0, -0.2f};
    t->scale = {20, 20, 0.2f};
    w.add<world::StaticMesh>(floor)->color = LinearColor::hex(0x8A8A8E);
    w.add<world::PlayerStart>(w.create("PlayerStart"));
}

bool open_asset(EditorState& s, const asset::AssetRecord& record) { return open_record(s, record); }
bool open_size_map(EditorState& s, const char* name_or_folder) { return open_size_map_for(s, name_or_folder); }
void open_asset_audit(EditorState& s) { open_asset_audit_window(s); }
u32 fix_up_redirectors(EditorState& s) { return fix_up_asset_redirectors(s); }
DetailsStyle editor_details_style(EditorState& s) { return {{u32_color(s.theme.details_row_a), u32_color(s.theme.details_row_b)}, nullptr, details_widget, &s}; }

bool surface_ray(EditorState& s, Vec3 origin, Vec3 dir, f32& t) {
    Entity hit;
    return raycast(s, {origin, dir}, {}, hit, t);
}
void viewport_ray(const EditorCamera& camera, ImVec2 pos, ImVec2 size, ImVec2 mouse, Vec3& origin, Vec3& dir) {
    const Ray ray = mouse_ray(camera, pos, size, mouse);
    origin = ray.origin;
    dir = ray.dir;
}
Entity place_object(EditorState& s, const char* name, const char* mesh, const char* component, Vec3 position) { return place(s, name, mesh, component, position); }
void mark_edited(EditorState& s, const char* label) { edited(s, label); }
void new_data_asset(EditorState& s, const reflect::TypeInfo& type) { create_data_asset(s, type); }

bool begin_editor(const char* title, bool* open) {
    const std::string id = window_id(title);
    auto it = std::find_if(g_tabs.begin(), g_tabs.end(), [&](const PanelTab& t) { return t.id == id; });
    if (g_editor_area) {  // the workspace's editor area holds it, inside the Level
        if (it != g_tabs.end()) {  // it was a panel tab: move it in
            g_tabs.erase(it);
            ImGui::SetNextWindowDockID(g_editor_area, ImGuiCond_Always);
        } else {
            ImGui::SetNextWindowDockID(g_editor_area, ImGuiCond_Appearing);
        }
        if (!ImGui::Begin(title, open)) {
            ImGui::End();
            return false;
        }
        return true;
    }
    if (it == g_tabs.end()) {  // just opened: its tab comes forward, like Unreal's asset editors
        g_tabs.push_back({id, title, true});
        it = g_tabs.end() - 1;
        g_select_tab = id;
    }
    it->label = title;
    it->seen = true;
    const bool closing = g_close_tab == id;
    if (closing) {
        *open = false;  // the editor runs its own close (saving a changed graph...)
        g_close_tab.clear();
    } else if (g_active_tab != id) {
        return false;  // behind another tab
    }
    const ImGuiViewport* main = ImGui::GetMainViewport();  // fills the space under the panel tabs
    ImGui::SetNextWindowPos(main->WorkPos);
    ImGui::SetNextWindowSize(main->WorkSize);
    ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
    if (g_focused_tab != id) {  // once per switch: other windows (Preferences...) can still come forward
        ImGui::SetNextWindowFocus();
        g_focused_tab = id;
    }
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin(title, open, flags)) {
        ImGui::End();
        return false;
    }
    return true;
}

void apply_layout_file(const char* path, char* status, usize status_capacity) {
    if (!std::filesystem::exists(path)) {
        std::snprintf(status, status_capacity, "Layout %s not found", path);
        return;
    }
    ImGui::LoadIniSettingsFromDisk(path);
    // Settings only reach windows when they are created, so move the open windows to their saved nodes.
    ImGuiContext& g = *ImGui::GetCurrentContext();
    for (ImGuiWindowSettings* ws = g.SettingsWindows.begin(); ws != nullptr; ws = g.SettingsWindows.next_chunk(ws)) {
        ImGuiWindow* window = ImGui::FindWindowByID(ws->ID);
        if (!window) continue;
        if (ws->DockId) ImGui::DockBuilderDockWindow(window->Name, ws->DockId);
        else {
            ImGui::SetWindowPos(window, ImVec2(static_cast<f32>(ws->Pos.x), static_cast<f32>(ws->Pos.y)));
            ImGui::SetWindowSize(window, ImVec2(static_cast<f32>(ws->Size.x), static_cast<f32>(ws->Size.y)));
        }
    }
    std::snprintf(status, status_capacity, "Loaded layout %s", std::filesystem::path(path).stem().string().c_str());
}

void create_parts_prefab(EditorState& state, const asset::AssetRecord& mesh) { make_parts_prefab(state, mesh); }

} // namespace lsd::editor
