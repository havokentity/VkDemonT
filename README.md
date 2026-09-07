# DeMonT Engine (VkDemonT)

[![Build](https://github.com/havokentity/vkdemont/actions/workflows/build.yml/badge.svg)](https://github.com/havokentity/vkdemont/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/23)
[![Shaders: Slang](https://img.shields.io/badge/shaders-Slang-2D75A0)](https://shader-slang.org/)
[![Vulkan](https://img.shields.io/badge/Vulkan-1.3-AC162C?logo=vulkan&logoColor=white)](https://vulkan.org/)

> The **Windows / Vulkan / NVIDIA RTX** home for the planetary + terrain
> features, forked from `demont-engine`. See [`HANDOFF.md`](HANDOFF.md)
> for why the split exists and what is planned next (smooth planet-scale
> terrain streaming via RTX Mega Geometry).

> A real-time game engine that **never rasterizes**. Every pixel is a
> ray. Path tracing is the renderer; mesh CSG via Manifold is the
> headline; modern temporal denoisers (the in-house SVGF chain, OptiX
> HDR / temporal on NVIDIA) keep the frame interactive at 1 sample per
> pixel.
>
> **DeMonT** = **De** **Mon**te Carlo-esque **T**racer — *-esque*
> because the algorithm will keep evolving (MIS, ReSTIR, bidirectional
> later) without ever falling back to a rasterizer.

## Supported platforms

| Platform | Backend | Denoiser | Status |
|---|---|---|---|
| **NVIDIA RTX** (Windows, RTX 2000-series and up) | Vulkan (`VK_KHR_ray_query` + `VK_KHR_acceleration_structure`) | SVGF (basic / à-trous) or OptiX HDR / temporal; NRD library queued | ✓ primary |
| Software fallback (Windows) | CPU reference path tracer (Embree-traced meshes + analytic primitives) | none | reference / bring-up |

> A Linux/Vulkan path is structured in the build (`linux-*` presets) but
> untested; Windows + NVIDIA RTX is the target. The macOS/Metal backend
> that the parent engine carries has been stripped from this fork.

Built on C++23 with a platform RHI (Software / native Vulkan), Slang
shaders that compile to SPIR-V, and a unified path-tracing kernel that
intersects analytic primitives **and** triangle meshes (via hardware ray
query) in one pass.

The full design lives in [`Raytracer Plan/`](Raytracer%20Plan/) — five
hand-authored HTML documents covering architecture principles, design,
phase-by-phase implementation plan, console protocol, and a modern-C++
cheatsheet — plus [`Raytracer Plan/FOLLOW_UPS.md`](Raytracer%20Plan/FOLLOW_UPS.md)
which tracks deferred work in detail.

## Why no rasterizer

Triangle rasterization is fundamentally a different lighting model than
path tracing — every special case (shadows, GI, reflections, refraction,
caustics, proper transparency) has to be re-implemented as a separate
hack on top. By committing to rays for every pixel, the engine collapses
those special cases into a single algorithm and frees us to focus
hardware budget on the things that *only* path tracing can do well:
millions of CSG-defined primitives, mirror-perfect reflections, real
glass, soft shadows from area lights — all without an `if-rasterizer`
switch in the codebase.

The cost is the noise floor and the GPU bill. Both are addressed by
modern temporal denoisers (the in-house SVGF chain, OptiX HDR / temporal
variants on NVIDIA) and by the path tracer's own accumulation when the
camera is stationary.

## Status

| Phase | Status |
|---|---|
| P0–P3 toolchain, RHI, Software backend | ✓ |
| P4 Native Vulkan (Windows) | ✓ |
| P5 FPS camera + analytic primitives | ✓ |
| P6 Materials (Lambert / metal / dielectric) | ✓ |
| P7 Path tracing + HDR accumulation + ACES tonemap | ✓ |
| P8 Triangle meshes + hardware ray query | ✓ |
| **P9 Mesh CSG via Manifold (headline)** | ✓ |
| Renderer unification (analytic + mesh in one shader) | ✓ |
| Vulkan denoiser: SVGF (basic / à-trous) + OptiX on NVIDIA | ✓ (`nrd` aliases the à-trous chain until the NRD library is wired) |
| P11 MIS, env maps, archived cvars, scene I/O | ✓ |
| P12 Windows bringup with native Vulkan | ✓ |

Post-P12 work — SDF raymarching phases 1–3 (#97–#99), the React scene
editor, the physics / destruction arcs, the planetary terrain + sky +
ocean, and the terrain-streaming plan in [`HANDOFF.md`](HANDOFF.md) — is
tracked in GitHub issues and the release tags rather than phase rows
here.

## Build

### Windows (NVIDIA RTX)

Requires:
- Windows 10/11 with current NVIDIA drivers (RTX 2000-series or newer for hardware ray-tracing)
- [Vulkan SDK 1.3.296+](https://vulkan.lunarg.com/sdk/home) — sets `VULKAN_SDK` env var. 1.4.x SDKs build and run cleanly; no upper bound pinned.
- CMake ≥ 3.27
- Ninja
- MSVC 2022 or MSVC 2026 (Build Tools or full IDE) — or clang-cl as an alternative. C++23 support required.

```pwsh
cmake --preset win-clang-release
cmake --build --preset win-clang-release
.\build\win-clang-release\src\app\demont.exe
```

(or `win-debug` / `win-release` for the MSVC toolchain, `win-clang-debug` for a clang-cl debug build.)

The Windows build:
- Auto-selects Vulkan as the default backend (`r_backend vulkan`)
- Uses native Vulkan (no MoltenVK portability extensions)
- Uses a native Win32 console overlay (GDI + monospace font)
- Picks a discrete GPU when both iGPU + dGPU are present
- Compiles every shader to SPIR-V via Slang's prebuilt Windows binary

`VK_KHR_acceleration_structure` + `VK_KHR_ray_query` are wired — the path tracer issues hardware-traversed ray queries from a compute kernel against BLAS/TLAS built per CSG/mesh bake. The dedicated `VK_KHR_ray_tracing_pipeline` (raygen / miss / hit shader binding tables) is queued; ray-query in compute already gives hardware traversal, so the extra pipeline pathway is only worth wiring when we need dynamic hit groups.

CMake auto-downloads Slang into `third_party/` on first configure.
Manifold, fmt, glm, glfw, mimalloc, enkiTS, civetweb, nlohmann_json,
tomlplusplus and (optionally) Tracy are vendored via FetchContent.

## Run

Default scene ships with three analytic spheres (red Lambert / gold
roughened metal / glass dielectric IOR 1.5), a ground plane, **and** a
CSG drilled cube (`box - sphere`) — all rendered in one pass.

- **WASD** + **Space/Ctrl** — fly camera
- **Right-click drag** — mouse-look (release to free the cursor)
- **Shift** — sprint
- **Backtick (`** `)`** — open the in-window console (native Win32
  overlay, monospace-rendered, attached to the GLFW window). Tab
  completes commands + cvar values; Up/Down for history; Esc to clear or
  close. Pasting text with newlines runs each line as its own command.

The web console is **not** auto-opened anymore (it kept stealing focus).
Type `web_console` in either console (or hit the URL directly:
<http://localhost:27960>) to launch it.

For shell scripting, the line protocol on `127.0.0.1:27961`:

```sh
echo "sys_info"           | nc localhost 27961
echo "r_denoiser svgf_atrous" | nc localhost 27961
echo "toggle r_denoiser"  | nc localhost 27961
echo "csg_dump"           | nc localhost 27961
```

## Notable cvars + commands

| | |
|---|---|
| `r_backend` | `none` / `software` / `vulkan` (default `vulkan`) |
| `r_denoiser` | `off` / `svgf_basic` / `svgf_atrous` / `nrd` / `optix_hdr` / `optix_hdr_aov` / `optix_temporal_hdr` / `optix_temporal_hdr_aov`. `nrd` aliases the à-trous chain until the NRD library is wired; the `optix_*` variants require a build with OptiX detected. `list_cvars r_denoiser` prints the full matrix; A/B with `toggle r_denoiser` |
| `r_render_scale` | internal render resolution as a fraction of the window (default `1.0`, clamped to `[0.25, 1.0]`). Below 1.0 the path tracer, its accumulator and every denoiser G-buffer run at the smaller extent and a resolve pass magnifies onto the swapchain. The resolve is a plain bilinear upscale — a placeholder for DLSS, not a quality feature, so expect softness. At `1.0` the renderer is byte-identical to having no render scaling at all |
| `r_camera_jitter` | deterministic Halton(2,3) sub-pixel camera jitter, one offset per frame shared by every ray (default `0` = off, per-ray random sampling as before). Exists so a temporal upscaler can be told the exact offset a frame was rendered with; `r_camera_jitter_period` sets the cycle length (default 16). Turning it on without a temporal reconstruction consuming it makes single frames aliased |
| `render_info` | print the internal vs presented extent, the effective render scale, and the sub-pixel jitter offset the last frame used |
| `r_spp` | samples per pixel per dispatch (1..32). Higher = cleaner motion at proportional GPU cost. |
| `r_max_bounces` | path-tracer bounce cap (default 8) |
| `csg_*` | live CSG editing — `csg_box`, `csg_sphere`, `csg_op subtract …`, `csg_set_root`, `csg_dump`, … |
| `prim_*` | analytic primitives — `prim_sphere`, `prim_plane`, `prim_remove`, `prim_list`, … |
| `screenshot <name> [accum\|denoise_color\|bloom_mip0\|swap\|depth\|motion]` | in-app GPU readback. Format comes from `r_capture_format` (`png`, default, or `ppm`) — the matching extension is auto-appended to `<name>`, overriding any extension you typed |
| `toggle <cvar>` | cycle a cvar's allowed_values — quick A/B for any boolean / enum |
| `web_console` | open the browser UI on demand |

## Architecture

```
src/
  core/     Log, Memory (tagged allocator + arenas + pool),
            Jobs (enkiTS), Hardware (worker-count sizing)
  rhi/      Render Hardware Interface — Device / CommandBuffer /
            Handles / Resources / Denoise. Backend-agnostic.
  rhi_software/  CPU reference path tracer — Embree-traced meshes +
                 analytic primitives, Lambert direct lighting.
  rhi_vulkan/    Native Vulkan backend (Windows), SVGF + OptiX
                 denoisers, hardware ray query.
  renderer/      Camera, Csg/CsgScene (Manifold-backed),
                 AnalyticBvh (incl. SDF clusters), LightTree,
                 GltfImporter, HdrImage, Astronomy/BscCatalog.
  physics/       PhysicsSystem (Verlet + rigid body), SmokeSPH,
                 OceanFFT.
  destruction/   VoxelGrid + Voxelizer (CSG mesh → voxel box pile).
  effects/       ParticleSystem.
  audio/         AudioSystem.
  editor/        EditorRoutes (panel HTTP/WS routes) + SceneGraph.
  console/       CVar/Command registry + civetweb WS/HTTP server +
                 raw line-protocol TCP server.
  app/           GLFW window, native Win32 console + perf overlays.
  engine/        Top-level orchestrator + the unified path-trace pass.
shaders/   PathTrace.slang megakernel + 28 more .slang kernels
           (SVGF denoise chain, ReSTIR, bloom, clouds, SDF libraries,
           tonemap / post).
web/       Vanilla-JS console UI at the top level (no build step) +
           editor/ — React 18 + Vite multi-panel scene editor whose
           npm build is embedded into the binary by cmake/Editor.cmake.
cmake/     SetupBinaries, Slang.cmake (compile_slang),
           EmbedResource.cmake, Dependencies.cmake, Editor.cmake
```

## License

MIT. See [`LICENSE`](LICENSE).

The plan documents in `Raytracer Plan/` are © Rajesh D'Monte;
everything in `src/`, `shaders/`, `web/`, `cmake/` is the
implementation against that plan.
