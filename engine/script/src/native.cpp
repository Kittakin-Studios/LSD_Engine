#include "lsd/script/native.h"

#include "lsd/foundation/log.h"
#include "lsd/script/script.h"
#include "lsd/world/components.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lsd::script {

const lsd_behaviour* NativeModule::find(const char* name) const {
    for (u32 i = 0; i < count; ++i) {
        if (std::strcmp(names[i], name) == 0) return &behaviours[i];
    }
    return nullptr;
}

namespace {

void* make_state(const lsd_behaviour& b) {
    if (b.create) return b.create();
    void* state = std::calloc(1, b.state_size ? b.state_size : 1);
    return state;
}

void free_state(const lsd_behaviour& b, void* state) {
    if (b.destroy) b.destroy(state);
    else std::free(state);
}

// A property's value as text, or from text (the Native Script component keeps overrides as text).
void property_to_text(const lsd_property& p, const void* state, char* out, usize capacity) {
    const unsigned char* at = static_cast<const unsigned char*>(state) + p.offset;
    if (p.type == LSD_FLOAT) {
        f32 v;
        std::memcpy(&v, at, sizeof v);
        std::snprintf(out, capacity, "%g", static_cast<double>(v));
    } else if (p.type == LSD_INT) {
        i32 v;
        std::memcpy(&v, at, sizeof v);
        std::snprintf(out, capacity, "%d", v);
    } else if (p.type == LSD_BOOL) {
        std::snprintf(out, capacity, "%s", *at ? "true" : "false");
    } else {
        std::snprintf(out, capacity, "%.*s", static_cast<int>(p.size), reinterpret_cast<const char*>(at));
    }
}

void property_from_text(const lsd_property& p, void* state, const char* text) {
    unsigned char* at = static_cast<unsigned char*>(state) + p.offset;
    if (p.type == LSD_FLOAT) {
        const f32 v = std::strtof(text, nullptr);
        std::memcpy(at, &v, sizeof v);
    } else if (p.type == LSD_INT) {
        const i32 v = static_cast<i32>(std::strtol(text, nullptr, 10));
        std::memcpy(at, &v, sizeof v);
    } else if (p.type == LSD_BOOL) {
        *at = std::strcmp(text, "true") == 0 || std::strcmp(text, "1") == 0;
    } else if (p.size > 0) {
        std::snprintf(reinterpret_cast<char*>(at), p.size, "%s", text);
    }
}

} // namespace

void native_property_default(const lsd_behaviour& behaviour, const lsd_property& property, char* out, usize capacity) {
    void* state = make_state(behaviour);
    property_to_text(property, state, out, capacity);
    free_state(behaviour, state);
}

// ---- The function table (lsd_api) ----

struct NativeApi {
    static inline ScriptSystem* active = nullptr;  // the playing ScriptSystem

    static world::World* world() { return active ? active->world_ : nullptr; }
    static const ScriptHost* host() { return active ? &active->host_ : nullptr; }
    static world::Entity entity(lsd_entity e) {
        world::Entity out;
        out.value = e;
        return out;
    }
    static bool live(lsd_entity e) { return world() && world()->alive(entity(e)); }
    static lsd_vec3 vec(Vec3 v) { return {v.x, v.y, v.z}; }
    static Vec3 vec(lsd_vec3 v) { return {v.x, v.y, v.z}; }

    static void register_behaviour(lsd_registry* registry, const lsd_behaviour* b) {
        auto& m = *reinterpret_cast<NativeModule*>(registry);
        if (!b || !b->name || m.count == NativeModule::kMaxBehaviours) return;
        m.behaviours[m.count] = *b;
        std::snprintf(m.names[m.count], sizeof m.names[0], "%s", b->name);
        ++m.count;
    }
    static void log(int error, const char* text) {
        if (error) LSD_LOG_ERROR("game", "%s", text);
        else LSD_LOG_INFO("game", "%s", text);
    }
    static lsd_entity find(const char* name) {
        if (!world() || !name) return 0;
        for (u32 slot = 0; slot < world()->slot_count(); ++slot) {
            const world::Entity e = world()->entity_at_slot(slot);
            if (e.valid() && std::strcmp(world()->name(e), name) == 0) return e.value;
        }
        return 0;
    }
    static const char* name(lsd_entity e) { return live(e) ? world()->name(entity(e)) : ""; }
    static int alive(lsd_entity e) { return live(e); }
    static void destroy(lsd_entity e) {
        if (live(e)) world()->destroy(entity(e));
    }
    static lsd_entity spawn(const char* asset, lsd_vec3 at) {
        const ScriptHost* h = host();
        return h && h->spawn ? h->spawn(h->user, asset, vec(at)).value : 0;
    }
    static lsd_entity player() {
        const ScriptHost* h = host();
        const world::Entity e = h && h->player ? h->player(h->user) : world::Entity{};
        return live(e.value) ? e.value : 0;
    }
    static double time() { return active ? active->time_ : 0.0; }
    static lsd_vec3 position(lsd_entity e) { return live(e) ? vec(world()->get<world::Transform>(entity(e))->position) : lsd_vec3{}; }
    static void set_position(lsd_entity e, lsd_vec3 p) {
        if (live(e)) world()->get<world::Transform>(entity(e))->position = vec(p);
    }
    static lsd_vec3 rotation(lsd_entity e) { return live(e) ? vec(world()->get<world::Transform>(entity(e))->rotation) : lsd_vec3{}; }
    static void set_rotation(lsd_entity e, lsd_vec3 r) {
        if (live(e)) world()->get<world::Transform>(entity(e))->rotation = vec(r);
    }
    static int has_tag(lsd_entity e, const char* tag) { return live(e) && world::has_tag(*world(), entity(e), tag); }

    // Components by name; fields by their Details name.
    static void* component(lsd_entity e, const char* type_name, const reflect::TypeInfo** type) {
        *type = world::find_component_type(type_name);
        return live(e) && *type ? world()->get(entity(e), **type) : nullptr;
    }
    static const reflect::Property* field(const reflect::TypeInfo& type, const char* name) {
        for (u32 i = 0; i < type.property_count; ++i) {
            if (std::strcmp(type.properties[i].name, name) == 0) return &type.properties[i];
        }
        return nullptr;
    }
    static int has(lsd_entity e, const char* c) {
        const reflect::TypeInfo* t;
        return component(e, c, &t) != nullptr;
    }
    static int add(lsd_entity e, const char* c) {
        const reflect::TypeInfo* t = world::find_component_type(c);
        return live(e) && t && world()->add(entity(e), *t) != nullptr;
    }
    static void remove(lsd_entity e, const char* c) {
        const reflect::TypeInfo* t = world::find_component_type(c);
        if (live(e) && t) world()->remove(entity(e), *t);
    }
    static int get_number(lsd_entity e, const char* c, const char* f, double* out) {
        const reflect::TypeInfo* t;
        const unsigned char* data = static_cast<const unsigned char*>(component(e, c, &t));
        const reflect::Property* p = data ? field(*t, f) : nullptr;
        if (!p || !out) return 0;
        const unsigned char* at = data + p->offset;
        switch (p->type) {
        case reflect::FieldType::Bool: *out = *reinterpret_cast<const bool*>(at) ? 1.0 : 0.0; return 1;
        case reflect::FieldType::I32: *out = *reinterpret_cast<const i32*>(at); return 1;
        case reflect::FieldType::U32: *out = *reinterpret_cast<const u32*>(at); return 1;
        case reflect::FieldType::I64: *out = static_cast<double>(*reinterpret_cast<const i64*>(at)); return 1;
        case reflect::FieldType::U64: *out = static_cast<double>(*reinterpret_cast<const u64*>(at)); return 1;
        case reflect::FieldType::F32: *out = *reinterpret_cast<const f32*>(at); return 1;
        case reflect::FieldType::F64: *out = *reinterpret_cast<const f64*>(at); return 1;
        default: return 0;
        }
    }
    static int set_number(lsd_entity e, const char* c, const char* f, double v) {
        const reflect::TypeInfo* t;
        unsigned char* data = static_cast<unsigned char*>(component(e, c, &t));
        const reflect::Property* p = data ? field(*t, f) : nullptr;
        if (!p) return 0;
        unsigned char* at = data + p->offset;
        switch (p->type) {
        case reflect::FieldType::Bool: *reinterpret_cast<bool*>(at) = v != 0.0; return 1;
        case reflect::FieldType::I32: *reinterpret_cast<i32*>(at) = static_cast<i32>(v); return 1;
        case reflect::FieldType::U32: *reinterpret_cast<u32*>(at) = static_cast<u32>(v); return 1;
        case reflect::FieldType::I64: *reinterpret_cast<i64*>(at) = static_cast<i64>(v); return 1;
        case reflect::FieldType::U64: *reinterpret_cast<u64*>(at) = static_cast<u64>(v); return 1;
        case reflect::FieldType::F32: *reinterpret_cast<f32*>(at) = static_cast<f32>(v); return 1;
        case reflect::FieldType::F64: *reinterpret_cast<f64*>(at) = v; return 1;
        default: return 0;
        }
    }
    static Vec3* vec3_field(lsd_entity e, const char* c, const char* f) {
        const reflect::TypeInfo* t;
        unsigned char* data = static_cast<unsigned char*>(component(e, c, &t));
        const reflect::Property* p = data ? field(*t, f) : nullptr;
        const bool is_vec3 = p && p->type == reflect::FieldType::Struct && p->size == sizeof(Vec3) && p->struct_type && p->struct_type->property_count == 3;
        return is_vec3 ? reinterpret_cast<Vec3*>(data + p->offset) : nullptr;
    }
    static int get_vec3(lsd_entity e, const char* c, const char* f, lsd_vec3* out) {
        const Vec3* v = vec3_field(e, c, f);
        if (!v || !out) return 0;
        *out = vec(*v);
        return 1;
    }
    static int set_vec3(lsd_entity e, const char* c, const char* f, lsd_vec3 value) {
        Vec3* v = vec3_field(e, c, f);
        if (!v) return 0;
        *v = vec(value);
        return 1;
    }
    static char* text_field(lsd_entity e, const char* c, const char* f, u32& capacity) {
        const reflect::TypeInfo* t;
        unsigned char* data = static_cast<unsigned char*>(component(e, c, &t));
        const reflect::Property* p = data ? field(*t, f) : nullptr;
        if (!p || p->type != reflect::FieldType::String) return nullptr;
        capacity = p->size;
        return reinterpret_cast<char*>(data + p->offset);
    }
    static int get_text(lsd_entity e, const char* c, const char* f, char* out, size_t capacity) {
        u32 size = 0;
        const char* text = text_field(e, c, f, size);
        if (!text || !out || capacity == 0) return 0;
        std::snprintf(out, capacity, "%s", text);
        return 1;
    }
    static int set_text(lsd_entity e, const char* c, const char* f, const char* value) {
        u32 size = 0;
        char* text = text_field(e, c, f, size);
        if (!text || !value) return 0;
        std::snprintf(text, size, "%s", value);
        return 1;
    }

    static lsd_vec3 input(const char* action) {
        const ScriptHost* h = host();
        return h && h->input ? vec(h->input(h->user, action)) : lsd_vec3{};
    }
    static int pressed(const char* action) {
        const ScriptHost* h = host();
        return h && h->pressed && h->pressed(h->user, action);
    }
    static int fill(bool found, world::Entity e, Vec3 point, Vec3 normal, lsd_hit* hit) {
        if (found && hit) *hit = {e.value, vec(point), vec(normal)};
        return found;
    }
    static int raycast(lsd_vec3 from, lsd_vec3 direction, float distance, lsd_hit* hit) {
        const ScriptHost* h = host();
        world::Entity e;
        Vec3 point, normal;
        return fill(h && h->raycast && h->raycast(h->user, vec(from), vec(direction), distance, e, point, normal), e, point, normal, hit);
    }
    static int sphere_trace(lsd_vec3 from, lsd_vec3 to, float radius, lsd_hit* hit) {
        const ScriptHost* h = host();
        world::Entity e;
        Vec3 point, normal;
        return fill(h && h->sweep && h->sweep(h->user, 0, {radius, 0, 0}, vec(from), vec(to), e, point, normal), e, point, normal, hit);
    }
    static uint32_t play_sound(const char* name, int at_position, lsd_vec3 at) {
        const ScriptHost* h = host();
        return h && h->play_sound ? h->play_sound(h->user, name, at_position != 0, vec(at)) : 0;
    }
    static void stop_sound(uint32_t id) {
        const ScriptHost* h = host();
        if (h && h->stop_sound) h->stop_sound(h->user, id);
    }
    static void set_anim_param(lsd_entity e, const char* name, float value) {
        const ScriptHost* h = host();
        if (h && h->set_anim_param && live(e)) h->set_anim_param(h->user, entity(e), name, value);
    }
    static int move_to(lsd_entity e, lsd_vec3 target) {
        const ScriptHost* h = host();
        return h && h->move_to && live(e) && h->move_to(h->user, entity(e), vec(target));
    }
    static void camera_shake(float strength, float seconds, float frequency) {
        const ScriptHost* h = host();
        if (h && h->camera_shake) h->camera_shake(h->user, strength, seconds, frequency);
    }
    static int open_level(const char* name) {
        const ScriptHost* h = host();
        return h && h->open_level && h->open_level(h->user, name);
    }
    static int ui_open(const char* name) {
        const ScriptHost* h = host();
        return h && h->ui_open ? h->ui_open(h->user, name) : -1;
    }
    static void ui_close(int doc) {
        const ScriptHost* h = host();
        if (h && h->ui_close) h->ui_close(h->user, doc);
    }
    static void ui_set_text(int doc, const char* id, const char* text) {
        const ScriptHost* h = host();
        if (h && h->ui_set) h->ui_set(h->user, doc, id, nullptr, text);
    }
    static int call(lsd_entity target, const char* event, lsd_entity other) {
        if (!active || !event) return 0;
        return static_cast<int>(active->send(entity(target), event, entity(other)));
    }
    static int broadcast(const char* event) {
        if (!active || !event) return 0;
        return static_cast<int>(active->broadcast(event, ""));
    }
    static void draw_line(lsd_vec3 a, lsd_vec3 b, lsd_vec3 color, float seconds) {
        const ScriptHost* h = host();
        if (h && h->draw_debug) h->draw_debug(h->user, 0, vec(a), vec(b), 0.0f, nullptr, vec(color), seconds);
    }
};

const lsd_api* native_api() {
    static const lsd_api api = {
        LSD_GAME_API_VERSION,
        NativeApi::register_behaviour,
        NativeApi::log,
        NativeApi::find,
        NativeApi::name,
        NativeApi::alive,
        NativeApi::destroy,
        NativeApi::spawn,
        NativeApi::player,
        NativeApi::time,
        NativeApi::position,
        NativeApi::set_position,
        NativeApi::rotation,
        NativeApi::set_rotation,
        NativeApi::has_tag,
        NativeApi::has,
        NativeApi::add,
        NativeApi::remove,
        NativeApi::get_number,
        NativeApi::set_number,
        NativeApi::get_vec3,
        NativeApi::set_vec3,
        NativeApi::get_text,
        NativeApi::set_text,
        NativeApi::input,
        NativeApi::pressed,
        NativeApi::raycast,
        NativeApi::sphere_trace,
        NativeApi::play_sound,
        NativeApi::stop_sound,
        NativeApi::set_anim_param,
        NativeApi::move_to,
        NativeApi::camera_shake,
        NativeApi::open_level,
        NativeApi::ui_open,
        NativeApi::ui_close,
        NativeApi::ui_set_text,
        NativeApi::call,
        NativeApi::broadcast,
        NativeApi::draw_line,
    };
    return &api;
}

// ---- Running behaviours (ScriptSystem's native half) ----

void ScriptSystem::add_natives(world::Entity root) {
    if (!world_ || !module_) return;
    const reflect::TypeInfo& type = reflect::type_of<world::NativeScript>();
    for (u32 i = 0; i < world_->count(type); ++i) {
        const world::Entity e = world_->owner(type, i);
        if (root.valid()) {
            bool inside = false;
            for (world::Entity p = e; p.valid() && !inside; p = world_->parent(p)) inside = p == root;
            if (!inside) continue;
        }
        const auto& component = *static_cast<const world::NativeScript*>(world_->at(type, i));
        if (!component.behaviour[0]) continue;
        const lsd_behaviour* b = module_->find(component.behaviour);
        if (!b) {
            LSD_LOG_WARN("game", "%s: no behaviour '%s' in the game module (Tools > Compile Game Code)", world_->name(e), component.behaviour);
            continue;
        }
        if (native_count_ == sizeof natives_ / sizeof natives_[0]) {
            LSD_LOG_WARN("game", "Too many native behaviours: %s does not run", world_->name(e));
            continue;
        }
        void* state = make_state(*b);
        for (const world::ScriptProperty& own : component.properties) {  // this object's values
            for (int k = 0; k < b->property_count; ++k) {
                if (std::strcmp(b->properties[k].name, own.name) == 0) property_from_text(b->properties[k], state, own.value);
            }
        }
        natives_[native_count_++] = {e, b, state};
    }
}

void ScriptSystem::create_natives() {
    if (!world_ || !module_ || native_count_) return;
    NativeApi::active = this;
    add_natives({});
}

void ScriptSystem::start_natives() {
    if (native_count_) return;
    create_natives();
    for (u32 i = 0; i < native_count_; ++i) {
        const NativeInstance n = natives_[i];  // a copy: on_start may spawn (attach adds natives)
        if (n.behaviour->on_start) n.behaviour->on_start(n.entity.value, n.state);
    }
}

void ScriptSystem::stop_natives() {
    for (u32 i = 0; i < native_count_; ++i) {
        const NativeInstance& n = natives_[i];
        if (n.behaviour->on_stop) n.behaviour->on_stop(n.entity.value, n.state);
    }
    for (u32 i = 0; i < native_count_; ++i) free_state(*natives_[i].behaviour, natives_[i].state);
    native_count_ = 0;
    if (NativeApi::active == this) NativeApi::active = nullptr;
}

u32 ScriptSystem::native_event(world::Entity target, const char* event, world::Entity other, bool everyone) {
    u32 taken = 0;
    for (u32 i = 0; i < native_count_; ++i) {
        const NativeInstance n = natives_[i];  // a copy: the call may add natives (spawn)
        if (!everyone && n.entity != target) continue;
        if (!world_->alive(n.entity)) continue;
        const lsd_behaviour& b = *n.behaviour;
        if (std::strcmp(event, "on_hit") == 0 && b.on_hit) b.on_hit(n.entity.value, n.state, other.value);
        else if (std::strcmp(event, "on_overlap_begin") == 0 && b.on_overlap_begin) b.on_overlap_begin(n.entity.value, n.state, other.value);
        else if (std::strcmp(event, "on_overlap_end") == 0 && b.on_overlap_end) b.on_overlap_end(n.entity.value, n.state, other.value);
        else if (b.on_event) b.on_event(n.entity.value, n.state, event, other.value);
        else continue;
        ++taken;
    }
    return taken;
}

} // namespace lsd::script
