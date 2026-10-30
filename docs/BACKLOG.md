# Backlog

Every feature planned for LSD_Engine, placed in the milestone that builds it.
`[x]` done · `[~]` partly done (note says what's missing) · `[ ]` to do.
Sources: the [engine plan doc](https://claude.ai/artifact/Q73b6jeNowQ4rQJGqRYwxW) Feature spec, plus the public
feature sets of Unreal Engine 5 and Autodesk Stingray (ideas only, see `docs/CLEAN_ROOM.md`; our own names and formats).
Unreal names in brackets. **(S)** = idea taken from Stingray's public feature list.

Rule for placing things: a feature goes in the earliest milestone whose exit test needs it, or that
everything later depends on. Anything a small team does not need to ship a first game goes to v2 or Later.

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
- [ ] Reflection: enums (dropdowns), fixed arrays, dynamic arrays, asset references, Vec3/Quat/Transform types
- [ ] Memory tracking per allocator shown in a debug panel [memreport]

## Milestone 2 — Assets + Content Browser (done)

Exit test: import a Blender FBX and find it by search. **Passes** (automated in `tests/asset_tests.cpp` and in the editor).

- [~] Renderer basics: buffers, depth, lit mesh + line pipelines, camera math, built-in cube/sphere/plane, GPU grid done on D3D12 and OpenGL; Vulkan waits for the SDK
- [~] Asset registry: GUIDs in `.meta` sidecars, type, path, folder, search, duplicate-GUID repair done; needs tags, dependency list, background indexing [Asset Registry]
- [x] Safe rename / move / delete in the editor (the .meta travels, GUIDs survive); folders: new, rename, delete
- [x] Binary serialization for reflected objects (name-hash keyed, versioned) and compiled meshes [cooked data]
- [x] Data compiler: Content → DerivedData, incremental (source size/time + import settings), editor hot reload of meshes
- [x] Mesh importers: FBX (ufbx), glTF (cgltf), meshoptimizer (merged mesh, Z-up metres)
- [x] Thumbnails (render-to-texture, re-rendered when the source changes)
- [x] Content Browser: folder tree and tiles, thumbnails, search, type filter, import by drop, drag to place, drag onto folders to move, right-click menus
- [ ] Thumbnail cache on disk (today they re-render every editor start)
- [ ] Content Browser extras: list view, sort, tile size slider, multi-select, favourites, collections, Duplicate, Reimport, "Find in Content Browser" [Collections]
- [ ] Asset editor windows: double-click opens a tab per asset (mesh viewer with stats, import settings, preview) [Static Mesh Editor]

## Milestone 3 — Editor shell

Exit test: block out a level and play it.

### World and levels

- [ ] World: entities as handles, components in SoA managers, transforms + parent/child hierarchy
- [ ] Levels: new / open / save / save as; persistent level + sub-levels; recent levels; startup level
- [ ] Autosave and crash recovery ("restore unsaved changes?")
- [ ] Copy / paste / duplicate entities (Ctrl+D, Alt-drag), also between levels as text
- [ ] Groups (select and move together) [Group Actors]
- [ ] Reference Viewer and dependency lists; "used by" before delete; fix broken references [Reference Viewer]

### Viewport

- [~] Grid, axes, axis gizmo, orbit done
- [ ] Move / rotate / scale gizmos, world / local space, pivot editing
- [ ] Snapping: grid, angle, scale, surface (drop to floor with End), vertex snap
- [ ] Picking: click select, Ctrl/Shift multi-select, box select, focus selection (F)
- [ ] Fly camera (right-mouse + WASD) with speed setting; camera bookmarks (Ctrl+1..9)
- [ ] Perspective + top/front/side ortho views; 1 / 2 / 4 viewport layouts
- [ ] View modes: lit, unlit, wireframe, normals, lighting only, collision, overdraw [View Modes, Buffer Visualization]
- [ ] Show flags (grid, gizmos, icons, bounds, collision, nav) and Game View (G) [Show flags]
- [ ] Billboard icons for lights, cameras, sounds, volumes; selection outline
- [ ] Stats overlay: fps, frame time, draw calls, triangles [stat fps / stat unit]

### Panels

- [~] Outliner: themed, lists objects, click selects; needs World entities, folders, search, hide/lock, parent by drag, type filter
- [~] Details: reflection-driven; needs multi-edit, reset to default, copy/paste values, asset slots with search + drag-drop, search field
- [ ] Place panel: shapes, lights, cameras, volumes, sound emitters, player start [Place Actors]
- [ ] World Settings and Project Settings windows (reflected config files)
- [ ] Output Log with filters + console with commands and variables (`r.Shadows 0`) [Output Log, CVars]
- [ ] Message Log for import / save / validation errors with click-to-asset [Message Log]
- [~] Editor preferences: theme done (`Config/editor_theme.json`); needs keymap editor, autosave interval, saved layouts
- [ ] Toolbar: Save, Play, Simulate, build buttons, platform/quality dropdown

### Editing

- [ ] Transactions: undo/redo across every editor, Undo History panel [Transaction system]
- [ ] Play-in-Editor: play in viewport / new window, possess/eject (F8), pause, frame step, simulate; changes revert on stop [PIE]
- [ ] Keep simulation changes: copy a property or object from PIE back to the editor world [Keep Simulation Changes]
- [ ] Volumes: trigger, kill, blocking, post-process, audio, nav modifier
- [ ] Brushes / blockout shapes with box UV, quick resize handles [BSP / Modeling Mode basics]
- [ ] Visual debugging: draw debug lines/shapes/text from scripts, persistent or for N seconds

## Milestone 4 — Gameplay layer

Exit test: a character runs, input wired with no code.

### Data

- [ ] Data assets, data tables (CSV/JSON import/export), curves and curve tables — instances of game-defined reflected types [Data Asset, Data Table, Curve]
- [ ] Gameplay tags: hierarchical names, tag containers, tag queries (moved up from v2: scripts and AI need them early) [Gameplay Tags]
- [ ] Enums and structs defined in the editor for scripts [User Defined Enum / Struct]

### Prefabs and components

- [ ] Prefabs with per-instance overrides (bold) and per-property revert; nested prefabs [Blueprint instances, Level Instances]
- [ ] Prefab editor window with its own viewport and component tree [Blueprint Editor viewport]
- [ ] Components added from a list: mesh, light, camera, spring arm, audio, collision, movement, script, spline [Actor Components]
- [ ] Exposed properties per instance with ranges and tooltips [EditAnywhere]
- [ ] Spawn / destroy at runtime, object pools

### Scripting

- [ ] Lua scripting (LuaJIT) with hot reload, bindings generated from reflection
- [ ] Lua debugger in the editor: breakpoints, step, watch, call stack **(S)**
- [ ] Visual scripting: events, variables, functions, macros, flow control, calls into C++/Lua [Blueprints]
- [ ] Level script: a visual-script graph owned by the level for one-off events **(S)** [Level Blueprint]
- [ ] Visual-script debugging: breakpoints, live wire flow, watch values [Blueprint Debugger]
- [ ] Custom visual-script nodes declared from C++ (`LSD_FUNCTION`) and from Lua
- [ ] Game rules framework: Game Mode, Player Controller, Pawn/Character, Player State, Game Instance, subsystems
- [ ] Timers, event dispatchers, interfaces, latent actions (Delay)

### Input, physics, movement

- [ ] Input actions (triggers, modifiers) and mapping contexts by priority; keyboard, mouse, gamepad [Enhanced Input]
- [ ] Physics (Jolt): collision shapes (auto on import, simple + convex), channels + presets, rigid bodies, physical materials, kinematic bodies
- [ ] Constraints and joints (hinge, ball, slider) (moved up from v2: doors and chains are blockout staples)
- [ ] Traces and overlaps (line, sphere, box, capsule), also as visual-script nodes
- [ ] Character movement: walk, slopes, steps, crouch, jump, fall, swim, moving platforms [Character Movement Component]
- [ ] Cameras: spring arm with collision, camera shake, blend between cameras [Camera Manager]
- [ ] Splines: editable curves in the viewport, follow-spline movement, spline-placed meshes [Spline Component, Spline Mesh]

## Milestone 5 — Content editors

Exit test: the audio worked example works end to end.

### Import

- [ ] Texture import (PNG, TGA, EXR, HDR; BC compression, mips, sRGB, normal maps) and audio import (WAV, OGG, FLAC)
- [ ] Multiple meshes and materials per imported file; scene import as a prefab; skeletal meshes, skeletons, animations
- [ ] Reimport on source change, import settings per asset, batch import
- [ ] Live link to Blender/Maya: re-export pushes into the running editor and game **(S)** [Live Link]

### Rendering content

- [ ] Shader pipeline: HLSL → DXIL / SPIR-V (DXC) / GLSL (SPIRV-Cross) as assets; replaces the hand-written test shaders; shader hot reload
- [ ] Physically based shading (metal/rough), opaque, masked, translucent, two-sided
- [ ] Material graph with live preview; material instances; material functions (moved up from v2) [Material Editor, Material Instance, Material Function]
- [ ] Decals (moved up from v2: blockout and set dressing need them) [Decal Actor]
- [ ] Lights: directional, point, spot, rect, disc/sphere, sky, emissive; physical units; IES; light functions; mobility
- [ ] Shadows: cascaded for the sun, shadow maps for local lights, quality per light, contact shadows
- [ ] Reflections: capture probes, screen-space reflections
- [ ] Sky, atmosphere, height fog; sun angle drives sky colour; environment asset that bundles sky, fog and post settings **(S)**
- [ ] Instanced meshes (many copies, one draw) [Instanced Static Mesh]

### Animation

- [ ] Animation sequences: preview, trim, root motion, notifies, curves (ozz-animation)
- [ ] Skeleton editor: sockets, bone preview, retarget base pose [Skeleton Editor]
- [ ] Animation graph (state machine + blends), blend spaces 1D/2D, montages and slots, sockets [Anim Blueprint]
- [ ] Simple IK: two-bone and look-at (moved up from v2; feet on slopes need it) [Two Bone IK]
- [ ] Morph targets / blend shapes [Morph Targets]

### Audio

- [ ] Audio (miniaudio): Sound Wave, Sound Cue, Sound Class, Sound Mix, Attenuation, Submix, emitters, concurrency limits
- [ ] Reverb zones from audio volumes; occlusion by trace
- [ ] Music: crossfades and layers driven by game state

### Effects

- [ ] Particle systems on CPU: emitter stack, spawn, forces, over-life curves, collision, sprite + mesh + ribbon renderers [Niagara]
- [ ] Particle editor with timeline preview and per-system stats

### Timelines

- [ ] Simple timeline / cutscene editor: tracks for transforms, properties, events, audio, camera cuts **(S)** [Sequencer basics]

## Milestone 6 — Vertical slice

Exit test: a packaged level runs on another PC.

### Rendering

- [ ] Post-process volume: exposure, bloom, grading, LUT, vignette, DOF, motion blur, AO, anti-aliasing (TAA / FXAA) [Post Process Volume]
- [ ] Baked lightmaps + probes; light build button with progress; lightmap density view; volumetric fog [Lightmass]
- [ ] LODs (auto-generated) and frustum/occlusion culling; distance culling volumes
- [ ] Scalability: Low/Medium/High/Epic per feature, per-device profiles [Scalability, Device Profiles]
- [ ] VRAM-budget scaling: texture streaming, shadow and render-target sizes follow `query_memory_budget()`
- [ ] Render to screenshot at high resolution [HighResShot]

### UI

- [ ] UI designer (RmlUi + QuickJS): canvas, anchors, Code tab with HTML/CSS/JS in two-way sync, rich text with inline HTML/CSS, bindings, events, styles and fonts [UMG]
- [ ] Gamepad / keyboard focus navigation (moved up from v2: needed for a menu on a controller) [Common UI]
- [ ] HUD helpers: health bars, prompts, damage numbers in world space anchored to entities

### AI

- [ ] Navmesh (Recast) with live preview, AI Move To + avoidance (Detour), nav links for jumps
- [ ] Behaviour trees + blackboard with live debugging
- [ ] Perception: sight and hearing (moved up from v2: needed for any enemy) [AI Perception]

### Game services

- [ ] Save game: slots, chosen fields, versioning
- [ ] Settings menu backend: graphics, audio, key rebinding (moved up from v2) [Game User Settings]
- [ ] Localisation: string tables, gather text, language switch (moved up from v2: cheaper before content grows) [Localization Dashboard]

### Ship

- [ ] Package project: one button, development/shipping, editor-only data stripped, resource packages per level **(S)** [Package Project, Cook]
- [ ] Platform settings: icons, splash, exe name, Steam app ID
- [ ] Connect the editor to a running game (this PC or another): remote console, push hot reloads, live tweak values **(S)**
- [ ] Profilers: Tracy, stat overlays (fps, draw calls, voices), memory per system, GPU timings [Unreal Insights]
- [ ] Asset validation before packaging: missing references, oversize textures, naming rules [Data Validation]
- [ ] Crash reporting: minidump + log saved, shown on next start (moved up from v2)
- [ ] Automated tests run from the editor: unit tests + "play this level for N seconds" smoke tests [Automation]
- [ ] Source control from the Content Browser (Git LFS, Perforce): check out, diff, history

## v2 (after the vertical slice)

- World: level streaming volumes, terrain (sculpt, paint layers, heightmaps, holes), foliage painting + procedural scatter rules, water bodies (lake, river, ocean) [Landscape, Foliage, PCG, Water]
- Modeling: in-editor mesh editing (extrude, bevel, boolean), UV tools [Modeling Mode]
- Rendering: global material parameters, HLOD, GPU particles, dynamic GI, virtual texturing, hair / fur cards
- Animation: full-body IK, foot placement, retargeting between skeletons, animation layers, motion matching [IK Rig, IK Retargeter]
- Physics: ragdolls and physics assets, cloth, vehicles [Physics Asset, Chaos Cloth, Chaos Vehicles]
- Gameplay: ability system (abilities, effects, attributes, cues) [Gameplay Ability System]
- AI: State Trees, smart objects, environment queries [State Tree, Smart Objects, EQS]
- Input: touch and gyro, input recording/playback
- UI: widget animations, world-space UI panels, accessibility (text scale, colour-blind filters, subtitles)
- Cinematics: full Sequencer (keys, curves, shots, sub-sequences), cinematic camera (film back, focus), render to video [Sequencer, Movie Render Queue]
- Audio: procedural audio graphs, spatial audio, dynamic music system [MetaSounds, Quartz]
- Data: editor utility scripts and widgets (Lua + visual script running in the editor) [Editor Utility Widgets]
- Assets: size map and asset audit, redirectors fix-up, bulk property editing [Size Map, Asset Audit, Property Matrix]
- Build: plugins, C++ hot reload, DLC / patch packages [Live Coding, Chunking]
- Collaboration: multi-user editing in one level [Multi-User Editing]
- Compute: optional CUDA / ROCm (HIP) plugins for editor and offline tools
- VR / XR via OpenXR **(S)**

## Later

World Partition + Data Layers, Nanite-style virtualised geometry, Lumen-style software ray-traced GI, hardware
ray tracing, in-engine rigging (Control Rig), destruction (fracture + debris), massive crowds (ECS-style mass
simulation), networking (replication, RPCs, dedicated server, online subsystem: lobbies, achievements),
USD / Alembic / Datasmith import, virtual production (camera tracking, take recorder), consoles and mobile platforms.
