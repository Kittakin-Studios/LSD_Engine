#include "lsd/script/script.h"

#include "lsd/foundation/log.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <cstdio>
#include <cstring>
#include <filesystem>

namespace lsd::script {

namespace {

i64 file_time(const char* path) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    return error ? 0 : static_cast<i64>(time.time_since_epoch().count());
}

int traceback(lua_State* L) {
    luaL_traceback(L, L, lua_tostring(L, 1), 1);
    return 1;
}

// pcall with a stack trace; logs and returns false on error. The function and its args are on the stack.
bool protected_call(lua_State* L, int args, const char* what) {
    const int handler = lua_gettop(L) - args;
    lua_pushcfunction(L, traceback);
    lua_insert(L, handler);
    const int status = lua_pcall(L, args, 0, handler);
    if (status != 0) LSD_LOG_ERROR("script", "%s: %s", what, lua_tostring(L, -1));
    lua_settop(L, handler - 1);  // the handler and the error message (if any)
    return status == 0;
}

// Timers live in Lua: closures keep each script's environment, and errors stay per callback.
constexpr char kPrelude[] = R"(
local timers, next_id = {}, 0
local function add(seconds, fn, every)
  next_id = next_id + 1
  timers[next_id] = {at = lsd.time() + seconds, fn = fn, every = every}
  return next_id
end
function lsd.after(seconds, fn) return add(seconds, fn, nil) end
function lsd.every(seconds, fn) return add(seconds, fn, seconds) end
function lsd.cancel(id) timers[id] = nil end
function lsd._tick()
  local now, due = lsd.time(), {}
  for id, t in pairs(timers) do if now >= t.at then due[#due + 1] = id end end
  table.sort(due)
  for _, id in ipairs(due) do
    local t = timers[id]
    if t then  -- an earlier callback may have cancelled it
      if t.every then t.at = t.at + t.every else timers[id] = nil end
      local ok, err = xpcall(t.fn, debug.traceback)
      if not ok then lsd.log_error(err) end
    end
  end
end
)";

void push_struct(lua_State* L, const reflect::TypeInfo& type, const u8* data);

void push_value(lua_State* L, const reflect::Property& p, const u8* field) {
    using reflect::FieldType;
    switch (p.type) {
        case FieldType::Bool: lua_pushboolean(L, *reinterpret_cast<const bool*>(field)); break;
        case FieldType::I32: lua_pushnumber(L, *reinterpret_cast<const i32*>(field)); break;
        case FieldType::U32: lua_pushnumber(L, *reinterpret_cast<const u32*>(field)); break;
        case FieldType::I64: lua_pushnumber(L, static_cast<lua_Number>(*reinterpret_cast<const i64*>(field))); break;
        case FieldType::U64: lua_pushnumber(L, static_cast<lua_Number>(*reinterpret_cast<const u64*>(field))); break;
        case FieldType::F32: lua_pushnumber(L, *reinterpret_cast<const f32*>(field)); break;
        case FieldType::F64: lua_pushnumber(L, *reinterpret_cast<const f64*>(field)); break;
        case FieldType::String: lua_pushstring(L, reinterpret_cast<const char*>(field)); break;
        case FieldType::Struct: push_struct(L, *p.struct_type, field); break;
        case FieldType::Array: {
            const reflect::Property element = reflect::element_property(p);
            const u32 count = *reinterpret_cast<const u32*>(field);
            lua_createtable(L, static_cast<int>(count), 0);
            for (u32 i = 0; i < count; ++i) {
                push_value(L, element, field + p.items_offset + i * p.element_size);
                lua_rawseti(L, -2, static_cast<int>(i + 1));
            }
            break;
        }
    }
}

void push_struct(lua_State* L, const reflect::TypeInfo& type, const u8* data) {
    lua_createtable(L, 0, static_cast<int>(type.property_count));
    for (u32 i = 0; i < type.property_count; ++i) {
        push_value(L, type.properties[i], data + type.properties[i].offset);
        lua_setfield(L, -2, type.properties[i].name);
    }
}

void read_struct(lua_State* L, int index, const reflect::TypeInfo& type, u8* data);

// Writes the Lua value at `index` into a field; raises a Lua error on a type mismatch.
void read_value(lua_State* L, int index, const reflect::Property& p, u8* field) {
    using reflect::FieldType;
    auto number = [&]() {
        if (!lua_isnumber(L, index)) luaL_error(L, "%s: expected a number", p.name);
        return lua_tonumber(L, index);
    };
    switch (p.type) {
        case FieldType::Bool:
            if (!lua_isboolean(L, index)) luaL_error(L, "%s: expected true or false", p.name);
            *reinterpret_cast<bool*>(field) = lua_toboolean(L, index) != 0;
            break;
        case FieldType::I32: *reinterpret_cast<i32*>(field) = static_cast<i32>(number()); break;
        case FieldType::U32: *reinterpret_cast<u32*>(field) = static_cast<u32>(number()); break;
        case FieldType::I64: *reinterpret_cast<i64*>(field) = static_cast<i64>(number()); break;
        case FieldType::U64: *reinterpret_cast<u64*>(field) = static_cast<u64>(number()); break;
        case FieldType::F32: *reinterpret_cast<f32*>(field) = static_cast<f32>(number()); break;
        case FieldType::F64: *reinterpret_cast<f64*>(field) = number(); break;
        case FieldType::String: {
            if (!lua_isstring(L, index)) luaL_error(L, "%s: expected a string", p.name);
            std::snprintf(reinterpret_cast<char*>(field), p.size, "%s", lua_tostring(L, index));
            break;
        }
        case FieldType::Struct:
            if (!lua_istable(L, index)) luaL_error(L, "%s: expected a table", p.name);
            read_struct(L, index, *p.struct_type, field);
            break;
        case FieldType::Array: {
            if (!lua_istable(L, index)) luaL_error(L, "%s: expected a list", p.name);
            const reflect::Property element = reflect::element_property(p);
            const u32 count = static_cast<u32>(lua_objlen(L, index));
            if (count > p.capacity) luaL_error(L, "%s: holds at most %d items", p.name, static_cast<int>(p.capacity));
            for (u32 i = 0; i < count; ++i) {
                lua_rawgeti(L, index, static_cast<int>(i + 1));
                read_value(L, lua_gettop(L), element, field + p.items_offset + i * p.element_size);
                lua_pop(L, 1);
            }
            *reinterpret_cast<u32*>(field) = count;
            break;
        }
    }
}

// Only the keys present in the table are written, so lsd.set(e, "X", {one_field = 1}) leaves the rest.
void read_struct(lua_State* L, int index, const reflect::TypeInfo& type, u8* data) {
    index = index < 0 ? lua_gettop(L) + index + 1 : index;
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        const char* key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : nullptr;
        const reflect::Property* p = key ? reflect::find_property(type, key) : nullptr;
        if (!p) luaL_error(L, "%s has no field '%s'", type.name, key ? key : "?");
        read_value(L, lua_gettop(L), *p, data + p->offset);
        lua_pop(L, 1);
    }
}

} // namespace

// The `lsd` table. Each function finds the ScriptSystem in its upvalue.
struct Api {
    static ScriptSystem& system(lua_State* L) { return *static_cast<ScriptSystem*>(lua_touserdata(L, lua_upvalueindex(1))); }
    static world::World& world(lua_State* L) { return *system(L).world_; }

    static world::Entity entity(lua_State* L, int index) {
        world::Entity e;
        e.value = static_cast<u32>(luaL_checknumber(L, index));
        return e;
    }
    static world::Entity live_entity(lua_State* L, int index) {
        const world::Entity e = entity(L, index);
        if (!world(L).alive(e)) luaL_argerror(L, index, "not a live entity");
        return e;
    }
    static void push_vec3(lua_State* L, Vec3 v) {
        lua_pushnumber(L, v.x);
        lua_pushnumber(L, v.y);
        lua_pushnumber(L, v.z);
    }
    static Vec3 check_vec3(lua_State* L, int first) {
        return {static_cast<f32>(luaL_checknumber(L, first)), static_cast<f32>(luaL_checknumber(L, first + 1)), static_cast<f32>(luaL_checknumber(L, first + 2))};
    }

    static int log(lua_State* L) {
        const int n = lua_gettop(L);
        for (int i = 1; i <= n; ++i) {  // tostring(arg) " " tostring(arg) ... then one concat
            lua_getglobal(L, "tostring");
            lua_pushvalue(L, i);
            lua_call(L, 1, 1);
            if (i < n) lua_pushliteral(L, " ");
        }
        if (n == 0) lua_pushliteral(L, "");
        else lua_concat(L, 2 * n - 1);
        LSD_LOG_INFO("script", "%s", lua_tostring(L, -1));
        return 0;
    }
    static int log_error(lua_State* L) {
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, 1);
        lua_call(L, 1, 1);
        LSD_LOG_ERROR("script", "%s", lua_tostring(L, -1));
        return 0;
    }
    static const reflect::TypeInfo& component_type(lua_State* L, int index) {
        const reflect::TypeInfo* type = world::find_component_type(luaL_checkstring(L, index));
        if (!type) luaL_argerror(L, index, "not a component type");
        return *type;
    }
    static int get(lua_State* L) {
        const world::Entity e = live_entity(L, 1);
        const reflect::TypeInfo& type = component_type(L, 2);
        if (const void* data = world(L).get(e, type)) push_struct(L, type, static_cast<const u8*>(data));
        else lua_pushnil(L);
        return 1;
    }
    // Validates into a copy first, so a bad field leaves the component as it was.
    static void write(lua_State* L, world::Entity e, const reflect::TypeInfo& type, void* data, int fields) {
        u8 copy[1024];
        if (type.size > sizeof copy) luaL_error(L, "%s is too big to set from Lua", type.name);
        std::memcpy(copy, data, type.size);
        read_struct(L, fields, type, copy);
        std::memcpy(world(L).get(e, type), copy, type.size);
    }
    static int set(lua_State* L) {
        const world::Entity e = live_entity(L, 1);
        const reflect::TypeInfo& type = component_type(L, 2);
        luaL_checktype(L, 3, LUA_TTABLE);
        void* data = world(L).get(e, type);
        if (!data) luaL_error(L, "%s has no %s (lsd.add it first)", world(L).name(e), type.name);
        write(L, e, type, data, 3);
        return 0;
    }
    static int add(lua_State* L) {
        const world::Entity e = live_entity(L, 1);
        const reflect::TypeInfo& type = component_type(L, 2);
        void* data = world(L).add(e, type);
        if (lua_istable(L, 3)) write(L, e, type, data, 3);
        return 0;
    }
    static int remove(lua_State* L) {
        const world::Entity e = live_entity(L, 1);
        world(L).remove(e, component_type(L, 2));
        return 0;
    }
    static int has(lua_State* L) {
        const world::Entity e = live_entity(L, 1);
        lua_pushboolean(L, world(L).get(e, component_type(L, 2)) != nullptr);
        return 1;
    }
    static int find(lua_State* L) {
        const char* name = luaL_checkstring(L, 1);
        const world::World& w = world(L);
        for (u32 slot = 0; slot < w.slot_count(); ++slot) {
            const world::Entity e = w.entity_at_slot(slot);
            if (e.valid() && std::strcmp(w.name(e), name) == 0) {
                lua_pushnumber(L, e.value);
                return 1;
            }
        }
        lua_pushnil(L);
        return 1;
    }
    static int name(lua_State* L) {
        lua_pushstring(L, world(L).name(live_entity(L, 1)));
        return 1;
    }
    static int alive(lua_State* L) {
        lua_pushboolean(L, world(L).alive(entity(L, 1)));
        return 1;
    }
    static int destroy(lua_State* L) {
        world(L).destroy(live_entity(L, 1));
        return 0;
    }
    static int position(lua_State* L) {
        push_vec3(L, world(L).get<world::Transform>(live_entity(L, 1))->position);
        return 3;
    }
    static int set_position(lua_State* L) {
        world(L).get<world::Transform>(live_entity(L, 1))->position = check_vec3(L, 2);
        return 0;
    }
    static int rotation(lua_State* L) {
        push_vec3(L, world(L).get<world::Transform>(live_entity(L, 1))->rotation);
        return 3;
    }
    static int set_rotation(lua_State* L) {
        world(L).get<world::Transform>(live_entity(L, 1))->rotation = check_vec3(L, 2);
        return 0;
    }
    static int has_tag(lua_State* L) {
        lua_pushboolean(L, world::has_tag(world(L), live_entity(L, 1), luaL_checkstring(L, 2)));
        return 1;
    }
    static int time(lua_State* L) {
        lua_pushnumber(L, system(L).time_);
        return 1;
    }
    static int input(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        const Vec3 v = host.input ? host.input(host.user, luaL_checkstring(L, 1)) : Vec3{};
        lua_pushnumber(L, v.x);
        lua_pushnumber(L, v.y);
        return 2;
    }
    static int pressed(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        lua_pushboolean(L, host.pressed && host.pressed(host.user, luaL_checkstring(L, 1)));
        return 1;
    }
};

ScriptSystem::~ScriptSystem() { stop(); }

void ScriptSystem::stop() {
    if (lua_) lua_close(lua_);
    lua_ = nullptr;
    world_ = nullptr;
    count_ = 0;
    time_ = 0;
}

u32 ScriptSystem::error_count() const {
    u32 n = 0;
    for (u32 i = 0; i < count_; ++i) n += instances_[i].failed;
    return n;
}

void ScriptSystem::start(world::World& world, const ScriptHost& host, PathFn path_of, void* path_user) {
    stop();
    world_ = &world;
    host_ = host;
    // shortcut: Lua's own allocator and the full standard library (io, os); give it an Allocator and a sandbox before shipping mods.
    lua_ = luaL_newstate();
    luaL_openlibs(lua_);

    const luaL_Reg api[] = {
        {"log", Api::log}, {"find", Api::find}, {"name", Api::name}, {"alive", Api::alive}, {"destroy", Api::destroy},
        {"position", Api::position}, {"set_position", Api::set_position}, {"rotation", Api::rotation},
        {"set_rotation", Api::set_rotation}, {"has_tag", Api::has_tag}, {"time", Api::time}, {"input", Api::input},
        {"pressed", Api::pressed}, {"log_error", Api::log_error}, {"get", Api::get}, {"set", Api::set},
        {"add", Api::add}, {"remove", Api::remove}, {"has", Api::has},
    };
    lua_newtable(lua_);
    for (const luaL_Reg& r : api) {
        lua_pushlightuserdata(lua_, this);
        lua_pushcclosure(lua_, r.func, 1);
        lua_setfield(lua_, -2, r.name);
    }
    lua_getfield(lua_, -1, "log");
    lua_setglobal(lua_, "print");  // print() goes to the Output Log too
    lua_setglobal(lua_, "lsd");
    run(kPrelude, "=lsd prelude");

    const reflect::TypeInfo& type = reflect::type_of<world::Script>();
    for (u32 i = 0; i < world.count(type) && count_ < sizeof instances_ / sizeof instances_[0]; ++i) {
        const auto* component = static_cast<const world::Script*>(world.at(type, i));
        if (!component->script[0]) continue;
        Instance& instance = instances_[count_];
        instance = Instance{};
        instance.entity = world.owner(type, i);
        if (!path_of || !path_of(path_user, component->script, instance.path, sizeof instance.path)) {
            LSD_LOG_WARN("script", "%s: its Script asset is missing", world.name(instance.entity));
            continue;
        }
        ++count_;
        // Its self table: { entity = <handle>, name = "<name>" }.
        lua_newtable(lua_);
        lua_pushnumber(lua_, instance.entity.value);
        lua_setfield(lua_, -2, "entity");
        lua_pushstring(lua_, world.name(instance.entity));
        lua_setfield(lua_, -2, "name");
        instance.self = luaL_ref(lua_, LUA_REGISTRYINDEX);
        // Its environment: own globals, falling back to the shared ones (lsd, math, string...).
        lua_newtable(lua_);
        lua_newtable(lua_);
        lua_pushvalue(lua_, LUA_GLOBALSINDEX);
        lua_setfield(lua_, -2, "__index");
        lua_setmetatable(lua_, -2);
        instance.env = luaL_ref(lua_, LUA_REGISTRYINDEX);
        load(instance);
    }
    // on_start after every script is loaded, so lsd.find sees them all.
    for (u32 i = 0; i < count_; ++i) {
        if (!instances_[i].failed) call(instances_[i], "on_start", nullptr, 0);
    }
}

bool ScriptSystem::load(Instance& instance) {
    instance.modified = file_time(instance.path);
    instance.failed = true;
    if (luaL_loadfile(lua_, instance.path) != 0) {
        LSD_LOG_ERROR("script", "%s", lua_tostring(lua_, -1));
        lua_pop(lua_, 1);
        return false;
    }
    lua_rawgeti(lua_, LUA_REGISTRYINDEX, instance.env);
    lua_setfenv(lua_, -2);
    instance.failed = !protected_call(lua_, 0, instance.path);
    return !instance.failed;
}

bool ScriptSystem::call(Instance& instance, const char* function, const f64* args, int arg_count) {
    lua_rawgeti(lua_, LUA_REGISTRYINDEX, instance.env);
    lua_getfield(lua_, -1, function);
    lua_remove(lua_, -2);
    if (!lua_isfunction(lua_, -1)) {
        lua_pop(lua_, 1);
        return true;  // scripts need not define every event
    }
    lua_rawgeti(lua_, LUA_REGISTRYINDEX, instance.self);
    for (int i = 0; i < arg_count; ++i) lua_pushnumber(lua_, args[i]);
    char what[300];
    std::snprintf(what, sizeof what, "%s (%s)", instance.path, function);
    if (!protected_call(lua_, 1 + arg_count, what)) instance.failed = true;
    return !instance.failed;
}

void ScriptSystem::update(f32 dt) {
    if (!lua_) return;
    const bool check_files = static_cast<i64>(time_ * 2.0) != static_cast<i64>((time_ + dt) * 2.0);  // twice a second
    time_ += dt;
    lua_getglobal(lua_, "lsd");
    lua_getfield(lua_, -1, "_tick");
    lua_remove(lua_, -2);
    protected_call(lua_, 0, "timers");
    const f64 dt_arg = dt;
    for (u32 i = 0; i < count_; ++i) {
        Instance& instance = instances_[i];
        if (!world_->alive(instance.entity)) continue;
        if (check_files && file_time(instance.path) != instance.modified) {
            if (load(instance)) LSD_LOG_INFO("script", "Reloaded %s", instance.path);
        }
        if (!instance.failed) call(instance, "on_update", &dt_arg, 1);
    }
}

void ScriptSystem::send(world::Entity target, const char* event, world::Entity other) {
    const f64 arg = other.value;
    for (u32 i = 0; i < count_; ++i) {
        if (instances_[i].entity == target && !instances_[i].failed) call(instances_[i], event, &arg, 1);
    }
}

bool ScriptSystem::run(const char* code, const char* chunk_name) {
    if (!lua_) return false;
    if (luaL_loadbuffer(lua_, code, std::strlen(code), chunk_name) != 0) {
        LSD_LOG_ERROR("script", "%s", lua_tostring(lua_, -1));
        lua_pop(lua_, 1);
        return false;
    }
    return protected_call(lua_, 0, chunk_name);
}

} // namespace lsd::script
