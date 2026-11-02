# Coding gameplay in C and C++

How to write game code for LSD_Engine in **C or C++**, and every command the engine gives you.
Unreal mapping: this is your project's C++ module (`Source/`), Actor / Component `BeginPlay` and
`Tick`, `UPROPERTY`, `UFUNCTION(BlueprintCallable)` and Live Coding.

> **Status.** C / C++ gameplay is **milestone 7** (see [BACKLOG.md](BACKLOG.md)). This page is the
> API it implements. Until it lands, every command below already works in **Lua** under the same
> name (`lsd.find`, `lsd.set_position`...) and from visual scripts, so you can build the game now
> and move code to C++ later without changing what it does. The authoritative list of what runs
> today is the comment at the top of `engine/script/include/lsd/script/script.h`.

Contents: [Project layout](#project-layout) · [Build and reload](#build-and-reload) ·
[Your first behaviour](#your-first-behaviour-c) · [Events](#events) · [Naming rules](#naming-rules-one-command-three-languages) ·
[Command reference](#command-reference) · [Your own components](#your-own-components) ·
[Blueprint nodes from C++](#blueprint-nodes-from-c) · [Writing it in plain C](#writing-it-in-plain-c) ·
[Console commands](#console-commands) · [Rules and gotchas](#rules-and-gotchas)

## Project layout

```text
MyGame/
  Content/            assets (levels, meshes, materials, .lua / .lsdgraph scripts)
  Config/             project settings, theme, input
  Source/             your C / C++ code -> builds Game.dll
    CMakeLists.txt    written by File > New C++ Class; add your files to it
    door.cpp
    pickup_counter.cpp
```

Game code includes **one header**: `#include <lsd/game.h>` (C) or `#include <lsd/game.hpp>`
(C++, header-only wrappers over the C one). It never includes engine headers, so `Game.dll` does
not depend on how the engine was compiled.

## Build and reload

| Do this | How | Unreal equivalent |
| --- | --- | --- |
| Add a class | File > New C++ Class > Behaviour or Component | Tools > New C++ Class |
| Build | Tools > Compile, or **Ctrl+Alt+F11**; errors go to the Message Log | Compile / Live Coding |
| Reload | Automatic when the build finishes, also during Play; object state is kept | Live Coding |
| Debug | **F5** in VS Code attaches to `lsd_editor.exe`; breakpoints in your `.cpp` | Attach to Process |
| Ship | File > Package Project builds Release and puts `Game.dll` beside the exe | Package Project |

From a terminal (the same build the button runs):

```powershell
cmake --preset windows-msvc -S Source -B Intermediate/Game
cmake --build Intermediate/Game --config Debug
```

A failed build leaves the previous `Game.dll` running.

## Your first behaviour (C++)

A **behaviour** is code attached to an object, like a Blueprint's event graph. Put a
**Native Script** component on the object in Details and pick the behaviour by name, the same way
a Script component picks a `.lua`.

```cpp
// Source/door.cpp: opens when the player walks into its trigger.
#include <lsd/game.hpp>
using namespace lsd::game;

LSD_STRUCT(Behaviour, Category = "Doors")
struct Door {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "deg") f32 open_angle = 90.0f;   // shown in Details, per object
    LSD_PROPERTY(Edit, Units = "s") f32 open_time = 1.0f;
    f32 t = 0.0f;                                              // plain fields: private state
    bool opening = false;

    void on_overlap_begin(Self self, Entity other) {
        if (other == player()) {
            opening = true;
            play_sound("S_DoorOpen", position(self));
        }
    }
    void on_update(Self self, f32 dt) {
        if (!opening || t >= 1.0f) return;
        t = min(t + dt / open_time, 1.0f);
        set_rotation(self, {0.0f, 0.0f, open_angle * t});
    }
};
LSD_BEHAVIOUR(Door)
```

- `Self` is the object the behaviour sits on (an `Entity`).
- Define only the events you need; there is no base class and nothing to override.
- `LSD_PROPERTY(Edit)` fields are Unreal's *Instance Editable*: each object can set its own values.

## Events

| Event (C++ member) | When | Unreal |
| --- | --- | --- |
| `void on_start(Self self)` | Play starts, or the object is spawned | BeginPlay |
| `void on_update(Self self, f32 dt)` | Every frame, `dt` in seconds | Tick |
| `void on_stop(Self self)` | Play stops, or the object is destroyed | EndPlay |
| `void on_hit(Self self, Entity other)` | A physics contact began | OnHit |
| `void on_overlap_begin(Self self, Entity other)` | Something entered its Trigger Volume | OnComponentBeginOverlap |
| `void on_overlap_end(Self self, Entity other)` | Something left it | OnComponentEndOverlap |
| `void on_timeline_finished(Self self, const char* name)` | A timeline ended | Sequencer event |
| `void <any_name>(Self self, const char* element_id)` | A UI element's `onclick="any_name"` | UMG OnClicked |
| `void <any_name>(Self self, ...)` | `broadcast("any_name", ...)` or `call(e, "any_name", ...)` | Event Dispatcher / Interface |

## Naming rules: one command, three languages

Every command has the same name everywhere:

| C++ | C | Lua (works today) |
| --- | --- | --- |
| `find("Door")` | `lsd_find("Door")` | `lsd.find("Door")` |
| `set_position(e, {1, 2, 3})` | `lsd_set_position(e, (lsd_vec3){1, 2, 3})` | `lsd.set_position(e, 1, 2, 3)` |
| `raycast(from, dir, 10)` → `std::optional<Hit>` | `lsd_raycast(from, dir, 10, &hit)` → `bool` | `lsd.raycast(x, y, z, dx, dy, dz, 10)` → `e, px...` or `nil` |

- C++ uses `Vec3` / `Color` where Lua uses three or four numbers.
- C++ returns `std::optional` where Lua returns `nil`; C returns `bool` and fills an out pointer.
- Lists come back as `std::span` in C++, and as `(out, capacity)` → count in C.
- An `Entity` that is not set (`Entity{}`, C `LSD_NONE`) is Lua's `nil`.
- World units: **metres**, **degrees**, Z up; rotations are Euler X, then Y, then Z.

## Command reference

C++ signatures, in `namespace lsd::game`. For C, add `lsd_` (see the rules above).

### Objects

| Command | What it does |
| --- | --- |
| `Entity find(const char* name)` | The object with that Outliner name, or none |
| `const char* name(Entity e)` | Its name |
| `bool alive(Entity e)` | Still exists? |
| `void destroy(Entity e)` | Removes it and its children |
| `Entity spawn(const char* asset, Vec3 at)` | Places a prefab, mesh, particle system or shape (`"Cube"`) by name |
| `Entity player()` | The player's Character, or none |
| `f64 time()` | Seconds since Play started |

### Transform

| Command | What it does |
| --- | --- |
| `Vec3 position(Entity e)` / `void set_position(Entity e, Vec3 p)` | Location, relative to its parent |
| `Vec3 rotation(Entity e)` / `void set_rotation(Entity e, Vec3 degrees)` | Rotation, relative to its parent |

### Components (any type, through reflection)

Field names are the ones Details shows (`walk_speed`, not "Walk Speed").

| Command | What it does |
| --- | --- |
| `template<T> std::optional<T> get(Entity e)` | A copy of the component, e.g. `get<Character>(e)` |
| `template<T> void set(Entity e, const T& value)` | Writes the whole component back |
| `template<T> T& add(Entity e)` / `void remove<T>(Entity e)` / `bool has<T>(Entity e)` | Add Component / Destroy Component / is it there |
| `set_field(e, "Character", "walk_speed", 8.0f)` | One field by name (also the only form in C) |

### Gameplay tags

| Command | What it does |
| --- | --- |
| `bool has_tag(Entity e, const char* tag)` | `"Damage"` matches `"Damage.Fire"` |
| `has_any_tag` / `has_all_tags` / `has_no_tag(Entity e, const char* list)` | Several at once: `"Enemy, Boss"` |

### Input

| Command | What it does |
| --- | --- |
| `Vec2 input(const char* action)` | An Input Action's value (by asset name, e.g. `"IA_Move"`) |
| `bool pressed(const char* action)` | True on the frame it fires (its trigger: pressed, hold, tap...) |
| `std::span<const char*> bindings(const char* action)` | The keys it listens to |
| `bool rebind(const char* action, const char* old_key, const char* new_key)` | Changes a key and saves it |
| `const char* key_pressed()` | A key that went down this frame ("press a key" menus), or null |

### Physics queries

All return the nearest hit (`Hit{entity, point, normal}`) or nothing.

| Command | What it does |
| --- | --- |
| `raycast(Vec3 from, Vec3 direction, f32 distance)` | Line Trace |
| `sphere_trace(Vec3 from, Vec3 to, f32 radius)` | Sphere Trace |
| `box_trace(Vec3 from, Vec3 to, Vec3 half_size)` | Box Trace |
| `capsule_trace(Vec3 from, Vec3 to, f32 radius, f32 half_height)` | Capsule Trace |
| `overlap_sphere(Vec3 at, f32 radius)` / `overlap_box(Vec3 at, Vec3 half_size)` → list of `Entity` | Overlap |

### Splines and curves

| Command | What it does |
| --- | --- |
| `spline_point(Entity e, f32 distance)` → `{point, direction}` | World point along `e`'s Spline |
| `f32 spline_length(Entity e)` | Metres |
| `f32 curve(const char* name, f32 time)` | A Curve asset (`curve_vector`, `curve_color` for the others) |
| `f32 curve_table(const char* table, const char* row, f32 time)` | A Data Table used as a curve table |

### Audio

| Command | What it does |
| --- | --- |
| `u32 play_sound(const char* name)` / `play_sound(name, Vec3 at)` | 2D or at a place; 0 if there is no such sound |
| `void stop_sound(u32 id)` | |
| `play_music(name, fade = 1)` / `stop_music(fade = 1)` | Crossfaded track |
| `music_layer(name, volume, fade = 1)` | A stem in step with the track |
| `push_mix(name)` / `pop_mix(name)` | Sound Mixes |
| `set_audio_param(name, value)` | Read by Sound Cue Switch nodes |
| `set_class_volume(name, volume)` | A Sound Class (`"Master"` too): settings sliders |

### Animation

| Command | What it does |
| --- | --- |
| `set_anim_param(Entity e, const char* name, f32 value)` | An Animation Graph parameter |
| `set_morph(Entity e, const char* name, f32 weight)` | A morph target (blend shape) |
| `ik_reach(Entity e, const char* bone, Vec3 target, f32 weight = 1)` | Two-bone IK: hand on a ledge |
| `ik_look_at(Entity e, const char* bone, Vec3 target, f32 weight = 1, Vec3 forward = {1,0,0})` | Head / turret aim |
| `socket(Entity e, const char* name)` → `{position, forward}` | A socket in the world |
| `play_timeline(name, loop = false)` / `stop_timeline(name)` | Cutscenes (Sequencer) |

### Camera

| Command | What it does |
| --- | --- |
| `set_view(Entity camera, f32 blend = 0)` | Set View Target with Blend; `Entity{}` returns to the player |
| `camera_shake(f32 strength, f32 seconds = 0.5, f32 frequency = 12)` | 1 = a hard hit |

### AI

| Command | What it does |
| --- | --- |
| `bool move_to(Entity e, Vec3 target)` | AI Move To on the navmesh; false if unreachable |
| `stop_move(Entity e)` / `bool is_moving(Entity e)` | |
| `find_path(Vec3 from, Vec3 to)` → list of `Vec3` | Path corners |
| `bb_set(e, key, f32 or Vec3)` / `bb_get(e, key)` / `bb_clear(e, key)` | The AI Controller's blackboard |
| `report_noise(Vec3 at, f32 loudness = 1)` | Heard by AI perception |

### UI (HTML / CSS documents)

| Command | What it does |
| --- | --- |
| `i32 ui_open(const char* name)` | Shows a `.rml` document over the game (Create Widget + Add to Viewport); -1 if none |
| `ui_close(doc)` | |
| `ui_set_text(doc, id, text)` / `ui_set(doc, id, attribute, value)` | Change an element |
| `ui_value(doc, id)` | A text field's contents |
| `ui_show_cursor(bool on)` | Menus: the mouse moves a cursor |
| `ui_follow(doc, id, Entity e, f32 height = 0)` / `ui_unfollow(doc, id)` | Name tags, health bars |

### Text and languages

| Command | What it does |
| --- | --- |
| `text(key, args...)` | String table text in the current language; fills `{0}`, `{1}` and plurals |
| `set_language(code)` / `languages()` | `"en"`, `"fr"`...; open UI is re-translated |

### Settings, saves, data

| Command | What it does |
| --- | --- |
| `get_settings()` / `set_settings(settings)` | The player's options (quality, volumes, sensitivity...) |
| `save_game(slot, value)` / `load_game(slot)` / `has_save(slot)` / `delete_save(slot)` | SaveGame, to `Saved/SaveGames/<slot>.json` |
| `save_objects(slot, entities, components)` / `load_objects(slot)` | Whole objects by name |
| `table(name)` | A Data Table (`.csv`): rows and columns |
| `enum_value(name, entry)` / `make_struct(name)` | Editor-made enums and structs |

### Talking between objects

| Command | What it does |
| --- | --- |
| `u32 broadcast(const char* event, args...)` | Calls `event` on every object that defines it (Event Dispatcher) |
| `u32 call(Entity e, const char* event, args...)` | Only on `e` (Interface message) |
| `bool implements(Entity e, const char* event)` | Does `e` handle it? |

These reach Lua scripts, visual scripts and C++ behaviours alike.

### Timers and game-wide state

| Command | What it does |
| --- | --- |
| `u32 after(f32 seconds, fn)` / `u32 every(f32 seconds, fn)` / `cancel(id)` | Set Timer by Event |
| `game_instance()` / `player_state()` / `subsystem(name)` | Data kept across level changes |
| `bool open_level(const char* name)` | Open Level, after this frame |

### Debug output

| Command | What it does |
| --- | --- |
| `log(fmt, ...)` / `log_error(fmt, ...)` | Output Log (Print String) |
| `draw_line(a, b, color = red, seconds = 0)` | Draw Debug Line |
| `draw_sphere(at, radius, ...)` / `draw_box(at, half_size, ...)` / `draw_text(at, text, ...)` | The other debug shapes |

## Your own components

Data the Details panel should show and the level should save goes in a **component**
(Unreal: a `USTRUCT` / `UActorComponent` with `UPROPERTY`s). Same macros as the engine's own:

```cpp
// Source/health.h
#include <lsd/game.hpp>

LSD_STRUCT(Component, Category = "Gameplay")
struct Health {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Min = 0) f32 max = 100.0f;
    LSD_PROPERTY(Edit, Min = 0) f32 current = 100.0f;
};
```

After a build it is in Add Component, Details, undo, save / load and Play like any built-in one,
and Lua and visual scripts reach it too (`lsd.get(e, "Health")`, Get / Set Health nodes).

Rules: plain structs, no inheritance, no pointers; text is a fixed `char name[N]`; lists are
`List<T, N>`. A field you add on reload takes its default value; a removed field is dropped.

## Blueprint nodes from C++

Mark a free function and it appears in the visual script Palette under its category
(Unreal's `BlueprintCallable`; `Pure` = no execution pins, like `BlueprintPure`):

```cpp
LSD_FUNCTION(Callable, Category = "Combat", Tooltip = "Hurts the target; returns true if it died")
bool apply_damage(Entity target, f32 amount);

LSD_FUNCTION(Pure, Category = "Math")
f32 damage_falloff(f32 distance, f32 radius);
```

Pin types come from the parameter types: `bool`, `i32`, `f32`, `Vec3`, `Color`, `Entity`,
`const char*` (text).

## Writing it in plain C

C behaviours register a struct of function pointers; leave unused events null.

```c
#include <lsd/game.h>

typedef struct Spinner { float speed; } Spinner;

static void spinner_update(lsd_entity self, void* state, float dt) {
    Spinner* s = (Spinner*)state;
    lsd_vec3 r = lsd_rotation(self);
    r.z += s->speed * dt;
    lsd_set_rotation(self, r);
}

LSD_EXPORT void lsd_game_register(lsd_registry* registry) {
    lsd_behaviour b = {0};
    b.name = "Spinner";
    b.state_size = sizeof(Spinner);
    b.on_update = spinner_update;
    lsd_register_behaviour(registry, &b);
}
```

C has no reflection macros: properties are registered with `lsd_add_property(&b, "speed",
LSD_F32, offsetof(Spinner, speed), 90.0f)`.

## Console commands

Type these in the console line at the bottom of the Output Log tab, or pass them on start with
`-exec="<command>"`. `help <prefix>` lists them with their help text.

| Command | What it does |
| --- | --- |
| `help [prefix]` | Commands and variables |
| `clear` | Clears the Output Log |
| `play` / `simulate` / `stop` | Play-in-Editor, Simulate (Alt+S), stop |
| `open <level>` / `save` | Open a level / save the current one |
| `lua <code>` | Runs one line of Lua against the level |
| `stat fps` (or `stat unit`) / `stat audio` | Overlays: frame time and draw calls / active sounds |
| `view perspective\|top\|front\|right` | Viewport angle |
| `layout 1\|2\|4` | One, two or four viewports |
| `viewmode lit\|unlit\|wireframe\|normals\|lightingonly\|collision\|overdraw` | View mode |
| `HighResShot <1-8>` | Screenshot at that scale to `Saved/Screenshots` |
| `BuildLighting` | Bake the level's light probes |
| `validate` | Validate Assets |
| `package` | File > Package Project |
| `memreport` | Every allocator's memory to the log |
| `asset.Select <name>` | Select an asset and show it in Details |
| `mesh.Open` / `prefab.Open` / `graph.Open` / `particles.Open` / `table.Open` / `timeline.Open` / `ui.Open <name>` | Open an asset in its editor |
| `input.CreateDefaults` | Create IA_Move, IA_Look, IA_Jump and IMC_Default |
| `debug.Break <file> <line>` | Toggle a Lua breakpoint |
| `test.Smoke [seconds]` / `test.Unit` | Smoke test this level / run the unit tests |
| `debug.Crash`, `ui.Click x y`, `ui.Keys ...` | Tests only |

Console variables (`r.ForceLOD 1` and so on) are listed by `help` too.

## Rules and gotchas

- **Use names, not pointers.** Keep an `Entity`, never a pointer to a component: pools move when
  objects are added. `get<T>` returns a copy; call `set` to write it back.
- **No engine headers.** Only `lsd/game.h(pp)`; that is what keeps hot reload and packaging safe.
- **State survives reload** only in reflected fields (`LSD_PROPERTY`) and plain fields of
  behaviours; static / global variables are reset.
- **Frame cost.** `on_update` runs for every object every frame. Use `after` / `every` timers or
  overlap events instead of polling where you can.
- **Clean room.** Write everything yourself; never paste Stingray / Bitsquid code or anything shown
  on screen in talks ([CLEAN_ROOM.md](CLEAN_ROOM.md)).
