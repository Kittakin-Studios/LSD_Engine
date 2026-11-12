#include "editor_utility.h"

#include "editor_ui.h"
#include "editor_utility_core.h"
#include "graph_editor.h"
#include "mesh_cache.h"
#include "play_session.h"
#include "tips.h"

#include "lsd/core/console.h"
#include "lsd/foundation/log.h"
#include "lsd/world/components.h"
#include "lsd/world/prefab.h"

#include <cstdio>
#include <cstring>

namespace lsd::editor {

namespace {

EditorUtilities& g_utilities = *new EditorUtilities;  // never destroyed: its Lua state may outlive the statics it reports to at exit

const asset::AssetRecord* find_named(const asset::AssetRegistry& registry, asset::AssetType type, const char* name) {
    return registry.find_named(name, type);
}

// lsd.spawn in the editor: a prefab or mesh asset by name, or a built-in shape ("Cube"); not selected.
world::Entity spawn(void* user, const char* name, Vec3 position) {
    EditorState& s = *static_cast<EditorState*>(user);
    const asset::AssetRecord* prefab = find_named(*s.registry, asset::AssetType::Prefab, name);
    const asset::AssetRecord* mesh = prefab ? nullptr : find_named(*s.registry, asset::AssetType::Mesh, name);
    char key[40];
    if (mesh) asset::to_string(mesh->guid, key);
    else std::snprintf(key, sizeof key, "%s", name);
    if (!prefab && !s.meshes->get(key)) {
        LSD_LOG_WARN("editor", "Editor utility spawn: no prefab, mesh or shape named '%s'", name);
        return {};
    }
    char unique[64];
    s.world->unique_name(name, unique, sizeof unique);
    const world::Entity e = s.world->create(unique);
    s.world->get<world::Transform>(e)->position = position;
    if (prefab) {
        asset::to_string(prefab->guid, s.world->add<world::PrefabInstance>(e)->prefab);
        char path[300];
        s.registry->source_path(*prefab, path, sizeof path);
        world::update_instance_from_file(*s.world, e, path, true);
    } else {
        std::snprintf(s.world->add<world::StaticMesh>(e)->mesh, sizeof(world::StaticMesh::mesh), "%s", key);
    }
    return e;
}

void ensure_started(EditorState& s) {
    if (g_utilities.started()) return;
    UtilityHooks hooks;
    hooks.user = &s;
    hooks.selection = [](void* user, world::Entity* out, u32 capacity) {
        const EditorState& st = *static_cast<const EditorState*>(user);
        u32 n = 0;
        for (; n < st.selection_count && n < capacity; ++n) out[n] = st.selection[n];
        return n;
    };
    hooks.select = [](void* user, const world::Entity* list, u32 count) {
        EditorState& st = *static_cast<EditorState*>(user);
        st.selection_count = 0;
        for (u32 i = 0; i < count && i < EditorState::kMaxSelection; ++i) st.selection[st.selection_count++] = list[i];
        st.selected = st.selection_count ? st.selection[st.selection_count - 1] : world::Entity{};
        if (st.selection_count) st.selected_asset = {};
    };
    hooks.open_asset = [](void* user, const char* name) {
        EditorState& st = *static_cast<EditorState*>(user);
        for (usize i = 0; i < st.registry->count(); ++i) {
            if (std::strcmp(st.registry->get(i).name, name) == 0) return open_asset(st, st.registry->get(i));
        }
        return false;
    };
    hooks.console = [](void*, const char* line) { console::execute(line); };
    hooks.spawn = spawn;
    hooks.assets = [](void* user, const char* type, const char* folder, char* out, usize capacity) {
        return list_assets(*static_cast<const EditorState*>(user)->registry, type, folder, out, capacity);
    };
    g_utilities.start(*s.world, hooks);
}

void after_run(EditorState& s, bool changed, const char* label) {
    if (changed) std::snprintf(s.pending_edit, sizeof s.pending_edit, "%s", label);  // one undo step
}

} // namespace

bool can_run_utility(const asset::AssetRecord& record) {
    return record.type == asset::AssetType::Script || record.type == asset::AssetType::Graph;
}

bool is_editor_utility(const asset::AssetRecord& record) { return can_run_utility(record) && std::strncmp(record.name, "EU_", 3) == 0; }

bool run_editor_utility(EditorState& s, const asset::AssetRecord& record) {
    if (s.playing || s.game_mode) {
        std::snprintf(s.status, sizeof s.status, "Editor utilities run on the edited level: stop Play first");
        return false;
    }
    char path[300];
    if (record.type == asset::AssetType::Graph) {
        if (!graph_lua(*s.registry, record, path, sizeof path)) return false;  // the compile error is logged
    } else if (record.type == asset::AssetType::Script) {
        s.registry->source_path(record, path, sizeof path);
    } else {
        return false;
    }
    ensure_started(s);
    char label[64];
    std::snprintf(label, sizeof label, "Run %s", record.name);
    after_run(s, g_utilities.run_file(path), label);
    std::snprintf(s.status, sizeof s.status, "Ran editor utility %s", record.name);
    return true;
}

void draw_utility_menu_items(EditorState& s) {
    u32 shown = 0;
    for (usize i = 0; i < s.registry->count(); ++i) {
        const asset::AssetRecord& r = s.registry->get(i);
        if (!is_editor_utility(r)) continue;
        ++shown;
        if (with_tip(ImGui::MenuItem(r.name, nullptr, false, !s.playing), "Runs this editor utility script on the level (one undo step)")) run_editor_utility(s, r);
    }
    if (!shown) ImGui::TextDisabled("No EU_ scripts yet (a Lua or visual script named EU_...)");
}

void draw_utility_panels(EditorState& s) {
    std::vector<UtilityPanel>& panels = g_utilities.panels();
    for (u32 p = 0; p < panels.size(); ++p) {
        if (!panels[p].open) continue;
        char title[128];
        std::snprintf(title, sizeof title, "%s###utility_%s", panels[p].title, panels[p].script.c_str());
        ImGui::SetNextWindowSize(ImVec2(320.0f, 240.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(title, &panels[p].open)) {
            ImGui::End();
            continue;
        }
        ImGui::BeginDisabled(s.playing);  // they edit the level, not the game
        for (u32 i = 0; i < panels[p].widgets.size(); ++i) {
            UtilityWidget& w = panels[p].widgets[i];
            ImGui::PushID(static_cast<int>(i));
            switch (w.kind) {
                case UtilityWidget::Kind::Label: ImGui::TextWrapped("%s", w.label); break;
                case UtilityWidget::Kind::Button:
                    if (with_tip(ImGui::Button(w.label), "Calls the utility script's function of this name (one undo step)")) {
                        char label[64];
                        std::snprintf(label, sizeof label, "%s", w.label);  // press may rebuild the panel
                        after_run(s, g_utilities.press(p, i), label);
                    }
                    break;
                case UtilityWidget::Kind::Checkbox:
                    if (with_tip(ImGui::Checkbox(w.label, &w.on), "Sets the utility script's variable of this name")) g_utilities.push_value(p, i);
                    break;
                case UtilityWidget::Kind::Number:
                    if (with_tip(ImGui::InputDouble(w.label, &w.number, 0.0, 0.0, "%.3f"), "Sets the utility script's variable of this name")) g_utilities.push_value(p, i);
                    break;
                case UtilityWidget::Kind::Text:
                    if (with_tip(ImGui::InputText(w.label, w.text, sizeof w.text), "Sets the utility script's variable of this name")) g_utilities.push_value(p, i);
                    break;
            }
            ImGui::PopID();
            if (p >= panels.size() || i >= panels[p].widgets.size()) break;  // a button rebuilt or dropped the panel
        }
        ImGui::EndDisabled();
        ImGui::End();
    }
}

void register_utility_commands(EditorState& s) {
    console::add_command("utility.Run", [](void* user, const char* args) {
        EditorState& st = *static_cast<EditorState*>(user);
        for (usize i = 0; i < st.registry->count(); ++i) {
            const asset::AssetRecord& r = st.registry->get(i);
            if (can_run_utility(r) && std::strcmp(r.name, args) == 0) {
                run_editor_utility(st, r);
                return;
            }
        }
        LSD_LOG_WARN("editor", "utility.Run: no Lua or visual script named '%s'", args);
    }, &s, "Run an editor utility script on the edited level, e.g. utility.Run EU_AlignToGrid");
}

} // namespace lsd::editor
