#include "js.h"

#include "lsd/script/graph.h"

#include "lsd/foundation/log.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <quickjs.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace lsd::script::js {

namespace {
constexpr u32 kSlots = 256;  // ScriptSystem::instances_
constexpr int kMaxDepth = 16;  // nested tables / objects copied between the languages
constexpr char kFunctionMeta[] = "lsd.js_function";

// console.log and print go to the Output Log like Lua's print.
constexpr char kPrelude[] = R"(
const show = (a) => a.map(v => typeof v === "object" && v !== null ? JSON.stringify(v) : String(v));
globalThis.console = { log: (...a) => lsd.log(...show(a)), warn: (...a) => lsd.log(...show(a)), error: (...a) => lsd.log_error(...show(a)) };
globalThis.print = console.log;
)";
} // namespace

struct Item {
    JSContext* ctx = nullptr;  // one per instance: its own globals, like a Lua script's environment
    JSValue self = JS_UNDEFINED;
};

// A .js file whose functions visual-script nodes call (lsd.js_call): one shared context per file.
struct Library {
    char path[260] = "";
    i64 modified = 0;
    JSContext* ctx = nullptr;
};

struct Host {
    lua_State* L = nullptr;
    JSRuntime* rt = nullptr;
    Item items[kSlots];
    Library libraries[32];
    int depth = 0;
    std::chrono::steady_clock::time_point deadline;
};

namespace {

// A call that runs too long (an endless loop) is stopped instead of freezing the game.
int interrupt(JSRuntime*, void* user) { return std::chrono::steady_clock::now() > static_cast<Host*>(user)->deadline ? 1 : 0; }
void enter(Host& h) {
    if (h.depth++ == 0) h.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
}

Host& host_of(JSContext* ctx) { return *static_cast<Host*>(JS_GetContextOpaque(ctx)); }

// "message\n    at on_update (Content/Scripts/door.js:5:3)" of the pending exception.
void describe_exception(JSContext* ctx, char* out, usize capacity) {
    JSValue e = JS_GetException(ctx);
    const char* text = JS_ToCString(ctx, e);
    JSValue stack = JS_IsObject(e) ? JS_GetPropertyStr(ctx, e, "stack") : JS_UNDEFINED;
    const char* trace = JS_IsUndefined(stack) ? nullptr : JS_ToCString(ctx, stack);
    std::snprintf(out, capacity, "%s%s%s", text ? text : "(error)", trace && trace[0] ? "\n" : "", trace ? trace : "");
    JS_FreeCString(ctx, trace);
    JS_FreeValue(ctx, stack);
    JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, e);
}

JSValue to_js(Host& h, JSContext* ctx, int index, int depth);
void push_lua(Host& h, JSContext* ctx, JSValueConst v, int depth);

// A JS function held by Lua (lsd.after(2, () => ...)): a userdata keeping it and its context alive.
struct JsFunction {
    JSContext* ctx;
    JSValue fn;
};

int js_function_gc(lua_State* L) {
    auto* f = static_cast<JsFunction*>(lua_touserdata(L, 1));
    JS_FreeValue(f->ctx, f->fn);
    JS_FreeContext(f->ctx);
    return 0;
}

// Calls fn with `self` (unless undefined) and the Lua arguments from `first`; pushes the result, or the
// error message when it threw (returns false; the caller frees what it holds, then raises it).
bool call_from_lua(lua_State* L, Host& h, JSContext* ctx, JSValueConst fn, JSValueConst self, int first) {
    JSValue argv[17];
    int argc = 0;
    if (!JS_IsUndefined(self)) argv[argc++] = JS_DupValue(ctx, self);
    for (int i = first; i <= lua_gettop(L) && argc < 17; ++i) argv[argc++] = to_js(h, ctx, i, 0);
    enter(h);
    JSValue result = JS_Call(ctx, fn, JS_UNDEFINED, argc, argv);
    --h.depth;
    for (int i = 0; i < argc; ++i) JS_FreeValue(ctx, argv[i]);
    if (JS_IsException(result)) {
        char message[2048];
        describe_exception(ctx, message, sizeof message);
        lua_pushstring(L, message);
        return false;
    }
    push_lua(h, ctx, result, 0);
    JS_FreeValue(ctx, result);
    return true;
}

int js_function_call(lua_State* L) {
    auto* f = static_cast<JsFunction*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!call_from_lua(L, host_of(f->ctx), f->ctx, f->fn, JS_UNDEFINED, 1)) return lua_error(L);  // nothing of ours to free
    return 1;
}

// A script function as seen from Lua: upvalues Host*, slot, name. Looked up on each call, so a reload
// that changes the function is used at once.
int shim(lua_State* L) {
    Host& h = *static_cast<Host*>(lua_touserdata(L, lua_upvalueindex(1)));
    Item& item = h.items[static_cast<u32>(lua_tointeger(L, lua_upvalueindex(2)))];
    const char* name = lua_tostring(L, lua_upvalueindex(3));
    if (!item.ctx) return 0;
    JSValue global = JS_GetGlobalObject(item.ctx);
    JSValue fn = JS_GetPropertyStr(item.ctx, global, name);
    JS_FreeValue(item.ctx, global);
    if (!JS_IsFunction(item.ctx, fn)) {
        JS_FreeValue(item.ctx, fn);
        return 0;
    }
    // self: built once from the Lua one (entity, name, exposed properties), then the script's own.
    if (JS_IsUndefined(item.self)) item.self = lua_istable(L, 1) ? to_js(h, item.ctx, 1, 0) : JS_NewObject(item.ctx);
    const bool ok = call_from_lua(L, h, item.ctx, fn, item.self, 2);
    JS_FreeValue(item.ctx, fn);
    if (!ok) return lua_error(L);
    return 1;
}

// lsd.<name>(...) in JavaScript: the Lua lsd table's function. Several results come back as an array.
JSValue js_lsd(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int, JSValueConst* data) {
    Host& h = host_of(ctx);
    lua_State* L = h.L;
    const int base = lua_gettop(L);
    if (!lua_checkstack(L, argc + 8)) return JS_ThrowInternalError(ctx, "too many arguments");
    const char* name = JS_ToCString(ctx, data[0]);
    lua_getglobal(L, "lsd");
    lua_getfield(L, -1, name ? name : "");
    lua_remove(L, -2);
    JS_FreeCString(ctx, name);
    for (int i = 0; i < argc; ++i) push_lua(h, ctx, argv[i], 0);
    if (lua_pcall(L, argc, LUA_MULTRET, 0) != 0) {
        JSValue e = JS_ThrowInternalError(ctx, "%s", lua_tostring(L, -1));
        lua_settop(L, base);
        return e;
    }
    const int count = lua_gettop(L) - base;
    JSValue out = JS_UNDEFINED;
    if (count == 1) {
        out = to_js(h, ctx, base + 1, 0);
    } else if (count > 1) {
        out = JS_NewArray(ctx);
        for (int i = 0; i < count; ++i) JS_SetPropertyUint32(ctx, out, static_cast<u32>(i), to_js(h, ctx, base + 1 + i, 0));
    }
    lua_settop(L, base);
    return out;
}

// Lua -> JS: tables with keys 1..n (or empty) become arrays, others objects; functions are not copied.
JSValue to_js(Host& h, JSContext* ctx, int index, int depth) {
    switch (lua_type(h.L, index)) {
    case LUA_TBOOLEAN: return JS_NewBool(ctx, lua_toboolean(h.L, index) != 0);
    case LUA_TNUMBER: return JS_NewFloat64(ctx, lua_tonumber(h.L, index));
    case LUA_TSTRING: {
        size_t length = 0;
        const char* s = lua_tolstring(h.L, index, &length);
        return JS_NewStringLen(ctx, s, length);
    }
    case LUA_TTABLE: break;
    default: return JS_UNDEFINED;
    }
    if (depth >= kMaxDepth || !lua_checkstack(h.L, 4)) return JS_UNDEFINED;
    const usize length = lua_objlen(h.L, index);
    usize keys = 0;
    lua_pushnil(h.L);
    while (lua_next(h.L, index)) {
        ++keys;
        lua_pop(h.L, 1);
    }
    if (keys == length) {  // a list
        JSValue array = JS_NewArray(ctx);
        for (usize i = 0; i < length; ++i) {
            lua_rawgeti(h.L, index, static_cast<int>(i + 1));
            JS_SetPropertyUint32(ctx, array, static_cast<u32>(i), to_js(h, ctx, lua_gettop(h.L), depth + 1));
            lua_pop(h.L, 1);
        }
        return array;
    }
    JSValue object = JS_NewObject(ctx);
    lua_pushnil(h.L);
    while (lua_next(h.L, index)) {
        char key[64];
        if (lua_type(h.L, -2) == LUA_TNUMBER) std::snprintf(key, sizeof key, "%.17g", lua_tonumber(h.L, -2));
        else if (lua_type(h.L, -2) == LUA_TSTRING) std::snprintf(key, sizeof key, "%s", lua_tostring(h.L, -2));
        else key[0] = '\0';
        if (key[0]) JS_SetPropertyStr(ctx, object, key, to_js(h, ctx, lua_gettop(h.L), depth + 1));
        lua_pop(h.L, 1);
    }
    return object;
}

// JS -> Lua: arrays become lists, objects tables, functions callable Lua values.
void push_lua(Host& h, JSContext* ctx, JSValueConst v, int depth) {
    lua_State* L = h.L;
    if (depth >= kMaxDepth || !lua_checkstack(L, 4)) {
        lua_pushnil(L);
    } else if (JS_IsBool(v)) {
        lua_pushboolean(L, JS_ToBool(ctx, v));
    } else if (JS_IsNumber(v)) {
        double d = 0;
        JS_ToFloat64(ctx, &d, v);
        lua_pushnumber(L, d);
    } else if (JS_IsString(v)) {
        size_t length = 0;
        const char* s = JS_ToCStringLen(ctx, &length, v);
        lua_pushlstring(L, s ? s : "", s ? length : 0);
        JS_FreeCString(ctx, s);
    } else if (JS_IsFunction(ctx, v)) {
        auto* f = static_cast<JsFunction*>(lua_newuserdata(L, sizeof(JsFunction)));
        f->ctx = JS_DupContext(ctx);
        f->fn = JS_DupValue(ctx, v);
        luaL_getmetatable(L, kFunctionMeta);
        lua_setmetatable(L, -2);
        lua_pushcclosure(L, js_function_call, 1);
    } else if (JS_IsArray(v)) {
        i64 length = 0;
        JS_GetLength(ctx, v, &length);
        lua_createtable(L, static_cast<int>(length), 0);
        for (i64 i = 0; i < length; ++i) {
            JSValue item = JS_GetPropertyUint32(ctx, v, static_cast<u32>(i));
            push_lua(h, ctx, item, depth + 1);
            JS_FreeValue(ctx, item);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
    } else if (JS_IsObject(v)) {
        lua_newtable(L);
        JSPropertyEnum* names = nullptr;
        u32 count = 0;
        if (JS_GetOwnPropertyNames(ctx, &names, &count, v, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (u32 i = 0; i < count; ++i) {
                const char* key = JS_AtomToCString(ctx, names[i].atom);
                JSValue item = JS_GetProperty(ctx, v, names[i].atom);
                push_lua(h, ctx, item, depth + 1);
                JS_FreeValue(ctx, item);
                lua_setfield(L, -2, key ? key : "");
                JS_FreeCString(ctx, key);
            }
            JS_FreePropertyEnum(ctx, names, count);
        }
    } else {
        lua_pushnil(L);  // undefined, null, symbols...
    }
}

std::vector<std::string> global_names(JSContext* ctx) {
    std::vector<std::string> out;
    JSValue global = JS_GetGlobalObject(ctx);
    JSPropertyEnum* names = nullptr;
    u32 count = 0;
    if (JS_GetOwnPropertyNames(ctx, &names, &count, global, JS_GPN_STRING_MASK) == 0) {
        for (u32 i = 0; i < count; ++i) {
            const char* key = JS_AtomToCString(ctx, names[i].atom);
            if (key) out.emplace_back(key);
            JS_FreeCString(ctx, key);
        }
        JS_FreePropertyEnum(ctx, names, count);
    }
    JS_FreeValue(ctx, global);
    return out;
}

bool eval(Host& h, JSContext* ctx, const std::string& source, const char* name) {
    enter(h);
    JSValue result = JS_Eval(ctx, source.c_str(), source.size(), name, JS_EVAL_TYPE_GLOBAL);
    --h.depth;
    const bool ok = !JS_IsException(result);
    if (!ok) {
        char message[2048];
        describe_exception(ctx, message, sizeof message);
        LSD_LOG_ERROR("script", "%s: %s", name, message);
    }
    JS_FreeValue(ctx, result);
    return ok;
}

bool read_file(const char* path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::stringstream text;
    text << file.rdbuf();
    out = text.str();
    return true;
}

// A context with `lsd` (every function of the Lua lsd table, built-in and prelude ones, so the API
// cannot drift), console and print.
JSContext* new_context(Host& h) {
    lua_State* L = h.L;
    JSContext* ctx = JS_NewContext(h.rt);
    JS_SetContextOpaque(ctx, &h);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue lsd = JS_NewObject(ctx);
    lua_getglobal(L, "lsd");
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        if (lua_type(L, -2) == LUA_TSTRING && lua_isfunction(L, -1) && lua_tostring(L, -2)[0] != '_') {
            const char* name = lua_tostring(L, -2);
            JSValue data = JS_NewString(ctx, name);
            JS_SetPropertyStr(ctx, lsd, name, JS_NewCFunctionData(ctx, js_lsd, 0, 0, 1, &data));
            JS_FreeValue(ctx, data);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    JS_SetPropertyStr(ctx, global, "lsd", lsd);
    JS_FreeValue(ctx, global);
    eval(h, ctx, kPrelude, "lsd prelude");
    return ctx;
}

i64 file_time(const char* path) {
    std::error_code error;
    const auto t = std::filesystem::last_write_time(path, error);
    return error ? 0 : static_cast<i64>(t.time_since_epoch().count());
}

// lsd.js_call(path, function, ...) -> result: a JavaScript node's function, in its file's shared context.
int lua_js_call(lua_State* L) {
    Host*& slot = *static_cast<Host**>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!slot) slot = create(L);
    Host& h = *slot;
    const char* path = luaL_checkstring(L, 1);
    const char* function = luaL_checkstring(L, 2);
    Library* library = nullptr;
    for (Library& l : h.libraries) {
        if (l.ctx && std::strcmp(l.path, path) == 0) library = &l;
    }
    if (!library) {
        for (Library& l : h.libraries) {
            if (!l.ctx) library = &l;
        }
        if (!library) return luaL_error(L, "too many JavaScript node files (32)");
        bool ok = false;
        {
            std::string source;  // gone before any Lua error below
            if (read_file(path, source)) {
                std::snprintf(library->path, sizeof library->path, "%s", path);
                library->modified = file_time(path);
                library->ctx = new_context(h);
                ok = eval(h, library->ctx, source, path);  // logs its error
            }
        }
        if (!ok) {
            if (library->ctx) JS_FreeContext(library->ctx);
            *library = Library{};
            return luaL_error(L, "%s: cannot load its JavaScript nodes", path);
        }
    }
    JSContext* ctx = library->ctx;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, function);
    JS_FreeValue(ctx, global);
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, fn);
        return luaL_error(L, "%s has no function %s", path, function);
    }
    const bool ok = call_from_lua(L, h, ctx, fn, JS_UNDEFINED, 3);
    JS_FreeValue(ctx, fn);
    if (!ok) return lua_error(L);
    return 1;
}

} // namespace

bool is_js(const char* path) {
    const usize n = std::strlen(path);
    return n > 3 && (std::strcmp(path + n - 3, ".js") == 0 || std::strcmp(path + n - 3, ".JS") == 0);
}

Host* create(lua_State* L) {
    auto* h = new Host;
    h->L = L;
    h->rt = JS_NewRuntime();
    JS_SetMaxStackSize(h->rt, 512 * 1024);  // well inside the main thread's stack
    // shortcut: one memory cap for every JS script together, set it per mod when mods arrive.
    JS_SetMemoryLimit(h->rt, 256 * 1024 * 1024);
    JS_SetInterruptHandler(h->rt, interrupt, h);
    luaL_newmetatable(L, kFunctionMeta);
    lua_pushcfunction(L, js_function_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    return h;
}

void destroy(Host* h) {
    if (!h) return;
    for (Item& item : h->items) {
        if (!item.ctx) continue;
        JS_FreeValue(item.ctx, item.self);
        JS_FreeContext(item.ctx);
    }
    for (Library& l : h->libraries) {
        if (l.ctx) JS_FreeContext(l.ctx);
    }
    JS_FreeRuntime(h->rt);
    delete h;
}

void release(Host* host, u32 slot) {
    if (slot >= kSlots) return;
    Item& item = host->items[slot];
    if (!item.ctx) return;
    JS_FreeValue(item.ctx, item.self);
    JS_FreeContext(item.ctx);
    item = Item{};
}

bool load(Host* host, u32 slot, const char* path) {
    Host& h = *host;
    lua_State* L = h.L;
    const int env = lua_gettop(L);
    Item& item = h.items[slot];
    // A reload starts a fresh context (top-level let / const may be declared again); self comes along as JSON.
    std::string kept;
    if (item.ctx) {
        if (!JS_IsUndefined(item.self)) {
            JSValue json = JS_JSONStringify(item.ctx, item.self, JS_UNDEFINED, JS_UNDEFINED);
            if (const char* text = JS_IsString(json) ? JS_ToCString(item.ctx, json) : nullptr) {
                kept = text;
                JS_FreeCString(item.ctx, text);
            } else {
                JS_FreeValue(item.ctx, JS_GetException(item.ctx));  // cyclic self: it starts over
            }
            JS_FreeValue(item.ctx, json);
        }
        JS_FreeValue(item.ctx, item.self);
        JS_FreeContext(item.ctx);
        item = Item{};
    }
    // Its functions and tables from the last load go; what this load defines replaces them.
    lua_pushnil(L);
    while (lua_next(L, env)) {
        lua_pop(L, 1);
        lua_pushvalue(L, -1);
        lua_pushnil(L);
        lua_rawset(L, env);
    }

    std::string source;
    if (!read_file(path, source)) {
        LSD_LOG_ERROR("script", "%s: cannot read the file", path);
        return false;
    }

    JSContext* ctx = new_context(h);
    item.ctx = ctx;
    if (!kept.empty()) {
        item.self = JS_ParseJSON(ctx, kept.c_str(), kept.size(), "self");
        if (JS_IsException(item.self)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            item.self = JS_UNDEFINED;
        }
    }
    JSValue global = JS_GetGlobalObject(ctx);
    const std::vector<std::string> before = global_names(ctx);

    const bool ok = eval(h, ctx, source, path);
    if (ok) {
        // Its top-level functions become Lua functions in its environment; properties / defaults tables too.
        for (const std::string& name : global_names(ctx)) {
            bool old = false;
            for (const std::string& b : before) old = old || b == name;
            if (old) continue;
            JSValue v = JS_GetPropertyStr(ctx, global, name.c_str());
            if (JS_IsFunction(ctx, v)) {
                lua_pushlightuserdata(L, &h);
                lua_pushinteger(L, static_cast<lua_Integer>(slot));
                lua_pushstring(L, name.c_str());
                lua_pushcclosure(L, shim, 3);
                lua_setfield(L, env, name.c_str());
            } else if ((name == "properties" || name == "defaults") && JS_IsObject(v)) {
                push_lua(h, ctx, v, 0);
                lua_setfield(L, env, name.c_str());
            }
            JS_FreeValue(ctx, v);
        }
    }
    JS_FreeValue(ctx, global);
    lua_settop(L, env);
    return ok;
}

void register_lua(lua_State* L, Host** host) {
    lua_getglobal(L, "lsd");
    lua_pushlightuserdata(L, host);
    lua_pushcclosure(L, lua_js_call, 1);
    lua_setfield(L, -2, "js_call");
    lua_pop(L, 1);
}

void update(Host* h) {
    if (!h) return;
    for (Library& l : h->libraries) {  // a changed node file loads again on its next call
        if (l.ctx && file_time(l.path) != l.modified) {
            JS_FreeContext(l.ctx);
            l = Library{};
        }
    }
}

} // namespace lsd::script::js

namespace lsd::script {

namespace {

PinType pin_type(const std::string& name) {
    if (name == "boolean" || name == "bool") return PinType::Bool;
    if (name == "string" || name == "text") return PinType::String;
    if (name == "vector") return PinType::Vector;
    if (name == "object") return PinType::Entity;
    if (name == "any") return PinType::Any;
    return PinType::Float;  // number
}

std::string js_text(JSContext* ctx, JSValueConst v) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) return {};
    const char* s = JS_ToCString(ctx, v);
    std::string out = s ? s : "";
    JS_FreeCString(ctx, s);
    return out;
}

std::string js_field(JSContext* ctx, JSValueConst object, const char* key) {
    JSValue v = JS_GetPropertyStr(ctx, object, key);
    std::string out = js_text(ctx, v);
    JS_FreeValue(ctx, v);
    return out;
}

std::vector<std::string> own_keys(JSContext* ctx, JSValueConst object) {
    std::vector<std::string> out;
    JSPropertyEnum* names = nullptr;
    u32 count = 0;
    if (!JS_IsObject(object) || JS_GetOwnPropertyNames(ctx, &names, &count, object, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) return out;
    for (u32 i = 0; i < count; ++i) {
        const char* key = JS_AtomToCString(ctx, names[i].atom);
        out.emplace_back(key ? key : "");
        JS_FreeCString(ctx, key);
    }
    JS_FreePropertyEnum(ctx, names, count);
    return out;
}

// One node from nodes.<function>; false if the description or the function is missing.
bool register_one(JSContext* ctx, JSValueConst global, JSValueConst desc, const char* path, const char* name, const std::string& function) {
    JSValue fn = JS_GetPropertyStr(ctx, global, function.c_str());
    const bool callable = JS_IsFunction(ctx, fn);
    JS_FreeValue(ctx, fn);
    if (!callable || !JS_IsObject(desc)) return false;
    const std::string id = std::string("JS_") + name + "_" + function;
    std::string title = js_field(ctx, desc, "title"), category = js_field(ctx, desc, "category");
    const std::string tooltip = js_field(ctx, desc, "tooltip"), output = js_field(ctx, desc, "output");
    if (title.empty()) title = function;
    if (category.empty()) category = "JavaScript";
    JSValue pure_value = JS_GetPropertyStr(ctx, desc, "pure");
    const bool pure = JS_ToBool(ctx, pure_value);  // no exec pins: read when its value is needed
    JS_FreeValue(ctx, pure_value);

    PinDef pins[kMaxNodeInputs]{};
    std::string pin_names[kMaxNodeInputs], defaults[kMaxNodeInputs];
    u32 in = 0;
    // Inputs: { amount: "number" } or { amount: ["number", 10] } (the value when nothing is wired).
    JSValue inputs = JS_GetPropertyStr(ctx, desc, "inputs");
    for (const std::string& pin : own_keys(ctx, inputs)) {
        if (in == kMaxNodeInputs - 1) break;  // the Exec pin takes one
        JSValue spec = JS_GetPropertyStr(ctx, inputs, pin.c_str());
        std::string type = js_text(ctx, spec);
        if (JS_IsArray(spec)) {
            JSValue t = JS_GetPropertyUint32(ctx, spec, 0), d = JS_GetPropertyUint32(ctx, spec, 1);
            type = js_text(ctx, t);
            defaults[in] = js_text(ctx, d);
            JS_FreeValue(ctx, t);
            JS_FreeValue(ctx, d);
        }
        JS_FreeValue(ctx, spec);
        const PinType t = pin_type(type);
        if (defaults[in].empty()) defaults[in] = t == PinType::Entity ? "self" : t == PinType::Bool ? "false" : t == PinType::Float ? "0" : "";
        pin_names[in] = pin;
        pins[in] = {pin_names[in].c_str(), t, defaults[in].c_str()};
        ++in;
    }
    JS_FreeValue(ctx, inputs);
    const PinType result = pin_type(output);
    const std::string call = "lsd.js_call(\"" + std::string(path) + "\", \"" + function + "\"";
    register_call_node(id.c_str(), title.c_str(), category.c_str(), tooltip.c_str(), pins, in, output.empty() ? nullptr : &result, pure, call.c_str());
    return true;
}

} // namespace

u32 register_js_nodes(const char* path, const char* name) {
    std::string source;
    if (std::strpbrk(path, "\"\\\n") || !js::read_file(path, source)) return 0;  // the path goes into Lua strings
    if (source.find("nodes") == std::string::npos) return 0;  // most scripts: no need to run them
    JSRuntime* rt = JS_NewRuntime();
    JS_SetMemoryLimit(rt, 16 * 1024 * 1024);
    JS_SetMaxStackSize(rt, 256 * 1024);
    JSContext* ctx = JS_NewContext(rt);
    // Only the description is read: lsd calls the file makes while loading do nothing here.
    // shortcut: no time limit; a file that loops forever at load hangs the editor, add the interrupt guard if that bites.
    const char kStub[] = "globalThis.lsd = new Proxy({}, { get: () => () => undefined });";
    JS_FreeValue(ctx, JS_Eval(ctx, kStub, sizeof kStub - 1, "stub", JS_EVAL_TYPE_GLOBAL));
    JSValue result = JS_Eval(ctx, source.c_str(), source.size(), path, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        char message[1024];
        js::describe_exception(ctx, message, sizeof message);
        LSD_LOG_WARN("script", "%s: %s (its nodes are read anyway)", path, message);
    }
    JS_FreeValue(ctx, result);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue nodes = JS_GetPropertyStr(ctx, global, "nodes");
    u32 registered = 0;
    for (const std::string& function : own_keys(ctx, nodes)) {
        JSValue desc = JS_GetPropertyStr(ctx, nodes, function.c_str());
        if (register_one(ctx, global, desc, path, name, function)) ++registered;
        else LSD_LOG_WARN("script", "%s: nodes.%s needs a description { inputs, output... } and a function %s", path, function.c_str(), function.c_str());
        JS_FreeValue(ctx, desc);
    }
    JS_FreeValue(ctx, nodes);
    JS_FreeValue(ctx, global);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return registered;
}

} // namespace lsd::script
