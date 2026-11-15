# LSD_Engine

![Build and test](https://github.com/Kittakin-Studios/LSD_Engine/actions/workflows/build.yml/badge.svg)
![Status: Work in progress](https://img.shields.io/badge/status-work%20in%20progress-orange)
![Version: early alpha](https://img.shields.io/badge/version-early%20alpha-yellow)
![License: MIT OR CC0-1.0](https://img.shields.io/badge/license-MIT%20OR%20CC0--1.0-blue)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus)
![Platforms: Windows | Linux](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux-lightgrey)
![Graphics: D3D12 | Vulkan | OpenGL](https://img.shields.io/badge/graphics-D3D12%20%7C%20Vulkan%20%7C%20OpenGL-red)

A data-oriented, editor-first 3D game engine by [Kittakin Studios](https://github.com/Kittakin-Studios).

![The LSD_Engine editor](docs/images/editor_overview.jpg)
*The editor: menu bar, Play / Simulate, workspace tabs, viewport, Outliner, Details and the Content Browser.*

> [!WARNING]
> **LSD_Engine is new and still a work in progress.**
> The engine was started in 2026 and is in early alpha. All the milestones below run, but many features are first versions that still need testing, polish and optimisation.
>
> - **Expect bugs and crashes.** Save often and keep your projects in version control.
> - **Things will change.** APIs, file formats and editor layouts can still change between versions. Projects made now may need updating later.
> - **Not production-ready yet.** It's great for learning, prototypes and small games. For a commercial release, test carefully on your target hardware first.
> - **Documentation is still growing.** Some pages may be missing or out of date.
>
> Bug reports, feedback and contributions are very welcome.

## Download

Get the latest Windows build from [Releases](https://github.com/Kittakin-Studios/LSD_Engine/releases/latest):

- **`LSD_Engine_Setup.exe`**: the installer. It installs for your user only (no administrator rights) into
  `%LOCALAPPDATA%\Programs\LSD_Engine`, adds a Start Menu (and optional desktop) shortcut and an uninstaller.
- **`LSD_Engine_Windows.zip`**: the portable version. Unzip it anywhere you can write to and double-click `lsd_editor.exe`.

Both hold the editor (also the game runtime), the sample project (Coin Run and the Quaternius character) and the docs.
The editor keeps its project in its own folder (`Content/`, `Config/`, and the `Saved/` / `DerivedData/` it writes), so do not unzip into `Program Files`.

System requirements: Windows 10 or 11, 64-bit; a graphics card with Direct3D 12, Vulkan 1.3 or OpenGL 4.6.
For C / C++ game code you also need Visual Studio 2022 or newer with *Desktop development with C++*.
Linux users build from source (see [Building](#building)).

## What is LSD_Engine?

LSD_Engine is a complete engine for building 3D (and 2D) games, written from scratch in C++20. It gives you an editor where you build levels, import and edit assets, and write gameplay, plus a runtime that packages and runs the finished game on Windows and Linux.

If you have used Unreal Engine, the workflow will feel familiar. You place objects in a viewport, select them in the Outliner, edit their properties in the Details panel, manage assets in the Content Browser, and press **Play** to test the game inside the editor. The engine itself is built differently from Unreal: it is lightweight, data-oriented and designed to scale to whatever hardware it runs on.

### What you can do with it

- **Build worlds.** Place, move and edit objects with gizmos and full undo. Make prefabs and stream sub-levels in and out. Sculpt landscapes, paint foliage and model blockouts in the editor.
- **Write gameplay in the language you prefer.** Use Lua (LuaJIT), visual scripting, C/C++ or JavaScript. Visual scripts compile to Lua, and all of them share the same `lsd` API.
- **Add physics, characters and vehicles.** Jolt physics, a ready-made character controller, vehicles, transports, moving platforms and mesh fracture.
- **Create content inside the editor.** Material graphs, animation graphs (including montages, root motion and motion matching), audio and particle editors. Particles support mesh and ribbon types.
- **Make it look good.** PBR, shadows, baked lightmaps and light probes, reflection captures, and Unreal-style post-processing (SSAO, TAA, bloom, depth of field, a Color Grading LUT, motion blur, contact shadows). CPU and GPU culling keep scenes fast.
- **Add game systems.** AI (navigation, behaviour trees), game UI and HUDs in HTML / CSS, input actions, save games, localisation and profiling are built in.
- **Go online with one checkbox.** Built-in multiplayer with text and voice chat, and playtests by join code.
- **Make 2D games too.** Paper2D-style sprites, flipbooks and tile maps.
- **Ship it.** Cook and package optimised Shipping builds.
- **Get help while you work.** LSD Assistant is an optional in-editor AI helper that uses your own model, writes and tests code, and comes with lessons.

### Screenshots

| | |
| --- | --- |
| [![Time of day](docs/images/gfx_time_of_day.jpg)](docs/GRAPHICS.md) <br> *Sky, sun and reflections at two times of day* | [![Rect lights](docs/images/gfx_rect_lights.jpg)](docs/GRAPHICS.md) <br> *Rect and Disc lights* |
| [![Particle editor](docs/images/ed_particles.jpg)](docs/MANUAL.md) <br> *Particle editor with live preview* | [![Material graph](docs/images/ed_material_graph.jpg)](docs/MANUAL.md) <br> *Material Graph with live preview* |
| [![Visual script](docs/images/ed_visual_script.jpg)](docs/MANUAL.md) <br> *Visual scripting, compiled to Lua* | [![2D mode](docs/images/template_2d.jpg)](docs/MANUAL.md) <br> *2D mode: tile maps, sprites, flipbooks* |
| [![3D modeling](docs/images/mode_modeling_edit.jpg)](docs/MANUAL.md) <br> *3D Modeling mode: extrude, bevel, inset* | [![Multiplayer workspace](docs/images/mp_workspace.jpg)](docs/Multiplayer.md) <br> *Multiplayer workspace: host, join codes* |

### How it works under the hood

- **Core:** C++20 with data-oriented design. Entities are generational handles, components are reflected plain structs in packed pools, and systems loop over arrays. Text source assets and hot reload everywhere.
- **Reflection:** one `LSD_STRUCT` / `LSD_PROPERTY` layer and a code generator give the Details panel, JSON save / load, undo, Lua access and Play copies for free.
- **Graphics:** one rendering interface (RHI) with Direct3D 12 (default), Vulkan 1.3 and OpenGL 4.6 backends. It runs on NVIDIA, AMD and Intel GPUs.
- **Scaling:** the engine picks the best GPU automatically, sizes memory use to the VRAM and RAM actually available, and spreads work across all CPU cores.

The ideas come from the public Bitsquid / Stingray talks, implemented from scratch under a [clean-room rule](docs/CLEAN_ROOM.md).

## Status

**Early alpha, work in progress.** The core engine and editor are in place. The current focus is stability, performance, documentation and polishing the features below. CI builds and runs the 15 CTest tests on Windows (MSVC) and Linux (Clang) on every push, and both compilers build with 0 warnings.

| # | Milestone | State |
| --- | --- | --- |
| 0-2 | Core, RHI, reflection, assets, Content Browser | Done |
| 3 | Editor shell: viewport, gizmos, Outliner, Details, undo, Play-in-Editor | Done |
| 4 | Gameplay: prefabs, Lua + visual scripting, input, Jolt physics, character | Done |
| 5 | Content editors: materials, animation, audio, particles | Done |
| 6 | Vertical slice: packaging, save games, AI, game UI, localisation, profiling | Done |
| 7 | Gameplay languages: C / C++ game code, JavaScript scripts | Done |

"Done" means a milestone's features work. It does not mean they are finished: some items are partial, and each one will keep improving as the engine grows. Every feature by milestone, ticked when done, is in [docs/BACKLOG.md](docs/BACKLOG.md); plans are in [docs/ROADMAP.md](docs/ROADMAP.md).

## Documentation

| Document | What is in it |
| --- | --- |
| [Examples (first timers)](docs/Examples/Examples.md) | Start here: Input Actions (move, look, jump, crouch, sprint, swim, interact), a health and stamina HUD, draining each, a door, step by step |
| [MANUAL](docs/MANUAL.md) | How to use the engine, panel by panel, with pictures |
| [EXAMPLES](docs/EXAMPLES.md) | Step-by-step tutorials (a first level, a HUD, a guard AI, a menu, a campfire, shipping) |
| [How to code in LSD_Engine](docs/How%20to%20code%20in%20LSD_Engine.md) | Lua and visual scripts from scratch |
| [CODING](docs/CODING.md) | C / C++ gameplay and every command |
| [GRAPHICS](docs/GRAPHICS.md) | Step by step to high-end graphics |
| [Multiplayer](docs/Multiplayer.md) | Playtests: join codes, Tailscale |
| [ARCHITECTURE](docs/ARCHITECTURE.md) | How the engine works inside |

## Building

Requirements: CMake 3.24+, a C++20 compiler (MSVC 2022, Clang 16+ or GCC 13+). Ninja is recommended.

```sh
# Windows (Visual Studio 2022 developer prompt; CI uses windows-2022)
cmake --preset windows-msvc
cmake --build --preset windows-msvc
cmake --build --preset windows-msvc --config Release   # also copies lsd_editor.exe to the project root

# Linux (Ubuntu / Debian; verified on Ubuntu 26.04 with Clang 21). The system packages SDL3 needs:
sudo apt install clang lld cmake ninja-build pkg-config libx11-dev libxext-dev libxrandr-dev \
  libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev libxkbcommon-dev libwayland-dev \
  wayland-protocols libegl1-mesa-dev libgl1-mesa-dev libdrm-dev libgbm-dev libasound2-dev \
  libpulse-dev libudev-dev libdbus-1-dev libvulkan-dev
cmake --preset linux-clang
cmake --build --preset linux-clang

# Run tests and the hardware report
ctest --preset linux-clang
./build/linux-clang/runtime/lsd_runtime -rhi=vulkan
```

To make the Windows downloads yourself: `packaging/make_portable.ps1` builds the portable zip, and `packaging/LSD_Engine.iss` (Inno Setup 6) the installer. Pushing a `v*` tag runs both on GitHub and publishes a release.

### Build options

| Option | Default | Meaning |
| --- | --- | --- |
| `LSD_RHI_D3D12` | ON on Windows | Direct3D 12 backend |
| `LSD_RHI_VULKAN` | ON | Vulkan backend |
| `LSD_RHI_OPENGL` | ON | OpenGL 4.6 fallback backend |
| `LSD_COMPUTE_CUDA` | OFF | Optional CUDA plugin for editor/offline tools |
| `LSD_COMPUTE_HIP` | OFF | Optional ROCm/HIP plugin for editor/offline tools |
| `LSD_BUILD_EDITOR` | ON | Build the editor |
| `LSD_BUILD_TOOLS` | ON | Build the data compiler |
| `LSD_BUILD_TESTS` | ON | Build unit tests |

### Runtime flags

- `-rhi=d3d12|vulkan|opengl` — force a graphics backend
- `-gpu=<index>` — force a GPU instead of automatic selection

## Repository layout

```
engine/
  foundation/   types, memory, logging, handles, hashing, system info
  rhi/          graphics interface + backends (d3d12, vulkan, opengl)
  core/ world/ script/ render/ audio/ physics/ ui/   (milestones 1-6)
editor/         the editor application
tools/          data compiler and other command-line tools
runtime/        game executable
tests/          unit tests
packaging/      Windows installer and portable zip
docs/           manual, tutorials, architecture, roadmap, clean-room rules
third_party/    vendored or fetched dependencies
```

## Contributing

Issues and pull requests are welcome. Before you send code:

- Read [`docs/CLEAN_ROOM.md`](docs/CLEAN_ROOM.md): ideas from public talks and papers only, never code or data from other engines.
- Keep both compilers at 0 warnings (MSVC `/W4`, Clang `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`) and `ctest` passing.
- New dependencies must be permissive (MIT, BSD, zlib, Apache-2.0, public domain), pinned in `third_party/CMakeLists.txt`
  and listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
- Sample content in `Content/` must be your own work or CC0. The sample character, its animations and textures (`Content/Character/Quaternius/`) are by [Quaternius](https://quaternius.com), CC0 ([LICENSE.txt](Content/Character/Quaternius/LICENSE.txt)).

## License

LSD_Engine is free and open source. Copyright (c) 2026 Charlize Pauli (Kittakin Studios).

You may use it under **either** of these licenses, whichever you prefer:

- **MIT License** — see [`LICENSE-MIT`](LICENSE-MIT)
- **CC0 1.0 Universal** (public domain dedication) — see [`LICENSE-CC0`](LICENSE-CC0)

In short, you can use and modify the engine for free, and sell and ship the games you make with it, with no fees or royalties.

SPDX identifier: `MIT OR CC0-1.0`

### Third-party components

Third-party components are not covered by the licenses above and keep their own licenses. The full list of dependencies, versions and licenses is in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). Ship it (or its FreeType credit line) with the engine, and keep FreeType built under its permissive FreeType License.
