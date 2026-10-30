#pragma once
// World: entities are generational handles; components live in one packed pool per type
// (all Transforms together, all StaticMeshes together), so systems loop over plain arrays.
// Component types are reflected structs; the world stores them as bytes using their TypeInfo.
#include "lsd/foundation/allocator.h"
#include "lsd/foundation/array.h"
#include "lsd/foundation/handle.h"
#include "lsd/foundation/math.h"
#include "lsd/reflect/reflect.h"

namespace lsd::world {

struct EntityTag;
using Entity = Handle<EntityTag>;

constexpr u32 kMaxComponentTypes = 16;

// Component types the world knows, in "Add Component" order. Transform is always first.
const reflect::TypeInfo* const* component_types(u32& count);
const reflect::TypeInfo* find_component_type(const char* name);  // by TypeInfo name, e.g. "StaticMesh"

class World {
public:
    explicit World(Allocator& allocator);
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // New entity with a Transform. Names need not be unique (unique_name() helps).
    Entity create(const char* name);
    // Recreates an entity with an exact handle (level load, undo). False if that slot is in use.
    bool create_at(Entity handle, const char* name);
    void destroy(Entity entity);  // children too
    void clear();
    bool alive(Entity entity) const;

    const char* name(Entity entity) const;
    void set_name(Entity entity, const char* name);
    // "Cube" -> "Cube", or "Cube2", "Cube3"... if taken.
    void unique_name(const char* base, char* out, usize capacity) const;

    // Hierarchy. set_parent refuses cycles; by default the child keeps its world placement
    // (its Transform is rewritten relative to the new parent), like dragging in Unreal's Outliner.
    Entity parent(Entity entity) const;
    bool set_parent(Entity child, Entity parent, bool keep_world_placement = true);
    Mat4 world_matrix(Entity entity) const;
    void set_world_matrix(Entity entity, const Mat4& world);  // writes the Transform relative to the parent

    // Components by type. add() returns the existing one if present; new ones copy the type's defaults.
    void* add(Entity entity, const reflect::TypeInfo& type);
    void* get(Entity entity, const reflect::TypeInfo& type);
    const void* get(Entity entity, const reflect::TypeInfo& type) const;
    void remove(Entity entity, const reflect::TypeInfo& type);  // Transform cannot be removed

    template <typename T> T* add(Entity e) { return static_cast<T*>(add(e, reflect::type_of<T>())); }
    template <typename T> T* get(Entity e) { return static_cast<T*>(get(e, reflect::type_of<T>())); }
    template <typename T> const T* get(Entity e) const { return static_cast<const T*>(get(e, reflect::type_of<T>())); }

    // Packed iteration over one component type: for (i < count(type)) owner(type, i), at(type, i).
    u32 count(const reflect::TypeInfo& type) const;
    Entity owner(const reflect::TypeInfo& type, u32 i) const;
    void* at(const reflect::TypeInfo& type, u32 i);
    const void* at(const reflect::TypeInfo& type, u32 i) const { return const_cast<World*>(this)->at(type, i); }

    // All live entities in slot order (stable: the Outliner and level files use it).
    u32 slot_count() const { return static_cast<u32>(records_.size()); }
    Entity entity_at_slot(u32 slot) const;  // invalid if the slot is free

private:
    struct Record {
        u32 generation;
        bool alive;
        char name[64];
        Entity parent;
        u32 component[kMaxComponentTypes];  // index into each pool, kNone if absent
    };
    struct Pool {
        const reflect::TypeInfo* type = nullptr;
        u8* data = nullptr;
        Entity* owners = nullptr;
        u32 count = 0;
        u32 capacity = 0;
        u32 stride = 0;
    };
    static constexpr u32 kNone = 0xFFFFFFFFu;

    Record* record(Entity entity);
    const Record* record(Entity entity) const;
    i32 pool_index(const reflect::TypeInfo& type) const;
    void init_record(u32 slot, u32 generation, const char* name);
    void remove_component(Record& r, u32 pool);

    Allocator& allocator_;
    Array<Record> records_;
    Array<u32> free_slots_;
    Pool pools_[kMaxComponentTypes];
    u32 pool_count_ = 0;
};

} // namespace lsd::world
