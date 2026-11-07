#include "lsd/asset/material_import.h"

#include <cgltf.h>
#include <stb_image.h>
#include <stb_image_write.h>
#include <ufbx.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lsd::asset {

namespace {

bool ends_with(const char* text, const char* ext) {
    const usize n = std::strlen(text), e = std::strlen(ext);
    if (n < e) return false;
    for (usize i = 0; i < e; ++i) {
        if (std::tolower(static_cast<unsigned char>(text[n - e + i])) != ext[i]) return false;
    }
    return true;
}

const char* file_part(const char* path) {
    const char* name = path;
    for (const char* c = path; *c; ++c) {
        if (*c == '/' || *c == '\\') name = c + 1;
    }
    return name;
}

// Keeps a copy of an embedded image so it outlives the scene it was read from.
void embed(ImportedMaterials& out, ImportedMap& map, const void* data, usize size, const char* name) {
    u8* copy = static_cast<u8*>(std::malloc(size));
    if (!copy) return;
    std::memcpy(copy, data, size);
    out.blobs.push_back(copy);
    map.embedded = copy;
    map.embedded_size = size;
    std::snprintf(map.embedded_name, sizeof map.embedded_name, "%s", name);
}

// A texture node to the file it reads: layered / procedural textures use their first file.
const ufbx_texture* file_texture(const ufbx_texture* t) {
    if (!t) return nullptr;
    if (t->type == UFBX_TEXTURE_FILE) return t;
    return t->file_textures.count ? t->file_textures.data[0] : nullptr;
}

void read_map(ImportedMaterials& out, ImportedMap& map, const ufbx_material_map& pbr, const ufbx_material_map& fbx) {
    const ufbx_texture* t = file_texture(pbr.texture ? pbr.texture : fbx.texture);
    if (!t) return;
    // `filename` is resolved against where the FBX is now (Content/...): the exporter's own paths come first.
    const ufbx_string& name = t->absolute_filename.length ? t->absolute_filename : t->filename.length ? t->filename : t->relative_filename;
    std::snprintf(map.path, sizeof map.path, "%s", name.data);
    std::snprintf(map.relative_path, sizeof map.relative_path, "%s", t->relative_filename.data);
    if (t->content.size) embed(out, map, t->content.data, t->content.size, file_part(name.data));
}

bool list_fbx(const char* path, ImportedMaterials& out, char* error, usize error_capacity) {
    ufbx_load_opts opts{};
    opts.ignore_geometry = true;  // materials only: skip the vertices
    opts.ignore_animation = true;
    ufbx_error ufbx_err{};
    ufbx_scene* scene = ufbx_load_file(path, &opts, &ufbx_err);
    if (!scene) {
        std::snprintf(error, error_capacity, "FBX: %s", ufbx_err.description.data);
        return false;
    }
    for (usize i = 0; i < scene->materials.count; ++i) {
        const ufbx_material* mat = scene->materials.data[i];
        ImportedMaterial m;
        std::snprintf(m.name, sizeof m.name, "%s", mat->name.data);
        const ufbx_vec4 c = mat->pbr.base_color.value_vec4;
        const f32 factor = mat->pbr.base_factor.has_value ? static_cast<f32>(mat->pbr.base_factor.value_real) : 1.0f;
        m.base_color[0] = static_cast<f32>(c.x) * factor;
        m.base_color[1] = static_cast<f32>(c.y) * factor;
        m.base_color[2] = static_cast<f32>(c.z) * factor;
        m.metallic = static_cast<f32>(mat->pbr.metalness.value_real);
        if (mat->pbr.roughness.has_value) m.roughness = static_cast<f32>(mat->pbr.roughness.value_real);
        if (mat->pbr.opacity.has_value) m.opacity = static_cast<f32>(mat->pbr.opacity.value_real);
        m.roughness_is_glossiness = mat->features.roughness_as_glossiness.enabled;
        const ufbx_material_map none{};
        read_map(out, m.base_color_map, mat->pbr.base_color, mat->fbx.diffuse_color);
        read_map(out, m.normal_map, mat->pbr.normal_map, mat->fbx.normal_map.texture ? mat->fbx.normal_map : mat->fbx.bump);
        read_map(out, m.metallic_map, mat->pbr.metalness, mat->fbx.reflection_factor);
        read_map(out, m.roughness_map, mat->pbr.roughness, none);
        read_map(out, m.occlusion_map, mat->pbr.ambient_occlusion, none);
        read_map(out, m.opacity_map, mat->pbr.opacity, mat->fbx.transparency_factor.texture ? mat->fbx.transparency_factor : mat->fbx.transparency_color);
        // Blender writes the colour map into the transparency slot when its alpha is used: not a separate mask.
        if (m.opacity_map.exists() && std::strcmp(m.opacity_map.path, m.base_color_map.path) == 0) m.opacity_map = ImportedMap{};
        out.materials.push_back(m);
    }
    ufbx_free_scene(scene);
    return true;
}

void read_map(ImportedMaterials& out, ImportedMap& map, const cgltf_texture* t, u32 channel) {
    if (!t || !t->image) return;
    map.channel = channel;
    const cgltf_image& image = *t->image;
    if (image.uri && std::strncmp(image.uri, "data:", 5) != 0) {
        std::snprintf(map.path, sizeof map.path, "%s", image.uri);  // relative to the .gltf
        cgltf_decode_uri(map.path);
    } else if (image.buffer_view && image.buffer_view->buffer->data) {  // a GLB's own images
        const u8* bytes = static_cast<const u8*>(image.buffer_view->buffer->data) + image.buffer_view->offset;
        char name[96];
        std::snprintf(name, sizeof name, "%s.%s", image.name && image.name[0] ? image.name : "Texture",
                      image.mime_type && std::strstr(image.mime_type, "jpeg") ? "jpg" : "png");
        embed(out, map, bytes, image.buffer_view->size, name);
    }
    // shortcut: base64 data: URIs inside a .gltf are not read; export a .glb or separate images
}

bool list_gltf(const char* path, ImportedMaterials& out, char* error, usize error_capacity) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path, &data) != cgltf_result_success || cgltf_load_buffers(&options, data, path) != cgltf_result_success) {
        std::snprintf(error, error_capacity, "glTF: could not read %s", path);
        cgltf_free(data);
        return false;
    }
    for (cgltf_size i = 0; i < data->materials_count; ++i) {
        const cgltf_material& mat = data->materials[i];
        ImportedMaterial m;
        std::snprintf(m.name, sizeof m.name, "%s", mat.name ? mat.name : "Material");
        const cgltf_pbr_metallic_roughness& pbr = mat.pbr_metallic_roughness;
        std::memcpy(m.base_color, pbr.base_color_factor, sizeof m.base_color);
        m.metallic = pbr.metallic_factor;
        m.roughness = pbr.roughness_factor;
        read_map(out, m.base_color_map, pbr.base_color_texture.texture, 0);
        read_map(out, m.normal_map, mat.normal_texture.texture, 0);
        read_map(out, m.roughness_map, pbr.metallic_roughness_texture.texture, 1);
        read_map(out, m.metallic_map, pbr.metallic_roughness_texture.texture, 2);
        read_map(out, m.occlusion_map, mat.occlusion_texture.texture, 0);
        out.materials.push_back(m);
    }
    cgltf_free(data);
    return true;
}

} // namespace

ImportedMaterials::~ImportedMaterials() {
    for (u8* blob : blobs) std::free(blob);
}

bool list_mesh_materials(const char* path, ImportedMaterials& out, char* error, usize error_capacity) {
    out.materials.clear();
    if (error_capacity) error[0] = '\0';
    if (ends_with(path, ".fbx") || ends_with(path, ".obj")) return list_fbx(path, out, error, error_capacity);  // ufbx: OBJ and its .mtl
    if (ends_with(path, ".gltf") || ends_with(path, ".glb")) return list_gltf(path, out, error, error_capacity);
    std::snprintf(error, error_capacity, "Not a mesh file: %s", path);
    return false;
}

Image::~Image() { stbi_image_free(pixels); }

bool load_image(const char* path, Image& out) {
    int w = 0, h = 0, channels = 0;
    stbi_image_free(out.pixels);
    out.pixels = stbi_load(path, &w, &h, &channels, 4);
    out.width = static_cast<u32>(w);
    out.height = static_cast<u32>(h);
    return out.pixels != nullptr;
}

bool load_image(const u8* bytes, usize size, Image& out) {
    int w = 0, h = 0, channels = 0;
    stbi_image_free(out.pixels);
    out.pixels = stbi_load_from_memory(bytes, static_cast<int>(size), &w, &h, &channels, 4);
    out.width = static_cast<u32>(w);
    out.height = static_cast<u32>(h);
    return out.pixels != nullptr;
}

bool pack_channels(const char* out_png, const PackSource& r, const PackSource& g, const PackSource& b, const PackSource& a) {
    const PackSource* sources[4] = {&r, &g, &b, &a};
    u32 width = 1, height = 1;
    for (const PackSource* s : sources) {
        if (!s->image || !s->image->pixels) continue;
        width = s->image->width > width ? s->image->width : width;
        height = s->image->height > height ? s->image->height : height;
    }
    Array<u8> pixels(default_allocator());
    pixels.resize(static_cast<usize>(width) * height * 4);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            u8* out = &pixels[(static_cast<usize>(y) * width + x) * 4];
            for (u32 c = 0; c < 4; ++c) {
                const PackSource& s = *sources[c];
                u8 v = s.constant;
                if (s.image && s.image->pixels) {
                    const u32 sx = static_cast<u32>(static_cast<u64>(x) * s.image->width / width);
                    const u32 sy = static_cast<u32>(static_cast<u64>(y) * s.image->height / height);
                    v = s.image->pixels[(static_cast<usize>(sy) * s.image->width + sx) * 4 + (s.channel & 3u)];
                }
                out[c] = s.invert ? static_cast<u8>(255 - v) : v;
            }
        }
    }
    stbi_write_png_compression_level = 2;  // big maps: faster to write, a little larger on disk
    return stbi_write_png(out_png, static_cast<int>(width), static_cast<int>(height), 4, pixels.data(), static_cast<int>(width * 4)) != 0;
}

} // namespace lsd::asset
