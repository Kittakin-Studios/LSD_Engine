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
//   lsd.time()                            seconds since Play started
//   lsd.input(action) -> x, y             value of an Input Action, by asset name
//   lsd.pressed(action)                   true on the frame the action fires (its Trigger: pressed, hold, tap...)
//   lsd.log_error(...)                    print as an error
//   lsd.spawn(name, x, y, z) -> e         a prefab, mesh asset or built-in shape ("Cube") by name; nil if none
//   lsd.raycast(x, y, z, dx, dy, dz, distance) -> e, px, py, pz, nx, ny, nz   nearest hit, or nil
//   lsd.spline_point(e, distance) -> x, y, z, dx, dy, dz   world point and direction along e's Spline
//   lsd.spline_length(e)                  metres
//   lsd.play_sound(name [, x, y, z]) -> id   a sound asset by name, 2D or at a position; 0 if none
//   lsd.stop_sound(id)
//   lsd.push_mix(name), lsd.pop_mix(name)   Sound Mix data assets by name
//   lsd.set_audio_param(name, value)      read by Sound Cue Switch nodes
//   lsd.set_anim_param(e, name, value)    an Animation Graph parameter on e's skeletal mesh (and its children's)
//   lsd.set_class_volume(name, volume)    a Sound Class by name ("Master" too): settings sliders
//   lsd.curve(name, time) -> value        a Curve data asset by name (0 if none); re-read when it changes
//   lsd.move_to(e, x, y, z) -> ok         a Nav Agent walks there on the navmesh (AI Move To); false if unreachable
//   lsd.stop_move(e), lsd.is_moving(e) -> bool
//   lsd.find_path(x, y, z, tx, ty, tz) -> {{x, y, z}, ...}   corners of the navmesh path (empty if none)
//   lsd.bb_set(e, key, number | x, y, z), lsd.bb_get(e, key) -> number | x, y, z | nil, lsd.bb_clear(e, key)
//                                         an AI Controller's blackboard (what its behaviour tree reads)
//   lsd.report_noise(x, y, z [, loudness]) AI Controllers in hearing range set NoiseLocation
//   lsd.ui_open(name) -> doc              a UI document (.rml) by asset name over the game; nil if none
//   lsd.ui_close(doc), lsd.ui_set_text(doc, id, text), lsd.ui_set(doc, id, attribute, value)
//   lsd.ui_value(doc, id) -> text         a text field's value
//   lsd.ui_show_cursor(on)                a menu: the mouse moves a cursor instead of the player
//   lsd.ui_follow(doc, id, e [, height])  keeps that element over the object (name tags, health bars);
//                                         hidden when it is behind the camera; lsd.ui_unfollow(doc, id)
//   lsd.get_settings() -> {fullscreen, quality, master_volume, music_volume, effects_volume,
//                          mouse_sensitivity, invert_look}   the player's settings (options menus)
//   lsd.set_settings({quality = 1, ...})  changes those given, applies and saves them
//   lsd.text(key) -> text                 the string table's text in the current language (the key if missing)
//   lsd.set_language(code)                switches (en, fr...): open UI documents are reloaded translated
//   lsd.languages() -> {"en", "fr", ...}
// UI events: an element's onclick="start_game" (onchange, onsubmit...) calls start_game(self, element_id)
// on every script that defines it.
//   lsd.save_game(slot, value) -> ok      Unreal's SaveGame: numbers, strings, booleans and tables (lists or
//                                         string keys) to Saved/SaveGames/<slot>.json, versioned
//   lsd.load_game(slot) -> value          what was saved, or nil; lsd.has_save(slot), lsd.delete_save(slot)
//   lsd.table(name) -> {Row = {Column = value}}   a Data Table (.csv) by asset name; numbers where
//                                         cells are numbers; re-read when the file changes; nil if none
//
// Any component, through reflection (field names as in Details; structs are tables):
//   lsd.get(e, "Character")               a copy of the component as a table, or nil
//   lsd.set(e, "Character", {walk_speed = 8})          writes only the fields given
//   lsd.add(e, "RigidBody" [, fields]), lsd.remove(e, "RigidBody"), lsd.has(e, "RigidBody")
//
// Timers (callbacks run before on_update):
//   lsd.after(seconds, fn) -> id, lsd.every(seconds, fn) -> id, lsd.cancel(id)
//
// Events sent by the game: on_overlap_begin(self, other), on_overlap_end(self, other) (Trigger Volumes),
// on_hit(self, other) (physics contact began; the player counts as its Character).
//
// Saving a script while playing reloads it; `self` keeps its fields. A script that errors stops
// running (the error and stack go to the Output Log) until it is fixed and saved.
#include "lsd/foundation/math.h"
#include "lsd/world/world.h"
#include "lsd/world/curve.h"

struct lua_State;

namespace lsd::script {

// What the game provides to scripts besides the world.
struct ScriptHost {
    void* user = nullptr;
    Vec3 (*input)(void* user, const char* action) = nullptr;   // x, y (z unused)
    bool (*pressed)(void* user, const char* action) = nullptr;
    world::Entity (*spawn)(void* user, const char* name, Vec3 position) = nullptr;
    bool (*raycast)(void* user, Vec3 origin, Vec3 direction, f32 distance, world::Entity& hit, Vec3& point, Vec3& normal) = nullptr;
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
    i32 (*ui_open)(void* user, const char* name) = nullptr;  // -1 if none
    void (*ui_close)(void* user, i32 doc) = nullptr;
    bool (*ui_set)(void* user, i32 doc, const char* id, const char* attribute, const char* value) = nullptr;  // attribute nullptr: the text
    bool (*ui_value)(void* user, i32 doc, const char* id, char* out, usize capacity) = nullptr;
    void (*ui_show_cursor)(void* user, bool on) = nullptr;
    void (*ui_follow)(void* user, i32 doc, const char* id, world::Entity e, f32 height) = nullptr;  // e invalid: stop
    // The player's settings: a reflected struct, and a call after Lua changed it.
    void* (*settings)(void* user, const reflect::TypeInfo** type) = nullptr;
    void (*settings_changed)(void* user) = nullptr;
    const char* (*text)(void* user, const char* key) = nullptr;
    void (*set_language)(void* user, const char* code) = nullptr;
    u32 (*languages)(void* user, const char** out, u32 capacity) = nullptr;
    void (*push_mix)(void* user, const char* name) = nullptr;
    void (*pop_mix)(void* user, const char* name) = nullptr;
    void (*set_audio_param)(void* user, const char* name, f32 value) = nullptr;
    void (*set_anim_param)(void* user, world::Entity e, const char* name, f32 value) = nullptr;
    void (*set_class_volume)(void* user, const char* name, f32 volume) = nullptr;
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

    u32 script_count() const { return count_; }
    u32 error_count() const;  // scripts stopped by an error

    // Runs a chunk of Lua against the world (tests and the console). False on error (logged).
    bool run(const char* code, const char* chunk_name = "=console");
    // Where lsd.save_game writes (default Saved/SaveGames).
    void set_save_directory(const char* path);
    // Calls `event(self, other)` on the scripts of `target`, if they define it.
    void send(world::Entity target, const char* event, world::Entity other);
    // A behaviour tree's Run Script: `function(self, dt)` on `target`'s script. 1 = returned true,
    // 0 = false (or an error), 2 = nothing (still running), -1 = no such function.
    int call_task(world::Entity target, const char* function, f32 dt);
    // Calls `function(self, text)` on every script that defines it (UI actions). Returns how many.
    u32 broadcast(const char* function, const char* text);

private:
    char save_dir_[260] = "Saved/SaveGames";
    struct Instance {
        world::Entity entity;
        char path[260];
        i64 modified;  // file time when last loaded
        int env = 0;   // registry refs: the script's environment and its self table
        int self = 0;
        bool failed = false;
    };
    void add_instance(world::Entity entity, const char* guid);
    bool load(Instance& instance);
    bool call(Instance& instance, const char* function, const f64* args, int arg_count);

    lua_State* lua_ = nullptr;
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
