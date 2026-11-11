#include "lsd/script/script.h"
#include "lsd/script/material_graph.h"

#include "js.h"

#include "lsd/asset/data_asset.h"
#include "lsd/asset/data_table.h"
#include "lsd/asset/guid.h"
#include "lsd/asset/localization.h"
#include "lsd/foundation/log.h"
#include "lsd/world/abilities.h"
#include "lsd/world/spline.h"
#include "lsd/world/user_types.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace lsd::script {

namespace {

i64 file_time(const char* path) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    return error ? 0 : static_cast<i64>(time.time_since_epoch().count());
}

// ---- Save games: Lua values <-> JSON ----

constexpr int kSaveVersion = 1;

void write_json_string(std::string& out, const char* text, usize length) {
    out += '"';
    for (usize i = 0; i < length; ++i) {
        const char c = text[i];
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char escape[8];
            std::snprintf(escape, sizeof escape, "\\u%04x", c);
            out += escape;
        } else {
            out += c;
        }
    }
    out += '"';
}

// The value at `index` as JSON; false for what cannot be saved (functions, cycles too deep).
bool write_json(lua_State* L, int index, std::string& out, int depth) {
    if (depth > 32) return false;
    index = index < 0 ? lua_gettop(L) + index + 1 : index;
    switch (lua_type(L, index)) {
        case LUA_TNIL: out += "null"; return true;
        case LUA_TBOOLEAN: out += lua_toboolean(L, index) ? "true" : "false"; return true;
        case LUA_TNUMBER: {
            char number[40];
            std::snprintf(number, sizeof number, "%.17g", lua_tonumber(L, index));
            out += number;
            return true;
        }
        case LUA_TSTRING: {
            size_t length = 0;
            const char* text = lua_tolstring(L, index, &length);
            write_json_string(out, text, length);
            return true;
        }
        case LUA_TTABLE: {
            const usize n = lua_objlen(L, index);
            usize keys = 0;
            lua_pushnil(L);
            while (lua_next(L, index)) {
                ++keys;
                lua_pop(L, 1);
            }
            if (keys == n && n > 0) {  // a list
                out += '[';
                for (usize i = 1; i <= n; ++i) {
                    if (i > 1) out += ',';
                    lua_rawgeti(L, index, static_cast<int>(i));
                    const bool ok = write_json(L, -1, out, depth + 1);
                    lua_pop(L, 1);
                    if (!ok) return false;
                }
                out += ']';
                return true;
            }
            out += '{';
            bool first = true;
            lua_pushnil(L);
            while (lua_next(L, index)) {
                lua_pushvalue(L, -2);  // the key as text (numbers too), without disturbing lua_next
                size_t length = 0;
                const char* key = lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER ? lua_tolstring(L, -1, &length) : nullptr;
                if (key) {
                    if (!first) out += ',';
                    first = false;
                    write_json_string(out, key, length);
                    out += ':';
                }
                lua_pop(L, 1);
                if (key && !write_json(L, -1, out, depth + 1)) {
                    lua_pop(L, 2);
                    return false;
                }
                lua_pop(L, 1);
            }
            out += '}';
            return true;
        }
        default: return false;
    }
}

// Pushes the JSON value at `c` (advanced past it); false on malformed text.
bool read_json(lua_State* L, const char*& c, int depth) {
    auto skip = [&] { while (*c == ' ' || *c == '\n' || *c == '\r' || *c == '\t') ++c; };
    skip();
    if (depth > 32) return false;
    if (*c == '{' || *c == '[') {
        const bool list = *c == '[';
        const char close = list ? ']' : '}';
        ++c;
        lua_newtable(L);
        skip();
        int i = 1;
        while (*c && *c != close) {
            if (!list) {
                if (!read_json(L, c, depth + 1) || lua_type(L, -1) != LUA_TSTRING) return false;  // key
                skip();
                if (*c++ != ':') return false;
            }
            if (!read_json(L, c, depth + 1)) return false;
            if (list) lua_rawseti(L, -2, i++);
            else lua_settable(L, -3);
            skip();
            if (*c == ',') ++c;
            skip();
        }
        if (*c != close) return false;
        ++c;
        return true;
    }
    if (*c == '"') {
        std::string text;
        for (++c; *c && *c != '"'; ++c) {
            if (*c != '\\') {
                text += *c;
                continue;
            }
            ++c;
            if (*c == 'n') text += '\n';
            else if (*c == 'u' && c[1] && c[2] && c[3] && c[4]) {
                char hex[5] = {c[1], c[2], c[3], c[4], 0};
                text += static_cast<char>(std::strtol(hex, nullptr, 16));  // shortcut: ASCII escapes only (what we write)
                c += 4;
            } else if (*c) text += *c;
        }
        if (*c != '"') return false;
        ++c;
        lua_pushlstring(L, text.data(), text.size());
        return true;
    }
    if (std::strncmp(c, "true", 4) == 0) { c += 4; lua_pushboolean(L, 1); return true; }
    if (std::strncmp(c, "false", 5) == 0) { c += 5; lua_pushboolean(L, 0); return true; }
    if (std::strncmp(c, "null", 4) == 0) { c += 4; lua_pushnil(L); return true; }
    char* end = nullptr;
    const double number = std::strtod(c, &end);
    if (end == c) return false;
    c = end;
    lua_pushnumber(L, number);
    return true;
}

// Saved/SaveGames/<slot>.json; slot names keep letters, digits, '-' and '_' only.
std::string save_path(const char* dir, const char* slot) {
    std::string name;
    for (const char* c = slot; *c && name.size() < 64; ++c) {
        if (std::isalnum(static_cast<unsigned char>(*c)) || *c == '-' || *c == '_') name += *c;
    }
    return std::string(dir) + "/" + (name.empty() ? std::string("Default") : name) + ".json";
}

int traceback(lua_State* L) {
    luaL_traceback(L, L, lua_tostring(L, 1), 1);
    return 1;
}

// pcall with a stack trace; logs and returns false on error. The function and its args are on the stack.
ScriptSystem* g_debugged = nullptr;  // the system whose line hook is set (one Lua state in Play)

bool protected_call(lua_State* L, int args, const char* what) {
    const int handler = lua_gettop(L) - args;
    lua_pushcfunction(L, traceback);
    lua_insert(L, handler);
    const int status = lua_pcall(L, args, 0, handler);
    if (status != 0) LSD_LOG_ERROR("script", "%s: %s", what, lua_tostring(L, -1));
    lua_settop(L, handler - 1);  // the handler and the error message (if any)
    return status == 0;
}

// The sandbox (run before any game script): scripts are content, possibly from mods, so they get no
// files, no processes and no native code. Saving goes through lsd.save_game; modules come from Content/.
constexpr char kSandbox[] = R"(
io = nil
os = {time = os.time, clock = os.clock, date = os.date, difftime = os.difftime}
debug = {traceback = debug.traceback}
dofile, loadfile = nil, nil
string.dump = nil
local raw_load = load
function load(chunk, name, mode, env) return raw_load(chunk, name, "t", env) end  -- text only: no bytecode tricks
loadstring = function(text, name) return raw_load(text, name, "t") end
package.loadlib = nil
package.cpath = ""
package.path = "Content/?.lua;Content/Scripts/?.lua"
package.loaded.io, package.loaded.os, package.loaded.debug, package.loaded.ffi = nil, os, debug, nil
package.preload.ffi = nil
for i = #package.loaders, 3, -1 do package.loaders[i] = nil end  -- keep preload and Lua files; drop C module searchers
)";

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
-- Multiplayer: the events run locally when this machine is the one they are for.
function lsd.has_authority() return lsd.net_mode() ~= "client" end
function lsd.server_event(name, ...)  -- Run on Server
  if lsd.net_mode() ~= "client" then return lsd.broadcast(name, lsd.player_id(), ...) end
  lsd._net_send(-1, name, {...})
end
function lsd.multicast(name, ...)  -- Multicast: the server tells everyone, itself too
  if lsd.net_mode() == "client" then lsd.log_error("multicast: only the server sends to everyone; clients use server_event") return end
  if lsd.net_mode() == "server" then lsd._net_send(-2, name, {...}) end
  return lsd.broadcast(name, 0, ...)
end
function lsd.client_event(player, name, ...)  -- Run on Owning Client
  if player == lsd.player_id() then return lsd.broadcast(name, 0, ...) end
  if lsd.net_mode() ~= "server" then lsd.log_error("client_event: only the server sends to a player") return end
  lsd._net_send(player, name, {...})
end
-- Curve tables (Unreal's Curve Table): a Data Table whose column headers are times.
function lsd.curve_table(name, row, time)
  local t = lsd.table(name)
  local r = t and t[row]
  if not r then return 0 end
  local keys = {}
  for k, v in pairs(r) do
    local at = tonumber(k)
    if at and type(v) == "number" then keys[#keys + 1] = {at, v} end
  end
  if #keys == 0 then return 0 end
  table.sort(keys, function(a, b) return a[1] < b[1] end)
  if time <= keys[1][1] then return keys[1][2] end
  for i = 2, #keys do
    if time <= keys[i][1] then
      local a, b = keys[i - 1], keys[i]
      return a[2] + (b[2] - a[2]) * (time - a[1]) / (b[1] - a[1])
    end
  end
  return keys[#keys][2]
end
-- Tables kept across level changes for the whole Play (plain data only: they travel as JSON).
local function kept(key)
  local t = lsd.__persist[key]
  if type(t) ~= "table" then t = {} lsd.__persist[key] = t end
  return t
end
function lsd.game_instance() return kept("game_instance") end
function lsd.player_state() return kept("player_state") end
function lsd.subsystem(name)
  local all = kept("subsystems")
  local t = all[name]
  if type(t) ~= "table" then t = {} all[name] = t end
  return t
end
-- Whole objects in a save game (Unreal's SaveGame-flagged properties): the chosen components of
-- each object (Transform when none are named), by object name, so loading into a freshly opened
-- level finds them again. `extra` rides along (score, inventory...) and comes back from load.
function lsd.save_objects(slot, objects, components, extra)
  components = components or {"Transform"}
  local saved = {}
  for _, e in ipairs(objects) do
    if lsd.alive(e) then
      local entry = {}
      for _, c in ipairs(components) do entry[c] = lsd.get(e, c) end
      saved[lsd.name(e)] = entry
    end
  end
  return lsd.save_game(slot, {objects = saved, extra = extra})
end
-- Puts saved components back on the objects with those names (adding missing components).
-- Returns the extra value and how many objects were found; objects no longer in the level are skipped.
function lsd.load_objects(slot)
  local data = lsd.load_game(slot)
  if type(data) ~= "table" or type(data.objects) ~= "table" then return nil, 0 end
  local found = 0
  for name, components in pairs(data.objects) do
    local e = lsd.find(name)
    if e then
      for c, fields in pairs(components) do
        if c == "Transform" then  -- through the setters, so physics bodies and children follow
          local p, r = fields.position, fields.rotation
          if p then lsd.set_position(e, p.x, p.y, p.z) end
          if r then lsd.set_rotation(e, r.x, r.y, r.z) end
          if fields.scale then lsd.set(e, "Transform", {scale = fields.scale}) end
        elseif lsd.has(e, c) then lsd.set(e, c, fields)
        else lsd.add(e, c, fields) end
      end
      found = found + 1
    end
  end
  return data.extra, found
end
-- Ready-made HUD widgets: one built-in document, an element per widget, rebuilt when the set changes.
local hud = {doc = nil, list = {}, order = {}, prompt = false}
local HUD_RML = [[<rml><head><style>
body { width: 100%; height: 100%; font-family: sans; }
div { display: block; }
.w { position: absolute; }
.bar { width: 84px; height: 10px; margin-left: -42px; margin-top: -5px; background-color: rgba(13, 4, 4, 210); border: 1px #000000; border-radius: 3px; }
.fill { height: 100%; width: 100%; background-color: #9BE11F; border-radius: 2px; }
.dmg { width: 160px; margin-left: -80px; margin-top: -16px; text-align: center; font-size: 28px; font-weight: bold; color: #FFE51A; }
.prompt { width: 640px; margin-left: -320px; text-align: center; font-size: 22px; color: #FFFFFF; padding: 6px 12px; background-color: rgba(13, 4, 4, 190); border-radius: 6px; }
#screen_prompt { position: absolute; left: 50%; bottom: 12%; display: none; }
</style></head><body><div id="widgets"></div><div id="screen_prompt" class="prompt"></div></body></rml>]]
local function hud_doc()
  if not hud.doc then hud.doc = lsd._ui_open_text(HUD_RML) end
  return hud.doc
end
local function hud_bar_color(w)
  if w.color then return w.color end
  local f = math.max(0, math.min(1, w.fraction))
  return string.format("#%02X%02X1F", math.floor(255 * (1 - f) + 155 * f), math.floor(60 * (1 - f) + 225 * f))
end
local function hud_apply(key, w)
  local doc = hud.doc
  if w.kind == "bar" then
    lsd.ui_style(doc, key .. "_fill", "width", string.format("%.1f%%", math.max(0, math.min(1, w.fraction)) * 100))
    lsd.ui_style(doc, key .. "_fill", "background-color", hud_bar_color(w))
  elseif w.kind == "dmg" then
    if w.color then lsd.ui_style(doc, key, "color", w.color) end
  end
  lsd.ui_follow(doc, key, w.e, w.height)
end
local function hud_rebuild()
  local doc = hud_doc()
  if not doc then return end
  local parts = {}
  for _, key in ipairs(hud.order) do
    local w = hud.list[key]
    if w.kind == "bar" then parts[#parts + 1] = '<div class="w bar" id="' .. key .. '"><div class="fill" id="' .. key .. '_fill"></div></div>'
    elseif w.kind == "dmg" then parts[#parts + 1] = '<div class="w dmg" id="' .. key .. '">' .. w.text .. '</div>'
    else parts[#parts + 1] = '<div class="w prompt" id="' .. key .. '">' .. w.text .. '</div>' end
  end
  lsd.ui_rml(doc, "widgets", table.concat(parts))
  for _, key in ipairs(hud.order) do hud_apply(key, hud.list[key]) end
end
local function hud_escape(text)
  return (string.gsub(tostring(text), "[<>&]", {["<"] = "&lt;", [">"] = "&gt;", ["&"] = "&amp;"}))
end
local function hud_add(key, w)
  if not hud.list[key] then hud.order[#hud.order + 1] = key end
  hud.list[key] = w
  hud_rebuild()
end
local function hud_remove(key)
  if not hud.list[key] then return end
  hud.list[key] = nil
  for i, k in ipairs(hud.order) do if k == key then table.remove(hud.order, i) break end end
  if hud.doc then lsd.ui_unfollow(hud.doc, key) end
  hud_rebuild()
end
local hud_number = 0
function lsd.health_bar(e, fraction, height, color)
  local key = "hb" .. tostring(e)
  local w = hud.list[key]
  if w then
    w.fraction, w.height, w.color = fraction, height or w.height, color or w.color
    if hud.doc then hud_apply(key, w) end
  else
    hud_add(key, {kind = "bar", e = e, fraction = fraction, height = height or 2.2, color = color})
  end
end
function lsd.remove_health_bar(e) hud_remove("hb" .. tostring(e)) end
function lsd.damage_number(e, amount, color)
  if not lsd.alive(e) then return end
  local live = 0
  for _, k in ipairs(hud.order) do if hud.list[k].kind == "dmg" then live = live + 1 end end
  if live >= 32 then return end  -- the oldest are still rising
  hud_number = hud_number + 1
  local x, y, z = lsd.position(e)
  hud_add("dn" .. hud_number, {kind = "dmg", e = e, text = hud_escape(amount), color = color, height = 2.0, born = lsd.time(), base = 2.0})
end
function lsd.prompt(text, e)
  if e then
    local key = "pr" .. tostring(e)
    if text == nil then hud_remove(key) else hud_add(key, {kind = "prompt", e = e, text = hud_escape(text), height = 2.4}) end
    return
  end
  local doc = hud_doc()
  if not doc then return end
  if text == nil then
    lsd.ui_style(doc, "screen_prompt", "display", "none")
  else
    lsd.ui_set_text(doc, "screen_prompt", tostring(text))
    lsd.ui_style(doc, "screen_prompt", "display", "block")
  end
end
-- UI actions handled once, engine-wide (before scripts' functions of that name): lsd.on_ui(action, function(element_id) end).
lsd._ui_actions = {}
function lsd.on_ui(action, fn) lsd._ui_actions[action] = fn end
-- A ready-made options menu (Unreal's Game User Settings in a menu): lsd.settings_menu() opens / closes it.
local settings_doc = nil
local SETTINGS_RML = [[<rml><head><style>
body { width: 100%; height: 100%; font-family: sans; color: #FFFFFF; background-color: rgba(0, 0, 0, 120); }
div { display: block; }
#panel { position: absolute; left: 50%; top: 50%; width: 560px; margin-left: -280px; margin-top: -330px; padding: 18px 24px;
         background-color: rgba(13, 4, 4, 235); border: 2px #F3C312; border-radius: 10px; }
#title { font-size: 34px; font-weight: bold; color: #FFE51A; margin-bottom: 12px; }
.row { margin: 8px 0; font-size: 20px; }
.label { display: inline-block; width: 220px; }
input.range { display: inline-block; width: 240px; height: 18px; nav: auto; tab-index: auto; focus: auto; }
input.range slidertrack { background-color: #3C3838; height: 6px; margin-top: 6px; }
input.range sliderbar { width: 16px; height: 18px; background-color: #F3C312; border-radius: 3px; }
.button { display: inline-block; padding: 4px 14px; margin-right: 6px; background-color: #025291; border-radius: 5px; nav: auto; tab-index: auto; focus: auto; }
.button:hover, .button:focus { background-color: #EEA70E; }
.button.on { background-color: #F3C312; color: #141414; }
#close { margin-top: 14px; padding: 6px 30px; background-color: #F3C312; color: #141414; font-weight: bold; }
</style></head><body><div id="panel">
<div id="title">Settings</div>
<div class="row"><span class="label">Quality</span><button class="button" id="q0" onclick="__lsd_quality">Low</button><button class="button" id="q1" onclick="__lsd_quality">Medium</button><button class="button" id="q2" onclick="__lsd_quality">High</button><button class="button" id="q3" onclick="__lsd_quality">Epic</button></div>
<div class="row"><span class="label">Fullscreen</span><button class="button" id="fullscreen" onclick="__lsd_toggle">Off</button></div>
<div class="row"><span class="label">Master volume</span><input type="range" class="range" id="master_volume" min="0" max="1" step="0.05" onchange="__lsd_slider"/></div>
<div class="row"><span class="label">Music</span><input type="range" class="range" id="music_volume" min="0" max="1" step="0.05" onchange="__lsd_slider"/></div>
<div class="row"><span class="label">Effects</span><input type="range" class="range" id="effects_volume" min="0" max="1" step="0.05" onchange="__lsd_slider"/></div>
<div class="row"><span class="label">Mouse sensitivity</span><input type="range" class="range" id="mouse_sensitivity" min="1" max="50" step="1" onchange="__lsd_slider"/></div>
<div class="row"><span class="label">Invert look</span><button class="button" id="invert_look" onclick="__lsd_toggle">Off</button></div>
<div class="row"><span class="label">Text size</span><input type="range" class="range" id="ui_text_scale" min="0.5" max="2" step="0.1" onchange="__lsd_slider"/></div>
<div class="row"><span class="label">Subtitles</span><button class="button" id="subtitles" onclick="__lsd_toggle">Off</button></div>
<div class="row"><span class="label">Colour blindness</span><button class="button" id="cb0" onclick="__lsd_colorblind">Off</button><button class="button" id="cb1" onclick="__lsd_colorblind">Deutan</button><button class="button" id="cb2" onclick="__lsd_colorblind">Protan</button><button class="button" id="cb3" onclick="__lsd_colorblind">Tritan</button></div>
<div class="row"><span class="label">Filter strength</span><input type="range" class="range" id="color_deficiency_strength" min="0" max="1" step="0.1" onchange="__lsd_slider"/></div>
<button class="button" id="close" onclick="__lsd_settings_close">Close</button>
</div></body></rml>]]
local function settings_show()
  local s = lsd.get_settings()
  if not s or not settings_doc then return end
  for q = 0, 3 do lsd.ui_set(settings_doc, "q" .. q, "class", s.quality == q and "button on" or "button") end
  for q = 0, 3 do lsd.ui_set(settings_doc, "cb" .. q, "class", s.color_deficiency == q and "button on" or "button") end
  for _, id in ipairs({"fullscreen", "invert_look", "subtitles"}) do
    lsd.ui_set_text(settings_doc, id, s[id] and "On" or "Off")
    lsd.ui_set(settings_doc, id, "class", s[id] and "button on" or "button")
  end
  for _, id in ipairs({"master_volume", "music_volume", "effects_volume", "ui_text_scale", "color_deficiency_strength"}) do lsd.ui_set(settings_doc, id, "value", tostring(s[id])) end
  lsd.ui_set(settings_doc, "mouse_sensitivity", "value", tostring(math.floor(s.mouse_sensitivity * 10 + 0.5)))  -- in tenths, whole steps
end
function lsd.settings_menu(open)
  if open == nil then open = settings_doc == nil end
  if open and not settings_doc then
    settings_doc = lsd._ui_open_text(SETTINGS_RML)
    settings_show()
    lsd.ui_show_cursor(true)
  elseif not open and settings_doc then
    lsd.ui_close(settings_doc)
    settings_doc = nil
    lsd.ui_show_cursor(false)
  end
  return settings_doc ~= nil
end
lsd.on_ui("__lsd_quality", function(id) lsd.set_settings({quality = tonumber(string.sub(id, 2))}) settings_show() end)
lsd.on_ui("__lsd_colorblind", function(id) lsd.set_settings({color_deficiency = tonumber(string.sub(id, 3))}) settings_show() end)
lsd.on_ui("__lsd_toggle", function(id) local s = lsd.get_settings() lsd.set_settings({[id] = not s[id]}) settings_show() end)
lsd.on_ui("__lsd_slider", function(id)
  local v = tonumber(lsd.ui_value(settings_doc, id))
  if v and id == "mouse_sensitivity" then v = v / 10 end  -- the slider is in tenths (whole steps)
  if v then lsd.set_settings({[id] = v}) end
end)
lsd.on_ui("__lsd_settings_close", function() lsd.settings_menu(false) end)
local function hud_tick(now)
  if #hud.order == 0 then return end
  local gone = {}
  for _, key in ipairs(hud.order) do
    local w = hud.list[key]
    if not lsd.alive(w.e) then gone[#gone + 1] = key
    elseif w.kind == "dmg" then
      local age = now - w.born
      if age >= 1.0 then gone[#gone + 1] = key
      else
        w.height = w.base + age * 1.2
        lsd.ui_follow(hud.doc, key, w.e, w.height)
        lsd.ui_style(hud.doc, key, "opacity", string.format("%.2f", 1.0 - age * age))
      end
    end
  end
  for _, key in ipairs(gone) do
    hud.list[key] = nil
    for i, k in ipairs(hud.order) do if k == key then table.remove(hud.order, i) break end end
    if hud.doc then lsd.ui_unfollow(hud.doc, key) end
  end
  if #gone > 0 then hud_rebuild() end
end
function lsd._tick()
  local now, due = lsd.time(), {}
  hud_tick(now)
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

struct ScriptSystem::CachedCurve {
    char path[260];
    i64 modified;
    world::CurveKeys channels[4];  // Curve: 1, CurveVector: 3, CurveColor: 4
    u32 channel_count;
};
constexpr u32 kMaxCurves = 32;

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
        static std::vector<u8> copy;  // reused: components with long lists (a Skeletal Mesh's morphs) pass 8 KB
        if (copy.size() < type.size) copy.resize(type.size);
        std::memcpy(copy.data(), data, type.size);
        read_struct(L, fields, type, copy.data());
        std::memcpy(world(L).get(e, type), copy.data(), type.size);
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
    static int spawn(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        const char* name = luaL_checkstring(L, 1);
        const world::Entity e = host.spawn ? host.spawn(host.user, name, check_vec3(L, 2)) : world::Entity{};
        if (e.valid()) lua_pushnumber(L, e.value);
        else lua_pushnil(L);
        return 1;
    }
    static int raycast(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        world::Entity hit;
        Vec3 point, normal;
        if (!host.raycast || !host.raycast(host.user, check_vec3(L, 1), check_vec3(L, 4), static_cast<f32>(luaL_checknumber(L, 7)), hit, point, normal)) {
            lua_pushnil(L);
            return 1;
        }
        if (hit.valid()) lua_pushnumber(L, hit.value);
        else lua_pushboolean(L, true);  // something without an object (should not happen)
        push_vec3(L, point);
        push_vec3(L, normal);
        return 7;
    }
    // Sweeps: from (1-3) to (4-6), then the shape's size; results like raycast.
    static int trace(lua_State* L, int shape, Vec3 size) {
        const ScriptHost& host = system(L).host_;
        world::Entity hit;
        Vec3 point, normal;
        if (!host.sweep || !host.sweep(host.user, shape, size, check_vec3(L, 1), check_vec3(L, 4), hit, point, normal)) {
            lua_pushnil(L);
            return 1;
        }
        if (hit.valid()) lua_pushnumber(L, hit.value);
        else lua_pushboolean(L, true);
        push_vec3(L, point);
        push_vec3(L, normal);
        return 7;
    }
    static int open_level(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        lua_pushboolean(L, host.open_level && host.open_level(host.user, luaL_checkstring(L, 1)));
        return 1;
    }
    static int player(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        const world::Entity e = host.player ? host.player(host.user) : world::Entity{};
        if (e.valid() && world(L).alive(e)) lua_pushnumber(L, e.value);
        else lua_pushnil(L);
        return 1;
    }
    static int sphere_trace(lua_State* L) { return trace(L, 0, {static_cast<f32>(luaL_checknumber(L, 7)), 0, 0}); }
    static int box_trace(lua_State* L) { return trace(L, 1, check_vec3(L, 7)); }
    static int capsule_trace(lua_State* L) { return trace(L, 2, {static_cast<f32>(luaL_checknumber(L, 7)), static_cast<f32>(luaL_checknumber(L, 8)), 0}); }
    // Overlaps: a list of the objects touching the shape at (1-3).
    static int overlap(lua_State* L, int shape, Vec3 size) {
        const ScriptHost& host = system(L).host_;
        world::Entity found[64];
        const u32 n = host.overlap ? host.overlap(host.user, shape, size, check_vec3(L, 1), found, 64) : 0;
        lua_createtable(L, static_cast<int>(n), 0);
        for (u32 i = 0; i < n; ++i) {
            lua_pushnumber(L, found[i].value);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }
    static int overlap_sphere(lua_State* L) { return overlap(L, 0, {static_cast<f32>(luaL_checknumber(L, 4)), 0, 0}); }
    static int overlap_box(lua_State* L) { return overlap(L, 1, check_vec3(L, 4)); }
    static int spline_point(lua_State* L) {
        Vec3 points[32], tangents[32];
        bool closed = false;
        const u32 n = world::spline_world_curve(world(L), live_entity(L, 1), points, tangents, closed);
        if (n == 0) luaL_argerror(L, 1, "has no Spline");
        Vec3 tangent;
        push_vec3(L, world::spline_at_distance(points, n, closed, static_cast<f32>(luaL_checknumber(L, 2)), &tangent, tangents));
        const f32 len = length(tangent);
        push_vec3(L, len > 0.0f ? tangent * (1.0f / len) : Vec3{1, 0, 0});
        return 6;
    }
    static int spline_length(lua_State* L) {
        Vec3 points[32], tangents[32];
        bool closed = false;
        const u32 n = world::spline_world_curve(world(L), live_entity(L, 1), points, tangents, closed);
        lua_pushnumber(L, n ? world::spline_length(points, n, closed, tangents) : 0.0);
        return 1;
    }
    static int play_sound(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        const char* name = luaL_checkstring(L, 1);
        const bool spatial = lua_gettop(L) >= 4;
        const Vec3 at = spatial ? check_vec3(L, 2) : Vec3{};
        lua_pushnumber(L, host.play_sound ? host.play_sound(host.user, name, spatial, at) : 0);
        return 1;
    }
    static int text(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        const char* key = luaL_checkstring(L, 1);
        const char* pattern = host.text ? host.text(host.user, key) : key;
        const int extra = lua_gettop(L) - 1;
        if (extra <= 0) {
            lua_pushstring(L, pattern);
            return 1;
        }
        const char* args[10] = {};
        const u32 count = static_cast<u32>(extra < 10 ? extra : 10);
        for (u32 i = 0; i < count; ++i) args[i] = luaL_tolstring_compat(L, static_cast<int>(i) + 2);  // numbers and booleans too
        char out[1024];
        asset::format_text(pattern, args, count, host.language ? host.language(host.user) : "en", out, sizeof out);
        lua_pushstring(L, out);
        return 1;
    }
    static int set_language(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.set_language) host.set_language(host.user, luaL_checkstring(L, 1));
        return 0;
    }
    // lsd.broadcast(name, ...) / lsd.call(e, name, ...): name(self, ...) on every script (or e's).
    static int dispatch(lua_State* L, bool one) {
        ScriptSystem& sys = system(L);
        const int name_index = one ? 2 : 1;
        const world::Entity target = one ? entity(L, 1) : world::Entity{};
        const char* name = luaL_checkstring(L, name_index);
        const int extra = lua_gettop(L) - name_index;
        int called = 0;
        for (u32 i = 0; i < sys.count_; ++i) {
            ScriptSystem::Instance& instance = sys.instances_[i];
            if (instance.failed || !sys.world_->alive(instance.entity) || (one && instance.entity != target)) continue;
            lua_rawgeti(L, LUA_REGISTRYINDEX, instance.env);
            lua_getfield(L, -1, name);
            lua_remove(L, -2);
            if (!lua_isfunction(L, -1)) {
                lua_pop(L, 1);
                continue;
            }
            lua_rawgeti(L, LUA_REGISTRYINDEX, instance.self);
            for (int k = 1; k <= extra; ++k) lua_pushvalue(L, name_index + k);
            char what[300];
            std::snprintf(what, sizeof what, "%s (%s)", instance.path, name);
            if (!protected_call(L, 1 + extra, what)) instance.failed = true;
            ++called;
        }
        world::Entity other;
        if (extra > 0 && lua_type(L, name_index + 1) == LUA_TNUMBER) other.value = static_cast<u32>(lua_tonumber(L, name_index + 1));
        called += static_cast<int>(sys.native_event(target, name, other, !one));
        lua_pushinteger(L, called);
        return 1;
    }
    static int debug_shape(lua_State* L, int shape, int numbers) {  // numbers before the optional colour / seconds
        const ScriptHost& host = system(L).host_;
        f32 v[6] = {};
        for (int i = 0; i < numbers; ++i) v[i] = static_cast<f32>(luaL_checknumber(L, i + 1));
        int next = numbers + 1;
        const char* text = shape == 3 ? luaL_checkstring(L, next++) : nullptr;
        const Vec3 color{static_cast<f32>(luaL_optnumber(L, next, 1.0)), static_cast<f32>(luaL_optnumber(L, next + 1, 0.0)), static_cast<f32>(luaL_optnumber(L, next + 2, 0.0))};
        const f32 seconds = static_cast<f32>(luaL_optnumber(L, next + 3, 0.0));
        if (host.draw_debug) host.draw_debug(host.user, shape, Vec3{v[0], v[1], v[2]}, Vec3{v[3], v[4], v[5]}, v[3], text, color, seconds);
        return 0;
    }
    static int draw_line(lua_State* L) { return debug_shape(L, 0, 6); }
    static int draw_sphere(lua_State* L) { return debug_shape(L, 1, 4); }
    static int draw_box(lua_State* L) { return debug_shape(L, 2, 6); }
    static int draw_text(lua_State* L) { return debug_shape(L, 3, 3); }
    static int user_enum(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        char path[260];
        world::UserEnum e;
        if (!host.data_path || !host.data_path(host.user, "UserEnum", luaL_checkstring(L, 1), path, sizeof path) || !asset::load_data_asset(path, e)) {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, static_cast<int>(e.values.count), static_cast<int>(e.values.count));
        for (u32 i = 0; i < e.values.count; ++i) {
            lua_pushnumber(L, i);
            lua_setfield(L, -2, e.values[i].name);
            lua_pushstring(L, e.values[i].name);
            lua_rawseti(L, -2, static_cast<int>(i));
        }
        return 1;
    }
    static int user_struct(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        char path[260];
        world::UserStruct st;
        if (!host.data_path || !host.data_path(host.user, "UserStruct", luaL_checkstring(L, 1), path, sizeof path) || !asset::load_data_asset(path, st)) {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, static_cast<int>(st.fields.count));
        for (const world::StructField& f : st.fields) {
            if (f.type == 0) {
                lua_pushnumber(L, std::strtod(f.default_value, nullptr));
            } else if (f.type == 1) {
                lua_pushboolean(L, std::strcmp(f.default_value, "true") == 0);
            } else if (f.type == 2) {
                lua_pushstring(L, f.default_value);
            } else {
                double v[3] = {0, 0, 0};
                std::sscanf(f.default_value, "%lf , %lf , %lf", &v[0], &v[1], &v[2]);
                lua_createtable(L, 0, 3);
                lua_pushnumber(L, v[0]);
                lua_setfield(L, -2, "x");
                lua_pushnumber(L, v[1]);
                lua_setfield(L, -2, "y");
                lua_pushnumber(L, v[2]);
                lua_setfield(L, -2, "z");
            }
            lua_setfield(L, -2, f.name);
        }
        return 1;
    }
    static int socket(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        Vec3 p, f;
        if (!host.socket || !host.socket(host.user, live_entity(L, 1), luaL_checkstring(L, 2), p, f)) {
            lua_pushnil(L);
            return 1;
        }
        push_vec3(L, p);
        push_vec3(L, f);
        return 6;
    }
    static int play_timeline(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        lua_pushboolean(L, host.timeline && host.timeline(host.user, 0, luaL_checkstring(L, 1), lua_toboolean(L, 2) != 0));
        return 1;
    }
    static int stop_timeline(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.timeline) host.timeline(host.user, 1, luaL_checkstring(L, 1), false);
        return 0;
    }
    static int play_music(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.music) host.music(host.user, 0, luaL_checkstring(L, 1), 1.0f, static_cast<f32>(luaL_optnumber(L, 2, 1.0)));
        return 0;
    }
    static int stop_music(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.music) host.music(host.user, 1, "", 0.0f, static_cast<f32>(luaL_optnumber(L, 1, 1.0)));
        return 0;
    }
    static int music_layer(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.music) host.music(host.user, 2, luaL_checkstring(L, 1), static_cast<f32>(luaL_checknumber(L, 2)), static_cast<f32>(luaL_optnumber(L, 3, 1.0)));
        return 0;
    }
    static int music_intensity(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.music) host.music(host.user, 3, "", static_cast<f32>(luaL_checknumber(L, 1)), 0.0f);
        return 0;
    }
    static int music_segment(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.music) host.music(host.user, 4, luaL_checkstring(L, 1), 0.0f, 0.0f);
        return 0;
    }
    static int trigger_audio(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.trigger_audio) host.trigger_audio(host.user, luaL_checkstring(L, 1));
        return 0;
    }
    static int ik(lua_State* L, bool aim) {
        const ScriptHost& host = system(L).host_;
        const world::Entity e = live_entity(L, 1);
        const char* bone = luaL_checkstring(L, 2);
        const Vec3 target{static_cast<f32>(luaL_checknumber(L, 3)), static_cast<f32>(luaL_checknumber(L, 4)), static_cast<f32>(luaL_checknumber(L, 5))};
        const f32 weight = static_cast<f32>(luaL_optnumber(L, 6, 1.0));
        const Vec3 forward{static_cast<f32>(luaL_optnumber(L, 7, 1.0)), static_cast<f32>(luaL_optnumber(L, 8, 0.0)), static_cast<f32>(luaL_optnumber(L, 9, 0.0))};
        if (host.ik) host.ik(host.user, e, bone, target, weight, aim, forward);
        return 0;
    }
    static int ik_reach(lua_State* L) { return ik(L, false); }
    static int ik_look_at(lua_State* L) { return ik(L, true); }
    static int set_view(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        const world::Entity e = lua_isnoneornil(L, 1) ? world::Entity{} : entity(L, 1);
        if (host.set_view) host.set_view(host.user, e, static_cast<f32>(luaL_optnumber(L, 2, 0.0)));
        return 0;
    }
    static int camera_shake(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.camera_shake) {
            host.camera_shake(host.user, static_cast<f32>(luaL_checknumber(L, 1)), static_cast<f32>(luaL_optnumber(L, 2, 0.5)), static_cast<f32>(luaL_optnumber(L, 3, 12.0)));
        }
        return 0;
    }
    static int set_camera_view(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        const char* view = luaL_checkstring(L, 1);
        const u32 v = std::strcmp(view, "first") == 0 ? 0u : std::strcmp(view, "third") == 0 ? 1u : 2u;
        if (v == 2 && std::strcmp(view, "camera") != 0) return luaL_argerror(L, 1, "\"first\", \"third\" or \"camera\"");
        if (host.set_camera_view) host.set_camera_view(host.user, v);
        return 0;
    }
    static int camera_view(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        lua_pushstring(L, host.camera_view && host.camera_view(host.user) == 1 ? "third" : "first");
        return 1;
    }
    static int camera_location(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        push_vec3(L, host.camera_location ? host.camera_location(host.user) : Vec3{});
        return 3;
    }
    static int camera_forward(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        push_vec3(L, host.camera_forward ? host.camera_forward(host.user) : Vec3{1, 0, 0});
        return 3;
    }
    static int touch_count(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        lua_pushnumber(L, host.touch_count ? static_cast<lua_Number>(host.touch_count(host.user)) : 0.0);
        return 1;
    }
    static int touch(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        const lua_Number n = luaL_checknumber(L, 1);
        f32 x = 0, y = 0;
        if (n < 1 || !host.touch_point || !host.touch_point(host.user, static_cast<u32>(n) - 1, &x, &y)) return 0;
        lua_pushnumber(L, static_cast<lua_Number>(x));
        lua_pushnumber(L, static_cast<lua_Number>(y));
        return 2;
    }
    static int broadcast(lua_State* L) { return dispatch(L, false); }
    static int ride_call(lua_State* L, int what, bool target) {
        const ScriptHost& host = system(L).host_;
        const world::Entity e = target ? entity(L, 1) : world::Entity{};
        const u32 result = host.ride ? host.ride(host.user, what, e) : 0;
        if (what == 2) {
            if (result) lua_pushnumber(L, result);
            else lua_pushnil(L);
        } else {
            lua_pushboolean(L, result != 0);
        }
        return 1;
    }
    static int sublevel_call(lua_State* L, bool load) {
        const ScriptHost& host = system(L).host_;
        const world::Entity e = entity(L, 1);
        lua_pushboolean(L, host.sublevel && host.sublevel(host.user, e, load));
        return 1;
    }
    static int load_sublevel(lua_State* L) { return sublevel_call(L, true); }
    static int input_recording(lua_State* L, int what) {
        const ScriptHost& host = system(L).host_;
        const char* file = what == 2 ? "" : luaL_checkstring(L, 1);
        lua_pushboolean(L, host.input_recording && host.input_recording(host.user, what, file));
        return 1;
    }
    static int input_record(lua_State* L) { return input_recording(L, 0); }
    static int input_play(lua_State* L) { return input_recording(L, 1); }
    static int input_stop(lua_State* L) { return input_recording(L, 2); }
    static int unload_sublevel(lua_State* L) { return sublevel_call(L, false); }
    static int enter_vehicle(lua_State* L) { return ride_call(L, 0, true); }
    static int exit_vehicle(lua_State* L) { return ride_call(L, 1, false); }
    static int vehicle(lua_State* L) { return ride_call(L, 2, false); }
    static int ride(lua_State* L) { return ride_call(L, 3, true); }
    static int stop_riding(lua_State* L) { return ride_call(L, 4, false); }
    static int start_mover(lua_State* L) { return ride_call(L, 5, true); }
    static int stop_mover(lua_State* L) { return ride_call(L, 6, true); }
    static int restart_mover(lua_State* L) { return ride_call(L, 7, true); }
    static int net_mode(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        const int mode = host.net_mode ? host.net_mode(host.user) : 0;
        lua_pushstring(L, mode == 1 ? "server" : mode == 2 ? "client" : "standalone");
        return 1;
    }
    static int player_id(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        lua_pushnumber(L, host.net_player ? host.net_player(host.user) : 0);
        return 1;
    }
    static int net_send(lua_State* L) {  // lsd._net_send(target, name, {args}): the prelude's events call it
        const ScriptHost& host = system(L).host_;
        const i32 target = static_cast<i32>(luaL_checknumber(L, 1));
        const char* name = luaL_checkstring(L, 2);
        std::string json;
        if (!lua_istable(L, 3) || !write_json(L, 3, json, 0)) return luaL_error(L, "%s: its arguments must be numbers, text, booleans or tables", name);
        if (host.net_send) host.net_send(host.user, target, name, json.c_str());
        return 0;
    }
    static int chat(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        const char* channel = luaL_checkstring(L, 1);
        const char* text = luaL_checkstring(L, 2);
        lua_pushboolean(L, host.net_chat && host.net_chat(host.user, channel, text));
        return 1;
    }
    static int set_team(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.net_set_group) host.net_set_group(host.user, static_cast<u32>(luaL_checknumber(L, 1)), static_cast<u32>(luaL_checknumber(L, 2)), static_cast<u32>(luaL_optnumber(L, 3, 0)));
        return 0;
    }
    static int team(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        u32 t = 0, squad = 0;
        if (host.net_group) host.net_group(host.user, static_cast<u32>(luaL_checknumber(L, 1)), t, squad);
        lua_pushnumber(L, t);
        lua_pushnumber(L, squad);
        return 2;
    }
    static int net_seen_position(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        Vec3 p;
        if (!host.net_seen_position || !host.net_seen_position(host.user, live_entity(L, 1).value, static_cast<u32>(luaL_checknumber(L, 2)), p)) return 0;
        lua_pushnumber(L, p.x);
        lua_pushnumber(L, p.y);
        lua_pushnumber(L, p.z);
        return 3;
    }
    static int net_session(lua_State* L, int what, const char* arg) {
        const ScriptHost& host = system(L).host_;
        char why[160] = "multiplayer is not available here";
        const bool ok = host.net_session && host.net_session(host.user, what, arg, why, sizeof why);
        lua_pushboolean(L, ok);
        if (ok) return 1;
        lua_pushstring(L, why);
        return 2;
    }
    static int net_host(lua_State* L) { return net_session(L, 0, luaL_optstring(L, 1, "")); }
    static int editor_workspace(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        lua_pushstring(L, host.editor_workspace ? host.editor_workspace(host.user) : "");
        return 1;
    }
    static int editor_call(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (!host.editor) return luaL_error(L, "lsd.editor: editor utility scripts run only in the editor, not in Play or a game");
        static char out[64 * 1024];
        out[0] = 0;
        if (!host.editor(host.user, luaL_checkstring(L, 1), luaL_optstring(L, 2, ""), out, sizeof out)) lua_pushnil(L);
        else lua_pushstring(L, out);
        return 1;
    }
    static int net_join(lua_State* L) { return net_session(L, 1, luaL_checkstring(L, 1)); }
    static int net_leave(lua_State* L) { return net_session(L, 2, luaL_optstring(L, 1, "")); }
    static int net_join_code(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        char text[2048] = "";
        if (host.net_join_codes) host.net_join_codes(host.user, text, sizeof text);
        lua_createtable(L, 0, 4);
        lua_createtable(L, 4, 0);  // lan
        int lan = 0;
        for (const char* line = text; *line;) {
            const char* end = std::strchr(line, '\n');
            const std::string row(line, end ? static_cast<usize>(end - line) : std::strlen(line));
            line = end ? end + 1 : line + row.size();
            const usize a = row.find('|'), b = row.find('|', a + 1), c = row.find('|', b + 1);
            if (a == std::string::npos || b == std::string::npos) continue;
            if (row.compare(0, a, "upnp") == 0 && c != std::string::npos) {
                lua_pushstring(L, row.substr(a + 1, b - a - 1).c_str());
                lua_setfield(L, -3, "upnp");
                lua_pushstring(L, row.substr(b + 1, c - b - 1).c_str());
                lua_setfield(L, -3, "message");
                if (c + 1 < row.size()) {
                    lua_pushstring(L, row.substr(c + 1).c_str());
                    lua_setfield(L, -3, "internet");
                }
            } else if (row.compare(0, a, "lan") == 0) {
                const std::string address = row.substr(a + 1, b - a - 1);
                lua_createtable(L, 0, 3);
                lua_pushstring(L, address.c_str());
                lua_setfield(L, -2, "address");
                lua_pushstring(L, row.substr(b + 1).c_str());
                lua_setfield(L, -2, "code");
                unsigned x = 0, y = 0;
                lua_pushboolean(L, std::sscanf(address.c_str(), "%u.%u", &x, &y) == 2 && x == 100 && (y & 0xC0u) == 64u);  // Tailscale's range
                lua_setfield(L, -2, "tailscale");
                lua_rawseti(L, -2, ++lan);
            }
        }
        lua_setfield(L, -2, "lan");
        return 1;
    }
    static int net_find_games(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        static char text[8192];
        text[0] = '\0';
        const u32 n = host.net_find_games ? host.net_find_games(host.user, static_cast<f32>(luaL_optnumber(L, 1, 0.5)), text, sizeof text) : 0;
        lua_createtable(L, static_cast<int>(n), 0);
        int k = 0;
        for (const char* line = text; *line && k < static_cast<int>(n);) {
            const char* end = std::strchr(line, '\n');
            const std::string row(line, end ? static_cast<usize>(end - line) : std::strlen(line));
            line = end ? end + 1 : line + row.size();
            const usize a = row.find('|'), b = row.find('|', a + 1), c = row.find('|', b + 1);
            if (a == std::string::npos || b == std::string::npos || c == std::string::npos) continue;
            lua_createtable(L, 0, 4);
            lua_pushstring(L, row.substr(0, a).c_str());
            lua_setfield(L, -2, "address");
            lua_pushstring(L, row.substr(a + 1, b - a - 1).c_str());
            lua_setfield(L, -2, "name");
            lua_pushnumber(L, std::atof(row.substr(b + 1, c - b - 1).c_str()));
            lua_setfield(L, -2, "players");
            lua_pushnumber(L, std::atof(row.substr(c + 1).c_str()));
            lua_setfield(L, -2, "max_players");
            lua_rawseti(L, -2, ++k);
        }
        return 1;
    }
    static int call_on(lua_State* L) { return dispatch(L, true); }
    // lsd.native_call(name, ...) -> result: a function the game module registered (C / C++ nodes).
    static int native_call(lua_State* L) {
        ScriptSystem& sys = system(L);
        const char* name = luaL_checkstring(L, 1);
        const lsd_function* f = sys.module_ ? sys.module_->find_function(name) : nullptr;
        if (!f) return luaL_error(L, "no C / C++ function %s (compile the game code?)", name);
        lsd_value args[8] = {};
        const int count = f->input_count < 0 ? 0 : f->input_count < 8 ? f->input_count : 8;
        for (int i = 0; i < count; ++i) {
            const int at = i + 2;
            lsd_value& v = args[i];
            switch (f->inputs[i].type) {
            case LSD_VALUE_BOOL: v.number = lua_toboolean(L, at); break;
            case LSD_VALUE_TEXT: v.text = lua_isstring(L, at) ? lua_tostring(L, at) : ""; break;
            case LSD_VALUE_OBJECT: v.object = static_cast<lsd_entity>(lua_tonumber(L, at)); break;
            case LSD_VALUE_VECTOR:
                if (lua_istable(L, at)) {
                    f32* xyz[3] = {&v.vector.x, &v.vector.y, &v.vector.z};
                    for (int k = 0; k < 3; ++k) {
                        lua_getfield(L, at, k == 0 ? "x" : k == 1 ? "y" : "z");
                        *xyz[k] = static_cast<f32>(lua_tonumber(L, -1));
                        lua_pop(L, 1);
                    }
                }
                break;
            default: v.number = lua_tonumber(L, at);
            }
        }
        lsd_value result = {};
        f->call(args, count, &result);
        switch (f->output_type) {
        case LSD_VALUE_NONE: return 0;
        case LSD_VALUE_BOOL: lua_pushboolean(L, result.number != 0); break;
        case LSD_VALUE_TEXT: lua_pushstring(L, result.text ? result.text : ""); break;
        case LSD_VALUE_OBJECT:
            if (result.object) lua_pushnumber(L, result.object);
            else lua_pushnil(L);
            break;
        case LSD_VALUE_VECTOR:  // as the graph's vectors: {x, y, z}
            lua_createtable(L, 0, 3);
            lua_pushnumber(L, result.vector.x);
            lua_setfield(L, -2, "x");
            lua_pushnumber(L, result.vector.y);
            lua_setfield(L, -2, "y");
            lua_pushnumber(L, result.vector.z);
            lua_setfield(L, -2, "z");
            break;
        default: lua_pushnumber(L, result.number);
        }
        return 1;
    }
    static int implements(lua_State* L) {  // Blueprint interfaces: does e's script define name?
        ScriptSystem& sys = system(L);
        const world::Entity target = entity(L, 1);
        const char* name = luaL_checkstring(L, 2);
        bool found = false;
        for (u32 i = 0; i < sys.count_ && !found; ++i) {
            ScriptSystem::Instance& instance = sys.instances_[i];
            if (instance.entity != target || instance.failed) continue;
            lua_rawgeti(L, LUA_REGISTRYINDEX, instance.env);
            lua_getfield(L, -1, name);
            found = lua_isfunction(L, -1);
            lua_pop(L, 2);
        }
        lua_pushboolean(L, found);
        return 1;
    }
    static int languages(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        const char* codes[32];
        const u32 n = host.languages ? host.languages(host.user, codes, 32) : 0;
        lua_createtable(L, static_cast<int>(n), 0);
        for (u32 i = 0; i < n; ++i) {
            lua_pushstring(L, codes[i]);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }
    static int bindings(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        const char* keys[64];
        const u32 n = host.bindings ? host.bindings(host.user, luaL_checkstring(L, 1), keys, 64) : 0;
        lua_createtable(L, static_cast<int>(n), 0);
        for (u32 i = 0; i < n; ++i) {
            lua_pushstring(L, keys[i]);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }
    static int rebind(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        lua_pushboolean(L, host.rebind && host.rebind(host.user, luaL_checkstring(L, 1), luaL_checkstring(L, 2), luaL_checkstring(L, 3)));
        return 1;
    }
    static int key_pressed(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        const char* key = host.key_pressed ? host.key_pressed(host.user) : nullptr;
        if (key) lua_pushstring(L, key);
        else lua_pushnil(L);
        return 1;
    }
    static int get_settings(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        const reflect::TypeInfo* type = nullptr;
        const void* data = host.settings ? host.settings(host.user, &type) : nullptr;
        if (!data || !type) return 0;
        push_struct(L, *type, static_cast<const u8*>(data));
        return 1;
    }
    static int set_settings(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        luaL_checktype(L, 1, LUA_TTABLE);
        const reflect::TypeInfo* type = nullptr;
        void* data = host.settings ? host.settings(host.user, &type) : nullptr;
        if (!data || !type) return 0;
        read_struct(L, 1, *type, static_cast<u8*>(data));
        if (host.settings_changed) host.settings_changed(host.user);
        return 0;
    }
    static int ui_open(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        const i32 doc = host.ui_open ? host.ui_open(host.user, luaL_checkstring(L, 1)) : -1;
        if (doc < 0) return 0;
        lua_pushinteger(L, doc);
        return 1;
    }
    static int ui_close(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.ui_close && !lua_isnil(L, 1)) host.ui_close(host.user, static_cast<i32>(luaL_checkinteger(L, 1)));
        return 0;
    }
    static int ui_set_text(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        lua_pushboolean(L, host.ui_set && host.ui_set(host.user, static_cast<i32>(luaL_checkinteger(L, 1)), luaL_checkstring(L, 2), nullptr, luaL_tolstring_compat(L, 3)));
        return 1;
    }
    static const char* luaL_tolstring_compat(lua_State* L, int index) {  // numbers and booleans as text
        if (lua_isboolean(L, index)) return lua_toboolean(L, index) ? "true" : "false";
        const char* s = lua_tostring(L, index);
        return s ? s : "";
    }
    static int ui_set(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        lua_pushboolean(L, host.ui_set && host.ui_set(host.user, static_cast<i32>(luaL_checkinteger(L, 1)), luaL_checkstring(L, 2), luaL_checkstring(L, 3),
                                                      luaL_tolstring_compat(L, 4)));
        return 1;
    }
    static int ui_data(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (!host.ui_data) return 0;
        const char* name = luaL_checkstring(L, 1);
        if (lua_isboolean(L, 2)) host.ui_data(host.user, name, 2, lua_toboolean(L, 2) ? 1.0 : 0.0, "");
        else if (lua_type(L, 2) == LUA_TNUMBER) host.ui_data(host.user, name, 0, lua_tonumber(L, 2), "");
        else if (lua_istable(L, 2)) {  // a list (data-for) or record: handed over as JSON
            std::string json;
            if (!write_json(L, 2, json, 0)) return luaL_error(L, "ui_data: a list holds numbers, strings, booleans and tables only");
            host.ui_data(host.user, name, 3, 0.0, json.c_str());
        }
        else host.ui_data(host.user, name, 1, 0.0, luaL_tolstring_compat(L, 2));
        return 0;
    }
    static int ui_value(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        char text[512];
        if (!host.ui_value || !host.ui_value(host.user, static_cast<i32>(luaL_checkinteger(L, 1)), luaL_checkstring(L, 2), text, sizeof text)) return 0;
        lua_pushstring(L, text);
        return 1;
    }
    static int ui_style(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        lua_pushboolean(L, host.ui_style && host.ui_style(host.user, static_cast<i32>(luaL_checkinteger(L, 1)), luaL_checkstring(L, 2), luaL_checkstring(L, 3),
                                                          luaL_tolstring_compat(L, 4)));
        return 1;
    }
    static int ui_play_animation(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        lua_pushboolean(L, host.ui_animate && host.ui_animate(host.user, static_cast<i32>(luaL_checkinteger(L, 1)), luaL_checkstring(L, 2), nullptr, luaL_checkstring(L, 3), 0.0f, nullptr));
        return 1;
    }
    static int ui_animate(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        lua_pushboolean(L, host.ui_animate && host.ui_animate(host.user, static_cast<i32>(luaL_checkinteger(L, 1)), luaL_checkstring(L, 2), luaL_checkstring(L, 3),
                                                              luaL_tolstring_compat(L, 4), static_cast<f32>(luaL_optnumber(L, 5, 0.3)), luaL_optstring(L, 6, nullptr)));
        return 1;
    }
    static int ui_rml(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        lua_pushboolean(L, host.ui_rml && host.ui_rml(host.user, static_cast<i32>(luaL_checkinteger(L, 1)), luaL_checkstring(L, 2), luaL_checkstring(L, 3)));
        return 1;
    }
    static int ui_open_text(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        const i32 doc = host.ui_open_text ? host.ui_open_text(host.user, luaL_checkstring(L, 1)) : -1;
        if (doc < 0) lua_pushnil(L);
        else lua_pushinteger(L, doc);
        return 1;
    }
    static int ui_follow(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.ui_follow) host.ui_follow(host.user, static_cast<i32>(luaL_checkinteger(L, 1)), luaL_checkstring(L, 2), live_entity(L, 3), static_cast<f32>(luaL_optnumber(L, 4, 2.0)));
        return 0;
    }
    static int ui_unfollow(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.ui_follow) host.ui_follow(host.user, static_cast<i32>(luaL_checkinteger(L, 1)), luaL_checkstring(L, 2), world::Entity{}, 0.0f);
        return 0;
    }
    static int ui_show_cursor(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.ui_show_cursor) host.ui_show_cursor(host.user, lua_toboolean(L, 1) != 0);
        return 0;
    }
    static int bb_set(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        const world::Entity e = live_entity(L, 1);
        const char* key = luaL_checkstring(L, 2);
        const bool vector = lua_gettop(L) >= 5;
        f32 v[3];
        v[0] = static_cast<f32>(lua_isboolean(L, 3) ? (lua_toboolean(L, 3) ? 1.0 : 0.0) : luaL_checknumber(L, 3));
        v[1] = vector ? static_cast<f32>(luaL_checknumber(L, 4)) : v[0];
        v[2] = vector ? static_cast<f32>(luaL_checknumber(L, 5)) : v[0];
        if (host.bb_set) host.bb_set(host.user, e, key, v, vector);
        return 0;
    }
    static int bb_get(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        f32 v[3] = {};
        const int n = host.bb_get ? host.bb_get(host.user, live_entity(L, 1), luaL_checkstring(L, 2), v) : 0;
        for (int i = 0; i < n; ++i) lua_pushnumber(L, v[i]);
        return n;
    }
    static int bb_clear(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.bb_clear) host.bb_clear(host.user, live_entity(L, 1), luaL_checkstring(L, 2));
        return 0;
    }
    static int report_noise(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        const Vec3 at{static_cast<f32>(luaL_checknumber(L, 1)), static_cast<f32>(luaL_checknumber(L, 2)), static_cast<f32>(luaL_checknumber(L, 3))};
        if (host.report_noise) host.report_noise(host.user, at, static_cast<f32>(luaL_optnumber(L, 4, 1.0)));
        return 0;
    }
    static int claim_smart_object(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        Vec3 slot;
        const world::Entity object = host.claim_smart_object
            ? host.claim_smart_object(host.user, live_entity(L, 1), luaL_optstring(L, 2, ""), static_cast<f32>(luaL_optnumber(L, 3, 15.0)), slot)
            : world::Entity{};
        if (!object.valid()) {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, object.value);
        push_vec3(L, slot);
        return 4;
    }
    static int release_smart_object(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.release_smart_object) host.release_smart_object(host.user, live_entity(L, 1));
        return 0;
    }
    static int run_query(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        Vec3 best;
        if (!host.run_query || !host.run_query(host.user, live_entity(L, 1), luaL_checkstring(L, 2), best)) {
            lua_pushnil(L);
            return 1;
        }
        push_vec3(L, best);
        return 3;
    }
    static int apply_damage(lua_State* L) {
        ScriptSystem& s = system(L);
        const world::Entity target = entity(L, 1);
        const f32 amount = static_cast<f32>(luaL_checknumber(L, 2));
        world::Entity instigator;
        if (!lua_isnoneornil(L, 3)) instigator = entity(L, 3);
        const f64 args[2] = {amount, instigator.valid() ? static_cast<f64>(instigator.value) : std::nan("")};  // none: nil
        s.send(target, "on_damage", args, 2, instigator);
        if (s.host_.damaged) s.host_.damaged(s.host_.user, target, amount, instigator);
        return 0;
    }

    // ---- Ability system (rules in lsd/world/abilities.h) ----
    // shortcut: runs wherever it is called and nothing is replicated (single player, or server-side scripts
    // guarded by lsd.has_authority()); replicate AttributeSet / AbilitySystem when multiplayer games need it.
    // A GameplayEffect / GameplayAbility asset by name or GUID; `name` gets its asset name (the file's stem).
    static bool load_gameplay(ScriptSystem& s, const char* data_type, const char* key, const reflect::TypeInfo& type, void* out, char* name, usize capacity) {
        char path[260];
        if (!key[0] || !s.host_.data_path || !s.host_.data_path(s.host_.user, data_type, key, path, sizeof path) || !asset::load_data_asset(path, type, out)) return false;
        const char* stem = path;
        for (const char* p = path; *p; ++p) {
            if (*p == '/' || *p == '\\') stem = p + 1;
        }
        const char* dot = std::strrchr(stem, '.');
        std::snprintf(name, capacity, "%.*s", static_cast<int>(dot ? dot - stem : static_cast<std::ptrdiff_t>(std::strlen(stem))), stem);
        return true;
    }
    struct AttributeValues {
        u32 count;
        f32 value[16];
    };
    static AttributeValues attribute_values(world::World& w, world::Entity e) {
        AttributeValues v{};
        if (world::AttributeSet* set = w.get<world::AttributeSet>(e)) {
            world::recompute(*set);  // edited base values have no current one until something computes it
            v.count = set->attributes.count;
            for (u32 i = 0; i < v.count; ++i) v.value[i] = set->attributes[i].current;
        }
        return v;
    }
    // lsd.call(e, event, text, numbers... [, flag]): an event on e's scripts and native behaviours.
    static void send_event(ScriptSystem& s, world::Entity e, const char* event, const char* text, const f64* numbers, int count, int flag = -1) {
        lua_State* L = s.lua_;
        lua_getglobal(L, "lsd");
        lua_getfield(L, -1, "call");
        lua_remove(L, -2);
        lua_pushnumber(L, e.value);
        lua_pushstring(L, event);
        lua_pushstring(L, text);
        for (int i = 0; i < count; ++i) lua_pushnumber(L, numbers[i]);
        if (flag >= 0) lua_pushboolean(L, flag);
        protected_call(L, 3 + count + (flag >= 0 ? 1 : 0), event);
    }
    // on_attribute_changed(self, name, value, old) for each current value that changed since `before`.
    static void notify_attributes(ScriptSystem& s, world::Entity e, const AttributeValues& before) {
        const world::AttributeSet* set = s.world_->get<world::AttributeSet>(e);
        if (!set) return;
        struct Change {
            char name[32];
            f64 values[2];
        } changes[16];
        u32 n = 0;
        for (u32 i = 0; i < set->attributes.count; ++i) {
            const f32 old = i < before.count ? before.value[i] : 0.0f;
            if (i < before.count && set->attributes[i].current == old) continue;
            std::snprintf(changes[n].name, sizeof changes[n].name, "%s", set->attributes[i].name);
            changes[n].values[0] = set->attributes[i].current;
            changes[n++].values[1] = old;
        }
        for (u32 i = 0; i < n; ++i) send_event(s, e, "on_attribute_changed", changes[i].name, changes[i].values, 2);  // scripts may change the world: copied first
    }
    static bool apply_effect_to(ScriptSystem& s, world::Entity target, const char* key) {
        world::World& w = *s.world_;
        world::GameplayEffect effect;
        char name[64];
        if (!w.alive(target) || !load_gameplay(s, "GameplayEffect", key, reflect::type_of<world::GameplayEffect>(), &effect, name, sizeof name)) return false;
        const AttributeValues before = attribute_values(w, target);
        if (!world::apply_effect(*w.add<world::AttributeSet>(target), effect, name)) {
            LSD_LOG_WARN("script", "apply_effect: %s has 16 effects already, %s not applied", w.name(target), name);
            return false;
        }
        // Gameplay Cue: a sound and / or particles at the target.
        const Mat4 m = w.world_matrix(target);
        const Vec3 at{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
        if (effect.cue_sound[0] && s.host_.play_sound) s.host_.play_sound(s.host_.user, effect.cue_sound, true, at);
        if (effect.cue_effect[0] && s.host_.spawn) s.host_.spawn(s.host_.user, effect.cue_effect, at);
        notify_attributes(s, target, before);
        return true;
    }
    static int apply_effect(lua_State* L) {
        const world::Entity e = entity(L, 1);
        lua_pushboolean(L, apply_effect_to(system(L), e, luaL_checkstring(L, 2)));
        return 1;
    }
    static int remove_effect(lua_State* L) {
        ScriptSystem& s = system(L);
        const world::Entity e = live_entity(L, 1);
        const char* name = luaL_checkstring(L, 2);
        const AttributeValues before = attribute_values(*s.world_, e);
        world::AttributeSet* set = s.world_->get<world::AttributeSet>(e);
        const u32 removed = set ? world::remove_effect(*set, name) : 0;
        if (removed) notify_attributes(s, e, before);
        lua_pushnumber(L, removed);
        return 1;
    }
    static int attribute(lua_State* L) {
        const world::Entity e = live_entity(L, 1);
        const char* name = luaL_checkstring(L, 2);
        world::AttributeSet* set = world(L).get<world::AttributeSet>(e);
        if (set) world::recompute(*set);
        const world::Attribute* a = set ? world::find_attribute(*set, name) : nullptr;
        if (!a) {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, a->current);
        lua_pushnumber(L, a->base);
        return 2;
    }
    static int set_attribute(lua_State* L) {
        ScriptSystem& s = system(L);
        const world::Entity e = live_entity(L, 1);
        const char* name = luaL_checkstring(L, 2);
        const f32 value = static_cast<f32>(luaL_checknumber(L, 3));
        const AttributeValues before = attribute_values(*s.world_, e);
        world::AttributeSet& set = *s.world_->add<world::AttributeSet>(e);
        if (!world::find_attribute(set, name)) {
            world::Attribute fresh;
            std::snprintf(fresh.name, sizeof fresh.name, "%s", name);
            if (!set.attributes.push_back(fresh)) return luaL_error(L, "set_attribute: %s has 16 attributes already", s.world_->name(e));
        }
        world::GameplayEffect set_base;  // an Instant Override: the base changes and is clamped like any effect's
        std::snprintf(set_base.modifiers.items[0].attribute, sizeof set_base.modifiers.items[0].attribute, "%s", name);
        set_base.modifiers.items[0].op = static_cast<u32>(world::ModifierOp::Override);
        set_base.modifiers.items[0].magnitude = value;
        set_base.modifiers.count = 1;
        world::apply_effect(set, set_base, "");
        notify_attributes(s, e, before);
        return 0;
    }
    // The slot of e's granted ability `name` (asset name, or the GUID Details stored); its name filled in. -1 if none.
    static i32 ability_slot(ScriptSystem& s, world::Entity e, const char* name) {
        world::AbilitySystem* abilities = s.world_->get<world::AbilitySystem>(e);
        for (u32 i = 0; abilities && i < abilities->abilities.count; ++i) {
            world::GrantedAbility& g = abilities->abilities[i];
            if (!g.name[0]) {
                world::GameplayAbility unused;
                load_gameplay(s, "GameplayAbility", g.ability, reflect::type_of<world::GameplayAbility>(), &unused, g.name, sizeof g.name);
            }
            if (std::strcmp(g.name, name) == 0 || std::strcmp(g.ability, name) == 0) return static_cast<i32>(i);
        }
        return -1;
    }
    static world::GrantedAbility* granted(ScriptSystem& s, world::Entity e, i32 slot) {
        world::AbilitySystem* abilities = slot >= 0 ? s.world_->get<world::AbilitySystem>(e) : nullptr;
        return abilities && static_cast<u32>(slot) < abilities->abilities.count ? &abilities->abilities[static_cast<u32>(slot)] : nullptr;
    }
    static int give_ability(lua_State* L) {
        ScriptSystem& s = system(L);
        const world::Entity e = live_entity(L, 1);
        world::GameplayAbility ability;
        world::GrantedAbility g;
        if (!load_gameplay(s, "GameplayAbility", luaL_checkstring(L, 2), reflect::type_of<world::GameplayAbility>(), &ability, g.name, sizeof g.name)) {
            lua_pushboolean(L, 0);
            return 1;
        }
        lua_pushboolean(L, ability_slot(s, e, g.name) >= 0 || s.world_->add<world::AbilitySystem>(e)->abilities.push_back(g));
        return 1;
    }
    static int try_activate_ability(lua_State* L) {
        ScriptSystem& s = system(L);
        world::World& w = *s.world_;
        const world::Entity e = live_entity(L, 1);
        const i32 slot = ability_slot(s, e, luaL_checkstring(L, 2));
        world::GameplayAbility ability;
        char name[64];
        const world::GrantedAbility* g = granted(s, e, slot);
        if (!g || !load_gameplay(s, "GameplayAbility", g->ability[0] ? g->ability : g->name, reflect::type_of<world::GameplayAbility>(), &ability, name, sizeof name)) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, "no such ability");
            return 2;
        }
        world::GameplayEffect cost, cooldown;
        char unused[64];
        const bool has_cost = load_gameplay(s, "GameplayEffect", ability.cost_effect, reflect::type_of<world::GameplayEffect>(), &cost, unused, sizeof unused);
        const bool has_cooldown = load_gameplay(s, "GameplayEffect", ability.cooldown_effect, reflect::type_of<world::GameplayEffect>(), &cooldown, unused, sizeof unused);
        char tags[1024];
        world::owned_tags(w, e, tags, sizeof tags);
        world::AttributeSet* set = w.get<world::AttributeSet>(e);
        if (set) world::recompute(*set);
        const world::AbilityBlock block = world::can_activate(ability, *g, tags, has_cooldown ? cooldown.granted_tags : "", set, has_cost ? &cost : nullptr);
        if (block != world::AbilityBlock::None) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, world::to_string(block));
            return 2;
        }
        if (has_cost) apply_effect_to(s, e, ability.cost_effect);
        if (has_cooldown) apply_effect_to(s, e, ability.cooldown_effect);
        world::GrantedAbility* state = granted(s, e, slot);  // again: on_attribute_changed may have changed the world
        if (state) {
            state->active = true;
            state->cooldown_left = ability.cooldown;
            state->active_left = ability.active_seconds;
            std::snprintf(state->active_tags, sizeof state->active_tags, "%s", ability.active_tags);
            state->montage = false;
            if (ability.montage[0] && s.host_.play_montage) {
                const f32 seconds = s.host_.play_montage(s.host_.user, e, ability.montage, 1.0f, "", 0.2f, false, "Default");
                state->montage = seconds > 0.0f;
                if (ability.active_seconds <= 0.0f) state->active_left = seconds;
            }
        }
        if (ability.function[0]) send_event(s, e, ability.function, name, nullptr, 0);
        lua_pushboolean(L, 1);
        return 1;
    }
    static int finish_ability(lua_State* L, bool cancelled) {
        ScriptSystem& s = system(L);
        const world::Entity e = live_entity(L, 1);
        world::GrantedAbility* g = granted(s, e, ability_slot(s, e, luaL_checkstring(L, 2)));
        if (!g || !g->active) {
            lua_pushboolean(L, 0);
            return 1;
        }
        g->active = false;
        g->active_tags[0] = '\0';
        if (cancelled && g->montage && s.host_.stop_montage) s.host_.stop_montage(s.host_.user, e, 0.2f, "Default");
        char name[64];
        std::snprintf(name, sizeof name, "%s", g->name);
        send_event(s, e, "on_ability_ended", name, nullptr, 0, cancelled ? 1 : 0);
        lua_pushboolean(L, 1);
        return 1;
    }
    static int end_ability(lua_State* L) { return finish_ability(L, false); }
    static int cancel_ability(lua_State* L) { return finish_ability(L, true); }
    static int ability_cooldown(lua_State* L) {
        ScriptSystem& s = system(L);
        const world::Entity e = live_entity(L, 1);
        const world::GrantedAbility* g = granted(s, e, ability_slot(s, e, luaL_checkstring(L, 2)));
        lua_pushnumber(L, g ? g->cooldown_left : 0.0f);
        return 1;
    }
    // Every frame before on_update: effect timers and periodic ticks, ability cooldowns and active time.
    static void tick_gameplay(ScriptSystem& s, f32 dt) {
        world::World& w = *s.world_;
        constexpr u32 kMax = 256;  // shortcut: up to 256 objects with effects (and 256 with abilities) tick a frame, enough for now
        world::Entity due[kMax];
        u32 n = 0;
        const reflect::TypeInfo& sets = reflect::type_of<world::AttributeSet>();
        for (u32 i = 0; i < w.count(sets) && n < kMax; ++i) {
            if (static_cast<const world::AttributeSet*>(w.at(sets, i))->effects.count) due[n++] = w.owner(sets, i);
        }
        for (u32 i = 0; i < n; ++i) {  // owners first: scripts called back may add or destroy objects
            const AttributeValues before = attribute_values(w, due[i]);
            world::AttributeSet* set = w.alive(due[i]) ? w.get<world::AttributeSet>(due[i]) : nullptr;
            if (set && world::tick_effects(*set, dt)) notify_attributes(s, due[i], before);
        }
        n = 0;
        const reflect::TypeInfo& systems = reflect::type_of<world::AbilitySystem>();
        for (u32 i = 0; i < w.count(systems) && n < kMax; ++i) due[n++] = w.owner(systems, i);
        for (u32 i = 0; i < n; ++i) {
            world::AbilitySystem* abilities = w.alive(due[i]) ? w.get<world::AbilitySystem>(due[i]) : nullptr;
            if (!abilities) continue;
            u32 ended[16];
            char names[16][64];
            const u32 k = world::tick_abilities(*abilities, dt, ended, 16);
            for (u32 j = 0; j < k; ++j) std::snprintf(names[j], sizeof names[j], "%s", abilities->abilities[ended[j]].name);
            for (u32 j = 0; j < k; ++j) send_event(s, due[i], "on_ability_ended", names[j], nullptr, 0, 0);
        }
    }
    static int rebuild_navmesh(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        lua_pushboolean(L, host.rebuild_navmesh && host.rebuild_navmesh(host.user));
        return 1;
    }
    static int move_to(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        const Vec3 target{static_cast<f32>(luaL_checknumber(L, 2)), static_cast<f32>(luaL_checknumber(L, 3)), static_cast<f32>(luaL_checknumber(L, 4))};
        lua_pushboolean(L, host.move_to && host.move_to(host.user, live_entity(L, 1), target));
        return 1;
    }
    static int stop_move(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.stop_move) host.stop_move(host.user, live_entity(L, 1));
        return 0;
    }
    static int is_moving(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        lua_pushboolean(L, host.is_moving && host.is_moving(host.user, live_entity(L, 1)));
        return 1;
    }
    static int find_path(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        const Vec3 from{static_cast<f32>(luaL_checknumber(L, 1)), static_cast<f32>(luaL_checknumber(L, 2)), static_cast<f32>(luaL_checknumber(L, 3))};
        const Vec3 to{static_cast<f32>(luaL_checknumber(L, 4)), static_cast<f32>(luaL_checknumber(L, 5)), static_cast<f32>(luaL_checknumber(L, 6))};
        Vec3 points[64];
        const u32 n = host.find_path ? host.find_path(host.user, from, to, points, 64) : 0;
        lua_createtable(L, static_cast<int>(n), 0);
        for (u32 i = 0; i < n; ++i) {
            lua_createtable(L, 0, 3);
            lua_pushnumber(L, points[i].x);
            lua_setfield(L, -2, "x");
            lua_pushnumber(L, points[i].y);
            lua_setfield(L, -2, "y");
            lua_pushnumber(L, points[i].z);
            lua_setfield(L, -2, "z");
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }
    static int stop_sound(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.stop_sound) host.stop_sound(host.user, static_cast<u32>(luaL_checknumber(L, 1)));
        return 0;
    }
    static int push_mix(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.push_mix) host.push_mix(host.user, luaL_checkstring(L, 1));
        return 0;
    }
    static int pop_mix(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.pop_mix) host.pop_mix(host.user, luaL_checkstring(L, 1));
        return 0;
    }
    static int set_audio_param(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.set_audio_param) host.set_audio_param(host.user, luaL_checkstring(L, 1), static_cast<f32>(luaL_checknumber(L, 2)));
        return 0;
    }
    static int play_montage(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        const f32 seconds = host.play_montage ? host.play_montage(host.user, entity(L, 1), luaL_checkstring(L, 2), static_cast<f32>(luaL_optnumber(L, 3, 1.0)),
                                                                  luaL_optstring(L, 4, ""), static_cast<f32>(luaL_optnumber(L, 5, 0.2)), lua_toboolean(L, 6) != 0,
                                                                  luaL_optstring(L, 7, "Default")) : 0.0f;
        lua_pushnumber(L, seconds);
        return 1;
    }
    static int stop_montage(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.stop_montage) host.stop_montage(host.user, entity(L, 1), static_cast<f32>(luaL_optnumber(L, 2, 0.2)), luaL_optstring(L, 3, ""));
        return 0;
    }
    static int montage_playing(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        lua_pushboolean(L, host.montage_playing && host.montage_playing(host.user, entity(L, 1), luaL_optstring(L, 2, "")));
        return 1;
    }
    static int set_motion_tag(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.set_motion_tag) host.set_motion_tag(host.user, entity(L, 1), luaL_optstring(L, 2, ""));
        return 0;
    }
    static int anim_curve(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        f32 value = 0.0f;
        if (host.anim_curve) host.anim_curve(host.user, entity(L, 1), luaL_checkstring(L, 2), value);
        lua_pushnumber(L, value);
        return 1;
    }
    // A global material parameter (Project Settings > Materials): one number for all four values, or x, y, z [, w].
    static int set_material_parameter(lua_State* L) {
        const f32 x = static_cast<f32>(luaL_checknumber(L, 2));
        const f32 value[4] = {x, static_cast<f32>(luaL_optnumber(L, 3, x)), static_cast<f32>(luaL_optnumber(L, 4, x)), static_cast<f32>(luaL_optnumber(L, 5, x))};
        lua_pushboolean(L, set_global_param(luaL_checkstring(L, 1), value));
        return 1;
    }
    static int ragdoll(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        lua_pushboolean(L, host.ragdoll && host.ragdoll(host.user, entity(L, 1), lua_isnoneornil(L, 2) || lua_toboolean(L, 2)));
        return 1;
    }
    static int set_anim_param(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.set_anim_param) host.set_anim_param(host.user, entity(L, 1), luaL_checkstring(L, 2), static_cast<f32>(luaL_checknumber(L, 3)));
        return 0;
    }
    // A morph target weight on e's skeletal mesh, or on the first one below it (a Character's mesh).
    static int set_morph(lua_State* L) {
        world::World& w = world(L);
        const world::Entity e = live_entity(L, 1);
        world::SkeletalMesh* mesh = w.get<world::SkeletalMesh>(e);
        for (u32 slot = 0; !mesh && slot < w.slot_count(); ++slot) {
            const world::Entity c = w.entity_at_slot(slot);
            if (!c.valid() || !w.get<world::SkeletalMesh>(c)) continue;
            for (world::Entity p = w.parent(c); p.valid() && !mesh; p = w.parent(p)) mesh = p == e ? w.get<world::SkeletalMesh>(c) : nullptr;
        }
        if (!mesh) return 0;
        const char* name = luaL_checkstring(L, 2);
        const f32 weight = static_cast<f32>(luaL_checknumber(L, 3));
        for (world::MorphWeight& m : mesh->morphs) {
            if (std::strcmp(m.name, name) != 0) continue;
            m.weight = weight;
            return 0;
        }
        if (mesh->morphs.count == mesh->morphs.capacity()) return luaL_error(L, "%s already sets %u morph targets", w.name(e), mesh->morphs.count);
        world::MorphWeight m;
        std::snprintf(m.name, sizeof m.name, "%s", name);
        m.weight = weight;
        mesh->morphs.push_back(m);
        return 0;
    }
    // Splits host "a|b|c" lines into a list of tables with the given keys.
    static int push_lines(lua_State* L, const char* text, const char* const* keys, int key_count) {
        lua_newtable(L);
        int n = 0;
        for (const char* line = text; *line;) {
            const char* end = std::strchr(line, '\n');
            if (!end) end = line + std::strlen(line);
            lua_newtable(L);
            const char* field = line;
            for (int k = 0; k < key_count; ++k) {
                const char* bar = static_cast<const char*>(std::memchr(field, '|', static_cast<usize>(end - field)));
                const char* stop = bar && k + 1 < key_count ? bar : end;
                lua_pushlstring(L, field, static_cast<usize>(stop - field));
                lua_setfield(L, -2, keys[k]);
                field = stop < end ? stop + 1 : end;
            }
            lua_rawseti(L, -2, ++n);
            line = *end ? end + 1 : end;
        }
        return 1;
    }
    static int assets(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        static char text[64 * 1024];
        text[0] = 0;
        if (host.assets) host.assets(host.user, luaL_checkstring(L, 1), luaL_optstring(L, 2, ""), text, sizeof text);
        static const char* const kKeys[] = {"name", "guid", "folder"};
        return push_lines(L, text, kKeys, 3);
    }
    static int morph_names(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        static char text[16 * 1024];
        text[0] = 0;
        if (host.morph_names) host.morph_names(host.user, live_entity(L, 1), text, sizeof text);
        lua_newtable(L);
        int n = 0;
        for (const char* line = text; *line;) {
            const char* end = std::strchr(line, '\n');
            if (!end) end = line + std::strlen(line);
            lua_pushlstring(L, line, static_cast<usize>(end - line));
            lua_rawseti(L, -2, ++n);
            line = *end ? end + 1 : end;
        }
        return 1;
    }
    static int set_outfit(lua_State* L) {
        world::World& w = world(L);
        const world::Entity e = live_entity(L, 1);
        const char* slot = luaL_checkstring(L, 2);
        const char* mesh = luaL_checkstring(L, 3);
        char guid[33] = "";
        asset::Guid parsed;
        if (asset::parse_guid(mesh, parsed)) std::snprintf(guid, sizeof guid, "%s", mesh);
        else if (mesh[0]) {  // by name
            const ScriptHost& host = system(L).host_;
            static char text[64 * 1024];
            text[0] = 0;
            if (host.assets) host.assets(host.user, "Mesh", "", text, sizeof text);
            for (const char* line = text; *line && !guid[0];) {
                const char* bar = std::strchr(line, '|');
                const char* end = std::strchr(line, '\n');
                if (!end) end = line + std::strlen(line);
                if (bar && bar < end && static_cast<usize>(bar - line) == std::strlen(mesh) && std::strncmp(line, mesh, std::strlen(mesh)) == 0)
                    std::snprintf(guid, sizeof guid, "%.32s", bar + 1);
                line = *end ? end + 1 : end;
            }
            if (!guid[0]) return luaL_error(L, "set_outfit: no mesh named '%s'", mesh);
        }
        auto* outfit = w.get<world::Outfit>(e) ? w.get<world::Outfit>(e) : w.add<world::Outfit>(e);
        world::OutfitSlot* target = nullptr;
        for (world::OutfitSlot& s : outfit->slots) {
            if (std::strcmp(s.slot, slot) == 0) target = &s;
        }
        if (!target) {
            if (outfit->slots.count == outfit->slots.capacity()) return luaL_error(L, "set_outfit: %s has no room for slot %s", w.name(e), slot);
            world::OutfitSlot s;
            std::snprintf(s.slot, sizeof s.slot, "%s", slot);
            outfit->slots.push_back(s);
            target = &outfit->slots[outfit->slots.count - 1];
        }
        std::snprintf(target->mesh, sizeof target->mesh, "%s", guid);
        if (lua_gettop(L) >= 6) target->color = {static_cast<f32>(luaL_checknumber(L, 4)), static_cast<f32>(luaL_checknumber(L, 5)), static_cast<f32>(luaL_checknumber(L, 6)), 1.0f};
        return 0;
    }
    static int set_class_volume(lua_State* L) {
        const ScriptHost& host = system(L).host_;
        if (host.set_class_volume) host.set_class_volume(host.user, luaL_checkstring(L, 1), static_cast<f32>(luaL_checknumber(L, 2)));
        return 0;
    }
    // Background saves (Unreal's AsyncSaveGameToSlot): one pending write per file; anything touching that file waits for it.
    static std::mutex& pending_mutex() {
        static std::mutex m;
        return m;
    }
    static std::map<std::string, std::shared_future<bool>>& pending_saves() {
        static std::map<std::string, std::shared_future<bool>> pending;
        return pending;
    }
    static void wait_for_save(const std::string& path) {
        std::shared_future<bool> f;
        {
            std::lock_guard<std::mutex> lock(pending_mutex());
            auto it = pending_saves().find(path);
            if (it == pending_saves().end()) return;
            f = it->second;
            pending_saves().erase(it);
        }
        f.wait();
    }
    static bool write_save(const std::string& dir, const std::string& path, const std::string& json) {
        std::error_code error;
        std::filesystem::create_directories(dir, error);
        const std::string temp = path + ".tmp";  // written whole, then swapped in: a crash never leaves half a save
        FILE* file = std::fopen(temp.c_str(), "wb");
        const bool ok = file && std::fwrite(json.data(), 1, json.size(), file) == json.size();
        if (file) std::fclose(file);
        if (ok) std::filesystem::rename(temp, path, error);
        if (!ok || error) LSD_LOG_WARN("script", "save_game: could not write %s", path.c_str());
        return ok && !error;
    }
    // The save file's text: the engine's format version, the game's own (`version`), then the value.
    static bool save_json(lua_State* L, std::string& json) {
        luaL_checkany(L, 2);
        const lua_Integer version = luaL_optinteger(L, 3, 1);
        json = "{\"lsd_save\": " + std::to_string(kSaveVersion) + ", \"version\": " + std::to_string(static_cast<long long>(version)) + ", \"data\": ";
        if (!write_json(L, 2, json, 0)) return false;
        json += "}\n";
        return true;
    }
    static int save_game(lua_State* L) {
        const std::string path = save_path(system(L).save_dir_, luaL_checkstring(L, 1));
        std::string json;
        if (!save_json(L, json)) return luaL_error(L, "save_game: only numbers, strings, booleans and tables can be saved");
        wait_for_save(path);
        lua_pushboolean(L, write_save(system(L).save_dir_, path, json));
        return 1;
    }
    static int save_game_async(lua_State* L) {
        const std::string path = save_path(system(L).save_dir_, luaL_checkstring(L, 1));
        std::string json;
        if (!save_json(L, json)) return luaL_error(L, "save_game_async: only numbers, strings, booleans and tables can be saved");
        wait_for_save(path);  // an older write of this slot lands first
        std::shared_future<bool> f = std::async(std::launch::async, write_save, system(L).save_dir_, path, std::move(json)).share();
        {
            std::lock_guard<std::mutex> lock(pending_mutex());
            pending_saves()[path] = f;
        }
        lua_pushboolean(L, 1);
        return 1;
    }
    static int load_game(lua_State* L) {
        const std::string path = save_path(system(L).save_dir_, luaL_checkstring(L, 1));
        wait_for_save(path);
        FILE* file = std::fopen(path.c_str(), "rb");
        if (!file) return 0;
        std::string text;
        char buffer[4096];
        for (usize n; (n = std::fread(buffer, 1, sizeof buffer, file)) > 0;) text.append(buffer, n);
        std::fclose(file);
        const char* data = std::strstr(text.c_str(), "\"data\":");
        const char* at = data ? data + 7 : nullptr;
        const int top = lua_gettop(L);
        if (!at || !read_json(L, at, 0)) {
            lua_settop(L, top);
            LSD_LOG_WARN("script", "load_game: %s is damaged", path.c_str());
            return 0;
        }
        const char* version = std::strstr(text.c_str(), "\"version\":");
        lua_pushinteger(L, version && version < data ? std::atoll(version + 10) : 1);  // saves from before versions: 1
        return 2;
    }
    static int has_save(lua_State* L) {
        std::error_code error;
        const std::string path = save_path(system(L).save_dir_, luaL_checkstring(L, 1));
        wait_for_save(path);
        lua_pushboolean(L, std::filesystem::exists(path, error));
        return 1;
    }
    static int delete_save(lua_State* L) {
        std::error_code error;
        const std::string path = save_path(system(L).save_dir_, luaL_checkstring(L, 1));
        wait_for_save(path);
        lua_pushboolean(L, std::filesystem::remove(path, error));
        return 1;
    }
    static int curve(lua_State* L) {
        ScriptSystem& self = system(L);
        const ScriptHost& host = self.host_;
        const char* name = luaL_checkstring(L, 1);
        const f32 time = static_cast<f32>(luaL_checknumber(L, 2));
        char path[260];
        if (!host.curve_path || !host.curve_path(host.user, name, path, sizeof path)) {
            lua_pushnumber(L, 0);
            return 1;
        }
        const i64 modified = file_time(path);
        ScriptSystem::CachedCurve* slot = nullptr;
        for (u32 i = 0; i < self.curve_count_ && !slot; ++i) {
            if (std::strcmp(self.curves_[i].path, path) == 0) slot = &self.curves_[i];
        }
        if (!slot && self.curve_count_ < kMaxCurves) {
            slot = &self.curves_[self.curve_count_++];
            std::snprintf(slot->path, sizeof slot->path, "%s", path);
            slot->modified = -1;
        }
        if (!slot) luaL_error(L, "too many curves in use");
        if (slot->modified != modified) {  // first use, or the asset was saved again: read it as its type
            char type[32] = "";
            asset::read_data_asset_type(path, type, sizeof type);
            slot->channel_count = 0;
            if (std::strcmp(type, "CurveVector") == 0) {
                world::CurveVector v;
                if (asset::load_data_asset(path, v)) {
                    slot->channels[0] = v.x, slot->channels[1] = v.y, slot->channels[2] = v.z;
                    slot->channel_count = 3;
                }
            } else if (std::strcmp(type, "CurveColor") == 0) {
                world::CurveColor c;
                if (asset::load_data_asset(path, c)) {
                    slot->channels[0] = c.r, slot->channels[1] = c.g, slot->channels[2] = c.b, slot->channels[3] = c.a;
                    slot->channel_count = 4;
                }
            } else {
                world::Curve c;
                if (asset::load_data_asset(path, c)) {
                    slot->channels[0] = c.keys;
                    slot->channel_count = 1;
                }
            }
            for (u32 k = 0; k < slot->channel_count; ++k) world::sort_keys(slot->channels[k]);
            slot->modified = modified;
        }
        if (!slot->channel_count) {
            lua_pushnumber(L, 0);
            return 1;
        }
        for (u32 k = 0; k < slot->channel_count; ++k) lua_pushnumber(L, world::evaluate(slot->channels[k], time));
        return static_cast<int>(slot->channel_count);
    }
    static int table(lua_State* L) {
        ScriptSystem& self = system(L);
        const ScriptHost& host = self.host_;
        char path[260];
        if (!host.table_path || !host.table_path(host.user, luaL_checkstring(L, 1), path, sizeof path)) {
            lua_pushnil(L);
            return 1;
        }
        const i64 modified = file_time(path);
        ScriptSystem::CachedTable* slot = nullptr;
        for (u32 i = 0; i < self.table_count_ && !slot; ++i) {
            if (std::strcmp(self.tables_[i].path, path) == 0) slot = &self.tables_[i];
        }
        if (slot && slot->modified == modified) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, slot->ref);
            return 1;
        }
        asset::DataTable data(default_allocator());
        if (!data.load(path)) {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, static_cast<int>(data.rows()));
        for (u32 r = 1; r < data.rows(); ++r) {
            lua_createtable(L, 0, static_cast<int>(data.columns()));
            for (u32 c = 1; c < data.columns(); ++c) {
                const char* text = data.cell(r, c);
                char column[128];
                const asset::ColumnType type = asset::column_type(data.cell(0, c), column, sizeof column);  // "Damage:number"
                char* stop = nullptr;
                const double number = std::strtod(text, &stop);
                const bool numeric = text[0] && stop && *stop == '\0';
                double v[3] = {0, 0, 0};
                switch (type) {
                    case asset::ColumnType::String: lua_pushstring(L, text); break;  // "007" stays text
                    case asset::ColumnType::Bool: lua_pushboolean(L, std::strcmp(text, "true") == 0 || std::strcmp(text, "1") == 0); break;
                    case asset::ColumnType::Number:
                    case asset::ColumnType::Int: lua_pushnumber(L, numeric ? number : 0.0); break;
                    case asset::ColumnType::Vector:
                        std::sscanf(text, "%lf , %lf , %lf", &v[0], &v[1], &v[2]);
                        lua_createtable(L, 0, 3);
                        lua_pushnumber(L, v[0]);
                        lua_setfield(L, -2, "x");
                        lua_pushnumber(L, v[1]);
                        lua_setfield(L, -2, "y");
                        lua_pushnumber(L, v[2]);
                        lua_setfield(L, -2, "z");
                        break;
                    case asset::ColumnType::Auto:  // "12.5" is a number, "12 mm" text
                        if (numeric) lua_pushnumber(L, number);
                        else lua_pushstring(L, text);
                        break;
                }
                lua_setfield(L, -2, column);
            }
            lua_setfield(L, -2, data.cell(r, 0));
        }
        if (!slot && self.table_count_ < sizeof self.tables_ / sizeof self.tables_[0]) {
            slot = &self.tables_[self.table_count_++];
            std::snprintf(slot->path, sizeof slot->path, "%s", path);
            slot->ref = LUA_NOREF;
        }
        if (slot) {
            luaL_unref(L, LUA_REGISTRYINDEX, slot->ref);
            lua_pushvalue(L, -1);
            slot->ref = luaL_ref(L, LUA_REGISTRYINDEX);
            slot->modified = modified;
        }
        return 1;
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
    // unique_name() numbers copies "Sphere2"; "Sphere 2" (typed by hand) counts too.
    static bool named_or_numbered(const char* name, const char* base) {
        const usize n = std::strlen(base);
        if (std::strncmp(name, base, n) != 0) return false;
        const char* rest = name + n + (name[n] == ' ' && name[n + 1] ? 1 : 0);
        while (*rest >= '0' && *rest <= '9') ++rest;
        return *rest == '\0';
    }
    static int find_all(lua_State* L) {
        const char* name = luaL_checkstring(L, 1);
        const world::World& w = world(L);
        lua_newtable(L);
        int n = 0;
        for (u32 slot = 0; slot < w.slot_count(); ++slot) {
            const world::Entity e = w.entity_at_slot(slot);
            if (!e.valid() || !named_or_numbered(w.name(e), name)) continue;
            lua_pushnumber(L, e.value);
            lua_rawseti(L, -2, ++n);
        }
        return 1;
    }
    static int find_tagged(lua_State* L) {
        const char* tag = luaL_checkstring(L, 1);
        const world::World& w = world(L);
        lua_newtable(L);
        int n = 0;
        for (u32 slot = 0; slot < w.slot_count(); ++slot) {
            const world::Entity e = w.entity_at_slot(slot);
            if (!e.valid() || !world::has_tag(w, e, tag)) continue;
            lua_pushnumber(L, e.value);
            lua_rawseti(L, -2, ++n);
        }
        return 1;
    }
    static int name(lua_State* L) {
        lua_pushstring(L, world(L).name(live_entity(L, 1)));
        return 1;
    }
    static int set_name(lua_State* L) {  // the World allows duplicate names, so it is kept as given
        world(L).set_name(live_entity(L, 1), luaL_checkstring(L, 2));
        return 0;
    }
    static int get_world_settings(lua_State* L) {
        push_struct(L, reflect::type_of<world::WorldSettings>(), reinterpret_cast<const u8*>(&world(L).settings()));
        return 1;
    }
    static int set_world_settings(lua_State* L) {  // into a copy first, so a bad field changes nothing
        luaL_checktype(L, 1, LUA_TTABLE);
        world::WorldSettings copy = world(L).settings();
        read_struct(L, 1, reflect::type_of<world::WorldSettings>(), reinterpret_cast<u8*>(&copy));
        world(L).settings() = copy;
        return 0;
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
    static int has_any_tag(lua_State* L) {
        lua_pushboolean(L, world::has_tags(world(L), live_entity(L, 1), luaL_checkstring(L, 2), world::TagQuery::Any));
        return 1;
    }
    static int has_all_tags(lua_State* L) {
        lua_pushboolean(L, world::has_tags(world(L), live_entity(L, 1), luaL_checkstring(L, 2), world::TagQuery::All));
        return 1;
    }
    static int has_no_tag(lua_State* L) {
        lua_pushboolean(L, world::has_tags(world(L), live_entity(L, 1), luaL_checkstring(L, 2), world::TagQuery::None));
        return 1;
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

ScriptSystem::~ScriptSystem() {
    stop();
    clear_persistent();
}

void ScriptSystem::clear_persistent() {
    delete[] persistent_;
    persistent_ = nullptr;
}

void ScriptSystem::stop() {
    stop_natives();
    clear_kept();  // a real stop: nothing carries over to the next Play
    if (g_debugged == this) g_debugged = nullptr;
    stepping_ = DebugAction::Continue;
    if (lua_) {  // keep the game instance for the next start (a level change)
        lua_getglobal(lua_, "lsd");
        lua_getfield(lua_, -1, "__persist");
        std::string json;
        if (lua_istable(lua_, -1) && !write_json(lua_, -1, json, 0)) {
            LSD_LOG_WARN("script", "game_instance / player_state / subsystem hold something that cannot be kept (a function?): they start empty in the next level");
            json.clear();
        }
        lua_pop(lua_, 2);
        clear_persistent();
        persistent_ = new char[json.size() + 1];
        std::memcpy(persistent_, json.c_str(), json.size() + 1);
    }
    remove_memory_source(this);
    if (lua_) lua_close(lua_);
    lua_ = nullptr;
    js::destroy(js_);  // after Lua, which held JS functions (timers)
    js_ = nullptr;
    world_ = nullptr;
    count_ = 0;
    table_count_ = 0;  // their refs went with the Lua state
    delete[] curves_;
    curves_ = nullptr;
    curve_count_ = 0;
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
    path_of_ = path_of;
    path_user_ = path_user;
    // shortcut: Lua's own allocator (no memory cap per script); give it an Allocator with a budget before shipping mods.
    lua_ = luaL_newstate();
    add_memory_source("Scripts (Lua)", [](void* user) -> usize {  // LuaJIT keeps its own heap: its collector counts it
        lua_State* L = static_cast<ScriptSystem*>(user)->lua_;
        return L ? static_cast<usize>(lua_gc(L, LUA_GCCOUNT, 0)) * 1024 + static_cast<usize>(lua_gc(L, LUA_GCCOUNTB, 0)) : 0;
    }, this);
    curves_ = new CachedCurve[kMaxCurves];
    luaL_openlibs(lua_);

    const luaL_Reg api[] = {
        {"log", Api::log}, {"find", Api::find}, {"find_all", Api::find_all}, {"find_tagged", Api::find_tagged}, {"name", Api::name},
        {"set_name", Api::set_name}, {"get_world_settings", Api::get_world_settings}, {"set_world_settings", Api::set_world_settings},
        {"alive", Api::alive}, {"destroy", Api::destroy},
        {"position", Api::position}, {"set_position", Api::set_position}, {"rotation", Api::rotation},
        {"set_rotation", Api::set_rotation}, {"has_tag", Api::has_tag},
        {"has_any_tag", Api::has_any_tag}, {"has_all_tags", Api::has_all_tags}, {"has_no_tag", Api::has_no_tag}, {"time", Api::time}, {"input", Api::input},
        {"pressed", Api::pressed}, {"log_error", Api::log_error}, {"get", Api::get}, {"set", Api::set},
        {"add", Api::add}, {"remove", Api::remove}, {"has", Api::has}, {"spawn", Api::spawn}, {"raycast", Api::raycast},
        {"sphere_trace", Api::sphere_trace}, {"box_trace", Api::box_trace}, {"capsule_trace", Api::capsule_trace},
        {"overlap_sphere", Api::overlap_sphere}, {"overlap_box", Api::overlap_box}, {"open_level", Api::open_level}, {"player", Api::player}, {"camera_shake", Api::camera_shake}, {"set_camera_view", Api::set_camera_view}, {"camera_view", Api::camera_view}, {"camera_location", Api::camera_location}, {"camera_forward", Api::camera_forward}, {"touch_count", Api::touch_count}, {"touch", Api::touch},
        {"table", Api::table}, {"curve", Api::curve}, {"save_game", Api::save_game}, {"save_game_async", Api::save_game_async}, {"load_game", Api::load_game},
        {"has_save", Api::has_save}, {"delete_save", Api::delete_save}, {"move_to", Api::move_to}, {"stop_move", Api::stop_move},
        {"is_moving", Api::is_moving}, {"find_path", Api::find_path}, {"bb_set", Api::bb_set}, {"bb_get", Api::bb_get},
        {"bb_clear", Api::bb_clear}, {"report_noise", Api::report_noise}, {"claim_smart_object", Api::claim_smart_object}, {"release_smart_object", Api::release_smart_object}, {"run_query", Api::run_query}, {"apply_damage", Api::apply_damage}, {"rebuild_navmesh", Api::rebuild_navmesh}, {"ui_open", Api::ui_open}, {"ui_close", Api::ui_close},
        {"ui_set_text", Api::ui_set_text}, {"ui_set", Api::ui_set}, {"ui_value", Api::ui_value}, {"ui_data", Api::ui_data}, {"ui_show_cursor", Api::ui_show_cursor}, {"ui_follow", Api::ui_follow}, {"ui_unfollow", Api::ui_unfollow}, {"ui_style", Api::ui_style}, {"ui_rml", Api::ui_rml}, {"_ui_open_text", Api::ui_open_text},
        {"ui_play_animation", Api::ui_play_animation}, {"ui_animate", Api::ui_animate},
        {"get_settings", Api::get_settings}, {"set_settings", Api::set_settings}, {"text", Api::text}, {"set_language", Api::set_language},
        {"languages", Api::languages}, {"bindings", Api::bindings}, {"broadcast", Api::broadcast}, {"set_view", Api::set_view}, {"enum", Api::user_enum}, {"ik_reach", Api::ik_reach}, {"ik_look_at", Api::ik_look_at}, {"play_music", Api::play_music}, {"play_timeline", Api::play_timeline}, {"socket", Api::socket}, {"stop_timeline", Api::stop_timeline}, {"stop_music", Api::stop_music}, {"music_layer", Api::music_layer}, {"music_intensity", Api::music_intensity}, {"music_segment", Api::music_segment}, {"trigger_audio", Api::trigger_audio}, {"struct", Api::user_struct}, {"draw_line", Api::draw_line}, {"draw_sphere", Api::draw_sphere}, {"draw_box", Api::draw_box}, {"draw_text", Api::draw_text}, {"call", Api::call_on}, {"native_call", Api::native_call}, {"net_mode", Api::net_mode}, {"load_sublevel", Api::load_sublevel}, {"unload_sublevel", Api::unload_sublevel}, {"enter_vehicle", Api::enter_vehicle}, {"exit_vehicle", Api::exit_vehicle},
        {"vehicle", Api::vehicle}, {"ride", Api::ride}, {"stop_riding", Api::stop_riding}, {"start_mover", Api::start_mover}, {"stop_mover", Api::stop_mover},
        {"restart_mover", Api::restart_mover}, {"player_id", Api::player_id},
        {"_net_send", Api::net_send}, {"chat", Api::chat}, {"set_team", Api::set_team}, {"team", Api::team},
        {"net_seen_position", Api::net_seen_position}, {"net_find_games", Api::net_find_games}, {"net_host", Api::net_host}, {"net_join", Api::net_join}, {"net_leave", Api::net_leave}, {"net_join_code", Api::net_join_code}, {"editor_workspace", Api::editor_workspace}, {"_editor", Api::editor_call}, {"implements", Api::implements}, {"rebind", Api::rebind}, {"key_pressed", Api::key_pressed},
        {"input_record", Api::input_record}, {"input_play", Api::input_play}, {"input_stop", Api::input_stop},
        {"play_sound", Api::play_sound}, {"stop_sound", Api::stop_sound}, {"push_mix", Api::push_mix}, {"pop_mix", Api::pop_mix},
        {"set_audio_param", Api::set_audio_param}, {"set_anim_param", Api::set_anim_param}, {"ragdoll", Api::ragdoll}, {"set_material_parameter", Api::set_material_parameter}, {"play_montage", Api::play_montage}, {"stop_montage", Api::stop_montage}, {"montage_playing", Api::montage_playing}, {"anim_curve", Api::anim_curve}, {"set_motion_tag", Api::set_motion_tag}, {"set_morph", Api::set_morph}, {"morph_names", Api::morph_names}, {"set_outfit", Api::set_outfit}, {"assets", Api::assets}, {"set_class_volume", Api::set_class_volume}, {"spline_point", Api::spline_point}, {"spline_length", Api::spline_length},
        {"apply_effect", Api::apply_effect}, {"remove_effect", Api::remove_effect}, {"attribute", Api::attribute}, {"set_attribute", Api::set_attribute},
        {"give_ability", Api::give_ability}, {"try_activate_ability", Api::try_activate_ability}, {"end_ability", Api::end_ability},
        {"cancel_ability", Api::cancel_ability}, {"ability_cooldown", Api::ability_cooldown},
    };
    lua_newtable(lua_);
    for (const luaL_Reg& r : api) {
        lua_pushlightuserdata(lua_, this);
        lua_pushcclosure(lua_, r.func, 1);
        lua_setfield(lua_, -2, r.name);
    }
    lua_getfield(lua_, -1, "log");
    lua_setglobal(lua_, "print");  // print() goes to the Output Log too
    const char* kept = persistent_;
    if (!kept || !*kept || !read_json(lua_, kept, 0) || !lua_istable(lua_, -1)) {
        lua_settop(lua_, 1);
        lua_newtable(lua_);
    }
    lua_setfield(lua_, -2, "__persist");
    lua_setglobal(lua_, "lsd");
    js::register_lua(lua_, &js_);  // lsd.js_call: JavaScript nodes in visual scripts
    run(kPrelude, "=lsd prelude");
    run(kSandbox, "=lsd sandbox");
    update_hook();  // breakpoints set before Play
    if (host.editor) return;  // editor utilities: the level's own scripts wait for Play

    const reflect::TypeInfo& sets = reflect::type_of<world::AttributeSet>();
    for (u32 i = 0; i < world.count(sets); ++i) world::recompute(*static_cast<world::AttributeSet*>(world.at(sets, i)));  // current values from the edited bases
    const reflect::TypeInfo& type = reflect::type_of<world::Script>();
    for (u32 i = 0; i < world.count(type); ++i) add_instance(world.owner(type, i), static_cast<const world::Script*>(world.at(type, i))->script);
    create_natives();  // before any on_start, so events sent from one reach the behaviours too
    // on_start after every script is loaded, so lsd.find sees them all.
    for (u32 i = 0; i < count_; ++i) {
        if (!instances_[i].failed) call(instances_[i], "on_start", nullptr, 0);
    }
    for (u32 i = 0; i < native_count_; ++i) {
        const NativeInstance n = natives_[i];
        if (n.behaviour->on_start) n.behaviour->on_start(n.entity.value, n.state);
    }
}

void ScriptSystem::attach(world::Entity root) {
    if (!lua_) return;
    static u32 added[256];  // slots filled now (reused ones are anywhere, not only at the end)
    u32 added_count = 0;
    const reflect::TypeInfo& type = reflect::type_of<world::Script>();
    for (u32 i = 0; i < world_->count(type); ++i) {
        const world::Entity e = world_->owner(type, i);
        bool inside = false;
        for (world::Entity p = e; p.valid() && !inside; p = world_->parent(p)) inside = p == root;
        if (!inside) continue;
        const u32 slot = add_instance(e, static_cast<const world::Script*>(world_->at(type, i))->script);
        if (slot != kNoSlot) added[added_count++] = slot;
    }
    for (u32 k = 0; k < added_count; ++k) {
        if (!instances_[added[k]].failed) call(instances_[added[k]], "on_start", nullptr, 0);
    }
    added_count = 0;
    add_natives(root, added, &added_count);
    for (u32 k = 0; k < added_count; ++k) {
        const NativeInstance n = natives_[added[k]];  // a copy: on_start may spawn (attach adds natives)
        if (n.behaviour->on_start) n.behaviour->on_start(n.entity.value, n.state);
    }
}

u32 ScriptSystem::add_instance(world::Entity entity, const char* guid) {
    if (!guid[0]) return kNoSlot;
    u32 slot = 0;
    while (slot < count_ && world_->alive(instances_[slot].entity)) ++slot;  // a destroyed object's slot first
    if (slot == sizeof instances_ / sizeof instances_[0]) {
        LSD_LOG_WARN("script", "Too many scripts: %s does not run", world_->name(entity));
        return kNoSlot;
    }
    Instance& instance = instances_[slot];
    if (slot < count_) {  // let go of the old one: its refs and JS context
        if (instance.self > 0) luaL_unref(lua_, LUA_REGISTRYINDEX, instance.self);
        if (instance.env > 0) luaL_unref(lua_, LUA_REGISTRYINDEX, instance.env);
        if (js_) js::release(js_, slot);
    } else {
        ++count_;
    }
    instance = Instance{};
    instance.entity = entity;
    instance.failed = true;
    if (!path_of_ || !path_of_(path_user_, guid, instance.path, sizeof instance.path)) {
        LSD_LOG_WARN("script", "%s: its Script asset is missing", world_->name(entity));
        return kNoSlot;
    }
    instance.failed = false;
    // Its self table: { entity = <handle>, name = "<name>" }.
    lua_newtable(lua_);
    lua_pushnumber(lua_, entity.value);
    lua_setfield(lua_, -2, "entity");
    lua_pushstring(lua_, world_->name(entity));
    lua_setfield(lua_, -2, "name");
    instance.self = luaL_ref(lua_, LUA_REGISTRYINDEX);
    // Its environment: own globals, falling back to the shared ones (lsd, math, string...).
    lua_newtable(lua_);
    lua_newtable(lua_);
    lua_pushvalue(lua_, LUA_GLOBALSINDEX);
    lua_setfield(lua_, -2, "__index");
    lua_setmetatable(lua_, -2);
    instance.env = luaL_ref(lua_, LUA_REGISTRYINDEX);
    if (!load(instance)) return slot;
    // A visual script's `defaults` (declared variables), then the exposed properties: the script's
    // defaults (its `properties` table), then this object's values.
    lua_rawgeti(lua_, LUA_REGISTRYINDEX, instance.self);
    lua_rawgeti(lua_, LUA_REGISTRYINDEX, instance.env);
    for (const char* table : {"defaults", "properties"}) {
        lua_getfield(lua_, -1, table);
        if (lua_istable(lua_, -1)) {
            lua_pushnil(lua_);
            while (lua_next(lua_, -2)) {  // self[k] = v
                lua_pushvalue(lua_, -2);
                lua_insert(lua_, -2);
                lua_settable(lua_, -6);
            }
        }
        lua_pop(lua_, 1);
    }
    lua_pop(lua_, 1);
    if (const auto* script = world_->get<world::Script>(entity)) {
        for (const world::ScriptProperty& p : script->properties) {
            if (!p.name[0]) continue;
            char* end = nullptr;
            const double number = std::strtod(p.value, &end);
            if (std::strcmp(p.value, "true") == 0 || std::strcmp(p.value, "false") == 0) lua_pushboolean(lua_, p.value[0] == 't');
            else if (p.value[0] && end && *end == '\0') lua_pushnumber(lua_, number);
            else lua_pushstring(lua_, p.value);
            lua_setfield(lua_, -2, p.name);
        }
    }
    lua_pop(lua_, 1);
    return slot;
}

// ---- Debugging ----

namespace {

bool same_source(const char* a, const char* b) {  // paths as the hook and the editor write them
    for (; *a && *b; ++a, ++b) {
        const char x = *a == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(*a)));
        const char y = *b == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(*b)));
        if (x != y) return false;
    }
    return *a == *b;
}

i32 stack_depth(lua_State* L) {
    lua_Debug ar;
    i32 depth = 0;
    while (lua_getstack(L, depth, &ar)) ++depth;
    return depth;
}

void describe(lua_State* L, int index, char* out, usize capacity) {
    switch (lua_type(L, index)) {
        case LUA_TNIL: std::snprintf(out, capacity, "nil"); break;
        case LUA_TBOOLEAN: std::snprintf(out, capacity, "%s", lua_toboolean(L, index) ? "true" : "false"); break;
        case LUA_TNUMBER: std::snprintf(out, capacity, "%.6g", lua_tonumber(L, index)); break;
        case LUA_TSTRING: std::snprintf(out, capacity, "\"%s\"", lua_tostring(L, index)); break;
        case LUA_TTABLE: {
            lua_getfield(L, index, "x");
            const bool vec = lua_isnumber(L, -1) != 0;
            lua_pop(L, 1);
            if (vec) {
                lua_getfield(L, index, "x");
                lua_getfield(L, index > 0 ? index : index - 1, "y");
                lua_getfield(L, index > 0 ? index : index - 2, "z");
                std::snprintf(out, capacity, "(%.4g, %.4g, %.4g)", lua_tonumber(L, -3), lua_tonumber(L, -2), lua_tonumber(L, -1));
                lua_pop(L, 3);
            } else {
                std::snprintf(out, capacity, "table (%d items)", static_cast<int>(lua_objlen(L, index)));
            }
            break;
        }
        case LUA_TFUNCTION: std::snprintf(out, capacity, "function"); break;
        default: std::snprintf(out, capacity, "%s", lua_typename(L, lua_type(L, index))); break;
    }
}

void fill_stop(lua_State* L, DebugStop& stop) {
    stop.frame_count = 0;
    lua_Debug ar;
    for (int level = 0; stop.frame_count < 12 && lua_getstack(L, level, &ar); ++level) {
        lua_getinfo(L, "nSl", &ar);
        if (ar.currentline < 0) continue;  // C functions
        DebugFrame& f = stop.frames[stop.frame_count++];
        std::snprintf(f.function, sizeof f.function, "%s", ar.name ? ar.name : (ar.what && std::strcmp(ar.what, "main") == 0 ? "(file)" : "(function)"));
        std::snprintf(f.source, sizeof f.source, "%s", ar.source[0] == '@' ? ar.source + 1 : ar.source);
        f.line = ar.currentline;
        f.local_count = 0;
        for (int i = 1; f.local_count < 40; ++i) {
            const char* name = lua_getlocal(L, &ar, i);
            if (!name) break;
            if (name[0] == '(') {  // temporaries
                lua_pop(L, 1);
                continue;
            }
            DebugVariable& v = f.locals[f.local_count++];
            std::snprintf(v.name, sizeof v.name, "%s", name);
            describe(L, -1, v.value, sizeof v.value);
            if (std::strcmp(name, "self") == 0 && lua_istable(L, -1)) {  // its fields, like Blueprint variables
                lua_pushnil(L);
                while (lua_next(L, -2) && f.local_count < 40) {
                    if (lua_type(L, -2) == LUA_TSTRING) {
                        DebugVariable& field = f.locals[f.local_count++];
                        std::snprintf(field.name, sizeof field.name, "self.%s", lua_tostring(L, -2));
                        describe(L, -1, field.value, sizeof field.value);
                    }
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
    }
}

} // namespace

void ScriptSystem::hook(lua_State* L, lua_Debug* ar) {
    ScriptSystem* self = g_debugged;
    if (!self || self->in_debugger_ || ar->event != LUA_HOOKLINE) return;
    lua_getinfo(L, "S", ar);
    const char* source = ar->source[0] == '@' ? ar->source + 1 : ar->source;
    if (self->trace_) self->trace_(self->trace_user_, source, ar->currentline);
    if (!self->debugger_) return;
    bool stop = false;
    if (self->stepping_ == DebugAction::StepInto) stop = true;
    else if (self->stepping_ == DebugAction::StepOver) stop = stack_depth(L) <= self->step_depth_;
    else if (self->stepping_ == DebugAction::StepOut) stop = stack_depth(L) < self->step_depth_;
    for (u32 i = 0; i < self->breakpoint_count_ && !stop; ++i) {
        stop = self->breakpoints_[i].line == ar->currentline && same_source(self->breakpoints_[i].source, source);
    }
    if (!stop) return;
    static DebugStop info;  // large; one stop at a time
    fill_stop(L, info);
    self->in_debugger_ = true;
    const DebugAction action = self->debugger_(self->debugger_user_, info);
    self->in_debugger_ = false;
    self->stepping_ = action;
    self->step_depth_ = stack_depth(L);
    self->update_hook();
}

bool ScriptSystem::line_hooks_on() { return g_debugged != nullptr; }

bool ScriptSystem::foreign_line(const char* source, i32 line, i32 (*depth)(void*), void (*fill)(void*, DebugStop&), void* user) {
    ScriptSystem* self = g_debugged;
    if (!self || self->in_debugger_) return false;
    if (self->trace_) self->trace_(self->trace_user_, source, line);
    if (!self->debugger_) return false;
    // shortcut: step depths are per language (a JS step out into its Lua caller stops at the next marker); share a depth if mixed stepping matters
    bool stop = self->stepping_ == DebugAction::StepInto;
    if (self->stepping_ == DebugAction::StepOver) stop = depth(user) <= self->step_depth_;
    else if (self->stepping_ == DebugAction::StepOut) stop = depth(user) < self->step_depth_;
    for (u32 i = 0; i < self->breakpoint_count_ && !stop; ++i) {
        stop = self->breakpoints_[i].line == line && same_source(self->breakpoints_[i].source, source);
    }
    if (!stop) return false;
    static DebugStop info;  // large; one stop at a time
    info = DebugStop{};
    fill(user, info);
    self->in_debugger_ = true;
    const DebugAction action = self->debugger_(self->debugger_user_, info);
    self->in_debugger_ = false;
    self->stepping_ = action;
    self->step_depth_ = depth(user);
    self->update_hook();
    return true;
}

void ScriptSystem::update_hook() {
    if (!lua_) return;
    const bool on = trace_ || (debugger_ && (breakpoint_count_ || stepping_ != DebugAction::Continue));
    g_debugged = on ? this : (g_debugged == this ? nullptr : g_debugged);
    lua_sethook(lua_, on ? hook : nullptr, on ? LUA_MASKLINE : 0, 0);  // shortcut: line hooks keep LuaJIT in its interpreter while debugging
}

void ScriptSystem::set_debugger(DebugHandler handler, void* user) {
    debugger_ = handler;
    debugger_user_ = user;
    update_hook();
}

void ScriptSystem::set_breakpoint(const char* source, i32 line, bool on) {
    for (u32 i = 0; i < breakpoint_count_; ++i) {
        if (breakpoints_[i].line == line && same_source(breakpoints_[i].source, source)) {
            if (!on) breakpoints_[i] = breakpoints_[--breakpoint_count_];
            update_hook();
            return;
        }
    }
    if (on && breakpoint_count_ < sizeof breakpoints_ / sizeof breakpoints_[0]) {
        std::snprintf(breakpoints_[breakpoint_count_].source, sizeof breakpoints_[0].source, "%s", source);
        breakpoints_[breakpoint_count_++].line = line;
    }
    update_hook();
}

void ScriptSystem::clear_breakpoints() {
    breakpoint_count_ = 0;
    update_hook();
}

bool ScriptSystem::has_breakpoint(const char* source, i32 line) const {
    for (u32 i = 0; i < breakpoint_count_; ++i) {
        if (breakpoints_[i].line == line && same_source(breakpoints_[i].source, source)) return true;
    }
    return false;
}

void ScriptSystem::set_line_trace(LineTrace trace, void* user) {
    trace_ = trace;
    trace_user_ = user;
    update_hook();
}

bool ScriptSystem::load(Instance& instance) {
    instance.modified = file_time(instance.path);
    instance.failed = true;
    if (js::is_js(instance.path)) {
        if (!js_) js_ = js::create(lua_);
        lua_rawgeti(lua_, LUA_REGISTRYINDEX, instance.env);
        instance.failed = !js::load(js_, static_cast<u32>(&instance - instances_), instance.path, debuggable());
        lua_pop(lua_, 1);
        return !instance.failed;
    }
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
    for (int i = 0; i < arg_count; ++i) {
        if (args[i] != args[i]) lua_pushnil(lua_);  // NaN: no value (an event's missing object)
        else lua_pushnumber(lua_, args[i]);
    }
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
    Api::tick_gameplay(*this, dt);
    if (check_files) js::update(js_);
    const f64 dt_arg = dt;
    for (u32 i = 0; i < count_; ++i) {
        Instance& instance = instances_[i];
        if (!world_->alive(instance.entity)) continue;
        if (check_files && file_time(instance.path) != instance.modified) {
            if (load(instance)) LSD_LOG_INFO("script", "Reloaded %s", instance.path);
        }
        if (!instance.failed) call(instance, "on_update", &dt_arg, 1);
    }
    for (u32 i = 0; i < native_count_; ++i) {
        const NativeInstance n = natives_[i];  // a copy: on_update may spawn (attach adds natives)
        if (n.behaviour->on_update && world_->alive(n.entity)) n.behaviour->on_update(n.entity.value, n.state, dt);
    }
}

u32 ScriptSystem::send(world::Entity target, const char* event, world::Entity other) {
    const f64 arg = other.value;
    return send(target, event, &arg, 1, other);
}

u32 ScriptSystem::send(world::Entity target, const char* event, const f64* args, u32 count, world::Entity other) {
    u32 called = 0;
    for (u32 i = 0; lua_ && i < count_; ++i) {
        Instance& instance = instances_[i];
        if (instance.entity != target || instance.failed) continue;
        lua_rawgeti(lua_, LUA_REGISTRYINDEX, instance.env);
        lua_getfield(lua_, -1, event);
        called += lua_isfunction(lua_, -1);
        lua_pop(lua_, 2);
        call(instance, event, args, static_cast<int>(count));
    }
    return called + native_event(target, event, other, false);
}

int ScriptSystem::call_task(world::Entity target, const char* function, f32 dt) {
    for (u32 i = 0; i < count_; ++i) {
        Instance& instance = instances_[i];
        if (instance.entity != target || instance.failed) continue;
        lua_rawgeti(lua_, LUA_REGISTRYINDEX, instance.env);
        lua_getfield(lua_, -1, function);
        lua_remove(lua_, -2);
        if (!lua_isfunction(lua_, -1)) {
            lua_pop(lua_, 1);
            continue;
        }
        lua_pushcfunction(lua_, traceback);
        lua_insert(lua_, -2);
        const int handler = lua_gettop(lua_) - 1;
        lua_rawgeti(lua_, LUA_REGISTRYINDEX, instance.self);
        lua_pushnumber(lua_, dt);
        if (lua_pcall(lua_, 2, 1, handler) != 0) {
            LSD_LOG_ERROR("script", "%s (%s): %s", instance.path, function, lua_tostring(lua_, -1));
            lua_settop(lua_, handler - 1);
            return 0;
        }
        const int result = lua_isnil(lua_, -1) ? 2 : lua_toboolean(lua_, -1) ? 1 : 0;
        lua_settop(lua_, handler - 1);
        return result;
    }
    return -1;
}

u32 ScriptSystem::broadcast(const char* function, const char* text) {
    u32 called = 0;
    if (lua_) {  // an engine-wide handler (lsd.on_ui) first: called once, whatever scripts there are
        lua_getglobal(lua_, "lsd");
        lua_getfield(lua_, -1, "_ui_actions");
        if (lua_istable(lua_, -1)) {
            lua_getfield(lua_, -1, function);
            if (lua_isfunction(lua_, -1)) {
                lua_pushstring(lua_, text);
                char what[160];
                std::snprintf(what, sizeof what, "lsd.on_ui (%s)", function);
                protected_call(lua_, 1, what);
                ++called;
            } else {
                lua_pop(lua_, 1);
            }
        }
        lua_pop(lua_, 2);
    }
    for (u32 i = 0; i < count_; ++i) {
        Instance& instance = instances_[i];
        if (instance.failed || !world_->alive(instance.entity)) continue;
        lua_rawgeti(lua_, LUA_REGISTRYINDEX, instance.env);
        lua_getfield(lua_, -1, function);
        lua_remove(lua_, -2);
        if (!lua_isfunction(lua_, -1)) {
            lua_pop(lua_, 1);
            continue;
        }
        lua_rawgeti(lua_, LUA_REGISTRYINDEX, instance.self);
        lua_pushstring(lua_, text);
        char what[300];
        std::snprintf(what, sizeof what, "%s (%s)", instance.path, function);
        if (!protected_call(lua_, 2, what)) instance.failed = true;
        ++called;
    }
    return called + native_event({}, function, {}, true);
}

bool ScriptSystem::call_api(const char* function, const char* json_args, std::string& out) {
    out.clear();
    if (!lua_ || !function || function[0] == '_') {
        out = "{\"error\": \"not playing, or a hidden function\"}";
        return false;
    }
    const int base = lua_gettop(lua_);
    lua_getglobal(lua_, "lsd");
    lua_getfield(lua_, -1, function);
    lua_remove(lua_, -2);
    if (!lua_isfunction(lua_, -1)) {
        lua_settop(lua_, base);
        out = std::string("{\"error\": \"no lsd.") + function + "\"}";
        return false;
    }
    int args = 0;
    const char* c = json_args && *json_args ? json_args : "[]";
    if (!read_json(lua_, c, 0) || !lua_istable(lua_, -1)) {
        lua_settop(lua_, base);
        out = "{\"error\": \"the arguments must be a JSON array\"}";
        return false;
    }
    const int list = lua_gettop(lua_);
    args = static_cast<int>(lua_objlen(lua_, list));
    lua_checkstack(lua_, args + 4);
    for (int i = 1; i <= args; ++i) lua_rawgeti(lua_, list, i);
    lua_remove(lua_, list);
    if (lua_pcall(lua_, args, LUA_MULTRET, 0) != 0) {
        const char* error = lua_tostring(lua_, -1);
        LSD_LOG_ERROR("game", "lsd_lua_call(%s): %s", function, error ? error : "error");
        out = "{\"error\": \"see the Output Log\"}";
        lua_settop(lua_, base);
        return false;
    }
    const int results = lua_gettop(lua_) - base;
    if (results == 0) {  // nothing returned: an empty list (an empty Lua table would read as {})
        out = "[]";
        return true;
    }
    lua_createtable(lua_, results, 0);  // the results as one list (nil becomes null)
    for (int i = 1; i <= results; ++i) {
        lua_pushvalue(lua_, base + i);
        lua_rawseti(lua_, -2, i);
    }
    const bool ok = write_json(lua_, lua_gettop(lua_), out, 0);
    if (!ok) out = "{\"error\": \"a result cannot be written as JSON (a function?)\"}";
    lua_settop(lua_, base);
    return ok;
}

void ScriptSystem::net_call(const char* function, u32 from, const char* json) {
    if (!lua_) return;
    const int base = lua_gettop(lua_);
    lua_getglobal(lua_, "lsd");
    lua_getfield(lua_, -1, "broadcast");
    lua_remove(lua_, -2);
    lua_pushstring(lua_, function);
    lua_pushnumber(lua_, from);
    const char* c = json && *json ? json : "[]";
    int args = 2;
    if (read_json(lua_, c, 0) && lua_istable(lua_, -1)) {  // the array's items become the arguments
        const int list = lua_gettop(lua_);
        const int n = static_cast<int>(lua_objlen(lua_, list));
        lua_checkstack(lua_, n + 4);
        for (int i = 1; i <= n; ++i) lua_rawgeti(lua_, list, i);
        lua_remove(lua_, list);
        args += n;
    } else {
        lua_settop(lua_, base + 3);  // bad JSON: just the sender
        LSD_LOG_WARN("script", "%s: arguments that are not a JSON array were dropped", function);
    }
    protected_call(lua_, args, function);
    lua_settop(lua_, base);
}

void ScriptSystem::set_save_directory(const char* path) { std::snprintf(save_dir_, sizeof save_dir_, "%s", path); }

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
