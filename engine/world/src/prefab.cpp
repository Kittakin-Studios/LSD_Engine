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

PrefabPathFn g_resolve = nullptr;
void* g_resolve_user = nullptr;
u32 g_nesting = 0;  // nested updates in progress: a prefab that contains itself stops here

// The first object without a parent: a loaded prefab's root.
Entity prefab_source(const World& prefab) {
    for (u32 slot = 0; slot < prefab.slot_count(); ++slot) {
        const Entity e = prefab.entity_at_slot(slot);
        if (e.valid() && !prefab.parent(e).valid()) return e;
    }
    return {};
}

// "Body/Wheel" -> "Body" ("" for a direct child).
void parent_path(const char* path, char* out, usize capacity) {
    const char* slash = std::strrchr(path, '/');
    std::snprintf(out, capacity, "%.*s", slash ? static_cast<int>(slash - path) : 0, path);
}

} // namespace

void set_prefab_resolver(PrefabPathFn resolve, void* user) {
    g_resolve = resolve;
    g_resolve_user = user;
}

bool record_structure_overrides(World& world, Entity root, const char* text, usize length) {
    auto* instance = world.get<PrefabInstance>(root);
    World prefab(default_allocator());
    if (!instance || !load_level(prefab, text, length)) return false;
    const Entity source = prefab_source(prefab);
    if (!source.valid()) return false;
    for (u32 i = instance->overrides.count; i-- > 0;) {  // structural ones are worked out afresh
        if (instance->overrides[i].kind != 0) instance->overrides.remove(i);
    }
    bool fits = true;
    auto add = [&](const char* path, const char* component, u32 kind) {
        PrefabOverride o;
        std::snprintf(o.object, sizeof o.object, "%s", path);
        std::snprintf(o.component, sizeof o.component, "%s", component);
        o.kind = kind;
        fits = instance->overrides.push_back(o) && fits;
    };
    u32 type_count = 0;
    const reflect::TypeInfo* const* types = component_types(type_count);
    // The instance's objects: new ones (kept whole), and per matching object its added / removed components.
    for (u32 slot = 0; slot < world.slot_count(); ++slot) {
        const Entity e = world.entity_at_slot(slot);
        char path[64];
        if (!e.valid() || !instance_path(world, root, e, path, sizeof path)) continue;
        const Entity match = find_in_instance(prefab, source, path);
        if (!match.valid()) {
            char above[64];
            parent_path(path, above, sizeof above);
            if (above[0] == '\0' || find_in_instance(prefab, source, above).valid()) add(path, "", 3);  // the top of an added subtree
            continue;
        }
        for (u32 t = 0; t < type_count; ++t) {
            if (keeps_own(types[t]) || (e == root && types[t] == &reflect::type_of<Transform>())) continue;
            const bool mine = world.get(e, *types[t]) != nullptr, theirs = prefab.get(match, *types[t]) != nullptr;
            if (mine && !theirs) add(path, types[t]->name, 1);
            if (!mine && theirs) add(path, types[t]->name, 2);
        }
    }
    // The prefab's objects the instance no longer has.
    for (u32 slot = 0; slot < prefab.slot_count(); ++slot) {
        const Entity e = prefab.entity_at_slot(slot);
        char path[64];
        if (!e.valid() || e == source || !instance_path(prefab, source, e, path, sizeof path) || find_in_instance(world, root, path).valid()) continue;
        char above[64];
        parent_path(path, above, sizeof above);
        if (above[0] == '\0' || find_in_instance(world, root, above).valid()) add(path, "", 4);
    }
    return fits;
}

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
    const Entity source = prefab_source(prefab);
    if (!source.valid()) return false;

    // Overridden values, kept aside while the instance is rebuilt: a property's bytes, an added
    // component's whole data, an added child's subtree (copied into `kept`).
    const auto* own = world.get<PrefabInstance>(instance);
    PrefabOverride overrides[32];
    const u32 override_count = own ? own->overrides.count : 0;
    for (u32 i = 0; i < override_count; ++i) overrides[i] = own->overrides[i];
    Array<u8> saved(default_allocator());
    Array<u32> saved_at(default_allocator());  // offset in `saved`, or ~0u when nothing was found
    World kept(default_allocator());
    Entity kept_roots[32];
    for (u32 i = 0; i < override_count; ++i) {
        const PrefabOverride& o = overrides[i];
        const Entity e = find_in_instance(world, instance, o.object);
        kept_roots[i] = o.kind == 3 && e.valid() && e != instance ? copy_entity(world, e, kept, {}, false) : Entity{};
        const reflect::TypeInfo* type = find_component_type(o.component);
        const reflect::Property* p = type && o.kind == 0 ? reflect::find_property(*type, o.property) : nullptr;
        const u32 size = p ? p->size : type && o.kind == 1 ? type->size : 0u;
        const void* data = e.valid() && size ? world.get(e, *type) : nullptr;
        saved_at.push_back(data ? static_cast<u32>(saved.size()) : ~0u);
        if (!data) continue;
        saved.resize(saved.size() + size);
        std::memcpy(saved.data() + saved.size() - size, static_cast<const u8*>(data) + (p ? p->offset : 0u), size);
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
    // The instance's own structure back on top: removed objects and components go, added ones return.
    for (u32 i = 0; i < override_count; ++i) {
        const PrefabOverride& o = overrides[i];
        const Entity e = o.kind == 3 ? Entity{} : find_in_instance(world, instance, o.object);
        const reflect::TypeInfo* type = find_component_type(o.component);
        if (o.kind == 4 && e.valid() && e != instance) world.destroy(e);
        if (o.kind == 2 && e.valid() && type && !keeps_own(type)) world.remove(e, *type);
        if (o.kind == 1 && e.valid() && type && saved_at[i] != ~0u) std::memcpy(world.add(e, *type), saved.data() + saved_at[i], type->size);
        if (o.kind == 3 && kept_roots[i].valid()) {
            char above[64];
            parent_path(o.object, above, sizeof above);
            const Entity parent = find_in_instance(world, instance, above);  // gone with the prefab: under the root
            copy_entity(kept, kept_roots[i], world, parent.valid() ? parent : instance, false);
        }
    }
    // Overridden values back on top.
    for (u32 i = 0; i < override_count; ++i) {
        const PrefabOverride& o = overrides[i];
        if (o.kind != 0 || saved_at[i] == ~0u) continue;
        const Entity e = find_in_instance(world, instance, o.object);
        const reflect::TypeInfo* type = find_component_type(o.component);
        const reflect::Property* p = reflect::find_property(*type, o.property);
        if (void* data = e.valid() ? world.get(e, *type) : nullptr) std::memcpy(static_cast<u8*>(data) + p->offset, saved.data() + saved_at[i], p->size);
    }
    // Nested prefabs inside: refreshed from their own files (the outer prefab holds a copy that may be old).
    if (g_resolve && g_nesting < 8) {
        ++g_nesting;
        Array<Entity> nested(default_allocator());
        for (u32 slot = 0; slot < world.slot_count(); ++slot) {
            const Entity e = world.entity_at_slot(slot);
            char path[64];
            if (e.valid() && e != instance && world.get<PrefabInstance>(e) && instance_path(world, instance, e, path, sizeof path)) nested.push_back(e);
        }
        for (const Entity e : nested) {
            const auto* inner = world.alive(e) ? world.get<PrefabInstance>(e) : nullptr;  // an earlier one may have replaced it
            char file[300];
            if (inner && g_resolve(g_resolve_user, inner->prefab, file, sizeof file)) update_instance_from_file(world, e, file);
        }
        --g_nesting;
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
