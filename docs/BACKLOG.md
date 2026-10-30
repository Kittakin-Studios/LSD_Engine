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
- [~] Reflection: Vec3 (one-row X/Y/Z), type defaults (new components, later reset-to-default) done; needs enums (dropdowns), arrays, asset references, Quat
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

Exit test: block out a level and play it. **Works** (place shapes, move them with gizmos, save, Play, walk and jump on them);
the first-person player is temporary until Jolt + Character Movement in milestone 4.

### World and levels

- [x] World: entities as generational handles, one packed pool per reflected component type, transforms + parent/child hierarchy (`engine/world`)
- [~] Levels: new / open / save / save as (`.lsdlevel` JSON text in Content/Levels), last level reopens on start, unsaved-changes prompts; needs sub-levels, recent list
- [x] Autosave and crash recovery: asks to save on quit, autosaves unsaved levels to Saved/Autosaves (default every 5 min), offers to restore after a crash
- [x] Duplicate (Ctrl+D, Alt+drag with the gizmo), Cut / Copy / Paste (Ctrl+X/C/V) as level text, also between levels and to a text editor
- [x] Groups: Ctrl+G puts the selection under a new "Group" empty, Shift+G ungroups [Group Actors]
- [ ] Reference Viewer and dependency lists; "used by" before delete; fix broken references [Reference Viewer]

### Viewport

- [x] Grid, axes, axis gizmo
- [~] Move / rotate / scale gizmos (W / E / R, ImGuizmo), world / local space; needs pivot editing
- [~] Snapping: move, angle, scale steps; End drops to the surface below; needs vertex snap, surface snapping while dragging
- [~] Picking: click select (meshes and volumes by box, icons on screen), Ctrl+click toggles, Shift+click adds (ranges in the Outliner), drag a selection box, Ctrl+A, focus (F); gizmo moves the whole selection around the last-clicked object; needs triangle-accurate picking
- [x] Unreal camera: RMB look + WASD/QE fly, wheel speed, MMB pan, Alt+LMB orbit, wheel dolly; bookmarks Ctrl+0..9 / 0..9
- [~] Perspective + Top / Front / Right orthographic views (Alt+G/J/H/K), 1x1 or 2x2 layout (each viewport has its own camera and gizmo); needs a 1x2 layout, vertical grid in side views
- [~] View modes: Lit, Unlit, Wireframe (Alt+4/3/2) on D3D12 and OpenGL; needs normals, lighting only, collision, overdraw [View Modes, Buffer Visualization]
- [~] Show flags: grid, axis gizmo, Game View (G), stats; needs bounds, collision, nav [Show flags]
- [~] Icons for Player Start (capsule + facing arrow), empties and volumes (wire boxes); selection tint; needs light/camera/sound icons, selection outline
- [x] Stats overlay: fps, frame time, draw calls, triangles (`stat fps`) [stat fps / stat unit]

### Panels

- [~] Outliner: Outline tab (flat, search) and Hierarchy tab (tree, drag to attach, drop on empty space to detach), right-click Focus / Duplicate / Detach / Delete, hide (eye) and lock buttons saved in the level; needs folders, type filter
- [x] Details: entity name, one section per component, Add / remove component, mesh slot (shapes + mesh assets, drag-drop), search field, multi-edit (only changed values go to every selected object), right-click Reset to Default / Copy / Paste
- [~] Place panel (tab next to Content): Cube, Sphere, Plane, Player Start, Blocking Volume, Kill Volume, Trigger Volume, Empty, by drag or double-click; lights, cameras, sounds when they exist [Place Actors]
- [x] World Settings (Window menu; Kill Z, gravity; saved in the level) and Project Settings (Edit menu; name, company, game default level, editor startup level; `Config/project.json`)
- [x] Output Log tab with filters + console (`engine/core` console: variables and commands, Tab completion, history; `help` lists them; `-exec=` runs lines at startup) [Output Log, CVars]
- [~] Message Log (Window menu): warnings and errors only, unread count in the menu bar; needs click-to-asset [Message Log]
- [~] Editor preferences window (Edit menu): theme (`Config/editor_theme.json`) + per-user settings (`Saved/editor_settings.json`: snapping, camera speed, last level); needs keymap editor, autosave interval, saved layouts
- [~] Menu bar: File (New / Open / Save / Save As), Edit (Undo / Redo / Duplicate / Delete / Drop to Floor), Window (Undo History), Play / Stop; needs Simulate, build buttons, platform/quality dropdown

### Editing

- [x] Transactions: undo/redo (Ctrl+Z / Ctrl+Y) of every level edit, Undo History window with click-to-jump, unsaved marker in the title [Transaction system]
- [~] Play-in-Editor (Alt+P / Esc): play in the viewport from the Player Start, mouse capture (Shift+F1), Pause + frame step (]), Eject / Possess (F8: editor camera and selection while the game runs), changes revert on Stop; needs play in a new window, Simulate [PIE]
- [ ] Keep simulation changes: copy a property or object from PIE back to the editor world [Keep Simulation Changes]
- [~] Volumes: Blocking, Kill and Trigger (on_overlap_begin / end to scripts; point tests for now), post-process / audio / nav modifier come with those systems
- [~] Blockout: basic shapes scaled with the gizmo, collision on by default; needs box UVs, resize handles [BSP / Modeling Mode basics]
- [ ] Visual debugging: draw debug lines/shapes/text from scripts, persistent or for N seconds

## Milestone 4 — Gameplay layer

Exit test: a character runs, input wired with no code. **Works**: place a Character, pick IMC_Default and the
IA_Move / IA_Look / IA_Jump assets in its Details (Content Browser > New Input Setup makes them), press Play.

### Data

- [~] Data assets (`.lsdasset`, any registered reflected type, New Data Asset, edit + save in Details); data tables: a .csv in Content is the asset (Excel-friendly), grid editor window (add / delete rows and columns, rename), `lsd.table(name)` in Lua (numbers parsed, re-read on change), Get Table Value node, import by drop; curves: Curve data asset (smooth / linear / constant keys, auto tangents) with a graph editor in Details (drag, double-click to add, right-click to delete), `lsd.curve(name, t)`, Evaluate Curve node; needs typed table columns, JSON export, curve tables, tangent handles, vector / colour curves [Data Asset, Data Table, Curve]
- [x] Reflection: List<T, N> fields (JSON arrays, binary, +/remove in Details), Choice dropdowns, asset pickers (Asset:<Type>, drag-drop), key picker with Listen
- [~] Gameplay tags: Tags component (comma-separated, dotted hierarchy, `has_tag` / `lsd.has_tag` where "Damage.Fire" matches "Damage"); needs a project tag list with a picker, tag containers on data assets, richer queries (any / all / none) [Gameplay Tags]
- [ ] Enums and structs defined in the editor for scripts [User Defined Enum / Struct]

### Prefabs and components

- [~] Prefabs (`.lsdprefab`, `engine/world/prefab.h`): Outliner > Create Prefab, drag from the Content Browser to place, Apply to Prefab (updates every instance in the level), Revert, instances refresh when a level opens; per-instance overrides (properties edited in Details or moved with the gizmo inside an instance are kept when the prefab changes, shown gold with *, right-click > Revert to Prefab; up to 32 per instance); needs overrides for added / removed components and children, nested prefab updates [Blueprint instances, Level Instances]
- [ ] Prefab editor window with its own viewport and component tree [Blueprint Editor viewport]
- [ ] Components added from a list: mesh, light, camera, spring arm, audio, collision, movement, script, spline [Actor Components]
- [ ] Exposed properties per instance with ranges and tooltips [EditAnywhere]
- [~] Spawn / destroy at runtime: `lsd.spawn(name, x, y, z)` (prefab, mesh asset or built-in shape; gets physics and scripts), `lsd.destroy`; needs object pools

### Scripting

- [~] Lua scripting (LuaJIT, `engine/script`): Script component, on_start / on_update per object with its own `self`, `lsd` API (log, find, position / rotation, tags, input, time, destroy), hot reload on save while playing, errors with stack in the Output Log, New Lua Script + double-click to edit, `lua` console command; any component from Lua through reflection (`lsd.get` / `set` / `add` / `remove` / `has`), timers (`lsd.after` / `every` / `cancel`), overlap events from Trigger Volumes; `on_hit` from physics contacts, `lsd.spawn`, `lsd.raycast`; needs sandboxing
- [ ] Lua debugger in the editor: breakpoints, step, watch, call stack **(S)**
- [~] Visual scripting (`.lsdgraph`, `engine/script/graph.h`, editor/graph_editor): Blueprint-style canvas (nodes, exec + typed data wires, typed pin values, node menu with search, pan), compiled to Lua so it shares events, timers, hot reload and errors with Lua scripts; ~45 nodes (events, Branch, Sequence, Delay, Do Once, Set Timer, transform, spawn / destroy, colour, tags, input, variables, math); needs functions, macros, local variables of any type, reroute nodes, comments, zoom, copy / paste, undo inside the graph [Blueprints]
- [ ] Level script: a visual-script graph owned by the level for one-off events **(S)** [Level Blueprint]
- [ ] Visual-script debugging: breakpoints, live wire flow, watch values [Blueprint Debugger]
- [ ] Custom visual-script nodes declared from C++ (`LSD_FUNCTION`) and from Lua (today: the node table in engine/script/src/graph.cpp)
- [~] Game rules framework: Game Mode data asset picked in World Settings (player prefab spawned at the Player Start, game script on a GameMode object); needs Player Controller, Player State, Game Instance, subsystems
- [ ] Timers, event dispatchers, interfaces, latent actions (Delay)

### Input, physics, movement

- [~] Input Actions (Button / Axis 1D / Axis 2D) and Mapping Contexts (swizzle, scale, priority with key consumption); keyboard, mouse, gamepad with dead zones; New Input Setup template; triggers on the action (Down, Pressed, Released, Hold, Tap with a time) drive Jump, `lsd.pressed` and Input Pressed; needs chorded actions, more modifiers (dead zone per mapping, smoothing), player rebinding [Enhanced Input]
- [~] Physics (Jolt 5.6, `engine/physics`): static collision from meshes and Blocking Volumes, RigidBody component (box / sphere / capsule fitted to the mesh, mass, friction, bounciness), simulated in Play; needs convex / mesh shapes, channels + presets, physical materials, kinematic bodies in the editor
- [~] Constraints and joints: Physics Joint component (fixed, hinge, ball, slider; to another object by name or to the world; limits; velocity motor), pivot / axis marker in the viewport; needs a joint gizmo, breakable joints, springs, cone / twist limits (moved up from v2: doors and chains are blockout staples)
- [~] Traces: line trace (`PhysicsWorld::raycast`, `lsd.raycast`); needs sphere / box / capsule sweeps and overlaps, visual-script nodes
- [~] Character component: Jolt CharacterVirtual (walk, slopes, stairs, jump, fall), input asset wiring, first-person camera, possessed in Play (else a default walker at the Player Start); moving platforms (Spline Follower); needs crouch, swim [Character Movement Component]
- [~] Cameras: Character `camera_distance` spring arm (third person) pulled in by walls; needs camera shake, blends, camera components [Camera Manager]
- [~] Splines: Spline component (Catmull-Rom through up to 32 points, open or closed), drawn in the viewport, click a point to move it with the gizmo, Add / Remove Point; Spline Follower (Once / Loop / Ping-Pong, face forward; kinematic moving platform when solid); `lsd.spline_point` / `spline_length`, Spline Location / Length nodes; needs tangent handles, Alt-drag to add points, spline-placed meshes [Spline Component, Spline Mesh]

## Milestone 5 — Content editors

Exit test: the audio worked example works end to end. **Works** (the "Play Sound" animation notify too): import WAVs, Sound Cue graph, Sound Class tree, Sound Mix push / pop,
Attenuation, Concurrency, Submix effect chains, Sound Emitters by drag, Audio Volumes, trigger and
visual-script sounds, `stat audio`.

### Import

- [~] Audio import: .wav .ogg .flac .mp3 dropped into Content become Sound assets (settings in their .meta); needs waveform thumbnails
- [~] Texture import (stb_image): PNG, JPG, TGA, BMP, PSD as Texture assets, mips built on load (colour mips averaged in linear light), Max Size per texture, sRGB / linear chosen by the material slot, the image as its Content Browser thumbnail, hot reload on save; needs EXR / HDR, BC compression in the data compiler, streaming
- [ ] Multiple meshes and materials per imported file; scene import as a prefab; skeletal meshes, skeletons, animations
- [ ] Reimport on source change, import settings per asset, batch import
- [ ] Live link to Blender/Maya: re-export pushes into the running editor and game **(S)** [Live Link]

### Rendering content

- [ ] Shader pipeline: HLSL → DXIL / SPIR-V (DXC) / GLSL (SPIRV-Cross) as assets; replaces the hand-written test shaders; shader hot reload
- [~] Physically based shading (metal / rough, GGX, sun + sky ambient, sRGB output) on D3D12 and OpenGL with base colour, normal (derivative tangent frame) and ORM textures, emissive; Material data asset (New Material, Create Material from a texture, drag onto an object, StaticMesh material slot, live edits); needs masked / translucent, image-based lighting, Vulkan meshes (needs the Vulkan SDK)
- [~] Material instances (MaterialInstance data asset: parent + ticked overrides, follows parent edits live; Create Material Instance on a material); needs instances of material graphs (parameters), material functions (moved up from v2) [Material Instance, Material Function]
- [~] Material Graph (`.lsdmatgraph`, `engine/script/material_graph.h`): nodes on the graph canvas (Material Output: base colour, metallic, roughness, emissive, normal, AO; Constant, Color, Time, Texture Sample with Color / Normal / Data and a texture picker, Texture Coordinate tiling, Panner, World Position, Vertex Normal, Camera Vector, Fresnel, Add / Subtract / Multiply / Divide / Lerp / One Minus / Power / Clamp / Dot / Normalize / Sine / Abs / Frac, Break / Make Vector) compiled to one shader function in shared HLSL / GLSL syntax and built into the lit-mesh pipelines (`create_material_shader`, static + skinned) on D3D12 and OpenGL; used in any Material slot, recompiled when saved; errors name the node; live preview in the graph window (turning sphere / cube, recompiled 0.3 s after an edit, before saving); Scalar / Vector Parameter nodes (16 per material, a constant buffer b3) set by Material Instances whose parent is a graph (Parameters list by name, "Add the graph's parameters" button; one shared shader, no recompile); needs up to 3 textures for now, masked / translucent blend modes, Vulkan [Material Editor]
- [ ] Decals (moved up from v2: blockout and set dressing need them) [Decal Actor]
- [~] Lights: Directional (lux), Point and Spot (lumens, attenuation radius, cone angles), Sky Light (ambient), colour temperature in Kelvin, World Settings exposure (EV100), 16 per frame (nearest local lights), Place tab entries, direction / range markers; needs shadows, rect / disc / sphere area lights (LTC), IES, light functions, mobility
- [~] Shadows: cascaded sun shadows (4 cascades in a 4096 atlas, practical splits, stable spheres snapped to texels, 3x3 hardware PCF, slope bias + normal offset, pancaking; Cast Shadows / Shadow Distance on the Directional Light) on D3D12 and OpenGL; spot and point light shadows (a second 4096 atlas of 16 tiles: 1 per spot light, 6 cube faces per point light, nearest lights first, casters culled by range, Cast Shadows per light); needs quality per light, contact shadows, sun caster culling, shadow caching for static lights
- [ ] Reflections: capture probes, screen-space reflections
- [~] Sky, atmosphere, height fog: Sky component (zenith / horizon / ground gradient, sun disc from the first Directional Light; `draw_sky`), Height Fog component (exponential, height falloff integrated along the view ray, start distance, max opacity) on lit meshes, material graphs and the sky; needs sun angle driving the sky colour (scattering), fog on particles, an environment asset bundling sky / fog / post **(S)**
- [ ] Instanced meshes (many copies, one draw) [Instanced Static Mesh]

### Animation

- [~] Animation (ozz-animation, `engine/animation`): rigged FBX import (skin weights, skeleton, every take sampled at 30 fps, retargeted by bone name so animations can live in other files), GPU skinning on D3D12 / OpenGL incl. shadows, SkeletalMesh component (mesh, material, animation file, clip, rate, loop, plays in the editor), drag a rigged FBX in, Play Sound notifies on the animation's .meta (fire in Play); tested against ufbx's own skinning; needs glTF skins, clip preview / trim, root motion, curves, a clip list in Details
- [ ] Skeleton editor: sockets, bone preview, retarget base pose [Skeleton Editor]
- [~] Animation graph (`engine/animation/anim_graph.h`, AnimGraph data asset on the SkeletalMesh): states playing a clip or a 1D blend space (synced phase), transitions on parameters (Greater / Less / True / False / clip Finished) with crossfades, Character fills Speed / VerticalSpeed / InAir, Lua `lsd.set_anim_param` + "Set Anim Parameter" node, notifies follow the strongest clip; needs a node-graph editor (edited in Details for now), 2D blend spaces, montages and slots, sockets, a state readout while playing [Anim Blueprint]
- [ ] Simple IK: two-bone and look-at (moved up from v2; feet on slopes need it) [Two Bone IK]
- [ ] Morph targets / blend shapes [Morph Targets]

### Audio

- [~] Audio (miniaudio, `engine/audio`): Sound assets (volume, pitch, loop, stream, class, attenuation, concurrency, subtitle), preview in the Content Browser, Sound Class tree (volume groups), Sound Attenuation, Sound Concurrency (stop oldest / quietest / refuse), Sound Emitter component (drag a sound into the viewport), Trigger Volume enter sound, `lsd.play_sound` / `stop_sound`, Play Sound 2D / at Location nodes, `stat audio` overlay, `-nosound`; Sound Cues (`.lsdcue` node graph in the graph editor: Wave Player, Random no-repeat, Modulator, Mixer, Concatenator, Delay, Looping, Switch on an audio parameter, Output; Play preview), Sound Mixes (class volume adjusters with fades; `lsd.push_mix` / `pop_mix`, Push / Pop Sound Mix nodes), `lsd.set_audio_param` / Set Audio Parameter; Submixes (effect chains a Sound Class runs through: Reverb, Low Pass, High Pass, EQ Band, Echo, Compressor), `lsd.set_class_volume` / Set Sound Class Volume for settings sliders; needs a submix graph with live meters, low-pass in mixes, a project base mix, a settings menu (with the UI system)
- [~] Audio Volume component: reverb while the listener is inside (faded), sounds through its walls muffled by a per-voice low-pass; needs occlusion by trace, priority for overlapping volumes
- [ ] Music: crossfades and layers driven by game state

### Effects

- [~] Particle systems on CPU (`engine/particles`): ParticleSystem data asset with up to 8 emitters (rate, burst, duration / loop, cap, lifetime; Point / Sphere / Box shape; direction + cone spread, speed; gravity scale, wind, drag; start -> end colour, opacity and size; spin; local or world space; texture or a built-in soft dot; Alpha (sorted back to front) or Additive), ParticleEffect component (active, time scale, editor preview; Lua via `lsd.set`), drag a system into the viewport, Place tab entry, hot reload on save, `draw_particles` on D3D12 / OpenGL; needs over-life curves, collision, mesh + ribbon renderers, lit / soft particles, spawn by name from Lua [Niagara]
- [~] Particle Editor window (double-click a Particle System, or `particles.Open <name>`): its own preview viewport (orbit / zoom), play / pause / restart / loop / speed, timeline scrub (re-simulated from the start), per-emitter counts against their caps, sim time; emitters edited in Details with the preview following unsaved edits; needs a node / stack view of modules, bounds display, per-platform scalability

### Timelines

- [ ] Simple timeline / cutscene editor: tracks for transforms, properties, events, audio, camera cuts **(S)** [Sequencer basics]

## Milestone 6 — Vertical slice

Exit test: a packaged level runs on another PC. **Works on this PC** (Build/<Project>/<Project>.exe started from another folder opens the Game Default Level in Play, full window; Release build + app-local VC++ runtime, every other DLL is part of Windows 10/11); still to try on a second PC.

### Rendering

- [~] Post-process volume: Post Process Volume component (unbound or a box, priority): exposure compensation, ACES filmic tonemapper, saturation, contrast, tint, vignette, applied in the forward shaders; needs an HDR scene target for bloom, LUT grading, DOF, motion blur, AO, anti-aliasing (TAA / FXAA), blend weights between volumes [Post Process Volume]
- [ ] Baked lightmaps + probes; light build button with progress; lightmap density view; volumetric fog [Lightmass]
- [ ] LODs (auto-generated) and frustum/occlusion culling; distance culling volumes
- [ ] Scalability: Low/Medium/High/Epic per feature, per-device profiles [Scalability, Device Profiles]
- [ ] VRAM-budget scaling: texture streaming, shadow and render-target sizes follow `query_memory_budget()`
- [ ] Render to screenshot at high resolution [HighResShot]

### UI

- [~] UI designer (RmlUi + QuickJS): `engine/ui` on RmlUi 6 + FreeType drawn by `draw_ui` (D3D12 / OpenGL); `.rml` UI documents and `.rcss` styles in Content (New UI Document with a starter panel), the UI window with the Code tab and a live 1280x720 preview; fonts from Content/Fonts plus the system font as `sans`; in Play `lsd.ui_open` / `ui_close` / `ui_set_text` / `ui_set` / `ui_value` / `ui_show_cursor`, Open UI / Close UI / Set UI Text / Show Mouse Cursor nodes, the Game Mode's HUD (and Show Cursor for menus), on<event>="fn" calls fn(self, element_id) in Lua; needs the drag-and-drop canvas in two-way sync with the code, QuickJS scripting, data bindings, anchors / safe zones, Vulkan [UMG]
- [ ] Gamepad / keyboard focus navigation (moved up from v2: needed for a menu on a controller) [Common UI]
- [ ] HUD helpers: health bars, prompts, damage numbers in world space anchored to entities

### AI

- [~] Navmesh (Recast & Detour, `engine/ai`): built from the collision boxes (meshes with collision, Blocking Volumes) inside Nav Mesh Bounds (or the whole level) with World Settings > Navigation (agent radius / height, step, slope, cell size); P / Show > Navigation draws it (editor, ejected); Nav Agent component walks with crowd avoidance in Play; `lsd.move_to` / `stop_move` / `is_moving` / `find_path`, AI Move To / AI Stop Moving / Is Moving nodes; needs nav links for jumps, tiled / runtime rebuilds, triangle-exact geometry, the overlay depth-tested and in Play
- [~] Behaviour trees + blackboard with live debugging: `.lsdbt` on the graph canvas (Root, reactive Selector, Sequence, Move To, Wait, Random Location, Set / Clear Key, Run Script, Is Set, Compare), interpreted by `ai::BtRunner`; AI Controller component runs one per Nav Agent; the open tree glows along the active branch in Play; `lsd.bb_set` / `bb_get` / `bb_clear`, Set Blackboard Value node; needs decorators / services as in Unreal, a blackboard asset with typed keys, hot reload while playing, subtrees
- [~] Perception: sight (range, field of view, line of sight by physics ray: CanSeePlayer, PlayerLocation) and hearing (`lsd.report_noise` / Report Noise in range: NoiseLocation, HeardNoise) on the AI Controller; needs seeing other agents / teams, memory and forgetting, debug cones in the viewport [AI Perception]

### Game services

- [~] Save game: `lsd.save_game(slot, value)` / `load_game` / `has_save` / `delete_save` (numbers, strings, booleans, lists and keyed tables as versioned JSON in Saved/SaveGames/, written whole then swapped in), Save Value / Load Value nodes; needs save-whole-object helpers (chosen component fields), version upgrades, async saves, cloud / platform storage
- [ ] Settings menu backend: graphics, audio, key rebinding (moved up from v2) [Game User Settings]
- [ ] Localisation: string tables, gather text, language switch (moved up from v2: cheaper before content grows) [Localization Dashboard]

### Ship

- [~] Package project: File > Package Project (or `package`) writes Build/<Project Name>/ with the Release exe, Content/, Config/, the VC++ runtime and a game.json marker; the exe then runs as the game (also `-game`): Game Default Level in Play, no editor, Esc quits; needs cooking (compiled assets only, editor-only data stripped), development / shipping configs, resource packages per level, building Release from the editor **(S)** [Package Project, Cook]
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
