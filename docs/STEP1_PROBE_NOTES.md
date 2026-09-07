> **PROVENANCE.** These are the results of the Step 1 go/no-go probe, salvaged
> from PR #39 (`probe/rt-raygen-o2`) when that PR was closed. The *findings*
> are what matter and they are recorded here; the probe *code* was deliberately
> not merged.
>
> Why the code did not land: it is exploratory scaffolding (`PT_RT_PIPELINE`
> guards, `PathTraceRaygen.slang`, RT payload/stub shaders) for work that has
> not started, and by the time it was assessed it had rotted. It breaks two
> source-contract tests (`pt_planet_shader`, `pt_math_epsilon`, which count
> occurrences in `PathTrace.slang` -- a file that has since moved by roughly a
> thousand lines), and although it merges *textually* clean it breaks the build
> semantically: the branch predates the Embree removal, so merging it
> resurrects a `target_compile_options` call against a target that no longer
> exists.
>
> The branch is retained, not deleted. Rebase it when Step 1 is actually
> implemented rather than carrying a stale port in main.

# Step 1 go/no-go probe: the megakernel as a ray-tracing-pipeline raygen

Branch `probe/rt-raygen-o2` on top of `feat/vulkan-step0-features` (203b4c7).
Box: RTX 5090, driver 616.56, Vulkan 1.4.351, SDK 1.4.341.1, Slang 2026.8.
The question, from `docs/STEP1_RT_PIPELINE_DESIGN.md` section 1 and risk R1:
does a raygen that is still most of the old compute megakernel re-trip the
driver miscompile that forces `PT_SLANGC_OPT=-O0` (`cmake/Slang.cmake`) --
and can the driver build it at all inside the design's 120 s abort criterion?

## What was built

* `shaders/PathTraceRaygen.slang` `#include`s `shaders/PathTrace.slang` with
  `PT_RT_PIPELINE=1`, so the raygen is the megakernel's own text compiled as
  a `raygeneration` stage (no copy of `main()`). Under that switch the
  bounce-loop and G-buffer traces go through `TraceRay` against the stub
  miss / closest-hit pair in `shaders/PathTraceRtStubs.slang` (payload in
  `shaders/PathTraceRtPayload.slang`, 68 B: t, primitive, instance id,
  barycentrics, object-to-world), decoded by the kernel's verbatim hit code;
  every `transmittance()` leg keeps the inline `RayQuery` (design R3). The
  compute build expands every guard away and its SPIR-V is byte-identical to
  the pre-probe module (md5 `c8ebbba1...` against a pristine compile), and
  `r_pt_pipeline compute` renders bit-identically to the pre-probe binary
  (`cornell_csg`, 8 frames, seed 1, imgdiff max/mean delta 0).
* `PT_PROBE_VARIANT` 0 / 1 / 2 = the design's P0 / P1 / P2 escalation. P0
  keeps Lambert only (other materials end the path) and stubs
  `transmittance()` to 1; P1 restores every BSDF branch and the shadow chain;
  P2 is the full kernel. Two -O2 diagnostics of P1: `p1a` = P1 with
  `transmittance()` still stubbed, `p1b` = P1 with every shadow leg tracing
  the TLAS alone instead of the five software tiers.
* `pt_compile_slang` gained `OPT` (per-record slangc level, default
  `PT_SLANGC_OPT`), `CAPABILITY` and `EXTRA_DEPENDS`; the raygen records
  build each variant at -O0 and -O2 with `spirv_1_6+SPV_KHR_ray_tracing`.
  Eight raygen blobs plus the two stubs are embedded; the engine picks one
  at run time.
* RHI / Vulkan (design section 7, minimum): `RayTracingPipelineDesc` (named
  stages, like the named compute pipelines), `Device::CreateRayTracingPipeline`
  (synchronous, timed, logged) and `SupportsRayTracingPipeline`,
  `CommandBuffer::BindRayTracingPipeline` / `TraceRays`, `RayTracingRead` /
  `RayTracingWrite` barrier stages. One pipeline of three groups, recursion
  depth 1, dynamic stack size, a device-local SBT with 64 B-aligned regions
  of one 32 B handle each. `Dispatch`'s descriptor-ring / Frame-UBO-slice
  body became `BindSharedSetAndPush(bind_point)`, shared with `TraceRays`.
* The three shared-layout edits (design section 8): every binding's
  `stageFlags` and the push range carry `COMPUTE | RAYGEN | MISS |
  CLOSEST_HIT | CALLABLE` when the RT pipeline is enabled, and
  `vkCmdPushConstants` passes the layout's flags instead of a hard-coded
  `COMPUTE_BIT`. No binding number moved (the raygen's 41 `Binding`
  decorations are identical to the compute kernel's under `spirv-dis`).
* Engine: `r_pt_pipeline {compute, rt}`, `r_pt_rt_variant`, `r_pt_rt_opt`,
  `r_pt_rt_stats`. Under `rt` the PathTrace pass binds the RT pipeline and
  launches `TraceRays(w, h, 1)` with exactly the compute dispatch's binds and
  push constants; the barriers after the pass name the pipeline that ran it.

## Measurements

Soak = `demont --smoke-frames=60 --r-backend=vulkan` with the cfg
`r_pt_pipeline rt / r_pt_rt_variant / r_pt_rt_opt / r_sky_mode / app_window_*`
(default `earth` scene, denoiser off). PASS = `pipelines ready`, the RT
pipeline active, 0 `DEVICE_LOST`, PNG written, no new Windows event 153.
Pipeline-create times are the first compile of each blob (cold: no entry in
`%LOCALAPPDATA%\demont\pipeline.cache` or the driver's own cache); a repeat
run of a compiled blob returns in 0.02 s from the pipeline cache. Statistics
capture (`VK_KHR_pipeline_executable_properties`) was on for the first P0
runs and off afterwards; the driver reports **0 executables** for the RT
pipeline either way, so there is no register / spill number to report --
the driver's per-group stack size from `vkGetRayTracingShaderGroupStackSizeKHR`
is the only pressure indicator it exposes.

| variant | slangc opt | slangc time | SPIR-V bytes | driver pipeline-create, COLD (s) | 16x16 soak | 1280x720 soak | physical-sky soak | note |
|---|---|---|---|---|---|---|---|---|
| P0 | -O0 | 0.9-1.0 s | 322,356 | **54.89** | PASS | PASS | PASS | inside the 120 s criterion |
| P0 | -O2 | 10.6 s | 2,950,852 | **56.68** | PASS | PASS | PASS | inside the criterion: 9.2x the -O0 SPIR-V costs the driver 1.03x the time |
| P1 | -O0 | 0.9-1.0 s | 389,696 | **284** | PASS | PASS | PASS | over the criterion |
| P1 | -O2 | 35 s | 6,672,624 | **461.83** | PASS | PASS | PASS | over; an earlier attempt was KILLED at a ~200 s ceiling, not by the driver |
| P2 | -O0 | 0.9-1.0 s | 435,808 | **520** | PASS | PASS | PASS | over; also killed once at the ~200 s ceiling |
| P2 | -O2 | 50 s | 8,633,632 | **799** | PASS | PASS | PASS | over; the full kernel -- the variant the verdict rests on |

Every cell above is measured; nothing in this table is estimated, and there is no
"not run" cell left. The p1a / p1b rows are compile-time diagnostics of P1 that do
not render the design's image, so they appear only in the SPIR-V shape table below
and were never soaked.

Two columns come from different runs and must not be read as one wall time. The
**create** column is the cold first compile of each blob, from the escalation runs
(no cache entry, `r_pt_rt_stats 0`). The three **soak** columns were re-run at the
end of the probe against a warm cache, where every create returned in 0.02-0.06 s;
they say the pipeline *runs* clean, not how long it took to build. All twelve soak
cells are 60 frames, `r_denoiser off`, RT pipeline confirmed active in the log,
0 `DEVICE_LOST`, 0 `[ERROR]`, PNG written, and the Windows System log shows no
event 153 (and no 4101 / display-provider reset) across the whole session. The
physical-sky column is `r_sky_mode physical` at 1280x720; no run fell back to
procedural, so each one really marched the atmosphere shell.

**The design's 120 s abort criterion is exceeded by P1 and P2 at both optimisation
levels** (284 / 461.83 / 520 / 799 s against a 120 s bar; only P0 clears it). That
is COMPILE TIME, not a hang. Two of those four runs had been killed earlier at a
~200 s harness ceiling; both were re-run with a 900 s ceiling and both returned
`VK_SUCCESS` and then rendered 60 clean frames. Nothing in the probe ever hung,
timed out inside the driver, or lost the device. The criterion therefore fails as
a *build-budget* test and says nothing about R1's miscompile question, which is
what the verdict turns on.

SPIR-V shape (spirv-dis counts):

| blob | bytes | functions | OpTraceRayKHR | OpRayQueryInitializeKHR | OpLoopMerge |
|---|---|---|---|---|---|
| PathTrace compute -O0 | 434,480 | 226 | 0 | 1 | 51 |
| raygen P0 -O0 | 322,356 | 197 | 1 | 1 | 35 |
| raygen P0 -O2 | 2,950,852 | 1 | 2 | 1 | 174 |
| raygen P1 -O0 | 389,696 | 223 | 1 | 1 | 42 |
| raygen P1 -O2 | 6,672,624 | 1 | 2 | 11 | 633 |
| raygen P2 -O0 | 435,808 | 227 | 1 | 1 | 51 |
| raygen P2 -O2 | 8,633,632 | 1 | 2 | 13 | 807 |
| raygen p1a -O2 (P1, no transmittance) | 3,740,332 | 1 | 2 | 1 | - |
| raygen p1b -O2 (P1, TLAS-only shadow legs) | 5,443,632 | 1 | 2 | 11 | - |

slangc time for the raygen: 0.9-1.0 s at -O0, 10.6 s (P0) / 35 s (P1) /
50 s (P2) at -O2.

## The compute-vs-RT A/B (the 1a acceptance evidence)

Design section 9's acceptance test for 1a: the raygen is today's `main()`, so
`r_pt_pipeline rt` must reproduce `r_pt_pipeline compute` **bitwise**. Harness:
`tools/pt_render_one_frame` (`--backend vulkan --denoiser off --extra
"r_capture_seed 1"`), one binary, one build, the fixtures' own cfgs from
`tests/goldens/scenes/`; RT side adds `r_pt_pipeline rt; r_pt_rt_variant p2;
r_pt_rt_opt o0` -- **P2, the full kernel, at -O0** (chosen because both P2 blobs
were already in the pipeline cache, so the create was 0.05 s instead of another
520 / 799 s; see the -O0 / -O2 control below for why the level does not matter).
Diffed with `tools/imgdiff` at `--max-delta 0 --mean-delta 0 --fail-percent 0` --
no floor, because the determinism baseline for this harness is 30/30 bit-identical,
so any non-zero delta is real.

| fixture | frames | pixels | differing px | % px | max L2 delta | mean L2 delta | rms | threshold 0 |
|---|---|---|---|---|---|---|---|---|
| `planet_surface` | 64 | 196,608 | 9 | 0.004578 | 2.4495 | 0.000059 | 0.009299 | **fail** |
| `orbital_limb` | 8 | 196,608 | 3 | 0.001526 | 1.0000 | 0.000015 | 0.003906 | **fail** |
| `cornell_csg` | 8 | 196,608 | 13 | 0.006612 | 65.3835 | 0.000552 | 0.156185 | **fail** |

**Threshold 0 does not hold. The RT path is not bitwise-identical to the compute
path.** The deviation is tiny -- 3 to 13 pixels in 196,608, at most 0.0066% -- but
it is there, and it is not noise. Four controls:

* **Both paths are deterministic.** `cornell_csg` rendered twice on compute gives
  one md5 (`1e34d527...`) and twice on RT gives one md5 (`8e55c65c...`). The delta
  is a stable, reproducible compute-vs-RT difference, not run-to-run jitter.
* **The slangc level is irrelevant to the pixels.** RT P2 at -O2 produces the
  *same md5 as RT P2 at -O0* (`8e55c65c...`), byte for byte, from a 8.63 MB module
  and a 435 KB one. That is finding 1 seen from the image side: the driver inlines
  to the same code either way, so `PT_SLANGC_OPT` cannot be the cause and cannot be
  the cure.
* **It is not accumulation.** `cornell_csg` at `r_max_bounces 1` still differs
  (14 px, max 21.4709, mean 0.000175), so the divergence is already present in the
  first bounce rather than drifting apart over the path.
* **It is not scheduling.** The megakernel contains no wave intrinsics, no
  `groupshared` and no quad ops (grep of `shaders/PathTrace.slang`), so the raygen's
  different launch geometry cannot reorder a cross-lane reduction; and the RNG is
  seeded per pixel, identically under `DispatchThreadID` and `DispatchRaysIndex`.

Per-pixel shape of the difference: on `planet_surface` and `orbital_limb` every
differing pixel is +/-1 LSB in one or two 8-bit channels. On `cornell_csg` twelve of
the thirteen are 1-14 LSB, and exactly one -- (248,166), compute (13,31,34) vs RT
(68,66,39) -- is a genuine path flip and supplies the whole 65.38 max on its own.
That is the signature of an ULP-level arithmetic difference between the driver's
compute-kernel and raygen code generators, occasionally large enough to flip a
discrete decision (a silhouette hit/miss, a Russian-roulette or branch test) at one
pixel. Root-causing it further was outside the probe's time box.

Consequence for 1a: **the acceptance test as written (bitwise) is not met by the
`TraceRay` + stub-closest-hit trace form.** This does not touch R1 -- it is not a
miscompile, the images are visually identical and the difference is deterministic --
but 1a must either root-cause the residual or state an explicit, justified tolerance
before it ships. It is an open item, not a blocker. The compute side is exonerated:
its SPIR-V is byte-identical to the pre-probe module and `r_pt_pipeline compute`
still renders bit-identically to the pre-probe binary, so the whole delta is on the
RT side.

## Validation layers

`kEnableValidation` in `src/rhi_vulkan/VulkanDevice.cpp` is `!NDEBUG`, so the
release build under test carries no layer and no debug messenger. The layer was
forced on with `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` +
`VK_LOADER_LAYERS_ENABLE=VK_LAYER_KHRONOS_validation` (VVL from SDK 1.4.341.1,
messages land on stdout with no messenger registered).

* `r_pt_pipeline compute`, `cornell_csg`, 8 frames: **zero** validation messages.
* `r_pt_pipeline rt` (P2 -O0), same fixture: **one** message, `VUID-vkCmdTraceRaysKHR-None-08608`,
  once per `vkCmdTraceRaysKHR` (8 frames -> 8). Nothing else -- in particular the
  section 8 stage-flag widening on the shared descriptor set and the push range
  draws no message at all, which is the result that mattered for the layout edits.

**That VUID is a validation-layer false positive, and this is measured, not
assumed.** The text reads "VkPipeline ... doesn't set up
`VK_DYNAMIC_STATE_RAY_TRACING_PIPELINE_STACK_SIZE_KHR`, but since the
vkCmdBindPipeline, the related dynamic state commands
(`vkCmdSetRayTracingPipelineStackSizeKHR`) have been called" -- i.e. the layer
believes the state is static. It is not: `CreateRayTracingPipeline` sets
`ci.pDynamicState` to a one-entry
`VK_DYNAMIC_STATE_RAY_TRACING_PIPELINE_STACK_SIZE_KHR` list whenever both
`vkGetRayTracingShaderGroupStackSizeKHR` and `vkCmdSetRayTracingPipelineStackSizeKHR`
resolved, and both did (the create logs the queried per-group stack sizes, and the
layer itself confirms the set call happened). Proof by control: rebuilding with
`ci.pDynamicState = nullptr` -- the genuinely illegal form -- produces the **same
message, the same count, the same wording**. VVL 1.4.341.1 emits it either way, so
it does not read `pDynamicState` off a ray-tracing pipeline create info. The control
patch was reverted; the shipped code declares the dynamic state, which is the
spec-correct form, and the comment at the call site now says the layer reports it
anyway so the next reader does not "fix" a correct pipeline.

## Findings

1. **The driver inlines the whole raygen whatever slangc emitted.** P0
   compiles in 55 s at -O0 (197 functions, 322 KB) and 55 s at -O2 (one
   function, 2.95 MB); P1 takes 284 s at -O0. The `-O0` out-of-line trick
   that keeps the compute kernel alive does not exist for a raygen: from the
   driver's point of view every raygen is the fully-inlined kernel, so the
   size the driver compiles is set by the source that reaches the stage, not
   by the optimisation level. The design's per-module decomposition is
   therefore not a performance nicety; it is the only way to bound what the
   driver compiles per stage.
2. **No miscompile at the sizes that compile.** Every pipeline the driver
   finished ran 60 frames at 16x16 and 1280x720 with no `DEVICE_LOST` and no
   event 153, at -O0 and at -O2, including the physical-sky march. The TDR
   the compute kernel hits at -O1+ (`docs/NVIDIA_DRIVER_BUG_REPORT.md`) did
   not reproduce in the RT execution model on any variant that built.
3. **Compile time, not correctness, is the wall.** P1 and P2 exceed the
   120 s abort criterion at both optimisation levels (see the table for the
   measured or bounded times). Inlining replicates the five software tiers
   into every `transmittance()` leg (`OpRayQueryInitializeKHR` 1 -> 11 -> 13
   between P0, P1, P2 at -O2), which is exactly the reason design module C8
   exists; the p1a / p1b diagnostics attribute P1's cost between the BSDF
   branches and that replication.
4. Slang 2026.8 and `HitObject` (design 1a shape, R4): `GetRayTCurrent()`,
   `GetObjectToWorld4x3()`, `GetRayTMin()` and `GetWorldRayOrigin()` are
   rejected in the raygeneration stage on the spirv target ("unavailable
   features in entry point"); `GetPrimitiveIndex / GetInstanceID /
   GetAttributes / GetRayDesc / GetObjectToWorld / MakeNop` are accepted. The
   direct SPIR-V emitter declares both `SPV_NV_` and `SPV_EXT_shader_invocation_reorder`
   and mixes `OpTypeHitObjectNV` (5281) with `OpHitObject*EXT` opcodes
   (5313+), which `spirv-val` rejects; `-emit-spirv-via-glsl` produces a
   valid EXT-only module but through a different code generator than the
   compute kernel. The probe therefore ships the plain `TraceRay` + stub
   closest-hit form (`PT_RT_HITOBJECT=0`); the `HitObject` form is kept
   compilable behind `PT_RT_HITOBJECT=1` with the accessor substitutions
   (`GetRayDesc().TMax`, `GetObjectToWorld()`).
5. Other surprises: slangc names every SPIR-V entry point `main` whatever
   `-entry` said (`OpEntryPoint MissKHR %ptMiss "main"`), so the stage's
   `pName` must be `"main"` -- the Slang name is a
   `VK_ERROR_INITIALIZATION_FAILED`. The stage-flag change on the shared
   layout produced no validation message. The one RT-specific VUID
   (`VUID-vkCmdTraceRaysKHR-None-08608`) survives declaring
   `VK_DYNAMIC_STATE_RAY_TRACING_PIPELINE_STACK_SIZE_KHR` and is a VVL
   1.4.341.1 false positive -- see the Validation layers section, which
   carries the control experiment. The other layer warning seen on this
   tree (`Undefined-Value-StorageImage-FormatMismatch-ImageView`,
   `swap_out` rgba8 vs the B8G8R8A8 swapchain view) is raised on a
   `vkCmdDispatch` of the compute path, predates the probe, and does not
   appear in the headless fixtures at all.
6. `--smoke-exec` with `app_window_width 16` produces a 176x16 window on
   this desktop (the platform's minimum width); the 16x16 cells are 176x16.

## Verdict

**GO.** Design section 10, R1's decision rule is "P2 passes -> 1a ships at `-O2`,
1b is a performance step", and P2 -- the full kernel, nothing stubbed -- passed at
both optimisation levels: it built, and it soaked 60 frames at 16x16, at 1280x720
and against the physical-sky march with 0 `DEVICE_LOST` and no event 153. The
miscompile that pins the compute kernel to `-O0` did not reproduce in the RT
execution model on any variant that built. The central unknown is answered.

What the findings then force. The driver fully inlines every raygen whatever slangc
emitted (P0 costs the same 55 s from a 322 KB out-of-line module and from a 2.95 MB
one-function module), so **per-module decomposition (design section 3) is not a
performance nicety -- it is the only mechanism that bounds per-stage compile time**,
and the 120 s budget cannot be met without it. Module **C8, the shared visibility
callable, is load-bearing**: inlining replicates the five software tiers into every
`transmittance()` leg (`OpRayQueryInitializeKHR` 1 -> 11 -> 13 across P0/P1/P2 at
-O2), which is most of what separates P1's 462 s from P0's 57 s. And since the
driver re-inlines regardless, `PT_SLANGC_OPT` is irrelevant for RT stages -- P2 -O0
and P2 -O2 render bit-identically -- so RT stages should be built at `-O0` purely
because slangc is 50x faster there (1.0 s vs 50 s).

One open item rides along, and it is not a blocker: the compute-vs-RT A/B is not
bitwise (3-13 px per 196,608, deterministic, opt-level independent), so 1a must
root-cause that residual or state an explicit tolerance before it claims the design's
acceptance test.
