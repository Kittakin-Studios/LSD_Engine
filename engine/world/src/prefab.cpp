#include "lsd/world/prefab.h"

#include "lsd/world/level.h"

#include <cstdio>
#include <cstring>

namespace lsd::world {

namespace {

bool read_file(const char* path, Array<char>& out) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    char chunk[8192];
    for (usize n; (n = std::fread(chunk, 1, sizeof chunk, file)) > 0;) {
        const usize at = out.size();
        out.resize(at + n);
        std::memcpy(out.data() + at, chunk, n);
    }
    std::fclose(file);
    return true;
}

bool keeps_own(const reflect::TypeInfo* type) {
    return type == &reflect::type_of<Transform>() || type == &reflect::type_of<PrefabInstance>();
}

} // namespace

void save_prefab(const World& world, Entity root, Array<char>& out) {
    World prefab(default_allocator());
    const Entity copy = copy_entity(world, root, prefab, {});
    prefab.get<Transform>(copy)->position = {};
    prefab.remove(copy, reflect::type_of<PrefabInstance>());  // and with it the overrides: applied, they are the prefab now
    save_level(prefab, out);
}

bool save_prefab_file(const World& world, Entity root, const char* path) {
    Array<char> text(default_allocator());
    save_prefab(world, root, text);
    FILE* file = std::fopen(path, "wb");
    if (!file) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    return std::fclose(file) == 0 && ok;
}

bool update_instance(World& world, Entity instance, const char* text, usize length, bool new_placement) {
    World prefab(default_allocator());
    if (!world.alive(instance) || !load_level(prefab, text, length)) return false;
    Entity source;
    for (u32 slot = 0; slot < prefab.slot_count() && !source.valid(); ++slot) {
        const Entity e = prefab.entity_at_slot(slot);
        if (e.valid() && !prefab.parent(e).valid()) source = e;
    }
    if (!source.valid()) return false;

    // Overridden values, kept aside while the instance is rebuilt.
    const auto* own = world.get<PrefabInstance>(instance);
    PrefabOverride overrides[32];
    const u32 override_count = own ? own->overrides.count : 0;
    for (u32 i = 0; i < override_count; ++i) overrides[i] = own->overrides[i];
    Array<u8> saved(default_allocator());
    Array<u32> saved_at(default_allocator());  // offset in `saved`, or ~0u when the value was not found
    for (u32 i = 0; i < override_count; ++i) {
        const PrefabOverride& o = overrides[i];
        const Entity e = find_in_instance(world, instance, o.object);
        const reflect::TypeInfo* type = find_component_type(o.component);
        const reflect::Property* p = type ? reflect::find_property(*type, o.property) : nullptr;
        const void* data = e.valid() && p ? world.get(e, *type) : nullptr;
        saved_at.push_back(data ? static_cast<u32>(saved.size()) : ~0u);
        if (!data) continue;
        saved.resize(saved.size() + p->size);
        std::memcpy(saved.data() + saved.size() - p->size, static_cast<const u8*>(data) + p->offset, p->size);
    }

    // Old children out (collected first: destroy() frees slots while we scan).
    Array<Entity> children(default_allocator());
    for (u32 slot = 0; slot < world.slot_count(); ++slot) {
        const Entity e = world.entity_at_slot(slot);
        if (e.valid() && world.parent(e) == instance) children.push_back(e);
    }
    for (const Entity child : children) world.destroy(child);

    u32 type_count = 0;
    const reflect::TypeInfo* const* types = component_types(type_count);
    for (u32 t = 0; t < type_count; ++t) {
        if (keeps_own(types[t])) continue;
        if (const void* data = prefab.get(source, *types[t])) std::memcpy(world.add(instance, *types[t]), data, types[t]->size);
        else world.remove(instance, *types[t]);
    }
    if (new_placement) {
        Transform& placed = *world.get<Transform>(instance);
        placed.rotation = prefab.get<Transform>(source)->rotation;
        placed.scale = prefab.get<Transform>(source)->scale;
    }
    for (u32 slot = 0; slot < prefab.slot_count(); ++slot) {
        const Entity child = prefab.entity_at_slot(slot);
        if (child.valid() && prefab.parent(child) == source) copy_entity(prefab, child, world, instance, false);  // prefab names: override paths use them
    }
    // Overridden values back on top.
    for (u32 i = 0; i < override_count; ++i) {
        if (saved_at[i] == ~0u) continue;
        const PrefabOverride& o = overrides[i];
        const Entity e = find_in_instance(world, instance, o.object);
        const reflect::TypeInfo* type = find_component_type(o.component);
        const reflect::Property* p = reflect::find_property(*type, o.property);
        if (void* data = e.valid() ? world.get(e, *type) : nullptr) std::memcpy(static_cast<u8*>(data) + p->offset, saved.data() + saved_at[i], p->size);
    }
    return true;
}

bool instance_path(const World& world, Entity root, Entity object, char* out, usize capacity) {
    char path[256] = "";
    for (Entity e = object; e.valid(); e = world.parent(e)) {
        if (e == root) {
            std::snprintf(out, capacity, "%s", path);
            return true;
        }
        char longer[256];
        std::snprintf(longer, sizeof longer, path[0] ? "%s/%s" : "%s%s", world.name(e), path);
        std::snprintf(path, sizeof path, "%s", longer);
    }
    return false;
}

Entity find_in_instance(const World& world, Entity root, const char* path) {
    Entity at = root;
    while (*path && at.valid()) {
        const char* end = std::strchr(path, '/');
        const usize n = end ? static_cast<usize>(end - path) : std::strlen(path);
        Entity next;
        for (u32 slot = 0; slot < world.slot_count() && !next.valid(); ++slot) {
            const Entity e = world.entity_at_slot(slot);
            if (e.valid() && world.parent(e) == at && std::strlen(world.name(e)) == n && std::strncmp(world.name(e), path, n) == 0) next = e;
        }
        at = next;
        path += n + (end ? 1 : 0);
    }
    return at;
}

bool is_overridden(const World& world, Entity root, const char* path, const char* component, const char* property) {
    const auto* instance = world.get<PrefabInstance>(root);
    if (!instance) return false;
    for (const PrefabOverride& o : instance->overrides) {
        if (std::strcmp(o.object, path) == 0 && std::strcmp(o.component, component) == 0 && std::strcmp(o.property, property) == 0) return true;
    }
    return false;
}

bool set_override(World& world, Entity root, const char* path, const char* component, const char* property, bool on) {
    auto* instance = world.get<PrefabInstance>(root);
    if (!instance) return false;
    for (u32 i = 0; i < instance->overrides.count; ++i) {
        const PrefabOverride& o = instance->overrides[i];
        if (std::strcmp(o.object, path) == 0 && std::strcmp(o.component, component) == 0 && std::strcmp(o.property, property) == 0) {
            if (!on) instance->overrides.remove(i);
            return true;
        }
    }
    if (!on) return true;
    PrefabOverride o;
    std::snprintf(o.object, sizeof o.object, "%s", path);
    std::snprintf(o.component, sizeof o.component, "%s", component);
    std::snprintf(o.property, sizeof o.property, "%s", property);
    return instance->overrides.push_back(o);
}

bool update_instance_from_file(World& world, Entity instance, const char* path, bool new_placement) {
    Array<char> text(default_allocator());
    return read_file(path, text) && update_instance(world, instance, text.data(), text.size(), new_placement);
}

} // namespace lsd::world
