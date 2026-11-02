# Coding gameplay in C and C++

How to write game code for LSD_Engine in **C or C++**, and every command the engine gives it.
Unreal mapping: `Source/` is your project's C++ module, a *script* is an Actor Component class
(`BeginPlay` / `Tick` / `OnHit`), its properties are `UPROPERTY(EditAnywhere)`, and compiling
reloads it while the editor runs, like Live Coding.

C / C++ sits next to the other ways to script, not instead of them: Lua scripts, visual scripts
and C / C++ scripts run side by side in the same level, get the same events and can send each
other events. Step-by-step clicking for all of them: [How to code in LSD_Engine](How%20to%20code%20in%20LSD_Engine.md).

Contents: [Quick start](#quick-start) · [Project layout](#project-layout) ·
[A script in C++](#a-script-in-c) · [Events](#events) · [Properties](#properties-shown-in-details) ·
[Command reference](#command-reference) · [Writing it in plain C](#writing-it-in-plain-c) ·
[Talking to Lua and visual scripts](#talking-to-lua-and-visual-scripts) · [Build, reload, debug, ship](#build-reload-debug-ship) ·
[Console commands](#console-commands) · [Rules and gotchas](#rules-and-gotchas) · [Not in C / C++ yet](#not-in-c--c-yet)

## Quick start

1. **Tools > New C / C++ Script...**, type a name (e.g. `Spinner`), **Create**. The editor writes
   `Source/Spinner.cpp` from a template and compiles it (the Output Log shows the build).
2. Select an object, **Details > Add Component > Native Script**, and pick **Spinner** in its
   **Script** dropdown.
3. **Play**: it spins. Edit `Source/Spinner.cpp` in VS Code, save, press **Ctrl+Alt+F11** (Tools >
   Compile Game Code): the new code loads by itself, also while playing.

You need Visual Studio 2022 or newer with **Desktop development with C++** (the same one that
builds the engine). The editor finds it on its own.

## Project layout

```text
<project>/
  Content/              assets, levels, Lua and visual scripts
  Source/               your C / C++ game code -> Binaries/Game.dll
    CMakeLists.txt      written once by New C / C++ Script: every .c / .cpp here is built
    GameModule.cpp      the module's entry point: LSD_GAME_MODULE()  (exactly one per module)
    Spinner.cpp         one script per file (any number of files)
  Binaries/Game.dll     the build (not in git); packaged games ship it
  Intermediate/         build files (not in git)
```

Game code includes **one header**: `<lsd/game.hpp>` (C++) or `<lsd/lsd_game.h>` (C). It never
sees engine headers, so `Game.dll` does not depend on how the engine was compiled.

## A script in C++

A script is a plain struct: its fields are its per-object state, its member functions are the
events it wants. `LSD_SCRIPT(Name)` registers it; no base class, nothing to override.

```cpp
// Source/Door.cpp: opens when the player walks into its Trigger Volume.
#include <lsd/game.hpp>
using namespace lsd::game;

struct Door {
    float open_angle = 90.0f;  // defaults; each object can change them in Details
    float open_time = 1.0f;
    float t = 0.0f;            // plain state, per object
    bool opening = false;

    static void properties(Properties<Door>& p) {
        p.add("open_angle", &Door::open_angle);
        p.add("open_time", &Door::open_time);
    }

    void on_overlap_begin(Self self, Entity other) {
        if (other == player()) {
            opening = true;
            play_sound("S_DoorOpen", position(self));
        }
    }

    void on_update(Self self, float dt) {
        if (!opening || t >= 1.0f) return;
        t = t + dt / open_time > 1.0f ? 1.0f : t + dt / open_time;
        set_rotation(self, {0.0f, 0.0f, open_angle * t});
    }
};
LSD_SCRIPT(Door)
```

`Self` and `Entity` are object handles (0 = none). Each object with this script gets its own
`Door` (constructed with `new`, so default member values apply), created when Play starts.

## Events

| Member function | When | Unreal |
| --- | --- | --- |
| `void on_start(Self self)` | Play starts, or the object is spawned | BeginPlay |
| `void on_update(Self self, float dt)` | Every frame, `dt` in seconds | Tick |
| `void on_stop(Self self)` | Play stops (also before a reload) | EndPlay |
| `void on_hit(Self self, Entity other)` | A physics contact began | OnHit |
| `void on_overlap_begin(Self self, Entity other)` | Something entered its Trigger Volume | OnComponentBeginOverlap |
| `void on_overlap_end(Self self, Entity other)` | Something left it | OnComponentEndOverlap |
| `void on_event(Self self, const char* name, Entity other)` | Any named event: `call` / `broadcast` from any language, Call Event nodes, UI actions (`onclick="name"`) | Custom Event / Interface |

## Properties shown in Details

`static void properties(Properties<T>& p)` lists the fields an object can override; they appear
under the Native Script as **Script properties**, with **Reset** back to the default. Types:
`float`, `int32_t`, `bool`, `char name[N]`. The object's values are written into its state before
`on_start`.

## Command reference

C++ names in `namespace lsd::game`; C adds `lsd_` (`find` -> `lsd_find`); Lua uses the same name
under `lsd.` (`lsd.find`). Vectors are `Vec3 {x, y, z}` (C: `lsd_vec3`), metres and degrees, Z up.

### Objects and transform

| C++ | What it does |
| --- | --- |
| `Entity find(const char* name)` | The object with that Outliner name, or 0 |
| `const char* name(Entity e)` / `bool alive(Entity e)` / `void destroy(Entity e)` | |
| `Entity spawn(const char* asset, Vec3 at)` | Places a prefab, mesh, particle system or shape (`"Cube"`) |
| `Entity player()` | The player's Character, or 0 |
| `double time()` | Seconds since Play started |
| `Vec3 position(e)` / `set_position(e, Vec3)` | Location, relative to the parent |
| `Vec3 rotation(e)` / `set_rotation(e, Vec3 degrees)` | Rotation, relative to the parent |
| `bool has_tag(e, const char* tag)` | Gameplay tags (`"Damage"` matches `"Damage.Fire"`) |

### Any component, by name

Component and field names are the ones Details shows (`"Character"`, `"walk_speed"`).

| C++ | What it does |
| --- | --- |
| `bool has(e, "RigidBody")` / `add(e, "RigidBody")` / `remove(e, "RigidBody")` | |
| `std::optional<double> get_number(e, "Character", "walk_speed")` | Numbers and booleans (1 / 0) |
| `bool set_number(e, "Character", "walk_speed", 8)` | |
| `get_vec3` / `set_vec3(e, "Transform", "scale", {2, 2, 2})` | Vector fields |
| `get_text` / `set_text(e, "NativeScript", "script", "Door")` | Text fields |

### Input, physics, audio, AI, camera, levels, UI

| C++ | What it does |
| --- | --- |
| `Vec3 input(const char* action)` / `bool pressed(const char* action)` | Input Actions by asset name |
| `std::optional<Hit> raycast(Vec3 from, Vec3 dir, float distance)` | Line Trace: `hit->entity`, `point`, `normal` |
| `std::optional<Hit> sphere_trace(Vec3 from, Vec3 to, float radius)` | Sphere Trace |
| `uint32_t play_sound(name)` / `play_sound(name, Vec3 at)` / `stop_sound(id)` | |
| `set_anim_param(e, name, value)` | Animation Graph parameter |
| `bool move_to(e, Vec3 target)` | AI Move To on the navmesh |
| `camera_shake(strength, seconds = 0.5, frequency = 12)` | |
| `bool open_level(name)` | Open Level, after this frame |
| `int ui_open(name)` / `ui_close(doc)` / `ui_set_text(doc, id, text)` | UI documents (.rml) |
| `int call(Entity target, const char* event, Entity other = 0)` | Event on one object's scripts and scripts |
| `int broadcast(const char* event)` | Event on every script and script |
| `log(fmt, ...)` / `log_error(fmt, ...)` | Output Log, printf-style |
| `draw_line(Vec3 a, Vec3 b, Vec3 color = red, float seconds = 0)` | Debug line |

## Writing it in plain C

**Tools > New C / C++ Script...**, pick **C**, type a name: the editor writes `Source/<Name>.c`.
A C script is a struct of event functions (leave the ones you do not need NULL), registered by a
function that `LSD_C_SCRIPT(...)` runs when the code loads; C and C++ scripts live side by side in
`Source/` and you never list them anywhere.

```c
/* Source/Spinner.c */
#include <lsd/lsd_game.h>
#include <stdlib.h>

typedef struct Spinner { float speed; } Spinner;  /* per-object state */

static void* Spinner_create(void) {  /* defaults: shown in Details, changed per object */
    Spinner* s = (Spinner*)calloc(1, sizeof(Spinner));
    if (s) s->speed = 90.0f;
    return s;
}
static void Spinner_destroy(void* state) { free(state); }

static void Spinner_update(lsd_entity self, void* state, float dt) {
    lsd_vec3 r = lsd_rotation(self);
    r.z += ((Spinner*)state)->speed * dt;
    lsd_set_rotation(self, r);
}

static const lsd_property Spinner_properties[] = {{"speed", LSD_FLOAT, offsetof(Spinner, speed), 0}};

static void Spinner_register(lsd_registry* registry) {
    lsd_script script = {0};
    script.name = "Spinner";
    script.state_size = sizeof(Spinner);
    script.create = Spinner_create;
    script.destroy = Spinner_destroy;
    script.on_update = Spinner_update;
    script.properties = Spinner_properties;
    script.property_count = 1;
    lsd_register_script(registry, &script);
}
LSD_C_SCRIPT(Spinner_register)
```

C names are the C++ ones with `lsd_` in front (`lsd_find`, `lsd_set_position`, `lsd_raycast(from,
dir, distance, &hit)` returning 1 when it hits). Without `create`, the state is `state_size` zeroed
bytes. `Source/GameModule.cpp` (written once, `LSD_GAME_MODULE()`) collects every C and C++ script;
leave it in place.

## Talking to Lua and visual scripts

Events cross languages both ways:

- C++ -> Lua: `call(door, "unlock")` runs `function unlock(self, other)` in the door's Lua script,
  and the **Custom Event** `unlock` in its visual script.
- Lua -> C++: `lsd.call(door, "unlock", self.entity)` and visual scripts' **Call Event on Object**
  reach the script's `on_event(self, "unlock", other)`.
- `broadcast` / `lsd.broadcast` / **Call Event** reach everyone; UI `onclick="start_game"` too.

## Build, reload, debug, ship

| Do this | How |
| --- | --- |
| Add a script | Tools > New C / C++ Script (or console `game.NewScript Name` (add `c` for C)) |
| Build | Tools > Compile Game Code, **Ctrl+Alt+F11**, or console `game.Compile`; the lines go to the Output Log |
| Reload | Automatic when `Binaries/Game.dll` changes. During Play, every script gets `on_stop`, the new code loads, and they start again (`on_start`, fresh state) |
| See what loaded | console `game.Scripts` |
| Debug | VS Code / Visual Studio: attach to `lsd_editor.exe`, breakpoints in `Source/*.cpp` (built with debug info, no optimisation) |
| Ship | File > Package Project copies `Binaries/` into the package; the game loads it at start |

A failed build leaves the running code as it was. The build is CMake + Ninja with the newest
installed MSVC (`Intermediate/compile_game.bat`); on Linux, the compilers on PATH.

## Console commands

Type these in the console line at the bottom of the Output Log tab, or pass them on start with
`-exec="<command>"`. `help <prefix>` lists them with their help text.

| Command | What it does |
| --- | --- |
| `help [prefix]` | Commands and variables |
| `clear` | Clears the Output Log |
| `play` / `simulate` / `stop` | Play-in-Editor, Simulate (Alt+S), stop |
| `open <level>` / `save` | Open a level / save the current one |
| `lua <code>` | Runs Lua in the game while playing |
| `game.Compile` / `game.NewScript <Name> [c]` / `game.Scripts` | C / C++ game code: build, new script, list |
| `scene.List` / `asset.List [text]` / `log.Tail [n]` | The level's objects, the assets, the latest log lines |
| `asset.Open <name>` | Open an asset in its editor, like a double-click in the Content Browser |
| `screenshot` | Save the next frame to `Saved/screenshot.bmp` (OpenGL, `-rhi=opengl`) |
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

- **Keep handles, not pointers.** Store `Entity` values; components move in memory as objects come
  and go. `get_*` returns copies; `set_*` writes.
- **Only `lsd/game.hpp` / `lsd/lsd_game.h`.** No engine headers: that is what keeps reloading and
  packaging safe.
- **State starts over on reload.** A script's fields are rebuilt when new code loads; keep what
  must survive in components (`set_number`...) or Lua's `lsd.game_instance()`.
- **Statics and globals** in game code reset on every reload too.
- **A script added during Play** (`add(e, "NativeScript")`) starts with the next Play or reload;
  objects *spawned* from prefabs that have one start at once.
- **Frame cost.** `on_update` runs for every object every frame; prefer overlaps and events to
  polling.
- **Clean room.** Write everything yourself; never paste Stingray / Bitsquid code or anything shown
  on screen in talks ([CLEAN_ROOM.md](CLEAN_ROOM.md)).

## Not in C / C++ yet

These are Lua-only for now (call them from Lua, or send an event to a Lua script that does):
box / capsule traces and overlaps, splines, curves and data tables, music and mixes, IK, morphs,
sockets, timelines, blackboards and perception, `set_view`, UI cursor / follow / values, settings,
key rebinding, text and languages, save games, timers (`after` / `every`), Game Instance data,
debug spheres / boxes / text. Still planned for C++: C++ functions as visual-script nodes, your own
reflected components in `Source/`, keeping script state across a reload, and JavaScript gameplay
scripts ([BACKLOG.md](BACKLOG.md), milestone 7).
