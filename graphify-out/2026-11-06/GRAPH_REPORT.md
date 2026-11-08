# Graph Report - LSD_Engine  (2026-11-06)

## Corpus Check
- 250 files · ~5,891,896 words
- Verdict: corpus is large enough that graph structure adds value.
- Unclassified: 185 file(s) not represented in the graph (top: .meta 119, .lsdasset 35, .fbx 14)

## Summary
- 8075 nodes · 16748 edges · 326 communities (304 shown, 22 thin omitted)
- Extraction: 90% EXTRACTED · 10% INFERRED · 0% AMBIGUOUS · INFERRED: 1691 edges (avg confidence: 0.85)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `afaf6aaa`
- Run `git rev-parse HEAD` and compare to check if the graph is stale.
- Run `graphify update .` after code changes (no API cost).

## Community Hubs (Navigation)
- Play Session (Play mode)
- Editor State & Panels
- Lua Script API
- D3D12 Backend
- Editor UI Panels
- JavaScript Host (QuickJS)
- AssetRegistry
- OpenGL Backend
- CMake Build Targets
- asset_registry.h
- lsd_game.h
- World
- Vulkan Backend
- Math Library
- Visual Script Graphs
- cstring
- Timeline
- Script System
- Content Browser Assets
- Lua Bindings
- World Entities
- UI Document Editor
- Jolt Physics
- Data Tables
- Native C/C++ API
- src/navigation.cpp
- assistant_core.cpp
- Level Modes
- Job System & Foundation Tests
- Graph Editor Window
- Native Script Runtime
- Reflection Generator Parser
- RHI Device Interface
- Script Tests
- Multiplayer NetSession
- ClipSet
- Animation (ozz)
- Details Rows
- Audio (miniaudio)
- Editor Screenshots
- Anim Graph: GraphState
- Audio: AudioSystem
- Main: main()
- Audio: AudioSystem::Impl
- Game: Vec3
- Particles: ParticleEmitter
- Rhi: UiDraw
- Lighting: lighting.cpp
- Net: Reader
- Physics: PhysicsWorld::Impl
- Physics: PhysicsWorld
- Reflect: Property
- Reflect: reflect.cpp
- Components: f32
- Mover
- D3d12 Device: u32
- Editor Settings: ProjectSettings
- string
- Net: NetSession::Impl
- Mesh Import: mesh_import.cpp
- Animation: Pose
- Editor Theme: EditorTheme
- Rhi: MeshDraw
- Game: Node
- ui.cpp
- Editor Screenshots: Details panel (reflection-generated, gol
- components.h
- Audio: PlayDesc
- Rhi: PostDraw
- Main: draw_world()
- Particle Editor: ParticleEditor
- D3d12 Device: check()
- Allocator: ArenaAllocator
- Physics: BodyDesc
- Transactions: Transactions
- Animation: SkinnedModel
- Thumbnails: ThumbnailCache
- texture.cpp
- Vulkan Device: vulkan_device.cpp
- Rhi: AdapterInfo
- Native: NativeModule
- System Info: system_info.cpp
- graph_editor.cpp
- physics.h
- Rhi: DeviceDesc
- Opengl Device: MeshProgram
- Ui: UiContext
- AnimationPlayer
- Mesh Cache: CachedMesh
- Sound Assets: SubmixEffect
- Voice Io: VoiceIO::Impl
- Console: console.cpp
- Rhi: LightSetup
- Components: PostProcessVolume
- Game Module: game_module.cpp
- Editor Settings: EditorSettings
- Mesh Cache: mesh_cache.cpp
- Input: input.cpp
- Reflect Test Types: PointLight
- remote_console.cpp
- cstdio
- Ui: RenderInterface
- Animation Player: State
- Script: script.h
- Material Cache: Entry
- Details Panel: details_panel.cpp
- D3d12 Device: .lookup()
- MotionData
- Animation: Pose::Impl
- allocator.cpp
- Lua Debugger: lua_debugger.cpp
- Mesh Editor: MeshWindow
- Paper2d: paper2d.cpp
- JSContext
- Reflect: reflect.h
- Audio: Voice
- .wait_for_gpu
- User Types: user_types.h
- Net: net.cpp
- Particle Player: ParticlePlayer
- Net: NetSettings
- draw_entity_details
- Material: MaterialInstance
- Curve: curve.h
- assistant.cpp
- Sound Player: sound_player.cpp
- Navigation: NavMesh
- Asset Meta: AssetMeta
- Script Tests: Properties
- Mesh: MeshData
- Opengl Device: TextureHandle
- Material Graph: Compiler
- Components: Character
- Editor Screenshots: Play-in-Editor full editor screenshot
- Play Session: Moving
- material_import.cpp
- Behavior Tree: behavior_tree.cpp
- level.cpp
- Json: load_level()
- Components: TileMap
- Components: Landscape
- Components: u32
- string
- Importer
- Crash: crash.cpp
- Debug Draw: Text
- Sound Player: SoundPlayer
- Prefab Editor: PrefabWindow
- Behavior Tree: BtRunner
- Log: log.cpp
- Reflect: TypeInfo
- Input: InputSystem
- Components: SkeletalMesh
- Material Cache: TextureCache
- References: references.cpp
- Behavior Tree: BtType
- Net: NetEvent
- Graph: PinType
- Material Graph: MaterialCode
- mesh_materials.cpp
- Montage
- Source Control: source_control.cpp
- Input: Action
- ParticleInstance
- Particles: particles.cpp
- D3d12 Device: PostChain
- Opengl Device: PostChain
- Architecture Docs: Game.dll game module (hot reload)
- Manual Docs: Command line flags
- Decals: DecalAtlas
- Animation: import_clips()
- World Tests: world_tests.cpp
- TimelineWindow
- Claude Docs: CLAUDE.md LSD_Engine briefing
- Architecture Docs: EXAMPLES: worked tutorials
- Editor Ui: EditorCamera
- Light Bake: Bvh
- MeshPart
- Mesh Import: CompiledHeader
- Third-Party Notices
- Net: ChatChannel
- Physics: JointDesc
- motion_matching.cpp
- Components: Sprite
- Physics: BroadPhaseLayers
- Physics: Joint
- lsd_entity
- MotionDatabase
- play_session.h
- Array: Array
- Material: Material
- Vulkan Device: Frame
- Components: PhysicsJoint
- asset_registry.cpp
- Visual scripting (compiled to Lua)
- Behavior: Builder
- Editor Settings: GameUserSettings
- LineMap
- NodeDef
- Sound Cue: Evaluator
- Behavior Tree: BtNode
- Examples: your first game systems, step by step
- asset_tests.cpp
- Input: InputFrame
- Net: Writer
- Net: Msg
- Components: WorldSettings
- Vehicle
- script_tests.cpp
- Level modes (3D Modeling, Foliage, Landscape, Animation Sequence, PCG, VFX, 2D)
- SkinnedEntry
- game.hpp
- Game: ApiResult
- AssetType
- Script: Instance
- Components: PcgVolume
- Editor Screenshots: Editors dock (asset editors opened in a 
- Manual Docs: Audio (Sound, Sound Cue, Sound Class, Mix, Atte
- assistant_lessons.cpp
- lsd_position
- Table Editor: TableWindow
- Behavior Tree: Blackboard
- Input: KeyMapping
- spline.cpp
- Rhi: GpuLight
- Light Bake: BakedLighting
- Sound Cue: CueEnv
- Sound Player: u32
- Thumbnails: Entry
- Ui: ActionListener
- Grid: GpuLines
- Script: .write()
- TrackType
- Vec3
- Record
- ImportedMaterial
- Viewport Settings: ViewportSettings
- Ai Tests: add_box()
- Manual Docs: Details panel
- OcclusionBuffer
- skinned_cache.cpp
- Components: CullDistanceVolume
- Packaging: validate_assets()
- Play Session: Agent
- Audio: CompressorNode
- Voice: voice.cpp
- Opengl Device: Target
- Components: Camera
- LinearColor
- NavCutout
- Components: StaticMesh
- TextureData
- Claude Docs: Clean-room rules
- Editor Screenshots: Output Log + Console (Messages/Warnings/
- QueryShape
- Editor Modes: pcg_hash()
- occlusion.cpp
- LineCall
- Audio: Mix
- NavMesh::Impl
- Input: Binding
- Input: InputAction
- D3d12 Device: Buffer
- Components: SplineMeshes
- Chat
- File
- Editor Screenshots: Edit menu screenshot
- Editor Screenshots: Editor Preferences (Startup, Viewport, S
- Hit
- Sound Cue: CueVoice
- Rhi: MemoryBudget
- Opengl Device: MaterialShader
- LightingBuild
- SkinnedCache
- Curve: tangent()
- usize
- FieldType
- Manual Docs: Content Browser
- SkyDraw
- Asset Meta: MeshSocket
- Input: Trigger
- MaterialImportReport
- Plan
- Editor Screenshots: Content Browser panel (folder tree, asse
- hash_id.h
- Animation Player: Sockets
- cctype
- Light Bake: Node
- AssetRegistry::Async
- Opengl Device: .create_swapchain()
- u32
- material.h
- Script: NativeInstance
- Script Tests: Health
- Vendor
- Manual Docs: Build Lighting (light probes, Lightmass Importa
- Type
- BtRunner
- OutlineRow
- light_bake.cpp
- Main: DebugFrameContext
- Sound Player: Param
- Behavior Tree: BtStatus
- Audio: .class_node()
- GameMode
- PropertyFlags
- editor_settings.h
- Library
- SubLevel
- ToolSpec
- TagQuery
- Architecture Docs: AssetRegistry and .meta GUIDs
- Editor Screenshots: Engine Scalability (Low, Medium, High, E
- Editor Screenshots: Remote Console window screenshot
- Build Targets: CUDA / HIP compute plugin options
- Architecture Docs: Animation (ozz SkinnedModel, AnimGraph, m
- Architecture Docs: Level modes (Landscape, Foliage, PCG)
- Architecture Docs: 2D / Paper2D (sprites, tile maps)
- Architecture Docs: Framework-free unit tests (CTest)
- Architecture Docs: Workspaces and panel tabs
- Manual Docs: Height Fog
- Manual Docs: Viewport navigation and view modes

## God Nodes (most connected - your core abstractions)
1. `EditorState` - 349 edges
2. `PlaySession` - 226 edges
3. `D3D12Device` - 206 edges
4. `Entity` - 197 edges
5. `World` - 183 edges
6. `OpenGLDevice` - 155 edges
7. `Api` - 130 edges
8. `Mat4` - 115 edges
9. `VulkanDevice` - 86 edges
10. `ScriptSystem` - 86 edges

## Surprising Connections (you probably didn't know these)
- `2. Lua in five minutes` --references--> `input()`  [INFERRED]
  docs/Examples/Examples.md → engine/game_api/include/lsd/game.hpp
- `2. Lua in five minutes` --references--> `ui_open()`  [INFERRED]
  docs/Examples/Examples.md → engine/game_api/include/lsd/game.hpp
- `8. When something does not work` --references--> `ui_open()`  [INFERRED]
  docs/Examples/Examples.md → engine/game_api/include/lsd/game.hpp
- `apply_gun()` --references--> `InputAction`  [INFERRED]
  editor/assistant_lessons.cpp → engine/input/include/lsd/input/input.h
- `apply_gun()` --references--> `World`  [INFERRED]
  editor/assistant_lessons.cpp → engine/world/include/lsd/world/world.h

## Import Cycles
- None detected.

## Hyperedges (group relationships)
- **Core engine design principles** — docs_architecture_data_oriented_design, docs_architecture_reflection_drives_tools, docs_architecture_text_source_compiled_on_demand, docs_architecture_hot_reload [EXTRACTED 1.00]
- **Default editor layout: viewport, Outliner, Details, Content Browser, Editors dock** — docs_images_play_in_editor_content_browser, docs_images_outliner_outliner_panel, docs_images_play_in_editor_details_panel, docs_images_play_in_editor_editors_dock, docs_images_play_in_editor_workspace_tabs [EXTRACTED 1.00]
- **AI stack: navmesh, behaviour trees, perception** — docs_manual_navmesh, docs_manual_behavior_trees, docs_manual_perception [EXTRACTED 1.00]
- **Ways to add visual script nodes (ScriptNode, C/C++, JavaScript)** — docs_manual_script_node, docs_manual_lsd_node, docs_manual_js_nodes, docs_manual_visual_scripting [EXTRACTED 1.00]
- **Gameplay languages sharing the lsd API and script events** — docs_manual_lua_scripting, docs_manual_javascript_scripts, docs_manual_native_scripts, docs_manual_visual_scripting, docs_manual_lua_api [EXTRACTED 1.00]
- **Editor default layout: menu bar, tabs, workspaces, viewport, Outliner, Details, Content Browser, Editors dock** — docs_images_editor_overview_menu_bar, docs_images_editor_overview_level_asset_tabs, docs_images_editor_overview_workspaces, docs_images_editor_overview_viewport, docs_images_editor_overview_outliner, docs_images_editor_overview_details, docs_images_editor_overview_content_browser, docs_images_editor_overview_editors_dock [EXTRACTED 1.00]
- **Workspace tabs rearranging the Level editor (Modeling, Shading, Animation, Scripting, UI, Effects)** — docs_images_ws_modeling_workspace_tabs, docs_images_ws_modeling, docs_images_ws_shading, docs_images_ws_animation, docs_images_ws_scripting, docs_images_ws_ui, docs_images_ws_effects [EXTRACTED 1.00]
- **Gameplay languages sharing one binding list** — docs_architecture_one_binding_list, docs_architecture_lsd_game_c_api, docs_architecture_javascript_quickjs, docs_architecture_script_system, docs_architecture_script_graph, docs_architecture_scripthost [EXTRACTED 1.00]
- **Node libraries on script::Graph** — docs_architecture_script_graph, docs_architecture_material_graphs, docs_architecture_behavior_tree, docs_architecture_native_and_js_nodes [EXTRACTED 1.00]
- **JavaScript (QuickJS-ng) embedders** — engine_script_cmakelists_lsd_script, engine_ui_cmakelists_lsd_ui, third_party_cmakelists_quickjs [EXTRACTED 1.00]
- **Modules using lsd_reflect() code generation** — engine_reflect_cmakelists_lsd_reflect, engine_render_cmakelists_lsd_render, engine_asset_cmakelists_lsd_asset, engine_world_cmakelists_lsd_world, engine_input_cmakelists_lsd_input, engine_audio_cmakelists_lsd_audio, engine_animation_cmakelists_lsd_animation, engine_particles_cmakelists_lsd_particles, editor_cmakelists_lsd_editor, runtime_cmakelists_lsd_runtime, tools_reflect_cmakelists_lsd_reflect_gen [EXTRACTED 1.00]
- **One RHI, three graphics backends** — engine_rhi_cmakelists_lsd_rhi, engine_rhi_backends_d3d12_readme_d3d12_backend, engine_rhi_backends_opengl_readme_opengl_backend, engine_rhi_backends_vulkan_readme_vulkan_backend [EXTRACTED 1.00]
- **Per-asset editors opened as tabs in the Editors window** — docs_images_ed_mesh_asset_editor_tabs, docs_images_ed_mesh_mesh_editor, docs_images_ed_particles_particle_editor, docs_images_ed_prefab_prefab_editor, docs_images_ed_ui_ui_document_editor [EXTRACTED 1.00]
- **Profiling and diagnostics views** — docs_images_stat_unit_stat_overlay, docs_images_win_memory_memory_window, docs_images_output_log_output_log_console, docs_images_win_lua_debugger_lua_debugger [INFERRED 0.75]
- **Editor settings windows (Editor Preferences, Project Settings, World Settings)** — docs_images_win_preferences_editor_preferences, docs_images_win_project_settings_project_json, docs_images_win_world_settings_world_settings [INFERRED 0.85]
- **Level tab editor modes (3D Modeling, Landscape, PCG) sharing the viewport toolbar** — docs_images_mode_modeling_modeling_mode, docs_images_mode_landscape_landscape_mode, docs_images_mode_pcg_pcg_volume, docs_images_viewport_toolbar_toolbar [INFERRED 0.85]
- **Level modes with side panels painting into the level** — docs_images_level_modes_menu_level_modes, docs_images_mode_2d_2d_mode, docs_images_mode_foliage_foliage_mode [INFERRED 0.85]
- **C / C++ gameplay code workflow in the Tools menu** — docs_images_menu_tools_new_cpp_script, docs_images_menu_tools_compile_game_code, docs_images_menu_tools_setup_debugging_vscode [INFERRED 0.85]
- **Data assets edited in the reflection-generated Details panel (Save / Revert)** — docs_images_details_details_panel, docs_images_det_input_action_ia_interact, docs_images_det_material_material_data_asset, docs_images_det_particle_asset_ps_fire [INFERRED 0.95]
- **Graph editors sharing one node-graph UI (Palette, wiring, shortcuts)** — docs_images_ed_behavior_tree_node_graph_editor, docs_images_ed_behavior_tree_behavior_tree_editor, docs_images_ed_material_graph_material_graph_editor [INFERRED 0.95]

## Communities (326 total, 22 thin omitted)

### Community 0 - "Play Session (Play mode)"
Cohesion: 0.01
Nodes (141): AnimationPlayer, Brain, board, dt, entity, runner, session, tree (+133 more)

### Community 1 - "Editor State & Panels"
Cohesion: 0.01
Nodes (131): EditorState, active_collection, active_view, animations, asset_selection, asset_selection_count, bookmark_set, bookmarks (+123 more)

### Community 2 - "Lua Script API"
Cohesion: 0.05
Nodes (3): ScriptHost, user, Api

### Community 3 - "D3D12 Backend"
Cohesion: 0.02
Nodes (102): D3D12Device, adapter_, allocators_, back_buffers_, blend_pipelines_, buffers_, cull_args_, cull_out_ (+94 more)

### Community 4 - "Editor UI Panels"
Cohesion: 0.05
Nodes (65): apply_theme(), begin_panel(), build_bottom_editor_workspace(), build_layout_workspace(), build_scripting_workspace(), build_shading_workspace(), build_side_editor_workspace(), build_workspace() (+57 more)

### Community 5 - "JavaScript Host (QuickJS)"
Cohesion: 0.08
Nodes (45): call_from_lua(), create(), describe_exception(), describe_value(), destroy(), enter(), eval(), file_time() (+37 more)

### Community 6 - "AssetRegistry"
Cohesion: 0.11
Nodes (4): AssetRegistry, async_, records_, sync_scans_

### Community 7 - "OpenGL Backend"
Cohesion: 0.02
Nodes (82): OpenGLDevice, buffers_, context_, cull_args_, cull_in_, cull_out_, cull_pass_, cull_program_ (+74 more)

### Community 8 - "CMake Build Targets"
Cohesion: 0.06
Nodes (78): LSD_Engine CMake project, LSD_TRACY option, RHI backend options (LSD_RHI_D3D12/VULKAN/OPENGL), lsd_editor target, Editor shell (Viewport, Outliner, Details, Content Browser), EditorTheme (editor_theme.h, Config/editor_theme.json), lsd_ai target, lsd_animation target (+70 more)

### Community 9 - "asset_registry.h"
Cohesion: 0.05
Nodes (34): MeshCache, begin_editor(), Device, GizmoMode, Rotate, Scale, Translate, MeshCache (+26 more)

### Community 10 - "lsd_game.h"
Cohesion: 0.08
Nodes (21): broadcast(), camera_shake(), log(), log_error(), open_level(), pressed(), stop_sound(), time() (+13 more)

### Community 11 - "World"
Cohesion: 0.05
Nodes (36): find_named(), foliage_stamp(), duplicate(), default_controls(), fitted_body(), inside_volume(), json_string(), load_asset() (+28 more)

### Community 12 - "Vulkan Backend"
Cohesion: 0.04
Nodes (24): VulkanDevice, debug_ui_, device_, extent_, format_, frame_, frames_, gpu_ (+16 more)

### Community 13 - "Math Library"
Cohesion: 0.05
Nodes (96): ImDrawList, ImVec2, generate_pcg(), landscapes(), mode_brush(), project(), sculpt(), tile_brush() (+88 more)

### Community 14 - "Visual Script Graphs"
Cohesion: 0.09
Nodes (39): GraphNode, id, type, values, x, y, append(), append_quoted() (+31 more)

### Community 15 - "cstring"
Cohesion: 0.06
Nodes (14): Level, Error, Fatal, Info, Trace, Warning, writef(), frustum_planes() (+6 more)

### Community 16 - "Timeline"
Cohesion: 0.13
Nodes (18): Timeline, keys, tracks, TimelineKey, position, rotation, text, time (+10 more)

### Community 17 - "Script System"
Cohesion: 0.04
Nodes (28): ScriptSystem, breakpoint_count_, breakpoints_, count_, curve_count_, curves_, debugger_user_, host_ (+20 more)

### Community 18 - "Content Browser Assets"
Cohesion: 0.06
Nodes (56): add_to_collection(), asset_in_message(), asset_matches(), asset_selected(), create_data_asset(), create_default_input(), create_input_setup(), create_parts_prefab() (+48 more)

### Community 19 - "Lua Bindings"
Cohesion: 0.05
Nodes (41): debugger_, trace_, ScriptSystem::create_natives(), ScriptSystem::start_natives(), describe(), file_time(), fill_stop(), protected_call() (+33 more)

### Community 20 - "World Entities"
Cohesion: 0.06
Nodes (59): Entity, load_sublevel(), unload_sublevel(), find_in_instance(), instance_path(), is_overridden(), keeps_own(), parent_path() (+51 more)

### Community 21 - "UI Document Editor"
Cohesion: 0.07
Nodes (51): element_end(), find_attribute(), parse_style(), rml_append_to_body(), rml_attribute(), rml_find_tag(), rml_remove(), rml_set_attribute() (+43 more)

### Community 22 - "Jolt Physics"
Cohesion: 0.05
Nodes (23): PhysicsWorld::add_body(), PhysicsWorld::add_joint(), PhysicsWorld::add_vehicle(), PhysicsWorld::body_count(), PhysicsWorld::character_feet(), PhysicsWorld::create_character(), PhysicsWorld::drive_vehicle(), PhysicsWorld::joint_count() (+15 more)

### Community 23 - "Data Tables"
Cohesion: 0.05
Nodes (28): ColumnType, Auto, Bool, Int, Number, String, Vector, DataTable (+20 more)

### Community 25 - "src/navigation.cpp"
Cohesion: 0.18
Nodes (5): NavMesh::agent_moving(), NavMesh::agent_velocity(), NavMesh::remove_agent(), NavMesh::stop_agent(), NavMesh::update()

### Community 26 - "assistant_core.cpp"
Cohesion: 0.10
Nodes (34): code_blocks(), has(), Json, begin, end, json_escape(), flag, keys (+26 more)

### Community 27 - "Level Modes"
Cohesion: 0.10
Nodes (38): asset_list(), draw_2d(), draw_animation(), draw_foliage(), draw_landscape(), draw_mode_menu(), draw_mode_panel(), draw_modeling() (+30 more)

### Community 28 - "Job System & Foundation Tests"
Cohesion: 0.07
Nodes (23): Job, counter, data, fn, JobCounter, pending, JobSystem, capacity_mask_ (+15 more)

### Community 29 - "Graph Editor Window"
Cohesion: 0.04
Nodes (43): GraphWindow, box_start, boxing, changed_at, comment_resizing, device, dirty, drop_at (+35 more)

### Community 30 - "Native Script Runtime"
Cohesion: 0.07
Nodes (26): free_state(), make_state(), native_api(), native_property_default(), NativeModule::find(), NativeModule::find_function(), property_from_text(), property_to_text() (+18 more)

### Community 31 - "Reflection Generator Parser"
Cohesion: 0.06
Nodes (49): Arg, key, line, value, error(), Field, category, element_type (+41 more)

### Community 33 - "Script Tests"
Cohesion: 0.30
Nodes (22): clear_custom_nodes(), compile_graph(), main(), test_collapse_to_function(), test_custom_nodes(), test_data_table(), test_debugger(), test_events() (+14 more)

### Community 34 - "Multiplayer NetSession"
Cohesion: 0.05
Nodes (20): draw_chat_box(), LanGame, address, max_players, name, players, Mode, Client (+12 more)

### Community 35 - "ClipSet"
Cohesion: 0.14
Nodes (12): ClipSet, anim_guid, clips, count, meta, model_guid, modified, GraphEntry (+4 more)

### Community 36 - "Animation (ozz)"
Cohesion: 0.08
Nodes (18): blended_morph_weight(), Clip::morph_weight(), direction(), point(), Pose::aim_ik(), Pose::joint_model(), Pose::layer(), Pose::rebuild() (+10 more)

### Community 37 - "Details Rows"
Cohesion: 0.16
Nodes (30): add_to_selection(), copy_selection(), delete_selected(), draw_folder_row(), draw_gizmo(), draw_joint_pivot_gizmo(), draw_menu_bar(), draw_rows() (+22 more)

### Community 38 - "Audio (miniaudio)"
Cohesion: 0.12
Nodes (28): apply_class_volume(), AudioSystem::add_class(), AudioSystem::add_concurrency(), AudioSystem::class_count(), AudioSystem::class_name(), AudioSystem::fade_voice(), AudioSystem::init(), AudioSystem::length_seconds() (+20 more)

### Community 39 - "Editor Screenshots"
Cohesion: 0.06
Nodes (39): Event Start / Event Update nodes, Input Pressed node (IA_Interact Input Action), My Blueprint panel (Event Graph, Event Dispatchers, Functions, Variables), Save + Compile (visual script to Lua), Visual Script Editor screenshot (Interact_System), Content Browser (Content, Place, Output Log; Collections tree), Details panel (Transform, StaticMesh, + Add Component), Editors dock (asset editors open as tabs) (+31 more)

### Community 40 - "Anim Graph: GraphState"
Cohesion: 0.05
Nodes (47): AnimGraph, animation, entry_state, states, transitions, AnimState, blend_parameter, blend_parameter_y (+39 more)

### Community 41 - "Audio: AudioSystem"
Cohesion: 0.08
Nodes (10): AudioSystem, impl_, kMaxClasses, kMaxVoices, AudioSystem::AudioSystem(), AudioSystem::reset_classes(), AudioSystem::shutdown(), loudness() (+2 more)

### Community 42 - "Main: main()"
Cohesion: 0.08
Nodes (29): autosave_path(), batch_key(), BatchItem, draw, key, world, can_batch(), close_play_window() (+21 more)

### Community 43 - "Audio: AudioSystem::Impl"
Cohesion: 0.06
Nodes (31): AudioSystem::Impl, chains, class_count, class_cutoff, class_names, class_parent, class_volume, concurrency (+23 more)

### Community 44 - "Game: Vec3"
Cohesion: 0.11
Nodes (21): draw_line(), input(), move_to(), play_sound(), rotation(), set_rotation(), set_vec3(), spawn() (+13 more)

### Community 45 - "Particles: ParticleEmitter"
Cohesion: 0.05
Nodes (43): LifeColorKey, color, time, LifeKey, time, value, ParticleEmitter, acceleration (+35 more)

### Community 46 - "Rhi: UiDraw"
Cohesion: 0.06
Nodes (29): BufferTag, LightType, Directional, Point, Spot, LineVertex, color, position (+21 more)

### Community 47 - "Lighting: lighting.cpp"
Cohesion: 0.10
Nodes (31): add_atmosphere(), BakedLighting, find_sky(), fit_cascades(), forward_of(), level_lights(), light_rgb(), lights_only() (+23 more)

### Community 48 - "Net: Reader"
Cohesion: 0.16
Nodes (10): count, NetSession::find_lan_games(), NetSession::receive(), NetSession::send(), NetSession::set_owner(), Reader, left, ok (+2 more)

### Community 49 - "Physics: PhysicsWorld::Impl"
Cohesion: 0.05
Nodes (27): Contacts, mutex, pending, system, init_jolt(), PhysicsWorld::Impl, bodies, broad_phase_layers (+19 more)

### Community 50 - "Physics: PhysicsWorld"
Cohesion: 0.11
Nodes (9): PhysicsWorld, impl_, box(), centre(), main(), test_joint_extras(), test_joints(), test_shapes() (+1 more)

### Community 51 - "Reflect: Property"
Cohesion: 0.10
Nodes (19): Property, capacity, category, element_size, element_type, flags, has_max, has_min (+11 more)

### Community 52 - "Reflect: reflect.cpp"
Cohesion: 0.15
Nodes (26): BinaryWriter, capacity, length, out, element_property(), find_property(), load_binary(), load_file() (+18 more)

### Community 53 - "Components: f32"
Cohesion: 0.05
Nodes (38): AIController, behavior_tree, hearing_range, sight_angle, sight_range, DirectionalLight, cast_shadows, color (+30 more)

### Community 54 - "Mover"
Cohesion: 0.13
Nodes (15): Mover, duration, face_forward, offset, path, pause, repeat, repeat_minutes (+7 more)

### Community 55 - "D3d12 Device: u32"
Cohesion: 0.09
Nodes (20): ShadowMap, dsv, resource, srv_cpu, srv_gpu, Target, color, depth (+12 more)

### Community 56 - "Editor Settings: ProjectSettings"
Cohesion: 0.06
Nodes (36): ProjectSettings, base_sound_mix, build_configuration, chat_all, chat_box, chat_proximity, chat_squad, chat_team (+28 more)

### Community 57 - "string"
Cohesion: 0.07
Nodes (31): CodeBlock, code, language, path, Provider, kAnthropic, kOpenAICompatible, Reply (+23 more)

### Community 58 - "Net: NetSession::Impl"
Cohesion: 0.05
Nodes (37): History, head, s, NetSession::Impl, by_id, clock, current, destroyed_level_ids (+29 more)

### Community 59 - "Mesh Import: mesh_import.cpp"
Cohesion: 0.14
Nodes (25): compile_mesh_asset(), compile_mesh_key(), compiled_mesh_up_to_date(), compute_bounds(), compute_normals(), file_name(), from_ufbx(), has_extension() (+17 more)

### Community 60 - "Animation: Pose"
Cohesion: 0.11
Nodes (12): Pose, impl_, joints_, rows_, blended, graph_tests(), main(), morph_tests() (+4 more)

### Community 61 - "Editor Theme: EditorTheme"
Cohesion: 0.06
Nodes (32): EditorTheme, axis_x, axis_y, axis_z, content_area, content_bar, content_tab, details_row_a (+24 more)

### Community 62 - "Rhi: MeshDraw"
Cohesion: 0.05
Nodes (33): blend_view(), MeshDraw, alpha_cutoff, base_color_texture, blend, camera_position, color, debug_view (+25 more)

### Community 63 - "Game: Node"
Cohesion: 0.08
Nodes (18): Behaviour, name, Component, category, name, find(), Node, category (+10 more)

### Community 64 - "ui.cpp"
Cohesion: 0.06
Nodes (36): Geometry, indices, vertices, init(), read_file(), script_path_for(), Source, path (+28 more)

### Community 65 - "Editor Screenshots: Details panel (reflection-generated, gol"
Cohesion: 0.07
Nodes (32): IA_Interact InputAction data asset (Button bool, Trigger Down, Chord Action, Smoothing), Details panel: InputAction asset screenshot, Material data asset (PBR: Base Color, Metallic, Roughness, Normal/ORM textures, Emissive, Blend Mode, Opacity Mask Clip), Details panel: Material asset screenshot, PS_Fire ParticleSystem data asset (Emitters list: spawn rate, burst, lifetime, shape, spread), Details panel: ParticleSystem asset screenshot, Details panel (reflection-generated, gold tab, blue/grey rows, search, + Add Component), Details panel: selected entity screenshot (+24 more)

### Community 66 - "components.h"
Cohesion: 0.04
Nodes (50): AttachToSocket, socket, target, BlockingVolume, enabled, InstancedStaticMesh, cast_shadows, color (+42 more)

### Community 67 - "Audio: PlayDesc"
Cohesion: 0.07
Nodes (29): MixAdjust, lowpass, sound_class, volume, PlayDesc, attenuation, concurrency, delay (+21 more)

### Community 68 - "Rhi: PostDraw"
Cohesion: 0.05
Nodes (36): PostDraw, ao_intensity, ao_radius, auto_exposure, bloom_intensity, bloom_threshold, color_lut, contact_shadow (+28 more)

### Community 69 - "Main: draw_world()"
Cohesion: 0.14
Nodes (20): Caster, count, indices, vertices, world, draw_batches(), draw_casters(), draw_instanced_casters() (+12 more)

### Community 70 - "Particle Editor: ParticleEditor"
Cohesion: 0.08
Nodes (21): ParticleEditor, distance_, drawn_, eye_, focus_, guid_, instance_, kPreviewSize (+13 more)

### Community 72 - "Allocator: ArenaAllocator"
Cohesion: 0.09
Nodes (16): ArenaAllocator, memory_, offset_, HeapAllocator, allocated_, peak_, count_sink(), main() (+8 more)

### Community 73 - "Physics: BodyDesc"
Cohesion: 0.10
Nodes (21): BodyDesc, centre_of_mass_offset, debris, friction, half_extents, index_count, indices, mass (+13 more)

### Community 74 - "Transactions: Transactions"
Cohesion: 0.11
Nodes (9): State, label, size, text, Transactions, kMaxHistory, saved_, saved_valid_ (+1 more)

### Community 75 - "Animation: SkinnedModel"
Cohesion: 0.07
Nodes (30): apply_morphs(), BlendInput, clip, ratio, weight, Clip, duration, impl (+22 more)

### Community 76 - "Thumbnails: ThumbnailCache"
Cohesion: 0.12
Nodes (6): MaterialCache, ThumbnailCache, data_dir_, entries_, materials_, temporaries_

### Community 77 - "texture.cpp"
Cohesion: 0.27
Nodes (8): build_texture(), downsample(), load_texture(), SrgbTables, to_linear, to_srgb, texture_size(), write_png()

### Community 78 - "Vulkan Device: vulkan_device.cpp"
Cohesion: 0.09
Nodes (8): check(), debug_callback(), has_extension(), PhysicalDevice, handle, memory_budget, queue_family, vendor_from_id()

### Community 79 - "Rhi: AdapterInfo"
Cohesion: 0.09
Nodes (23): to_string(), AdapterInfo, dedicated_vram_bytes, index, name, shared_memory_bytes, supports_mesh_shaders, supports_raytracing (+15 more)

### Community 80 - "Native: NativeModule"
Cohesion: 0.06
Nodes (21): Component, category, defaults, field_count, field_names, fields, name, size (+13 more)

### Community 81 - "System Info: system_info.cpp"
Cohesion: 0.10
Nodes (17): CpuFeatures, avx2, avx512f, neon, sse42, SystemInfo, available_ram_bytes, cpu (+9 more)

### Community 82 - "graph_editor.cpp"
Cohesion: 0.07
Nodes (46): add_variable_node(), canvas_drops(), copy_selection(), create_behavior_tree_file(), create_cue_file(), create_graph_file(), create_material_graph_file(), draw_graph_editor() (+38 more)

### Community 83 - "physics.h"
Cohesion: 0.05
Nodes (44): CharacterDesc, feet, height, max_slope_degrees, radius, step_height, Contact, a (+36 more)

### Community 84 - "Rhi: DeviceDesc"
Cohesion: 0.11
Nodes (14): create_d3d12_device(), create_opengl_device(), create_vulkan_device(), Backend, Auto, D3D12, OpenGL, Vulkan (+6 more)

### Community 85 - "Opengl Device: MeshProgram"
Cohesion: 0.12
Nodes (10): debug_callback(), MeshProgram, color, flags, id, material, mvp, normal (+2 more)

### Community 86 - "Ui: UiContext"
Cohesion: 0.06
Nodes (7): SDL_Event, UiContext, impl_, key_of(), modifiers(), UiContext::close_all(), UiContext::process_event()

### Community 87 - "AnimationPlayer"
Cohesion: 0.05
Nodes (21): AnimationPlayer, cache_, dt_, finished_montages_, ik_count_, iks_, montages_, motion_inputs_ (+13 more)

### Community 88 - "Mesh Cache: CachedMesh"
Cohesion: 0.08
Nodes (16): CachedMesh, bounds_max, bounds_min, compiled_time, failed, generated, gpu, key (+8 more)

### Community 89 - "Sound Assets: SubmixEffect"
Cohesion: 0.06
Nodes (36): SoundAttenuation, falloff, max_distance, min_distance, occlusion, occlusion_muffle, rolloff, spatialize (+28 more)

### Community 90 - "Voice Io: VoiceIO::Impl"
Cohesion: 0.05
Nodes (24): VoiceIO, capture_, impl_, kFrame, kMaxSpeakers, kRate, Speaker, active (+16 more)

### Community 91 - "Console: console.cpp"
Cohesion: 0.11
Nodes (24): VarType, Bool, F32, I32, add(), add_command(), add_variable(), complete() (+16 more)

### Community 93 - "Rhi: LightSetup"
Cohesion: 0.08
Nodes (25): LightSetup, count, decal_info, decals, exposure, fog, fog_color, ground (+17 more)

### Community 94 - "Components: PostProcessVolume"
Cohesion: 0.07
Nodes (30): PostProcessVolume, anti_aliasing, ao_intensity, ao_radius, auto_exposure, bloom_intensity, bloom_threshold, color_grading_lut (+22 more)

### Community 95 - "Game Module: game_module.cpp"
Cohesion: 0.14
Nodes (13): api_include_dir(), close_library(), compile(), find_vcvars(), load(), new_script(), open_library(), process_id() (+5 more)

### Community 96 - "Editor Settings: EditorSettings"
Cohesion: 0.07
Nodes (28): EditorSettings, assistant_effort, assistant_enabled, assistant_endpoint, assistant_key, assistant_model, assistant_provider, assistant_role (+20 more)

### Community 97 - "Mesh Cache: mesh_cache.cpp"
Cohesion: 0.14
Nodes (8): build_lods(), destroy_lods(), file_time(), make_builtin(), MeshCache, data_dir_, index_, meshes_

### Community 98 - "Input: input.cpp"
Cohesion: 0.15
Nodes (14): InputSystem::add_context(), InputSystem::configure(), InputSystem::down(), InputSystem::find_action(), InputSystem::fired(), InputSystem::pressed(), InputSystem::value(), is_2d_key() (+6 more)

### Community 99 - "Reflect Test Types: PointLight"
Cohesion: 0.11
Nodes (18): Color, b, g, r, Palette, blend, colors, weights (+10 more)

### Community 100 - "remote_console.cpp"
Cohesion: 0.06
Nodes (37): capture_sink(), draw_window(), listen(), localhost(), poll(), receive(), run_captured(), send_all() (+29 more)

### Community 101 - "cstdio"
Cohesion: 0.15
Nodes (9): format_text(), plural_choice(), plural_one(), main(), test_lists(), test_round_trip(), test_strings_and_binary(), test_type_info() (+1 more)

### Community 102 - "Ui: RenderInterface"
Cohesion: 0.12
Nodes (8): Vertex, RenderInterface, device, drawing, scissor, scissor_on, viewport, UiContext::navigate()

### Community 103 - "Animation Player: State"
Cohesion: 0.12
Nodes (14): State, entity, graph, input_count, inputs, morph_key, morphed, motion (+6 more)

### Community 104 - "Script: script.h"
Cohesion: 0.07
Nodes (27): on_stop(), stop(), Breakpoint, line, source, DebugAction, Continue, StepInto (+19 more)

### Community 105 - "Material Cache: Entry"
Cohesion: 0.08
Nodes (19): Entry, blend_mode, failed, graph_instance, guid, material, modified, param_count (+11 more)

### Community 106 - "Details Panel: details_panel.cpp"
Cohesion: 0.14
Nodes (18): DetailsStyle, categories, custom_user, custom_widget, filter, row_colors, contains_ignore_case(), display_name() (+10 more)

### Community 108 - "MotionData"
Cohesion: 0.06
Nodes (31): MotionClip, animation, clip, looping, MotionData, clip_count, clip_first, clip_frames (+23 more)

### Community 109 - "Animation: Pose::Impl"
Cohesion: 0.07
Nodes (21): Animation, add_joint(), Clip::Impl, animation, morphs, MorphCurve, name, weights (+13 more)

### Community 110 - "allocator.cpp"
Cohesion: 0.10
Nodes (20): AllocatorStats, allocations, arena, bytes, name, peak, align_up(), add() (+12 more)

### Community 111 - "Lua Debugger: lua_debugger.cpp"
Cohesion: 0.20
Nodes (10): attach(), draw_window(), has(), load_source(), lookup(), now(), resume(), seconds_since() (+2 more)

### Community 112 - "Mesh Editor: MeshWindow"
Cohesion: 0.08
Nodes (21): MeshWindow, bone, device, key, meta, meta_dirty, morph, morph_weight (+13 more)

### Community 113 - "Paper2d: paper2d.cpp"
Cohesion: 0.17
Nodes (14): add_quad(), CachedMesh, cell_uv(), MeshCache, sprite_bounds(), sprite_cell(), sprite_mesh(), tile_map_bounds() (+6 more)

### Community 114 - "JSContext"
Cohesion: 0.11
Nodes (22): arg(), DocumentScript, close_requested, ctx_, document, name_, interrupt(), js_close() (+14 more)

### Community 115 - "Reflect: reflect.h"
Cohesion: 0.12
Nodes (11): TypeInfo, ListTraits, ListTraits<List<T, N>>, items_offset, load_text(), save_binary(), save_text(), TypeFlags (+3 more)

### Community 116 - "Audio: Voice"
Cohesion: 0.09
Nodes (19): AudioSystem::set_listener(), Voice, base_cutoff, cutoff, fading_out, group, id, instance (+11 more)

### Community 117 - ".wait_for_gpu"
Cohesion: 0.17
Nodes (5): MaterialShader, generation, mesh, skinned, size

### Community 118 - "User Types: user_types.h"
Cohesion: 0.10
Nodes (21): EnumEntry, name, NodePin, default_value, name, type, ScriptNode, category (+13 more)

### Community 119 - "Net: net.cpp"
Cohesion: 0.11
Nodes (15): Group, squad, team, name_id(), NetSession::host(), NetSession::join(), NetSession::NetSession(), NetSession::player_position() (+7 more)

### Community 120 - "Particle Player: ParticlePlayer"
Cohesion: 0.06
Nodes (22): Effect, entity, instance, system, used, file_time(), ParticlePlayer, collider_ (+14 more)

### Community 121 - "Net: NetSettings"
Cohesion: 0.07
Nodes (27): NetPlay, address, avatar, dedicated, enabled, host, open_mic, push_to_talk_key (+19 more)

### Community 122 - "draw_entity_details"
Cohesion: 0.08
Nodes (31): apply_layout_file(), asset_picker(), copy_changed(), details_is_overridden(), details_revert(), details_widget(), draw_entity_details(), draw_material_slots() (+23 more)

### Community 123 - "Material: MaterialInstance"
Cohesion: 0.12
Nodes (16): MaterialInstance, base_color, base_color_texture, emissive, emissive_strength, metallic, normal_texture, override_base_color (+8 more)

### Community 124 - "Curve: curve.h"
Cohesion: 0.12
Nodes (19): Curve, keys, CurveColor, a, b, g, r, CurveKey (+11 more)

### Community 125 - "assistant.cpp"
Cohesion: 0.11
Nodes (23): answer_tools(), ask(), draw_lessons(), draw_reply(), draw_settings(), draw_window(), engine_guide(), Job (+15 more)

### Community 127 - "Navigation: NavMesh"
Cohesion: 0.14
Nodes (3): NavMesh, impl_, main()

### Community 128 - "Asset Meta: AssetMeta"
Cohesion: 0.08
Nodes (26): AnimNotify, clip, sound, time, AssetMeta, attenuation, concurrency, guid (+18 more)

### Community 129 - "Script Tests: Properties"
Cohesion: 0.12
Nodes (8): add(), input_types(), Properties, list, push(), lsd_add(), Spinner, speed

### Community 130 - "Mesh: MeshData"
Cohesion: 0.10
Nodes (23): gather_bake_scene(), load_mesh(), Allocator, GpuMesh, index_count, indices, vertices, MeshData (+15 more)

### Community 132 - "Material Graph: Compiler"
Cohesion: 0.18
Nodes (10): compile_material_graph(), Compiler, body, done, error, error_capacity, visiting, contains() (+2 more)

### Community 133 - "Components: Character"
Cohesion: 0.09
Nodes (23): Character, camera_distance, camera_offset, crouch_action, crouch_height, crouch_speed, eye_height, height (+15 more)

### Community 134 - "Editor Screenshots: Play-in-Editor full editor screenshot"
Cohesion: 0.12
Nodes (20): Landscape Mode screenshot, Landscape Mode (New Landscape size, Create Landscape, Sculpt), 3D Modeling Mode screenshot, 3D Modeling Mode (Cube/Sphere/Plane shapes, Box UV, size snap, Collision), PCG Mode screenshot, PCG Volume (rules: mesh, density, seed, slope, scale; rebuilds on move/resize), Outliner panel screenshot, Outliner (Outline / Hierarchy tabs, visibility eye, lock, alternating rows, yellow scroll thumb) (+12 more)

### Community 135 - "Play Session: Moving"
Cohesion: 0.08
Nodes (23): Moving, body, centre, direction, distance, entity, fall, parked (+15 more)

### Community 136 - "material_import.cpp"
Cohesion: 0.12
Nodes (12): ImportedMaterials, blobs, materials, embed(), ends_with(), file_part(), file_texture(), list_fbx() (+4 more)

### Community 137 - "Behavior Tree: behavior_tree.cpp"
Cohesion: 0.17
Nodes (11): BtHost, user, Blackboard::find(), Blackboard::set(), BtRunner::abort(), BtRunner::active(), BtRunner::reset(), BtRunner::run() (+3 more)

### Community 138 - "level.cpp"
Cohesion: 0.38
Nodes (7): append(), append_component(), append_string(), in_transient(), load_level_file(), save_level(), save_level_file()

### Community 139 - "Json: load_level()"
Cohesion: 0.17
Nodes (15): data_types(), find_data_type(), load_data_asset(), parse(), read_data_asset_type(), read_file(), register_data_type(), save_data_asset() (+7 more)

### Community 140 - "Components: TileMap"
Cohesion: 0.10
Nodes (19): AudioVolume, enabled, fade, muffle, priority, reverb, room_size, TileMap (+11 more)

### Community 141 - "Components: Landscape"
Cohesion: 0.08
Nodes (22): List, count, items, Landscape, heights, kResolution, size, uv_tile (+14 more)

### Community 142 - "Components: u32"
Cohesion: 0.09
Nodes (22): NetReplicated, max_speed, net_id, owner, relevancy_distance, replicate_movement, replicated_fields, spawned_from (+14 more)

### Community 143 - "string"
Cohesion: 0.09
Nodes (24): mode_name(), draw_claude_mcp(), draw_history_window(), draw_panel_tabs(), EditorMode, Foliage, Landscape, Modeling (+16 more)

### Community 144 - "Importer"
Cohesion: 0.15
Nodes (9): import_mesh_materials(), Importer, images_, mesh_, search_, source_, textures_dir_, exists (+1 more)

### Community 145 - "Crash: crash.cpp"
Cohesion: 0.16
Nodes (9): crash_dir(), install_crash_reporting(), log_stack(), on_abort(), on_crash(), on_signal(), on_terminate(), save_log_and_marker() (+1 more)

### Community 146 - "Debug Draw: Text"
Cohesion: 0.13
Nodes (17): box(), count(), draw(), Line, a, b, color, seconds (+9 more)

### Community 147 - "Sound Player: SoundPlayer"
Cohesion: 0.10
Nodes (15): SoundPlayer, class_count_, classes_, cue_env_, group_count_, groups_, layer_count_, layers_ (+7 more)

### Community 148 - "Prefab Editor: PrefabWindow"
Cohesion: 0.12
Nodes (13): PrefabWindow, device, dirty, open, path, pitch, root, selected (+5 more)

### Community 149 - "Behavior Tree: BtRunner"
Cohesion: 0.17
Nodes (10): Entry, name, value, vector, State, child, ok, running (+2 more)

### Community 150 - "Log: log.cpp"
Cohesion: 0.21
Nodes (4): level_name(), set_min_level(), set_sink(), write()

### Community 151 - "Reflect: TypeInfo"
Cohesion: 0.18
Nodes (11): TypeInfo, alignment, category, defaults, flags, id, name, properties (+3 more)

### Community 152 - "Input: InputSystem"
Cohesion: 0.18
Nodes (10): InputSystem, action_count_, actions_, binding_count_, bindings_, context_count_, kMaxActions, kMaxBindings (+2 more)

### Community 153 - "Components: SkeletalMesh"
Cohesion: 0.11
Nodes (19): MaterialOverride, material, slot, MorphWeight, name, weight, SkeletalMesh, anim_graph (+11 more)

### Community 154 - "Material Cache: TextureCache"
Cohesion: 0.12
Nodes (12): file_time(), find_texture_guid(), MaterialCache, count_, entries_, index_, path_of(), TextureCache (+4 more)

### Community 155 - "References: references.cpp"
Cohesion: 0.24
Nodes (9): cook_set(), files_of(), find_guids(), is_text(), read(), ReferenceIndex, uses_, replace_references() (+1 more)

### Community 156 - "Behavior Tree: BtType"
Cohesion: 0.11
Nodes (18): BtType, ClearKey, Compare, Condition, Cooldown, ForceSuccess, IsSet, Loop (+10 more)

### Community 157 - "Net: NetEvent"
Cohesion: 0.11
Nodes (18): Kind, Call, Chat, Connected, Disconnected, PlayerJoined, PlayerLeft, Voice (+10 more)

### Community 158 - "Graph: PinType"
Cohesion: 0.07
Nodes (26): graph_lua(), search, value_widget(), GraphKind, BehaviorTree, Material, Script, SoundCue (+18 more)

### Community 159 - "Material Graph: MaterialCode"
Cohesion: 0.12
Nodes (14): MaterialCode, blend_mode, code, param_count, params, texture_count, textures, MaterialParam (+6 more)

### Community 160 - "mesh_materials.cpp"
Cohesion: 0.09
Nodes (17): Image, height, pixels, width, ImportedMap, channel, embedded, embedded_name (+9 more)

### Community 162 - "Montage"
Cohesion: 0.09
Nodes (21): Ik, aim, bone, entity, forward, target, weight, Montage (+13 more)

### Community 163 - "Source Control: source_control.cpp"
Cohesion: 0.23
Nodes (13): asset_menu(), draw(), Entry, code, path, git(), pathspec(), read_status() (+5 more)

### Community 164 - "Input: Action"
Cohesion: 0.12
Nodes (16): Action, chord, down, fired, guid, held, hold_done, smoothed (+8 more)

### Community 165 - "ParticleInstance"
Cohesion: 0.07
Nodes (21): Emitter, burst_done, capacity, count, done, particles, spawn_carry, time (+13 more)

### Community 166 - "Particles: particles.cpp"
Cohesion: 0.20
Nodes (10): bracket(), color_at(), ParticleInstance::build_ribbon(), ParticleInstance::build_sprites(), ParticleInstance::random(), size_at(), ParticleVertex, color (+2 more)

### Community 167 - "D3d12 Device: PostChain"
Cohesion: 0.11
Nodes (18): PostChain, ao, dof, down, exposed, exposure, exposure_index, exposure_valid (+10 more)

### Community 168 - "Opengl Device: PostChain"
Cohesion: 0.11
Nodes (18): PostChain, ao, dof, down, exposed, exposure, exposure_index, exposure_valid (+10 more)

### Community 169 - "Architecture Docs: Game.dll game module (hot reload)"
Cohesion: 0.11
Nodes (21): AudioSystem (miniaudio, sound classes, submixes, mixes), ai::BehaviorTree / BtRunner / Blackboard, Game.dll game module (hot reload), JavaScript scripts (QuickJS-ng inside Lua script system), lsd_game.h C API (lsd_api table), NetSession multiplayer (ENet), C / C++ and JavaScript nodes (register_call_node), NativeModule / lsd_behaviour (+13 more)

### Community 170 - "Manual Docs: Command line flags"
Cohesion: 0.12
Nodes (19): Text and voice chat (Opus), Claude MCP plugin (lsd_mcp), Command line flags, Console / Output Log, Editor Preferences (Saved/editor_settings.json), Game Mode and kept tables (game_instance, player_state, subsystem), Game UI (RmlUi HTML/CSS, UI window, designer canvas), Localization (string tables, Format Text, plurals) (+11 more)

### Community 171 - "Decals: DecalAtlas"
Cohesion: 0.13
Nodes (7): DecalAtlas, atlas_, count_, keys_, kTile, kTiles, times_

### Community 172 - "Animation: import_clips()"
Cohesion: 0.16
Nodes (12): Rig, Other, UE4Mannequin, UE5Manny, UE5MannySimple, detect_rig(), import_clips(), load_scene() (+4 more)

### Community 173 - "World Tests: world_tests.cpp"
Cohesion: 0.27
Nodes (11): tags_match(), tags_query(), main(), near(), test_curve(), test_entities(), test_level_round_trip(), test_spline() (+3 more)

### Community 174 - "TimelineWindow"
Cohesion: 0.09
Nodes (18): Original, has_value, name, property, transform, value, TimelineWindow, data (+10 more)

### Community 175 - "Claude Docs: CLAUDE.md LSD_Engine briefing"
Cohesion: 0.15
Nodes (17): Editor look / theme colours, Engine plan doc (claude.ai artifact), CLAUDE.md LSD_Engine briefing, ARCHITECTURE.md, Claude MCP bridge (lsd_mcp + remote console), ARCHITECTURE: How LSD_Engine works, Packaging and game mode (game.json), BACKLOG: features by milestone (+9 more)

### Community 176 - "Architecture Docs: EXAMPLES: worked tutorials"
Cohesion: 0.18
Nodes (11): Automatic instancing (can_batch / draw_batches), Lit PBR shader (HLSL + GLSL mirror), Material graphs (compile_material_graph), ParticleSystem / ParticleInstance, Post pass draw_post (SSAO, TAA, DOF, bloom, eye adaptation, FXAA), rhi::Device (D3D12 / OpenGL / Vulkan), UI on RmlUi (UiContext), Example: campfire (particles, sound, light) (+3 more)

### Community 177 - "Editor Ui: EditorCamera"
Cohesion: 0.12
Nodes (14): EditorCamera, fov_y, ortho_height, pitch, pivot_distance, position, view, yaw (+6 more)

### Community 178 - "Light Bake: Bvh"
Cohesion: 0.28
Nodes (7): bake_probe(), bounce(), Bvh, nodes, order, scene, sky_radiance()

### Community 179 - "MeshPart"
Cohesion: 0.07
Nodes (25): CompiledMeshSource, settings_hash, source_size, source_time, compute_normals(), MeshBounds, max, min (+17 more)

### Community 180 - "Mesh Import: CompiledHeader"
Cohesion: 0.17
Nodes (10): CompiledHeader, bounds_max, bounds_min, index_count, magic, settings_hash, source_size, source_time (+2 more)

### Community 181 - "Third-Party Notices"
Cohesion: 0.09
Nodes (22): cgltf — v1.15 — MIT, Dear ImGui — v1.93.0-docking — MIT, ENet — v1.3.18 — MIT, FreeType — VER-2-13-3 — FreeType Licence (FTL)  [dual: FTL or GPL-2.0], ImGuizmo — `105e4fde` — MIT, Jolt Physics — v5.6.0 — MIT, LuaJIT — `8ca76297` (v2.1) — MIT, meshoptimizer — v1.3 — MIT (+14 more)

### Community 182 - "Net: ChatChannel"
Cohesion: 0.24
Nodes (10): ChatChannel, All, Proximity, Squad, Team, chat_channel_name(), NetSession::send_chat(), NetSession::send_voice() (+2 more)

### Community 183 - "Physics: JointDesc"
Cohesion: 0.13
Nodes (15): JointDesc, axis, body_a, body_b, break_force, cone_angle, limit_max, limit_min (+7 more)

### Community 184 - "motion_matching.cpp"
Cohesion: 0.19
Nodes (14): ends(), find_bone(), left_foot(), lower(), MotionData::build(), not_helper(), position_of(), predict_trajectory() (+6 more)

### Community 185 - "Components: Sprite"
Cohesion: 0.13
Nodes (15): Sprite, alpha_cutoff, collision, columns, flip_x, flip_y, fps, frame (+7 more)

### Community 186 - "Physics: BroadPhaseLayers"
Cohesion: 0.15
Nodes (4): BroadPhaseLayers, GetBroadPhaseLayerName(), ObjectPairFilter, ObjectVsBroadPhaseFilter

### Community 187 - "Physics: Joint"
Cohesion: 0.15
Nodes (12): JointType, Ball, Cone, Fixed, Hinge, Slider, Joint, a (+4 more)

### Community 188 - "lsd_entity"
Cohesion: 0.10
Nodes (20): alive(), call(), destroy(), has(), has_tag(), name(), player(), remove() (+12 more)

### Community 189 - "MotionDatabase"
Cohesion: 0.11
Nodes (18): Motion, data, database, key, mesh, ok, settings, MotionDatabase (+10 more)

### Community 190 - "play_session.h"
Cohesion: 0.09
Nodes (9): find(), KeyIndex, map_, UiContext, VoiceDecoder, state_, VoiceEncoder, state_ (+1 more)

### Community 191 - "Array: Array"
Cohesion: 0.19
Nodes (3): Array, allocator_, capacity_

### Community 192 - "Material: Material"
Cohesion: 0.14
Nodes (12): Material, base_color, base_color_texture, blend_mode, emissive, emissive_strength, metallic, normal_texture (+4 more)

### Community 193 - "Vulkan Device: Frame"
Cohesion: 0.12
Nodes (5): Frame, cmd, fence, image_available, pool

### Community 194 - "Components: PhysicsJoint"
Cohesion: 0.14
Nodes (14): PhysicsJoint, axis, break_force, cone_angle, limit_max, limit_min, limited, motor_speed (+6 more)

### Community 195 - "asset_registry.cpp"
Cohesion: 0.20
Nodes (14): AssetRegistry::apply(), AssetRegistry::AssetRegistry(), AssetRegistry::load_meta(), AssetRegistry::save_meta(), AssetRegistry::scan(), AssetRegistry::scan_async(), AssetRegistry::search(), contains_ignore_case() (+6 more)

### Community 196 - "Visual scripting (compiled to Lua)"
Cohesion: 0.12
Nodes (22): CODING.md, How to code in LSD_Engine, Compiler Results, Create Prefab from Parts, JavaScript scripts, Nodes from JavaScript (var nodes), LSD_NODE / LSD_PURE_NODE, lsd Lua API (+14 more)

### Community 197 - "Behavior: Builder"
Cohesion: 0.24
Nodes (7): build_behavior_tree(), Builder, capacity, error, visited, visited_count, load_behavior_tree()

### Community 198 - "Editor Settings: GameUserSettings"
Cohesion: 0.12
Nodes (14): GameUserSettings, effects_volume, fullscreen, invert_look, key_bindings, language, master_volume, mouse_sensitivity (+6 more)

### Community 199 - "LineMap"
Cohesion: 0.33
Nodes (4): LineMap, node_of_line, path, time

### Community 200 - "NodeDef"
Cohesion: 0.11
Nodes (18): GraphLink, from_node, from_pin, to_node, to_pin, NodeDef, category, code (+10 more)

### Community 201 - "Sound Cue: Evaluator"
Cohesion: 0.26
Nodes (5): evaluate_cue(), Evaluator, capacity, count, out

### Community 202 - "Behavior Tree: BtNode"
Cohesion: 0.17
Nodes (12): BehaviorTree, count, nodes, BtNode, child_count, children, id, key (+4 more)

### Community 203 - "Examples: your first game systems, step by step"
Cohesion: 0.12
Nodes (18): 0. Before you start: the four things you will use, 1.1 Make the Input Actions, 1.2 Map keys to them: the Input Mapping Context, 1.3 Give them to the Character, 1. Input Actions: Move, Look, Jump, Crouch, Sprint, Swim, Interact, 2. Lua in five minutes, 3. The HUD: a health bar and a stamina bar, 4. Player stats: health and stamina, and sprinting that drains stamina (+10 more)

### Community 204 - "asset_tests.cpp"
Cohesion: 0.11
Nodes (16): Handle, kGenerationBits, kGenerationMask, kIndexMask, kInvalid, value, operator==(), main() (+8 more)

### Community 205 - "Input: InputFrame"
Cohesion: 0.18
Nodes (12): InputFrame, gamepad_axes, gamepad_buttons, key_count, keys, mouse_buttons, mouse_dx, mouse_dy (+4 more)

### Community 206 - "Net: Writer"
Cohesion: 0.23
Nodes (3): NetSession::send_event(), Writer, bytes

### Community 207 - "Net: Msg"
Cohesion: 0.12
Nodes (16): Msg, kChat, kCorrection, kDestroy, kEvent, kFields, kGroup, kOwner (+8 more)

### Community 208 - "Components: WorldSettings"
Cohesion: 0.15
Nodes (13): NavSettings, agent_height, agent_radius, cell_size, max_slope, max_step_height, WorldSettings, exposure_ev100 (+5 more)

### Community 209 - "Vehicle"
Cohesion: 0.06
Nodes (34): Vehicle, acceleration, boost, brake_torque, braking, camera_distance, camera_height, climb_speed (+26 more)

### Community 210 - "script_tests.cpp"
Cohesion: 0.09
Nodes (22): curve_path(), DebugLog, count, lines, saw_local, input(), JsDebugLog, count (+14 more)

### Community 211 - "Level modes (3D Modeling, Foliage, Landscape, Animation Sequence, PCG, VFX, 2D)"
Cohesion: 0.11
Nodes (24): Animation Graph (states, blend spaces, transitions), Audio (Sound, Sound Cue, Sound Class, Mix, Attenuation, Occlusion), Character component, Input Actions and Mapping Contexts (Enhanced Input), Foliage mode, Instanced Static Mesh, Landscape mode, Level modes (3D Modeling, Foliage, Landscape, Animation Sequence, PCG, VFX, 2D) (+16 more)

### Community 212 - "SkinnedEntry"
Cohesion: 0.12
Nodes (16): Section, index_count, indices, slot_material, SkinnedEntry, failed, guid, index_count (+8 more)

### Community 213 - "game.hpp"
Cohesion: 0.13
Nodes (8): call_node(), ComponentRegistrar, from_value(), get_vec3(), NodeRegistrar, Registrar, to_value(), lsd_get_vec3()

### Community 214 - "Game: ApiResult"
Cohesion: 0.20
Nodes (8): api(), ApiResult, json, ok, get_text(), json_arg(), lsd_get_text(), lsd_lua_call()

### Community 215 - "AssetType"
Cohesion: 0.12
Nodes (16): AssetType, BehaviorTree, Cue, Data, Graph, Level, MaterialGraph, Mesh (+8 more)

### Community 216 - "Script: Instance"
Cohesion: 0.17
Nodes (11): CachedTable, modified, path, ref, Instance, entity, env, failed (+3 more)

### Community 217 - "Components: PcgVolume"
Cohesion: 0.17
Nodes (12): PcgVolume, align_to_surface, color, density, material, max_slope, mesh, output (+4 more)

### Community 218 - "Editor Screenshots: Editors dock (asset editors opened in a "
Cohesion: 0.20
Nodes (11): World Settings window screenshot, World Settings (Kill Z, Gravity, Exposure Ev100, Game Mode, Level Script, Navigation), Animation workspace screenshot, Effects workspace screenshot, Modeling workspace screenshot, Editors dock (asset editors opened in a workspace dock as tabs), Blender-style workspace tabs (Layout, Modeling, Shading, Animation, Scripting, UI, Effects), Scripting workspace screenshot (+3 more)

### Community 219 - "Manual Docs: Audio (Sound, Sound Cue, Sound Class, Mix, Atte"
Cohesion: 0.18
Nodes (11): Behavior Trees, Blackboard, decorators, services, Level script, Navmesh, Nav Agent, Nav Modifier Volume, AI perception (sight, hearing), Place tab, Post Process Volume (ACES, bloom, TAA/FXAA, SSR, SSAO, DOF, auto exposure), Splines, Spline Meshes, Spline Follower, Sub-levels and distance streaming (+3 more)

### Community 220 - "assistant_lessons.cpp"
Cohesion: 0.23
Nodes (9): apply_gun(), apply_lesson(), input_action(), Lesson, summary, text, title, lessons() (+1 more)

### Community 221 - "lsd_position"
Cohesion: 0.14
Nodes (10): position(), set_number(), set_position(), lsd_position(), lsd_register_script(), lsd_set_number(), lsd_set_position(), c_mover_register() (+2 more)

### Community 222 - "Table Editor: TableWindow"
Cohesion: 0.14
Nodes (16): Cell, text, create_table_file(), draw_table_editor(), open_table(), row_count(), save(), TableWindow (+8 more)

### Community 223 - "Behavior Tree: Blackboard"
Cohesion: 0.22
Nodes (5): Blackboard, count, entries, Blackboard::is_set(), bt_tests()

### Community 224 - "Input: KeyMapping"
Cohesion: 0.18
Nodes (10): InputMappingContext, mappings, priority, KeyMapping, action, axis, dead_zone, key (+2 more)

### Community 225 - "spline.cpp"
Cohesion: 0.43
Nodes (10): length, locate(), point_tangent(), spline_at_distance(), spline_length(), spline_point(), spline_segments(), spline_tangent() (+2 more)

### Community 226 - "Rhi: GpuLight"
Cohesion: 0.18
Nodes (11): GpuLight, color, direction, pad, position, range, shadow_offset, shadow_tile (+3 more)

### Community 227 - "Light Bake: BakedLighting"
Cohesion: 0.12
Nodes (12): BakedLighting, cell, dims, origin, sh, BakeScene, albedo, hi (+4 more)

### Community 228 - "Sound Cue: CueEnv"
Cohesion: 0.22
Nodes (8): CueEnv, last, last_count, seed, user, LastPick, index, node

### Community 229 - "Sound Player: u32"
Cohesion: 0.20
Nodes (9): Known, guid, index, Layer, name, voice, Pushed, id (+1 more)

### Community 230 - "Thumbnails: Entry"
Cohesion: 0.20
Nodes (8): Entry, failed, from_disk, guid, ready, save, source_time, texture

### Community 231 - "Ui: ActionListener"
Cohesion: 0.13
Nodes (7): ActionInstancer, ActionListener, action_, set_translator(), SystemInterface, translator, translator_user

### Community 232 - "Grid: GpuLines"
Cohesion: 0.13
Nodes (10): GpuLines, vertex_count, vertices, GridColors, axis_x, axis_y, major, minor (+2 more)

### Community 233 - "Script: .write()"
Cohesion: 0.33
Nodes (4): push_struct(), push_value(), read_struct(), read_value()

### Community 234 - "TrackType"
Cohesion: 0.40
Nodes (5): TrackType, CameraCut, Property, Sound, Transform

### Community 235 - "Vec3"
Cohesion: 0.33
Nodes (10): from_recast(), NavMesh::add_agent(), NavMesh::agent_position(), NavMesh::build(), NavMesh::debug_lines(), NavMesh::find_path(), NavMesh::move_agent(), NavMesh::nearest_point() (+2 more)

### Community 236 - "Record"
Cohesion: 0.10
Nodes (17): Pool, capacity, count, data, owners, stride, type, Record (+9 more)

### Community 237 - "ImportedMaterial"
Cohesion: 0.14
Nodes (13): ImportedMaterial, base_color, base_color_map, metallic, metallic_map, name, normal_map, occlusion_map (+5 more)

### Community 238 - "Viewport Settings: ViewportSettings"
Cohesion: 0.15
Nodes (8): ViewportSettings, background, draw_triangle, frame_rate, scale, spin_speed, vram_budget, vram_used

### Community 239 - "Ai Tests: add_box()"
Cohesion: 0.20
Nodes (6): add_box(), FakeAgent, at, scripts, target, walking

### Community 240 - "Manual Docs: Details panel"
Cohesion: 0.29
Nodes (6): Components reference, Curves (Curve, Curve Vector, Curve Color), Data assets (.lsdasset), Data Tables and Curve Tables, Details panel, Exposed script properties

### Community 241 - "OcclusionBuffer"
Cohesion: 0.20
Nodes (6): OcclusionBuffer, depth_, kHeight, kWidth, triangles_, test_occlusion()

### Community 242 - "skinned_cache.cpp"
Cohesion: 0.31
Nodes (5): destroy_buffers(), file_time(), model_section(), record_of(), section_material()

### Community 243 - "Components: CullDistanceVolume"
Cohesion: 0.33
Nodes (6): CullDistance, distance, size, CullDistanceVolume, cull_distances, enabled

### Community 244 - "Packaging: validate_assets()"
Cohesion: 0.22
Nodes (6): cook(), expected_prefix(), folder_name(), is_hex(), package_project(), validate_assets()

### Community 245 - "Play Session: Agent"
Cohesion: 0.22
Nodes (8): Agent, entity, id, UiFollower, doc, entity, height, id

### Community 246 - "Audio: CompressorNode"
Cohesion: 0.22
Nodes (8): CompressorNode, attack, base, envelope, makeup_db, ratio, release, threshold_db

### Community 247 - "Voice: voice.cpp"
Cohesion: 0.33
Nodes (3): VoiceDecoder::decode(), VoiceEncoder::encode(), VoiceEncoder::VoiceEncoder()

### Community 248 - "Opengl Device: Target"
Cohesion: 0.20
Nodes (7): Target, color, depth, fbo, generation, height, width

### Community 249 - "Components: Camera"
Cohesion: 0.22
Nodes (9): Camera, arm_length, auto_activate, follow_bone, fov, motion, ortho_height, orthographic (+1 more)

### Community 250 - "LinearColor"
Cohesion: 0.04
Nodes (44): LinearColor, a, b, g, r, Decal, texture, tint (+36 more)

### Community 251 - "NavCutout"
Cohesion: 0.15
Nodes (11): NavCutout, corners, z_max, z_min, NavSettings, agent_height, agent_radius, cell_height (+3 more)

### Community 252 - "Components: StaticMesh"
Cohesion: 0.22
Nodes (9): StaticMesh, box_uv, collision, collision_shape, color, material, max_draw_distance, mesh (+1 more)

### Community 253 - "TextureData"
Cohesion: 0.18
Nodes (6): TextureData, height, mip_count, mip_offset, pixels, width

### Community 254 - "Claude Docs: Clean-room rules"
Cohesion: 0.14
Nodes (10): Kittakin Studios, LSD_Engine, Game-code components (register_component_type), lsd_reflect_gen / TypeInfo, world::World (entities, component pools), Bitsquid development blog, Data-Oriented Design (Richard Fabian), Game Engine Architecture (Jason Gregory) (+2 more)

### Community 255 - "Editor Screenshots: Output Log + Console (Messages/Warnings/"
Cohesion: 0.25
Nodes (8): Output Log screenshot, Output Log + Console (Messages/Warnings/Errors filters, category tags, command line with Tab completion), Remote console (127.0.0.1:7797), Video memory budget (texture size, shadow atlas) and Graphics quality recommendation, stat unit overlay screenshot, stat unit overlay (fps, game / render / GPU ms, draws, triangles, culled, engine memory), Memory window screenshot, Memory window (process RAM, video memory vs budget, per-allocator in use / peak / blocks)

### Community 256 - "QueryShape"
Cohesion: 0.18
Nodes (12): Kind, Box, Capsule, Sphere, QueryShape, half_extents, half_height, kind (+4 more)

### Community 257 - "Editor Modes: pcg_hash()"
Cohesion: 0.20
Nodes (5): build_landscape_mesh(), fnv(), pcg_hash(), update_landscapes(), make_default_level()

### Community 258 - "occlusion.cpp"
Cohesion: 0.26
Nodes (8): edge(), OcclusionBuffer::add_occluder(), OcclusionBuffer::box_visible(), project(), Projected, w, x, y

### Community 259 - "LineCall"
Cohesion: 0.17
Nodes (11): js_stack(), JsFrame, function, line, source, line_depth(), LineCall, ctx (+3 more)

### Community 260 - "Audio: Mix"
Cohesion: 0.25
Nodes (8): Mix, adjust, count, fade_in, fade_out, id, popping, weight

### Community 261 - "NavMesh::Impl"
Cohesion: 0.17
Nodes (7): NavMesh::clear(), NavMesh::Impl, crowd, filter, mesh, query, NavMesh::NavMesh()

### Community 262 - "Input: Binding"
Cohesion: 0.25
Nodes (8): Binding, action, axis, context, dead_zone, priority, scale, source

### Community 263 - "Input: InputAction"
Cohesion: 0.25
Nodes (7): InputAction, chord_action, description, smoothing, trigger, trigger_time, value_type

### Community 264 - "D3d12 Device: Buffer"
Cohesion: 0.22
Nodes (7): Buffer, generation, resource, size, usage, BufferUsage, Index

### Community 265 - "Components: SplineMeshes"
Cohesion: 0.25
Nodes (8): SplineMeshes, cast_shadows, color, follow_curve, material, mesh, scale, spacing

### Community 266 - "Chat"
Cohesion: 0.17
Nodes (11): Chat, allow_all, armed_overwrite, include_context, input, job, last_error, pending (+3 more)

### Community 267 - "File"
Cohesion: 0.17
Nodes (8): AssetRegistry::Walk, files, ok, File, meta_time, relative, time, type

### Community 268 - "Editor Screenshots: Edit menu screenshot"
Cohesion: 0.29
Nodes (7): Editor Preferences, Object edit commands (Duplicate, Group, Ungroup, Cut/Copy/Paste, Delete, Drop to Floor), Project Settings, Edit menu screenshot, Undo / Redo, Window menu screenshot, Window panels (Undo History, World Settings, Message Log, Memory, Layout)

### Community 269 - "Editor Screenshots: Editor Preferences (Startup, Viewport, S"
Cohesion: 0.33
Nodes (7): Editor Preferences window screenshot, Editor Preferences (Startup, Viewport, Snapping, Content Browser settings), Startup: Last Level, Recent Levels, Autosave Minutes, Snapping settings (Snap Move / Angle / Scale, Snap To Surface), Project Settings window screenshot, Maps: Game Default Level / Editor Startup Level, Config/project.json (team-shared project settings)

### Community 270 - "Hit"
Cohesion: 0.20
Nodes (10): get_number(), Hit, entity, normal, point, raycast(), sphere_trace(), lsd_get_number() (+2 more)

### Community 271 - "Sound Cue: CueVoice"
Cohesion: 0.29
Nodes (6): CueVoice, delay, looping, pitch, sound, volume

### Community 272 - "Rhi: MemoryBudget"
Cohesion: 0.29
Nodes (3): MemoryBudget, budget_bytes, usage_bytes

### Community 273 - "Opengl Device: MaterialShader"
Cohesion: 0.18
Nodes (9): MaterialShader, generation, mesh, skinned, ShadowMap, fbo, generation, size (+1 more)

### Community 274 - "LightingBuild"
Cohesion: 0.18
Nodes (8): LightingBuild, done, finished, result, scene, started, thread, total

### Community 275 - "SkinnedCache"
Cohesion: 0.22
Nodes (7): SkinnedCache, count_, entries_, graph_count_, graphs_, set_count_, sets_

### Community 276 - "Curve: tangent()"
Cohesion: 0.48
Nodes (3): evaluate(), sort_keys(), tangent()

### Community 277 - "usize"
Cohesion: 0.29
Nodes (7): AssetRegistry::delete_asset(), AssetRegistry::meta_path(), AssetRegistry::move_asset(), AssetRegistry::rename_asset(), AssetRegistry::source_path(), move_with_meta(), valid_name()

### Community 278 - "FieldType"
Cohesion: 0.18
Nodes (11): FieldType, Array, Bool, F32, F64, I32, I64, String (+3 more)

### Community 279 - "Manual Docs: Content Browser"
Cohesion: 0.47
Nodes (4): Content Browser, Live Link folder, Reference Viewer / Replace References, Git source control in Content Browser

### Community 280 - "SkyDraw"
Cohesion: 0.18
Nodes (8): SkyDraw, camera, ground, horizon, inv_view_proj, sun_color, sun_direction, zenith

### Community 281 - "Asset Meta: MeshSocket"
Cohesion: 0.18
Nodes (8): MaterialSlot, material, name, MeshSocket, bone, name, position, rotation

### Community 282 - "Input: Trigger"
Cohesion: 0.29
Nodes (7): Trigger, Down, Hold, Pressed, Released, Tap, InputSystem::set_trigger()

### Community 283 - "MaterialImportReport"
Cohesion: 0.20
Nodes (6): MaterialImportReport, created, materials, missing, reused, textures

### Community 284 - "Plan"
Cohesion: 0.22
Nodes (9): Plan, base_color, masked, material_path, metal_map, normal, orm, rough_map (+1 more)

### Community 285 - "Editor Screenshots: Content Browser panel (folder tree, asse"
Cohesion: 0.40
Nodes (5): Collections (UI, Sky) in Content Browser folder tree, Content Browser panel (folder tree, asset grid, type filter, sort), Output Log tab, Place tab, Content Browser screenshot

### Community 286 - "hash_id.h"
Cohesion: 0.36
Nodes (4): hash64(), HashId, value, operator==()

### Community 287 - "Animation Player: Sockets"
Cohesion: 0.40
Nodes (4): Sockets, guid, meta, modified

### Community 288 - "cctype"
Cohesion: 0.50
Nodes (4): instrument(), one_of(), regex_after(), word_char()

### Community 289 - "Light Bake: Node"
Cohesion: 0.40
Nodes (5): Node, count, first, hi, lo

### Community 290 - "AssetRegistry::Async"
Cohesion: 0.25
Nodes (5): AssetRegistry::Async, done, generation, thread, walk

### Community 291 - "Opengl Device: .create_swapchain()"
Cohesion: 0.33
Nodes (3): GlFunctions, vendor_from_string(), SDL_Window

### Community 292 - "u32"
Cohesion: 0.13
Nodes (10): Buffer, generation, id, usage, Texture, generation, id, TextureFormat (+2 more)

### Community 293 - "material.h"
Cohesion: 0.25
Nodes (4): MaterialParameter, name, value, resolve()

### Community 294 - "Script: NativeInstance"
Cohesion: 0.40
Nodes (4): NativeInstance, behaviour, entity, state

### Community 295 - "Script Tests: Health"
Cohesion: 0.33
Nodes (5): Health, alive, armor, hp, title

### Community 296 - "Vendor"
Cohesion: 0.25
Nodes (7): vendor_from_id(), Vendor, Amd, Intel, Nvidia, Other, Unknown

### Community 297 - "Manual Docs: Build Lighting (light probes, Lightmass Importa"
Cohesion: 0.67
Nodes (4): Build Lighting (light probes, Lightmass Importance Volume), Lights (physical units), Sky and Atmosphere, Sky reflections (split-sum BRDF, no cubemaps)

### Community 298 - "Type"
Cohesion: 0.29
Nodes (7): Type, Array, Bool, Null, Number, Object, String

### Community 299 - "BtRunner"
Cohesion: 0.29
Nodes (6): BtRunner, clock_, cooldown_until_, moved_, states_, tree_

### Community 300 - "OutlineRow"
Cohesion: 0.40
Nodes (5): collect_hierarchy(), collect_rows(), OutlineRow, depth, entity

### Community 301 - "light_bake.cpp"
Cohesion: 0.31
Nodes (5): apply_baked_lighting(), bake_lighting(), baked_lighting_path(), load_baked_lighting(), save_baked_lighting()

### Community 302 - "Main: DebugFrameContext"
Cohesion: 0.50
Nodes (4): DebugFrameContext, device, state, window

### Community 303 - "Sound Player: Param"
Cohesion: 0.50
Nodes (3): Param, name, value

### Community 304 - "Behavior Tree: BtStatus"
Cohesion: 0.50
Nodes (4): BtStatus, Failure, Running, Success

### Community 306 - "GameMode"
Cohesion: 0.33
Nodes (6): GameMode, hud, player_controller, player_prefab, script, show_cursor

### Community 307 - "PropertyFlags"
Cohesion: 0.40
Nodes (5): PropertyFlags, kEdit, kEditorOnly, kSave, kVisible

### Community 310 - "editor_settings.h"
Cohesion: 0.12
Nodes (14): CollectionEntry, collection, guid, CustomWorkspace, base, name, FavouriteFolder, path (+6 more)

### Community 311 - "Library"
Cohesion: 0.40
Nodes (4): Library, ctx, modified, path

### Community 312 - "SubLevel"
Cohesion: 0.40
Nodes (5): SubLevel, level, load_at_start, load_distance, show_in_editor

### Community 313 - "ToolSpec"
Cohesion: 0.50
Nodes (4): ToolSpec, description, name, schema

### Community 314 - "TagQuery"
Cohesion: 0.50
Nodes (4): TagQuery, All, Any, None

## Ambiguous Edges - Review These
- `lsd_core target` → `engine/core README (placeholder)`  [AMBIGUOUS]
  engine/core/README.md · relation: conceptually_related_to

## Knowledge Gaps
- **3196 isolated node(s):** `speed`, `entity`, `pose`, `time`, `graph` (+3191 more)
  These have ≤1 connection - possible missing edges or undocumented components. (Counts symbols only; 4072 node(s) total have ≤1 connection when file, concept and rationale nodes are included.)
- **22 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `EditorState` connect `Editor State & Panels` to `Asset Meta: AssetMeta`, `Editor UI Panels`, `asset_registry.h`, `World`, `Math Library`, `string`, `Content Browser Assets`, `World Entities`, `Prefab Editor: PrefabWindow`, `UI Document Editor`, `Level Modes`, `Details Rows`, `Main: main()`, `Main: DebugFrameContext`, `Editor Ui: EditorCamera`, `Editor Settings: ProjectSettings`, `Editor Theme: EditorTheme`, `Rhi: MeshDraw`, `Main: draw_world()`, `Editor Settings: GameUserSettings`, `Particle Editor: ParticleEditor`, `graph_editor.cpp`, `assistant_lessons.cpp`, `Table Editor: TableWindow`, `Lua Debugger: lua_debugger.cpp`, `Mesh Editor: MeshWindow`, `draw_entity_details`, `assistant.cpp`?**
  _High betweenness centrality (0.102) - this node is a cross-community bridge._
- **Are the 5 inferred relationships involving `World` (e.g. with `apply_gun()` and `foliage_stamp()`) actually correct?**
  _`World` has 5 INFERRED edges - model-reasoned connections that need verification._
- **What connects `speed`, `entity`, `pose` to the rest of the system?**
  _3196 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `Play Session (Play mode)` be split into smaller, more focused modules?**
  _Cohesion score 0.013395892832857336 - nodes in this community are weakly interconnected._
- **Why does `PlaySession` connect `Play Session (Play mode)` to `Components: Character`, `Play Session: Moving`, `World`, `Math Library`, `Script System`, `Sound Player: SoundPlayer`, `World Entities`, `Data Tables`, `Input: InputSystem`, `Multiplayer NetSession`, `Main: main()`, `Physics: PhysicsWorld`, `play_session.h`, `Editor Settings: GameUserSettings`, `Voice Io: VoiceIO::Impl`, `Input: KeyMapping`, `Play Session: Agent`, `Net: NetSettings`, `Navigation: NavMesh`?**
  _High betweenness centrality (0.094) - this node is a cross-community bridge._
- **Should `Editor State & Panels` be split into smaller, more focused modules?**
  _Cohesion score 0.014705882352941176 - nodes in this community are weakly interconnected._

### Low-confidence Hints
_AMBIGUOUS edges — the extractor was unsure. Verify before acting on these._

- **What is the exact relationship between `lsd_core target` and `engine/core README (placeholder)`?**
  _Edge tagged AMBIGUOUS (relation: conceptually_related_to) - confidence is low._