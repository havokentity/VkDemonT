# NVIDIA driver bug report: GPU hang (TDR / `VK_ERROR_DEVICE_LOST`) on the first dispatch of a large fully-inlined SPIR-V compute kernel

Status: **ready to file** (owner action). Venue: the NVIDIA Developer Program
bug portal (developer.nvidia.com, "Report a Bug" under the developer account),
with the Vulkan section of the NVIDIA developer forums as the fallback. Once
filed, record the bug ID in `HANDOFF.md` under "Native-Vulkan bringup" -- that
is the acceptance item for `docs/NEXTGEN_PLAN.md` step 0.

Everything below is taken from the bisection recorded in `HANDOFF.md`
("Native-Vulkan bringup status") and `cmake/Slang.cmake` (the `PT_SLANGC_OPT`
comment), plus the environment block the engine now prints at startup. Nothing
here is inferred beyond what those runs showed; the one inference is labelled
as such.

---

## Title

Vulkan compute: GPU hang (Windows TDR event 153, `VK_ERROR_DEVICE_LOST`) on the
first `vkCmdDispatch` of a ~6.6 MB fully-inlined SPIR-V compute kernel; the same
source compiled with its functions out-of-line (~0.4 MB) renders correctly.

## Environment

| Item | Value |
|---|---|
| GPU | NVIDIA GeForce RTX 5090 |
| Driver | 616.56 (`32.0.16.1656`); `VkPhysicalDeviceDriverProperties`: driverName `NVIDIA`, driverInfo `616.56` |
| Device Vulkan version | 1.4.351 (`VkPhysicalDeviceProperties::apiVersion`) |
| Loader / SDK | Vulkan SDK 1.4.341.1 (loader 1.4.341, `VK_HEADER_VERSION 341`) |
| OS | Windows 11 Pro, build 10.0.26200 |
| Shader compiler | Slang 2026.8 (`slangc`, bundled in the repo at `third_party/slang`), target `spirv`, stage `compute`, entry `main` |
| Application | VkDemonT / "DeMonT Engine", native Vulkan backend. Compute-only path tracer: one compute kernel (`shaders/PathTrace.slang`, ~11 k lines) that does ray generation, inline `RayQuery` traversal (`VK_KHR_ray_query`), shading and accumulation. No graphics pipelines are ever created. |
| Instance / device setup | `apiVersion` 1.4; `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations`, `VK_EXT_mutable_descriptor_type` enabled; `bufferDeviceAddress`, descriptor-indexing update-after-bind + partially-bound; the full list is printed at startup (see "Startup log" below) |

Startup log as printed by the engine on this machine (the two `enabled` lines
are the exact feature set handed to `vkCreateDevice`):

```
Vulkan API: loader 1.4.341 device 1.4.351 driver 'NVIDIA' 616.56 -> requested 1.4.0 effective 1.4.0
Vulkan RT extension presence: ray_query=true accel_struct=true deferred_host_op=true -> hw_rt=true
Vulkan core features enabled: 1.2{scalarBlockLayout=true uniformBufferStandardLayout=true bufferDeviceAddress=true timelineSemaphore=true} 1.3{maintenance4=true synchronization2=true shaderDemoteToHelperInvocation=true} 1.4{maintenance5=true maintenance6=true shaderSubgroupRotate=true shaderSubgroupRotateClustered=true}
Vulkan extensions enabled: ray_tracing_pipeline=true pipeline_library=true ray_tracing_maintenance1=true invocation_reorder_ext=true position_fetch=true subgroup_uniform_control_flow=true mutable_descriptor_type=true
```

(The bisection itself was done before the 1.4 / RT-pipeline features were
enabled -- with `apiVersion` 1.3 and only the ray-query set -- and the hang was
identical. Those additions are inert for this kernel; they are listed so the
report matches what the engine creates today.)

## Summary

`slangc` at `-O1` or above inlines the whole path-tracing kernel into a single
SPIR-V `main` of about **6.6 MB**. With that module the driver builds the
compute pipeline, but the **first `vkCmdDispatch` never completes**: Windows
logs TDR event ID 153 (`nvlddmkm`: "GPU hung and reset") and the next
`vkQueueSubmit` / `vkWaitForFences` returns `VK_ERROR_DEVICE_LOST`.

The **same source at `-O0`** (functions kept out-of-line; `PathTrace.spv`
drops to ~336 KB at the time of the bisection, 434 KB in the current tree)
compiles, dispatches and renders correctly at every resolution tried,
including the interactive app at full window size.

## Why this is not a workload or application problem

- **Reproduces at 16x16.** A 16x16 dispatch (256 invocations) with the
  cheapest possible configuration -- gradient sky, no geometry, no planet or
  terrain, 1 sample per pixel, 1 bounce, bloom / clouds / denoiser off --
  hangs exactly like a full-resolution frame. 256 invocations of a
  primary-ray-miss cannot reach the 2 s TDR watchdog; this is not a
  per-pixel compute-cost timeout.
- **The executed source path is loop-free.** For that configuration every
  invocation takes the primary-ray-miss path (camera ray, no hit, sky
  gradient, write). Bisection of the shader source established that the
  code actually executed on that path contains no loops. Inference (not
  directly observed): the driver's generated code for the inlined module
  does not terminate on a path whose source terminates.
- **Only the optimisation level flips the outcome.** Identical source,
  identical descriptors, identical dispatch. Two earlier, genuine application
  bugs were found and fixed on the way (a per-kernel descriptor-type
  collision on one binding, fixed with `VK_EXT_mutable_descriptor_type`, and
  a storage-image `Unknown` format whose `OpImageWrite` silently no-ops on
  this driver, fixed by declaring `rgba8`); with both fixed the `-O0` build
  renders and the `-O1+` build still hangs.
- **`[noinline]` did not help.** Marking individual functions `[noinline]` at
  `-O1+` left a mixed inline / out-of-line kernel that still hung. Marking
  only the sky-shading function out-of-line made
  `vkCreateComputePipelines` never return (the run was aborted after
  ~213 s).

## Hang / no-hang matrix

| `slangc` optimisation (`PT_SLANGC_OPT`) | `PathTrace.spv` | Shape of the module | Result |
|---|---|---|---|
| `-O0` | ~336 KB (bisection) / 434 KB (today) | functions out-of-line | renders (16x16, 176x120, 320x240, interactive) |
| `-O1` | ~6.6 MB | everything inlined into `main` | TDR at first dispatch, at every resolution tried down to 16x16 |
| `-O2` | ~6.6 MB | everything inlined into `main` | TDR at first dispatch (same) |
| `-O1+` and `[noinline]` on individual functions | -- | mixed | still TDR |
| `-O1+` and `[noinline]` on the sky function only | -- | mixed | `vkCreateComputePipelines` never returned (~213 s, aborted) |

The engine ships at `-O0` as the workaround (`cmake/Slang.cmake`), which
leaves the kernel un-optimised; that is the cost this report is trying to
remove.

## Steps to reproduce

Prerequisites: Windows 11, RTX 5090 with driver 616.56, Vulkan SDK 1.4.341.1,
Visual Studio 2026 with clang-cl, CMake + Ninja. The repository bundles its
own `slangc` (2026.8).

1. Configure the failing build. `PT_SLANGC_OPT` is a CMake cache variable
   (`cmake/Slang.cmake`) applied to every `slangc` invocation:

   ```
   cmake --preset win-clang-release -DPT_SLANGC_OPT=-O2
   cmake --build build/win-clang-release --target demont
   ```

   This produces `build/win-clang-release/shaders/PathTrace.spv` (~6.6 MB).

2. Write the minimal fixture `repro.cfg` (one setting per line; the engine
   console splits on newlines and semicolons):

   ```
   app_window_width 16
   app_window_height 16
   r_scene_default spheres_csg
   pt_smoke_skip_prim_seed 1
   pt_smoke_skip_csg_seed 1
   r_sky_mode gradient
   r_spp 1
   r_max_bounces 1
   r_bloom 0
   r_clouds 0
   r_denoiser off
   r_capture_format png
   ```

3. Run one frame headlessly:

   ```
   build\win-clang-release\src\app\demont.exe --smoke-frames=1 --r-backend=vulkan --smoke-exec=repro.cfg --smoke-capture-out=repro.png
   ```

   Expected: the process exits 0 and writes `repro.png`.
   Actual: the first dispatch hangs; after the watchdog fires the process
   logs `VK_ERROR_DEVICE_LOST` and the System event log shows event ID 153
   from `nvlddmkm`.

4. Control: reconfigure with `-DPT_SLANGC_OPT=-O0`, rebuild, rerun step 3.
   The frame renders and the process exits 0.

The exact `slangc` command line for either configuration can be printed with
`ninja -C build/win-clang-release -t commands shaders/PathTrace.spv`.

## What to attach

1. `PathTrace.spv` from the `-O2` build (~6.6 MB) and from the `-O0` build
   (434 KB) -- same source, same defines, only the `-O` flag differs.
2. The `slangc` command lines for both (from the `ninja -t commands` line
   above).
3. `spirv-val --target-env vulkan1.4` output for both modules. ALREADY RUN,
   2026-09-07, SDK 1.4.341.1: **both modules validate cleanly, no
   diagnostics, exit 0**. State this in the report -- it rules out
   malformed SPIR-V and puts the fault in the driver's compilation of a
   valid module.

   Current sizes on the tree as of that run (they have grown since the
   original bisection, which recorded 6.6 MB / 336 KB -- quote the current
   pair when filing):

   | Build | `PathTrace.spv` |
   |---|---|
   | `-O2` | 8,663,272 bytes (8.66 MB) |
   | `-O0` | 435,524 bytes (435 KB) |

   The `-O2` module can be produced WITHOUT reconfiguring the build tree --
   which matters, because a `-O2` build tree hangs the GPU on next run.
   Take the `-O0` command line from `build.ninja` and change only `-O0` to
   `-O2` and the `-o` path, e.g. from `build/win-clang-release/src/rhi_vulkan`:

       ../../../../third_party/slang/bin/slangc.exe          ../../../../shaders/PathTrace.slang          -target spirv -entry main -stage compute          -DPT_TARGET_SPIRV -DPT_WATER_ENABLED=1 -DPT_LIGHT_TREE_ENABLED=1          -DPT_PLANET_ENABLED=1 -I ../../shaders -Wno-40100          -O2 -o /some/scratch/PathTrace_O2.spv

   It takes about 52 s.
4. The hang / no-hang matrix above.
5. The System event-log entry (event ID 153, source `nvlddmkm`) from a
   failing run, and the engine's stderr from the same run (it contains the
   startup block above and the `VK_ERROR_DEVICE_LOST` line).
6. Validation-layer output (`VK_LAYER_KHRONOS_validation`) for the failing
   and the passing run.
7. `vulkaninfo --summary` from the machine.
8. `repro.cfg` and the two commands from "Steps to reproduce".

## The ask

1. Confirm whether the driver's shader compiler mishandles this
   fully-inlined ~6.6 MB compute module, and either fix the code generation
   or identify the construct / limit that triggers it so the source can
   avoid it.
2. If an internal size or compile-time limit is being exceeded, report it at
   `vkCreateComputePipelines` (an error, or a validation-observable
   diagnostic) instead of producing a module that hangs the GPU at dispatch.
3. Any driver-side workaround (an inlining or optimisation control) that lets
   the optimised module run while a fix is pending.
