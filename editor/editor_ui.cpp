#include "editor_ui.h"
#include "graph_editor.h"
#include "lighting.h"
#include "material_cache.h"
#include "skinned_cache.h"
#include "sound_player.h"
#include "table_editor.h"
#include "particle_editor.h"
#include "ui_editor.h"
#include "packaging.h"

#include "lsd/script/material_graph.h"

#include "mesh_cache.h"
#include "debug_draw.h"
#include "references.h"
#include "remote_console.h"
#include "source_control.h"
#include "thumbnails.h"
#include "transactions.h"

#include "lsd/asset/data_asset.h"
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

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder API
#include <ImGuizmo.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>

namespace lsd::editor {

namespace {

bool asset_selected(const EditorState& s, asset::Guid g);
extern ReferenceIndex g_references;

// A Lua script's `properties = { name = value, ... }` defaults, read from its text: (name, value as text).
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
    auto skip = [&] { while (i < end && (std::isspace(static_cast<unsigned char>(text[i])) || text[i] == ',' || text[i] == ';')) ++i; };
    for (skip(); i < end; skip()) {
        size_t name_end = i;
        while (name_end < end && (std::isalnum(static_cast<unsigned char>(text[name_end])) || text[name_end] == '_')) ++name_end;
        if (name_end == i) break;
        const std::string name = text.substr(i, name_end - i);
        i = text.find('=', name_end);
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
    ImGui::DragFloat("Radius (m)", &radius, 0.1f, 0.1f, 1000.0f);
    ImGui::DragFloatRange2("Scale", &min_scale, &max_scale, 0.01f, 0.05f, 10.0f);
    if (ImGui::Button("Scatter")) {  // the edit is undoable like any Details change
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
    if (ImGui::Button("Clear")) ism.instances.count = 0;
}

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
    for (const auto& [name, fallback] : defaults) {
        world::ScriptProperty* own = nullptr;
        for (world::ScriptProperty& p : script.properties) {
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
            if (ImGui::SmallButton("Reset")) {  // back to the script's default
                changed = fallback;
                edited_now = true;
            }
        }
        ImGui::PopID();
        if (!edited_now) continue;
        if (changed == fallback) {  // the default again: no override
            for (u32 k = 0; k < script.properties.count; ++k) {
                if (name == script.properties[k].name) script.properties.remove(k);
            }
        } else if (own) {
            std::snprintf(own->value, sizeof own->value, "%s", changed.c_str());
        } else {
            world::ScriptProperty p;
            std::snprintf(p.name, sizeof p.name, "%s", name.c_str());
            std::snprintf(p.value, sizeof p.value, "%s", changed.c_str());
            script.properties.push_back(p);
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
    {"Post Process Volume", nullptr, "PostProcessVolume"}, {"Lightmass Importance Volume", nullptr, "LightmassImportanceVolume"}, {"Nav Mesh Bounds", nullptr, "NavMeshBounds"}, {"Nav Agent", "Cube", "NavAgent"}, {"AI Controller", nullptr, "AIController"}, {"Audio Volume", nullptr, "AudioVolume"},
    {"Directional Light", nullptr, "DirectionalLight"}, {"Point Light", nullptr, "PointLight"}, {"Spot Light", nullptr, "SpotLight"}, {"Sky Light", nullptr, "SkyLight"}, {"Empty", nullptr, nullptr},
};

// Volumes are the unit cube scaled by the transform (see components.h).
constexpr Vec3 kVolumeLo{-0.5f, -0.5f, 0.0f}, kVolumeHi{0.5f, 0.5f, 1.0f};

bool is_volume(const world::World& w, world::Entity e) {
    const auto* post = w.get<world::PostProcessVolume>(e);
    return w.get<world::BlockingVolume>(e) || w.get<world::KillVolume>(e) || w.get<world::TriggerVolume>(e) || w.get<world::AudioVolume>(e) || (post && !post->unbound) ||
           w.get<world::NavMeshBounds>(e) || w.get<world::LightmassImportanceVolume>(e);
}

// UI-only state for the confirmation dialogs.
char g_confirm_open[260] = "";  // level to open after "discard changes?"
bool g_confirm_new = false;
bool g_open_confirm = false;
bool g_open_save_as = false;
char g_save_as_name[96] = "";
bool g_was_using_gizmo[4] = {};  // per viewport
bool g_alt_duplicated[4] = {};
int g_request_active = -1;       // viewport clicked this frame

// The data asset shown in Details (loaded on selection, written back on Save).
alignas(16) u8 g_data[32768];
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
    if (e != s.selected) s.spline_point = -1;
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

// World-space box of an entity's mesh; a small box around the origin when it has none.
void world_bounds(EditorState& s, Entity e, Vec3& lo, Vec3& hi) {
    const Mat4 m = s.world->world_matrix(e);
    Vec3 local_lo{-0.25f, -0.25f, 0.0f}, local_hi{0.25f, 0.25f, 1.8f};
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
        if (const SkinnedEntry* model = s.skinned->get(skeletal->mesh)) {  // the rest pose
            local_lo = model->model->bounds_min;
            local_hi = model->model->bounds_max;
        }
    }
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

// Nearest mesh under the ray. shortcut: tests each mesh's bounding box, not its triangles;
// triangle-accurate picking when imported meshes get detailed.
// Hidden objects are never hit; with `pickable`, locked ones are skipped too (viewport clicks).
bool raycast(EditorState& s, const Ray& ray, Entity ignore, Entity& hit, f32& hit_t, bool pickable = false) {
    const reflect::TypeInfo& type = reflect::type_of<world::StaticMesh>();
    hit_t = 1e30f;
    for (u32 i = 0; i < s.world->count(type); ++i) {
        const Entity e = s.world->owner(type, i);
        if (e == ignore || s.world->hidden_in_hierarchy(e) || (pickable && s.world->locked(e))) continue;
        const CachedMesh* cached = s.meshes->get(static_cast<const world::StaticMesh*>(s.world->at(type, i))->mesh);
        if (!cached) continue;
        const Mat4 inv = affine_inverse(s.world->world_matrix(e));
        f32 t;
        if (ray_box(transform_point(inv, ray.origin), transform_vector(inv, ray.dir), cached->bounds_min, cached->bounds_max, t) && t < hit_t) {
            hit_t = t;
            hit = e;
        }
    }
    const reflect::TypeInfo& skinned_type = reflect::type_of<world::SkeletalMesh>();
    for (u32 i = 0; i < s.world->count(skinned_type) && s.skinned; ++i) {  // rigged meshes by their rest-pose box
        const Entity e = s.world->owner(skinned_type, i);
        if (e == ignore || s.world->hidden_in_hierarchy(e) || (pickable && s.world->locked(e))) continue;
        const SkinnedEntry* model = s.skinned->get(static_cast<const world::SkeletalMesh*>(s.world->at(skinned_type, i))->mesh);
        if (!model) continue;
        const Mat4 inv = affine_inverse(s.world->world_matrix(e));
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

void build_default_layout(ImGuiID dockspace, ImVec2 size) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_DockSpace) | ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::DockBuilderSetNodeSize(dockspace, size);
    ImGuiID right = 0, left = 0, right_top = 0, right_bottom = 0, content = 0, viewport = 0;
    ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Right, 0.22f, &right, &left);
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.45f, &right_top, &right_bottom);
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.30f, &content, &viewport);
    ImGui::DockBuilderDockWindow(kOutlinerWindow, right_top);
    ImGui::DockBuilderDockWindow(kDetailsWindow, right_bottom);
    ImGui::DockBuilderDockWindow(kContentWindow, content);
    ImGui::DockBuilderFinish(dockspace);
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
    if (hovered && io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) orbiting = true;
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
    const auto* spline = w.get<world::Spline>(e);
    if (!spline) return 0;
    const Mat4 m = w.world_matrix(e);
    for (u32 i = 0; i < spline->points.count; ++i) out[i] = transform_point(m, spline->points[i]);
    return spline->points.count;
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
    Mat4 m = translation(transform_point(world, spline->points[static_cast<u32>(s.spline_point)]));
    const f32 snap[3] = {s.settings.snap_move, s.settings.snap_move, s.settings.snap_move};
    ImGuizmo::SetOrthographic(s.camera.view != ViewType::Perspective);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(pos.x, pos.y, size.x, size.y);
    ImGui::PushID(index + 100);
    if (ImGuizmo::Manipulate(view.m, proj.m, ImGuizmo::TRANSLATE, ImGuizmo::WORLD, m.m, nullptr, s.settings.snap ? snap : nullptr)) {
        spline->points[static_cast<u32>(s.spline_point)] = transform_point(affine_inverse(world), {m.at(0, 3), m.at(1, 3), m.at(2, 3)});
        edited(s, "Move Spline Point");
    }
    const bool busy = ImGuizmo::IsUsing() || ImGuizmo::IsOver();
    ImGui::PopID();
    return busy;
}

bool draw_gizmo(EditorState& s, const Mat4& view, const Mat4& proj, ImVec2 pos, ImVec2 size, int index) {
    if (s.spline_point >= 0 && s.world->alive(s.selected) && !s.world->locked(s.selected) && !s.game_view && !(s.playing && !s.ejected)) {
        return draw_spline_point_gizmo(s, view, proj, pos, size, index);
    }
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
    const Mat4 before = s.world->world_matrix(s.selected);
    Mat4 m = before;
    ImGui::PushID(index);  // one gizmo per viewport, each with its own drag state
    const bool changed = ImGuizmo::Manipulate(view.m, proj.m, op, s.gizmo_local ? ImGuizmo::LOCAL : ImGuizmo::WORLD, m.m, nullptr,
                                              s.settings.snap ? snap : nullptr);
    const bool using_now = ImGuizmo::IsUsing();
    const bool over = ImGuizmo::IsOver();
    ImGui::PopID();
    if (using_now && !was_using) alt_duplicated = false;
    if (changed) {
        if (ImGui::GetIO().KeyAlt && !alt_duplicated) {
            duplicate_selected(s);  // copies are made from the untouched originals; they get the move
            alt_duplicated = true;
        }
        // The other selected objects follow the primary: same change, pivoting on the primary.
        const Mat4 delta = m * affine_inverse(before);
        Entity roots[EditorState::kMaxSelection];
        const u32 count = selection_roots(s, roots);
        for (u32 i = 0; i < count; ++i) {
            const world::Transform moved_before = *s.world->get<world::Transform>(roots[i]);
            if (roots[i] != s.selected) s.world->set_world_matrix(roots[i], delta * s.world->world_matrix(roots[i]));
            else s.world->set_world_matrix(s.selected, m);
            note_changed_overrides(s, roots[i], reflect::type_of<world::Transform>(), reinterpret_cast<const u8*>(&moved_before),
                                   reinterpret_cast<const u8*>(s.world->get<world::Transform>(roots[i])));
        }
        edited(s, alt_duplicated ? "Duplicate" : s.gizmo == GizmoMode::Translate ? "Move" : s.gizmo == GizmoMode::Rotate ? "Rotate" : "Scale");
    }
    was_using = using_now;
    return using_now || over;
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
        Vec3 points[32];
        const u32 n = spline_world_points(*s.world, e, points);
        const bool selected = s.is_selected(e);
        const ImU32 color = selected ? u32_color(t.details_tab_active) : IM_COL32(120, 200, 255, 220);
        const u32 samples = world::spline_segments(n, spline->closed) * 12;
        ImVec2 previous;
        bool have_previous = false;
        for (u32 k = 0; k <= samples; ++k) {
            ImVec2 p;
            const bool visible = project(view_proj, world::spline_point(points, n, spline->closed, static_cast<f32>(k) / 12.0f), pos, size, p);
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
            dl->AddCircle(base, 7.0f, color, 12, 2.0f);
            dl->AddText(ImVec2(base.x + 10, base.y - 8), color, s.world->name(e));
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

// Turns the 2x2 layout on or off. The first time, the extra views are framed around the camera's pivot.
void set_quad_view(EditorState& s, bool on) {
    if (on == s.quad_view) return;
    if (on && s.views[1].view == ViewType::Perspective) {
        const Vec3 pivot = s.camera.position + camera_forward(s.camera) * s.camera.pivot_distance;
        const ViewType types[3] = {ViewType::Top, ViewType::Front, ViewType::Right};
        for (int i = 1; i < 4; ++i) {
            s.views[i].view = types[i - 1];
            s.views[i].position = pivot;
            s.views[i].ortho_height = 24.0f;
        }
        s.views[0] = s.camera;
    }
    if (!on && s.active_view != 0) {  // back to one viewport: keep showing the active camera
        s.views[0] = s.camera;
        s.active_view = 0;
    }
    s.quad_view = on;
}

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
    ImGui::InvisibleButton("##surface", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                    ImGui::IsMouseClicked(ImGuiMouseButton_Middle))) {
        g_request_active = index;
    }
    if (s.quad_view && active) {  // gold frame on the active viewport
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
    // Click selects; Ctrl+click toggles, Shift+click adds; a drag from empty space draws a selection box.
    static bool marquee_by_view[4];
    static ImVec2 marquee_start_by_view[4];
    bool& marquee = marquee_by_view[index];
    ImVec2& marquee_start = marquee_start_by_view[index];
    const i32 clicked_point = ImGui::IsItemClicked(ImGuiMouseButton_Left) && !io.KeyAlt && !gizmo_busy ? spline_point_at(s, view_proj, pos, size, io.MousePos) : -1;
    if (clicked_point >= 0) s.spline_point = clicked_point;  // the gizmo now moves that point
    else if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !io.KeyAlt && !gizmo_busy) {
        const Entity hit = pick(s, view_proj, pos, size, io.MousePos);
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
    if (s.show_navigation && !s.game_view) {  // the navmesh's polygons, like Unreal's P
        for (u32 i = 0; i + 1 < s.nav_line_count; i += 2) {
            ImVec2 a, b;
            if (project(view_proj, s.nav_lines[i], pos, size, a) && project(view_proj, s.nav_lines[i + 1], pos, size, b)) dl->AddLine(a, b, IM_COL32(60, 230, 90, 200), 1.5f);
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
        static const char* const kModes[] = {"Lit", "Unlit", "Wireframe"};
        static const char* const kModeKeys[] = {"Alt+4", "Alt+3", "Alt+2"};
        if (ImGui::Button(kViews[static_cast<int>(s.camera.view)])) ImGui::OpenPopup("ViewMenu");
        if (ImGui::BeginPopup("ViewMenu")) {
            for (int v = 0; v < 4; ++v) {
                if (ImGui::MenuItem(kViews[v], kViewKeys[v], static_cast<int>(s.camera.view) == v)) set_view(s, static_cast<ViewType>(v));
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(kModes[static_cast<int>(s.view_mode)])) ImGui::OpenPopup("ModeMenu");
        if (ImGui::BeginPopup("ModeMenu")) {
            for (int m = 0; m < 3; ++m) {
                if (ImGui::MenuItem(kModes[m], kModeKeys[m], static_cast<int>(s.view_mode) == m)) s.view_mode = static_cast<ViewMode>(m);
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(s.quad_view ? "2x2" : "1x1")) set_quad_view(s, !s.quad_view);
        ImGui::SetItemTooltip("Viewport layout: one, or perspective + top + front + right");
        if (s.quad_view && !active) {  // the other viewports keep a short toolbar
            ImGui::End();
            return;
        }
        ImGui::SameLine();
        if (ImGui::Button("Show")) ImGui::OpenPopup("ShowMenu");
        if (ImGui::BeginPopup("ShowMenu")) {
            ImGui::Checkbox("Grid", &s.show_grid);
            ImGui::Checkbox("Axis gizmo", &s.show_axis_gizmo);
            ImGui::Checkbox("Navigation (P)", &s.show_navigation);
            ImGui::Checkbox("Game view (G)", &s.game_view);
            ImGui::Checkbox("Stats (stat fps)", &s.show_stats);
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (toggle_button("Grid", s.show_grid)) s.show_grid = !s.show_grid;
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (toggle_button("Move", s.gizmo == GizmoMode::Translate)) s.gizmo = GizmoMode::Translate;
        ImGui::SetItemTooltip("W");
        ImGui::SameLine();
        if (toggle_button("Rotate", s.gizmo == GizmoMode::Rotate)) s.gizmo = GizmoMode::Rotate;
        ImGui::SetItemTooltip("E");
        ImGui::SameLine();
        if (toggle_button("Scale", s.gizmo == GizmoMode::Scale)) s.gizmo = GizmoMode::Scale;
        ImGui::SetItemTooltip("R");
        ImGui::SameLine();
        if (ImGui::Button(s.gizmo_local ? "Local" : "World")) s.gizmo_local = !s.gizmo_local;
        ImGui::SetItemTooltip("Gizmo space (Ctrl+`)");
        ImGui::SameLine();
        if (toggle_button("Snap", s.settings.snap)) s.settings.snap = !s.settings.snap;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        f32* step = s.gizmo == GizmoMode::Translate ? &s.settings.snap_move : s.gizmo == GizmoMode::Rotate ? &s.settings.snap_angle : &s.settings.snap_scale;
        ImGui::DragFloat("##snap", step, 0.05f, 0.01f, 360.0f, s.gizmo == GizmoMode::Rotate ? "%.0f deg" : s.gizmo == GizmoMode::Translate ? "%.2f m" : "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::SliderFloat("##speed", &s.settings.camera_speed, 0.5f, 50.0f, "speed %.1f", ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("Camera speed (RMB + wheel)");
        if (!s.quad_view) {
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
};

void collect_rows(const world::World& w, Entity parent, int depth, OutlineRow* rows, int& count, int max) {
    // shortcut: scans all slots per level of the tree; keep child lists when levels get big.
    for (u32 slot = 0; slot < w.slot_count() && count < max; ++slot) {
        const Entity e = w.entity_at_slot(slot);
        if (!e.valid() || w.parent(e) != parent) continue;
        rows[count++] = {e, depth};
        collect_rows(w, e, depth + 1, rows, count, max);
    }
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
    for (int i = 0; i < visible; ++i) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(ImVec2(left, p.y), ImVec2(right, p.y + row_height), u32_color((i & 1) ? t.outline_row_b : t.outline_row_a));
        if (i >= row_count) { ImGui::Dummy(ImVec2(0, row_height - ImGui::GetStyle().ItemSpacing.y)); continue; }
        const OutlineRow& row = rows[i];
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
                for (int r = lo; r <= hi; ++r) add_to_selection(s, rows[r].entity, false);
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
            if (ImGui::MenuItem("Focus", "F")) focus_selected(s);
            if (s.playing && ImGui::MenuItem("Keep Simulation Changes", "K")) keep_simulation_changes(s);
            if (s.playing) ImGui::SetItemTooltip("When Play stops, the selected objects keep what they have now (where physics left them...)");
            asset::Guid used;  // the object's mesh (or script, sound) asset
            if (const auto* m = s.world->get<world::StaticMesh>(e)) asset::parse_guid(m->mesh, used);
            if (const auto* m = s.world->get<world::SkeletalMesh>(e); m && !used.valid()) asset::parse_guid(m->mesh, used);
            if (const auto* m = s.world->get<world::Script>(e); m && !used.valid()) asset::parse_guid(m->script, used);
            if (const auto* m = s.world->get<world::SoundEmitter>(e); m && !used.valid()) asset::parse_guid(m->sound, used);
            if (ImGui::MenuItem("Find in Content Browser", "Ctrl+B", false, used.valid())) find_in_content_browser(s, used);
            if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicate_selected(s);
            if (ImGui::MenuItem("Detach", nullptr, false, s.world->parent(e).valid())) {
                s.world->set_parent(e, {});
                edited(s, "Detach");
            }
            if (ImGui::MenuItem("Delete", "Delete")) delete_selected(s);
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
    if (ImGui::BeginTabBar("OutlinerTabs")) {
        if (themed_tab("Outline", 0, active_tab, t.outline_tab, t.panel_background, LinearColor::hex(0xFFFFFF))) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##filter", "Search objects", filter, sizeof filter);
            int count = 0;
            for (u32 slot = 0; slot < s.world->slot_count() && count < 4096; ++slot) {
                const Entity e = s.world->entity_at_slot(slot);
                if (e.valid() && contains_ignore_case(s.world->name(e), filter)) rows[count++] = {e, 0};
            }
            draw_rows(s, rows, count, false);
            ImGui::EndTabItem();
        }
        if (themed_tab("Hierarchy", 1, active_tab, t.outline_tab, t.panel_background, LinearColor::hex(0xFFFFFF))) {
            int count = 0;
            collect_rows(*s.world, {}, 0, rows, count, 4096);
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
        if (ImGui::Selectable("None", !guid_text[0])) {
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
        ImGui::Button("Press a key... (Esc cancels)", ImVec2(-1.0f, 0));
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

bool details_widget(void* user, const reflect::Property& p, u8* field, bool& changed) {
    auto& s = *static_cast<EditorState*>(user);
    if (p.type != reflect::FieldType::String) return false;
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
    LSD_ASSERT(g_data_type->size <= sizeof g_data);
    std::memcpy(g_data, g_data_type->defaults, g_data_type->size);
    char path[300];
    s.registry->source_path(r, path, sizeof path);
    if (!asset::load_data_asset(path, *g_data_type, g_data)) std::snprintf(s.status, sizeof s.status, "Could not read %s", r.path);
}

// Graph of a Curve asset: drag keys, double-click to add one, right-click a key to delete it.
bool draw_curve_editor(world::Curve& curve) {
    static f32 view[4] = {0, 1, 0, 1};  // time min / max, value min / max (kept while dragging)
    static i32 dragging = -1;
    bool changed = false;
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 220.0f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("curve_canvas", size, ImGuiButtonFlags_MouseButtonLeft);
    const bool canvas_hovered = ImGui::IsItemHovered();
    if (dragging < 0 && curve.keys.count) {  // fit the keys
        view[0] = view[1] = curve.keys[0].time;
        view[2] = view[3] = curve.keys[0].value;
        for (const world::CurveKey& k : curve.keys) {
            view[0] = k.time < view[0] ? k.time : view[0];
            view[1] = k.time > view[1] ? k.time : view[1];
            view[2] = k.value < view[2] ? k.value : view[2];
            view[3] = k.value > view[3] ? k.value : view[3];
        }
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
    ImVec2 previous;
    for (int i = 0; i <= 120; ++i) {
        const f32 t = view[0] + (view[1] - view[0]) * static_cast<f32>(i) / 120.0f;
        const ImVec2 p = to_screen(t, world::evaluate(curve, t));
        if (i) dl->AddLine(previous, p, IM_COL32(240, 200, 60, 255), 2.0f);
        previous = p;
    }
    const ImGuiIO& io = ImGui::GetIO();
    i32 hovered_key = -1;
    for (u32 i = 0; i < curve.keys.count; ++i) {
        const ImVec2 p = to_screen(curve.keys[i].time, curve.keys[i].value);
        const bool hot = (io.MousePos.x - p.x) * (io.MousePos.x - p.x) + (io.MousePos.y - p.y) * (io.MousePos.y - p.y) < 64.0f;
        if (hot) hovered_key = static_cast<i32>(i);
        dl->AddCircleFilled(p, hot || dragging == static_cast<i32>(i) ? 6.0f : 4.5f, IM_COL32(255, 255, 255, 255));
    }
    if (canvas_hovered && hovered_key >= 0) {
        ImGui::SetTooltip("%.3f s  =  %.3f", static_cast<double>(curve.keys[static_cast<u32>(hovered_key)].time), static_cast<double>(curve.keys[static_cast<u32>(hovered_key)].value));
    }
    if (ImGui::IsItemActivated() && hovered_key >= 0) dragging = hovered_key;
    if (dragging >= 0 && static_cast<u32>(dragging) < curve.keys.count) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            to_curve(io.MousePos, curve.keys[static_cast<u32>(dragging)].time, curve.keys[static_cast<u32>(dragging)].value);
            changed = true;
        } else {
            dragging = -1;
            world::sort_keys(curve);
            changed = true;
        }
    }
    if (canvas_hovered && hovered_key < 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && curve.keys.count < curve.keys.capacity()) {
        world::CurveKey key;
        to_curve(io.MousePos, key.time, key.value);
        curve.keys.push_back(key);
        world::sort_keys(curve);
        changed = true;
    }
    if (canvas_hovered && hovered_key >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && curve.keys.count > 1) {
        curve.keys.remove(static_cast<u32>(hovered_key));
        changed = true;
    }
    ImGui::TextDisabled("Drag keys. Double-click: add a key. Right-click a key: delete it.");
    return changed;
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

namespace {

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
    ImGui::BeginDisabled(!g_data_dirty);
    if (ImGui::Button("Save")) {
        if (asset::save_data_asset(path, *g_data_type, g_data)) {
            g_data_dirty = false;
            std::snprintf(s.status, sizeof s.status, "Saved %s", r.path);
            s.registry->scan();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) load_selected_data(s, r);
    ImGui::EndDisabled();
    ImGui::BeginChild("DataRows");
    const DetailsStyle style{{u32_color(t.details_row_a), u32_color(t.details_row_b)}, nullptr, details_widget, &s};
    if (g_data_type == &reflect::type_of<world::Curve>() && draw_curve_editor(*reinterpret_cast<world::Curve*>(g_data))) g_data_dirty = true;
    if (g_data_type == &reflect::type_of<render::MaterialInstance>() && fill_graph_parameters(s, *reinterpret_cast<render::MaterialInstance*>(g_data))) g_data_dirty = true;
    if (draw_properties(*g_data_type, g_data, &style)) g_data_dirty = true;
    ImGui::EndChild();
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

void create_script(EditorState& s) {
    const std::string path = new_asset_path(s, "NewScript", ".lua");
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return;
    std::fputs(kScriptTemplate, file);
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
    if (asset::parse_guid(mesh.mesh, guid)) {
        const asset::AssetRecord* r = s.registry->find(guid);
        shown = r ? r->name : "(missing asset)";
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
            if (ImGui::Button("Apply to Prefab") && !s.playing) {
                world::save_prefab_file(w, root, path);
                w.get<world::PrefabInstance>(root)->overrides.count = 0;  // its values are the prefab's now
                const u32 others = refresh_instances(s, guid, root);
                std::snprintf(s.status, sizeof s.status, "Saved the prefab; updated %u other instance%s", others, others == 1 ? "" : "s");
                edited(s, "Apply Prefab");
            }
            ImGui::SetItemTooltip("Saves this instance's children and components to the prefab and updates every other instance in the level");
            ImGui::SameLine();
            if (ImGui::Button("Revert") && !s.playing) {
                w.get<world::PrefabInstance>(root)->overrides.count = 0;
                world::update_instance_from_file(w, root, path);
                prune_selection(s);
                edited(s, "Revert Prefab");
            }
            ImGui::SetItemTooltip("Throws away every change to this instance (except its placement) and reloads it from the prefab. One value: right-click it > Revert to Prefab");
            ImGui::SameLine();
            if (ImGui::Button("Browse")) {
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
            static u8 before[8192];  // PrefabInstance with its override list is ~4 KB
            LSD_ASSERT(types[i]->size <= sizeof before);
            g_details_type = types[i];
            std::memcpy(before, component, types[i]->size);
            if (types[i] == &reflect::type_of<world::StaticMesh>()) draw_mesh_slot(s, *static_cast<world::StaticMesh*>(component));
            if (types[i] == &reflect::type_of<world::Spline>()) {  // point tools; points are moved in the viewport
                auto& points = static_cast<world::Spline*>(component)->points;
                const bool has_point = s.spline_point >= 0 && static_cast<u32>(s.spline_point) < points.count;
                if (ImGui::Button("Add Point") && points.count < points.capacity()) {
                    // After the selected point (or at the end), continuing the curve's direction.
                    const u32 after = has_point ? static_cast<u32>(s.spline_point) : points.count - 1;
                    const Vec3 a = points.count ? points[after] : Vec3{};
                    const Vec3 b = after > 0 ? points[after - 1] : a - Vec3{2, 0, 0};
                    const Vec3 next = after + 1 < points.count ? (a + points[after + 1]) * 0.5f : a + (a - b);
                    for (u32 k = points.count; k > after + 1; --k) points[k] = points[k - 1];
                    points[after + 1] = next;
                    ++points.count;
                    s.spline_point = static_cast<i32>(after + 1);
                    edited(s, "Add Spline Point");
                }
                ImGui::SameLine();
                if (ImGui::Button("Remove Point") && has_point && points.count > 2) {
                    points.remove(static_cast<u32>(s.spline_point));
                    s.spline_point = -1;
                    edited(s, "Remove Spline Point");
                }
                ImGui::SameLine();
                ImGui::TextDisabled(has_point ? "Point %d selected" : "Click a point in the viewport", s.spline_point);
            }
            draw_properties(*types[i], component, &style);
            if (types[i] == &reflect::type_of<world::Script>()) draw_script_properties(s, *static_cast<world::Script*>(component));
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
    if (ImGui::Button("+ Add Component", ImVec2(-1.0f, 0))) ImGui::OpenPopup("AddComponent");
    if (ImGui::BeginPopup("AddComponent")) {
        for (u32 i = 1; i < type_count; ++i) {
            if (!w.get(e, *types[i]) && ImGui::MenuItem(types[i]->name)) {
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
                if (ImGui::Button("Save")) {
                    // The data compiler sees the new settings and rebuilds; placed copies update live.
                    std::snprintf(s.status, sizeof s.status, s.registry->save_meta(*selected, s.selected_meta) ? "Saved %s.meta" : "Could not save %s.meta", selected->name);
                }
                ImGui::SameLine();
                if (ImGui::Button("Revert")) s.registry->load_meta(*selected, s.selected_meta);
                if (selected->type == asset::AssetType::Sound || selected->type == asset::AssetType::Cue) {
                    ImGui::SameLine();
                    if (ImGui::Button(s.sounds && s.sounds->audio()->playing(s.preview_voice) ? "Stop" : "Play")) toggle_preview(s, *selected);
                }
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
            } else if (s.registry->move_asset(*r, folder, error, sizeof error)) {
                std::snprintf(s.status, sizeof s.status, "Moved to Content/%s", folder);
            } else {
                std::snprintf(s.status, sizeof s.status, "Move failed: %s", error);
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
        if ((ImGui::Button("Rename") || enter) && ok) {
            char error[200] = "";
            bool done = false;
            if (const asset::AssetRecord* r = s.rename_asset.valid() ? s.registry->find(s.rename_asset) : nullptr) {
                char old_path[300];
                s.registry->source_path(*r, old_path, sizeof old_path);
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
                else std::filesystem::rename(from, to, ec);
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
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
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
        if (ImGui::Button("Delete")) {
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
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// Draws the sub-folders of `dir` as tree nodes; clicking one selects it. Returns false if none.
// shortcut: reads the disk every frame; move to the registry's index when folders get many.
bool draw_folder_tree(EditorState& s, const std::filesystem::path& dir, ImGuiTreeNodeFlags flags) {
    std::error_code error;
    bool any = false;
    for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
        if (!entry.is_directory(error)) continue;
        any = true;
        const std::string name = entry.path().filename().string();
        const std::string relative = std::filesystem::relative(entry.path(), kContentDir, error).generic_string();
        const bool selected = relative == s.content_folder;
        const bool open = ImGui::TreeNodeEx(name.c_str(), flags | (selected ? ImGuiTreeNodeFlags_Selected : 0));
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            std::snprintf(s.content_folder, sizeof s.content_folder, "%s", relative.c_str());
            s.search[0] = '\0';
        }
        folder_drop_target(s, relative.c_str());
        if (open) {
            draw_folder_tree(s, entry.path(), flags);
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
        if (ImGui::SmallButton("Replace with selected")) {  // fix a broken reference with the Content Browser's selection
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
    if (ImGui::Button("Refresh")) g_references.build(*s.registry);
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
        if (ImGui::MenuItem("New Folder")) {
            std::error_code error;
            const std::filesystem::path base = std::filesystem::path(kContentDir) / s.content_folder;
            std::filesystem::path dir = base / "NewFolder";
            for (int n = 2; std::filesystem::exists(dir, error); ++n) dir = base / ("NewFolder" + std::to_string(n));
            std::filesystem::create_directories(dir, error);
            start_rename(s, {}, join_folder(s.content_folder, dir.filename().string().c_str()).c_str(), dir.filename().string().c_str());
        }
        if (ImGui::BeginMenu("New Data Asset")) {
            u32 type_count = 0;
            const reflect::TypeInfo* const* types = asset::data_types(type_count);
            for (u32 i = 0; i < type_count; ++i) {
                if (ImGui::MenuItem(types[i]->name)) create_data_asset(s, *types[i]);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("New Visual Script")) {
            const std::string path = create_graph_file((std::filesystem::path(kContentDir) / s.content_folder).generic_string().c_str(), "NewVisualScript");
            s.registry->scan();
            if (const asset::AssetRecord* r = path.empty() ? nullptr : record_at(s, path)) {
                select(s, {});
                s.selected_asset = r->guid;
                start_rename(s, r->guid, "", r->name);
            }
        }
        ImGui::SetItemTooltip("A node graph (like a Blueprint event graph); double-click to edit, pick it in a Script component");
        if (ImGui::MenuItem("New Data Table")) {
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
        if (ImGui::MenuItem("New Material Graph")) {
            const std::string path = create_material_graph_file((std::filesystem::path(kContentDir) / s.content_folder).generic_string().c_str(), "M_New");
            s.registry->scan();
            if (const asset::AssetRecord* r = path.empty() ? nullptr : record_at(s, path)) {
                select(s, {});
                s.selected_asset = r->guid;
                start_rename(s, r->guid, "", r->name);
            }
        }
        ImGui::SetItemTooltip("A material built from nodes (textures, maths, Time, Fresnel...) like Unreal's Material Editor. Double-click to edit; drag onto an object");
        if (ImGui::MenuItem("New UI Document")) {
            const std::string path = create_ui_document((std::filesystem::path(kContentDir) / s.content_folder).generic_string().c_str(), "UI_New");
            s.registry->scan();
            if (const asset::AssetRecord* r = path.empty() ? nullptr : record_at(s, path)) {
                select(s, {});
                s.selected_asset = r->guid;
                start_rename(s, r->guid, "", r->name);
            }
        }
        ImGui::SetItemTooltip("Game UI in HTML / CSS (Unreal's Widget Blueprint): a HUD or a menu. Open it from Lua (lsd.ui_open) or as the Game Mode's HUD");
        if (ImGui::MenuItem("New Behavior Tree")) {
            const std::string path = create_behavior_tree_file((std::filesystem::path(kContentDir) / s.content_folder).generic_string().c_str(), "BT_New");
            s.registry->scan();
            if (const asset::AssetRecord* r = path.empty() ? nullptr : record_at(s, path)) {
                select(s, {});
                s.selected_asset = r->guid;
                start_rename(s, r->guid, "", r->name);
            }
        }
        ImGui::SetItemTooltip("AI decisions as a tree (Selector, Sequence, Move To, Wait, conditions...). Pick it on an AI Controller");
        if (ImGui::MenuItem("New Sound Cue")) {
            const std::string path = create_cue_file((std::filesystem::path(kContentDir) / s.content_folder).generic_string().c_str(), "SC_New");
            s.registry->scan();
            if (const asset::AssetRecord* r = path.empty() ? nullptr : record_at(s, path)) {
                select(s, {});
                s.selected_asset = r->guid;
                start_rename(s, r->guid, "", r->name);
            }
        }
        ImGui::SetItemTooltip("A node graph that combines sounds: Random, Modulator, Mixer, Delay, Looping, Switch... Double-click to edit");
        if (ImGui::MenuItem("New Lua Script")) create_script(s);
        ImGui::SetItemTooltip("A .lua file with on_start / on_update; add a Script component to an object and pick it");
        if (ImGui::MenuItem("New Input Setup (Move, Look, Jump)")) create_default_input(s);
        ImGui::SetItemTooltip("Creates IA_Move, IA_Look, IA_Jump and IMC_Default mapped to WASD, mouse, Space and a gamepad");
        if (ImGui::MenuItem("Show in Explorer")) show_in_explorer(s.content_folder);
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
        const bool enter = ImGui::InputText("Name", g_new_collection, sizeof g_new_collection, ImGuiInputTextFlags_EnterReturnsTrue);
        if ((ImGui::Button("Create") || enter) && g_new_collection[0]) {
            for (u32 k = 0; k < s.asset_selection_count; ++k) add_to_collection(s, g_new_collection, s.asset_selection[k]);
            std::snprintf(s.active_collection, sizeof s.active_collection, "%s", g_new_collection);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
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
            if (ImGui::MenuItem("Rename")) start_rename(s, {}, folder_path.c_str(), folders[f]);
            if (ImGui::MenuItem("Delete...")) {
                s.delete_asset = {};
                std::snprintf(s.delete_folder, sizeof s.delete_folder, "%s", folder_path.c_str());
                s.open_delete = true;
            }
            if (ImGui::MenuItem("Show in Explorer")) show_in_explorer(folder_path.c_str());
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
        const asset::AssetRecord& r = s.registry->get(hits[n]);
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
            if (level && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                char path[300];
                s.registry->source_path(r, path, sizeof path);
                request_open(s, path);
            }
            if (r.type == asset::AssetType::Script && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) open_external(s, r);
            if ((r.type == asset::AssetType::Graph || r.type == asset::AssetType::Cue || r.type == asset::AssetType::MaterialGraph || r.type == asset::AssetType::BehaviorTree) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                open_graph(s, r);
            }
            if (r.type == asset::AssetType::Table && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) open_table(s, r);
            if (r.type == asset::AssetType::Sound && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) toggle_preview(s, r);
            if (s.particle_editor && asset_matches(r, "ParticleSystem") && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) s.particle_editor->open(s, r);
            if ((r.type == asset::AssetType::UiDocument || r.type == asset::AssetType::UiStyle) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) open_ui_editor(s, r);
        }
        if (ImGui::BeginPopupContextItem("asset_menu")) {
            if (level && ImGui::MenuItem("Open Level")) {
                char path[300];
                s.registry->source_path(r, path, sizeof path);
                request_open(s, path);
            }
            if (r.type == asset::AssetType::Script && ImGui::MenuItem("Edit Script")) open_external(s, r);
            if (r.type == asset::AssetType::Graph && ImGui::MenuItem("Edit Visual Script")) open_graph(s, r);
            if (r.type == asset::AssetType::Table && ImGui::MenuItem("Edit Data Table")) open_table(s, r);
            if (s.particle_editor && asset_matches(r, "ParticleSystem") && ImGui::MenuItem("Open Particle Editor")) s.particle_editor->open(s, r);
            if ((r.type == asset::AssetType::Sound || r.type == asset::AssetType::Cue) && ImGui::MenuItem("Play / Stop")) toggle_preview(s, r);
            if (r.type == asset::AssetType::Cue && ImGui::MenuItem("Edit Sound Cue")) open_graph(s, r);
            if (r.type == asset::AssetType::MaterialGraph && ImGui::MenuItem("Edit Material Graph")) open_graph(s, r);
            if (r.type == asset::AssetType::BehaviorTree && ImGui::MenuItem("Edit Behavior Tree")) open_graph(s, r);
            if ((r.type == asset::AssetType::UiDocument || r.type == asset::AssetType::UiStyle) && ImGui::MenuItem("Edit UI")) open_ui_editor(s, r);
            if (r.type == asset::AssetType::Data && std::strcmp(r.data_type, "Material") == 0 && ImGui::MenuItem("Create Material Instance")) {
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
            if (r.type == asset::AssetType::Texture && ImGui::MenuItem("Create Material")) {  // like Unreal: a material using this texture
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
            if (r.type == asset::AssetType::Table && ImGui::MenuItem("Open in External Program")) open_external(s, r);
            if (!asset_selected(s, r.guid)) select_asset(s, r, false);  // right-click acts on what is selected
            if (ImGui::MenuItem("Reference Viewer...")) open_reference_viewer(s, r.guid);
            if (ImGui::MenuItem("Duplicate")) duplicate_asset(s, r);
            if ((r.type == asset::AssetType::Mesh || r.type == asset::AssetType::Texture || r.type == asset::AssetType::Sound) && ImGui::MenuItem("Reimport")) {
                for (u32 k = 0; k < s.asset_selection_count; ++k) {
                    if (const asset::AssetRecord* each = s.registry->find(s.asset_selection[k])) reimport_asset(s, *each);
                }
            }
            if (ImGui::BeginMenu("Add to Collection")) {
                const char* names[64];
                const u32 n_names = collection_names(s, names);
                for (u32 c = 0; c < n_names; ++c) {
                    if (ImGui::MenuItem(names[c])) {
                        for (u32 k = 0; k < s.asset_selection_count; ++k) add_to_collection(s, names[c], s.asset_selection[k]);
                    }
                }
                if (n_names) ImGui::Separator();
                if (ImGui::MenuItem("New Collection...")) {
                    std::snprintf(g_new_collection, sizeof g_new_collection, "Collection%u", n_names + 1);
                    g_open_new_collection = true;
                }
                ImGui::EndMenu();
            }
            if (s.active_collection[0] && ImGui::MenuItem("Remove from Collection")) {
                for (u32 k = 0; k < s.asset_selection_count; ++k) remove_from_collection(s, s.active_collection, s.asset_selection[k]);
            }
            source_control::asset_menu(r.path);
            if (ImGui::MenuItem("Rename", "F2")) start_rename(s, r.guid, "", r.name);
            if (ImGui::MenuItem(s.asset_selection_count > 1 ? "Delete Selected..." : "Delete...")) {
                s.delete_asset = r.guid;
                s.open_delete = true;
            }
            if (ImGui::MenuItem("Show in Explorer")) show_in_explorer(r.folder);
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
        } else if (r.type == asset::AssetType::Mesh && s.thumbnails) {
            thumbnail = s.thumbnails->get(r.guid);
        }
        if (thumbnail) {
            const bool flip = s.thumbnails->flipped();
            dl->AddImage(ImTextureRef(static_cast<ImTextureID>(thumbnail)), lo, hi, ImVec2(0, flip ? 1.0f : 0.0f), ImVec2(1, flip ? 0.0f : 1.0f));
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
        ImGui::SetItemTooltip("Drag into the viewport, or double-click to place in front of the camera");
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
    ImGui::Checkbox("Messages", &show_info);
    ImGui::SameLine();
    ImGui::Checkbox("Warnings", &show_warnings);
    ImGui::SameLine();
    ImGui::Checkbox("Errors", &show_errors);
    ImGui::SameLine();
    if (ImGui::Button("Clear")) clear_output_log();

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
    if (!open) { ImGui::End(); return; }

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
                        if (ImGui::MenuItem("Remove from Favourites")) toggle_favourite(s, path);
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

void draw_menu_bar(EditorState& s) {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Level", "Ctrl+N", false, !s.playing)) request_new(s);
        if (ImGui::BeginMenu("Open Level", !s.playing)) {
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
        ImGui::Separator();
        if (ImGui::MenuItem("Save Level", "Ctrl+S", false, !s.playing)) request_save(s, false);
        if (ImGui::MenuItem("Save Level As...", "Ctrl+Shift+S", false, !s.playing)) request_save(s, true);
        ImGui::Separator();
        if (ImGui::MenuItem("Validate Assets")) {
            const u32 n = validate_assets(s.project, *s.registry);
            std::snprintf(s.status, sizeof s.status, "Asset validation: %u problem%s (Message Log)", n, n == 1 ? "" : "s");
            if (n) s.show_message_log = true;
        }
        ImGui::SetItemTooltip("Missing references, oversize textures, name prefixes (Project Settings > Validation)");
        if (ImGui::MenuItem("Import...", nullptr, false, !s.playing)) s.import_requested = true;
        ImGui::SetItemTooltip("Pick files (several at once) to copy into the Content Browser's folder; dropping files or a whole folder on the editor works too");
        if (ImGui::MenuItem("Package Project", nullptr, false, !s.playing)) s.package_requested = true;
        ImGui::SetItemTooltip("Build/<Project Name>/: the game in a folder that runs on another PC (opens Project Settings > Game Default Level)");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, s.history->can_undo() && !s.playing)) s.history->undo(*s.world);
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, s.history->can_redo() && !s.playing)) s.history->redo(*s.world);
        ImGui::Separator();
        const bool has = s.world->alive(s.selected) && !s.playing;
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, has)) duplicate_selected(s);
        if (ImGui::MenuItem("Group", "Ctrl+G", false, has)) group_selection(s);
        if (ImGui::MenuItem("Ungroup", "Shift+G", false, has)) ungroup_selection(s);
        if (ImGui::MenuItem("Select All", "Ctrl+A", false, !s.playing)) {
            for (u32 slot = 0; slot < s.world->slot_count(); ++slot) add_to_selection(s, s.world->entity_at_slot(slot), false);
        }
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, has)) {
            copy_selection(s);
            delete_selected(s);
        }
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, has)) copy_selection(s);
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, !s.playing)) paste(s);
        if (ImGui::MenuItem("Delete", "Delete", false, has)) delete_selected(s);
        if (ImGui::MenuItem("Drop to Floor", "End", false, has)) drop_to_floor(s);
        ImGui::Separator();
        ImGui::MenuItem("Editor Preferences", nullptr, &s.show_preferences);
        ImGui::MenuItem("Project Settings", nullptr, &s.show_project_settings);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        if (ImGui::MenuItem("Smoke Test Level (5 s)", nullptr, false, !s.playing)) s.smoke_test_seconds = 5.0f;
        ImGui::SetItemTooltip("Plays this level for 5 seconds and reports any errors it logs (Unreal's automation smoke test)");
        if (ImGui::MenuItem("Run Unit Tests")) s.run_unit_tests = true;
        ImGui::SetItemTooltip("Runs the engine's tests (ctest) in the background; the result goes to the Output Log");
        ImGui::MenuItem("Remote Console", nullptr, &s.show_remote_console);
        ImGui::SetItemTooltip("Sends console commands to a game running on this PC with -remote");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Build")) {
        if (ImGui::MenuItem("Build Lighting", nullptr, false, !s.playing && s.build_lighting_progress < 0.0f)) s.build_lighting_requested = true;
        ImGui::SetItemTooltip("Bakes light probes (sky, and light bounced off the level) into <level>_BuiltData.lsdlight; save the level first");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
        ImGui::MenuItem("Undo History", nullptr, &s.show_history);
        ImGui::MenuItem("World Settings", nullptr, &s.show_world_settings);
        ImGui::MenuItem("Message Log", nullptr, &s.show_message_log);
        ImGui::MenuItem("Memory", nullptr, &s.show_memory);
        ImGui::EndMenu();
    }
    // Play / Stop, like Unreal's main toolbar.
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.5f - 40.0f);
    if (!s.playing) {
        ImGui::PushStyleColor(ImGuiCol_Text, vec4(s.theme.axis_y));
        if (ImGui::MenuItem("Play", "Alt+P")) s.play_requested = true;
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, vec4(s.theme.axis_x));
        if (ImGui::MenuItem("Stop", "Esc")) s.stop_requested = true;
        ImGui::PopStyleColor();
        if (ImGui::MenuItem(s.paused ? "Resume" : "Pause", "Pause")) s.paused = !s.paused;
        if (s.paused && ImGui::MenuItem("Step", "]")) s.step_requested = true;
        if (ImGui::MenuItem(s.ejected ? "Possess" : "Eject", "F8")) {
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
        if (ImGui::Button("Restore")) {
            s.restore_requested = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Ignore")) {
            s.recovery_path[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (s.quit_requested) ImGui::OpenPopup("Quit");
    s.quit_requested = false;
    if (ImGui::BeginPopupModal("Quit", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save changes to %s before closing?", s.level_path[0] ? s.level_path : "the untitled level");
        if (ImGui::Button("Save")) {
            s.quit_after_save = true;
            request_save(s, false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save")) {
            s.quit_confirmed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (g_open_confirm) ImGui::OpenPopup("Unsaved Changes");
    if (g_open_save_as) ImGui::OpenPopup("Save Level As");
    g_open_confirm = g_open_save_as = false;

    if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("The current level has unsaved changes. Discard them?");
        if (ImGui::Button("Discard")) {
            if (g_confirm_new) s.new_level_requested = true;
            else std::snprintf(s.open_level_requested, sizeof s.open_level_requested, "%s", g_confirm_open);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Save Level As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Saved to %s/<name>%s", kLevelsFolder, world::kLevelExtension);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##level_name", g_save_as_name, sizeof g_save_as_name, ImGuiInputTextFlags_EnterReturnsTrue);
        const bool ok = asset::valid_name(g_save_as_name);
        if (!ok) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Not a valid name");
        if ((ImGui::Button("Save") || enter) && ok) {
            std::snprintf(s.save_as_requested, sizeof s.save_as_requested, "%s/%s%s", kLevelsFolder, g_save_as_name, world::kLevelExtension);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
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

void draw_message_log(EditorState& s) {
    if (!s.show_message_log) return;
    ImGui::SetNextWindowSize(ImVec2(620, 300), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Message Log", &s.show_message_log)) {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        g_unread_errors = g_unread_warnings = 0;  // seen
        if (ImGui::Button("Clear")) g_messages_count = 0;
        ImGui::SameLine();
        ImGui::TextDisabled("Warnings and errors from imports, saves and the engine (all lines are in the Output Log)");
        ImGui::Separator();
        if (g_messages_count == 0) ImGui::TextDisabled("No messages.");
        const u32 first = (g_messages_next + kMessages - g_messages_count) % kMessages;
        for (u32 i = g_messages_count; i-- > 0;) {  // newest first
            const LogLine& m = g_messages[(first + i) % kMessages];
            const bool error = m.level >= log::Level::Error;
            ImGui::TextColored(error ? ImVec4(1.0f, 0.42f, 0.38f, 1) : ImVec4(1.0f, 0.82f, 0.35f, 1), "%s", error ? "Error" : "Warning");
            ImGui::SameLine(80.0f);
            ImGui::TextDisabled("[%s]", m.channel);
            ImGui::SameLine();
            ImGui::TextWrapped("%s", m.text);
        }
    }
    ImGui::End();
}

void draw_project_settings(EditorState& s) {
    if (!s.show_project_settings) return;
    ImGui::SetNextWindowSize(ImVec2(520, 300), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Project Settings", &s.show_project_settings)) {
        constexpr char kProjectPath[] = "Config/project.json";
        ImGui::TextDisabled("%s (shared with the team: commit it)", kProjectPath);
        if (ImGui::Button("Save")) std::snprintf(s.status, sizeof s.status, reflect::save_file(s.project, kProjectPath) ? "Saved %s" : "Could not save %s", kProjectPath);
        ImGui::SameLine();
        if (ImGui::Button("Use Current Level as Game Default") && s.level_path[0]) std::snprintf(s.project.game_default_level, sizeof s.project.game_default_level, "%s", s.level_path);
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

void draw_preferences_window(EditorState& s) {
    if (!s.show_preferences) return;
    EditorTheme& t = s.theme;
    ImGui::SetNextWindowSize(ImVec2(460, 600), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Editor Preferences", &s.show_preferences)) {
        const DetailsStyle style{{u32_color(t.details_row_a), u32_color(t.details_row_b)}};
        if (ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_DefaultOpen)) draw_details(reflect::type_of<EditorSettings>(), &s.settings, &style);
        if (ImGui::CollapsingHeader("Theme", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("%s", kThemePath);
            if (ImGui::Button("Save Theme")) std::snprintf(s.status, sizeof s.status, reflect::save_file(t, kThemePath) ? "Saved %s" : "Could not save %s", kThemePath);
            ImGui::SameLine();
            if (ImGui::Button("Reload")) {
                reflect::load_file(t, kThemePath);
                apply_theme(t);
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset")) {
                t = EditorTheme{};
                apply_theme(t);
            }
            if (draw_details(reflect::type_of<EditorTheme>(), &t, &style)) apply_theme(t);
        }
    }
    ImGui::End();
}

void handle_shortcuts(EditorState& s) {
    ImGuiIO& io = ImGui::GetIO();
    if (s.playing) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) s.stop_requested = true;
        if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F1)) s.mouse_captured = false;
        if (ImGui::IsKeyPressed(ImGuiKey_Pause)) s.paused = !s.paused;
        if (s.paused && ImGui::IsKeyPressed(ImGuiKey_RightBracket)) s.step_requested = true;
        if (ImGui::IsKeyPressed(ImGuiKey_F8)) {
            s.ejected = !s.ejected;
            s.mouse_captured = !s.ejected;
        }
        if (!s.ejected) return;
        if (!io.WantTextInput && s.world->alive(s.selected) && ImGui::IsKeyPressed(ImGuiKey_F)) focus_selected(s);
        return;
    }
    // Asset editor windows have their own Ctrl+S and Delete: level shortcuts stay out of them.
    if (const ImGuiWindow* focused = GImGui->NavWindow; focused && focused->RootWindow &&
        (std::strstr(focused->RootWindow->Name, "###VisualScript") || std::strstr(focused->RootWindow->Name, "###DataTable"))) {
        return;
    }
    if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_P)) s.play_requested = true;
    if (!io.KeyAlt && !io.KeyCtrl && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_P, false)) s.show_navigation = !s.show_navigation;
    if (io.KeyAlt && !io.WantTextInput) {  // Unreal's viewport keys
        if (ImGui::IsKeyPressed(ImGuiKey_G)) set_view(s, ViewType::Perspective);
        if (ImGui::IsKeyPressed(ImGuiKey_J)) set_view(s, ViewType::Top);
        if (ImGui::IsKeyPressed(ImGuiKey_H)) set_view(s, ViewType::Front);
        if (ImGui::IsKeyPressed(ImGuiKey_K)) set_view(s, ViewType::Right);
        if (ImGui::IsKeyPressed(ImGuiKey_4)) s.view_mode = ViewMode::Lit;
        if (ImGui::IsKeyPressed(ImGuiKey_3)) s.view_mode = ViewMode::Unlit;
        if (ImGui::IsKeyPressed(ImGuiKey_2)) s.view_mode = ViewMode::Wireframe;
        return;
    }
    if (io.WantTextInput) return;
    const bool ctrl = io.KeyCtrl;
    const bool flying = ImGui::IsMouseDown(ImGuiMouseButton_Right);  // WASD/QE move the camera then
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
        if (io.KeyShift) s.history->redo(*s.world);
        else s.history->undo(*s.world);
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) s.history->redo(*s.world);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S)) request_save(s, io.KeyShift);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N)) request_new(s);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_GraveAccent)) s.gizmo_local = !s.gizmo_local;
    const bool has = s.world->alive(s.selected);
    if (ctrl && has && ImGui::IsKeyPressed(ImGuiKey_C)) copy_selection(s);
    if (ctrl && has && ImGui::IsKeyPressed(ImGuiKey_X)) {
        copy_selection(s);
        delete_selected(s);
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V)) paste(s);
    if (ctrl && has && ImGui::IsKeyPressed(ImGuiKey_D)) duplicate_selected(s);
    if (!ctrl && s.playing && s.ejected && has && ImGui::IsKeyPressed(ImGuiKey_K)) keep_simulation_changes(s);
    if (ctrl && has && ImGui::IsKeyPressed(ImGuiKey_B)) {  // Find in Content Browser: the selected object's asset
        asset::Guid used;
        if (const auto* m = s.world->get<world::StaticMesh>(s.selected)) asset::parse_guid(m->mesh, used);
        if (const auto* m = s.world->get<world::SkeletalMesh>(s.selected); m && !used.valid()) asset::parse_guid(m->mesh, used);
        if (const auto* m = s.world->get<world::Script>(s.selected); m && !used.valid()) asset::parse_guid(m->script, used);
        if (used.valid()) find_in_content_browser(s, used);
    }
    if (ctrl && has && ImGui::IsKeyPressed(ImGuiKey_G)) group_selection(s);
    if (!ctrl && io.KeyShift && has && ImGui::IsKeyPressed(ImGuiKey_G)) {
        ungroup_selection(s);
        return;  // not also the G (game view) key below
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
        for (u32 slot = 0; slot < s.world->slot_count(); ++slot) add_to_selection(s, s.world->entity_at_slot(slot), false);
    }
    for (int n = 0; n < 10; ++n) {  // camera bookmarks
        if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + n)) || flying) continue;
        if (ctrl) {
            s.bookmarks[n] = s.camera;
            s.bookmark_set[n] = true;
            std::snprintf(s.status, sizeof s.status, "Camera bookmark %d set", n);
        } else if (s.bookmark_set[n]) {
            s.camera = s.bookmarks[n];
        }
    }
    if (ctrl || flying) return;
    if (ImGui::IsKeyPressed(ImGuiKey_W)) s.gizmo = GizmoMode::Translate;
    if (ImGui::IsKeyPressed(ImGuiKey_E)) s.gizmo = GizmoMode::Rotate;
    if (ImGui::IsKeyPressed(ImGuiKey_R)) s.gizmo = GizmoMode::Scale;
    if (ImGui::IsKeyPressed(ImGuiKey_G)) s.game_view = !s.game_view;
    if (has && ImGui::IsKeyPressed(ImGuiKey_F)) focus_selected(s);
    if (has && ImGui::IsKeyPressed(ImGuiKey_End)) drop_to_floor(s);
    if (has && ImGui::IsKeyPressed(ImGuiKey_Delete)) delete_selected(s);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) select(s, {});
}

} // namespace

void set_layout(EditorState& s, bool quad) { set_quad_view(s, quad); }

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
    const ImGuiViewport* main = ImGui::GetMainViewport();
    const ImGuiID dockspace = ImHashStr("EditorDockspace");
    const ImGuiDockNode* existing = ImGui::DockBuilderGetNode(dockspace);
    if (!existing || !existing->IsSplitNode()) build_default_layout(dockspace, main->WorkSize);
    ImGui::DockSpaceOverViewport(dockspace, main, ImGuiDockNodeFlags_PassthruCentralNode);

    draw_outliner(s);
    draw_details_panel(s);
    draw_content_browser(s);
    draw_history_window(s);
    draw_preferences_window(s);
    draw_world_settings_window(s);
    draw_message_log(s);
    draw_project_settings(s);
    draw_level_dialogs(s);
    draw_graph_editor(s);
    draw_table_editor(s);
    if (s.particle_editor) s.particle_editor->draw(s);
    source_control::draw();
    remote::draw_window(&s.show_remote_console);
    draw_reference_viewer(s);
    draw_memory_window(s);
    draw_ui_editor(s);
    handle_shortcuts(s);

    // The central node is left empty and transparent: the 3D viewport shows through it.
    if (const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspace)) {
        const bool quad = s.quad_view && !(s.playing && !s.ejected);  // the game plays in one big viewport
        s.viewport_count = quad ? 4 : 1;
        g_request_active = -1;
        if (!quad) {
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
            const ImVec2 half((central->Size.x - gap) * 0.5f, (central->Size.y - gap) * 0.5f);
            for (int i = 0; i < 4; ++i) {
                const ImVec2 at(central->Pos.x + (i % 2) * (half.x + gap), central->Pos.y + static_cast<f32>(i / 2) * (half.y + gap));
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
    // One undo step per finished edit: wait until drags and text fields are released.
    if (s.pending_edit[0] && (s.playing || (!ImGui::IsAnyItemActive() && !ImGuizmo::IsUsing() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)))) {
        if (!s.playing) s.history->commit(*s.world, s.pending_edit);  // Play changes are thrown away on Stop
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

} // namespace lsd::editor
