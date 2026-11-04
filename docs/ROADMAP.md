# Roadmap

Editor-first order. Each milestone ends with something you can run (its exit test).
Full detail: [BACKLOG.md](BACKLOG.md) (the feature checklist) and [ARCHITECTURE.md](ARCHITECTURE.md).

| # | Milestone | Builds | Exit test |
| --- | --- | --- | --- |
| 0 | Skeleton | CMake, foundation basics, hardware report | Builds on Windows and Linux; tests pass |
| 1 | Core + reflection | Job system, window, RHI backends (D3D12, Vulkan, OpenGL), reflection codegen, serialization | A reflected object saves, loads and shows its properties |
| 2 | Assets + browser | Asset registry, importers, data compiler, thumbnails, Content Browser | Import a Blender FBX and find it by search |
| 3 | Editor shell | Viewport with grid and gizmos, Outliner, Details, undo, Play-in-Editor | Block out a level and play it |
| 4 | Gameplay layer | Prefabs, components, Lua + visual scripting, input, physics, movement | A character runs, input wired with no code |
| 5 | Content editors | Material and animation graphs, audio assets and mixes, particle editor | Audio worked example works end to end |
| 6 | Vertical slice | Lighting, post, UI designer, navmesh, behaviour trees, save game, packaging | Packaged level runs on another PC |
| 7 | Gameplay languages | C / C++ game module (C API, C++ wrappers, Native Script, own components, hot reload), JavaScript gameplay scripts | A door opens from a C++ behaviour, a pickup from a JavaScript one, in the packaged game |
