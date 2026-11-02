# CLAUDE.md — LSD_Engine briefing

Read this first. It is the short, current summary of how LSD_Engine is built and how to work on it.
The user-facing documentation is in `docs/`: [MANUAL.md](docs/MANUAL.md) (how to use the engine),
[ARCHITECTURE.md](docs/ARCHITECTURE.md) (how it works), [EXAMPLES.md](docs/EXAMPLES.md) (worked
examples), [CODING.md](docs/CODING.md) (C / C++ gameplay and every command), [BACKLOG.md](docs/BACKLOG.md) (every feature by milestone, ticked when done).

## What this is

**LSD_Engine** is a proprietary 3D game engine by **Kittakin Studios** (owner: Charlize Pauli). It
takes the *public ideas* behind Bitsquid/Stingray (data-oriented, data-driven, Lua gameplay) and gives
them an **editor-first workflow like Unreal Engine**: import a file, find it in the Content Browser,
open it in its own editor, wire it up by clicking. Code is for new behaviour, never for hooking up content.

- Repo: `github.com/Kittakin-Studios/LSD_Engine` (private). Work happens on branch `milestone-1`.
- License: **proprietary, all rights reserved** (`LICENSE`). Never add GPL code; LGPL only as a
  separately linked library. Third-party code is fetched by `third_party/CMakeLists.txt`, pinned, and
  listed with its licence in `third_party/README.md` (MIT / BSD / zlib / FreeType licence only).
- Plan doc (architecture, spec, editor look, roadmap): <https://claude.ai/artifact/Q73b6jeNowQ4rQJGqRYwxW>

## Hard rule: clean room

Read `docs/CLEAN_ROOM.md`. Never use Stingray/Bitsquid binaries, decompiled output, leaked source,
licensee-only docs, or code shown on screen in talks/videos. Ideas only; our own names, formats and APIs.

## Working with Charlize

- Concise, direct answers; step-by-step instructions for anything in the IDE or terminal.
- She debugs by sharing screenshots: ask for one when something fails.
- She is an experienced Unreal 5 developer and technical artist: map every concept to its Unreal
  equivalent (Content Browser, Details, Blueprints, UMG, Niagara, Sequencer...).
- Keep this engine separate from her other game projects.
- Commit and push after each finished chunk (explicit pathspecs, message ends with the
  `Co-Authored-By` line); `gh` is not logged in, so no pull requests.

## Build, run, test

- Windows, MSVC, VS Code + CMake Tools, preset **`windows-msvc`** (Ninja Multi-Config). Linux: `linux-clang`.
- `cmake --preset windows-msvc` then `cmake --build --preset windows-msvc` (Debug);
  Release: `cmake --build --preset windows-msvc --config Release` (packaging uses it).
- Tests: `ctest --preset windows-msvc` (14 suites, framework-free, in `tests/`). Keep them passing.
- Editor: `build\windows-msvc\editor\Debug\lsd_editor.exe`. The same exe is the game (`-game`, or a
  folder with `game.json` from File > Package Project).
- Both Debug and Release build with **0 warnings**; keep it that way (`/W4`, `-Wall -Wextra -Wpedantic
  -Wshadow -Wconversion`). Third-party code gets its warnings silenced where it is built.

**Automated runs must be silent**: always pass `-nosound` (test audio once reached her speakers).
Useful flags: `-rhi=d3d12|vulkan|opengl`, `-gpu=<n>`, `-validation` (D3D12 debug layer), `-frames=<n>`
(quit after n frames), `-screenshot=<n>` (OpenGL: Saved/screenshot.bmp; does not quit by itself),
`-level=<path>`, `-play`, `-select=<name>`, `-import=<file>`, `-place=<asset>`, `-exec="<console line>"`
(repeatable; runs once the editor is up), `-smoketest=<seconds>` (CI: exit 1 if errors were logged), `-vram=<MB>` (pretend video memory),
`-remote[=port]` (Tools > Remote Console connects to it; localhost only).
Console: `asset.Open <name>` (double-click), `screenshot` (OpenGL, next frame). Test-only console commands: `ui.Click x y`, `ui.Drag x0 y0 x1 y1 [shift|alt|ctrl]`, `ui.Keys down enter` (real SDL events, 0.8 s later),
`debug.Crash`. After an automated run, delete the test files you made in Content/ and Saved/.

Tooling notes for AI sessions: write Python helper scripts with the file tool (bash heredocs mangle
`\n` / `\0` escapes); the scratchpad has `build.bat` (configure + build + ctest), `build_editor.bat`,
`build_target.bat <targets>`, `build_release.bat`, `configure.bat`. Close Charlize's running editor
before linking `lsd_editor.exe` (the exe is locked while it runs).

## Key decisions

| Area | Decision |
| --- | --- |
| Language | C++20 engine and editor. Gameplay: LuaJIT, visual scripts (compiled to Lua) and C / C++ scripts (a `Game.dll` from `Source/` over the C API `lsd_game.h` + `lsd/game.hpp`, Native Script component, reloaded on rebuild); JavaScript scripts (QuickJS-ng) planned. UI: HTML / CSS (RmlUi) + each document's `.js` |
| Graphics | One RHI, three backends: **D3D12** (default), **OpenGL 4.6** (fallback, screenshots), **Vulkan 1.3** (UI only until the Vulkan SDK is installed). Built-in shaders are HLSL + a GLSL mirror kept in step |
| Architecture | Data-oriented: entities are generational handles, components are reflected plain structs in packed pools, systems loop over arrays. Text source assets, hot reload everywhere |
| Reflection | `LSD_STRUCT` / `LSD_PROPERTY` / `LSD_GENERATED()` + a code generator: Details panel, JSON save/load, undo, Lua access and Play copies come free. No inheritance; fixed `char name[N]` strings; `List<T, N>` for lists |
| World | Z-up, right-handed (like Blender), metres; rotations are Euler degrees X then Y then Z |
| Assets | GUIDs in `.meta` sidecars, `AssetRegistry` scans Content/; files stay in their source format and compile on demand into DerivedData/ |
| UI | RmlUi (HTML / CSS) drawn by `rhi::Device::draw_ui`; fonts from Content/Fonts + the system UI font as `sans`; designer canvas in the UI window (`rml_edit.h` writes into the text); a document's own `.js` runs on QuickJS-ng (`engine/ui/src/ui_script.*`) |
| Libraries | SDL3, Dear ImGui (editor), ImGuizmo, Jolt Physics, LuaJIT, ozz-animation, miniaudio, Recast & Detour, ufbx, cgltf, meshoptimizer, stb, FreeType, RmlUi, QuickJS-ng, Tracy (optional) |

## Editor look

3D viewport top-left (near-black `#0D0404`, grid, red X / green Y axes, axis gizmo bottom-left);
Outliner top-right (rows `#3C3838` / `#242855`, yellow scroll thumb `#FFE51A`); Details / World
Partition bottom-right (gold active tab `#F3C312`, rows `#596CC1` / `#3C3838`); Content Browser along
the bottom (orange tab `#EEA70E`, blue bar `#025291`, folder tree black with lime border `#9BE11F`,
yellow text `#FBF800`). All colours come from the theme asset (`Config/`).

## Repository layout

```text
engine/foundation/  types, log (+ log file, error count), allocators, handles, hashed ids, system info, math,
                    profile.h (Tracy zones when built with -DLSD_TRACY=ON)
engine/core/        job system, console variables / commands, crash reporting (minidump + log)
engine/reflect/     reflection tables, JSON save / load, List<T, N>, LinearColor
engine/rhi/         RHI interface + D3D12 / OpenGL / Vulkan: meshes (PBR, skinned, material graphs),
                    shadows, sky, particles, UI, lines, render targets
engine/render/      mesh data, built-in shapes, grid, Material / MaterialInstance data
engine/asset/       GUIDs, .meta, AssetRegistry, FBX / glTF import, textures, data assets, data tables,
                    localization (string tables)
engine/world/       World (entities, component pools, hierarchy), components.h (every component),
                    levels, prefabs, splines, curves
engine/input/       Input Actions, Mapping Contexts, triggers, InputSystem
engine/physics/     Jolt: bodies, character, joints, raycasts
engine/game_api/    lsd_game.h (C API for game code) and lsd/game.hpp (C++ layer); no engine headers
engine/script/      LuaJIT ScriptSystem and the lsd API; native.cpp runs C / C++ scripts; graph.h (all node libraries: visual scripts,
                    sound cues, materials, behaviour trees) and their compilers (Lua, shader code)
engine/animation/   ozz: skinned models, clips, poses, blending, animation graphs
engine/audio/       miniaudio: voices, sound classes, submixes, attenuation, concurrency, mixes
engine/particles/   ParticleSystem data and CPU simulation
engine/ai/          Recast & Detour navmesh + crowd; behaviour trees and blackboards
engine/ui/          RmlUi contexts (documents, input, actions, translation)
engine/debug_ui/    ImGui Details panel generated from reflection
editor/             lsd_editor (also the game): main.cpp (loop, rendering), editor_ui.cpp (panels),
                    play_session (Play: physics, scripts, AI, UI, audio), caches (mesh, texture, material,
                    skinned), graph / particle / UI / table editors, packaging, navigation, behaviour trees
runtime/            minimal runtime exe (hardware report, triangle); the shipped game is lsd_editor -game
tools/              lsd_reflect_gen (reflection code generator), data_compiler, mcp (lsd_mcp: Claude MCP plugin bridge)
tests/              unit tests (CTest)
docs/               MANUAL, ARCHITECTURE, EXAMPLES, BACKLOG, ROADMAP, CLEAN_ROOM
```

Conventions: namespace `lsd`, snake_case functions, PascalCase types, systems take an `Allocator&`,
no STL in hot paths (editor-time code may use it, marked `shortcut:`), `LSD_LOG_*` macros, one-line
comments for the *why*. A known limitation gets `// shortcut: <limit>, <when to upgrade>`.

## Status

| # | Milestone | State |
| --- | --- | --- |
| 0–2 | Core, RHI, reflection, assets, Content Browser | Done |
| 3 | Editor shell: viewport, gizmos, Outliner, Details, undo, Play-in-Editor | Done (smaller items open) |
| 4 | Gameplay: prefabs, Lua + visual scripting, input, Jolt physics, character | Exit test works |
| 5 | Content editors: materials + material graphs, animation + anim graphs, audio, particles | Exit test works |
| 6 | Vertical slice | Done: exit test works on this PC (package runs from another folder); every planned item in, some partial (BACKLOG [~]) |
| 7 | Gameplay languages: C / C++ game module, JavaScript scripts | C / C++ works (Native Script, Game.dll from Source/, compile + reload in the editor, docs/CODING.md); JavaScript, C++ nodes, module components planned |

Milestone 6 has: packaging + game mode, save games, sky / height fog / post-process volume, navmesh +
Nav Agents, behaviour trees + perception, game UI on RmlUi (HUD, menus, keyboard / gamepad navigation,
world-anchored elements, the UI window with a designer canvas, documents' own JavaScript on
QuickJS-ng), game user settings with key rebinding, localisation, crash reports, frustum culling, auto
LODs, asset validation, platform settings, Tracy / `stat unit`, smoke tests, Git source control in the
Content Browser, a remote console (`-remote`), video-memory scaling.
Since then (Nov 2026): multi-mesh files as parts + Create Prefab from Parts; morph targets (also
animated in takes); 2D blend spaces; mesh Normals (Smooth by default, Solid / Import / Auto Smooth);
Box UV; per-mesh LOD screen sizes, Max Draw Distance, Cull Distance Volumes; sound occlusion, mix
low-pass, Base Sound Mix; particle Over Life keys and Auto Destroy; BT decorators / services;
localisation {0} / plurals; lsd.save_objects; Blueprint panels in the graph window (My Blueprint,
variable Details, Palette); thumbnails for materials (sphere), sounds (waveform), BT / Lua tiles;
Editor Preferences > Plugins with Claude MCP (Claude Code drives the editor through `lsd_mcp`);
Unreal-style panel tabs under the menu bar (Level + each open asset editor), and under them, on the Level tab,
Blender-style workspace tabs (Layout = the original arrangement; `editor/workspaces.h`).
Post pass (draw_post, when a Post Process Volume applies): SSAO, TAA, depth of field, bloom, ACES,
grading, FXAA. Build > Build Lighting bakes light probes (`editor/light_bake.*`).
Later (see BACKLOG, partial items): lightmap textures, remote console to other machines, Perforce /
LFS locking, UI resize handles and data bindings, per-mesh LOD screen sizes.
