# Graph Report - lsd_docs  (2026-10-31)

## Corpus Check
- Corpus is ~19,240 words - fits in a single context window. You may not need a graph.

## Summary
- 251 nodes · 382 edges · 11 communities (10 shown, 1 thin omitted)
- Extraction: 80% EXTRACTED · 20% INFERRED · 0% AMBIGUOUS · INFERRED: 75 edges (avg confidence: 0.84)
- Token cost: 272,690 input · 0 output

## Community Hubs (Navigation)
- Engine Architecture Modules
- Backlog Feature Set
- Manual Editor Workflows
- Components and Gameplay Reference
- Project Identity and Rules
- Roadmap and Milestones
- Libraries, Licences and UI
- Worked Examples
- Reflection and Rendering Core
- Play, Undo and Recovery
- Particle Editor

## God Nodes (most connected - your core abstractions)
1. `Architecture doc (How LSD_Engine works)` - 50 edges
2. `Milestone 6: Vertical slice` - 20 edges
3. `CLAUDE.md briefing` - 14 edges
4. `third_party README` - 13 edges
5. `LSD_Engine Examples` - 12 edges
6. `Milestone 5: Content editors` - 11 edges
7. `Milestone 4: Gameplay layer` - 9 edges
8. `rhi::Device` - 9 edges
9. `TypeInfo` - 8 edges
10. `PlaySession` - 8 edges

## Surprising Connections (you probably didn't know these)
- `EditorTheme (editor_theme.json)` --semantically_similar_to--> `Editor look / theme colours`  [INFERRED] [semantically similar]
  editor/README.md → CLAUDE.md
- `third_party README` --references--> `Proprietary licence and third-party licence policy`  [INFERRED]
  third_party/README.md → CLAUDE.md
- `LSD_Engine` --conceptually_related_to--> `Layered static-library modules (lsd_<name>)`  [INFERRED]
  CLAUDE.md → docs/ARCHITECTURE.md
- `EditorTheme (editor_theme.json)` --references--> `TypeInfo`  [INFERRED]
  editor/README.md → docs/ARCHITECTURE.md
- `Render layer (mesh data, built-in shapes, grid)` --references--> `rhi::Device`  [INFERRED]
  engine/render/README.md → docs/ARCHITECTURE.md

## Hyperedges (group relationships)
- **Node graph editors (material, visual script, sound cue, behavior tree)** — docs_manual_material_graph, docs_manual_visual_scripting, docs_manual_sound_cue, docs_manual_behavior_tree [INFERRED 0.85]
- **AI stack: navmesh, behavior tree, perception** — docs_manual_navmesh, docs_manual_behavior_tree, docs_manual_perception [EXTRACTED 1.00]
- **Gameplay scripting layers compiling to or calling Lua** — docs_manual_lua_scripting, docs_manual_visual_scripting, docs_manual_script_node, docs_manual_ui_document_script, docs_manual_level_script [INFERRED 0.85]
- **Milestone sequence 0-6 with exit tests** — docs_roadmap_milestone_0_skeleton, docs_backlog_milestone_1_core_reflection, docs_backlog_milestone_2_assets_content_browser, docs_backlog_milestone_3_editor_shell, docs_backlog_milestone_4_gameplay_layer, docs_backlog_milestone_5_content_editors, docs_backlog_milestone_6_vertical_slice [EXTRACTED 1.00]
- **Node-graph editors sharing the graph canvas (visual scripts, materials, behaviour trees)** — docs_backlog_visual_scripting, docs_backlog_material_graph, docs_backlog_behaviour_trees, docs_backlog_audio_miniaudio [INFERRED 0.85]
- **Clean-room reference material (ideas only)** — docs_clean_room_bitsquid_blog, docs_clean_room_niklas_gray_youtube, docs_clean_room_data_oriented_design, docs_clean_room_game_engine_architecture [EXTRACTED 1.00]
- **Three RHI backends behind rhi::Device** — engine_rhi_backends_d3d12_readme_backend, engine_rhi_backends_opengl_readme_backend, engine_rhi_backends_vulkan_readme_backend, docs_architecture_rhi_device [INFERRED 0.95]
- **Four node libraries sharing script::Graph** — docs_architecture_script_graph, docs_architecture_visual_script_to_lua, docs_architecture_material_graph, docs_architecture_sound_cue, docs_architecture_behavior_tree [EXTRACTED 1.00]
- **Features generated from reflection TypeInfo** — docs_architecture_typeinfo, docs_architecture_debug_ui_details, docs_architecture_json_serialization, docs_architecture_world, docs_architecture_lsd_lua_api [EXTRACTED 1.00]

## Communities (11 total, 1 thin omitted)

### Community 0 - "Engine Architecture Modules"
Cohesion: 0.06
Nodes (51): Animation (ozz: SkinnedModel, Clip, Pose, AnimGraph), AssetRegistry, audio::AudioSystem (miniaudio), ai::BehaviorTree / BtRunner / Blackboard, core module (jobs, console, crash), Crash reporting (minidump + log), debug_ui Details panel, Architecture doc (How LSD_Engine works) (+43 more)

### Community 1 - "Backlog Feature Set"
Cohesion: 0.08
Nodes (40): Animation graph (states, blend spaces, transitions), Animation (ozz-animation, GPU skinning), Asset registry (GUIDs in .meta sidecars), Asset validation before packaging, Audio (miniaudio): Sound Cues, Classes, Mixes, Submixes, Behaviour trees + blackboard (.lsdbt), Character component (Jolt CharacterVirtual), Content Browser (+32 more)

### Community 2 - "Manual Editor Workflows"
Cohesion: 0.07
Nodes (32): ARCHITECTURE.md, EXAMPLES.md, LSD_Engine Manual, Animation Graph (states, blend spaces, transitions), Audio (Sounds, Classes, Mixes, Attenuation, Concurrency), Build Lighting (Light Probes), Character Component, Command Line Flags (+24 more)

### Community 3 - "Components and Gameplay Reference"
Cohesion: 0.08
Nodes (28): Behavior Tree and Blackboard, Components Reference, Data Table (.csv), Details Panel, Exposed Script Properties, Game Mode, Game UI (RmlUi documents), Jolt Physics (Rigid Body, Physics Joint) (+20 more)

### Community 4 - "Project Identity and Rules"
Cohesion: 0.10
Nodes (21): Bitsquid / Stingray (public ideas), Charlize Pauli, Editor command-line flags (-rhi, -frames, -screenshot, -smoketest, -remote...), CLAUDE.md briefing, Editor look / theme colours, Engine plan doc (claude.ai artifact), Kittakin Studios, lsd_editor.exe (editor and game) (+13 more)

### Community 5 - "Roadmap and Milestones"
Cohesion: 0.11
Nodes (15): Autodesk Stingray (public feature list, ideas only), Data compiler (Content -> DerivedData, incremental), Engine plan doc (claude.ai artifact), Live Link folder (Blender/Maya re-export), Milestone 3: Editor shell, Play-in-Editor (PIE), Prefabs (.lsdprefab) with per-instance overrides, Transactions: undo/redo and Undo History (+7 more)

### Community 6 - "Libraries, Licences and UI"
Cohesion: 0.14
Nodes (17): Auto LODs (meshoptimizer simplify), Frustum culling, Importers (ufbx, cgltf, stb_image, miniaudio, CSV), UiContext (RmlUi), ui README, Game UI on RmlUi (lsd::ui::init, .rml/.rcss documents), cgltf, third_party README (+9 more)

### Community 7 - "Worked Examples"
Cohesion: 0.20
Nodes (12): particles::ParticleSystem / ParticleInstance, Prefab (.lsdprefab) with per-field overrides, Example 6: campfire (particles, sound, light), Example 2: coins, score HUD, save game, LSD_Engine Examples, Example 7: door with a visual script, Game Mode data asset, Example 3: glowing lava material (+4 more)

### Community 8 - "Reflection and Rendering Core"
Cohesion: 0.33
Nodes (7): Details panel generated from reflection, Milestone 1: Core + reflection, Post Process Volume (SSAO, TAA, DOF, bloom, ACES, FXAA, SSR), Reflection code generator (LSD_STRUCT / LSD_PROPERTY), RHI backends (D3D12, Vulkan 1.3, OpenGL 4.6), Shadows: cascaded sun + spot/point atlas, VRAM-budget scaling (-vram)

### Community 9 - "Play, Undo and Recovery"
Cohesion: 0.50
Nodes (4): Crash Reports (minidump + log), Keep Simulation Changes, Play in Editor, Undo, Autosave and Crash Recovery

## Knowledge Gaps
- **53 isolated node(s):** `ARCHITECTURE.md`, `EXAMPLES.md`, `Live Link Folder`, `Reference Viewer and Replace References`, `Validate Assets` (+48 more)
  These have ≤1 connection - possible missing edges or undocumented components. (Counts symbols only; 56 node(s) total have ≤1 connection when file, concept and rationale nodes are included.)
- **1 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `Architecture doc (How LSD_Engine works)` connect `Engine Architecture Modules` to `Project Identity and Rules`, `Libraries, Licences and UI`, `Worked Examples`?**
  _High betweenness centrality (0.143) - this node is a cross-community bridge._
- **What connects `ARCHITECTURE.md`, `EXAMPLES.md`, `Live Link Folder` to the rest of the system?**
  _53 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `Engine Architecture Modules` be split into smaller, more focused modules?**
  _Cohesion score 0.06038961038961039 - nodes in this community are weakly interconnected._
- **Why does `Milestone 6: Vertical slice` connect `Backlog Feature Set` to `Reflection and Rendering Core`, `Roadmap and Milestones`?**
  _High betweenness centrality (0.033) - this node is a cross-community bridge._
- **Should `Backlog Feature Set` be split into smaller, more focused modules?**
  _Cohesion score 0.07948717948717948 - nodes in this community are weakly interconnected._
- **Why does `CLAUDE.md briefing` connect `Project Identity and Rules` to `Libraries, Licences and UI`, `Worked Examples`?**
  _High betweenness centrality (0.031) - this node is a cross-community bridge._
- **Should `Manual Editor Workflows` be split into smaller, more focused modules?**
  _Cohesion score 0.06628787878787878 - nodes in this community are weakly interconnected._