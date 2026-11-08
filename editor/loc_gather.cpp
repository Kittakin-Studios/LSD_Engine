#include "loc_gather.h"

#include "lsd/asset/asset_registry.h"
#include "lsd/asset/data_table.h"
#include "lsd/asset/guid.h"
#include "lsd/foundation/log.h"
#include "lsd/script/graph.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

namespace lsd::editor {

namespace {

std::string trim(const std::string& s) {
    usize a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    std::string t = s.substr(a, b - a);
    std::string out;  // inner runs of white space become one space, as the page shows them
    for (char c : t) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!out.empty() && out.back() != ' ') out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

std::string decode_entities(std::string s) {
    static const char* const kEntities[][2] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&nbsp;", " "}};
    for (const auto& e : kEntities) {
        for (usize at = s.find(e[0]); at != std::string::npos; at = s.find(e[0], at + 1)) s.replace(at, std::strlen(e[0]), e[1]);
    }
    return s;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace

void gather_script_text(const char* source, std::vector<GatheredText>& out) {
    for (const char* at = std::strstr(source, "lsd.text("); at; at = std::strstr(at + 1, "lsd.text(")) {
        const char* p = at + 9;
        while (*p == ' ' || *p == '\t') ++p;
        const char quote = *p;
        if (quote != '"' && quote != '\'') continue;  // a key built at run time: not gathered
        const char* end = std::strchr(p + 1, quote);
        if (!end || end == p + 1) continue;
        out.push_back({std::string(p + 1, end), ""});
    }
}

void gather_rml_text(const char* rml, std::vector<GatheredText>& out) {
    const std::string s = rml;
    usize i = 0;
    while (i < s.size()) {
        const usize lt = s.find('<', i);
        const std::string text = trim(decode_entities(s.substr(i, (lt == std::string::npos ? s.size() : lt) - i)));
        bool letters = false;
        for (char c : text) letters |= std::isalpha(static_cast<unsigned char>(c)) != 0;
        if (letters && text.find("{{") == std::string::npos) out.push_back({text, text});
        if (lt == std::string::npos) break;
        // Skip the tag; head, style and script contents are not shown text.
        usize gt = s.find('>', lt);
        if (gt == std::string::npos) break;
        for (const char* skipped : {"head", "style", "script"}) {
            const std::string open = std::string("<") + skipped;
            if (s.compare(lt, open.size(), open) == 0 && (s[lt + open.size()] == '>' || std::isspace(static_cast<unsigned char>(s[lt + open.size()])))) {
                const usize close = s.find(std::string("</") + skipped, gt);
                gt = close == std::string::npos ? s.size() - 1 : s.find('>', close);
                if (gt == std::string::npos) gt = s.size() - 1;
            }
        }
        if (s.compare(lt, 4, "<!--") == 0) {
            const usize close = s.find("-->", lt);
            gt = close == std::string::npos ? s.size() - 1 : close + 2;
        }
        i = gt + 1;
    }
}

u32 merge_string_table(const std::string& csv, const std::vector<GatheredText>& found, std::string& out) {
    asset::DataTable table(default_allocator());
    const bool has_table = !csv.empty() && table.parse(csv.c_str(), csv.size()) && table.columns() >= 1 && table.rows() >= 1;
    const u32 columns = has_table ? (table.columns() < 2 ? 2u : table.columns()) : 2u;
    std::vector<std::string> cells;
    std::set<std::string> keys;
    const u32 rows = has_table ? table.rows() : 1u;
    for (u32 r = 0; r < rows; ++r) {
        for (u32 c = 0; c < columns; ++c) cells.push_back(has_table ? table.cell(r, c) : (c == 0 ? "Key" : "en"));
        if (has_table && columns == 2 && table.columns() < 2 && r == 0) cells.back() = "en";
        if (r) keys.insert(cells[r * columns]);
    }
    u32 added = 0;
    for (const GatheredText& g : found) {
        if (g.key.empty() || !keys.insert(g.key).second) continue;
        cells.push_back(g.key);
        cells.push_back(g.text);
        for (u32 c = 2; c < columns; ++c) cells.push_back("");
        ++added;
    }
    std::vector<const char*> pointers;
    for (const std::string& c : cells) pointers.push_back(c.c_str());
    Array<char> text(default_allocator());
    asset::write_csv(pointers.data(), static_cast<u32>(cells.size() / columns), columns, text);
    out.assign(text.data(), text.size());
    return added;
}

bool gather_project_text(ProjectSettings& project, asset::AssetRegistry& registry, char* message, usize capacity, bool& project_changed) {
    project_changed = false;
    namespace fs = std::filesystem;
    std::vector<GatheredText> found;
    u32 files = 0;
    for (usize i = 0; i < registry.count(); ++i) {
        const asset::AssetRecord& r = registry.get(i);
        char path[300];
        registry.source_path(r, path, sizeof path);
        if (r.type == asset::AssetType::Script && fs::path(path).extension() != ".lsdgraph") {
            gather_script_text(read_file(path).c_str(), found);
            ++files;
        } else if (r.type == asset::AssetType::UiDocument) {
            gather_rml_text(read_file(path).c_str(), found);
            ++files;
        } else if (r.type == asset::AssetType::Graph || fs::path(path).extension() == ".lsdgraph") {
            script::Graph graph(default_allocator());
            if (!script::load_graph_file(graph, path)) continue;
            for (const script::GraphNode& n : graph.nodes) {
                if (std::strcmp(n.type, "LocText") == 0 || std::strcmp(n.type, "FormatText") == 0) found.push_back({n.values[0], ""});
            }
            ++files;
        }
    }
    // The table: the project's, or a new one.
    std::string table_path;
    asset::Guid guid;
    if (const asset::AssetRecord* r = project.string_table[0] && asset::parse_guid(project.string_table, guid) ? registry.find(guid) : nullptr) {
        char path[300];
        registry.source_path(*r, path, sizeof path);
        table_path = path;
    } else {
        const fs::path dir = fs::path(registry.content_dir()) / "Localization";
        std::error_code ignored;
        fs::create_directories(dir, ignored);
        table_path = (dir / "ST_Game.csv").generic_string();
    }
    const std::string before = fs::exists(table_path) ? read_file(table_path) : std::string();
    std::string after;
    const u32 added = merge_string_table(before, found, after);
    if (added || before.empty()) {
        std::ofstream f(table_path, std::ios::binary | std::ios::trunc);
        f.write(after.data(), static_cast<std::streamsize>(after.size()));
        if (!f) {
            std::snprintf(message, capacity, "Could not write %s", table_path.c_str());
            return false;
        }
    }
    if (!project.string_table[0] || !registry.find(guid)) {  // set the new table on the project
        registry.scan();
        for (usize i = 0; i < registry.count(); ++i) {
            char path[300];
            registry.source_path(registry.get(i), path, sizeof path);
            if (fs::path(path).lexically_normal() == fs::path(table_path).lexically_normal()) {
                asset::to_string(registry.get(i).guid, project.string_table);
                project_changed = true;
            }
        }
    }
    std::snprintf(message, capacity, "Gather Text: %u new key%s from %u file%s into %s", added, added == 1 ? "" : "s", files, files == 1 ? "" : "s", table_path.c_str());
    LSD_LOG_INFO("editor", "%s", message);
    return true;
}

} // namespace lsd::editor
