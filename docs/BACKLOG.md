# Backlog

Every feature in the plan doc's Feature spec, placed in the milestone that builds it.
`[x]` done · `[~]` partly done (note says what's missing) · `[ ]` to do.
Source: [engine plan doc](https://claude.ai/artifact/Q73b6jeNowQ4rQJGqRYwxW), Feature spec tab. Unreal names in brackets.

## Milestone 1 — Core + reflection (done)

- [x] Job system sized to logical cores − 1
- [x] SDL3 window and event loop
- [x] RHI interface + adapter scoring, `-gpu=`, `-rhi=`, quality preset recommendation
- [x] Direct3D 12 backend: adapters, live VRAM budget, debug layer → log
- [~] Vulkan 1.3 backend: runs; triangle needs the Vulkan SDK's DXC; validation layers need the SDK
- [x] OpenGL 4.6 backend
- [x] Reflection code generator (`LSD_STRUCT` / `LSD_PROPERTY`) [UHT]
- [x] Text serialization with versioning (missing keys keep defaults, unknown keys skipped)
- [x] Details panel generated from reflection, colour pickers
- [ ] Linux / Clang build verified (`linux-clang` preset never run since milestone 0)

## Milestone 2 — Assets + Content Browser

Exit test: import a Blender FBX and find it by search.

- [~] Renderer basics: buffers, depth, lit mesh + line pipelines, camera math, built-in cube/sphere/plane, GPU grid done on D3D12 and OpenGL; Vulkan waits for the SDK; next: imported meshes, textures, thumbnails
- [ ] Asset registry: GUID, type, path, tags, dependency list, background indexing [Asset Registry]
- [ ] Safe rename / move with reference fix-up; Reference Viewer
- [ ] Binary serialization for compiled data [cooked data]
- [ ] Data compiler: source text → binary blobs, incremental, hot reload of data
- [ ] Importers: FBX (ufbx), glTF (cgltf), mesh optimisation (meshoptimizer), textures, WAV/OGG audio
- [ ] Shader pipeline: HLSL → DXIL / SPIR-V (DXC) / GLSL (SPIRV-Cross) as assets; replaces the 3 hand-written test shaders
- [ ] Thumbnails
- [~] Content Browser: themed shell and real `Content/` folder tree done; needs asset grid, search, filters, drag-drop import
- [ ] Data assets, Data tables (CSV import/export), Curves [Data Asset, Data Table, Curve]

## Milestone 3 — Editor shell

Exit test: block out a level and play it.

- [ ] World: entities as handles, components in SoA managers, transforms + hierarchy
- [ ] Levels: create / open / save; persistent level + sub-levels
- [~] Viewport: grid, axes, gizmo, orbit done; needs ortho views, move/rotate/scale gizmos, grid/angle/surface snapping, camera bookmarks, view modes (lit, unlit, wireframe, lighting only, collision)
- [~] Outliner: themed, lists the scene's objects, click selects; needs World entities, folders, search, hide/lock, parent by drag
- [~] Details: reflection-driven; needs multi-edit, reset to default, copy/paste, asset slots with search + drag-drop
- [ ] Place panel: lights, cameras, volumes, sound emitters, player start
- [ ] Transactions: undo/redo across all editors, Undo History [Transaction system]
- [ ] Play-in-Editor: play in viewport / window, possess/eject, pause, step, simulate; changes revert on stop [PIE]
- [ ] World Settings / Project Settings windows
- [ ] Output Log + console commands/variables (`r.Shadows 0`)
- [ ] Volumes: trigger, kill, blocking, post-process, audio, nav modifier
- [~] Editor preferences: theme done (`Config/editor_theme.json`); needs keymap, autosave, layout presets
- [ ] Visual debugging: draw debug lines/shapes/text from scripts; collision and nav view modes

## Milestone 4 — Gameplay layer

Exit test: a character runs, input wired with no code.

- [ ] Prefabs with per-instance overrides (bold) and per-property revert [Blueprint instances]
- [ ] Components added from a list: mesh, light, audio, collision, movement, script [Actor Components]
- [ ] Exposed properties per instance with ranges and tooltips [EditAnywhere]
- [ ] Lua scripting (LuaJIT) with hot reload, bindings generated from reflection
- [ ] Visual scripting: events, variables, functions, flow control, calls into C++/Lua [Blueprints]
- [ ] Game rules framework: Game Mode, Player Controller, Pawn/Character, Player State, Game Instance
- [ ] Timers, event dispatchers, interfaces
- [ ] Input actions (triggers, modifiers) and mapping contexts by priority [Enhanced Input]
- [ ] Physics (Jolt): collision shapes (auto on import), channels + presets, rigid bodies, physical materials
- [ ] Traces and overlaps, also as visual-script nodes
- [ ] Character movement: walk, slopes, steps, jump, fall, swim [Character Movement Component]

## Milestone 5 — Content editors

Exit test: the audio worked example works end to end.

- [ ] Material graph with live preview; material instances [Material Editor, Material Instance]
- [ ] Lights: directional, point, spot, rect, disc/sphere, sky, emissive; physical units; IES; light functions; mobility
- [ ] Shadows: cascaded for the sun, shadow maps for local lights, quality per light
- [ ] Reflections: capture probes, screen-space reflections
- [ ] Sky, atmosphere, height fog; sun angle drives sky colour
- [ ] Animation sequences: preview, trim, root motion, notifies (ozz-animation)
- [ ] Animation graph (state machine + blends), blend spaces 1D/2D, montages and slots, sockets [Anim Blueprint]
- [ ] Audio (miniaudio): Sound Wave, Sound Cue, Sound Class, Sound Mix, Attenuation, Submix, emitters
- [ ] Particle systems on CPU: emitter stack, forces, over-life curves, collision [Niagara]

## Milestone 6 — Vertical slice

Exit test: a packaged level runs on another PC.

- [ ] Post-process volume: exposure, bloom, grading, LUT, vignette, DOF, motion blur, AO
- [ ] Baked lightmaps + probes; volumetric fog
- [ ] LODs (auto-generated) and frustum/occlusion culling
- [ ] VRAM-budget scaling: texture streaming, shadow and render-target sizes follow `query_memory_budget()`
- [ ] UI designer (RmlUi + QuickJS): canvas, anchors, Code tab with HTML/CSS/JS in two-way sync, rich text with inline HTML/CSS, bindings, events, styles and fonts [UMG]
- [ ] Navmesh (Recast) with live preview, AI Move To + avoidance (Detour)
- [ ] Behaviour trees + blackboard with live debugging
- [ ] Save game: slots, chosen fields, versioning
- [ ] Package project: one button, development/shipping, editor-only data stripped
- [ ] Platform settings: icons, splash, exe name, Steam app ID
- [ ] Profilers: Tracy, stat overlays (fps, draw calls, voices), memory per system
- [ ] Source control from the Content Browser (Git LFS, Perforce)

## v2 (after the vertical slice)

- World: level streaming, terrain (sculpt, paint layers, heightmaps), foliage + procedural scatter, level instances
- Rendering: material functions, global material parameters, decals, HLOD, GPU particles, dynamic GI
- Animation: IK (foot placement, look-at, two-bone, full-body), retargeting, morph targets
- Physics: ragdolls and physics assets, constraints
- Gameplay: gameplay tags
- Input: player key rebinding
- UI: widget animations, world-space UI, gamepad navigation
- AI: perception (sight, hearing)
- Cinematics: Sequencer, cinematic camera
- Data: localisation, editor utilities
- Build: crash reporting, plugins, C++ hot reload
- Compute: optional CUDA / ROCm (HIP) plugins for editor and offline tools

## Later

World Partition + Data Layers, Nanite-style virtualised geometry, in-engine rigging (Control Rig),
destruction / cloth / vehicles, ability system, networking (replication, RPCs, dedicated server),
EQS, State Trees, render to video.
