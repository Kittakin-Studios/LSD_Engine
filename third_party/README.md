# third_party

Fetched by `CMakeLists.txt` at configure time, pinned to a tag. Every library is permissive
(MIT, BSD, zlib, Apache-2.0, or public domain); none is GPL or otherwise copyleft. FreeType is
dual-licensed (FreeType Licence / GPL-2.0) and we build and distribute it under the permissive
FreeType Licence. Full license texts are shipped in `THIRD-PARTY-NOTICES.md`.

| Library | Version | Licence | Used for |
| --- | --- | --- | --- |
| SDL3 | release-3.4.18 | zlib | window, input, gamepads |
| Dear ImGui | v1.93.0-docking | MIT | debug UI, editor v1 |
| Jolt Physics | v5.6.0 | MIT | rigid bodies, collision, character controller |
| ImGuizmo | `105e4fde` | MIT | viewport move/rotate/scale gizmos |
| Vulkan-Headers | vulkan-sdk-1.4.363.0 | Apache-2.0 / MIT | Vulkan API headers |
| volk | vulkan-sdk-1.4.363.0 | MIT | Vulkan function loader |
| ufbx | v0.23.1 | MIT / public domain | FBX import |
| cgltf | v1.15 | MIT | glTF 2.0 import |
| meshoptimizer | v1.3 | MIT | mesh indexing and optimisation |
| LuaJIT | `8ca76297` (v2.1) | MIT | gameplay scripting |
| miniaudio | 0.11.25 | MIT-0 / public domain | audio device, decoding, mixing, 3D |
| stb (stb_image) | `003501f6` | public domain / MIT | texture decoding (PNG, JPG, HDR, ...) |
| ozz-animation | 0.17.0 | MIT | skeletal animation, compression, IK |
| Recast & Detour | v1.6.0 | zlib | navmesh, path finding, crowd steering |
| FreeType | VER-2-13-3 | FreeType Licence (dual: GPL-2.0) | font rasterising for RmlUi |
| RmlUi | 6.0 | MIT | game UI: HTML / CSS documents |
| QuickJS-ng | v0.16.2 | MIT | JavaScript for UI documents' scripts |
| ENet | v1.3.18 | MIT | multiplayer: reliable UDP (engine/net) |
| miniupnpc | 2.2.8 | BSD-3-Clause | multiplayer: opens the host's router port (UPnP) and reads its public address (engine/net) |
| Opus | v1.5.2 | BSD-3-Clause | voice chat codec (engine/net) |
| Tracy | v0.11.1 | BSD-3-Clause | CPU profiler client, only with -DLSD_TRACY=ON |
