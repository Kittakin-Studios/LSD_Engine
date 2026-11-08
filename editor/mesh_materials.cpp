#include "mesh_materials.h"

#include "lsd/asset/data_asset.h"
#include "lsd/asset/material_import.h"
#include "lsd/foundation/log.h"
#include "lsd/render/material.h"

#include <SDL3/SDL_stdinc.h>

#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>

namespace lsd::editor {

namespace fs = std::filesystem;

namespace {

// A file or asset name made safe: characters Windows does not allow become '_'.
std::string safe_name(const char* name) {
    std::string out = name && name[0] ? name : "Material";
    for (char& c : out) {
        if (std::strchr("\\/:*?\"<>|", c) || static_cast<unsigned char>(c) < 32) c = '_';
    }
    return out;
}

// One material's plan between the passes: the texture files it will use (Content paths, "" = none).
struct Plan {
    const asset::ImportedMaterial* source = nullptr;
    std::string material_path;  // Content/<folder>/Materials/M_<name>.lsdasset
    bool exists = false;        // kept as it is
    std::string base_color, normal, orm, emissive;
    bool masked = false;
    bool metal_map = false, rough_map = false;  // packed into the ORM: the values become 1 (they multiply the map)
};

class Importer {
public:
    Importer(asset::AssetRegistry& registry, const asset::AssetRecord& mesh, const char* original_folder) : registry_(registry), mesh_(mesh) {
        char source[300];
        registry.source_path(mesh, source, sizeof source);
        source_ = fs::path(source);
        mesh_dir_ = source_.parent_path();
        textures_dir_ = mesh_dir_ / "Textures";
        const std::string fbm = source_.stem().string() + ".fbm";  // FBX exporters put textures in <file>.fbm
        if (original_folder && original_folder[0]) {
            search_.push_back(fs::path(original_folder));
            search_.push_back(fs::path(original_folder) / fbm);
        }
        search_.push_back(mesh_dir_);
        search_.push_back(mesh_dir_ / fbm);
        search_.push_back(textures_dir_);
    }

    // The source image of a map on disk: where the file says, then the search folders, then any
    // texture in Content with that file name. Embedded images are written into Textures/ first.
    std::string locate(const asset::ImportedMap& map, bool quiet = false) {
        std::error_code error;
        if (map.embedded) {
            fs::create_directories(textures_dir_, error);
            const fs::path out = textures_dir_ / safe_name(map.embedded_name);
            if (!fs::exists(out, error)) {
                if (FILE* f = std::fopen(out.string().c_str(), "wb")) {
                    std::fwrite(map.embedded, 1, map.embedded_size, f);
                    std::fclose(f);
                }
            }
            return fs::exists(out, error) ? out.generic_string() : std::string();
        }
        if (!map.path[0]) return {};
        const fs::path written(map.path), relative(map.relative_path);
        if (written.is_absolute() && fs::exists(written, error)) return written.generic_string();
        const fs::path name = written.filename();
        for (const fs::path& dir : search_) {
            for (const fs::path& p : {written, relative}) {
                if (!p.empty() && !p.is_absolute() && fs::exists(dir / p, error)) return (dir / p).generic_string();
            }
            if (fs::exists(dir / name, error)) return (dir / name).generic_string();
        }
        for (usize i = 0; i < registry_.count(); ++i) {
            const asset::AssetRecord& r = registry_.get(i);
            if (r.type != asset::AssetType::Texture || SDL_strcasecmp(fs::path(r.path).filename().string().c_str(), name.string().c_str()) != 0) continue;
            char path[300];
            registry_.source_path(r, path, sizeof path);
            return fs::path(path).generic_string();
        }
        if (quiet) return {};
        LSD_LOG_WARN("editor", "%s: texture %s not found (looked next to the file, in its .fbm folder and in Content)", mesh_.path, map.path);
        ++missing_;
        return {};
    }

    // A map the file does not connect, found by its texture set's naming next to the colour map
    // (Pouches_001_Diffuse.png -> Pouches_001_Metallic.png): Substance, Character Creator and most
    // exporters name sets like this, and FBX has no metallic / roughness slot Blender fills.
    std::string sibling(const std::string& colour, std::initializer_list<const char*> kinds) {
        if (colour.empty()) return {};
        const fs::path file(colour);
        std::string stem = file.stem().string();
        for (const char* suffix : {"_Diffuse", "_BaseColor", "_Base_Color", "_Albedo", "_Color", "_Col", "_D"}) {
            const usize n = std::strlen(suffix);
            if (stem.size() > n && SDL_strcasecmp(stem.c_str() + stem.size() - n, suffix) == 0) {
                stem.resize(stem.size() - n);
                std::error_code error;
                for (const char* kind : kinds) {
                    for (const char* ext : {".png", ".jpg", ".jpeg", ".tga"}) {
                        const fs::path candidate = file.parent_path() / (stem + "_" + kind + ext);
                        if (fs::exists(candidate, error)) {
                            LSD_LOG_INFO("editor", "%s: using %s (named like the colour map)", mesh_.path, candidate.filename().string().c_str());
                            return candidate.generic_string();
                        }
                    }
                }
                break;
            }
        }
        return {};
    }

    // A located image as a texture in Content: kept where it is when already inside Content, else
    // copied into Textures/ (once).
    std::string into_content(const std::string& located) {
        if (located.empty()) return {};
        std::error_code error;
        const fs::path content = fs::absolute(registry_.content_dir(), error);
        const fs::path file = fs::absolute(located, error);
        const std::string relative = fs::relative(file, content, error).generic_string();
        if (!error && !relative.empty() && relative.rfind("..", 0) != 0) return file.generic_string();
        fs::create_directories(textures_dir_, error);
        const fs::path out = textures_dir_ / file.filename();
        if (!fs::exists(out, error)) {
            if (!fs::copy_file(file, out, error)) {
                LSD_LOG_WARN("editor", "Could not copy %s: %s", located.c_str(), error.message().c_str());
                return {};
            }
            ++copied_;
        }
        return out.generic_string();
    }

    const asset::Image* image(const std::string& path) {
        if (path.empty()) return nullptr;
        auto& slot = images_[path];
        if (!slot) {
            slot = std::make_unique<asset::Image>();
            if (!asset::load_image(path.c_str(), *slot)) LSD_LOG_WARN("editor", "Could not read the image %s", path.c_str());
        }
        return slot->pixels ? slot.get() : nullptr;
    }

    // Maps packed into one new texture in Textures/ (overwritten: it is made from the sources).
    std::string pack(const std::string& name, const asset::PackSource& r, const asset::PackSource& g, const asset::PackSource& b, const asset::PackSource& a) {
        std::error_code error;
        fs::create_directories(textures_dir_, error);
        const fs::path out = textures_dir_ / (name + ".png");
        if (!asset::pack_channels(out.string().c_str(), r, g, b, a)) {
            LSD_LOG_WARN("editor", "Could not write %s", out.generic_string().c_str());
            return {};
        }
        ++copied_;
        return out.generic_string();
    }

    std::string guid_at(const std::string& path) {
        if (path.empty()) return {};
        std::error_code error;
        const std::string relative = fs::relative(path, registry_.content_dir(), error).generic_string();
        for (usize i = 0; i < registry_.count(); ++i) {
            if (relative == registry_.get(i).path) {
                char id[33];
                asset::to_string(registry_.get(i).guid, id);
                return id;
            }
        }
        return {};
    }

    fs::path mesh_dir() const { return mesh_dir_; }
    u32 copied() const { return copied_; }
    u32 missing() const { return missing_; }
    void free_images() { images_.clear(); }

private:
    asset::AssetRegistry& registry_;
    asset::AssetRecord mesh_;
    fs::path source_, mesh_dir_, textures_dir_;
    std::vector<fs::path> search_;
    std::map<std::string, std::unique_ptr<asset::Image>> images_;
    u32 copied_ = 0, missing_ = 0;
};

} // namespace

struct MaterialImport {
    MaterialImport(asset::AssetRegistry& r, const asset::AssetRecord& m, const char* from) : registry(r), mesh(m), file(default_allocator()), in(r, m, from) {}
    asset::AssetRegistry& registry;
    const asset::AssetRecord mesh;  // a copy: the registry's records move when it rescans
    asset::ImportedMaterials file;
    Importer in;
    fs::path materials_dir;
    std::vector<Plan> plans;
    usize next = 0;  // the next file material to plan
};

namespace {

// Pass 1 for one material: its texture files (copied or packed).
void plan_material(MaterialImport& job, const asset::ImportedMaterial& m) {
    Importer& in = job.in;
    std::error_code fs_error;
    Plan plan;
    plan.source = &m;
    const std::string name = safe_name(m.name);
    plan.material_path = (job.materials_dir / ("M_" + name + asset::kDataAssetExtension)).generic_string();
    plan.exists = fs::exists(plan.material_path, fs_error);
    if (!plan.exists) {
        LSD_LOG_INFO("editor", "Importing material %s", m.name);
        const std::string base = in.locate(m.base_color_map);
        auto find = [&](const asset::ImportedMap& map, std::initializer_list<const char*> kinds) {
            const std::string found = in.locate(map);
            return found.empty() ? in.sibling(base, kinds) : found;
        };
        const std::string opacity = find(m.opacity_map, {"Opacity", "Alpha", "Mask"});
        if (!opacity.empty()) {  // colour + its cut-out mask in alpha (Masked)
            const asset::Image* colour = in.image(base);
            const asset::Image* mask = in.image(opacity);
            asset::PackSource r{colour, 0}, g{colour, 1}, b{colour, 2}, a{mask, 0};
            plan.base_color = in.pack("T_" + name + "_BaseColor", r, g, b, a);
            plan.masked = !plan.base_color.empty() && mask;
        } else {
            plan.base_color = in.into_content(base);
        }
        plan.normal = in.into_content(find(m.normal_map, {"Normal", "Normals", "Nrm", "N"}));
        plan.emissive = in.into_content(find(m.emissive_map, {"Emissive", "Emission", "Glow"}));
        const std::string metal = find(m.metallic_map, {"Metallic", "Metalness", "Metal"});
        const std::string occlusion = find(m.occlusion_map, {"AO", "AmbientOcclusion", "Occlusion"});
        std::string rough = find(m.roughness_map, {"Roughness", "Rough"});
        bool glossy = m.roughness_is_glossiness;
        if (rough.empty()) {  // smoothness maps (Unity, older Substance presets) are inverted roughness
            rough = in.sibling(base, {"Glossiness", "Gloss", "Smoothness"});
            glossy = !rough.empty();
        }
        if (!metal.empty() || !rough.empty() || !occlusion.empty()) {  // R occlusion, G roughness, B metallic
            asset::PackSource r{in.image(occlusion), m.occlusion_map.channel};
            asset::PackSource g{in.image(rough), m.roughness_map.channel, 255, glossy};
            asset::PackSource b{in.image(metal), m.metallic_map.channel};
            asset::PackSource a{};
            plan.orm = in.pack("T_" + name + "_ORM", r, g, b, a);
            plan.metal_map = !plan.orm.empty() && b.image;
            plan.rough_map = !plan.orm.empty() && g.image;
        }
        in.free_images();  // big maps: keep only one material's in memory
    }
    job.plans.push_back(plan);
}

// Pass 2 (the Materials) and the mesh's Material Slots.
void finish(MaterialImport& job, MaterialImportReport& report, char* status, usize status_capacity) {
    asset::AssetRegistry& registry = job.registry;
    const asset::AssetRecord& mesh = job.mesh;
    std::vector<Plan>& plans = job.plans;
    registry.scan();
    // Pass 2: the Materials.
    for (Plan& plan : plans) {
        if (plan.exists) {
            ++report.reused;
            continue;
        }
        const asset::ImportedMaterial& m = *plan.source;
        render::Material material;
        const bool has_colour_map = !plan.base_color.empty();
        material.base_color = LinearColor::hex(0xFFFFFF);
        if (!has_colour_map) {  // with a map, the file's constant is usually a leftover (Blender's 0.8 grey): Unreal ignores it too
            material.base_color.r = m.base_color[0];
            material.base_color.g = m.base_color[1];
            material.base_color.b = m.base_color[2];
        }
        material.metallic = plan.metal_map ? 1.0f : m.metallic;
        const f32 roughness = m.roughness_is_glossiness ? 1.0f - m.roughness : m.roughness;
        material.roughness = plan.rough_map ? 1.0f : roughness;
        std::snprintf(material.base_color_texture, sizeof material.base_color_texture, "%s", job.in.guid_at(plan.base_color).c_str());
        std::snprintf(material.normal_texture, sizeof material.normal_texture, "%s", job.in.guid_at(plan.normal).c_str());
        std::snprintf(material.orm_texture, sizeof material.orm_texture, "%s", job.in.guid_at(plan.orm).c_str());
        std::snprintf(material.emissive_texture, sizeof material.emissive_texture, "%s", job.in.guid_at(plan.emissive).c_str());
        const f32 glow = std::max(m.emissive[0], std::max(m.emissive[1], m.emissive[2]));
        const f32 scale = glow > 1.0f ? 1.0f / glow : 1.0f;  // brighter than 1: the colour normalised, the rest as Emissive Strength
        material.emissive.r = m.emissive[0] * scale;
        material.emissive.g = m.emissive[1] * scale;
        material.emissive.b = m.emissive[2] * scale;
        if (glow > 1.0f) material.emissive_strength = glow;
        if (!plan.emissive.empty() && glow <= 0.0f) material.emissive = LinearColor::hex(0xFFFFFF);  // a map with no colour: its own colours
        if (plan.masked) material.blend_mode = 1;
        else if (m.opacity < 0.999f) {
            material.blend_mode = 2;  // Translucent
            material.opacity = m.opacity;
        }
        if (!asset::save_data_asset(plan.material_path.c_str(), reflect::type_of<render::Material>(), &material)) {
            LSD_LOG_WARN("editor", "Could not write %s", plan.material_path.c_str());
            continue;
        }
        ++report.created;
    }
    registry.scan();
    // The mesh's Material Slots: each file material to its Material (slots already set are kept).
    asset::AssetMeta meta;
    registry.load_meta(mesh, meta);
    for (const Plan& plan : plans) {
        const std::string guid = job.in.guid_at(plan.material_path);
        if (guid.empty()) continue;
        report.materials.emplace_back(plan.source->name, guid);
        asset::MaterialSlot* slot = nullptr;
        for (asset::MaterialSlot& s : meta.material_slots) slot = std::strcmp(s.name, plan.source->name) == 0 ? &s : slot;
        if (!slot && meta.material_slots.count < meta.material_slots.capacity()) {
            slot = &meta.material_slots.items[meta.material_slots.count++];
            *slot = asset::MaterialSlot{};
            std::snprintf(slot->name, sizeof slot->name, "%s", plan.source->name);
        }
        if (slot && !slot->material[0]) std::snprintf(slot->material, sizeof slot->material, "%s", guid.c_str());
    }
    if (const asset::AssetRecord* r = registry.find(mesh.guid)) registry.save_meta(*r, meta);
    report.textures = job.in.copied();
    report.missing = job.in.missing();
    std::snprintf(status, status_capacity, "%s: %u materials made, %u kept, %u textures imported%s", mesh.name, report.created, report.reused, report.textures,
                  report.missing ? " (some textures not found: see the Output Log)" : "");
    LSD_LOG_INFO("editor", "%s", status);
}

} // namespace

MaterialImport* begin_mesh_materials(asset::AssetRegistry& registry, const asset::AssetRecord& mesh_record, const char* original_folder, bool only_with_textures,
                                     char* status, usize status_capacity) {
    auto job = std::make_unique<MaterialImport>(registry, mesh_record, original_folder);
    const asset::AssetRecord& mesh = job->mesh;
    char source[300], error[256];
    registry.source_path(mesh, source, sizeof source);
    if (!asset::list_mesh_materials(source, job->file, error, sizeof error)) {
        std::snprintf(status, status_capacity, "Could not read the materials of %s: %s", mesh.path, error);
        return nullptr;
    }
    if (job->file.materials.empty()) {
        std::snprintf(status, status_capacity, "%s has no materials", mesh.path);
        return nullptr;
    }
    if (only_with_textures) {  // automatic import: only files that bring textures (not animation-only files)
        bool any = false;
        for (const asset::ImportedMaterial& m : job->file.materials) {
            for (const asset::ImportedMap* map : {&m.base_color_map, &m.normal_map, &m.metallic_map, &m.roughness_map, &m.occlusion_map, &m.opacity_map, &m.emissive_map}) {
                any = any || (map->exists() && !job->in.locate(*map, true).empty());
                if (any) break;
            }
            if (any) break;
        }
        if (!any) {
            std::snprintf(status, status_capacity, "%s: no textures found, no materials made", mesh.path);
            return nullptr;
        }
    }
    std::error_code fs_error;
    job->materials_dir = job->in.mesh_dir() / "Materials";
    fs::create_directories(job->materials_dir, fs_error);
    return job.release();
}

bool step_mesh_materials(MaterialImport* job, MaterialImportReport& report, char* status, usize status_capacity) {
    if (!job) return true;
    if (job->next < job->file.materials.size()) {
        plan_material(*job, job->file.materials[job->next++]);
        return false;
    }
    report = MaterialImportReport{};
    finish(*job, report, status, status_capacity);
    delete job;
    return true;
}

f32 mesh_materials_progress(const MaterialImport* job, char* label, usize label_capacity) {
    if (!job) return 1.0f;
    const usize total = job->file.materials.size();
    const usize at = job->next < total ? job->next : total - 1;
    std::snprintf(label, label_capacity, "Importing %s: %s (%u / %u)", job->mesh.name, job->file.materials[at].name, static_cast<u32>(at + 1), static_cast<u32>(total));
    return static_cast<f32>(job->next) / static_cast<f32>(total + 1);  // + 1: the Materials themselves
}

namespace {
struct QueuedImport {
    asset::AssetRecord mesh;
    std::string from;
    bool only_with_textures;
};
std::deque<QueuedImport> g_queue;  // shortcut: editor main thread only, like the registry it uses
MaterialImport* g_running = nullptr;
} // namespace

void queue_mesh_materials(const asset::AssetRecord& mesh, const char* original_folder, bool only_with_textures) {
    g_queue.push_back({mesh, original_folder ? original_folder : "", only_with_textures});
}

void update_mesh_materials(asset::AssetRegistry& registry, char* status, usize status_capacity, f32& progress, char* label, usize label_capacity) {
    while (!g_running && !g_queue.empty()) {  // the next one (nothing to do: its reason goes to the status line)
        const QueuedImport next = g_queue.front();
        g_queue.pop_front();
        g_running = begin_mesh_materials(registry, next.mesh, next.from.c_str(), next.only_with_textures, status, status_capacity);
        if (!g_running) LSD_LOG_INFO("editor", "%s", status);
    }
    if (!g_running) {
        progress = -1.0f;
        return;
    }
    progress = mesh_materials_progress(g_running, label, label_capacity);
    MaterialImportReport report;
    if (step_mesh_materials(g_running, report, status, status_capacity)) g_running = nullptr;
}

bool import_mesh_materials(asset::AssetRegistry& registry, const asset::AssetRecord& mesh, const char* original_folder, bool only_with_textures,
                           MaterialImportReport& report, char* status, usize status_capacity) {
    report = MaterialImportReport{};
    MaterialImport* job = begin_mesh_materials(registry, mesh, original_folder, only_with_textures, status, status_capacity);
    if (!job) return false;
    while (!step_mesh_materials(job, report, status, status_capacity)) {
    }
    return true;
}

} // namespace lsd::editor
