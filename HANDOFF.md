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

## Pending polish branches — adversarially reviewed on native Vulkan, integrating on `integrate/polish-vulkan`

Each was rebased onto `fix/vulkan-native-bringup` (Mac strip + native-Vulkan fixes), rebuilt, and adversarially reviewed on the RTX 5090 (2026-09-05), then either integrated onto `integrate/polish-vulkan` (stacked on PR #2) or rejected:

- `fix/terrain-test-split` — **INTEGRATED with fixes.** The split holds (core = 68 s on CI's `win-debug`; per-PR ctest ~965 s → ~270 s). Review found the Windows lane was NOT failing on terrain alone: `pt_sky_units` timed out at 61 s against a 60 s default and `pt_planet_residency` ran 261/300 s — both budgets raised with measurements. Two per-PR sentinels the split had demoted to nightly were restored (#326 cull wiring, #284 "no converged leaf" — the nightly stand-ins could not see those bugs). Zero-match test filters now fail loudly.
- `fix/distant-water-magenta` — **REWORKED for Vulkan and INTEGRATED** (the original was MetalFX-only; its golden cell died with the backend). The defect is real on the in-house SVGF path: it demodulates by raw albedo, but a planet's primary-hit radiance is `C = T·A·E + S` (attenuated surface + additive in-scatter), so `C/A` reaches >50× physical radiance on dark hazed surfaces — the magenta band. New guide `G = T·A + (1−T)`, with camera→surface transmittance `T` carried in `albedo_tex.a` — derived, not tuned; bit-identical for non-atmosphere scenes; red→green (20 failures → 167/167) via `pt_denoise_demod_guide`. Smoked on Vulkan with `svgf_atrous` engaged. **Owed: a live low-sun look, and a Vulkan SVGF golden (none exists; needs owner sign-off).**
- `fix/stars-in-space` — **INTEGRATED with fixes.** Extinction is the real Simpson optical depth over the shell chord; "use `ro` consistently" fixed a genuine bug (bounce rays judged star occlusion from the camera). Corrected a WRONG citation: the veiling formula is a constant-Weber-contrast soft threshold (Blackwell 1946 / order-2 Naka-Rushton), not Rose (1948) as claimed; dropped a `1e-30` magic epsilon; added four behaviour pins, each shown red. "No goldens move" is only vacuously true — no committed golden runs the Vulkan sky path. **Measured on Vulkan:** day-side space above the limb now shows stars (the headline fix works), no daylight leak; **visible behaviour change:** `ground_night` stars dim by up to 78 levels from real horizon extinction in physical mode — correct, but the owner should know. K=8 stays honestly labelled (demont-engine #338 open).
- `fix/clouds-transition` — **REJECTED for this tree (rework recommended).** The `clouds_raymarched` golden does not exist here (software cell only; software never runs Slang), so "moves one golden" is vacuous — and the pre-pass half of the branch is **unobservable on Vulkan**: `CloudsComposite`'s only call site sits inside the Metal-gated `use_engine_tonemap` block, so raymarched clouds never composite (the same dead-code gate that sinks the lens flare). Measured on the live inline path against a 4 m converged reference: the field-scale schedule is not "27% de-aliasing" but step-dependent **bias reduction** (42% closer in clear air, 76% in the horizon band) that is **39% farther mid-deck**, costs **+55–60% PathTrace time** under `-O0` (horizon steps 24–32 → 100–130), and its constants (0.07 / [8,160] m / 1.04) are admittedly tuned. The pop's root cause is the rectangle-rule cell weight `dens·seg·T_start` (over-counts by τ/(1−e^{−τ}) at τ≈0.5–1 per cell); the Beer–Lambert-exact weight `T_start·(1−exp(−dens·seg))` (Hillaire 2016) converges with the *existing* uniform march at no cost (old/new schedules then agree within 0.2 luminance) — a small, principled rework, but a **visible change (≈−15% clear-air toward the converged value) that is the owner's call**. "Deck-entry brightening is physical" is confirmed (converged luminance rises 136→143→198 into the deck), though the branch exaggerates it slightly more than base. Honest docs + a red→green step test (`pt_cloud_march_step`) are committed on `polish-rebased/clouds-transition` (tip `f5ed68b`); its shader commits are unchanged.
- `feat/sun-lensflare` — **REJECTED for this tree (rework needed).** Rebases and builds clean, 11/11 tests pass, no goldens move — but the flare lives in `Tonemap.slang`, whose only dispatch is gated `use_engine_tonemap = … && backend_is_metal`, now compile-time false: **`Tonemap.slang` is dead code on Vulkan** (the swapchain is written by the denoiser finalize / inline tonemap), so "on by default from orbit" changes cvar state only. Also found: the "cannot overflow" vis-gate still yields +Inf (correctness rests on `1/(1+Inf)=0`); ghost radiance over-claims energy conservation (~80× on large ghosts); the occlusion ramp uses the true solar half-angle while the renderer draws a 2.06× disc; the chromaticity worked-example is wrong from orbit. Comment fixes are on `polish-rebased/sun-lensflare`. **Blocked on a Vulkan tonemap/flare dispatch — a new task the strip exposed.**

**Systemic finding from the two rejections:** the engine's post-process chain — `Tonemap.slang`, the lens-flare composite, and `CloudsComposite` for raymarched clouds — is dispatched only inside `use_engine_tonemap = … && backend_is_metal`, which the strip made compile-time false. **That whole chain is dead on Vulkan.** A Vulkan dispatch path for it is now a first-class task: it unblocks the lens flare, raymarched clouds, and the engine tonemap operators.

Known pre-existing: `pt_math_sphere` bit-pin fails on the base tree (verified still failing on `origin/main` on 2026-09-07; spun out to its own task). The "software goldens flake under CPU load" note is retired with the software backend.

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
3. ~~Merge the pending polish branches after a live check.~~ **Live check passed 2026-09-05; all five reviewed — three integrated on `integrate/polish-vulkan`, two rejected (see the section above).**
4. Start P1 of the streaming plan.

### Mac-strip status (branch `chore/strip-mac-vulkan-only`)

All macOS/Metal *support* has been removed and the Vulkan-only tree
builds clean (`demont.exe` + all targets, `win-clang-release`):

- Deleted `src/rhi_metal/`, the 4 Cocoa `.mm` files, the Metal ocean GPU
  test, the Mac CMake presets, and the metal-cpp / MSL toolchain wiring.
- `BackendType::Metal` removed; `r_backend` defaults to `vulkan`. Its
  allowed set is now `none|vulkan` — **the software backend was removed too**
  (2026-09-07, see "Software RHI removed" below). A retired `r_backend metal`
  from an old macOS cfg is normalized to `vulkan` at boot.
- The Metal denoiser-selection branches and every `#if defined(__APPLE__)`
  block in the kept sources were removed; `OceanGpuActive()` (Metal-only)
  now returns early on the id==0 test (its software-exclusion guard went
  with the software backend).

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

### Native-Vulkan bringup status (branch `fix/vulkan-native-bringup`) — **RENDERS on native Vulkan now**

Step 2 above (native-Vulkan pixel correctness) was completed on a real RTX
5090. The backend had **never actually worked on native Vulkan** — it was
only exercised through MoltenVK, which hid several bugs. Symptom was a black
screen then a GPU hang on the first frame. All three faults are now fixed and
the smoke test renders a correct frame (gradient sky + ground, 0
`DEVICE_LOST`) at 176×120 and 320×240.

- **FIXED (committed, validated): descriptor binding-2 type collision.** One
  shared `VkDescriptorSetLayout` served every kernel with per-kernel binding
  *numbers* that mean different resource TYPES (binding 2 = scene TLAS for
  PathTrace, storage image for the cloud kernels). Native Vulkan faulted;
  fix = declare binding 2 `MUTABLE_EXT` + enable `VK_EXT_mutable_descriptor_type`
  in `VulkanDevice.cpp`. `VUID-07990` 2→0.
- **FIXED: swapchain storage-image format.** Commit `5687583` switched the
  swapchain outputs from `rgba8` to `Unknown` format to satisfy a validation
  warning — but on driver **32.0.16.1656** `Unknown`-format `OpImageWrite`
  silently NO-OPS (the swapchain stays black), the exact 596.x-family
  regression the `PathTrace.slang` top comment warns about. **Reverted the
  swapchain outputs back to `rgba8`** (`output`, `swap_out`, `ldr_out`,
  `out_image` in PathTrace / DenoiseFinalize / Tonemap / PerfOverlay /
  EditorOverlay). The BGRA8-vs-rgba8 mismatch is only a validation warning,
  not a fault.
- **FIXED — the TDR hang was the megakernel being fully INLINED.** The hang
  (Windows event **153**, `nvlddmkm` "GPU hung and reset") was
  workload-independent — a 16×16 gradient frame with no planet/terrain/CSG,
  bloom/clouds off, 1 bounce, 1 spp still hung, which rules out a per-pixel
  compute-cost TDR (256 pixels cannot cross the 2 s watchdog). Root cause:
  Slang **inlines the whole path tracer into a single ~6.6 MB SPIR-V `main`**,
  and the NVIDIA driver miscompiles that blob into a **non-terminating ISA
  loop** on the primary-ray-miss path (the executed source there is loop-free
  — verified by bisection). Fix: compile the SPIR-V shaders with **`slangc
  -O0`** (`PT_SLANGC_OPT` in `cmake/Slang.cmake`), which keeps functions
  out-of-line — `PathTrace.spv` drops 6.6 MB → 336 KB and the driver compiles
  it correctly. `-O1`+ re-inline and re-hang; `[noinline]` on individual
  functions did not help (a mixed inline/out-of-line kernel still choked the
  driver, and skyColor alone out-of-line never finished the pipeline build).
  **Caveat / follow-up:** `-O0` leaves the megakernel un-optimised, so path
  tracing is slower than it should be. The proper long-term fix is to
  **split/shrink the megakernel** (the repo's standing plan) so it can run
  optimised; `-O0` is what unblocks native-Vulkan bringup today.
- **FIXED in passing:** `-DPT_PLANET_ENABLED=OFF` compiles now (the
  `atmo_ms_lut` / `ptMsLutReady` hoist out of the planet `#if` gate, commit
  `4f36768`).
- **Live-checked (2026-09-05):** the owner eyeballed and tested the
  interactive app on the RTX 5090 — the presented swapchain renders correctly
  (confirming the `rgba8` revert) and everything is back in working order.
  That is the live check step 3 above was gated on, so the pending polish
  branches can now be merged. **The "owed Vulkan golden cell" is DONE and
  then some** — the whole matrix is Vulkan now (40 cells), see below.

### Next-gen roadmap progress (see `docs/NEXTGEN_PLAN.md`)

- **Step 0 — DONE** (`feat/vulkan-step0-features`, PR #4): Vulkan 1.4 requested and effective; the promoted 1.2/1.3/1.4 features and the RT-pipeline family (`VK_KHR_ray_tracing_pipeline`, `pipeline_library`, `ray_tracing_maintenance1`, `VK_EXT_ray_tracing_invocation_reorder`, `position_fetch`, `subgroup_uniform_control_flow`) enabled; every next-gen extension queried and logged at startup — all present on the RTX 5090 / 616.56 (SER in REORDER mode, cluster-AS rev 4, PTLAS, coopvec/coopmat, FP8, OMM; only `shaderBFloat16DotProduct` absent). Rendering bit-identical (md5). `docs/NVIDIA_DRIVER_BUG_REPORT.md` is **ON HOLD, do not file yet** (2026-09-07): GPU-assisted validation found a real out-of-bounds read in our own shader (`mesh_uvs`, binding 35, ~3 KB past a 16-byte placeholder), fixed in `a88f5a9`. An OOB read is undefined behaviour, which is a live alternative explanation for the `-O2` hang the report blames on the driver. **Retest `-O2` with the fix in before filing**; if it still hangs, file — the report is now stronger, since both modules pass `spirv-val` AND GPU-AV. Record the bug ID here if filed.
- **Step 1 — design done** (`docs/STEP1_RT_PIPELINE_DESIGN.md`): 21 shader groups in 4 pipeline libraries, software tiers merged in raygen via portable `MakeMiss` records, 48 B payload, two SER reorder points, shared layout reused. Leads with a **2-day go/no-go probe** (raygen-only port at `-O2`, 16×16 soak) before the split; wavefront-compute is plan B. Note: `NEXTGEN_PLAN.md`'s `PathTrace.slang` line numbers are ~220 lines stale since the polish landed; the design cites current lines.
- **Owed before 1b:** ~~Vulkan golden cells~~ **DONE** — the matrix is 40 Vulkan cells, all pinned, all passing. The determinism baseline for the A/B harness is still owed.

## Software RHI removed; the golden matrix is GPU-only (2026-09-07, PR #41)

The engine is Windows/Vulkan/NVIDIA-exclusive and targets 5090-class hardware,
so the CPU tracer was carrying maintenance cost while validating nothing the
renderer runs. Deleted `src/rhi_software/` (3,486 lines), `BackendType::Software`,
`r_backend software`, and the vulkan→software HWND recreate path.

**Embree went with it** — it existed solely to give that backend a CPU BVH, and
every other reference in the tree was a comment. `cmake/EmbreeBinary.cmake`,
`cmake/EmbreeConfig.cmake`, `cmake/embree-prebuild/` and
`.github/workflows/prebuild-embree.yml` are gone. That removes the slowest
dependency in the build (10–15 min from source, which is why the prebuilt-cache
workflow existed).

**The golden matrix moved rather than died.** All 20 committed Windows goldens
were *software* cells, so the matrix could never detect a GPU rendering
regression; the three registered Vulkan cells had no goldens at all. The cells
were converted: **40 cells, all Vulkan, all pinned, all passing.**

**CI CONSEQUENCE, and it is the important one.** `build.yml` ran
`ctest -L golden_software` because that lane worked on any CPU. GitHub-hosted
`windows-latest` has no usable Vulkan ICD, so **the golden matrix cannot run on
hosted CI at all.** The step was removed rather than left silently red. Until a
self-hosted RTX runner is attached, `ctest -R golden` is a **local gate** — run
it before merging anything touching shaders or radiometry, because CI will not
catch it. Unit tests still run on every push; the Linux sanitizer job is
unaffected (CPU-only host code, and no longer fetches Embree).

Testing rules that came out of the move, each measured rather than assumed:
- **Star cells MUST use `DENOISER svgf_atrous`.** `r_star_split` routes stars
  through `StarsComposite.slang` *after* the denoiser, so a `DENOISER off` cell
  pins a starless frame and passes forever.
- **Night cells are pinned PIXEL-EXACT (0/0/0).** A *complete* star wipeout is
  only `mean_delta 0.019` over 0.44% of pixels — the matrix's usual `32/4/2`
  would pass it. Threshold 0 is reachable because the render is bit-identical
  run-to-run on this host. A driver update legitimately re-pins these:
  regenerate, do not loosen.
- **Still-painted terms keep their authored exposure**, documented in-fixture:
  the moon disc (`lunar_night`) and the cloud path (`bsc_night_clouds`, which
  rendered flat white at the derived night exposure). Migrating those is the
  successor to the #338 radiometry work.

## Night-sky radiometry landed (#338 closed, PR #41)

Stars had no radiometry: Vega was `4.0` arbitrary units while the sky was in real
W/m²/sr, the splat wrote *peak amplitude* (so energy went as `flux·σ²`), and σ was
tiered *by magnitude* (applying the magnitude scale twice). Replaced with Pogson's
ratio on a cited 3.19e-9 W/m² V-band zero point, a discretely-renormalised
energy-conserving splat, one optical PSF σ, and footprint-integrated sampling.
`kStarVeilK = 8.0` is **retired** — on a shared scale a star is ~2e-5 of a noon
zenith, so daylight suppression needs no help. **demont-engine #338 is closed by
this**; the "K=8 stays honestly labelled" note above is superseded.

The stars were not the visible problem, though. `procSky`'s night floor was
~2e5× too bright (0.0589 W/m²/sr of luma against a real dark sky's 2.5e-7),
which is what actually drowned them. Fixed from V=22.0 mag/arcsec² dark-site
brightness. `r_exposure_max` went 4.0 → 2000, *derived* from the naked-eye
limiting magnitude. Daylight is unchanged (median 200.88 → 200.80); twilight
moved toward truth (0.0954 → 0.0421 W/m²/sr against a real ~0.03).

## DLSS / NRD in flight

- `docs/DLSS_INTEGRATION_PLAN.md` — recommends **direct NGX over Streamline**,
  reversing `NEXTGEN_PLAN.md` §2 item 5. Two blockers it found: Ray
  Reconstruction's guide buffers (specular albedo / roughness / hit distance)
  are **dead code** behind a MetalFX-only gate (`Engine.cpp:8118`), and the
  G-buffer ray is traced **unjittered** while colour is jittered
  (`PathTrace.slang:7995` vs `:8391`) — which degrades SVGF and NRD *today*,
  not just future DLSS.
- Build flags `PT_ENABLE_NRD` / `PT_ENABLE_DLSS` default **ON** (gated on
  Vulkan, following the `PT_NRD_ACTIVE` pattern); the runtime cvars default
  **off**, so the out-of-the-box render stays the physically-correct native one
  and the pixel-exact goldens keep a ground-truth reference.
- DLSS testing strategy: pin our *contract* with DLSS (render scale, jitter
  value, guide-buffer sanity — deterministic and ours) plus one pixel-exact
  end-to-end cell. No loose-tolerance DLSS cell: loose tolerances demonstrably
  hide real regressions (see the star-wipeout figure above).

## Conventions (carried from the parent)

- Real physics, metric units, real *cited* constants. No magic epsilons — derive every tolerance. No heuristic shortcuts dressed as physics.
- The engine never rasterizes.
- Adversarially review before merging; verify claims red-then-green; don't regenerate goldens without the owner's sign-off.
- No `Co-Authored-By` and no "Generated with Claude Code" in commit messages (robot line in PR bodies only).
- Design/research and the pending PRs' full history live in the **demont-engine** issue tracker (esp. #339 for streaming, #337 for the MetalFX/swapchain golden-coverage gaps); reference them from here.
