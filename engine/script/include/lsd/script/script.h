#pragma once
// Lua gameplay scripts on LuaJIT (Unreal: a Blueprint's Event BeginPlay / Event Tick, in Lua).
// Every entity with a Script component runs its .lua file in its own environment:
//
//   function on_start(self) end          -- once, when Play starts
//   function on_update(self, dt) end     -- every frame, dt in seconds
//
// `self` is a table per object (self.entity, self.name) that scripts can store their own state in.
// The `lsd` table is the engine API:
//
//   lsd.log(...)                          print to the Output Log
//   lsd.find(name)                        entity by name, or nil
//   lsd.name(e), lsd.alive(e), lsd.destroy(e)
//   lsd.position(e) / lsd.set_position(e, x, y, z)     local Transform, metres
//   lsd.rotation(e) / lsd.set_rotation(e, x, y, z)     local Transform, degrees
//   lsd.has_tag(e, tag)                   gameplay tags ("Damage" matches "Damage.Fire")
//   lsd.has_any_tag(e, "A, B") / lsd.has_all_tags(e, "A, B") / lsd.has_no_tag(e, "A, B")   several at once
//   lsd.time()                            seconds since Play started
//   lsd.input(action) -> x, y             value of an Input Action, by asset name
//   lsd.pressed(action)                   true on the frame the action fires (its Trigger: pressed, hold, tap...)
//   lsd.log_error(...)                    print as an error
//   lsd.spawn(name, x, y, z) -> e         a prefab, mesh asset, particle system or built-in shape ("Cube") by name; nil if none
//   lsd.raycast(x, y, z, dx, dy, dz, distance) -> e, px, py, pz, nx, ny, nz   nearest hit, or nil
//   lsd.sphere_trace(x1, y1, z1, x2, y2, z2, radius) -> e, px, py, pz, nx, ny, nz   a sphere swept from 1 to 2
//   lsd.box_trace(x1, y1, z1, x2, y2, z2, hx, hy, hz)  /  lsd.capsule_trace(..., radius, half_height)   the same
//   lsd.overlap_sphere(x, y, z, radius) -> {e, ...}  /  lsd.overlap_box(x, y, z, hx, hy, hz) -> {e, ...}
//   lsd.spline_point(e, distance) -> x, y, z, dx, dy, dz   world point and direction along e's Spline
//   lsd.spline_length(e)                  metres
//   lsd.play_sound(name [, x, y, z]) -> id   a sound asset by name, 2D or at a position; 0 if none
//   lsd.stop_sound(id)
//   lsd.play_music(name [, fade = 1])     crossfades from the current track (seconds); same track: nothing
//   lsd.stop_music([fade = 1])
//   lsd.music_layer(name, volume [, fade = 1])   a stem in step with the track, faded to volume (0..1)
//   lsd.push_mix(name), lsd.pop_mix(name)   Sound Mix data assets by name
//   lsd.set_audio_param(name, value)      read by Sound Cue Switch nodes
//   lsd.set_anim_param(e, name, value)    an Animation Graph parameter on e's skeletal mesh (and its children's)
//   lsd.set_morph(e, name, weight)        a morph target (blend shape) weight on e's skeletal mesh (or its child's)
//   lsd.ragdoll(e, on)                     e's skeletal mesh (or its child's) becomes a ragdoll (true) or animates again (false)
//   lsd.morph_names(e) -> {"Nose_Wide", ...}   the morph targets e's skeletal mesh (or its child's) has
//   lsd.set_outfit(e, slot, mesh [, r, g, b])  what e's Outfit wears in a slot ("Shirt", "Shoes"...), a mesh
//                                         asset by name or GUID ("" takes it off); the slot is added if new
//   lsd.assets(type [, folder]) -> {{name =, guid =, folder =}, ...}   assets of a type ("Mesh", "Prefab",
//                                         "Sound"...) in a folder and below (Content/ relative; "" = all), by name
//   lsd.play_montage(e, animation [, rate = 1 [, from_bone = "" [, blend = 0.2 [, root_motion = false]]]]) -> seconds   an animation file
//                                         (by asset name) played once over what e plays, from from_bone down
//                                         ("spine_01": the upper body only; "": all of it), blended in and out
//                                         (Unreal's Play Montage); 0 if none. When it ends: on_montage_ended(self).
//                                         root_motion: the clip's root movement moves the player's Character
//                                         (dodges, attacks that travel) instead of the input
//   lsd.play_montage(..., root_motion, slot = "Default")   montage slots (Unreal's): a montage replaces the one
//                                         in its slot only; montages in other slots (an "UpperBody" attack,
//                                         a "Face" blink) play over it at the same time, in the order started
//   lsd.stop_montage(e [, blend = 0.2 [, slot]]), lsd.montage_playing(e [, slot]) -> bool   (no slot: every slot)
//   lsd.anim_curve(e, name) -> number     an animation curve's value where e's animation is now (0 when none has it)
//   lsd.set_motion_tag(e, tag)            motion matching picks only clips with this tag ("" = back to automatic:
//                                         crouch / swim / air from the Character, else the untagged clips)
//   lsd.set_class_volume(name, volume)    a Sound Class by name ("Master" too): settings sliders
//   lsd.curve(name, time) -> value        a Curve data asset by name (0 if none); re-read when it changes.
//                                         A Curve Vector gives x, y, z; a Curve Color r, g, b, a
//   lsd.curve_table(table, row, time) -> value   a Data Table used as a curve table: column headers are
//                                         times, each row a curve; linear between columns, flat past the ends
//   lsd.move_to(e, x, y, z) -> ok         a Nav Agent walks there on the navmesh (AI Move To); false if unreachable
//   lsd.stop_move(e), lsd.is_moving(e) -> bool
//   lsd.find_path(x, y, z, tx, ty, tz) -> {{x, y, z}, ...}   corners of the navmesh path (empty if none)
//   lsd.bb_set(e, key, number | x, y, z), lsd.bb_get(e, key) -> number | x, y, z | nil, lsd.bb_clear(e, key)
//                                         an AI Controller's blackboard (what its behaviour tree reads)
//   lsd.report_noise(x, y, z [, loudness]) AI Controllers in hearing range set NoiseLocation
//   lsd.claim_smart_object(e [, activity [, radius = 15]]) -> object, x, y, z | nil   the nearest free Smart Object
//                                         slot (activity "" = any), held for e until lsd.release_smart_object(e)
//   lsd.release_smart_object(e)           frees every slot e holds
//   lsd.run_query(e, query) -> x, y, z | nil   an Environment Query asset (by name) run for e: its best point
//   lsd.rebuild_navmesh() -> ok           builds the navmesh again now (after moving walls or doors); agents keep walking
//   lsd.apply_damage(target, amount [, instigator])  calls target's on_damage(self, amount, instigator) (nil when none); an AI target senses it
// Ability system (Unreal's Gameplay Ability System; lsd/world/abilities.h): effects and abilities by asset name.
//   lsd.attribute(e, name) -> current, base | nil, lsd.set_attribute(e, name, base)   an Attribute Set's values
//   lsd.apply_effect(target, effect) -> ok, lsd.remove_effect(target, effect) -> count   Gameplay Effects
//   lsd.give_ability(e, ability) -> ok    adds it to e's Ability System
//   lsd.try_activate_ability(e, ability) -> ok, why   checks cooldown / tags / cost ("cooldown", "blocked",
//                                         "missing tags", "cost", "active"), pays the cost, starts the cooldown
//   lsd.end_ability(e, ability), lsd.cancel_ability(e, ability) -> ok, lsd.ability_cooldown(e, ability) -> seconds
// Events: on_attribute_changed(self, name, value, old), the ability's Function(self, ability),
// on_ability_ended(self, ability, cancelled). Effects' granted tags count for lsd.has_tag.
//   lsd.ui_open(name) -> doc              a UI document (.rml) by asset name over the game; nil if none
//   lsd.ui_close(doc), lsd.ui_set_text(doc, id, text), lsd.ui_set(doc, id, attribute, value)
//   lsd.ui_value(doc, id) -> text         a text field's value
//   lsd.ui_data(name, value)              a data binding (number, text or boolean): documents with
//                                         data-model="game" show it as {{game.name}} and in data-* attributes;
//                                         a table is a list (data-for="item : game.name", {{item.field}}, {{game.name.size}})
//   lsd.ui_show_cursor(on)                a menu: the mouse moves a cursor instead of the player
//   lsd.ui_follow(doc, id, e [, height])  keeps that element over the object (name tags, health bars);
//                                         hidden when it is behind the camera; lsd.ui_unfollow(doc, id)
//   lsd.ui_style(doc, id, property, value)   one style property ("width", "40%"), the others kept
//   lsd.ui_rml(doc, id, markup)           replaces the element's contents with RML markup
//   lsd.ui_play_animation(doc, id, anim)  plays (replays) RCSS @keyframes: "pop 0.4s cubic-out"; "none" stops
//   lsd.ui_animate(doc, id, property, value [, seconds, tween])   tweens one property from where it is
//                                         (UMG widget animations: "opacity", "0", 0.5, "cubic-in-out")
// Ready-made HUD widgets (no UI document needed; they float over objects):
//   lsd.health_bar(e, fraction [, height = 2.2 [, "#RRGGBB"]])   a bar over e (0..1), updated by calling
//                                         again; green to red as it empties; lsd.remove_health_bar(e)
//   lsd.damage_number(e, amount [, "#RRGGBB"])   a number that rises from e and fades in a second
//   lsd.prompt(text [, e])                "Press E to open": at the bottom of the screen, or over e;
//                                         lsd.prompt(nil [, e]) hides it
//   lsd.settings_menu([open]) -> open     a ready-made options menu (quality, fullscreen, volumes, mouse, accessibility);
//                                         no argument: opens or closes it
//   lsd.on_ui(action, function(element_id) end)   a UI action handled once, engine-wide (before scripts')
//   lsd.get_settings() -> {fullscreen, quality, master_volume, music_volume, effects_volume,
//                          mouse_sensitivity, invert_look}   the player's settings (options menus)
//   lsd.set_settings({quality = 1, ...})  changes those given, applies and saves them
//   lsd.text(key [, ...]) -> text         the string table's text in the current language (the key if missing),
//                                         with {0}, {1}... filled from the extra arguments and
//                                         {0}|plural(one=apple,other=apples) forms picked by number
//   lsd.set_language(code)                switches (en, fr...): open UI documents are reloaded translated
//   lsd.languages() -> {"en", "fr", ...}
//   lsd.bindings(action) -> {"W", "Gamepad Left Stick", ...}   the keys an Input Action listens to
//   lsd.rebind(action, old_key, new_key) -> ok   swaps a key now and saves it in the player's settings
//   lsd.key_pressed() -> name | nil       a key / button that went down this frame ("press a key" menus)
//   lsd.input_record(file) / lsd.input_play(file) -> ok, lsd.input_stop()   records the Input Actions' values
//                                         each frame to Saved/<file>, or plays a recording back (tests, demos)
// UI events: an element's onclick="start_game" (onchange, onsubmit...) calls start_game(self, element_id)
// on every script that defines it.
//   lsd.save_game(slot, value [, version]) -> ok   Unreal's SaveGame: numbers, strings, booleans and tables (lists or
//                                         string keys) to Saved/SaveGames/<slot>.json; version: your game's save
//                                         format (a whole number, default 1)
//   lsd.save_game_async(slot, value [, version]) -> ok   the same, the file written in the background (a load or
//                                         another save of that slot waits for it); ok = the value could be saved
//   lsd.load_game(slot) -> value, version what was saved and its version (1 for saves without one), or nil:
//                                         upgrade old saves by version; lsd.has_save(slot), lsd.delete_save(slot)
//   lsd.save_objects(slot, {e1, e2}, [{"Transform", "Character"}], [extra]) -> ok   whole objects by name
//   lsd.load_objects(slot) -> extra, found   puts their components back (Transform through set_position)
//   lsd.table(name) -> {Row = {Column = value}}   a Data Table (.csv) by asset name; numbers where
//                                         cells are numbers; re-read when the file changes; nil if none
//
// Any component, through reflection (field names as in Details; structs are tables):
//   lsd.get(e, "Character")               a copy of the component as a table, or nil
//   lsd.set(e, "Character", {walk_speed = 8})          writes only the fields given
//   lsd.add(e, "RigidBody" [, fields]), lsd.remove(e, "RigidBody"), lsd.has(e, "RigidBody")
//
// Editor-made types (data assets): lsd.enum(name) -> {Sword = 0, Bow = 1, [0] = "Sword", ...};
// lsd.struct(name) -> a new table with the struct's fields at their defaults; nil if there is none.
//
// IK (call every frame you want it): lsd.ik_reach(e, bone, x, y, z [, weight]) puts a bone on a
// world point bending its parent (hands on ledges); lsd.ik_look_at(e, bone, x, y, z [, weight [, fx, fy, fz]])
// turns a bone's forward axis (its own space, default +X) toward a point (heads, turrets).
//
// Timelines (cutscenes): lsd.play_timeline(name [, loop]) -> ok, lsd.stop_timeline(name); when one
// ends, on_timeline_finished(self, name) runs on every script that has it.
//
// Sockets: lsd.socket(e, name) -> x, y, z, fx, fy, fz   a socket on e's skeletal mesh (or its child's) in
// the world, and its +X; nil if there is none (muzzle flashes, thrown objects).
//
// Cameras: lsd.set_view(e [, blend_seconds]) looks through e's Camera component, cutting or blending
// (Set View Target with Blend); lsd.set_view(nil [, blend]) goes back to the player.
// lsd.camera_shake(strength [, seconds = 0.5 [, frequency = 12]]): 1 = a hard hit (5 cm, about 1 degree), fading out.
// lsd.set_camera_view("first" | "third" | "camera"): the player's view (a Camera on the Character); "camera" = its own View.
// lsd.camera_view() -> "first" | "third".
// lsd.camera_location() -> x, y, z: where the player's view is this frame (Unreal's Get Camera Location).
// lsd.camera_forward() -> x, y, z: the way it looks, a unit vector (shoot along it from camera_location: crosshairs).
//
// Debug drawing over the viewport (r, g, b 0..1, default red; seconds, default 0 = this frame):
//   lsd.draw_line(x1, y1, z1, x2, y2, z2 [, r, g, b [, seconds]])
//   lsd.draw_sphere(x, y, z, radius [, r, g, b [, seconds]]), lsd.draw_box(x, y, z, hx, hy, hz [, ...])
//   lsd.draw_text(x, y, z, text [, r, g, b [, seconds]])
//
// Events between objects (Blueprint event dispatchers and interfaces):
//   lsd.broadcast(name, ...) -> count     calls name(self, ...) on every script that defines it
//   lsd.call(e, name, ...) -> count       only on e's scripts (an interface message)
//   lsd.implements(e, name) -> bool       does one of e's scripts define name?
//
// Timers (callbacks run before on_update):
//   lsd.after(seconds, fn) -> id, lsd.every(seconds, fn) -> id, lsd.cancel(id)
//   lsd.game_instance() -> table          kept across level changes for the whole Play (Game Instance)
//   lsd.player_state() -> table           the same, for the player's own data (Player State)
//   lsd.subsystem(name) -> table          one kept table per name (Game Instance Subsystems)
//   lsd.open_level(name) -> ok            loads another level (by asset name) after this frame; kept tables travel
//   lsd.player() -> e                     the player's Character (possessed), or nil
//
// Events sent by the game: on_overlap_begin(self, other), on_overlap_end(self, other) (Trigger Volumes),
// on_hit(self, other) (physics contact began; the player counts as its Character).
//
// Exposed properties (Unreal's Instance Editable): a top-level `properties = { speed = 5, label = "Door",
// locked = false }` shows those values in each object's Details, under its Script; each object can
// change them, and self.speed etc. hold the object's values before on_start.
//
// JavaScript: the Script component also takes a .js file (QuickJS-ng, engine/script/src/js.*): the same
// events as top-level function declarations, the same lsd object (several results come back as an
// array: const [x, y, z] = lsd.position(self.entity)), `var properties = { speed: 5 }`, console.log.
//   lsd.js_call(path, function, ...) -> result   a function of a .js file (what JavaScript nodes compile to)
//
// Level streaming: lsd.load_sublevel(e) / lsd.unload_sublevel(e) -> ok, for an object with a Sub Level
// component (its scripts and physics start with it). Load Distance streams them by themselves.
//
// Vehicles and movers (Place tab: Drivable Vehicle, Transport, Moving Mesh):
//   lsd.enter_vehicle(e) / lsd.exit_vehicle() -> ok, lsd.vehicle() -> e | nil   (E does it too)
//   lsd.ride(e) / lsd.stop_riding() -> ok   a Transport (train, ferry, lift)
//   lsd.start_mover(e), lsd.stop_mover(e), lsd.restart_mover(e) -> ok
// Vehicles and transports get on_enter(self, player) and on_exit(self, player).
//
// Multiplayer (Project Settings > Multiplayer; in a single-player game these answer as standalone):
//   lsd.net_mode() -> "standalone" | "server" | "client", lsd.player_id() (0 = the server's player)
//   lsd.has_authority() -> bool          true on the server and in single player (Unreal's HasAuthority)
//   lsd.server_event(name, ...)          runs name(self, player, ...) on the server (Run on Server)
//   lsd.multicast(name, ...)             server: runs name(self, 0, ...) everywhere (Multicast)
//   lsd.client_event(player, name, ...)  server: runs it on that player's machine (Run on Owning Client)
//   lsd.chat(channel, text) -> ok        "all", "team", "squad", "proximity" (those the project turns on)
//   lsd.set_team(player, team [, squad]) server: groups for team / squad chat; lsd.team(player) -> team, squad
//   lsd.net_seen_position(e, player) -> x, y, z   server: where e was when that player saw it (lag
//                                        compensation: check a client's shot against this); nil if unknown
//   lsd.net_find_games([seconds]) -> { {address, name, players, max_players}, ... }   games on the LAN
//                                        (blocks up to `seconds`, default 0.5: call it from a menu, not every frame)
//   lsd.net_host([level]) -> ok, why     host from here (a menu): Play restarts on `level` (default: this one) as the server
//   lsd.net_join(code) -> ok, why        a join code ("K7QF2-MXA9P") or an address ("1.2.3.4[:port]"): restarts as a client,
//                                        then follows the server to its level (on_connected / on_disconnected tell how it went)
//   lsd.net_leave([level]) -> ok         back to single player
//   lsd.editor.*                          editor utility scripts only (editor/editor_utility_core.h): selection,
//                                         panels, open assets, console; raises in Play and in a packaged game
//   lsd.editor_workspace() -> name       the editor's workspace tab while playing in it ("Layout", "Multiplayer"...); "" in a packaged game
//   lsd.net_join_code() -> { upnp = "off" | "working" | "open" | "failed", message, internet = code | nil,
//                            lan = { {address, code, tailscale = bool}, ... } }   server: codes to give playtesters;
//                                        the first call asks the router to open the port (UPnP, a couple of seconds)
// Events: on_player_joined(self, id), on_player_left(self, id), on_chat(self, from, channel, text),
// on_connected(self, id), on_disconnected(self). Arguments travel as JSON (numbers, text, tables).
//
// Saving a script while playing reloads it; `self` keeps its fields. A script that errors stops
// running (the error and stack go to the Output Log) until it is fixed and saved.
#include "lsd/foundation/math.h"
#include "lsd/script/native.h"
#include "lsd/world/world.h"
#include "lsd/world/curve.h"

#include <string>

struct lua_State;
struct lua_Debug;

namespace lsd::script {

namespace js { struct Host; }

// ---- Debugging (the editor's Lua debugger) ----
struct DebugVariable {
    char name[64];
    char value[128];
};
struct DebugFrame {
    char function[64];
    char source[260];  // the file ("Content/Scripts/door.lua", or a visual script's compiled .lua)
    i32 line;
    DebugVariable locals[40];  // its locals, then self's fields as "self.name"
    u32 local_count;
};
struct DebugStop {
    DebugFrame frames[12];  // [0] is where it stopped
    u32 frame_count;
};
enum class DebugAction : u8 { Continue, StepInto, StepOver, StepOut };
// Runs while the script waits at a breakpoint / step (the editor shows its debugger meanwhile).
using DebugHandler = DebugAction (*)(void* user, const DebugStop& stop);
// Every line a script runs, while set (visual-script live flow); keep it cheap.
using LineTrace = void (*)(void* user, const char* source, i32 line);

// What the game provides to scripts besides the world.
struct ScriptHost {
    void* user = nullptr;
    Vec3 (*input)(void* user, const char* action) = nullptr;   // x, y (z unused)
    bool (*pressed)(void* user, const char* action) = nullptr;
    world::Entity (*spawn)(void* user, const char* name, Vec3 position) = nullptr;
    bool (*raycast)(void* user, Vec3 origin, Vec3 direction, f32 distance, world::Entity& hit, Vec3& point, Vec3& normal) = nullptr;
    // shape: 0 sphere (size.x = radius), 1 box (size = half extents), 2 capsule (size.x = radius, size.y = half height).
    bool (*sweep)(void* user, int shape, Vec3 size, Vec3 from, Vec3 to, world::Entity& hit, Vec3& point, Vec3& normal) = nullptr;
    u32 (*overlap)(void* user, int shape, Vec3 size, Vec3 at, world::Entity* out, u32 capacity) = nullptr;
    bool (*open_level)(void* user, const char* name) = nullptr;  // false if there is no such level
    world::Entity (*player)(void* user) = nullptr;
    bool (*table_path)(void* user, const char* name, char* out, usize capacity) = nullptr;  // Data Table file by asset name
    bool (*curve_path)(void* user, const char* name, char* out, usize capacity) = nullptr;  // Curve data asset by name
    u32 (*play_sound)(void* user, const char* name, bool spatial, Vec3 position) = nullptr;  // voice, 0 if none
    void (*stop_sound)(void* user, u32 voice) = nullptr;
    bool (*move_to)(void* user, world::Entity e, Vec3 target) = nullptr;
    void (*stop_move)(void* user, world::Entity e) = nullptr;
    bool (*is_moving)(void* user, world::Entity e) = nullptr;
    u32 (*find_path)(void* user, Vec3 from, Vec3 to, Vec3* out, u32 capacity) = nullptr;
    // Blackboards: values are 3 numbers (a number repeats in all three); get returns 0 if unset, 1 number, 3 location.
    void (*bb_set)(void* user, world::Entity e, const char* key, const f32* value, bool vector) = nullptr;
    int (*bb_get)(void* user, world::Entity e, const char* key, f32* value) = nullptr;
    void (*bb_clear)(void* user, world::Entity e, const char* key) = nullptr;
    void (*report_noise)(void* user, Vec3 at, f32 loudness) = nullptr;
    world::Entity (*claim_smart_object)(void* user, world::Entity e, const char* activity, f32 radius, Vec3& slot) = nullptr;  // invalid: none free
    void (*release_smart_object)(void* user, world::Entity e) = nullptr;
    bool (*run_query)(void* user, world::Entity e, const char* query, Vec3& out) = nullptr;
    void (*damaged)(void* user, world::Entity target, f32 amount, world::Entity instigator) = nullptr;  // AI damage sense
    bool (*rebuild_navmesh)(void* user) = nullptr;
    i32 (*ui_open)(void* user, const char* name) = nullptr;  // -1 if none
    void (*ui_close)(void* user, i32 doc) = nullptr;
    bool (*ui_set)(void* user, i32 doc, const char* id, const char* attribute, const char* value) = nullptr;  // attribute nullptr: the text
    bool (*ui_value)(void* user, i32 doc, const char* id, char* out, usize capacity) = nullptr;
    // Data bindings: type 0 number, 1 text, 2 boolean (number != 0).
    void (*ui_data)(void* user, const char* name, int type, f64 number, const char* text) = nullptr;  // type 0 number, 1 text, 2 bool, 3 JSON
    void (*ui_show_cursor)(void* user, bool on) = nullptr;
    void (*ui_follow)(void* user, i32 doc, const char* id, world::Entity e, f32 height) = nullptr;  // e invalid: stop
    // The player's settings: a reflected struct, and a call after Lua changed it.
    void* (*settings)(void* user, const reflect::TypeInfo** type) = nullptr;
    void (*settings_changed)(void* user) = nullptr;
    const char* (*text)(void* user, const char* key) = nullptr;
    const char* (*language)(void* user) = nullptr;  // for plural rules ("en" when null)
    void (*set_language)(void* user, const char* code) = nullptr;
    u32 (*languages)(void* user, const char** out, u32 capacity) = nullptr;
    bool (*rebind)(void* user, const char* action, const char* from, const char* to) = nullptr;
    void (*set_view)(void* user, world::Entity camera, f32 blend_seconds) = nullptr;  // invalid: back to the player
    void (*camera_shake)(void* user, f32 strength, f32 seconds, f32 frequency) = nullptr;
    // view: 0 first person, 1 third person, 2 the Camera's own setting; get returns 0 or 1.
    void (*set_camera_view)(void* user, u32 view) = nullptr;
    u32 (*camera_view)(void* user) = nullptr;
    Vec3 (*camera_location)(void* user) = nullptr;
    Vec3 (*camera_forward)(void* user) = nullptr;
    void (*ik)(void* user, world::Entity e, const char* bone, Vec3 target, f32 weight, bool aim, Vec3 forward) = nullptr;
    void (*music)(void* user, int what, const char* name, f32 volume, f32 fade) = nullptr;  // 0 play, 1 stop, 2 layer
    bool (*timeline)(void* user, int what, const char* name, bool loop) = nullptr;           // 0 play, 1 stop
    bool (*socket)(void* user, world::Entity e, const char* name, Vec3& position, Vec3& forward) = nullptr;
    bool (*data_path)(void* user, const char* data_type, const char* name, char* out, usize capacity) = nullptr;  // a data asset by type and name
    // shape: 0 line a-b, 1 sphere at a (radius), 2 box at a (half size b), 3 text at a.
    void (*draw_debug)(void* user, int shape, Vec3 a, Vec3 b, f32 radius, const char* text, Vec3 color, f32 seconds) = nullptr;
    u32 (*bindings)(void* user, const char* action, const char** out, u32 capacity) = nullptr;
    const char* (*key_pressed)(void* user) = nullptr;
    void (*push_mix)(void* user, const char* name) = nullptr;
    void (*pop_mix)(void* user, const char* name) = nullptr;
    void (*set_audio_param)(void* user, const char* name, f32 value) = nullptr;
    void (*set_anim_param)(void* user, world::Entity e, const char* name, f32 value) = nullptr;
    bool (*ragdoll)(void* user, world::Entity e, bool on) = nullptr;  // false: no skeletal mesh there
    f32 (*play_montage)(void* user, world::Entity e, const char* animation, f32 rate, const char* from_bone, f32 blend, bool root_motion, const char* slot) = nullptr;
    void (*stop_montage)(void* user, world::Entity e, f32 blend, const char* slot) = nullptr;  // slot "": all
    bool (*montage_playing)(void* user, world::Entity e, const char* slot) = nullptr;          // slot "": any
    bool (*anim_curve)(void* user, world::Entity e, const char* name, f32& value) = nullptr;
    void (*set_motion_tag)(void* user, world::Entity e, const char* tag) = nullptr;
    void (*set_class_volume)(void* user, const char* name, f32 volume) = nullptr;
    // Vehicles and movers: 0 enter vehicle, 1 exit, 2 driven vehicle, 3 ride, 4 get off, 5 / 6 / 7 start / stop / restart a mover.
    u32 (*ride)(void* user, int what, world::Entity e) = nullptr;
    bool (*sublevel)(void* user, world::Entity e, bool load) = nullptr;  // a Sub Level object: load / unload its level
    bool (*input_recording)(void* user, int what, const char* file) = nullptr;  // 0 record, 1 play, 2 stop (files under Saved/)
    // Multiplayer (all null when it is off: scripts then see a standalone game).
    int (*net_mode)(void* user) = nullptr;   // 0 standalone, 1 server, 2 client
    u32 (*net_player)(void* user) = nullptr; // this machine's player id (0 = the server's own)
    void (*net_send)(void* user, i32 target, const char* name, const char* json) = nullptr;  // -1 server, -2 everyone, else a player
    bool (*net_chat)(void* user, const char* channel, const char* text) = nullptr;
    void (*net_set_group)(void* user, u32 player, u32 team, u32 squad) = nullptr;
    void (*net_group)(void* user, u32 player, u32& team, u32& squad) = nullptr;
    bool (*net_seen_position)(void* user, u32 entity, u32 player, Vec3& out) = nullptr;  // lag compensation (server)
    // LAN games: writes "address|name|players|max" lines into `out` (newline-separated); returns how many.
    u32 (*net_find_games)(void* user, f32 seconds, char* out, usize capacity) = nullptr;
    // Host (0, `arg` a level or ""), join (1, `arg` a join code or address), leave (2, `arg` a level or ""):
    // Play restarts there in that mode. False with `why` when it cannot.
    bool (*net_session)(void* user, int what, const char* arg, char* why, usize capacity) = nullptr;
    // Join codes, as lines: "upnp|off/working/open/failed|message|internet code", then "lan|address|code".
    void (*net_join_codes)(void* user, char* out, usize capacity) = nullptr;
    const char* (*editor_workspace)(void* user) = nullptr;  // the editor's workspace tab; "" in a packaged game
    // "name|guid|folder" lines of the assets of `type` (to_string(AssetType)) under `folder`; returns how many.
    u32 (*assets)(void* user, const char* type, const char* folder, char* out, usize capacity) = nullptr;
    // Newline-separated morph target names of e's skeletal mesh (or its child's); returns how many.
    u32 (*morph_names)(void* user, world::Entity e, char* out, usize capacity) = nullptr;
    bool (*ui_style)(void* user, i32 doc, const char* id, const char* property, const char* value) = nullptr;
    bool (*ui_rml)(void* user, i32 doc, const char* id, const char* rml) = nullptr;
    i32 (*ui_open_text)(void* user, const char* rml) = nullptr;  // a document from markup (the HUD widgets); -1 if none
    // Widget animations: property nullptr plays `value` as an RCSS animation; else tweens property to value.
    bool (*ui_animate)(void* user, i32 doc, const char* id, const char* property, const char* value, f32 seconds, const char* tween) = nullptr;
    // Editor utility scripts only (lsd._editor(command, arg) -> text): set by the editor, never in Play or a packaged
    // game, so lsd.editor.* raises there. With it set, start() runs no Script components (the level is being edited).
    bool (*editor)(void* user, const char* command, const char* arg, char* out, usize capacity) = nullptr;
};

class ScriptSystem {
public:
    ScriptSystem() = default;
    ~ScriptSystem();
    ScriptSystem(const ScriptSystem&) = delete;
    ScriptSystem& operator=(const ScriptSystem&) = delete;

    // `path_of(user, guid, out, capacity)` turns a Script component's asset GUID into a file path.
    using PathFn = bool (*)(void* user, const char* guid, char* out, usize capacity);
    void start(world::World& world, const ScriptHost& host, PathFn path_of, void* path_user);
    void update(f32 dt);  // reloads changed files, then calls on_update
    void stop();
    // Starts the scripts on `root` and its children (objects spawned during Play).
    void attach(world::Entity root);
    // lsd.game_instance() / player_state() / subsystem(name) live across stop / start (a level change)
    // as JSON; a new Play clears them (Unreal's Game Instance, Player State, Game Instance Subsystems).
    void clear_persistent();

    u32 script_count() const { return count_; }
    u32 error_count() const;  // scripts stopped by an error

    // Runs a chunk of Lua against the world (tests and the console). False on error (logged).
    bool run(const char* code, const char* chunk_name = "=console");
    // Where lsd.save_game writes (default Saved/SaveGames).
    void set_save_directory(const char* path);
    // Calls `event(self, other)` on the scripts of `target`, if they define it.
    // Native behaviours on `target` get it too. Returns how many scripts / behaviours had it.
    u32 send(world::Entity target, const char* event, world::Entity other);
    // The same with numbers for its arguments (entities as their handle value, NaN as nil); native behaviours get `other`.
    u32 send(world::Entity target, const char* event, const f64* args, u32 count, world::Entity other);
    // A behaviour tree's Run Script: `function(self, dt)` on `target`'s script. 1 = returned true,
    // 0 = false (or an error), 2 = nothing (still running), -1 = no such function.
    int call_task(world::Entity target, const char* function, f32 dt);
    // Calls `function(self, text)` on every script that defines it (UI actions). Returns how many.
    u32 broadcast(const char* function, const char* text);
    // lsd.<function>(args...) with a JSON array of arguments; the results as a JSON array in `out`.
    // False (and the error in `out`) if there is no such function, the JSON is bad or it raised one.
    bool call_api(const char* function, const char* json_args, std::string& out);
    // A network event: `function(self, from, ...)` on every script that defines it, the arguments
    // from a JSON array (multiplayer events, chat, players joining).
    void net_call(const char* function, u32 from, const char* json);

    // Debugging. Breakpoints by file and line (any script loaded from that file); the handler is
    // called when one is hit or a step ends, and its answer resumes. A trace sees every line run.
    void set_debugger(DebugHandler handler, void* user);
    void set_breakpoint(const char* source, i32 line, bool on);
    void clear_breakpoints();
    bool has_breakpoint(const char* source, i32 line) const;
    void set_line_trace(LineTrace trace, void* user);
    // A line of a JavaScript script runs (its line markers call this): the same breakpoints, steps and
    // trace as Lua lines. `depth` gives its call depth (asked only while stepping or stopping), `fill`
    // writes the stop. True when it stopped (the handler ran). Cheap when nothing is being debugged.
    static bool foreign_line(const char* source, i32 line, i32 (*depth)(void* user), void (*fill)(void* user, DebugStop& stop), void* user);
    static bool line_hooks_on();  // breakpoints, a step or a trace are set: foreign_line has work to do
    // Whether scripts loaded from now on get line markers (a debugger is attached).
    bool debuggable() const { return debugger_ != nullptr; }

    // C / C++ behaviours (Native Script components) from a game module; they start, update and get
    // events with the Lua scripts. Before the module's DLL is unloaded or swapped: stop_natives(),
    // then start_natives() with the new one: properties (what Details shows) keep their values, other
    // members start over (Live Coding keeps reflected state the same way).
    void set_native_module(const NativeModule* module) { module_ = module; }
    void stop_natives();
    void start_natives();
    u32 native_count() const { return native_count_; }

private:
    struct NativeInstance {
        world::Entity entity;
        const lsd_behaviour* behaviour;
        void* state;
    };
    // Invalid root: the whole world. `added` (optional) gets the slots filled.
    void add_natives(world::Entity root, u32* added = nullptr, u32* added_count = nullptr);
    // Hot reload keeps state: stop_natives saves every running script's properties as text (by object,
    // script and property name); the next start_natives puts back those that still exist. Stop clears it.
    struct KeptState;
    KeptState* kept_ = nullptr;
    void clear_kept();
    void create_natives();                 // the instances, without on_start
    // Native side of an event; returns how many behaviours took it.
    u32 native_event(world::Entity target, const char* event, world::Entity other, bool everyone);
    const NativeModule* module_ = nullptr;
    NativeInstance natives_[256];
    u32 native_count_ = 0;
    friend struct NativeApi;

    char save_dir_[260] = "Saved/SaveGames";
    char* persistent_ = nullptr;  // lsd.__persist as JSON between stop and the next start (heap, NUL-terminated)
    struct Instance {
        world::Entity entity;
        char path[260];
        i64 modified;  // file time when last loaded
        int env = 0;   // registry refs: the script's environment and its self table
        int self = 0;
        bool failed = false;
    };
    // Returns its slot (kNoSlot: none); a destroyed object's slot is reused, so spawn / destroy runs on.
    static constexpr u32 kNoSlot = ~0u;
    u32 add_instance(world::Entity entity, const char* guid);
    bool load(Instance& instance);  // false if it failed
    bool call(Instance& instance, const char* function, const f64* args, int arg_count);

    void update_hook();
    static void hook(lua_State* L, lua_Debug* ar);
    struct Breakpoint {
        char source[260];
        i32 line;
    };
    Breakpoint breakpoints_[64];
    u32 breakpoint_count_ = 0;
    DebugHandler debugger_ = nullptr;
    void* debugger_user_ = nullptr;
    LineTrace trace_ = nullptr;
    void* trace_user_ = nullptr;
    DebugAction stepping_ = DebugAction::Continue;
    i32 step_depth_ = 0;
    bool in_debugger_ = false;
    lua_State* lua_ = nullptr;
    js::Host* js_ = nullptr;  // QuickJS for .js scripts, made on the first one
    world::World* world_ = nullptr;
    ScriptHost host_;
    PathFn path_of_ = nullptr;
    void* path_user_ = nullptr;
    f64 time_ = 0;
    Instance instances_[256];
    struct CachedTable {
        char path[260];
        i64 modified;
        int ref;  // registry ref of the Lua table
    };
    CachedTable tables_[32];
    u32 table_count_ = 0;
    struct CachedCurve;
    CachedCurve* curves_ = nullptr;  // 32 entries, allocated with the Lua state
    u32 curve_count_ = 0;
    u32 count_ = 0;

    friend struct Api;
};

} // namespace lsd::script
