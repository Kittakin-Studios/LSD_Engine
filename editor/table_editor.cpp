#include "table_editor.h"
#include "workspaces.h"

#include "lsd/asset/data_table.h"
#include "lsd/foundation/log.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace lsd::editor {

namespace {

struct Cell {
    char text[128] = "";
};

struct TableWindow {
    bool open = false;
    bool dirty = false;
    char path[300] = "";
    char name[64] = "";
    u32 columns = 0;
    std::vector<Cell> cells;  // row by row; row 0 = column names, column 0 = row names
    char message[160] = "";
};
TableWindow g_table;

u32 row_count() { return g_table.columns ? static_cast<u32>(g_table.cells.size() / g_table.columns) : 0; }
Cell& at(u32 row, u32 column) { return g_table.cells[row * g_table.columns + column]; }

void unique_name(char* out, usize capacity, const char* base, bool column) {
    for (int n = 1;; ++n) {
        std::snprintf(out, capacity, "%s%d", base, n);
        bool taken = false;
        const u32 count = column ? g_table.columns : row_count();
        for (u32 i = 0; i < count && !taken; ++i) taken = std::strcmp(column ? at(0, i).text : at(i, 0).text, out) == 0;
        if (!taken) return;
    }
}

bool save(EditorState& s) {
    TableWindow& t = g_table;
    std::vector<const char*> texts;
    for (const Cell& c : t.cells) texts.push_back(c.text);
    Array<char> csv(default_allocator());
    asset::write_csv(texts.data(), row_count(), t.columns, csv);
    FILE* file = std::fopen(t.path, "wb");
    const bool ok = file && std::fwrite(csv.data(), 1, csv.size(), file) == csv.size();
    if (file) std::fclose(file);
    std::snprintf(t.message, sizeof t.message, ok ? "Saved" : "Could not save %s", t.path);
    if (ok) t.dirty = false;
    std::snprintf(s.status, sizeof s.status, "%s: %s", t.name, t.message);
    return ok;
}

} // namespace

std::string create_table_file(const char* folder_path, const char* base) {
    const std::filesystem::path folder(folder_path);
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    std::filesystem::path path = folder / (std::string(base) + asset::kDataTableExtension);
    for (int n = 2; std::filesystem::exists(path, error); ++n) path = folder / (std::string(base) + std::to_string(n) + asset::kDataTableExtension);
    FILE* file = std::fopen(path.generic_string().c_str(), "wb");
    if (!file) return {};
    std::fputs("Name,Value\nRow1,0\n", file);
    std::fclose(file);
    return path.generic_string();
}

void open_table(EditorState& s, const asset::AssetRecord& record) {
    TableWindow& t = g_table;
    if (t.open && t.dirty) save(s);
    char path[300];
    s.registry->source_path(record, path, sizeof path);
    asset::DataTable data(default_allocator());
    if (!data.load(path)) {
        std::snprintf(s.status, sizeof s.status, "Could not read %s (an unclosed quote?)", record.path);
        return;
    }
    std::snprintf(t.path, sizeof t.path, "%s", path);
    std::snprintf(t.name, sizeof t.name, "%s", record.name);
    t.columns = data.columns() ? data.columns() : 1;
    t.cells.assign(static_cast<usize>(data.rows() ? data.rows() : 1) * t.columns, Cell{});
    for (u32 r = 0; r < data.rows(); ++r) {
        for (u32 c = 0; c < data.columns(); ++c) std::snprintf(at(r, c).text, sizeof(Cell::text), "%s", data.cell(r, c));
    }
    t.open = true;
    t.dirty = false;
    t.message[0] = '\0';
    ImGui::SetWindowFocus("###DataTable");
}

void draw_table_editor(EditorState& s) {
    TableWindow& t = g_table;
    if (!t.open) return;
    char title[128];
    std::snprintf(title, sizeof title, "%s%s - Data Table###DataTable", t.name, t.dirty ? "*" : "");
    const ImGuiViewport* main = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(main->WorkPos.x + main->WorkSize.x * 0.5f, main->WorkPos.y + main->WorkSize.y * 0.45f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(760, 420), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!begin_editor(title, &open)) return;  // a panel tab, or the workspace's editor area
    if (!open) {
        if (t.dirty) save(s);
        t.open = false;
    }
    if (ImGui::Button("Save")) save(s);
    ImGui::SetItemTooltip("Ctrl+S. Playing scripts read the new values on their next lsd.table() call");
    ImGui::SameLine();
    if (ImGui::Button("+ Row")) {
        const u32 rows = row_count();
        t.cells.resize(t.cells.size() + t.columns);
        unique_name(at(rows, 0).text, sizeof(Cell::text), "Row", false);
        t.dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Column") && t.columns < 64) {
        const u32 rows = row_count();
        std::vector<Cell> grown(static_cast<usize>(rows) * (t.columns + 1));
        for (u32 r = 0; r < rows; ++r) {
            for (u32 c = 0; c < t.columns; ++c) grown[r * (t.columns + 1) + c] = at(r, c);
        }
        t.cells.swap(grown);
        ++t.columns;
        unique_name(at(0, t.columns - 1).text, sizeof(Cell::text), "Column", true);
        t.dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Open in External Program")) {
        if (t.dirty) save(s);
        std::error_code error;
        SDL_OpenURL(("file:///" + std::filesystem::absolute(t.path, error).generic_string()).c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Export JSON")) {  // next to the .csv: the same rows with typed values
        if (t.dirty) save(s);
        asset::DataTable data(default_allocator());
        Array<char> json(default_allocator());
        const std::string out = std::filesystem::path(t.path).replace_extension(".json").string();
        FILE* file = data.load(t.path) ? std::fopen(out.c_str(), "wb") : nullptr;
        if (file) {
            asset::export_json(data, json);
            std::fwrite(json.data(), 1, json.size(), file);
            std::fclose(file);
        }
        std::snprintf(t.message, sizeof t.message, file ? "Exported %s" : "Could not export to %s", out.c_str());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", t.message[0] ? t.message : "Right-click a column name: its Type, or delete it");

    const u32 rows = row_count();
    i32 delete_row = -1, delete_column = -1;
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("cells", static_cast<int>(t.columns + 1), flags)) {
        ImGui::TableSetupScrollFreeze(2, 1);
        ImGui::TableSetupColumn("##delete", ImGuiTableColumnFlags_WidthFixed, 22.0f);
        for (u32 c = 0; c < t.columns; ++c) ImGui::TableSetupColumn(at(0, c).text, ImGuiTableColumnFlags_WidthFixed, c == 0 ? 140.0f : 120.0f);
        for (u32 r = 0; r < rows; ++r) {
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(r));
            ImGui::TableSetColumnIndex(0);
            if (r > 0 && ImGui::SmallButton("x")) delete_row = static_cast<i32>(r);
            if (r > 0) ImGui::SetItemTooltip("Delete this row");
            for (u32 c = 0; c < t.columns; ++c) {
                ImGui::TableSetColumnIndex(static_cast<int>(c + 1));
                ImGui::PushID(static_cast<int>(c));
                char column[128];
                const asset::ColumnType type = asset::column_type(at(0, c).text, column, sizeof column);
                const bool misfit = r > 0 && c > 0 && !asset::cell_fits(type, at(r, c).text);  // "abc" in a number column
                if (r == 0 || c == 0) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.18f, 0.25f, 0.45f, 1.0f));  // names
                else if (misfit) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.55f, 0.12f, 0.12f, 1.0f));
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::InputText("##cell", at(r, c).text, sizeof(Cell::text))) t.dirty = true;
                if (r == 0 || c == 0 || misfit) ImGui::PopStyleColor();
                if (misfit) ImGui::SetItemTooltip("Not a %s (the column's type)", asset::to_string(type));
                if (r == 0 && c > 0 && ImGui::BeginPopupContextItem("column_menu")) {
                    if (ImGui::BeginMenu("Type")) {  // Unreal's row struct: the header becomes "Name:type"
                        for (const asset::ColumnType each : {asset::ColumnType::Auto, asset::ColumnType::Number, asset::ColumnType::Int, asset::ColumnType::Bool,
                                                             asset::ColumnType::String, asset::ColumnType::Vector}) {
                            if (ImGui::MenuItem(asset::to_string(each), nullptr, type == each)) {
                                if (each == asset::ColumnType::Auto) std::snprintf(at(0, c).text, sizeof(Cell::text), "%s", column);
                                else std::snprintf(at(0, c).text, sizeof(Cell::text), "%s:%s", column, asset::to_string(each));
                                t.dirty = true;
                            }
                        }
                        ImGui::EndMenu();
                    }
                    if (ImGui::MenuItem("Delete Column")) delete_column = static_cast<i32>(c);
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (delete_row > 0) {
        t.cells.erase(t.cells.begin() + delete_row * static_cast<i32>(t.columns), t.cells.begin() + (delete_row + 1) * static_cast<i32>(t.columns));
        t.dirty = true;
    }
    if (delete_column > 0) {
        std::vector<Cell> shrunk;
        for (u32 r = 0; r < rows; ++r) {
            for (u32 c = 0; c < t.columns; ++c) {
                if (static_cast<i32>(c) != delete_column) shrunk.push_back(at(r, c));
            }
        }
        t.cells.swap(shrunk);
        --t.columns;
        t.dirty = true;
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) save(s);
    ImGui::End();
}

} // namespace lsd::editor
