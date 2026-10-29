# LSD_Engine

A data-oriented, editor-first 3D game engine by Kittakin Studios.

- **Core:** C++20, data-oriented design, generational handles, hashed IDs, per-system allocators
- **Gameplay:** LuaJIT and visual scripting, both generated from one reflection layer
- **Graphics:** one RHI with Direct3D 12, Vulkan and OpenGL 4.6 backends; NVIDIA, AMD and Intel GPUs
- **Scaling:** picks the best GPU automatically, sizes memory use to live VRAM and RAM budgets, scales jobs to CPU cores
- **Editor:** Unreal-style workflow — Content Browser, asset editors, Details panel, Play-in-Editor

The full plan (architecture, feature spec, editor layout, roadmap) lives in the
[engine plan doc](https://claude.ai/artifact/Q73b6jeNowQ4rQJGqRYwxW). A summary is in [`docs/`](docs/).

## Status

Milestone 0 — project skeleton:

- [x] CMake project with backend and plugin options
- [x] Foundation: types, logging, asserts, heap and arena allocators, generational handles, hashed IDs
- [x] Hardware report: CPU brand, cores, SIMD features, RAM
- [x] Adapter scoring, automatic GPU selection and quality-preset recommendation
- [x] Milestone 1 (part): job system, SDL3 window, D3D12 backend clearing the screen and drawing a triangle
- [ ] Milestone 1 (rest): reflection code generator, Vulkan and OpenGL backends

## Building

Requirements: CMake 3.24+, a C++20 compiler (MSVC 2022, Clang 16+ or GCC 13+), Ninja recommended.

```sh
# Windows (Visual Studio 2022 developer prompt)
cmake --preset windows-msvc
cmake --build --preset windows-msvc

# Linux
cmake --preset linux-clang
cmake --build --preset linux-clang

# Run tests and the hardware report
ctest --preset linux-clang
./build/linux-clang/runtime/lsd_runtime -rhi=vulkan
```

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
docs/           architecture, roadmap, clean-room rules
third_party/    vendored or fetched dependencies
```

## Clean-room rule

LSD_Engine is written from scratch. See [`docs/CLEAN_ROOM.md`](docs/CLEAN_ROOM.md) before contributing.

## License

Proprietary — Copyright (c) 2026 Kittakin Studios. All rights reserved. See [LICENSE](LICENSE).
Third-party components keep their own licences.
