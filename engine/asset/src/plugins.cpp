#include "lsd/asset/plugins.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace lsd::asset {

namespace fs = std::filesystem;

bool safe_relative_path(const char* path) {
    if (!path || !*path || path[0] == '/' || path[0] == '\\' || std::strchr(path, ':') || std::strchr(path, '\\')) return false;
    std::istringstream parts{std::string(path)};
    std::string part;
    while (std::getline(parts, part, '/')) {
        if (part == ".." || part.empty() || part == ".") return false;
    }
    return path[std::strlen(path) - 1] != '/';
}

namespace {

std::string descriptor_file(const char* root, const std::string& name) { return (fs::path(root) / name / "plugin.json").generic_string(); }

} // namespace

std::vector<PluginInfo> discover_plugins(const char* root) {
    std::vector<PluginInfo> out;
    std::error_code error;
    for (const fs::directory_entry& d : fs::directory_iterator(root, error)) {
        const std::string name = d.path().filename().string();
        PluginInfo info;
        if (!d.is_directory(error) || !valid_name(name.c_str()) || !reflect::load_file(info.descriptor, descriptor_file(root, name).c_str())) continue;
        std::snprintf(info.descriptor.name, sizeof info.descriptor.name, "%s", name.c_str());  // the folder is the identity
        info.folder = (fs::path(root) / name).generic_string();
        out.push_back(info);
    }
    std::sort(out.begin(), out.end(), [](const PluginInfo& a, const PluginInfo& b) { return std::strcmp(a.descriptor.name, b.descriptor.name) < 0; });
    return out;
}

bool create_plugin(const char* root, const char* name, std::string& error_text) {
    if (!valid_name(name) || std::strlen(name) >= sizeof(PluginDescriptor::name)) {
        error_text = "Not a valid plugin name";
        return false;
    }
    std::error_code error;
    const fs::path dir = fs::path(root) / name;
    if (fs::exists(dir, error)) {
        error_text = std::string("Plugins/") + name + " already exists";
        return false;
    }
    fs::create_directories(dir / "Content", error);
    PluginDescriptor d;
    std::snprintf(d.name, sizeof d.name, "%s", name);
    if (error || !reflect::save_file(d, descriptor_file(root, name).c_str())) {
        error_text = "Could not write the plugin folder";
        return false;
    }
    return true;
}

bool set_plugin_enabled(const char* root, const char* name, bool enabled) {
    if (!valid_name(name)) return false;
    PluginDescriptor d;
    const std::string file = descriptor_file(root, name);
    if (!reflect::load_file(d, file.c_str())) return false;
    d.enabled = enabled;
    return reflect::save_file(d, file.c_str());
}

std::vector<AssetMount> plugin_mounts(const std::vector<PluginInfo>& plugins) {
    std::vector<AssetMount> out;
    for (const PluginInfo& p : plugins) {
        if (!p.descriptor.enabled) continue;
        AssetMount m;
        std::snprintf(m.prefix, sizeof m.prefix, "Plugins/%s", p.descriptor.name);
        std::snprintf(m.dir, sizeof m.dir, "%s/Content", p.folder.c_str());
        out.push_back(m);
    }
    return out;
}

std::vector<std::string> plugin_source_files(const std::vector<PluginInfo>& plugins) {
    std::vector<std::string> out;
    for (const PluginInfo& p : plugins) {
        if (!p.descriptor.enabled) continue;
        std::error_code error;
        for (const fs::directory_entry& f : fs::directory_iterator(fs::path(p.folder) / "Source", error)) {
            const std::string ext = f.path().extension().string();
            if (f.is_regular_file(error) && (ext == ".c" || ext == ".cpp")) out.push_back(fs::absolute(f.path(), error).generic_string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

u64 hash_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;
    u64 h = 1469598103934665603ull;  // FNV-1a
    char buffer[8192];
    while (in.read(buffer, sizeof buffer) || in.gcount() > 0) {
        for (std::streamsize i = 0; i < in.gcount(); ++i) h = (h ^ static_cast<unsigned char>(buffer[i])) * 1099511628211ull;
    }
    return h;
}

Manifest manifest_of_folder(const char* dir) {
    Manifest m;
    std::error_code error;
    for (const fs::directory_entry& f : fs::recursive_directory_iterator(dir, error)) {
        if (!f.is_regular_file(error) || f.path().extension() == ".meta") continue;
        m.entries.push_back({fs::relative(f.path(), dir, error).generic_string(), hash_file(f.path().string().c_str()), static_cast<u64>(f.file_size(error))});
    }
    std::sort(m.entries.begin(), m.entries.end(), [](const ManifestEntry& a, const ManifestEntry& b) { return a.path < b.path; });
    return m;
}

std::string manifest_to_text(const Manifest& manifest) {
    std::string text = "lsd-manifest 1\nname " + manifest.name + "\n";
    char line[64];
    for (const ManifestEntry& e : manifest.entries) {
        std::snprintf(line, sizeof line, "%016llx %llu ", static_cast<unsigned long long>(e.hash), static_cast<unsigned long long>(e.size));
        text += line + e.path + "\n";
    }
    return text;
}

bool manifest_from_text(const std::string& text, Manifest& out) {
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line != "lsd-manifest 1") return false;
    Manifest m;
    if (!std::getline(in, line) || line.rfind("name ", 0) != 0) return false;
    m.name = line.substr(5);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        unsigned long long hash = 0, size = 0;
        int used = 0;
        if (std::sscanf(line.c_str(), "%16llx %llu %n", &hash, &size, &used) < 2 || used <= 0) return false;
        const std::string path = line.substr(static_cast<usize>(used));
        if (!safe_relative_path(path.c_str())) return false;
        m.entries.push_back({path, hash, size});
    }
    out = m;
    return true;
}

bool save_manifest(const Manifest& manifest, const char* path) {
    std::ofstream out(path, std::ios::binary);
    out << manifest_to_text(manifest);
    return static_cast<bool>(out);
}

bool load_manifest(const char* path, Manifest& out) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return in && manifest_from_text(text.str(), out);
}

bool manifest_matches(const Manifest& base, const std::string& path, u64 hash) {
    return std::any_of(base.entries.begin(), base.entries.end(), [&](const ManifestEntry& e) { return e.path == path && e.hash == hash; });
}

std::vector<std::string> manifest_changed(const Manifest& base, const Manifest& now) {
    std::map<std::string, u64> known;
    for (const ManifestEntry& e : base.entries) known[e.path] = e.hash;
    std::vector<std::string> out;
    for (const ManifestEntry& e : now.entries) {
        const auto it = known.find(e.path);
        if (it == known.end() || it->second != e.hash) out.push_back(e.path);
    }
    return out;
}

std::vector<AssetMount> dlc_mounts(const char* root) {
    std::vector<std::string> names;
    std::error_code error;
    for (const fs::directory_entry& d : fs::directory_iterator(root, error)) {
        if (d.is_directory(error) && fs::is_directory(d.path() / "Content", error)) names.push_back(d.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    std::vector<AssetMount> out;
    for (const std::string& n : names) {
        AssetMount m;
        std::snprintf(m.dir, sizeof m.dir, "%s/%s/Content", root, n.c_str());
        out.push_back(m);
    }
    return out;
}

} // namespace lsd::asset
