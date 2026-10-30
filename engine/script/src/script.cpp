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
        {"pressed", Api::pressed},
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
        if (!instances_[i].failed) call(instances_[i], "on_start", 0, false);
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

bool ScriptSystem::call(Instance& instance, const char* function, f32 dt, bool with_dt) {
    lua_rawgeti(lua_, LUA_REGISTRYINDEX, instance.env);
    lua_getfield(lua_, -1, function);
    lua_remove(lua_, -2);
    if (!lua_isfunction(lua_, -1)) {
        lua_pop(lua_, 1);
        return true;  // scripts need not define every event
    }
    lua_rawgeti(lua_, LUA_REGISTRYINDEX, instance.self);
    if (with_dt) lua_pushnumber(lua_, dt);
    char what[300];
    std::snprintf(what, sizeof what, "%s (%s)", instance.path, function);
    if (!protected_call(lua_, with_dt ? 2 : 1, what)) instance.failed = true;
    return !instance.failed;
}

void ScriptSystem::update(f32 dt) {
    if (!lua_) return;
    const bool check_files = static_cast<i64>(time_ * 2.0) != static_cast<i64>((time_ + dt) * 2.0);  // twice a second
    time_ += dt;
    for (u32 i = 0; i < count_; ++i) {
        Instance& instance = instances_[i];
        if (!world_->alive(instance.entity)) continue;
        if (check_files && file_time(instance.path) != instance.modified) {
            if (load(instance)) LSD_LOG_INFO("script", "Reloaded %s", instance.path);
        }
        if (!instance.failed) call(instance, "on_update", dt, true);
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
