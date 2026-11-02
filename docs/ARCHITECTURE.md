# How LSD_Engine works

The engine's design, module by module. For using it, see [MANUAL.md](MANUAL.md).

## The big picture

```text
                 ┌──────────────────────────── editor/ (lsd_editor.exe) ───────────────────────────┐
                 │  main.cpp: window, frame loop, rendering        editor_ui.cpp: panels (ImGui)   │
                 │  play_session: Play (physics, scripts, AI, UI)  caches: mesh, texture, material │
                 │  graph / particle / UI / table editors           packaging, validation          │
                 └───────────────┬───────────────────────────────────────────────┬─────────────────┘
                                 │                                               │
   gameplay        script (LuaJIT, graphs)   ai (Recast, BTs)   ui (RmlUi)   audio (miniaudio)
   simulation      physics (Jolt)            animation (ozz)    particles    input
   data            world (entities, components, levels, prefabs)   asset (registry, import, tables)
   rendering       render (meshes, materials)   rhi (D3D12 / OpenGL / Vulkan)
   base            reflect (types, JSON)   core (jobs, console, crash)   foundation (memory, log, math)
```

Lower layers never include higher ones. Each folder under `engine/` is a static library
(`lsd_<name>`) with its public headers in `include/lsd/<name>/`. The editor executable links them
all and is also the shipped game (game mode).

Principles carried through every module:

- **Data-oriented.** Entities are handles; components are plain structs in packed arrays; systems
  loop over arrays. No inheritance in gameplay data.
- **Reflection drives tools.** A struct is declared once with annotations; its Details panel, file
  format, undo, Lua access and Play copies all come from the generated type information.
- **Text source, compiled on demand.** Assets stay in readable source formats in `Content/`;
  anything slow to read is compiled into `DerivedData/` and recompiled when the source changes.
- **Hot reload everywhere.** File timestamps are checked about once a second; textures, materials,
  meshes, scripts, graphs, particle systems and data tables reload while the editor (or Play) runs.

## Foundation and core

- `foundation`: fixed-size types (`u32`, `f32`...), `Allocator` (heap and arena; systems take an
  `Allocator&`), `Array<T>` (plain-data vector), generational `Handle<Tag>`, FNV-1a hashed ids,
  `Vec3` / `Mat4` math (column-major, Z-up), the log (console, an optional file, a sink for the
  Output Log, an error counter), system info (cores, SIMD, RAM), profiling macros (Tracy).
- `core`: the job system (workers = cores − 1, counters, waiting threads help with work), console
  variables and commands (the Output Log's command line), crash reporting (an unhandled-exception
  filter writes a minidump and a copy of the log, and leaves a marker the next start reports).

## Reflection

```cpp
LSD_STRUCT(Component, Category = "Gameplay")
struct Character {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "m/s", Min = 0) f32 walk_speed = 5.0f;
    LSD_PROPERTY(Edit, Widget = "Asset:InputMappingContext") char mapping_context[33] = "";
};
```

`tools/reflect` (`lsd_reflect_gen`) reads headers listed with `lsd_reflect(<target> <headers>)` in
CMake and writes a `.gen.cpp` with a `TypeInfo` per struct: fields, offsets, kinds (number, bool,
string, colour, vector, nested struct, `List<T, N>`), and metadata (Min / Max / Units / Tooltip /
Widget / Category). From that:

- `debug_ui` draws any struct as a Details panel (widgets chosen by kind and `Widget`: `Choice:A,B`,
  `Asset:<Type>` pickers, `Key` capture, colour pickers).
- `reflect` saves and loads any struct as JSON text (levels, data assets, settings) and binary.
- The world stores components by `TypeInfo`, so levels, undo snapshots, copy / paste, prefabs and
  Play copies need no per-component code.
- Lua's `lsd.get` / `lsd.set` / `lsd.add` convert components to and from tables by field name.

## World

`world::World` holds entities (generational handles, names, parent links, hidden / locked flags)
and one packed pool per component type (up to 32 types, listed in `components.h` and registered in
`world.cpp`). World transforms are computed from the hierarchy. A level file (`.lsdlevel`) is JSON:
World Settings then each entity with its components as reflected JSON. Prefabs (`.lsdprefab`) are
the same format for a sub-tree; instances remember per-field overrides.

## Assets

- `AssetRegistry` scans `Content/` recursively. Each file has a sidecar `<file>.meta` (created on
  first sight) with its GUID and import / asset settings (import scale, texture max size, sound
  volume, animation notifies...). The type comes from the extension (data assets also record the
  reflected type inside).
- References between assets are GUID strings, so renames and moves never break them.
- Importers: FBX (ufbx) and glTF (cgltf) to meshes (indexed and optimised with meshoptimizer, saved
  as compiled `.lsdmesh` in DerivedData), skinned FBX + clips through ozz, images through stb_image
  (mip chains built in linear light), sounds through miniaudio, CSV data tables, localisation tables.
- `validate_assets` scans text assets for GUIDs no asset has, oversize textures and name prefixes.

## Rendering

### RHI

`rhi::Device` is one interface with three backends (`engine/rhi/backends/`): Direct3D 12 (default),
OpenGL 4.6 and Vulkan 1.3 (window and UI until the Vulkan SDK supplies a SPIR-V compiler). The
device is chosen by scoring adapters (discrete over integrated, VRAM, features). The interface is
built-in pipelines rather than general command lists:

| Call | Draws |
| --- | --- |
| `draw_mesh(MeshDraw)` | lit meshes (static or GPU-skinned, up to 256 joints), optionally a material graph shader |
| `begin_shadow_pass` / `draw_shadow_caster` | depth into shadow atlases |
| `draw_sky(SkyDraw)` | the gradient sky with a sun disc |
| `draw_particles` | camera-facing sprites (alpha or additive) |
| `draw_ui(UiDraw)` | 2D triangles for RmlUi (premultiplied alpha, scissor) |
| `draw_lines` | the grid and debug lines |

Per-frame data (lights, skinning matrices, material parameters, particle and UI vertices) is copied
into ring buffers that cycle with the swapchain. The editor's ImGui panels are drawn by the same
device.

### The lit shader

One HLSL shader (with a GLSL mirror kept in step) shades every mesh: physically based (GGX, Smith,
Schlick), up to 16 lights a frame (directional in lux; point and spot in candela with smooth range
falloff), sky / ground ambient, normal maps without stored tangents (a frame from screen
derivatives), emissive. Shadows: the sun has 4 cascades in a 4096² atlas (practical split scheme,
texel-snapped, 3×3 hardware PCF, slope bias + normal offset); spot and point lights share a second
atlas of 16 tiles (6 per point light). Output is exposed, then `finish()` applies tint, the ACES
tonemapper, exponential height fog (integrated along the view ray), saturation, contrast and
vignette, and the result is encoded to sRGB by hand.

**Post-processing.** When a Post Process Volume applies, each viewport is drawn into its own scene
target with `LightSetup.output` on: the shaders then write the exposed colour (fog applied)
range-compressed into the 8-bit target (`e = sqrt(c / (1 + c))`, so highlights up to a few hundred
times white survive), and `draw_post` decodes it: a soft-knee bright pass and five half-size
downsamples, tent-filtered upsamples back up (bloom), then tint, ACES, saturation, contrast and
vignette into an LDR target with luma in alpha, and FXAA into the window. Without a volume the
forward shaders tonemap and grade directly, as before.

The editor builds the light setup each frame (`editor/lighting.cpp`): lights from components (the
nearest local lights first), cascades fitted to the camera, the camera's Post Process Volume, the
fog, and the player's quality setting. Meshes outside the camera's frustum are culled first. Static meshes of 512+ triangles get up to
three simpler LODs when they load (meshoptimizer simplify, sharing the LOD 0 vertices); the draw picks
one from the mesh's bounding radius over its distance (`r.ForceLOD n` to inspect one).

The post pass (`draw_post`) can read the scene target's depth (a sampleable depth texture on both
backends). Its order: ambient occlusion (from depth, then a depth-aware blur, multiplied into the
scene), TAA (the editor jitters the projection by a Halton offset; each pixel is found in last
frame's output through `reproject`, clamped to its neighbours and blended 10 % per frame), depth of
field (a disc gather sized by each pixel's blur), bloom, then the composite (tint, ACES, grading)
and FXAA when TAA is off. The passes share one 48-float constant block.

### Material graphs

A material graph is the same node graph format as visual scripts (`script::Graph`) with the material
node library. `compile_material_graph` walks back from the Material Output, declares one `float3`
temporary per node output (every value is a float3, so scalars and colours mix), and emits a single
function, `Surface material_main(MaterialIn mi)`, in HLSL spelling. The backends insert it into the
lit shader at a marker (OpenGL maps `float3` / `lerp` / `frac` / `saturate` with macros) and compile
static and skinned pipelines (`create_material_shader`). Parameters live in a 16-entry constant
buffer, so instances only change data. The material cache recompiles a graph when its file changes;
the graph window compiles its own preview 0.3 s after each edit.

## Play

`PlaySession` owns everything that only exists while playing:

1. **Start**: a snapshot of the level is saved (Stop restores it). The Game Mode runs (player
   prefab, game script, HUD). Static geometry and volumes become Jolt bodies; Rigid Bodies are
   simulated; joints are created; the navmesh is built; scripts start (`on_start`).
2. **Each frame**: input actions are evaluated (Enhanced Input-style triggers), the character moves
   (Jolt CharacterVirtual), followers move along splines, physics steps, transforms are written
   back, triggers and hits become script events, scripts update, AI perceives and its behaviour trees
   tick, Nav Agents move, UI elements following objects are placed, sounds follow their objects.
3. **Stop**: everything is released and the level snapshot restored.

## Scripting

- **Lua** (LuaJIT, built by our CMake): each Script component gets its own environment table with
  `on_start` / `on_update` / events and a `self` table. `ScriptSystem` reloads a script when its file
  changes (keeping `self`), and stops a script that errors until it is fixed. The `lsd` table is
  implemented in C++ (`script.cpp`); the game supplies callbacks (`ScriptHost`) for spawning,
  sounds, navigation, UI, settings...
- **Graphs** (`script/graph.h`): one data model and canvas for four node libraries: visual scripts,
  sound cues, material graphs and behaviour trees. A visual script compiles to Lua: each node is a
  code template (`$1` inputs, `$o1` outputs, `#0` exec chains), so a graph runs exactly like a
  hand-written script and the two cannot drift apart.

### Planned: C, C++ and JavaScript gameplay (milestone 7)

One API, several languages. Everything a script can do already goes through two plain-data
layers: `ScriptHost` (C function pointers the game fills in) and the reflection tables (component
types and fields by name). They become the public C API:

```text
 Game.dll (C / C++, Source/)      .lua scripts        .js scripts        visual scripts (.lsdgraph)
        |                              |                    |                       | compiled to Lua
 lsd_game.h (C ABI) + C++ wrappers   LuaJIT bindings     QuickJS bindings  ---------+
        \______________________________|____________________/
                     one binding list over ScriptHost + reflection
                                       |
                        World, physics, audio, UI, AI (unchanged)
```

- **Game module**: `Source/` builds `Game.dll` against `lsd_game.h` only (a C header, so C works
  and the ABI does not depend on the engine's compiler settings). The editor copies the DLL before
  loading it, so rebuilding is never blocked by the file lock, and reloads it when a build finishes.
- **Native Script** component: names a behaviour the module registered (a C++ class or a C struct of
  function pointers); the play session calls its events where it calls Lua's today.
- **Module components**: `LSD_STRUCT(Component)` in `Source/` goes through `lsd_reflect_gen` like
  engine headers; the world's component registry becomes runtime so these types get pools,
  Details, save / load, undo and Play copies. On hot reload their data is written out through
  reflection and read back into the new types by field name (added fields take defaults).
- **JavaScript**: the Script component accepts `.js`; a JS script system on QuickJS-ng mirrors the
  Lua one (own environment per instance, `self`, events, reload on change). The `lsd` functions are
  declared once and bound to both Lua and JS from that list.
- UI keeps HTML / CSS on RmlUi with each document's own `.js` (`engine/ui/src/ui_script.*`).

## Animation

ozz-animation behind `anim::SkinnedModel` (mesh + skeleton from FBX), `Clip` (every take, sampled
at 30 fps, matched by bone name so animations can come from other files) and `Pose` (sampling,
blending up to 4 clips, joint matrices for GPU skinning). An `AnimGraph` is a state machine of clip
or 1D blend-space states with parameter-driven, crossfading transitions; the editor's animation
player steps it each frame and fires notifies.

## Audio

miniaudio behind `audio::AudioSystem`: voices in Sound Class groups, each class routed through its
Submix effect chain into a master group, an environment reverb and the device. Per-voice low-pass
filters (Audio Volume muffling), 3D attenuation, concurrency limits, Sound Mixes (stacked volume
adjustments with fades). Sound Cues are graphs evaluated when played (random, mixer, delay,
modulator...).

## Particles

`particles::ParticleSystem` (a data asset of up to 8 emitters) and `ParticleInstance` (the CPU
simulation: spawn rate and bursts, shapes, cone directions, gravity, wind, drag, colour and size over
life, spin, local or world space). Each frame the editor builds camera-facing quads (sorted back to
front for alpha) and draws them after the opaque meshes.

## AI

- `ai::NavMesh` wraps Recast and Detour: it voxelises the level's collision boxes inside the Nav
  Mesh Bounds into a single-tile navmesh (Z-up to Recast's Y-up inside), answers path queries, and
  runs a Detour crowd for agents with avoidance.
- `ai::BehaviorTree` is a flat array of nodes built from a `.lsdbt` graph; `BtRunner` ticks it from
  the root every frame (reactive selectors abort lower-priority branches; sequences remember where
  they were), against a `Blackboard` of named numbers and locations. Movement, random points and Lua
  tasks are provided by the Play session through `BtHost`. Perception (sight range, field of view,
  physics line of sight; hearing by reported noises) writes into the blackboard.

## UI

RmlUi (HTML / CSS layout and styling) with FreeType fonts. `engine/ui` implements RmlUi's render
interface on the RHI (geometry kept on the CPU, drawn with `draw_ui`; textures premultiplied) and
system interface (time, log, translation). A `UiContext` is a screen of UI: documents, SDL input
(mouse, keys, text, spatial navigation for gamepads), and inline `on<event>="name"` attributes that
become actions; the Play session sends actions to Lua. Text that equals a string table key is
translated as documents load; switching language reloads them in place.

## Packaging and game mode

`package_project` validates the assets, copies the Release executable (renamed to the project),
`Content/`, `Config/` and the app-local Visual C++ runtime into `Build/<Project>/`, and writes
`game.json`. An executable that finds `game.json` beside it switches to that folder and runs as the
game: no editor panels, the Game Default Level straight into Play, Esc quits, no autosave or lock
files. Assets compile into the package's own DerivedData on first run.

## Files written at runtime

| Path | What |
| --- | --- |
| `Saved/Logs/Editor.log` (`Game.log`) | this session's log; the last kept as `-previous` |
| `Saved/Crashes/<date-time>/` | `crash.dmp` + `log.txt` |
| `Saved/Autosaves/` | autosaved levels |
| `Saved/GameUserSettings.json` | the player's settings |
| `Saved/SaveGames/<slot>.json` | save games (`{"lsd_save": 1, "data": ...}`) |
| `Saved/editor_settings.json`, `Saved/editor_layout.ini` | editor preferences and panel layout |
| `DerivedData/` | compiled meshes and other caches |

## Tests

`tests/` holds framework-free unit tests, one executable per module (foundation, reflect, render,
asset, world, editor, input, physics, script, audio, animation, particles, ai), run by CTest. They
cover the logic without a GPU: serialisation, import against ufbx's own skinning, physics, Lua, graph
compilers (Lua and shader code), behaviour trees, navmesh paths, save games, localisation. Rendering
is checked with editor screenshots (`-screenshot`) and D3D12 debug-layer runs (`-validation`);
gameplay with smoke tests (`-smoketest`).
