#include "flipbook_editor.h"

#include "material_cache.h"
#include "paper2d_core.h"
#include "tips.h"
#include "workspaces.h"
#include "lsd/asset/data_asset.h"
#include "lsd/asset/guid.h"
#include "lsd/reflect/reflect.h"
#include "lsd/rhi/rhi.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>

namespace lsd::editor {

namespace {

struct Window {
    bool open = false, dirty = false, playing = true;
    world::Flipbook book;
    char path[300] = "";
    char name[96] = "";
    i32 selected = -1;  // the frame being edited
    f64 started = 0.0;  // ImGui time the preview started at
};
Window g_fb;

void save(EditorState& s) {
    if (asset::save_data_asset(g_fb.path, reflect::type_of<world::Flipbook>(), &g_fb.book)) {
        g_fb.dirty = false;
        std::snprintf(s.status, sizeof s.status, "%s: saved", g_fb.name);
    } else {
        std::snprintf(s.status, sizeof s.status, "Could not save %s", g_fb.path);
    }
}

// The UVs of sheet cell `cell` (left to right, then down).
void cell_uv(u32 cell, ImVec2& lo, ImVec2& hi) {
    const u32 cols = g_fb.book.preview_columns ? g_fb.book.preview_columns : 1, rows = g_fb.book.preview_rows ? g_fb.book.preview_rows : 1;
    const f32 x = static_cast<f32>(cell % cols), y = static_cast<f32>((cell / cols) % rows);
    lo = ImVec2(x / static_cast<f32>(cols), y / static_cast<f32>(rows));
    hi = ImVec2((x + 1.0f) / static_cast<f32>(cols), (y + 1.0f) / static_cast<f32>(rows));
}

// The preview sheet: a combo of the project's textures.
void sheet_picker(EditorState& s) {
    const char* current = "(none)";
    asset::Guid guid;
    if (const asset::AssetRecord* r = g_fb.book.preview_texture[0] && asset::parse_guid(g_fb.book.preview_texture, guid) ? s.registry->find(guid) : nullptr) current = r->name;
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo("Sheet", current)) {
        for (usize i = 0; i < s.registry->count(); ++i) {
            const asset::AssetRecord& r = s.registry->get(i);
            if (r.type != asset::AssetType::Texture) continue;
            char text[33];
            asset::to_string(r.guid, text);
            if (ImGui::Selectable(r.name, std::strcmp(text, g_fb.book.preview_texture) == 0)) {
                std::snprintf(g_fb.book.preview_texture, sizeof g_fb.book.preview_texture, "%s", text);
                g_fb.dirty = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("The sprite sheet the frames' cells are shown from (only for this window: sprites use their own sheet)");
    ImGui::SameLine();
    int cols = static_cast<int>(g_fb.book.preview_columns), rows = static_cast<int>(g_fb.book.preview_rows);
    ImGui::SetNextItemWidth(80);
    if (ImGui::InputInt("Columns", &cols)) {
        g_fb.book.preview_columns = static_cast<u32>(cols < 1 ? 1 : cols > 64 ? 64 : cols);
        g_fb.dirty = true;
    }
    ImGui::SetItemTooltip("Cells across the sheet (the sprite's Columns)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    if (ImGui::InputInt("Rows", &rows)) {
        g_fb.book.preview_rows = static_cast<u32>(rows < 1 ? 1 : rows > 64 ? 64 : rows);
        g_fb.dirty = true;
    }
    ImGui::SetItemTooltip("Cells down the sheet (the sprite's Rows)");
}

} // namespace

void open_flipbook_editor(EditorState& s, const asset::AssetRecord& record) {
    if (g_fb.open && g_fb.dirty) save(s);
    char path[300];
    s.registry->source_path(record, path, sizeof path);
    world::Flipbook book;
    if (!asset::load_data_asset(path, book)) {
        std::snprintf(s.status, sizeof s.status, "Could not open %s", record.name);
        return;
    }
    g_fb = Window{};
    g_fb.book = book;
    std::snprintf(g_fb.path, sizeof g_fb.path, "%s", path);
    std::snprintf(g_fb.name, sizeof g_fb.name, "%s", record.name);
    g_fb.open = true;
    g_fb.started = ImGui::GetTime();
    ImGui::SetWindowFocus("###Flipbook");
}

void draw_flipbook_editor(EditorState& s) {
    if (!g_fb.open) return;
    char title[160];
    std::snprintf(title, sizeof title, "%s%s - Flipbook###Flipbook", g_fb.name, g_fb.dirty ? "*" : "");
    ImGui::SetNextWindowSize(ImVec2(960, 600), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!begin_editor(title, &open)) return;
    if (!open) {
        ImGui::End();
        if (g_fb.dirty) save(s);
        g_fb.open = false;
        return;
    }
    world::Flipbook& book = g_fb.book;
    if (with_tip(ImGui::Button("Save"), "Writes the flipbook; sprites using it play the new frames at once")) save(s);
    ImGui::SameLine();
    if (with_tip(ImGui::Button(g_fb.playing ? "Pause" : "Play"), "Plays or stops the preview")) {
        g_fb.playing = !g_fb.playing;
        g_fb.started = ImGui::GetTime();
    }
    ImGui::SameLine();
    if (with_tip(ImGui::Checkbox("Loop", &book.loop), "Starts over when it ends; off: stops on the last frame")) g_fb.dirty = true;
    ImGui::SameLine();
    f32 total = 0.0f;
    for (const world::FlipbookFrame& f : book.frames) total += f.duration;
    ImGui::TextDisabled("%u frames, %.2f s", book.frames.count, static_cast<f64>(total));
    sheet_picker(s);

    const rhi::TextureHandle sheet = book.preview_texture[0] && s.textures ? s.textures->get(book.preview_texture, true) : rhi::TextureHandle{};
    const u64 image = sheet.valid() && s.device ? s.device->debug_ui_texture(sheet) : 0;
    const f32 left_width = ImGui::GetContentRegionAvail().x * 0.55f;
    ImGui::BeginChild("##sheet", ImVec2(left_width, 0), ImGuiChildFlags_Borders);
    // The preview: the frame playing now (or the selected one while paused).
    const i32 now = g_fb.playing ? flipbook_frame(book, static_cast<f32>(ImGui::GetTime() - g_fb.started)) : (g_fb.selected >= 0 ? g_fb.selected : 0);
    const f32 big = 180.0f;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(at, ImVec2(at.x + big, at.y + big), IM_COL32(36, 40, 85, 255));
    if (image && now >= 0 && static_cast<u32>(now) < book.frames.count) {
        ImVec2 lo, hi;
        cell_uv(book.frames[static_cast<u32>(now)].cell, lo, hi);
        dl->AddImage(ImTextureRef(static_cast<ImTextureID>(image)), at, ImVec2(at.x + big, at.y + big), lo, hi);
    }
    ImGui::Dummy(ImVec2(big, big));
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (now >= 0) ImGui::Text("Frame %d", now + 1);
    if (now >= 0 && static_cast<u32>(now) < book.frames.count && book.frames[static_cast<u32>(now)].event[0]) ImGui::TextColored(ImVec4(0.61f, 0.88f, 0.12f, 1), "Event: %s", book.frames[static_cast<u32>(now)].event);
    ImGui::EndGroup();
    ImGui::SeparatorText("Sheet: click a cell to add it as a frame");
    if (!image) {
        ImGui::TextDisabled("Pick the sprite sheet above (and its Columns / Rows) to see the cells.");
    } else {
        const u32 cols = book.preview_columns ? book.preview_columns : 1, rows = book.preview_rows ? book.preview_rows : 1;
        const f32 width = ImGui::GetContentRegionAvail().x;
        const ImVec2 size(width, width * static_cast<f32>(rows) / static_cast<f32>(cols));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Image(ImTextureRef(static_cast<ImTextureID>(image)), size);
        const f32 cw = size.x / static_cast<f32>(cols), ch = size.y / static_cast<f32>(rows);
        for (u32 c = 0; c <= cols; ++c) dl->AddLine(ImVec2(origin.x + cw * static_cast<f32>(c), origin.y), ImVec2(origin.x + cw * static_cast<f32>(c), origin.y + size.y), IM_COL32(0, 0, 0, 110));
        for (u32 r = 0; r <= rows; ++r) dl->AddLine(ImVec2(origin.x, origin.y + ch * static_cast<f32>(r)), ImVec2(origin.x + size.x, origin.y + ch * static_cast<f32>(r)), IM_COL32(0, 0, 0, 110));
        if (g_fb.selected >= 0 && static_cast<u32>(g_fb.selected) < book.frames.count) {  // the selected frame's cell
            const u32 cell = book.frames[static_cast<u32>(g_fb.selected)].cell;
            const ImVec2 lo(origin.x + cw * static_cast<f32>(cell % cols), origin.y + ch * static_cast<f32>((cell / cols) % rows));
            dl->AddRect(lo, ImVec2(lo.x + cw, lo.y + ch), IM_COL32(243, 195, 18, 255), 0.0f, 0, 2.0f);
        }
        if (ImGui::IsItemHovered()) {
            const ImVec2 m = ImGui::GetMousePos();
            const u32 cx = static_cast<u32>((m.x - origin.x) / cw), cy = static_cast<u32>((m.y - origin.y) / ch);
            const u32 cell = (cy < rows ? cy : rows - 1) * cols + (cx < cols ? cx : cols - 1);
            ImGui::SetTooltip("Cell %u: click to add it as a frame", cell);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && book.frames.count < book.frames.capacity()) {
                world::FlipbookFrame f;
                f.cell = cell;
                if (book.frames.count) f.duration = book.frames[book.frames.count - 1].duration;  // the same pace as the last
                book.frames.push_back(f);
                g_fb.selected = static_cast<i32>(book.frames.count - 1);
                g_fb.dirty = true;
            }
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##frames", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::SeparatorText("Frames");
    if (!book.frames.count) ImGui::TextDisabled("No frames yet: click cells of the sheet.");
    i32 move_from = -1, move_to = -1, remove = -1;
    for (u32 i = 0; i < book.frames.count; ++i) {
        world::FlipbookFrame& f = book.frames[i];
        ImGui::PushID(static_cast<int>(i));
        char label[16];
        std::snprintf(label, sizeof label, "%u", i + 1);
        if (ImGui::Selectable(label, g_fb.selected == static_cast<i32>(i), 0, ImVec2(24, 0))) g_fb.selected = static_cast<i32>(i);
        ImGui::SameLine();
        if (image) {
            ImVec2 lo, hi;
            cell_uv(f.cell, lo, hi);
            ImGui::Image(ImTextureRef(static_cast<ImTextureID>(image)), ImVec2(24, 24), lo, hi);
            ImGui::SameLine();
        }
        int cell = static_cast<int>(f.cell);
        ImGui::SetNextItemWidth(60);
        if (ImGui::InputInt("##cell", &cell, 0)) {
            f.cell = static_cast<u32>(cell < 0 ? 0 : cell);
            g_fb.dirty = true;
        }
        ImGui::SetItemTooltip("The sheet cell (left to right, then down)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        if (ImGui::DragFloat("##time", &f.duration, 0.005f, 0.001f, 10.0f, "%.3f s")) g_fb.dirty = true;
        ImGui::SetItemTooltip("How long the frame shows");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        if (ImGui::InputTextWithHint("##event", "event", f.event, sizeof f.event)) g_fb.dirty = true;
        ImGui::SetItemTooltip("In Play, the sprite's scripts run this function when the frame starts (footstep, swing...)");
        ImGui::SameLine();
        if (with_tip(ImGui::ArrowButton("##up", ImGuiDir_Up), "Earlier") && i > 0) move_from = static_cast<i32>(i), move_to = static_cast<i32>(i) - 1;
        ImGui::SameLine();
        if (with_tip(ImGui::ArrowButton("##down", ImGuiDir_Down), "Later") && i + 1 < book.frames.count) move_from = static_cast<i32>(i), move_to = static_cast<i32>(i) + 1;
        ImGui::SameLine();
        if (with_tip(ImGui::SmallButton("X"), "Deletes the frame")) remove = static_cast<i32>(i);
        ImGui::PopID();
    }
    if (move_from >= 0) {
        const world::FlipbookFrame t = book.frames[static_cast<u32>(move_from)];
        book.frames[static_cast<u32>(move_from)] = book.frames[static_cast<u32>(move_to)];
        book.frames[static_cast<u32>(move_to)] = t;
        g_fb.selected = move_to;
        g_fb.dirty = true;
    }
    if (remove >= 0) {
        for (u32 k = static_cast<u32>(remove); k + 1 < book.frames.count; ++k) book.frames[k] = book.frames[k + 1];
        --book.frames.count;
        g_fb.selected = -1;
        g_fb.dirty = true;
    }
    if (book.frames.count) {
        static f32 all = 0.1f;
        ImGui::Separator();
        ImGui::SetNextItemWidth(80);
        ImGui::DragFloat("##all", &all, 0.005f, 0.001f, 10.0f, "%.3f s");
        ImGui::SameLine();
        if (with_tip(ImGui::Button("Set All Durations"), "Gives every frame this duration (an even pace)")) {
            for (world::FlipbookFrame& f : book.frames) f.duration = all;
            g_fb.dirty = true;
        }
        ImGui::SameLine();
        if (with_tip(ImGui::Button("Clear"), "Deletes every frame")) {
            book.frames.count = 0;
            g_fb.selected = -1;
            g_fb.dirty = true;
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace lsd::editor
