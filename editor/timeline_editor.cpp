#include "timeline_editor.h"
#include "keymap.h"
#include "tips.h"
#include "workspaces.h"

#include "timeline_player.h"

#include "lsd/asset/data_asset.h"
#include "lsd/foundation/log.h"
#include "lsd/world/components.h"
#include "lsd/world/timeline.h"

#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace lsd::editor {

namespace {

struct Original {  // an object as it was before the preview moved it
    std::string name;
    world::Transform transform;
    std::string property;
    f32 value = 0.0f;
    bool has_value = false;
    LinearColor color;
    bool has_color = false;
};

struct MovieRender {  // Movie Render Queue: one frame per editor frame
    bool active = false;
    u32 frame = 0, frames = 0;
    f32 fps = 30.0f;
    u32 size[2] = {1920, 1080};
    char folder[200] = "";
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
    bool camera_lock = false;  // the main view looks through the Camera Cut / Shot camera at the playhead
    bool curves = false;       // the curve view of the selected track
    u32 channel = 0;           // the curve view's channel
    i32 curve_drag = -1;       // a key dragged in the curve view
    f32 curve_range[2] = {0.0f, 1.0f};  // value range kept while dragging
    MovieRender movie;
    std::vector<Original> originals;
};
TimelineWindow g_tl;

const char* const kTypes = "Transform\0Property\0Event\0Sound\0Camera Cut\0Shot\0";
const char* const kTransformChannels[] = {"Location X", "Location Y", "Location Z", "Rotation X", "Rotation Y", "Rotation Z"};
const char* const kColorChannels[] = {"Red", "Green", "Blue", "Alpha"};

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
    o.has_color = get_color_property(*s.world, e, track.property, o.color);
    g_tl.originals.push_back(o);
}

void restore(EditorState& s) {
    for (const Original& o : g_tl.originals) {
        const world::Entity e = find_named(*s.world, o.name.c_str());
        if (!s.world->alive(e)) continue;
        *s.world->get<world::Transform>(e) = o.transform;
        if (o.has_value) set_number_property(*s.world, e, o.property.c_str(), o.value);
        if (o.has_color) set_color_property(*s.world, e, o.property.c_str(), o.color);
    }
    g_tl.originals.clear();
}

// Every object the timeline (and its shots' sub-sequences) moves, remembered first.
void remember_all(EditorState& s, const world::Timeline& t, u32 depth) {
    for (const world::TimelineTrack& track : t.tracks) remember(s, track);
    for (const world::TimelineKey& k : t.keys) {
        const world::Timeline* sub = depth < 4 && k.sequence[0] ? find_timeline(k.sequence) : nullptr;
        if (sub && sub != &t) remember_all(s, *sub, depth + 1);
    }
}

void preview(EditorState& s) {
    remember_all(s, *g_tl.data, 0);
    pose_timeline(*s.world, *g_tl.data, g_tl.playhead);
    const bool look = g_tl.camera_lock || g_tl.movie.active;
    s.cine_camera = look ? find_named(*s.world, timeline_camera(*g_tl.data, g_tl.playhead)) : world::Entity{};
}

void save(EditorState& s) {
    if (asset::save_data_asset(g_tl.path, reflect::type_of<world::Timeline>(), g_tl.data)) {
        g_tl.dirty = false;
        forget_timelines();  // sub-sequences load again
        std::snprintf(s.status, sizeof s.status, "%s: saved", g_tl.name);
    } else {
        std::snprintf(s.status, sizeof s.status, "Could not save %s", g_tl.path);
    }
}

void close_window(EditorState& s) {
    if (g_tl.dirty) save(s);
    restore(s);
    s.cine_camera = {};
    g_tl.movie.active = false;
    delete g_tl.data;
    g_tl.data = nullptr;
    g_tl.open = false;
}

bool is_color_track(EditorState& s, const world::TimelineTrack& track) {
    return track.type == static_cast<u32>(world::TrackType::Property) && is_color_property(*s.world, find_named(*s.world, track.object), track.property);
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
        get_color_property(*s.world, e, track.property, k.color);
    }
    for (u32 i = 0; i < g_tl.data->keys.count; ++i) {  // a key already there: replace it, keeping its text and curve
        world::TimelineKey& existing = g_tl.data->keys[i];
        if (existing.track == k.track && std::fabs(existing.time - k.time) < 1e-3f) {
            std::snprintf(k.text, sizeof k.text, "%s", existing.text);
            std::snprintf(k.sequence, sizeof k.sequence, "%s", existing.sequence);
            k.interp = existing.interp;
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

// Starts the Movie Render Queue on the open timeline (frames from 0, written by draw_timeline_editor).
void start_movie(f32 fps, u32 width, u32 height) {
    MovieRender& m = g_tl.movie;
    m.fps = fps >= 1.0f && fps <= 240.0f ? fps : 30.0f;
    m.size[0] = width >= 16 && width <= 8192 ? width : 1920;
    m.size[1] = height >= 16 && height <= 8192 ? height : 1080;
    m.frame = 0;
    m.frames = world::movie_frame_count(g_tl.data->length, m.fps);
    std::snprintf(m.folder, sizeof m.folder, "Saved/MovieRenders/%s", g_tl.name);
    std::error_code ignored;
    std::filesystem::create_directories(m.folder, ignored);
    m.active = true;
    g_tl.previewing = false;
    LSD_LOG_INFO("timeline", "Render Movie: %s, %u frames at %g fps, %u x %u, into %s", g_tl.name, m.frames, static_cast<double>(m.fps), m.size[0], m.size[1], m.folder);
}

// The selected track's channels as curves on the ruler's time axis: drag a key up / down to change its value.
bool draw_curves(EditorState& s, f32 left, f32 height) {
    world::Timeline& tl = *g_tl.data;
    if (g_tl.track < 0 || g_tl.track >= static_cast<i32>(tl.tracks.count)) return false;
    const u32 track = static_cast<u32>(g_tl.track);
    const auto type = static_cast<world::TrackType>(tl.tracks[track].type);
    if (type != world::TrackType::Transform && type != world::TrackType::Property) {
        ImGui::TextDisabled("Curves: select a Transform or Property track.");
        return false;
    }
    const bool color = is_color_track(s, tl.tracks[track]);
    const u32 first = type == world::TrackType::Transform ? 0u : color ? 7u : 6u;
    const u32 count = type == world::TrackType::Transform ? 6u : color ? 4u : 1u;
    if (g_tl.channel < first || g_tl.channel >= first + count) g_tl.channel = first;
    ImGui::BeginChild("curves", ImVec2(0, height), ImGuiChildFlags_Borders);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    for (u32 c = 0; c < count; ++c) {  // the channel to edit
        const char* label = type == world::TrackType::Transform ? kTransformChannels[c] : color ? kColorChannels[c] : "Value";
        if (ImGui::RadioButton(label, g_tl.channel == first + c)) g_tl.channel = first + c;
    }
    const f32 x0 = origin.x + left, w = ImGui::GetContentRegionAvail().x - left - 10.0f;
    const f32 y0 = origin.y, h = height - 12.0f;
    if (w < 10.0f || h < 10.0f) {
        ImGui::EndChild();
        return false;
    }
    const u32 ch = g_tl.channel;
    if (g_tl.curve_drag < 0) {  // fit the channel's keys (kept while dragging)
        bool any = false;
        for (const world::TimelineKey& k : tl.keys) {
            if (k.track != track) continue;
            const f32 v = world::key_channel(k, ch);
            g_tl.curve_range[0] = any ? std::fmin(g_tl.curve_range[0], v) : v;
            g_tl.curve_range[1] = any ? std::fmax(g_tl.curve_range[1], v) : v;
            any = true;
        }
        const f32 pad = (g_tl.curve_range[1] - g_tl.curve_range[0]) * 0.15f + 0.1f;
        g_tl.curve_range[0] -= pad;
        g_tl.curve_range[1] += pad;
    }
    const f32 lo = g_tl.curve_range[0], hi = g_tl.curve_range[1];
    auto to_x = [&](f32 t) { return x0 + t / tl.length * w; };
    auto to_y = [&](f32 v) { return y0 + h - (v - lo) / (hi - lo) * h; };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), IM_COL32(24, 24, 28, 255));
    char label[32];
    std::snprintf(label, sizeof label, "%.3g", static_cast<double>(hi));
    dl->AddText(ImVec2(x0 + 4, y0 + 2), IM_COL32(150, 150, 160, 255), label);
    std::snprintf(label, sizeof label, "%.3g", static_cast<double>(lo));
    dl->AddText(ImVec2(x0 + 4, y0 + h - 16), IM_COL32(150, 150, 160, 255), label);
    ImVec2 previous;
    for (int i = 0; i <= 200; ++i) {
        const f32 t = tl.length * static_cast<f32>(i) / 200.0f;
        f32 v = 0.0f;
        world::sample_channel(tl, track, ch, t, v);
        const ImVec2 p(to_x(t), to_y(v));
        if (i) dl->AddLine(previous, p, IM_COL32(240, 200, 60, 255), 2.0f);
        previous = p;
    }
    dl->AddLine(ImVec2(to_x(g_tl.playhead), y0), ImVec2(to_x(g_tl.playhead), y0 + h), IM_COL32(238, 167, 14, 255), 2.0f);
    const ImGuiIO& io = ImGui::GetIO();
    i32 hovered = -1;
    for (u32 i = 0; i < tl.keys.count; ++i) {
        if (tl.keys[i].track != track) continue;
        const ImVec2 p(to_x(tl.keys[i].time), to_y(*world::key_channel(tl.keys[i], ch)));
        const bool hot = (io.MousePos.x - p.x) * (io.MousePos.x - p.x) + (io.MousePos.y - p.y) * (io.MousePos.y - p.y) < 64.0f;
        if (hot) hovered = static_cast<i32>(i);
        dl->AddCircleFilled(p, hot ? 6.0f : 4.5f, static_cast<i32>(i) == g_tl.key ? IM_COL32(255, 160, 60, 255) : IM_COL32(255, 255, 255, 255));
    }
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    ImGui::InvisibleButton("##curve", ImVec2(w, h));
    if (ImGui::IsItemActivated() && hovered >= 0) g_tl.curve_drag = g_tl.key = hovered;
    if (ImGui::IsItemHovered() && hovered >= 0) {
        ImGui::SetTooltip("%.2f s  =  %.3f", static_cast<double>(tl.keys[static_cast<u32>(hovered)].time), static_cast<double>(*world::key_channel(tl.keys[static_cast<u32>(hovered)], ch)));
    }
    bool changed = false;
    if (g_tl.curve_drag >= 0 && g_tl.curve_drag < static_cast<i32>(tl.keys.count)) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            *world::key_channel(tl.keys[static_cast<u32>(g_tl.curve_drag)], ch) = lo + (y0 + h - io.MousePos.y) / h * (hi - lo);
            changed = true;
        } else {
            g_tl.curve_drag = -1;
        }
    }
    ImGui::EndChild();
    return changed;
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

void render_timeline_movie(EditorState& s, const asset::AssetRecord& record, f32 fps, u32 width, u32 height) {
    if (s.playing) {
        LSD_LOG_WARN("timeline", "Render Movie: stop Play first");
        return;
    }
    if (!g_tl.open || std::strcmp(g_tl.name, record.name) != 0) open_timeline_editor(s, record);
    if (g_tl.data) start_movie(fps, width, height);
}

void draw_timeline_editor(EditorState& s) {
    if (!g_tl.open || !g_tl.data) return;
    world::Timeline& tl = *g_tl.data;
    if (s.playing) {  // Play owns the level; the preview comes back after
        restore(s);
        s.cine_camera = {};
        g_tl.movie.active = false;
        return;
    }
    // Movie Render Queue: pose the next moment once main.cpp has rendered and saved the last one (it
    // waits while the level is not on screen, e.g. behind this editor's tab).
    if (g_tl.movie.active && !s.high_res_shot) {
        MovieRender& m = g_tl.movie;
        if (m.frame < m.frames) {
            g_tl.playhead = std::fmin(static_cast<f32>(m.frame) / m.fps, tl.length);
            preview(s);
            s.high_res_shot = 1;
            s.shot_size[0] = m.size[0];
            s.shot_size[1] = m.size[1];
            std::snprintf(s.shot_path, sizeof s.shot_path, "%s/%s_%04u.png", m.folder, g_tl.name, m.frame);
            ++m.frame;
            std::snprintf(s.status, sizeof s.status, "Rendering %s: frame %u / %u", g_tl.name, m.frame, m.frames);
        } else {
            m.active = false;
            preview(s);  // the camera lock as it was
            LSD_LOG_INFO("timeline", "Render Movie: %u frames written to %s", m.frames, m.folder);
            std::snprintf(s.status, sizeof s.status, "Movie rendered: %u frames in %s", m.frames, m.folder);
        }
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
    if (with_tip(ImGui::Button(g_tl.previewing ? "Pause" : "Preview"), "Plays the timeline in the level (looping) without Play")) g_tl.previewing = !g_tl.previewing;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    moved |= with_tip(ImGui::SliderFloat("##time", &g_tl.playhead, 0.0f, tl.length, "%.2f s"), "The playhead: the level shows the timeline at this moment");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    if (with_tip(ImGui::DragFloat("Length", &tl.length, 0.05f, 0.1f, 3600.0f, "%.1f s"), "How long the timeline plays")) g_tl.dirty = true;
    ImGui::SameLine();
    if (with_tip(ImGui::Button("Key"), "A key at the playhead on the selected track: the object's position / rotation / value / colour now")) {
        add_key(s);
        moved = true;
    }
    ImGui::SameLine();
    if (with_tip(ImGui::Button("+ Track"), "A Transform track for the object selected in the level (change its type on the left)")) {
        world::TimelineTrack t;
        if (s.world->alive(s.selected)) std::snprintf(t.object, sizeof t.object, "%s", s.world->name(s.selected));
        if (tl.tracks.push_back(t)) {
            g_tl.track = static_cast<i32>(tl.tracks.count - 1);
            g_tl.dirty = true;
        }
    }
    ImGui::SameLine();
    if (with_tip(ImGui::Button("Restore Level"), "Puts the objects the preview moved back where they were (closing the window does it too)")) restore(s);
    ImGui::SameLine();
    moved |= with_tip(ImGui::Checkbox("Camera Lock", &g_tl.camera_lock), "The level view looks through the Camera Cut / Shot camera at the playhead, with its Cine Camera lens and focus");
    ImGui::SameLine();
    with_tip(ImGui::Checkbox("Curves", &g_tl.curves), "Shows the selected track's values as curves: drag a key up or down to change it");
    ImGui::SameLine();
    if (with_tip(ImGui::Button(g_tl.movie.active ? "Rendering..." : "Render Movie"), "Movie Render Queue: renders the timeline frame by frame into PNG files (Saved/MovieRenders)")) {
        ImGui::OpenPopup("movie");
    }
    if (ImGui::BeginPopup("movie")) {
        MovieRender& m = g_tl.movie;
        ImGui::SetNextItemWidth(120);
        with_tip(ImGui::DragFloat("Frame Rate", &m.fps, 0.5f, 1.0f, 240.0f, "%.0f fps"), "Frames per second of the image sequence");
        ImGui::SetNextItemWidth(120);
        int size[2] = {static_cast<int>(m.size[0]), static_cast<int>(m.size[1])};
        if (with_tip(ImGui::DragInt2("Resolution", size, 4.0f, 16, 8192), "Width and height of each frame in pixels")) {
            m.size[0] = static_cast<u32>(size[0]);
            m.size[1] = static_cast<u32>(size[1]);
        }
        ImGui::BeginDisabled(m.active);
        if (with_tip(ImGui::Button("Render"), "Starts the render: the level view plays the timeline through its cameras and each frame is saved")) {
            if (g_tl.dirty) save(s);
            start_movie(m.fps, m.size[0], m.size[1]);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    // Tracks (left) and keys on the ruler (right).
    const f32 row = ImGui::GetFrameHeightWithSpacing() * 1.3f;
    const f32 left = 330.0f;
    const f32 curve_height = g_tl.curves ? 190.0f : 0.0f;
    ImGui::BeginChild("tracks", ImVec2(0, -110 - curve_height), ImGuiChildFlags_Borders);
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
        if (with_tip(ImGui::Combo("##type", &type, kTypes), "What the track animates")) {
            track.type = static_cast<u32>(type);
            g_tl.dirty = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(track.type == static_cast<u32>(world::TrackType::Property) ? 90.0f : 160.0f);
        if (with_tip(ImGui::InputTextWithHint("##object", "object", track.object, sizeof track.object), "The object's name in the level")) g_tl.dirty = true;
        if (ImGui::IsItemClicked()) g_tl.track = static_cast<i32>(i);
        if (track.type == static_cast<u32>(world::TrackType::Property)) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70);
            if (with_tip(ImGui::InputTextWithHint("##prop", "Comp.field", track.property, sizeof track.property), "Component.field: a number or a colour, e.g. PointLight.intensity or PointLight.color")) g_tl.dirty = true;
        }
        ImGui::SameLine();
        if (with_tip(ImGui::SmallButton("X"), "Removes this track and its keys")) remove_track = static_cast<i32>(i);
        // Its row on the ruler: keys as diamonds (shots as bars to the next shot); click to select, drag to move in time.
        const f32 y = at.y + row * 0.5f;
        dl->AddLine(ImVec2(ruler_x, y), ImVec2(ruler_x + ruler_w, y), IM_COL32(70, 70, 80, 255));
        if (track.type == static_cast<u32>(world::TrackType::Shot)) {
            u32 shown = 0;
            for (u32 k = 0; k < tl.keys.count; ++k) {
                const world::TimelineKey& key = tl.keys[k];
                if (key.track != i) continue;
                f32 end = tl.length;
                for (const world::TimelineKey& other : tl.keys) {
                    if (other.track == i && other.time > key.time && other.time < end) end = other.time;
                }
                const ImU32 fill = (shown++ & 1) ? IM_COL32(60, 110, 150, 200) : IM_COL32(80, 140, 110, 200);
                dl->AddRectFilled(ImVec2(time_to_x(key.time), y - 9), ImVec2(time_to_x(end), y + 9), fill, 3.0f);
                if (key.sequence[0]) dl->AddText(ImVec2(time_to_x(key.time) + 8, y + 1), IM_COL32(230, 230, 230, 255), key.sequence);
            }
        }
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
            if (key.interp == 2) dl->AddRectFilled(ImVec2(x - 5, y - 5), ImVec2(x + 5, y + 5), color);  // Constant: a square
            else if (key.interp == 1) dl->AddTriangleFilled(ImVec2(x - 6, y + 5), ImVec2(x + 6, y + 5), ImVec2(x, y - 6), color);  // Linear: a triangle
            else dl->AddQuadFilled(ImVec2(x, y - 6), ImVec2(x + 6, y), ImVec2(x, y + 6), ImVec2(x - 6, y), color);  // Auto: a diamond
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
    if (g_tl.curves && draw_curves(s, left, curve_height - 6.0f)) {
        g_tl.dirty = true;
        moved = true;
    }

    // The selected key.
    if (g_tl.key >= 0 && g_tl.key < static_cast<i32>(tl.keys.count)) {
        world::TimelineKey& k = tl.keys[static_cast<u32>(g_tl.key)];
        const auto type = k.track < tl.tracks.count ? static_cast<world::TrackType>(tl.tracks[k.track].type) : world::TrackType::Transform;
        bool changed = with_tip(ImGui::DragFloat("Time", &k.time, 0.01f, 0.0f, tl.length, "%.2f s"), "When the key is");
        if (type == world::TrackType::Transform || type == world::TrackType::Property) {
            ImGui::SameLine();
            int interp = static_cast<int>(k.interp);
            ImGui::SetNextItemWidth(110);
            if (with_tip(ImGui::Combo("Interpolation", &interp, "Auto\0Linear\0Constant\0"), "From this key to the next: Auto (smooth cubic, automatic tangents), Linear, or Constant (a step)")) {
                k.interp = static_cast<u32>(interp);
                changed = true;
            }
        }
        if (type == world::TrackType::Transform) {
            changed |= with_tip(ImGui::DragFloat3("Position", &k.position.x, 0.05f), "Where the object is at this key");
            changed |= with_tip(ImGui::DragFloat3("Rotation", &k.rotation.x, 0.5f), "How it is turned at this key (degrees)");
        } else if (type == world::TrackType::Property && is_color_track(s, tl.tracks[k.track])) {
            changed |= with_tip(ImGui::ColorEdit4("Color", &k.color.r, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR), "The colour at this key");
        } else if (type == world::TrackType::Property) {
            changed |= with_tip(ImGui::DragFloat("Value", &k.value, 0.05f), "The value at this key");
        } else {
            const char* hint = type == world::TrackType::Event ? "function to call (Custom Event name)" : type == world::TrackType::Sound ? "sound name" : "Camera object name";
            changed |= with_tip(ImGui::InputTextWithHint("Text", hint, k.text, sizeof k.text), "Event: the function to call; Sound: the sound; Camera Cut and Shot: the camera object");
            if (type == world::TrackType::Shot) {
                changed |= with_tip(ImGui::InputTextWithHint("Sub-Sequence", "Timeline name (optional)", k.sequence, sizeof k.sequence),
                                    "Another Timeline played as this shot, from its start when the shot starts (its camera cuts are used when the shot has no camera)");
            }
        }
        if (with_tip(ImGui::Button("Delete Key"), "Removes the selected key") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && command_pressed(Command::Delete))) {
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
    if (moved && !g_tl.movie.active) preview(s);
    if (!g_tl.camera_lock && !g_tl.movie.active) s.cine_camera = {};
}

} // namespace lsd::editor
