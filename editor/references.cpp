#include "references.h"

#include <cctype>
#include <fstream>
#include <sstream>

namespace lsd::editor {

namespace {

bool is_text(asset::AssetType type) {
    using asset::AssetType;
    return type == AssetType::Level || type == AssetType::Data || type == AssetType::Prefab || type == AssetType::Graph || type == AssetType::Cue ||
           type == AssetType::MaterialGraph || type == AssetType::BehaviorTree;
}

bool read(const std::string& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::stringstream text;
    text << file.rdbuf();
    out = text.str();
    return true;
}

// The files an asset's links live in: its .meta, and the source itself if it is text.
std::vector<std::string> files_of(const asset::AssetRegistry& registry, const asset::AssetRecord& r) {
    char source[300];
    registry.source_path(r, source, sizeof source);
    std::vector<std::string> files{std::string(source) + ".meta"};
    if (is_text(r.type)) files.emplace_back(source);
    return files;
}

} // namespace

std::vector<std::string> find_guids(const std::string& text) {
    std::vector<std::string> out;
    size_t run = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        const bool hex = i < text.size() && std::isxdigit(static_cast<unsigned char>(text[i]));
        if (hex) {
            ++run;
            continue;
        }
        if (run == 32) {
            std::string g = text.substr(i - 32, 32);
            for (char& c : g) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            bool seen = false;
            for (const std::string& o : out) seen = seen || o == g;
            if (!seen) out.push_back(g);
        }
        run = 0;
    }
    return out;
}

void ReferenceIndex::build(const asset::AssetRegistry& registry) {
    uses_.clear();
    for (usize i = 0; i < registry.count(); ++i) {
        const asset::AssetRecord& r = registry.get(i);
        char self[33];
        asset::to_string(r.guid, self);
        std::vector<std::string>& list = uses_[self];
        for (const std::string& file : files_of(registry, r)) {
            std::string text;
            if (!read(file, text)) continue;
            for (std::string& g : find_guids(text)) {
                bool seen = g == self;  // a .meta names its own asset
                for (const std::string& o : list) seen = seen || o == g;
                if (!seen) list.push_back(std::move(g));
            }
        }
    }
}

std::vector<std::string> cook_set(const asset::AssetRegistry& registry, const std::string& root_text) {
    // Lookups by name and by path (relative to Content/, with and without the extension).
    std::unordered_map<std::string, std::vector<usize>> by_name, by_path;
    std::unordered_map<std::string, usize> by_guid;
    for (usize i = 0; i < registry.count(); ++i) {
        const asset::AssetRecord& r = registry.get(i);
        char g[33];
        asset::to_string(r.guid, g);
        by_guid[g] = i;
        by_name[r.name].push_back(i);
        const std::string path = r.path;
        by_path[path].push_back(i);
        const usize dot = path.rfind('.');
        if (dot != std::string::npos) by_path[path.substr(0, dot)].push_back(i);
    }
    std::vector<std::string> out;
    std::vector<usize> queue;
    std::vector<bool> taken(registry.count(), false);
    auto take = [&](usize i) {
        if (taken[i]) return;
        taken[i] = true;
        queue.push_back(i);
    };
    auto scan = [&](const std::string& text, bool strings) {
        for (const std::string& g : find_guids(text)) {
            const auto found = by_guid.find(g);
            if (found != by_guid.end()) take(found->second);
        }
        if (!strings) return;
        for (usize at = 0; at < text.size(); ++at) {  // every "quoted" or 'quoted' run up to 200 characters
            const char q = text[at];
            if (q != '"' && q != '\'') continue;
            const usize end = text.find(q, at + 1);
            if (end == std::string::npos) break;
            if (end - at - 1 <= 200 && end > at + 1) {
                std::string s = text.substr(at + 1, end - at - 1);
                for (char& c : s) c = c == '\\' ? '/' : c;
                if (s.rfind("Content/", 0) == 0) s.erase(0, 8);
                const auto p = by_path.find(s);
                if (p != by_path.end()) for (const usize i : p->second) take(i);
                const usize slash = s.rfind('/');
                const std::string leaf = slash == std::string::npos ? s : s.substr(slash + 1);
                const usize dot = leaf.rfind('.');
                const auto n = by_name.find(dot == std::string::npos ? leaf : leaf.substr(0, dot));
                if (n != by_name.end()) for (const usize i : n->second) take(i);
            }
            if (q == '"') at = end;  // apostrophes inside "..." are not openings
        }
    };
    scan(root_text, true);
    for (usize next = 0; next < queue.size(); ++next) {
        const asset::AssetRecord& r = registry.get(queue[next]);
        char self[33];
        asset::to_string(r.guid, self);
        out.emplace_back(self);
        char source[300];
        registry.source_path(r, source, sizeof source);
        std::string text;
        if (read(std::string(source) + ".meta", text)) scan(text, false);
        const bool scripted = r.type == asset::AssetType::Script || r.type == asset::AssetType::UiDocument || r.type == asset::AssetType::UiStyle ||
                              r.type == asset::AssetType::Table;
        if ((is_text(r.type) || scripted) && read(source, text)) scan(text, true);
        if (r.type == asset::AssetType::UiDocument) {  // its own .js beside it (Menu.rml + Menu.js)
            std::string js = r.path;
            js = js.substr(0, js.rfind('.')) + ".js";
            const auto p = by_path.find(js);
            if (p != by_path.end()) for (const usize i : p->second) take(i);
        }
    }
    return out;
}

const std::vector<std::string>& ReferenceIndex::uses(const std::string& guid) const {
    static const std::vector<std::string> none;
    const auto found = uses_.find(guid);
    return found == uses_.end() ? none : found->second;
}

std::vector<std::string> ReferenceIndex::used_by(const std::string& guid) const {
    std::vector<std::string> out;
    for (const auto& [user, list] : uses_) {
        for (const std::string& g : list) {
            if (g == guid) {
                out.push_back(user);
                break;
            }
        }
    }
    return out;
}

u32 replace_references(const asset::AssetRegistry& registry, const std::string& from, const std::string& to) {
    u32 changed = 0;
    if (from.size() != 32 || to.size() != 32 || from == to) return 0;
    for (usize i = 0; i < registry.count(); ++i) {
        const asset::AssetRecord& r = registry.get(i);
        char self[33];
        asset::to_string(r.guid, self);
        if (from == self) continue;  // the asset being replaced keeps its own identity
        for (const std::string& file : files_of(registry, r)) {
            std::string text;
            if (!read(file, text)) continue;
            bool any = false;
            for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + 32)) {
                text.replace(at, 32, to);
                any = true;
            }
            if (!any) continue;
            std::ofstream out(file, std::ios::binary);
            out << text;
            changed += out ? 1u : 0u;
        }
    }
    return changed;
}

} // namespace lsd::editor
