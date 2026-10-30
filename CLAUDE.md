# CLAUDE.md — LSD_Engine briefing

Read this first. It summarises every decision made so far for LSD_Engine.

## What this is

**LSD_Engine** is a proprietary 3D game engine by **Kittakin Studios** (owner: Charlize Pauli).
It is inspired by the *public ideas* behind Bitsquid/Stingray (data-oriented, data-driven, Lua
gameplay) but has an **editor-first workflow like Unreal Engine**: designers import a file, find it
in the Content Browser, open it in its own editor and wire it up by clicking. Code is only for new
behaviour, never for hooking up content.

- Repo: `github.com/Kittakin-Studios/LSD_Engine` (private)
- License: **Proprietary, all rights reserved** (see `LICENSE`). Never add GPL code; LGPL only as a
  separately linked library. Third-party code goes in `third_party/` with its licence file.
- Full plan doc (architecture, feature spec, editor look, roadmap):
  https://claude.ai/artifact/Q73b6jeNowQ4rQJGqRYwxW

## Hard rule: clean room

Read `docs/CLEAN_ROOM.md`. Never use Stingray/Bitsquid binaries, decompiled output, leaked source,
licensee-only docs, or code shown on screen in talks/videos (Niklas Gray's YouTube Stingray
walkthroughs show proprietary source — ideas only). Use our own names, formats and APIs.

## Developer environment

- Windows, AMD Ryzen 7 7800X3D (16 threads, AVX2 + AVX-512), 31 GiB RAM
- Repo at `E:\LSD_Engine\LSD_Engine` (repo root = project root)
- VS Code + CMake Tools + C/C++ extensions, MSVC compiler, preset **`windows-msvc`**
  (Ninja Multi-Config). Also builds on Linux with preset `linux-clang`.
- Build: `cmake --preset windows-msvc` → `cmake --build --preset windows-msvc`
- Run game: `build\windows-msvc\runtime\Debug\lsd_runtime.exe [-rhi=d3d12|vulkan|opengl] [-gpu=<n>]`
- Run editor: `build\windows-msvc\editor\Debug\lsd_editor.exe` (same flags)
- Tests: `ctest --preset windows-msvc`

## How Charlize likes to work

- Concise, direct answers. Step-by-step instructions for anything in the IDE or terminal.
- She debugs by sharing screenshots — ask for one when something fails.
- She is an experienced Unreal 5 developer and technical artist; map new concepts to their
  Unreal equivalents (Content Browser, Details panel, Blueprints, UMG, Niagara, Sequencer…).
- Keep this engine separate from her other game projects.

## Key decisions

| Area | Decision |
| --- | --- |
| Language | C++20 runtime and tools; LuaJIT for gameplay; visual scripting (Blueprint-like) generated from the same reflection layer |
| Graphics | One RHI interface, three backends: **Direct3D 12** (Windows default), **Vulkan 1.3** (Linux/Steam Deck), **OpenGL 4.6** (fallback). Shaders in HLSL → DXIL / SPIR-V / GLSL (SPIRV-Cross) |
| GPU vendors | NVIDIA, AMD, Intel via standard drivers. Game compute = D3D12/Vulkan compute shaders. **CUDA / ROCm(HIP)** only as optional plugins for editor/offline tools (baking, denoising, ML), never required by the shipped game |
| Scaling | Auto-select best GPU (discrete > integrated, VRAM, features); read live VRAM budget (`QueryVideoMemoryInfo` / `VK_EXT_memory_budget`) and system RAM; size texture streaming, shadows and render targets to the budget; degrade quality instead of crashing; auto Low/Medium/High/Epic preset; job workers = logical cores − 1; SIMD chosen at runtime |
| Architecture | Data-oriented: entities are handles, components live in SoA managers, systems loop over arrays. Data compiler turns text source into binary blobs; hot reload everywhere |
| Editor foundation | Reflection (code generator from C++ annotations) → Details panel, serialization, undo, visual-script nodes and Lua bindings all generated. Asset registry with GUIDs, safe renames, reference viewer. Transactions for undo/redo. Editor-only data stripped at packaging |
| UI system | UMG-style widget designer **plus** HTML/CSS/JS: a Code tab with HTML, CSS and JS editors kept in two-way sync with the designer. Rich text accepts inline HTML/CSS. Responsive via anchors, flexbox, media queries, DPI curve, safe zones. Runtime: **RmlUi + QuickJS** (both MIT); Ultralight is the alternative if full web compatibility is ever needed |
| Libraries | SDL3 (window/input), Jolt (physics), ozz-animation, miniaudio, Recast/Detour, ufbx + cgltf + meshoptimizer (import), RmlUi + QuickJS (game UI), Dear ImGui (debug/editor v1), Tracy (profiling) |
| Lighting | Directional, point, spot, rect (area), disc/sphere area, sky light, emissive; physical units (lux, lumens, candela, Kelvin); IES profiles; light functions; mobility Static/Stationary/Movable; LTC for area lights; baked GI + probes in v1, dynamic GI in v2; volumetric fog; post-process volumes |

## Editor look (default layout)

Large **3D viewport** top-left (near-black `#0D0404`, perspective grid floor with a major line every
5 units, red X / green Y axis lines, axis gizmo bottom-left, toolbar: Perspective, Lit, Show, Grid).
**Outline/Hierarchy** panel top-right (striped rows `#3C3838` / `#242855`, per-type text colours,
yellow scroll thumb `#FFE51A`). **Details / World Partition** bottom-right (gold active tab `#F3C312`,
rows `#596CC1` / `#3C3838`). **Content Browser** along the bottom (orange tab `#EEA70E`, blue bar
`#025291`, area `#2D2D30`, folder tree black with lime border `#9BE11F`, yellow text `#FBF800`).
All colours live in a theme asset. Fix contrast before shipping: folder text on tan row, prefab
blue `#0040FF` → `#5C8DFF`, VFX orange on red → `#FFC266`.

## Repository layout

```
engine/foundation/  types, logging, asserts, heap + arena allocators, generational handles,
                    constexpr hashed IDs (FNV-1a 64), system info (CPU, cores, SIMD, RAM)
engine/core/        job system (worker threads, counters, wait-by-helping), console variables/commands
engine/reflect/     reflection tables (LSD_STRUCT/LSD_PROPERTY/LSD_GENERATED), JSON save/load,
                    save_file/load_file, LinearColor (Color flag -> colour picker)
engine/debug_ui/    ImGui Details panel generated from TypeInfo (runtime overlay + editor v1)
engine/render/      mesh data, built-in shapes, GPU upload, grid lines
engine/asset/       GUIDs, .meta sidecars, AssetRegistry, FBX/glTF import, .lsdmesh compiled format
engine/rhi/         RHI interface, adapter scoring/selection, quality presets, create_device();
                    backends/d3d12, backends/vulkan, backends/opengl (each also renders ImGui)
engine/world/       entities (generational handles), packed component pools per reflected type,
                    hierarchy, built-in components (Transform, StaticMesh, PlayerStart), .lsdlevel text files
engine/input/       Input Action / Mapping Context data assets and the InputSystem (keys, mouse, gamepad)
engine/physics/     Jolt behind a small API: bodies (box/sphere/capsule) and the player's CharacterVirtual
engine/script/      LuaJIT gameplay scripts: Script component -> on_start / on_update, `lsd` API, hot reload;
                    graph.h: visual-script graphs and their compiler to Lua
engine/audio/       miniaudio behind AudioSystem: voices, Sound Class groups, attenuation, concurrency
engine/ui           placeholder
editor/             lsd_editor: level editor. Menu bar, Viewport (gizmos via ImGuizmo, picking, Unreal
                    camera), Outliner, Details (components), Content Browser + Place, undo history
                    (whole-level snapshots), Play-in-Editor; theme in Config/, user settings in Saved/
tools/data_compiler/ CLI stub: --source-dir / --data-dir
tools/reflect/      lsd_reflect_gen: header -> .gen.cpp; CMake: lsd_reflect(<target> <headers>)
runtime/            game exe; hardware report, SDL3 window, triangle, reflected Details panel
third_party/        fetched dependencies, pinned tags (SDL3, Dear ImGui, ImGuizmo, Vulkan-Headers, volk,
                    ufbx, cgltf, meshoptimizer, Jolt Physics, LuaJIT built by our CMake)
tests/              framework-free unit tests (CTest)
docs/               ROADMAP.md, CLEAN_ROOM.md, BACKLOG.md (every spec feature by milestone, ticked when done)
```

Conventions: namespace `lsd`, snake_case functions, `PascalCase` types, every system takes an
`Allocator&`, no STL in hot paths, `LSD_LOG_*` macros, warnings as clean as possible (`/W4`,
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`).

## Status

Work is on branch `milestone-1` (not merged to `main` yet). Full checklist: `docs/BACKLOG.md`.

**Milestone 0 — done.** Builds on MSVC; tests pass (Linux/Clang not re-checked since).

**Milestone 1 — done.** Job system; SDL3 window; RHI with D3D12, Vulkan 1.3 (volk, dynamic
rendering, VK_EXT_memory_budget) and OpenGL 4.6; reflection generator + JSON/binary save/load;
runtime Details panel from reflection. Vulkan draws only clear + UI until the **Vulkan SDK** is
installed (its DXC compiles HLSL to SPIR-V; it also brings validation layers).

**Milestone 2 — done.** Renderer basics (buffers, depth, lit-mesh and line pipelines, render
targets) on D3D12 and OpenGL; asset registry with GUID `.meta` sidecars; FBX/glTF import; compiled
`.lsdmesh` + data compiler; editor (`lsd_editor.exe`) with themed dock layout, GPU grid viewport,
Content Browser (thumbnails, search, import by drop, rename/move/delete, folders, drag to place),
Outliner of placed objects, Details for the selected asset or the theme. Data assets moved to
milestone 4, texture/audio import to milestone 5.

**Milestone 3 — in progress, exit test works.** World (entities + component pools + hierarchy),
`.lsdlevel` levels (new/open/save/save as, reopen last), move/rotate/scale gizmos with snapping,
picking, Unreal camera controls, Outliner tree with drag-to-attach, component Details, Place panel,
undo/redo with history, Play-in-Editor (first-person walk/jump with box collision until Jolt; pause,
step, eject), multi-select + box select, copy/paste, ortho views, Lit/Unlit/Wireframe, Output Log +
console (`engine/core/console`), stats, camera bookmarks, autosave + crash recovery, Blocking/Kill
volumes, World Settings, Project Settings (`Config/project.json`), multi-edit + Reset to Default in
Details, groups, hide/lock, Message Log, 2x2 viewport layout. Remaining milestone 3 items are in
`docs/BACKLOG.md` (Simulate, play in new window, pivot editing, vertex snap, Reference Viewer...).

**Milestone 4 — in progress, exit test works.** Reflection lists (`List<T, N>`) and Widget hints
(`Choice:`, `Asset:<Type>`, `Key`); data assets (`.lsdasset`, `engine/asset/data_asset.h`); Enhanced
Input-style Input Actions + Mapping Contexts (`engine/input`); Jolt physics (`engine/physics`: static
level collision, RigidBody, CharacterVirtual); Character component possessed in Play with its input
wired in Details. Prefabs (`.lsdprefab`, `engine/world/prefab.h`: create, place, apply, revert; no
per-instance overrides yet). Gameplay tags (Tags component). Lua scripts (`engine/script`, LuaJIT
built from source by third_party/CMakeLists.txt with its own minilua/buildvm tools). Lua reaches any
component through reflection (`lsd.get/set/add`), has timers, and gets overlap events from Trigger
Volumes. Lua can spawn
(`lsd.spawn`), raycast and gets `on_hit`; Game Mode data asset in World Settings; third-person spring
arm on the Character (`camera_distance`). Visual scripting:
`.lsdgraph` node graphs edited in the Visual Script window, compiled to Lua (Saved/Graphs/<guid>.lua)
and run by the Script component like any Lua file. Physics Joint
component (fixed / hinge / ball / slider, limits, motors). Data tables
(.csv assets, grid editor, `lsd.table`). Input
triggers (Hold, Tap...). Splines + Spline Follower (moving platforms). Curves (Curve
data asset with a graph editor, `lsd.curve`). Prefab
per-instance overrides (PrefabInstance.overrides, Revert to Prefab). Left for later: Player Controller /
State, Lua debugger, user-defined structs and enums.

**Milestone 5 — in progress.** Audio foundation on miniaudio (`engine/audio`): Sound assets with .meta
settings, Sound Class / Attenuation / Concurrency data assets, Sound Emitter, trigger sounds, Lua and
node playback, `stat audio`. Sound Cues (`.lsdcue`, same graph canvas as visual scripts, evaluated by
editor/sound_cue.cpp) and Sound Mixes (push / pop with fades). Submixes (effect chains
on Sound Classes, incl. our own compressor node) and Audio Volumes (reverb + muffling). The audio worked
example works except the animation notify. Rendering: textures (`engine/asset/texture.h`, stb_image),
`rhi::Device::create_texture`, PBR mesh shading with Material data assets (`engine/render/material.h`,
editor/material_cache), Material Instances, light components (editor/lighting.cpp -> rhi::LightSetup,
up to 16 per frame) with World Settings exposure, cascaded sun shadows (rhi shadow maps + passes,
cascades fitted in editor/lighting.cpp). Next: local-light shadows, material graph, animation, particles.

Debug flags (runtime and editor): `-rhi=d3d12|vulkan|opengl`, `-gpu=<n>`, `-validation`,
`-frames=<n>`; editor only: `-screenshot=<n>` (OpenGL, saves `Saved/screenshot.bmp`),
`-import=<file>`, `-place=<asset name>`, `-level=<path>`, `-play`, `-select=<object name>`,
`-exec="<console line>"` (repeatable, e.g. `-exec="view top" -exec="viewmode wireframe"`),
`-nosound` (no audio device: use it for every automated run so test sounds never play on the speakers).

Reflection decisions: annotation macros `LSD_STRUCT` / `LSD_PROPERTY` / `LSD_FUNCTION` plus
`LSD_GENERATED()` in the body (like `GENERATED_BODY()`); reflected types are plain structs, no
inheritance (composition only); strings are fixed `char name[N]`; source assets save as JSON text,
binary only after the data compiler. World is Z-up, right-handed (like Blender), metres. Rotations
are Euler degrees X then Y then Z (Blender's XYZ Euler). Components are reflected plain structs; the
world stores them by TypeInfo, so Details, level files, undo and Play copies need no per-component code.

## Roadmap (editor-first)

| # | Milestone | Exit test |
| --- | --- | --- |
| 1 | Core + reflection: job system, SDL3 window, first RHI backend (D3D12) drawing a triangle, then Vulkan and OpenGL; reflection code generator; serialization | A reflected object saves, loads and shows its properties in a debug panel |
| 2 | Assets + Content Browser: asset registry, importers, data compiler, thumbnails | Import a Blender FBX and find it by search |
| 3 | Editor shell: viewport with grid + gizmos, Outliner, Details, undo, Play-in-Editor | Block out a level and play it |
| 4 | Gameplay: prefabs, components, Lua + visual scripting, input assets, physics, character movement | A character runs; input wired with no code |
| 5 | Content editors: material graph, animation graph + blend spaces, audio (Sound Cue/Class/Mix/Attenuation/Submix), particle editor | The audio walkthrough in the plan doc works end to end |
| 6 | Vertical slice: lighting + post, UI designer, navmesh + behaviour trees, save game, packaging | A packaged level runs on another PC |
