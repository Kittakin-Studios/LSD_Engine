#include "timeline_editor.h"
#include "tips.h"
#include "workspaces.h"

#include "timeline_player.h"

#include "lsd/asset/data_asset.h"
#include "lsd/world/components.h"
#include "lsd/world/timeline.h"

#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace lsd::editor {

namespace {

struct Original {  // an object as it was before the preview moved it
    std::string name;
    world::Transform transform;
    std::string property;
    f32 value = 0.0f;
    bool has_value = false;
};

struct TimelineWindow {
    bool open = false;
    bool dirty = false;
    char name[96] = "";
    char path[300] = "";
    world::Timeline* data = nullptr;
    f32 playhead = 0.0f;
    bool previewing = false;  // the playhead runs on its own
    i32 track = -1;           // selected
    i32 key = -1;             // selected (index into keys)
    bool dragging_key = false;
    std::vector<Original> originals;
};
TimelineWindow g_tl;

const char* const kTypes = "Transform\0Property\0Event\0Sound\0Camera Cut\0";

// Remembers an object before the preview first changes it.
void remember(EditorState& s, const world::TimelineTrack& track) {
    const world::Entity e = find_named(*s.world, track.object);
    if (!s.world->alive(e)) return;
    for (const Original& o : g_tl.originals) {
        if (o.name == track.object && o.property == track.property) return;
    }
    Original o;
    o.name = track.object;
    o.transform = *s.world->get<world::Transform>(e);
    o.property = track.property;
    o.has_value = get_number_property(*s.world, e, track.property, o.value);
    g_tl.originals.push_back(o);
}

void restore(EditorState& s) {
    for (const Original& o : g_tl.originals) {
        const world::Entity e = find_named(*s.world, o.name.c_str());
        if (!s.world->alive(e)) continue;
        *s.world->get<world::Transform>(e) = o.transform;
        if (o.has_value) set_number_property(*s.world, e, o.property.c_str(), o.value);
    }
    g_tl.originals.clear();
}

void preview(EditorState& s) {
    for (const world::TimelineTrack& t : g_tl.data->tracks) remember(s, t);
    pose_timeline(*s.world, *g_tl.data, g_tl.playhead);
}

void save(EditorState& s) {
    if (asset::save_data_asset(g_tl.path, reflect::type_of<world::Timeline>(), g_tl.data)) {
        g_tl.dirty = false;
        std::snprintf(s.status, sizeof s.status, "%s: saved", g_tl.name);
    } else {
        std::snprintf(s.status, sizeof s.status, "Could not save %s", g_tl.path);
    }
}

void close_window(EditorState& s) {
    if (g_tl.dirty) save(s);
    restore(s);
    delete g_tl.data;
    g_tl.data = nullptr;
    g_tl.open = false;
}

// A key at the playhead on the selected track, from its object as it is now.
void add_key(EditorState& s) {
    if (g_tl.track < 0 || g_tl.track >= static_cast<i32>(g_tl.data->tracks.count)) return;
    const world::TimelineTrack& track = g_tl.data->tracks[static_cast<u32>(g_tl.track)];
    world::TimelineKey k;
    k.track = static_cast<u32>(g_tl.track);
    k.time = g_tl.playhead;
    const world::Entity e = find_named(*s.world, track.object);
    if (s.world->alive(e)) {
        const auto* t = s.world->get<world::Transform>(e);
        k.position = t->position;
        k.rotation = t->rotation;
        get_number_property(*s.world, e, track.property, k.value);
    }
    for (u32 i = 0; i < g_tl.data->keys.count; ++i) {  // a key already there: replace it
        world::TimelineKey& existing = g_tl.data->keys[i];
        if (existing.track == k.track && std::fabs(existing.time - k.time) < 1e-3f) {
            std::snprintf(k.text, sizeof k.text, "%s", existing.text);
            existing = k;
            g_tl.key = static_cast<i32>(i);
            g_tl.dirty = true;
            return;
        }
    }
    if (g_tl.data->keys.push_back(k)) {
        g_tl.key = static_cast<i32>(g_tl.data->keys.count - 1);
        g_tl.dirty = true;
    }
}

} // namespace

void open_timeline_editor(EditorState& s, const asset::AssetRecord& record) {
    if (g_tl.open) close_window(s);
    s.registry->source_path(record, g_tl.path, sizeof g_tl.path);
    g_tl.data = new world::Timeline();
    if (!asset::load_data_asset(g_tl.path, *g_tl.data)) {
        delete g_tl.data;
        g_tl.data = nullptr;
        std::snprintf(s.status, sizeof s.status, "Could not open %s", record.name);
        return;
    }
    std::snprintf(g_tl.name, sizeof g_tl.name, "%s", record.name);
    g_tl.open = true;
    g_tl.dirty = false;
    g_tl.playhead = 0.0f;
    g_tl.previewing = false;
    g_tl.track = g_tl.data->tracks.count ? 0 : -1;
    g_tl.key = -1;
    g_tl.originals.clear();
    ImGui::SetWindowFocus("###Timeline");
}

void draw_timeline_editor(EditorState& s) {
    if (!g_tl.open || !g_tl.data) return;
    world::Timeline& tl = *g_tl.data;
    if (s.playing) {  // Play owns the level; the preview comes back after
        restore(s);
        return;
    }
    char title[160];
    std::snprintf(title, sizeof title, "%s%s - Timeline###Timeline", g_tl.name, g_tl.dirty ? "*" : "");
    ImGui::SetNextWindowSize(ImVec2(1000, 340), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!begin_editor(title, &open)) return;  // a panel tab, or the workspace's editor area
    if (!open) {
        ImGui::End();
        close_window(s);
        return;
    }
    bool moved = false;  // the playhead or the keys changed: preview again
    if (g_tl.previewing) {
        g_tl.playhead += ImGui::GetIO().DeltaTime;
        if (g_tl.playhead > tl.length) g_tl.playhead = 0.0f;
        moved = true;
    }
    // Toolbar
    if (with_tip(ImGui::Button("Save"), "Writes the timeline to disk")) save(s);
    ImGui::SameLine();
    if (ImGui::Button(g_tl.previewing ? "Pause" : "Preview")) g_tl.previewing = !g_tl.previewing;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    moved |= ImGui::SliderFloat("##time", &g_tl.playhead, 0.0f, tl.length, "%.2f s");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    if (with_tip(ImGui::DragFloat("Length", &tl.length, 0.05f, 0.1f, 3600.0f, "%.1f s"), "How long the timeline plays")) g_tl.dirty = true;
    ImGui::SameLine();
    if (with_tip(ImGui::Button("Key"), "Records the selected track's object as it is now, at the playhead")) {
        add_key(s);
        moved = true;
    }
    ImGui::SetItemTooltip("A key at the playhead on the selected track: the object's position / rotation / value now");
    ImGui::SameLine();
    if (with_tip(ImGui::Button("+ Track"), "Adds a track for the object selected in the level")) {
        world::TimelineTrack t;
        if (s.world->alive(s.selected)) std::snprintf(t.object, sizeof t.object, "%s", s.world->name(s.selected));
        if (tl.tracks.push_back(t)) {
            g_tl.track = static_cast<i32>(tl.tracks.count - 1);
            g_tl.dirty = true;
        }
    }
    ImGui::SetItemTooltip("A Transform track for the selected object (change its type on the left)");
    ImGui::SameLine();
    if (ImGui::Button("Restore Level")) restore(s);
    ImGui::SetItemTooltip("Puts the objects the preview moved back where they were (closing the window does it too)");

    // Tracks (left) and keys on the ruler (right).
    const f32 row = ImGui::GetFrameHeightWithSpacing() * 1.3f;
    const f32 left = 330.0f;
    ImGui::BeginChild("tracks", ImVec2(0, -110), ImGuiChildFlags_Borders);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const f32 ruler_x = origin.x + left, ruler_w = ImGui::GetContentRegionAvail().x - left - 10.0f;
    auto time_to_x = [&](f32 t) { return ruler_x + t / tl.length * ruler_w; };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Ruler: a tick per second (or per 10 for long timelines), the playhead; click / drag to scrub.
    const f32 step = tl.length > 60.0f ? 10.0f : 1.0f;
    for (f32 t = 0.0f; t <= tl.length + 1e-3f; t += step) {
        dl->AddLine(ImVec2(time_to_x(t), origin.y), ImVec2(time_to_x(t), origin.y + 8), IM_COL32(180, 180, 180, 255));
        char label[16];
        std::snprintf(label, sizeof label, "%g", static_cast<double>(t));
        dl->AddText(ImVec2(time_to_x(t) + 2, origin.y), IM_COL32(180, 180, 180, 255), label);
    }
    ImGui::SetCursorScreenPos(ImVec2(ruler_x, origin.y));
    ImGui::InvisibleButton("##ruler", ImVec2(ruler_w > 1 ? ruler_w : 1, row * 0.8f));
    if (ImGui::IsItemActive()) {
        g_tl.playhead = std::fmin(std::fmax((ImGui::GetMousePos().x - ruler_x) / ruler_w * tl.length, 0.0f), tl.length);
        moved = true;
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + row * 0.8f));
    i32 remove_track = -1;
    for (u32 i = 0; i < tl.tracks.count; ++i) {
        world::TimelineTrack& track = tl.tracks[i];
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (static_cast<i32>(i) == g_tl.track) dl->AddRectFilled(at, ImVec2(at.x + left - 8, at.y + row), IM_COL32(89, 108, 193, 90));
        int type = static_cast<int>(track.type);
        ImGui::SetNextItemWidth(95);
        if (ImGui::Combo("##type", &type, kTypes)) {
            track.type = static_cast<u32>(type);
            g_tl.dirty = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(track.type == static_cast<u32>(world::TrackType::Property) ? 90.0f : 160.0f);
        if (ImGui::InputTextWithHint("##object", "object", track.object, sizeof track.object)) g_tl.dirty = true;
        if (ImGui::IsItemClicked()) g_tl.track = static_cast<i32>(i);
        if (track.type == static_cast<u32>(world::TrackType::Property)) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70);
            if (ImGui::InputTextWithHint("##prop", "Comp.field", track.property, sizeof track.property)) g_tl.dirty = true;
        }
        ImGui::SameLine();
        if (with_tip(ImGui::SmallButton("X"), "Removes this track and its keys")) remove_track = static_cast<i32>(i);
        // Its row on the ruler: keys as diamonds; click to select, drag to move in time.
        const f32 y = at.y + row * 0.5f;
        dl->AddLine(ImVec2(ruler_x, y), ImVec2(ruler_x + ruler_w, y), IM_COL32(70, 70, 80, 255));
        ImGui::SetCursorScreenPos(ImVec2(ruler_x, at.y));
        ImGui::InvisibleButton("##row", ImVec2(ruler_w > 1 ? ruler_w : 1, row));
        if (ImGui::IsItemClicked()) {
            g_tl.track = static_cast<i32>(i);
            g_tl.key = -1;
            for (u32 k = 0; k < tl.keys.count; ++k) {
                if (tl.keys[k].track == i && std::fabs(time_to_x(tl.keys[k].time) - ImGui::GetMousePos().x) < 7.0f) g_tl.key = static_cast<i32>(k);
            }
            g_tl.dragging_key = g_tl.key >= 0;
            if (g_tl.key < 0) {  // empty space: scrub there
                g_tl.playhead = std::fmin(std::fmax((ImGui::GetMousePos().x - ruler_x) / ruler_w * tl.length, 0.0f), tl.length);
                moved = true;
            }
        }
        for (u32 k = 0; k < tl.keys.count; ++k) {
            const world::TimelineKey& key = tl.keys[k];
            if (key.track != i) continue;
            const f32 x = time_to_x(key.time);
            const ImU32 color = static_cast<i32>(k) == g_tl.key ? IM_COL32(243, 195, 18, 255) : IM_COL32(220, 220, 230, 255);
            dl->AddQuadFilled(ImVec2(x, y - 6), ImVec2(x + 6, y), ImVec2(x, y + 6), ImVec2(x - 6, y), color);
            if (key.text[0]) dl->AddText(ImVec2(x + 8, y - 8), IM_COL32(200, 200, 200, 255), key.text);
        }
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + row));
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0.0f, 0.0f));  // an item where the rows end (ImGui sizes the child from it)
    if (g_tl.dragging_key && g_tl.key >= 0 && g_tl.key < static_cast<i32>(tl.keys.count)) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            f32 t = std::fmin(std::fmax((ImGui::GetMousePos().x - ruler_x) / ruler_w * tl.length, 0.0f), tl.length);
            if (!ImGui::GetIO().KeyCtrl) t = std::round(t * 30.0f) / 30.0f;  // whole frames at 30 fps (Ctrl: free)
            if (t != tl.keys[static_cast<u32>(g_tl.key)].time) {
                tl.keys[static_cast<u32>(g_tl.key)].time = t;
                g_tl.dirty = true;
                moved = true;
            }
        } else {
            g_tl.dragging_key = false;
        }
    }
    const f32 px = time_to_x(g_tl.playhead);
    dl->AddLine(ImVec2(px, origin.y), ImVec2(px, origin.y + row * 0.8f + row * static_cast<f32>(tl.tracks.count)), IM_COL32(238, 167, 14, 255), 2.0f);
    if (remove_track >= 0) {  // its keys go too; later tracks' keys move up one
        for (u32 k = tl.keys.count; k-- > 0;) {
            if (tl.keys[k].track == static_cast<u32>(remove_track)) tl.keys.remove(k);
            else if (tl.keys[k].track > static_cast<u32>(remove_track)) --tl.keys[k].track;
        }
        tl.tracks.remove(static_cast<u32>(remove_track));
        g_tl.track = -1;
        g_tl.key = -1;
        g_tl.dirty = true;
    }
    if (tl.tracks.count == 0) ImGui::TextDisabled("Select an object in the level and press + Track; scrub, move it, press Key.");
    ImGui::EndChild();

    // The selected key.
    if (g_tl.key >= 0 && g_tl.key < static_cast<i32>(tl.keys.count)) {
        world::TimelineKey& k = tl.keys[static_cast<u32>(g_tl.key)];
        const auto type = k.track < tl.tracks.count ? static_cast<world::TrackType>(tl.tracks[k.track].type) : world::TrackType::Transform;
        bool changed = with_tip(ImGui::DragFloat("Time", &k.time, 0.01f, 0.0f, tl.length, "%.2f s"), "When the key is");
        if (type == world::TrackType::Transform) {
            changed |= ImGui::DragFloat3("Position", &k.position.x, 0.05f);
            changed |= ImGui::DragFloat3("Rotation", &k.rotation.x, 0.5f);
        } else if (type == world::TrackType::Property) {
            changed |= with_tip(ImGui::DragFloat("Value", &k.value, 0.05f), "The value at this key");
        } else {
            const char* hint = type == world::TrackType::Event ? "function to call (Custom Event name)" : type == world::TrackType::Sound ? "sound name" : "Camera object name";
            changed |= ImGui::InputTextWithHint("Text", hint, k.text, sizeof k.text);
        }
        if (with_tip(ImGui::Button("Delete Key"), "Removes the selected key") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Delete))) {
            tl.keys.remove(static_cast<u32>(g_tl.key));
            g_tl.key = -1;
            changed = true;
        }
        if (changed) {
            g_tl.dirty = true;
            moved = true;
        }
    } else {
        ImGui::TextDisabled("Click a key to edit it; drag it to move it in time (snaps to 1/30 s, Ctrl for free).");
    }
    ImGui::End();
    if (moved) preview(s);
}

} // namespace lsd::editor
