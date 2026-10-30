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
    prefab.remove(copy, reflect::type_of<PrefabInstance>());
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
        if (child.valid() && prefab.parent(child) == source) copy_entity(prefab, child, world, instance);
    }
    return true;
}

bool update_instance_from_file(World& world, Entity instance, const char* path, bool new_placement) {
    Array<char> text(default_allocator());
    return read_file(path, text) && update_instance(world, instance, text.data(), text.size(), new_placement);
}

} // namespace lsd::world
