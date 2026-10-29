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
- Run: `build\windows-msvc\runtime\Debug\lsd_runtime.exe [-rhi=d3d12|vulkan|opengl] [-gpu=<n>]`
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
engine/core/        job system (worker threads, counters, wait-by-helping)
engine/reflect/     reflection tables (LSD_STRUCT/LSD_PROPERTY/LSD_GENERATED), JSON text save/load
engine/rhi/         RHI interface, adapter scoring/selection, quality presets, create_device();
                    backends/d3d12 (implemented), backends/vulkan, backends/opengl (to implement)
engine/world, script, render, audio, physics, ui   placeholders
editor/             editor app (placeholder)
tools/data_compiler/ CLI stub: --source-dir / --data-dir
tools/reflect/      lsd_reflect_gen: header -> .gen.cpp; CMake: lsd_reflect(<target> <headers>)
runtime/            game exe; hardware report, SDL3 window, clears + draws a triangle
third_party/        fetched dependencies (SDL3, pinned tag)
tests/              framework-free unit tests (CTest)
docs/               ROADMAP.md, CLEAN_ROOM.md
```

Conventions: namespace `lsd`, snake_case functions, `PascalCase` types, every system takes an
`Allocator&`, no STL in hot paths, `LSD_LOG_*` macros, warnings as clean as possible (`/W4`,
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`).

## Status

**Milestone 0 — done.** Builds clean on MSVC and Clang, tests pass, runtime prints the hardware
report on Charlize's machine.

**Milestone 1 — in progress.** Done: job system, SDL3 window, D3D12 backend (real adapter
enumeration, `query_memory_budget()`, clear + triangle), `-rhi=` / `-gpu=` / `-validation` /
`-frames=` flags; reflection generator + text save/load. Next: Dear ImGui debug Details panel
(milestone exit test), then Vulkan and OpenGL backends.

Reflection decisions: annotation macros `LSD_STRUCT` / `LSD_PROPERTY` / `LSD_FUNCTION` plus
`LSD_GENERATED()` in the body (like `GENERATED_BODY()`); reflected types are plain structs, no
inheritance (composition only); source assets save as JSON text, binary only after the data compiler.

## Roadmap (editor-first)

| # | Milestone | Exit test |
| --- | --- | --- |
| 1 | Core + reflection: job system, SDL3 window, first RHI backend (D3D12) drawing a triangle, then Vulkan and OpenGL; reflection code generator; serialization | A reflected object saves, loads and shows its properties in a debug panel |
| 2 | Assets + Content Browser: asset registry, importers, data compiler, thumbnails | Import a Blender FBX and find it by search |
| 3 | Editor shell: viewport with grid + gizmos, Outliner, Details, undo, Play-in-Editor | Block out a level and play it |
| 4 | Gameplay: prefabs, components, Lua + visual scripting, input assets, physics, character movement | A character runs; input wired with no code |
| 5 | Content editors: material graph, animation graph + blend spaces, audio (Sound Cue/Class/Mix/Attenuation/Submix), particle editor | The audio walkthrough in the plan doc works end to end |
| 6 | Vertical slice: lighting + post, UI designer, navmesh + behaviour trees, save game, packaging | A packaged level runs on another PC |
