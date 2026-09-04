# VkDemonT — Windows/Vulkan Planetary Path-Tracing Engine

This repo was **forked from `demont-engine`** (a C++23 real-time path tracer that never rasterizes) to become the **Windows-specific, Vulkan** home for the **planetary + terrain** features. The parent engine keeps the game-like path tracer (spheres/CSG/meshes, physics, editor, classic procedural sky + stars + time-of-day) and had its planetary features removed. This repo keeps *everything* — it is a full clone of the parent at the split point.

## Why the split

Smooth planet-scale terrain streaming (fly in/out of orbit like MSFS / Star Citizen / Google Earth) needs **NVIDIA RTX Mega Geometry** — continuous cluster LOD + on-demand RAM→VRAM streaming via `VK_NV_cluster_acceleration_structure`. That is **Vulkan/NVIDIA/Windows-only**; it does not exist on Metal/Mac. The parent engine targets Mac (Metal), so the planetary path was split here to run on Windows/Vulkan/RTX.

## Current state (main @ the split)

`main` is the full planetary engine as it stood in the parent — real physics throughout, metric units (1 unit = 1 m), cited constants:

- **Terrain**: cubed-sphere quadtree, ETOPO 2022 DEM (`assets/planet/earth_lite.ptdem`, PTDEM002 with a relief plane), fractal continuation broken at the 106 m hillslope scale, per-scale angle-of-repose slope cap, real MODIS land cover, chunk BLAS streamed into the ray query, retire-on-cover + whole-cut retention residency, from-orbit sub-pixel cull.
- **Sky/atmosphere**: Hillaire 2020 physical marched atmosphere (transmittance + multi-scatter LUTs), Bodhaine 1999 Rayleigh, ozone/Chappuis, Kopp & Lean solar irradiance, physical stars extinguished by the air column (Beer–Lambert to space), real default sky (no skybox), black space.
- **Ocean**: Tessendorf FFT cascades, Cox–Munk slope→GGX, Pope & Fry / Morel / Petzold water optics, water-leaving radiance from outside the medium.
- **Renderer**: megakernel path tracer, ReSTIR/NEE, in-house SVGF denoiser (+ OptiX HDR/temporal on NVIDIA), per-pass GPU timing (perf overlay tier 4). (The MetalFX denoiser finalizer went with the Metal backend — see the strip note below.)

## Pending branches (unfinished polish, reviewed, NOT merged)

Each was adversarially reviewed and had its over-claims corrected; each needs a live look on real hardware where noted:

- `feat/sun-lensflare` — physical Hullin lens flare enabled from orbit. **Critical fp32 fix landed** (the vis-gate overflowed to NaN → black frame; now the overflow-safe reciprocal form, locked by a CI unit test built without `-ffast-math`). Occlusion is a real ray-vs-body test; flare carries the sun's chromaticity. **Flare is swapchain-only → verify visually on a real GPU.**
- `fix/stars-in-space` — stars visible from orbit (Beer–Lambert extinction, not a sun-below-horizon flag) + daytime bright-star drown via Rose (1948) contrast. `K=8` is a documented veiling factor (see the filed follow-up to retire it via physical star radiometry). No goldens move.
- `fix/clouds-transition` — cloud march step derived from the field not the ray span (kills fly-through shimmer). Moves one golden (`clouds_raymarched`, a 27% horizon de-aliasing toward the converged reference). **The deck-entry brightening is physical and persists — that is weather, not the fix.**
- `fix/distant-water-magenta` — MetalFX-only demod overshoot on near-black albedo (floor the demod guide, empirical, with a data table + the repo's first MetalFX golden cell). **Metal-specific; re-evaluate for the Vulkan/DXR denoiser path here.**
- `fix/terrain-test-split` — CI: split `pt_planet_terrain` into a fast per-PR core + a nightly exhaustive sweep (the exhaustive sweep was timing out at 600 s; core is ~70 s on Windows).

## The headline next task: smooth terrain streaming

The parent's diagnosis (design issue **demont-engine#339**): the architecture (cubed-sphere quadtree + screen-space-error LOD + per-chunk BLAS) is correct, but it lacks **streaming discipline**. Symptoms a user hit flying in/out:
1. A **second low-res Earth** = the analytic backstop sphere (27.7 km below the surface, a hidden gap-filler) showing through because streamed terrain doesn't cover the view.
2. **Bad in/out transitions** and **terrain not loading as you move** = no full-frustum coverage guarantee and no predictive prefetch; fast motion outruns the `blas_budget_ms`-paced bakes.
3. **Clouds render below terrain** = cloud altitude is measured from the sea-level sphere (`ptAltitudeAboveSphere`), so 200 m cumulus renders inside any mountain > 200 m. Needs terrain-relative cloud base and/or terrain occlusion of the cloud march.

Plan (path-tracer-appropriate — no per-frame BLAS rebuild, so no vertex geomorphing; this is where **RTX Mega Geometry cluster LOD** is the Windows/Vulkan upgrade the split unlocks):
- **P1** — full-frustum coverage + predictive prefetch (extrapolate camera velocity/look, prioritize by screen-space error, never show the backstop at normal speed; degrade to a coarser *covering* LOD instead). Keep residency a pure function of the inputs (no wall-clock).
- **P2** — make the backstop a coarse but correct-height terrain skirt, not a 27.7 km-low sphere.
- **P3** — verify sub-pixel LOD swaps; add a cheap dual-resident cross-fade if pops remain (per-chunk opacity, no BLAS rebuild).
- **On Windows/Vulkan**: adopt **RTX Mega Geometry** (`VK_NV_cluster_acceleration_structure`, ref: `nvpro-samples/vk_lod_clusters`) for continuous cluster LOD + streaming — the technique this whole repo exists to enable.

## First steps on Windows

1. ~~Build with the Vulkan preset (`win-clang-release` / native Vulkan; drop the Metal/`rhi_metal` backend and Mac presets — "strip all Mac support").~~ **DONE** — see the strip note below; the tree configures, compiles, and links Vulkan-only on this box.
2. Verify the Vulkan backend renders correctly on a real NVIDIA GPU (it was only exercised via MoltenVK on Mac before, which had latent regressions — pixel correctness on native Vulkan is unverified). **← next; needs a live look.**
3. Merge the pending polish branches after a live check.
4. Start P1 of the streaming plan.

### Mac-strip status (branch `chore/strip-mac-vulkan-only`)

All macOS/Metal *support* has been removed and the Vulkan-only tree
builds clean (`demont.exe` + all targets, `win-clang-release`):

- Deleted `src/rhi_metal/`, the 4 Cocoa `.mm` files, the Metal ocean GPU
  test, the Mac CMake presets, and the metal-cpp / MSL toolchain wiring.
- `BackendType::Metal` removed; `r_backend` defaults to `vulkan` and its
  allowed set is `none|software|vulkan` (a retired `r_backend metal` from
  an old macOS cfg is normalized to `vulkan` at boot).
- The Metal denoiser-selection branches and every `#if defined(__APPLE__)`
  block in the kept sources were removed; `OceanGpuActive()` (Metal-only)
  now returns via the software-exclusion guard, behavior-identical.

Also done since: the macOS CI jobs were removed from all three workflows
(the nightly release now builds/packages on Windows), the
`tests/goldens/Darwin/` tree (79 PNGs / 14 MB) was deleted, and all 55
`--backend metal` golden cells + the two `if(APPLE)` manual metal blocks
were stripped from `tests/CMakeLists.txt` (configure clean, 91 tests, 0
metal cells).

Deliberately **left as verified follow-ups** (dead but harmless on the
Vulkan build; each wants a compile/behaviour check as it lands):
- The dead `DenoiserKind::MetalFX / SvgfBasicMetalFx / SvgfAtrousMetalFx`
  enum members and the ~1000-line Metal-only engine tonemap block they
  gate (`backend_is_metal` is now a compile-time `false`, so the block
  never runs). Purging it should stay red/green — it threads the render
  loop.
- The `#ifdef PT_TARGET_METAL` MSL blocks in `shaders/*.slang` (no `metal`
  Slang target is compiled any more). NOTE: `pt_planet_albedo_test.cpp`
  pins `land_basis_* == 2` in PathTrace.slang (Metal Push cbuffer + SPIR-V
  Frame); when the Metal shader blocks go, that expectation drops to `1`.
- Assorted MoltenVK/Metal *comment* references in kept Vulkan/shader files.

## Conventions (carried from the parent)

- Real physics, metric units, real *cited* constants. No magic epsilons — derive every tolerance. No heuristic shortcuts dressed as physics.
- The engine never rasterizes.
- Adversarially review before merging; verify claims red-then-green; don't regenerate goldens without the owner's sign-off.
- No `Co-Authored-By` and no "Generated with Claude Code" in commit messages (robot line in PR bodies only).
- Design/research and the pending PRs' full history live in the **demont-engine** issue tracker (esp. #339 for streaming, #337 for the MetalFX/swapchain golden-coverage gaps); reference them from here.
