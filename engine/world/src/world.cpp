#include "lsd/world/world.h"
#include "lsd/world/components.h"

#include <cstdio>
#include <cstring>

namespace lsd::world {

namespace {
// The built-in types, then those game code registers (register_component_type).
const reflect::TypeInfo* g_types[kMaxComponentTypes];
u32 g_type_count = 0;

void init_types() {
    if (g_type_count) return;
    static const reflect::TypeInfo* const types[] = {
        &reflect::type_of<Transform>(),
        &reflect::type_of<StaticMesh>(),
        &reflect::type_of<SkeletalMesh>(),
        &reflect::type_of<PlayerStart>(),
        &reflect::type_of<BlockingVolume>(),
        &reflect::type_of<KillVolume>(),
        &reflect::type_of<TriggerVolume>(),
        &reflect::type_of<SoundEmitter>(),
        &reflect::type_of<ParticleEffect>(),
        &reflect::type_of<AudioVolume>(),
        &reflect::type_of<DirectionalLight>(),
        &reflect::type_of<PointLight>(),
        &reflect::type_of<SpotLight>(),
        &reflect::type_of<SkyLight>(),
        &reflect::type_of<Sky>(),
        &reflect::type_of<HeightFog>(),
        &reflect::type_of<PostProcessVolume>(),
        &reflect::type_of<LightmassImportanceVolume>(),
        &reflect::type_of<NavMeshBounds>(), &reflect::type_of<NavModifierVolume>(), &reflect::type_of<CullDistanceVolume>(), &reflect::type_of<ReflectionCapture>(), &reflect::type_of<WaterVolume>(), &reflect::type_of<SplineMeshes>(),
        &reflect::type_of<NavAgent>(),
        &reflect::type_of<AIController>(),
        &reflect::type_of<RigidBody>(),
        &reflect::type_of<PhysicsJoint>(),
        &reflect::type_of<Character>(),
        &reflect::type_of<PrefabInstance>(),
        &reflect::type_of<Tags>(),
        &reflect::type_of<Script>(),
        &reflect::type_of<NativeScript>(),
        &reflect::type_of<Spline>(),
        &reflect::type_of<SplineFollower>(),
        &reflect::type_of<Camera>(),
        &reflect::type_of<RotatingMovement>(),
        &reflect::type_of<ProjectileMovement>(),
        &reflect::type_of<FootIK>(),
        &reflect::type_of<InstancedStaticMesh>(),
        &reflect::type_of<Decal>(),
        &reflect::type_of<AttachToSocket>(),
        &reflect::type_of<Landscape>(),
        &reflect::type_of<PcgVolume>(),
        &reflect::type_of<Sprite>(),
        &reflect::type_of<TileMap>(),
        &reflect::type_of<NetReplicated>(),
        &reflect::type_of<Mover>(),
        &reflect::type_of<Vehicle>(),
        &reflect::type_of<Transport>(),
        &reflect::type_of<SubLevel>(),
        &reflect::type_of<Outfit>(),
        &reflect::type_of<OutfitPiece>(),
    };
    static_assert(sizeof types / sizeof types[0] <= kMaxComponentTypes);
    for (const reflect::TypeInfo* t : types) g_types[g_type_count++] = t;
}
} // namespace

const reflect::TypeInfo* const* component_types(u32& count) {
    init_types();
    count = g_type_count;
    return g_types;
}

bool register_component_type(const reflect::TypeInfo& type) {
    init_types();
    for (u32 i = 0; i < g_type_count; ++i) {
        if (g_types[i] == &type) return true;
        if (std::strcmp(g_types[i]->name, type.name) == 0) return false;  // the name is taken
    }
    if (g_type_count == kMaxComponentTypes) return false;
    g_types[g_type_count++] = &type;
    return true;
}

const reflect::TypeInfo* find_component_type(const char* name) {
    u32 count = 0;
    const reflect::TypeInfo* const* types = component_types(count);
    for (u32 i = 0; i < count; ++i) {
        if (std::strcmp(types[i]->name, name) == 0) return types[i];
    }
    return nullptr;
}

World::World(Allocator& allocator) : allocator_(allocator), records_(allocator), free_slots_(allocator) {
    const reflect::TypeInfo* const* types = component_types(pool_count_);
    for (u32 i = 0; i < pool_count_; ++i) {
        pools_[i].type = types[i];
        pools_[i].stride = (types[i]->size + 15u) & ~15u;
    }
}

World::~World() {
    for (u32 i = 0; i < pool_count_; ++i) {
        if (pools_[i].data) allocator_.deallocate(pools_[i].data);
        if (pools_[i].owners) allocator_.deallocate(pools_[i].owners);
    }
}

World::Record* World::record(Entity e) {
    if (!e.valid() || e.index() >= records_.size()) return nullptr;
    Record& r = records_[e.index()];
    return r.alive && r.generation == e.generation() ? &r : nullptr;
}

const World::Record* World::record(Entity e) const { return const_cast<World*>(this)->record(e); }

bool World::alive(Entity e) const { return record(e) != nullptr; }

i32 World::pool_index(const reflect::TypeInfo& type) const {
    for (u32 i = 0; i < pool_count_; ++i) {
        if (pools_[i].type == &type) return static_cast<i32>(i);
    }
    // A type game code registered after this world was made: its pool is added now (empty until used).
    u32 count = 0;
    const reflect::TypeInfo* const* types = component_types(count);
    for (u32 i = pool_count_; i < count; ++i) {
        pools_[i].type = types[i];
        pools_[i].stride = (types[i]->size + 15u) & ~15u;
        pool_count_ = i + 1;
        if (types[i] == &type) return static_cast<i32>(i);
    }
    return -1;
}

void World::init_record(u32 slot, u32 generation, const char* name) {
    Record& r = records_[slot];
    r.generation = generation;
    r.alive = true;
    r.hidden = r.locked = r.transient = false;
    std::snprintf(r.name, sizeof r.name, "%s", name);
    r.folder[0] = '\0';
    r.parent = {};
    for (u32& c : r.component) c = kNone;
    add(Entity::make(slot, generation), reflect::type_of<Transform>());
}

Entity World::create(const char* name) {
    u32 slot;
    if (!free_slots_.empty()) {
        slot = free_slots_[free_slots_.size() - 1];
        free_slots_.resize(free_slots_.size() - 1);
    } else {
        slot = static_cast<u32>(records_.size());
        records_.resize(records_.size() + 1);
        records_[slot].generation = 0;
    }
    const u32 generation = records_[slot].generation & Entity::kGenerationMask;
    init_record(slot, generation, name);
    return Entity::make(slot, generation);
}

bool World::create_at(Entity handle, const char* name) {
    if (!handle.valid()) return false;
    const u32 slot = handle.index();
    while (records_.size() <= slot) {
        records_.resize(records_.size() + 1);
        Record& r = records_[records_.size() - 1];
        r.generation = 0;
        r.alive = false;
        free_slots_.push_back(static_cast<u32>(records_.size() - 1));
    }
    if (records_[slot].alive) return false;
    for (usize i = 0; i < free_slots_.size(); ++i) {
        if (free_slots_[i] == slot) {
            free_slots_.remove_swap(i);
            break;
        }
    }
    init_record(slot, handle.generation(), name);
    return true;
}

void World::destroy(Entity e) {
    Record* r = record(e);
    if (!r) return;
    // shortcut: children found by scanning all slots; keep a child list when worlds reach many thousands.
    for (u32 slot = 0; slot < records_.size(); ++slot) {
        if (records_[slot].alive && records_[slot].parent == e) destroy(Entity::make(slot, records_[slot].generation));
    }
    r = record(e);
    for (u32 p = 0; p < pool_count_; ++p) remove_component(*r, p);
    r->alive = false;
    r->generation = (r->generation + 1) & Entity::kGenerationMask;
    free_slots_.push_back(e.index());
}

void World::clear() {
    settings_ = WorldSettings{};
    records_.clear();
    free_slots_.clear();
    for (u32 p = 0; p < pool_count_; ++p) pools_[p].count = 0;
}

const char* World::name(Entity e) const {
    const Record* r = record(e);
    return r ? r->name : "";
}

void World::set_name(Entity e, const char* name) {
    if (Record* r = record(e)) std::snprintf(r->name, sizeof r->name, "%s", name);
}

void World::unique_name(const char* base, char* out, usize capacity) const {
    std::snprintf(out, capacity, "%s", base);
    for (int n = 2;; ++n) {
        bool taken = false;
        for (const Record& r : records_) taken = taken || (r.alive && std::strcmp(r.name, out) == 0);
        if (!taken) return;
        std::snprintf(out, capacity, "%s%d", base, n);
    }
}

bool World::hidden(Entity e) const {
    const Record* r = record(e);
    return r && r->hidden;
}

bool World::locked(Entity e) const {
    const Record* r = record(e);
    return r && r->locked;
}

void World::set_hidden(Entity e, bool value) {
    if (Record* r = record(e)) r->hidden = value;
}

void World::set_locked(Entity e, bool value) {
    if (Record* r = record(e)) r->locked = value;
}

bool World::transient(Entity e) const {
    const Record* r = record(e);
    return r && r->transient;
}

void World::set_transient(Entity e, bool value) {
    if (Record* r = record(e)) r->transient = value;
}

const char* World::folder(Entity e) const {
    const Record* r = record(e);
    return r ? r->folder : "";
}

void World::set_folder(Entity e, const char* folder) {
    if (Record* r = record(e)) std::snprintf(r->folder, sizeof r->folder, "%s", folder ? folder : "");
}

bool World::hidden_in_hierarchy(Entity e) const {
    for (; e.valid(); e = parent(e)) {
        if (hidden(e)) return true;
    }
    return false;
}

Entity World::parent(Entity e) const {
    const Record* r = record(e);
    return r && alive(r->parent) ? r->parent : Entity{};
}

bool World::set_parent(Entity child, Entity new_parent, bool keep_world_placement) {
    Record* r = record(child);
    if (!r || (new_parent.valid() && !alive(new_parent))) return false;
    for (Entity p = new_parent; p.valid(); p = parent(p)) {
        if (p == child) return false;  // would make a cycle
    }
    const Mat4 world = world_matrix(child);
    r->parent = new_parent;
    if (keep_world_placement) set_world_matrix(child, world);
    return true;
}

Mat4 World::world_matrix(Entity e) const {
    const Transform* t = get<Transform>(e);
    if (!t) return Mat4{};
    const Mat4 local = trs(t->position, t->rotation, t->scale);
    const Entity p = parent(e);
    return p.valid() ? world_matrix(p) * local : local;
}

void World::set_world_matrix(Entity e, const Mat4& world) {
    Transform* t = get<Transform>(e);
    if (!t) return;
    const Entity p = parent(e);
    const Mat4 local = p.valid() ? affine_inverse(world_matrix(p)) * world : world;
    decompose_trs(local, t->position, t->rotation, t->scale);
}

void* World::add(Entity e, const reflect::TypeInfo& type) {
    Record* r = record(e);
    const i32 p = pool_index(type);
    if (!r || p < 0) return nullptr;
    Pool& pool = pools_[p];
    if (r->component[p] != kNone) return pool.data + r->component[p] * pool.stride;
    if (pool.count == pool.capacity) {
        const u32 capacity = pool.capacity ? pool.capacity * 2 : 64;
        auto* data = static_cast<u8*>(allocator_.allocate(static_cast<usize>(capacity) * pool.stride, 16));
        auto* owners = static_cast<Entity*>(allocator_.allocate(sizeof(Entity) * capacity, alignof(Entity)));
        if (pool.count) {
            std::memcpy(data, pool.data, static_cast<usize>(pool.count) * pool.stride);
            std::memcpy(owners, pool.owners, sizeof(Entity) * pool.count);
        }
        if (pool.data) allocator_.deallocate(pool.data);
        if (pool.owners) allocator_.deallocate(pool.owners);
        pool.data = data;
        pool.owners = owners;
        pool.capacity = capacity;
    }
    const u32 index = pool.count++;
    u8* component = pool.data + index * pool.stride;
    std::memcpy(component, type.defaults, type.size);
    pool.owners[index] = e;
    r->component[p] = index;
    return component;
}

void* World::get(Entity e, const reflect::TypeInfo& type) {
    Record* r = record(e);
    const i32 p = pool_index(type);
    if (!r || p < 0 || r->component[p] == kNone) return nullptr;
    return pools_[p].data + r->component[p] * pools_[p].stride;
}

const void* World::get(Entity e, const reflect::TypeInfo& type) const { return const_cast<World*>(this)->get(e, type); }

void World::remove(Entity e, const reflect::TypeInfo& type) {
    Record* r = record(e);
    const i32 p = pool_index(type);
    if (r && p > 0) remove_component(*r, static_cast<u32>(p));  // pool 0 is Transform
}

// Moves the pool's last component into the hole so the pool stays packed.
void World::remove_component(Record& r, u32 p) {
    if (r.component[p] == kNone) return;
    Pool& pool = pools_[p];
    const u32 index = r.component[p];
    const u32 last = --pool.count;
    if (index != last) {
        std::memcpy(pool.data + index * pool.stride, pool.data + last * pool.stride, pool.stride);
        pool.owners[index] = pool.owners[last];
        records_[pool.owners[index].index()].component[p] = index;
    }
    r.component[p] = kNone;
}

u32 World::count(const reflect::TypeInfo& type) const {
    const i32 p = pool_index(type);
    return p < 0 ? 0 : pools_[p].count;
}

Entity World::owner(const reflect::TypeInfo& type, u32 i) const { return pools_[pool_index(type)].owners[i]; }

void* World::at(const reflect::TypeInfo& type, u32 i) {
    const Pool& pool = pools_[pool_index(type)];
    return pool.data + i * pool.stride;
}

Entity World::entity_at_slot(u32 slot) const {
    const Record& r = records_[slot];
    return r.alive ? Entity::make(slot, r.generation) : Entity{};
}

bool tags_match(const char* tags, const char* query) {
    const usize length = std::strlen(query);
    if (length == 0) return false;
    for (const char* p = tags; *p;) {
        while (*p == ' ' || *p == ',') ++p;
        const char* end = p;
        while (*end && *end != ',') ++end;
        const char* last = end;
        while (last > p && last[-1] == ' ') --last;
        const usize n = static_cast<usize>(last - p);
        // Exact, or the tag is a child of the query ("Damage.Fire" matches "Damage").
        if (n >= length && std::strncmp(p, query, length) == 0 && (n == length || p[length] == '.')) return true;
        p = end;
    }
    return false;
}

bool has_tag(const World& world, Entity entity, const char* query) {
    const Tags* tags = world.get<Tags>(entity);
    return tags && tags_match(tags->tags, query);
}

bool tags_query(const char* tags, const char* queries, TagQuery mode) {
    bool any = false, all = true, empty = true;
    for (const char* p = queries; *p;) {
        while (*p == ' ' || *p == ',') ++p;
        const char* end = p;
        while (*end && *end != ',') ++end;
        const char* last = end;
        while (last > p && last[-1] == ' ') --last;
        if (last > p) {
            char query[64];
            std::snprintf(query, sizeof query, "%.*s", static_cast<int>(last - p), p);
            const bool hit = tags_match(tags, query);
            any = any || hit;
            all = all && hit;
            empty = false;
        }
        p = end;
    }
    switch (mode) {
        case TagQuery::Any: return any;
        case TagQuery::All: return all && !empty;  // nothing asked: not "has all"
        case TagQuery::None: return !any;
    }
    return false;
}

bool has_tags(const World& world, Entity entity, const char* queries, TagQuery mode) {
    const Tags* tags = world.get<Tags>(entity);
    return tags_query(tags ? tags->tags : "", queries, mode);
}

Entity copy_entity(const World& from, Entity source, World& to, Entity parent, bool unique_names) {
    char name[64];
    if (unique_names) to.unique_name(from.name(source), name, sizeof name);
    else std::snprintf(name, sizeof name, "%s", from.name(source));
    const Entity copy = to.create(name);
    u32 type_count = 0;
    const reflect::TypeInfo* const* types = component_types(type_count);
    for (u32 t = 0; t < type_count; ++t) {
        if (const void* data = from.get(source, *types[t])) std::memcpy(to.add(copy, *types[t]), data, types[t]->size);
    }
    to.set_hidden(copy, from.hidden(source));
    to.set_locked(copy, from.locked(source));
    to.set_folder(copy, from.folder(source));
    to.set_parent(copy, parent, false);
    for (u32 slot = 0; slot < from.slot_count(); ++slot) {  // children too (slot order keeps them stable)
        const Entity child = from.entity_at_slot(slot);
        if (child.valid() && child != copy && from.parent(child) == source) copy_entity(from, child, to, copy, unique_names);
    }
    return copy;
}

} // namespace lsd::world
