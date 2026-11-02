#pragma once
// JavaScript gameplay scripts (QuickJS-ng) inside the Lua script system. A .js script's top-level
// functions become Lua functions in its environment, so events, broadcasts, lsd.call and behaviour-tree
// tasks reach it unchanged; its `lsd` object calls the Lua `lsd` table, so both languages share one API.
#include "lsd/foundation/types.h"

struct lua_State;

namespace lsd::script::js {

struct Host;
Host* create(lua_State* L);
void destroy(Host* host);  // after lua_close: Lua holds JS functions (timers) until then
// Runs `path` as instance `slot` and fills the Lua environment table on top of the stack with its
// functions (and its `properties` / `defaults` as tables). A reload keeps the slot's `self`. False on error (logged).
bool load(Host* host, u32 slot, const char* path);
bool is_js(const char* path);
// lsd.js_call(path, function, ...): what JavaScript nodes compile to. `host` is created on the first call.
void register_lua(lua_State* L, Host** host);
void update(Host* host);  // twice a second: node files that changed load again on their next call

} // namespace lsd::script::js
