#include "lsd/world/level.h"

#include "lsd/foundation/log.h"
#include "lsd/reflect/json.h"

#include <cstdio>
#include <cstring>

namespace lsd::world {

namespace {

constexpr int kLevelVersion = 1;

void append(Array<char>& out, const char* text, usize n) {
    const usize at = out.size();
    out.resize(at + n);
    std::memcpy(out.data() + at, text, n);
}

void append(Array<char>& out, const char* text) { append(out, text, std::strlen(text)); }

void append_string(Array<char>& out, const char* text) {
    append(out, "\"");
    for (const char* c = text; *c; ++c) {
        if (*c == '"' || *c == '\\') append(out, "\\", 1);
        if (static_cast<unsigned char>(*c) >= 0x20) append(out, c, 1);
    }
    append(out, "\"");
}

void append_component(Array<char>& out, const reflect::TypeInfo& type, const void* component) {
    char buffer[2048];
    const usize length = reflect::save_text(type, component, buffer, sizeof buffer, 2);
    if (length < sizeof buffer) {
        append(out, buffer, length);
        return;
    }
    Allocator& heap = default_allocator();  // rare: a component bigger than the stack buffer
    char* big = static_cast<char*>(heap.allocate(length + 1, 1));
    reflect::save_text(type, component, big, length + 1, 2);
    append(out, big, length);
    heap.deallocate(big);
}

// Reads an integer token (ids and parents).
bool read_integer(reflect::JsonReader& r, long long& out) {
    char token[32];
    r.read_token(token, sizeof token);
    char* stop = nullptr;
    out = std::strtoll(token, &stop, 10);
    if (r.ok && *stop) r.fail("expected an integer");
    return r.ok;
}

bool in_transient(const World& world, Entity e) {  // it or an ancestor is a Sub Level's content
    for (; e.valid(); e = world.parent(e)) {
        if (world.transient(e)) return true;
    }
    return false;
}

} // namespace

void save_level(const World& world, Array<char>& out) {
    out.clear();
    char line[128];
    std::snprintf(line, sizeof line, "{\n  \"lsd_level\": %d", kLevelVersion);
    append(out, line);
    append(out, ",\n  \"world_settings\": ");
    append_component(out, reflect::type_of<WorldSettings>(), &world.settings());
    append(out, ",\n  \"entities\": [");
    u32 type_count = 0;
    const reflect::TypeInfo* const* types = component_types(type_count);
    bool first = true;
    for (u32 slot = 0; slot < world.slot_count(); ++slot) {
        const Entity e = world.entity_at_slot(slot);
        if (!e.valid() || in_transient(world, e)) continue;  // Sub Level contents live in their own file
        const Entity parent = world.parent(e);
        std::snprintf(line, sizeof line, "%s\n    {\"id\": %u, \"name\": ", first ? "" : ",", e.value);
        append(out, line);
        append_string(out, world.name(e));
        std::snprintf(line, sizeof line, ", \"parent\": %lld", parent.valid() ? static_cast<long long>(parent.value) : -1LL);
        append(out, line);
        if (world.hidden(e)) append(out, ", \"hidden\": true");
        if (world.locked(e)) append(out, ", \"locked\": true");
        if (world.folder(e)[0]) {
            append(out, ", \"folder\": ");
            append_string(out, world.folder(e));
        }
        for (u32 t = 0; t < type_count; ++t) {
            const void* component = world.get(e, *types[t]);
            if (!component) continue;
            std::snprintf(line, sizeof line, ",\n     \"%s\": ", types[t]->name);
            append(out, line);
            append_component(out, *types[t], component);
        }
        append(out, "}");
        first = false;
    }
    append(out, "\n  ]\n}\n");
}

bool load_level(World& world, const char* text, usize length) {
    world.clear();
    Allocator& heap = default_allocator();
    struct Link { Entity child; long long parent; };
    Array<Link> links(heap);
    reflect::JsonReader r{text, text + length};

    r.expect('{');
    while (r.ok && !r.peek('}')) {
        char key[64];
        r.read_string(key, sizeof key);
        r.expect(':');
        if (std::strcmp(key, "lsd_level") == 0) {
            long long version = 0;
            if (read_integer(r, version) && version > kLevelVersion) r.fail("level saved by a newer editor");
        } else if (std::strcmp(key, "world_settings") == 0) {
            reflect::read_object(r, reflect::type_of<WorldSettings>(), &world.settings());
        } else if (std::strcmp(key, "entities") == 0) {
            r.expect('[');
            while (r.ok && !r.peek(']')) {
                // Entity object. "id" and "name" come first (save_level writes them first).
                Entity e;
                char name[64] = "";
                r.expect('{');
                while (r.ok && !r.peek('}')) {
                    char field[64];
                    r.read_string(field, sizeof field);
                    r.expect(':');
                    long long value = 0;
                    if (std::strcmp(field, "id") == 0) {
                        if (read_integer(r, value)) e.value = static_cast<u32>(value);
                    } else if (std::strcmp(field, "name") == 0) {
                        r.read_string(name, sizeof name);
                        if (!world.create_at(e, name)) r.fail("missing or duplicate entity id");
                    } else if (std::strcmp(field, "hidden") == 0 || std::strcmp(field, "locked") == 0) {
                        char token[8];
                        r.read_token(token, sizeof token);
                        const bool on = std::strcmp(token, "true") == 0;
                        if (field[0] == 'h') world.set_hidden(e, on);
                        else world.set_locked(e, on);
                    } else if (std::strcmp(field, "folder") == 0) {
                        char folder[64] = "";
                        r.read_string(folder, sizeof folder);
                        world.set_folder(e, folder);
                    } else if (std::strcmp(field, "parent") == 0) {
                        if (read_integer(r, value) && value >= 0) links.push_back({e, value});
                    } else if (const reflect::TypeInfo* type = find_component_type(field); type && world.alive(e)) {
                        reflect::read_object(r, *type, world.add(e, *type));
                    } else {
                        r.skip_value();  // unknown component (removed from the engine): dropped
                    }
                    if (!r.accept(',')) break;
                }
                r.expect('}');
                if (!r.accept(',')) break;
            }
            r.expect(']');
        } else {
            r.skip_value();
        }
        if (!r.accept(',')) break;
    }
    r.expect('}');

    for (const Link& link : links) {
        Entity parent;
        parent.value = static_cast<u32>(link.parent);
        // Raw link, not set_parent(): the saved Transform is already relative to the parent.
        if (!world.alive(parent) || !world.set_parent(link.child, parent, false)) LSD_LOG_WARN("world", "Dropped a bad parent link in the level");
    }
    if (!r.ok) world.clear();
    return r.ok;
}

StreamWant streaming_volumes_want(const World& world, const char* name, Vec3 point, bool loaded) {
    const reflect::TypeInfo& type = reflect::type_of<LevelStreamingVolume>();
    StreamWant want = StreamWant::None;
    for (u32 i = 0; i < world.count(type); ++i) {
        const auto* volume = static_cast<const LevelStreamingVolume*>(world.at(type, i));
        bool listed = false;
        for (u32 k = 0; k < volume->sub_levels.count && !listed; ++k) listed = std::strcmp(volume->sub_levels[k].name, name) == 0;
        if (!volume->enabled || !listed) continue;
        // The volume's unit box (x, y -0.5..0.5, z 0..1) under its transform, grown by the margin while loaded.
        const Mat4 m = world.world_matrix(world.owner(type, i));
        const Vec3 p = transform_point(affine_inverse(m), point);
        const f32 margin = loaded ? kStreamingMargin : 0.0f;
        const auto grow = [&](u32 column) {
            const f32 scale = length(Vec3{m.at(0, column), m.at(1, column), m.at(2, column)});
            return scale > 0.0f ? margin / scale : 0.0f;
        };
        const f32 gx = grow(0), gy = grow(1), gz = grow(2);
        if (p.x >= -0.5f - gx && p.x <= 0.5f + gx && p.y >= -0.5f - gy && p.y <= 0.5f + gy && p.z >= -gz && p.z <= 1.0f + gz) return StreamWant::Load;
        want = StreamWant::Unload;
    }
    return want;
}

bool sublevel_loaded(const World& world, Entity root) {
    for (u32 slot = 0; slot < world.slot_count(); ++slot) {
        const Entity e = world.entity_at_slot(slot);
        if (e.valid() && world.transient(e) && world.parent(e) == root) return true;
    }
    return false;
}

void unload_sublevel(World& world, Entity root) {
    for (u32 slot = 0; slot < world.slot_count(); ++slot) {
        const Entity e = world.entity_at_slot(slot);
        if (e.valid() && world.transient(e) && world.parent(e) == root) world.destroy(e);  // its children go with it
    }
}

bool load_sublevel(World& world, Entity root, const char* path) {
    if (!world.alive(root)) return false;
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    Allocator& heap = default_allocator();
    Array<char> text(heap);
    char chunk[8192];
    for (usize n; (n = std::fread(chunk, 1, sizeof chunk, file)) > 0;) {
        const usize at = text.size();
        text.resize(at + n);
        std::memcpy(text.data() + at, chunk, n);
    }
    std::fclose(file);
    World source(heap);
    if (!load_level(source, text.data(), text.size())) return false;
    unload_sublevel(world, root);
    // Copy every object (components are plain data), then rebuild the hierarchy under `root`.
    u32 type_count = 0;
    const reflect::TypeInfo* const* types = component_types(type_count);
    Array<Entity> copies(heap);
    copies.resize(source.slot_count());
    for (u32 slot = 0; slot < source.slot_count(); ++slot) {
        const Entity from = source.entity_at_slot(slot);
        copies[slot] = {};
        if (!from.valid()) continue;
        const Entity to = world.create(source.name(from));
        for (u32 t = 0; t < type_count; ++t) {
            const void* component = source.get(from, *types[t]);
            if (!component) continue;
            void* copy = world.add(to, *types[t]);
            if (copy) std::memcpy(copy, component, types[t]->size);
        }
        world.set_hidden(to, source.hidden(from));
        world.set_locked(to, true);
        copies[slot] = to;
    }
    for (u32 slot = 0; slot < source.slot_count(); ++slot) {
        const Entity from = source.entity_at_slot(slot);
        if (!from.valid() || !copies[slot].valid()) continue;
        const Entity parent = source.parent(from);
        const Entity new_parent = parent.valid() && copies[parent.index()].valid() ? copies[parent.index()] : root;
        world.set_parent(copies[slot], new_parent, false);  // local transforms: the level moves with `root`
        if (new_parent == root) world.set_transient(copies[slot], true);  // its top objects mark the whole branch
    }
    return true;
}

u32 destroy_editor_only(World& world) {
    const reflect::TypeInfo& type = reflect::type_of<EditorOnly>();
    u32 gone = 0;
    while (world.count(type)) {  // destroying shifts the pool: take the first each time
        world.destroy(world.owner(type, 0));
        ++gone;
    }
    return gone;
}

bool save_level_file(const World& world, const char* path) {
    Array<char> text(default_allocator());
    save_level(world, text);
    FILE* file = std::fopen(path, "wb");
    if (!file) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    return std::fclose(file) == 0 && ok;
}

bool load_level_file(World& world, const char* path) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    Array<char> text(default_allocator());
    char chunk[8192];
    for (usize n; (n = std::fread(chunk, 1, sizeof chunk, file)) > 0;) append(text, chunk, n);
    std::fclose(file);
    const bool ok = load_level(world, text.data(), text.size());
    if (!ok) LSD_LOG_ERROR("world", "Could not load level %s", path);
    return ok;
}

} // namespace lsd::world
