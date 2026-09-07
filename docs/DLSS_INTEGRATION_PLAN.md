# DLSS integration plan (Super Resolution + DLAA + Ray Reconstruction)

Date: 2026-09-07. Tree: `debug/stars-physical` @ `740341f`. Platform: Windows / native
Vulkan / NVIDIA RTX 5090, `win-clang-release`.

Every `file:line` in this document was read on `740341f`. Line numbers will drift as the
parallel workstreams land; the surrounding identifier is quoted so the site stays findable.
Anything I could not verify is marked **[unverified]** rather than asserted.

Goal, in the shape Cyberpunk 2077 exposes it: a Super Resolution mode selector
(Quality / Balanced / Performance, plus Ultra Performance for free), a native-resolution
DLAA mode, and a separate Ray Reconstruction toggle that composes with the mode selector —
all driven from console cvars.

---

## 0. What this plan depends on and does not implement

| Prerequisite | Owner | Status on `740341f` |
|---|---|---|
| Render-resolution decoupling (`r_render_scale`) | parallel workstream | **not present.** Nothing in the tree renders at a size different from the swapchain. Assumed to land; §3 defines the contract DLSS needs from it. |
| Deterministic per-frame sub-pixel jitter | parallel workstream / **already partly present** | **already present** — see §4. The brief said it was absent; it is not. It is present, deterministic, host-driven and already plumbed to the RHI. What is missing is the *contract*, not the mechanism. |
| Motion vectors | landed | `motion_tex`, `PathTrace.slang:57` (decl), `PathTrace.slang:8129` (write), `Engine.h:1814` (`motion_tex_id_`). |
| NRD library denoiser | parallel workstream | scaffolded (`cmake/Dependencies.cmake:158-206`), `VulkanNrdLibDenoiser::Encode` is a passthrough copy (`src/rhi_vulkan/VulkanNrdLibDenoiser.cpp:30`, `:234`, `:255`). §5 defines the coordination. |

This document decides; it does not write code.

---

## 1. Decision: direct NGX, not Streamline

**Recommendation: integrate against the DLSS SDK's NGX Vulkan API directly**
(`github.com/NVIDIA/DLSS`, tag `v310.7.0`, released 2026-06-23), behind an RHI seam shaped
so a later Streamline port replaces one file.

This *dissents* from `docs/NEXTGEN_PLAN.md` §2 item 5, which proposed Streamline. The
reasoning below is why.

### 1.1 What Streamline actually buys, priced against this engine

Streamline's two selling points are vendor abstraction and one-interface-for-everything.

**Vendor abstraction is worth zero here.** `AGENTS.md` ("Platform") states the fork is
Windows / Vulkan / NVIDIA RTX only, the Metal backend was stripped, and the software
backend was removed on this very branch (`740341f` deletes `src/rhi_software/`). There is
no second vendor to abstract over and no plan for one. Streamline's own abstraction does
not give you FSR or XeSS for free anyway — it gives you a plugin slot that NVIDIA fills.

**One-interface-for-everything is worth part of its price.** Streamline covers SR (`sl.dlss`),
RR (`sl.dlss_d`), Frame Generation (`sl.dlss_g`) and Reflex (`sl.reflex`) behind one tagging
API. But FG and Reflex are exactly the two features the owner did *not* ask for, and they are
exactly the two that justify Streamline's most invasive machinery: FG and Reflex need
present-queue and swapchain interception, SR and RR do not. Buying the interposer to get
SR + RR is paying the whole price for a third of the goods.

### 1.2 What Streamline costs this engine specifically

Streamline on Vulkan works by interposing `vkCreateInstance` / `vkCreateDevice` /
`vkGetDeviceProcAddr` (the app either links `sl.interposer` in place of the loader or hands
Streamline its Vulkan objects via `slSetVulkanInfo`). Against this codebase that means:

- `VulkanDevice` hand-rolls instance creation (`src/rhi_vulkan/VulkanDevice.cpp:879`,
  `ici.enabledExtensionCount`) and device creation (`:1772`, `dci.enabledExtensionCount`),
  with a version negotiation that requests Vulkan 1.4 when the loader supports it and falls
  back to 1.3 (`:846-855`). All of that would have to be routed through, or duplicated
  behind, the interposer.
- The descriptor model is deliberately unusual: one shared "god" set layout with
  `VK_EXT_mutable_descriptor_type` so a binding number can mean different resource *types*
  per kernel (`VulkanDevice.cpp:1484-1500`). The repo has already lost time to exactly one
  such collision (binding-2, native-Vulkan `DEVICE_LOST`). Adding a second closed-source
  layer between the engine and the driver makes the next one harder to bisect, not easier.
- Presentation is a compute write to the swapchain image (`sci.imageUsage` includes
  `VK_IMAGE_USAGE_STORAGE_BIT`, `VulkanDevice.cpp:2846`). There is no graphics pipeline and
  no render pass. Streamline assumes a more conventional app shape than this.

### 1.3 What NGX asks for, priced against this engine

The NGX Vulkan surface is: `NVSDK_NGX_VULKAN_Init*`, `NVSDK_NGX_VULKAN_GetCapabilityParameters`,
`NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements` /
`...GetFeatureDeviceExtensionRequirements`, `NVSDK_NGX_VULKAN_CreateFeature`,
`NVSDK_NGX_VULKAN_EvaluateFeature`, `NVSDK_NGX_VULKAN_ReleaseFeature`,
`NVSDK_NGX_VULKAN_Shutdown1`, plus the `NGX_VULKAN_*_DLSS_EXT` / `..._DLSSD_EXT` helper
macros in `nvsdk_ngx_helpers_vk.h` / `nvsdk_ngx_helpers_dlssd_vk.h`.

Evaluation takes a `VkCommandBuffer` and `NVSDK_NGX_Resource_VK` wrappers around
`VkImage` + `VkImageView` + format + extent. That is precisely the currency `VulkanDevice`
already deals in, and precisely the currency `Device::DenoiseDesc` (`src/rhi/Device.h:258`)
already passes around. The integration is a sibling of `VulkanOptixDenoiser` — a per-frame
`Encode`-shaped call over engine-owned images — not a new frame architecture.

The one genuine NGX-specific chore: both extension-requirement queries must run **before**
`vkCreateInstance` / `vkCreateDevice` (the header states this explicitly for each). So
`VulkanDevice::Init` gains a "if DLSS is compiled in, ask NGX what it needs and append to
`exts` / `dexts`" step ahead of `:879` and `:1772`. That is a bounded, readable change to
two existing extension lists.

### 1.4 The cost of choosing NGX — stated plainly

1. **Frame Generation later is a separate integration.** If the owner ever wants DLSS-FG,
   Streamline is still the sane way to get it (swapchain proxy, pacing, Reflex coupling) and
   that work is not reduced by having done NGX first. Accepting this now is the main
   concession. (For a physically-motivated path tracer used to produce goldens, interpolated
   frames are of questionable value anyway — they are not samples of the scene.)
2. **Reflex is not free.** Streamline would have given it for near-zero extra work.
3. **More boilerplate per feature.** DLL search path (`NVSDK_NGX_VULKAN_Init_with_ProjectID`
   / `Init_Ext`), feature create/release on every resize and mode change, image layout
   transitions into `VK_IMAGE_LAYOUT_GENERAL`/`SHADER_READ_ONLY_OPTIMAL` around evaluate,
   capability and driver-version gating — all ours to write. Streamline does some of it.
4. **No licence relief.** See §7: Streamline's own source is MIT, but the `nvngx_dlss*.dll`
   binaries it loads are the *same* NVIDIA-RTX-SDK-licensed files NGX loads. Choosing
   Streamline does not avoid the NVIDIA licence; it adds an MIT wrapper over it.

### 1.5 The seam that keeps the door open

Add `src/rhi/Upscaler.h` with an `UpscaleDesc` mirroring `DenoiseDesc`'s style
(`src/rhi/Device.h:258-380`) and a single Vulkan implementation
`src/rhi_vulkan/VulkanNgxUpscaler.{h,cpp}`. Everything Streamline-specific (or FG-specific)
later lands as a second implementation of that seam. The engine side — cvars, G-buffer
allocation, render/display extent split, pass ordering — is identical either way, so none
of the work in §8 is wasted if the decision is revisited.

---

## 2. Required inputs, per feature, against what this engine produces

### 2.1 Super Resolution / DLAA (`NVSDK_NGX_Feature_SuperSampling`)

`NVSDK_NGX_VK_DLSS_Eval_Params` (`include/nvsdk_ngx_helpers_vk.h`).

| Input | Engine status | Where | Work |
|---|---|---|---|
| `Feature.pInColor` — noisy linear HDR at **render** res | **HAVE** | `denoise_color`, `Engine.h` `denoise_color_tex_id_`; `DenoiseDesc::color_in` documented "RGBA16F linear (per-frame, not accumulated)" `src/rhi/Device.h:259` | Allocate it when DLSS is on, not only when `denoiser_active_` — the gate is `need_hdr_aux` at `Engine.cpp:8152-8162`. |
| `Feature.pInOutput` — linear HDR at **display** res | **MISSING** | `post_denoise_hdr_tex_id_` (`Engine.h:1833`) exists but is allocated at swapchain size, which today *is* the render size | New allocation at display res; §6 `r_render_scale` contract. |
| `pInDepth` | **HAVE, with caveats** | `depth_tex`, decl `PathTrace.slang:56` ("R32F linear camera-space depth"), write `PathTrace.slang:8128`, value `h0.t * dot(rd0, fwd)` at `PathTrace.slang:8062` | Three caveats below. |
| `pInMotionVectors` | **HAVE, with caveats** | `motion_tex`, decl `PathTrace.slang:57` ("RG16F pixel-space (prev - curr) delta"), write `PathTrace.slang:8129` | Two caveats below. |
| `InJitterOffsetX/Y` | **HAVE the mechanism, MISSING the contract** | `push.halton_jitter` `Engine.cpp:9023`/`:10149`, `last_jitter_x_` `Engine.h:2266`, already forwarded as `DenoiseDesc::jitter_x` `src/rhi/Device.h:360` / `Engine.cpp:12949` | See §4 — this is the substantive design item. |
| `pInExposureTexture` / `InPreExposure` | **MISMATCH** | `exposure_state` is a 4-byte **storage buffer**, `Engine.cpp:4091` (`.debug_name = "exposure_state"`), bound at `Engine.cpp:8618` | See caveat E below. |
| `InReset` | **HAVE** | `DenoiseDesc::reset_history` from `!prev_frame_valid_`, `Engine.cpp` near `:12950` | Reuse verbatim. |
| `InRenderSubrectDimensions` | derived | — | = render extent. |

**Caveat A — the depth/motion G-buffer is traced *unjittered*.** The primary G-buffer ray is
built at the pixel centre with no jitter at all:

```
PathTrace.slang:8000:  float2 uv0 = ((float2(tid) + 0.5) / float2(dim)) * 2.0 - 1.0;
```

while the colour samples *are* jittered (`PathTrace.slang:8391`, `jitter = halton_jitter;`).
So depth, motion, normal, albedo and the specular trio describe a surface up to half a pixel
away from the surface the colour sample actually hit. DLSS's whole premise is that colour and
its guides are the same point sample. This is a correctness item, not a polish item, and it is
not mentioned in `docs/NEXTGEN_PLAN.md`. Fix: apply the frame's jitter to `uv0` as well when
DLSS is active. Cost: the G-buffer becomes jittered, which is what NRD and SVGF also want (they
already receive jittered colour and unjittered guides today — a pre-existing latent defect this
change also repairs).

**Caveat B — the sky depth sentinel is `1.0e10`** (`PathTrace.slang:8116`). DLSS's depth
handling is not documented against a value that large. Two options, in preference order:
(1) keep linear depth and clamp the sky to the far plane the projection actually uses;
(2) feed the hardware-style `z/w` reconstructed from `curr_view_proj`. Note also that
`src/rhi/Device.h:260` documents `depth_in` as "R32F clip-space depth (z/w in [0,1])", which
contradicts the shader — **the RHI comment is stale**; the shader is authoritative. Fix that
comment while you are in there. **[unverified]** whether NGX SR accepts linear view depth
without a flag; the `DepthInverted` create-flag covers reversed-Z, not linearity.

**Caveat C — motion vectors are `RG16F` in *pixel* space.** NGX accepts pixel space via
`InMVScaleX/Y = {1, 1}`; the direction convention (`prev - curr`, i.e. "add this to the current
position to get the previous one") already matches DLSS's. But `Engine.cpp:8996` records the
engine has seen "hundreds of pixels of motion-vector jitter at close range", and fp16 spacing at
magnitude 1024 is 0.5 px, at 2048 it is 1.0 px. At 4K with fast motion that is visible
ghosting. Recommendation: either promote `motion_tex` to `RG32F`, or write normalised
`[-1,1]` motion and pass `InMVScaleX/Y = {width, height}`. Normalising is cheaper in bandwidth
and is what Streamline's guide recommends (`mvecScale = {1/width, 1/height}` for a pixel-space
source).

**Caveat D — motion vectors are correctly jitter-free.** `curr_view_proj` / `prev_view_proj`
are built host-side from the camera and contain no jitter; the jitter is added to the ray uv,
not baked into the matrix (`Engine.cpp:10149` sets the push, `PathTrace.slang:8391` consumes
it in ray-gen). So the reprojection at `PathTrace.slang:8120-8127` produces exactly the
unjittered motion vectors DLSS wants. No work. Worth stating because it is the input people
usually get wrong.

**Caveat E — exposure.** NGX wants either a 1×1 `R32F` *texture* or a scalar `InPreExposure`.
The engine has neither: `exposure_state` is a storage buffer holding a *post*-exposure scalar
applied at tonemap time, and the radiance in `denoise_color` is in physical units with an
enormous dynamic range (the night-sky work has values spanning many decades). Three-step plan:
(1) ship with `NVSDK_NGX_DLSS_Feature_Flags_AutoExposure` and let DLSS self-meter — safest
first move; (2) add a 1×1 `R32F` texture that `AutoExposure.slang` writes alongside
`exposure_state[0]`, and A/B it; (3) if banding or adaptation lag appears at extreme
luminances, feed `InPreExposure` and pre-divide the colour. Do not skip straight to (3):
pre-exposure changes the numbers the accumulator and the goldens see.

### 2.2 Ray Reconstruction (`NVSDK_NGX_Feature_RayReconstruction`, value 13)

`NVSDK_NGX_VK_DLSSD_Eval_Params` (`include/nvsdk_ngx_helpers_dlssd_vk.h`). RR takes everything
SR takes, plus guide buffers.

| Input | Engine status | Where | Verdict |
|---|---|---|---|
| `pInDiffuseAlbedo` | **HAVE** | `albedo_tex` decl `PathTrace.slang:148`, write `PathTrace.slang:8234` | Allocation gate `want_albedo_gbuffer` (`Engine.cpp:8095`) does not list a DLSS kind — extend it. `.a` carries the SVGF aerial-perspective demod guide; RR reads `.rgb`. **[unverified]** that RR ignores `.a` (OptiX documents that it does; DLSS-RR does not say). |
| `pInNormals` | **HAVE** | `normal_tex` decl `PathTrace.slang:122`, write `PathTrace.slang:8139` = `float4(hit_normal, 0)`; `hit_normal = h0.normal` at `PathTrace.slang:8060` | World-space, unit length, `RGBA16F` — matches RR's `RGB_F16`. Supply `pInWorldToViewMatrix` (see below). Set `normalRoughnessMode = eUnPacked` (roughness is a separate texture here). **[unverified]** whether `h0.normal` is the normal-mapped shading normal for every material path or the geometric normal for some; RR wants the shading normal. |
| `pInRoughness` | **texture exists, path is DEAD** | `roughness_tex` decl `PathTrace.slang:230`, write `PathTrace.slang:8292` | **BLOCKER** — see below. |
| `pInSpecularAlbedo` | **texture exists, path is DEAD, and the quantity is wrong** | `specular_albedo_tex` decl `PathTrace.slang:219`, write `PathTrace.slang:8282` | **BLOCKER** — see below. |
| `pInSpecularHitDistance` (optional) | **proxy only** | `specular_hit_distance_tex` decl `PathTrace.slang:253`, write `PathTrace.slang:8306` | **Not shippable as-is** — see below. |
| `pInWorldToViewMatrix`, `pInViewToClipMatrix` | **MISSING as separate matrices** | the push carries the *combined* `curr_view_proj` / `prev_view_proj` (`Engine.cpp:9023` region) | Push the two factors as well, or reconstruct host-side. Cheap. |
| `pInAlpha` / `pInOutputAlpha` | n/a | — | **[unverified]** whether these are mandatory or only used when alpha upscaling is requested. Verify at bringup. |
| HDR requirement | **HAVE** | `r_hdr_pipeline` default `1` (`Engine.cpp` cvar block) | RR *requires* HDR input. DLSS must refuse to engage with `r_hdr_pipeline 0` rather than silently produce garbage. |

#### The three blockers, in detail

**Blocker 1 — the specular guide trio is never allocated on this tree.** The allocation gate is

```
Engine.cpp:8118:  const bool want_specular_guidance_gbuffers =
                      (denoiser_kind_ == DenoiserKind::MetalFX             ||
                       denoiser_kind_ == DenoiserKind::SvgfBasicMetalFx    ||
                       denoiser_kind_ == DenoiserKind::SvgfAtrousMetalFx);
```

and the runtime write flags follow the texture ids (`Engine.cpp:9770-9776`,
`push.write_specular_albedo_gbuffer = (denoiser_active_ && specular_albedo_tex_id_ != 0)`).
The MetalFX kinds are unreachable on Vulkan: `r_denoiser`'s parser
(`Engine.cpp:7812` onward) maps `svgf_*_metalfx` down to plain SVGF and never yields
`DenoiserKind::MetalFX` at all. So `specular_albedo_tex_id_`, `roughness_tex_id_` and
`specular_hit_distance_tex_id_` (`Engine.h:1938-1940`) are permanently zero, the shader writes
at `PathTrace.slang:8282/8292/8306` never execute, and the bind sites at `Engine.cpp:8907/8910/8913`
never fire. The textures, the shader code, the RHI fields (`src/rhi/Device.h:299-301`) and the
push flags all exist and all are dead. Un-deading them is one line in the gate plus a
`DenoiserKind` value — the cheapest work item in this plan, and it is the one that unblocks RR.

**Blocker 2 — `specular_albedo` is raw F0, but RR wants the integrated specular reflectance.**
The shader writes the Fresnel reflectance at normal incidence (`PathTrace.slang:8270-8282`:
metal → albedo, dielectric → `float3(0.04)`, Lambert → `0`). Streamline's RR guide specifies
the *pre-integrated* environment-BRDF term — it gives `EnvBRDFApprox2(specularColor, roughness,
NdotV)`, i.e. the split-sum approximation `F0 * A(roughness, NdotV) + B(roughness, NdotV)`
(Karis, "Real Shading in Unreal Engine 4", SIGGRAPH 2013 course notes; Lazarov's analytic fit).
Handing RR raw F0 tells it every grazing-angle surface is as dark as its normal-incidence
reflectance, which is wrong by up to a factor of `1/F0` at the horizon — on water and on the
planet limb, the two places this engine cares about most. This is a derivation, not a tuning
constant: implement the split-sum term from the same `h0.roughness` and `dot(-rd0, hit_normal)`
already in scope at the write site, and cite the source in the comment. The existing raw-F0
write can stay as the fallback for any consumer that wants F0 proper, but RR must get the
integrated term.

**Blocker 3 — `specular_hit_distance` is a smoothness-weighted proxy, not a distance.** The
shader writes `h0.t * (1 - saturate(h0.roughness))` (`PathTrace.slang:8294-8306`), and its own
declaration comment (`PathTrace.slang:232-252`) says so: "MVP semantics: we don't run a second
trace per pixel". RR uses this to reproject reflections parallax-correctly; a value that
collapses to 0 on rough surfaces and to the *primary* distance on mirrors will actively
mis-reproject rather than help. The input is optional in RR, so the honest sequence is:
(a) ship RR with `pInSpecularHitDistance = nullptr`; (b) then either trace one real reflection
ray from the primary hit and write its `t`, or produce `pInSpecularMotionVectors` (the
documented alternative), and A/B both against (a). Do not ship the proxy under an input whose
contract is "distance to the reflected hit".

### 2.3 Summary — the missing-input list that blocks RR

1. Specular albedo / roughness / specular-hit-distance G-buffers are never allocated
   (`Engine.cpp:8118`). *Dead code today.*
2. Specular albedo is raw F0, not the split-sum integrated specular reflectance.
3. Specular hit distance is a proxy, not a distance.
4. Exposure is a storage buffer, not a texture, and there is no pre-exposure concept
   (`Engine.cpp:4091`).
5. World-to-view and view-to-clip are not pushed separately.
6. G-buffers are traced unjittered while colour is jittered (`PathTrace.slang:8000` vs `:8391`).
7. Jitter phase count is hardcoded to 16 and applies only to sample 0 (§4).
8. There is no display-resolution HDR output target.
9. Render-resolution decoupling itself (parallel workstream).
10. `RG16F` motion vectors may lack precision at 4K (caveat C).

Items 6, 7, 8, 9, 10 also block plain SR/DLAA. Items 1, 2, 3, 5 are RR-only. Item 4 is
soft for SR (auto-exposure) and soft for RR (RR ignores `useAutoExposure`), but is the
largest *quality* unknown for both.

---

## 3. The preset table

**The implementation must not hardcode these ratios.** The correct call is
`NGX_DLSS_GET_OPTIMAL_SETTINGS(params, displayW, displayH, perfQualityValue, &optimalW,
&optimalH, &maxW, &maxH, &minW, &minH, &sharpness)` from `include/nvsdk_ngx_helpers.h`,
once per display-resolution change and per mode change; the render extent is whatever
`optimalW/optimalH` comes back as. The table below documents what that query currently
returns, so the values can be reviewed, not so they can be typed into the source.

`NVSDK_NGX_PerfQuality_Value` (from `include/nvsdk_ngx_defs.h`, values are implicit 0..5 in
declaration order):

| Ordinal | Enum | `r_dlss` value | Per-axis ratio | Render res @ 3840×2160 | Render res @ 2560×1440 |
|---|---|---|---|---|---|
| 0 | `..._MaxPerf` | `performance` | 0.5 | 1920×1080 | 1280×720 |
| 1 | `..._Balanced` | `balanced` | 0.58 | 2227×1253 | 1485×835 |
| 2 | `..._MaxQuality` | `quality` | 2/3 (0.66666667) | 2560×1440 | 1707×960 |
| 3 | `..._UltraPerformance` | `ultra_performance` | 1/3 (0.33333334) | 1280×720 | 853×480 |
| 4 | `..._UltraQuality` | *not exposed* | 0.77 | — | — |
| 5 | `..._DLAA` | `dlaa` | 1.0 | 3840×2160 | 2560×1440 |

Notes:

- `UltraQuality` is defined in the enum but has historically not been implemented by the
  runtime. It is deliberately absent from `r_dlss`'s `allowed_values`. **[unverified]** whether
  the optimal-settings query signals this by returning `0` dimensions; the header does not
  document it. The implementation must treat a `0` or a failed query as "mode unavailable" and
  fall back to `off` with a log line, not silently render at zero size.
- Pixel counts, for reasoning about cost: Quality 44.4 % of display pixels, Balanced 33.6 %,
  Performance 25 %, Ultra Performance 11.1 %.
- Dynamic resolution: NGX returns `min`/`max` bounds and supports DRS between 1/2 and 1.0 of
  the output. **DLSS-RR does not support DRS.** This plan uses a fixed per-mode render extent
  for both SR and RR — the accumulator, the ReSTIR reservoirs and the golden harness all
  assume a stable extent, and a per-frame-varying one would be a separate project.

### 3.1 How this maps onto `r_render_scale`

`r_render_scale` is the parallel workstream's knob and stays the user's knob **while DLSS is
off**. When `r_dlss != off`, the relationship inverts:

1. On mode change or swapchain resize, call `NGX_DLSS_GET_OPTIMAL_SETTINGS` with the
   swapchain extent.
2. Set the engine's render extent to exactly `optimalW × optimalH`. Do **not** compute
   `round(displayW * ratio)` — the feature is created with the queried size and any mismatch
   between the size DLSS was created for and the size it is fed is an error, not a rounding
   nuisance.
3. Write the resulting ratio back into `r_render_scale` so the console, the perf overlay and
   `config.cfg` all report the truth, and log once that DLSS is driving it.
4. User writes to `r_render_scale` while DLSS is on are accepted into the cvar but have no
   effect; log the reason once (same pattern as `Engine.cpp:19328`, where
   `r_svgf_atrous_passes` explains itself when `r_denoiser` makes it inert).

The engine already has one place that must know both extents: everything downstream of the
denoiser. §5 covers the pass ordering.

---

## 4. Jitter: what a stochastic path tracer owes DLSS

This is the design question the brief flagged, and the tree is further along than the brief
assumed.

### 4.1 What exists

```
Engine.cpp:10134-10151   Halton(2,3) sub-pixel jitter, host-computed, 16-frame period,
                         range [-0.5, 0.5] per axis, written to push.halton_jitter[2]
                         and mirrored into last_jitter_x_ / last_jitter_y_.
Engine.h:2266-2267       last_jitter_x_, last_jitter_y_.
src/rhi/Device.h:360-361 DenoiseDesc::jitter_x / jitter_y  -- plumbed to the RHI and
                         consumed by nothing. No Vulkan backend reads them.
PathTrace.slang:1149     float2 halton_jitter in the push struct.
PathTrace.slang:8388-8393
        if (denoiser_enabled != 0u && s == 0u) { jitter = halton_jitter; }
        else                                   { jitter = float2(randf(seed), randf(seed)) - 0.5; }
```

So: a deterministic, image-uniform, host-driven Halton offset already exists, is already
reported to the RHI, and is already the *only* jitter for the common `r_spp 1` +
denoiser-on case (`r_spp` default `1`, `Engine.cpp:407`).

### 4.2 Why "stochastic sub-pixel sampling" is not actually the problem

DLSS's contract is not "the renderer must be deterministic". It is: *this frame is a point
sample of the scene at sub-pixel offset `(jx, jy)`, the same offset for every pixel, and the
sequence of offsets over frames tiles the pixel footprint*. A path tracer satisfies that
trivially — the primary ray direction is chosen by the offset; everything stochastic after the
primary hit (BSDF sampling, NEE, the cloud march, ReSTIR) is *radiance* noise at a known
sample location, which is exactly what DLSS-RR is trained to reconstruct and what DLSS-SR
tolerates as long as the sample location is honest.

The engine breaks the contract in three specific places, and all three are fixable:

**Break 1 — `s > 0` samples use random offsets.** With `r_spp > 1` the frame stops being a
point sample at `halton_jitter` and becomes a partially box-filtered estimate over the pixel,
while the host still reports `halton_jitter`. The reported offset becomes a lie whose size
grows with `spp`.

*Fix:* when DLSS is active, use `halton_jitter` for **every** sample `s`. Multi-sampling then
reduces radiance variance at one sub-pixel location instead of pre-filtering the pixel. This is
not a loss: pre-filtering the pixel with a box is *worse* reconstruction than letting DLSS's
learned filter do it, and it destroys the very high-frequency information DLSS exists to
recover. Concretely, `PathTrace.slang:8388` becomes a three-way choice gated on a new push
flag: DLSS on → `halton_jitter` for all `s`; denoiser on, DLSS off → today's behaviour
(preserves existing goldens bit-for-bit); everything off → random (free AA via accumulation).

**Break 2 — the G-buffer ray is unjittered** (`PathTrace.slang:8000`). Covered as caveat A in
§2.1. When DLSS is active, `uv0` must carry the same `halton_jitter`.

**Break 3 — the phase count is hardcoded to 16.** `Engine.cpp:10148` uses
`(push.frame_index % 16u) + 1u`. NVIDIA's documented recommendation is a Halton sequence of
length `8 * (displayWidth / renderWidth)^2` — i.e. enough phases to cover the number of
render-res samples that must land inside one display pixel, times 8. At the modes in §3:

| Mode | ratio | phases = `ceil(8 / ratio^2)` |
|---|---|---|
| DLAA | 1.0 | 8 |
| Quality | 0.66667 | 18 |
| Balanced | 0.58 | 24 |
| Performance | 0.5 | 32 |
| Ultra Performance | 0.33333 | 72 |

*Fix:* derive the modulus from the actual render/display extents rather than hardcoding either
16 or the table — `phases = ceil(8.0 * (displayW / renderW)^2)`, recomputed whenever the extents
change. Keep 16 as the value when DLSS is off so nothing existing moves.

### 4.3 Sign and units

`InJitterOffsetX/Y` are in **render-resolution pixels** and must use the same direction
convention as the motion vectors. The engine's motion is `prev_pix - cur_pix` where the pixel
mapping flips Y (`float2(pc.x, -pc.y)`, `PathTrace.slang:8121-8127`), and the jitter is added to
a `uv` whose Y is also flipped (`PathTrace.slang:8001`). The composition of those two flips is
easy to get wrong on paper and trivial to settle empirically: a wrong jitter sign produces a
characteristic half-pixel shimmer on static high-contrast edges. Ship a bringup cvar
(§6, `r_dlss_jitter_y_sign`) rather than asserting a sign in this document, and delete the cvar
once the answer is known. `NVSDK_NGX_VK_DLSS_Eval_Params` also carries
`InIndicatorInvertXAxis` / `InIndicatorInvertYAxis` for exactly this diagnosis.

### 4.4 The accumulator

`accum_hdr`'s running mean is antithetical to DLSS, which does its own temporal accumulation
and expects a fresh per-frame estimate. The engine already handles this for the denoiser path:
`DenoiseDesc::color_in` is documented "per-frame, not accumulated" (`src/rhi/Device.h:259`), and
`r_accum_ema_alpha`'s docstring already says the accumulator is ignored when a denoiser is on
(`Engine.cpp:832-836`). DLSS inherits that contract unchanged. No work — recorded so nobody
re-derives it.

---

## 5. Ray Reconstruction versus the denoiser chain

**RR replaces a denoiser; it does not stack with one.** Streamline's RR guide states DLSS-RR
"completely overrides DLSS (Super Resolution)" — RR *is* the upscaler as well as the denoiser.
Feeding RR an already-denoised image is strictly worse than feeding it the noisy one: it was
trained on Monte-Carlo noise and a spatially filtered input has had the very structure it keys
on smeared away.

### 5.1 What `r_denoiser` does when RR is on

**`r_denoiser` is ignored, loudly, once.** But it must not be forced to `off` internally,
because `denoiser_active_` is load-bearing for a great deal that has nothing to do with
denoising:

- G-buffer allocation of `denoise_color` / `depth` / `motion` / `normal` / `albedo`
  (`Engine.cpp:8152-8162`).
- The celestials composite gate: `engine_composite_active` requires `denoiser_active_`
  (`Engine.cpp:9928-9936`).
- `cloud_trans_tex`, `godrays_mask_tex`, the SIGMA shadow-visibility buffer, and the
  ReSTIR dispatch gate all key off it.

So the correct shape is a **new `DenoiserKind`**, exactly the way MetalFX was modelled:

```
Engine.h:2259:  enum class DenoiserKind : std::uint8_t {
                    Off, MetalFX, SvgfBasic, SvgfAtrous, Nrd,
                    SvgfBasicMetalFx, SvgfAtrousMetalFx,
                    OptixHdr, OptixHdrAov,
                    OptixTemporalHdr, OptixTemporalHdrAov,
                +   DlssRayReconstruction,
                };
```

`denoiser_active_` stays true (machinery alive, G-buffers allocated), the SVGF / NRD / OptiX
dispatch is replaced by the RR evaluate, and the kind is selected by `r_dlss_rr`, not by
`r_denoiser`. `r_denoiser`'s value is latched but inert, and the engine logs it once on the
transition — the same courtesy `Engine.cpp:19328` already extends to
`r_svgf_atrous_passes`.

Rejected alternative: adding `dlss_rr` as an `r_denoiser` *value*. It cannot express
"Performance-mode SR with SVGF" versus "Performance-mode RR" — those are two independent axes —
and it does not match the two-control surface (mode selector + RR toggle) the owner asked for.

### 5.2 Pass ordering

With RR on, at **render** resolution:

```
PathTrace (jittered colour + jittered G-buffers)
  -> ReSTIR temporal/spatial/final            (unchanged)
  -> SIGMA shadow re-add, if enabled          (must happen BEFORE RR: RR wants the
                                               complete noisy radiance, and the demod
                                               re-add is a render-res operation)
  -> denoise_color  (noisy linear HDR, render res)
```

then RR, then at **display** resolution:

```
  -> RR output (denoised + upscaled linear HDR, display res)
  -> StarsComposite                            (MUST be post-RR -- see below)
  -> bloom pyramid
  -> tonemap + sRGB OETF -> swapchain
```

**Celestials must composite after RR, at display resolution.** Stars are sub-pixel point
sources with an energy-conserving PSF; running them through a neural reconstructor at render
resolution would smear or delete them, and upscaling them would fabricate structure. The engine
already has the right seam: `vulkan_dual_denoise` (`Engine.cpp:13446`) splits the Vulkan
denoise into `SvgfNoFinalize -> StarsComposite -> FinalizeOnly` precisely so the composite lands
between the denoiser and the tonemap. RR slots into the same split; what changes is that the
composite target moves from render res to display res.

*Note:* a comment near `Engine.cpp:12927` claims "the metal-only guard on
`engine_composite_active` keeps `push.composite_celestials = 0` on Vulkan". That comment is
**stale** — the gate at `Engine.cpp:9928-9936` has no backend term, and the Vulkan dispatch path
at `Engine.cpp:13446` exists. Worth correcting when the file is next touched; it will otherwise
mislead whoever implements this.

### 5.3 Coordination with the NRD workstream

RR and NRD are mutually exclusive consumers of the same G-buffers, and they want *different
encodings* of the same two quantities:

- NRD is configured with `NRD_NORMAL_ENCODING=2` (`R10G10B10A2_UNORM` octahedral) and
  `NRD_ROUGHNESS_ENCODING=1` (`cmake/Dependencies.cmake:197-198`, inside the
  `PT_ENABLE_NRD` block) — i.e. normal and roughness packed into one texture.
- RR wants unpacked: `RGB_F16` normals in one texture, single-channel roughness in another,
  with `normalRoughnessMode = eUnPacked`.

**The path tracer should keep writing the unpacked pair** (`normal_tex` `RGBA16F` at
`PathTrace.slang:122`, `roughness_tex` `R32F` at `PathTrace.slang:230`) and NRD should pack in
its own front-end pass — NRD ships `NRD_FrontEnd_PackNormalAndRoughness` for exactly this. If
instead the shader is changed to write NRD's packed form, RR has to unpack it and the two
integrations will fight over one texture forever. This is the one concrete request to make of
the NRD workstream, and it costs them one extra compute pass they were going to need anyway.

Second coordination point: both need `roughness_tex` allocated, and today neither can get it
(`Engine.cpp:8118`). Whichever workstream lands first should widen that gate to a
"which kinds consume the specular trio" list rather than a MetalFX enumeration, and add its own
kind to it.

---

## 6. Cvar surface

House style, per the existing block at `Engine.cpp:709-855`: long explanatory strings, real
numbers with their provenance, `CVAR_ARCHIVE` for anything the user would want persisted,
and `allowed_values` attached in the `Engine` constructor the way `r_tonemap_op` does it
(`Engine.cpp:2516-2525`) so the console rejects typos and the web console renders a dropdown.

### `r_dlss` — mode selector

```c
PT_CVAR(r_dlss, "off",
    "NVIDIA DLSS Super Resolution. The renderer traces at a reduced "
    "resolution and DLSS reconstructs the display-resolution image from "
    "the render-resolution colour plus depth, motion vectors and the "
    "frame's sub-pixel jitter offset. off = no upscaling; the path tracer "
    "renders at the swapchain size and r_render_scale is the user's knob. "
    "dlaa = render at native resolution and use DLSS purely as an "
    "anti-aliaser (ratio 1.0); costs GPU time rather than saving it, and "
    "is the highest-quality option. quality / balanced / performance / "
    "ultra_performance select NVIDIA's published presets, whose per-axis "
    "render ratios are 2/3, 0.58, 1/2 and 1/3 -- at 3840x2160 that is "
    "2560x1440, 2227x1253, 1920x1080 and 1280x720, or 44.4%, 33.6%, 25% "
    "and 11.1% of the display pixel count. Those ratios are documentation, "
    "not constants: the engine calls NGX_DLSS_GET_OPTIMAL_SETTINGS for the "
    "current swapchain size and uses whatever render extent it returns, so "
    "a future SDK that changes a ratio is followed automatically. While "
    "this is not off the engine OWNS r_render_scale and overwrites it with "
    "the queried ratio; writes to r_render_scale are latched but inert "
    "until DLSS is off again. Requires r_hdr_pipeline 1 (DLSS consumes "
    "linear HDR). Falls back to off with a log line if the build lacks "
    "PT_ENABLE_DLSS, if the GPU/driver does not support the feature, or if "
    "the optimal-settings query fails for the requested mode.",
    CVAR_ARCHIVE);
// allowed_values: {"off","dlaa","quality","balanced","performance","ultra_performance"}
```

`UltraQuality` is deliberately absent (§3). An `auto` value — NVIDIA's per-display-resolution
default (Quality at 1080p, Balanced at 1440p, Performance at 4K) — is a *user-experience
default table*, not a derived quantity, and is deferred rather than smuggled in as if it were
physics. If it is added later, the docstring must say plainly that it is a convention.

### `r_dlss_rr` — Ray Reconstruction toggle

```c
PT_CVAR(r_dlss_rr, "0",
    "NVIDIA DLSS Ray Reconstruction. 0 = DLSS upscales only, and whatever "
    "r_denoiser selects still denoises the render-resolution image first. "
    "1 = DLSS Ray Reconstruction REPLACES the denoiser entirely: it takes "
    "the raw noisy path-traced radiance plus the guide G-buffers (diffuse "
    "albedo, integrated specular reflectance, shading normal, roughness) "
    "and produces the denoised, upscaled display-resolution image in one "
    "step. It is not a post-denoise filter and must not be stacked on one "
    "-- a spatially filtered input has had the Monte-Carlo structure RR "
    "keys on smeared away. While this is 1, r_denoiser is latched but "
    "inert and the engine says so once. Requires r_dlss != off (RR is a "
    "DLSS feature; there is no RR-without-DLSS mode). r_dlss dlaa + "
    "r_dlss_rr 1 is the native-resolution ray-reconstruction "
    "configuration. Requires r_hdr_pipeline 1 -- RR only accepts HDR "
    "input. Does not support dynamic resolution, so the render extent is "
    "fixed per mode.",
    CVAR_ARCHIVE);
```

### `r_dlss_preset` — model/preset hint

```c
PT_CVAR(r_dlss_preset, "default",
    "Per-mode DLSS render-preset hint, forwarded as the NGX "
    "DLSS_Hint_Render_Preset_* parameter. 'default' lets the runtime pick, "
    "which is what shipping titles should do -- the preset letters denote "
    "specific trained models and which letters exist, and which are the "
    "current CNN vs transformer models, changes between SDK and driver "
    "releases. Letters a..k are accepted for A/B testing against a known "
    "driver; a letter the installed runtime does not implement falls back "
    "to the runtime default with a log line. Super Resolution and Ray "
    "Reconstruction carry SEPARATE preset namespaces and this cvar applies "
    "to whichever feature is active.",
    CVAR_ARCHIVE);
```

**[unverified]** which specific letters `v310.7.0` implements for SR and for RR. The
Streamline docs for an older SDK said RR supported only preset D; the DLSS 4 transformer
models introduced further letters. Do not encode a letter-to-model mapping in the docstring
until it is checked against the shipped headers.

### `r_dlss_auto_exposure`

```c
PT_CVAR(r_dlss_auto_exposure, "1",
    "How DLSS learns the scene's exposure. 1 = pass the "
    "NVSDK_NGX_DLSS_Feature_Flags_AutoExposure create flag and let DLSS "
    "meter the HDR input itself. 0 = hand DLSS the engine's exposure. The "
    "default is 1 because the engine's exposure_state is a POST-tonemap "
    "scalar in a storage buffer, not the pre-exposure texture NGX expects, "
    "and because this renderer's radiance is in physical units spanning "
    "many decades (a night sky and a noon desert differ by ~1e6), where a "
    "stale or mis-scaled exposure hint is worse than none. Set 0 only once "
    "the 1x1 R32F exposure texture exists and has been A/B'd. Ray "
    "Reconstruction ignores this flag entirely -- RR always self-meters.",
    CVAR_ARCHIVE);
```

### `r_dlss_jitter_y_sign` — bringup only

```c
PT_CVAR(r_dlss_jitter_y_sign, "1",
    "Bringup diagnostic: sign applied to the Y component of the jitter "
    "offset reported to DLSS. DLSS requires the jitter offset in the SAME "
    "direction convention as the motion vectors; this engine flips Y once "
    "in the ray-gen uv (PathTrace.slang) and once in the reprojection "
    "pixel mapping, and the composition of those flips is easier to settle "
    "by observation than on paper. A wrong sign shows as a half-pixel "
    "shimmer on static high-contrast edges with a still camera. Set to -1 "
    "to flip. DELETE THIS CVAR once the convention is confirmed on real "
    "hardware -- it is a question, not a setting.",
    0 /* not CVAR_ARCHIVE: a diagnostic should not persist into config.cfg */);
```

### Composition rules the engine must enforce (and log, once each)

| Situation | Behaviour |
|---|---|
| `r_dlss off` + `r_dlss_rr 1` | RR does not engage. Log: RR is a DLSS feature and needs a mode. |
| `r_dlss != off` + `r_hdr_pipeline 0` | DLSS does not engage. Log: DLSS consumes linear HDR. |
| `r_dlss != off` + user writes `r_render_scale` | Write latched, no effect. Log the reason once. |
| `r_dlss_rr 1` + any `r_denoiser` value | `r_denoiser` latched, inert. Log which value is being ignored. |
| `r_dlss != off`, `r_dlss_rr 0`, `r_denoiser != off` | Both run: denoise at render res, then SR upscales. This is the normal SR configuration and is not a conflict. |
| `r_dlss != off` on a build without `PT_ENABLE_DLSS`, or on unsupported hardware | Falls back to `off`, one log line naming which check failed. Same shape as the `optix_*` fallback at `Engine.cpp:7812` onward. |
| `r_spp > 1` with DLSS on | Allowed. All `spp` samples share the frame's Halton offset (§4.2). Log once that the jitter contract changed, because it changes pixel values. |

---

## 7. Licensing and vendoring

### 7.1 What the owner must accept

**DLSS SDK** — `github.com/NVIDIA/DLSS`, latest tag `v310.7.0` (2026-06-23). Licensed under
the **NVIDIA RTX SDKs License** (`LICENSE.txt` at the repo root; GitHub reports the SPDX id as
`NOASSERTION`, i.e. not a recognised open-source licence). The obligations that matter here:

1. **Redistribution is only as part of an application** with "material additional
   functionality, beyond the included portions of the SDK". A path tracer qualifies; a
   standalone redistribution of the DLLs does not.
2. **Attribution is mandatory** for DLSS/NGX: NVIDIA must be credited and the NVIDIA Marks
   shown "on splash screens, in the about box" and in credits. This engine has neither a
   splash screen nor an about box today — one has to exist before a public release with DLSS
   enabled. That is a real, non-obvious deliverable.
3. **Notification before commercial release** via NVIDIA's designated portal. Keyed to
   *commercial* release; a personal or non-commercial build does not trigger it, but the
   attribution requirement is not similarly conditioned.
4. **NVIDIA may update the SDK components over the air.** Relevant to determinism: see §9.

**Streamline** — `github.com/NVIDIA-RTX/Streamline`, `license.txt` is the **MIT licence**
verbatim. This does *not* make the DLSS features MIT: the `nvngx_dlss.dll` / `nvngx_dlssd.dll`
binaries Streamline loads are the same NVIDIA-RTX-SDK-licensed files, and
`sl_nvperf.h`/`sl_nvperf.dll` carry a further separate Nsight Perf SDK licence. Choosing
Streamline adds MIT source on top of the same licensed binaries; it does not reduce the
obligations above. Worth recording because "Streamline is MIT" is a commonly-drawn and wrong
conclusion.

### 7.2 Is FetchContent viable the way the NRD block does it?

**Mechanically yes; economically, only with a narrower fetch.**

The NRD block (`cmake/Dependencies.cmake:184-206`) does `FetchContent_Declare(nrd URL
.../NRD/archive/refs/tags/v4.17.3.tar.gz URL_HASH SHA256=... SYSTEM)` and then builds NRD from
source. Two structural differences for DLSS:

- **DLSS ships prebuilt binaries, not source.** There is nothing to compile. The consumption
  is: add `include/` to the include path, link one import library from
  `lib/Windows_x86_64/x64/`, and copy the runtime DLLs next to `demont.exe`. That is an
  `IMPORTED` target, not `FetchContent_MakeAvailable` of a subproject.
- **The binaries are real files in git, not LFS pointers.** Verified: the repo has no
  `.gitattributes` (HTTP 404), so a GitHub tag tarball contains the actual `.dll` / `.lib`
  payloads rather than LFS stubs. This is the thing that would have silently broken a
  `FetchContent(URL=...tar.gz)` approach, and it does not apply here.
- **The tarball is large.** Measured from the GitHub contents API on `main`:
  `lib/Windows_x86_64/rel/` alone is `nvngx_dlss.dll` 59.0 MB + `nvngx_dlssd.dll` 40.9 MB +
  `nvngx_dlssg.dll` 7.5 MB ≈ 107 MB; `dev/` (the debug-overlay variants) adds a further
  ≈125 MB; `lib/Windows_x86_64/x64/` import libraries are ≈5 MB each across six CRT variants;
  plus `uwp/`, the vs20xx directories and the whole `Linux_x86_64` tree. A full-tag
  `FetchContent` is a several-hundred-megabyte download for ~100 MB of files we actually use.

**Recommendation:**

```
option(PT_ENABLE_DLSS "Build NVIDIA DLSS SR / DLAA / Ray Reconstruction (Vulkan-only;
                       downloads NVIDIA-RTX-SDK-licensed binaries at configure time)" OFF)
```

Default **OFF for a licensing reason**, not a toolchain reason — unlike `PT_ENABLE_NRD`
(`CMakeLists.txt:58`), whose OFF default is about the ShaderMake/dxc-spirv chain
(`CMakeLists.txt:34-57`). Flipping this option causes CMake to download NVIDIA-licensed
binaries; the `message(STATUS ...)` line must name the licence and the URL so the act of
enabling it is the act of accepting it. Then, mirroring the NRD block's structure and comment
density:

- `PT_DLSS_ACTIVE = PT_ENABLE_DLSS AND PT_ENABLE_VULKAN_BACKEND`, checked by consumers, so a
  stale cache entry is harmless — the exact pattern `PT_NRD_ACTIVE` uses
  (`cmake/Dependencies.cmake:184-190`).
- `FetchContent_Declare(dlss URL https://github.com/NVIDIA/DLSS/archive/refs/tags/v310.7.0.tar.gz
  URL_HASH SHA256=<pin at bringup> SYSTEM)`. The hash must be pinned exactly as NRD's is
  (`cmake/Dependencies.cmake:201`); computing it is a bringup step, not something to guess here.
- Link `lib/Windows_x86_64/x64/nvsdk_ngx_d.lib` (dynamic CRT) — the build uses clang-cl with the
  dynamic runtime, so `nvsdk_ngx_s.lib` (static CRT) is the wrong one. **[unverified]** whether
  the `_dbg` variants are needed for the Debug preset; check at bringup.
- Copy `lib/Windows_x86_64/rel/nvngx_dlss.dll` and `nvngx_dlssd.dll` to the output directory
  via `cmake/SetupBinaries.cmake`, which already exists for exactly this kind of job. Do **not**
  ship `nvngx_dlssg.dll` — Frame Generation is out of scope and shipping an unused 7.5 MB
  licensed binary invites questions.

**Alternative worth considering if the download size annoys:** vendor `include/` plus the two
DLLs and one `.lib` under `third_party/dlss/` (the repo already vendors `cgltf`, `miniaudio`,
`stb` there). ~110 MB in git history is a real cost and the licence permits redistribution
only inside an application, which a source repo arguably is not. On balance: **prefer
FetchContent**, accept the download size, and keep the licensed bits out of the repo's history.

---

## 8. Dependency-ordered work plan

Stages are strictly ordered; each is independently shippable and independently revertible.
Effort bands match `docs/NEXTGEN_PLAN.md`: S ≤ 3 engineer-days, M 3–8, L 8–15.

### Stage 0 — Prerequisites (blocking, owned elsewhere)

- **P0.1** `r_render_scale` + render/display extent split lands (parallel workstream).
  DLSS cannot start before the engine can render at a size other than the swapchain's.
- **P0.2** Confirm on the target box which NGX instance/device extensions are required, by
  calling `NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements` and
  `...GetFeatureDeviceExtensionRequirements` for `SuperSampling` and for `RayReconstruction`,
  and logging the answer. Both must run before `vkCreateInstance` (`VulkanDevice.cpp:879`) and
  `vkCreateDevice` (`VulkanDevice.cpp:1772`). *(S, ~0.5 d, can be done as a throwaway probe.)*

### Stage 1 — Build plumbing and NGX bootstrap (S, 2–3 d)

`PT_ENABLE_DLSS` / `PT_DLSS_ACTIVE`, the FetchContent block, the imported target, the DLL copy
step, and an NGX init/shutdown that does nothing but `Init_with_ProjectID` →
`GetCapabilityParameters` → query `NVSDK_NGX_Parameter_SuperSampling_Available` and the RR
equivalent → log → `Shutdown1`. Deliberately mirrors what `VulkanNrdLibDenoiser`'s scaffolding
stage did (`src/rhi_vulkan/VulkanNrdLibDenoiser.h:12-25`): prove the link and the runtime
bootstrap before writing any per-frame code.

*Acceptance:* a `win-clang-release` configure with `-DPT_ENABLE_DLSS=ON` builds; startup logs
the DLSS SDK version, driver-capability verdict and the required extension lists; a build with
the option OFF is byte-identical to today's.

### Stage 2 — The jitter contract (S, 2–3 d) — **do this before any DLSS evaluate call**

Independent of DLSS, testable without it, and a latent-defect fix in its own right.

- Derive the Halton modulus from the extents: `ceil(8 * (displayW/renderW)^2)`, replacing the
  hardcoded `16` at `Engine.cpp:10148`. Keep 16 exactly when DLSS is off.
- New push flag "uniform jitter for all samples"; `PathTrace.slang:8388-8393` becomes the
  three-way choice in §4.2.
- Apply the frame jitter to the G-buffer ray at `PathTrace.slang:8000`.
- Fix the stale `depth_in` comment at `src/rhi/Device.h:260`.

*Acceptance:* with DLSS off, every golden in `tests/goldens/Windows/` is bit-identical
(the new paths are all behind the DLSS-active flag). With the flag forced on and no DLSS,
a static camera at `r_spp 1` shows the Halton sequence walking the sub-pixel grid in a
debug AOV, and the G-buffer normal at a high-contrast silhouette tracks the colour sample.

### Stage 3 — Super Resolution + DLAA (M, 5–8 d)

- `src/rhi/Upscaler.h` seam + `src/rhi_vulkan/VulkanNgxUpscaler.{h,cpp}`.
- `r_dlss` cvar, `allowed_values` attached at `Engine.cpp:2516`-style site, optimal-settings
  query driving the render extent, `r_render_scale` ownership inversion (§3.1).
- Display-resolution HDR output target; post-DLSS pass chain re-pointed at display res
  (StarsComposite, bloom, tonemap).
- Feature create/release on resize and mode change; `InReset` from `prev_frame_valid_`.
- `r_dlss_auto_exposure` (default 1 — no exposure texture yet).

*Acceptance:* `r_dlss quality` at 3840×2160 renders at 2560×1440 as reported by the perf
overlay, presents at 3840×2160, and the frame time drops; `r_dlss dlaa` renders at native and
is visibly cleaner than `r_denoiser off` on the same fixture; no shimmer on static
high-contrast edges (the jitter-sign check); the orbital fixtures show no ghosting under the
128 m/frame translation; goldens with `r_dlss off` unchanged.

### Stage 4 — Un-dead the specular guide G-buffers (S, 1–2 d)

Widen `want_specular_guidance_gbuffers` (`Engine.cpp:8118`) from a MetalFX enumeration to a
"kinds that consume the specular trio" list, and add the DLSS-RR kind. Nothing else changes:
the textures, the shader writes (`PathTrace.slang:8282/8292/8306`), the push flags
(`Engine.cpp:9770-9776`), the binds (`Engine.cpp:8907/8910/8913`) and the RHI fields
(`src/rhi/Device.h:299-301`) already exist and are correct as plumbing.

*Acceptance:* with the new kind selected, an RGBA readback of `denoise_roughness` and
`denoise_specular_albedo` shows plausible values (ocean smooth, terrain rough, metal F0 =
albedo, sky = roughness 1.0 / F0 0.0) rather than an unallocated handle.

### Stage 5 — The specular albedo derivation (S, 1–2 d)

Replace the raw-F0 write at `PathTrace.slang:8270-8282` with the split-sum integrated specular
reflectance `F0 * A(roughness, NdotV) + B(roughness, NdotV)` for the RR consumer, citing Karis
(SIGGRAPH 2013) / Lazarov's analytic fit in the comment, using the `h0.roughness` and
`dot(-rd0, hit_normal)` already in scope. Keep the raw-F0 form available for any consumer that
wants F0 proper.

*Acceptance:* the written value at grazing incidence on the ocean approaches 1 rather than
staying at 0.04; a unit test over the fit reproduces the reference split-sum table to the
documented tolerance of the approximation.

### Stage 6 — Ray Reconstruction (M, 5–8 d)

- `DenoiserKind::DlssRayReconstruction`, `r_dlss_rr` cvar, the composition rules in §6.
- Push world-to-view and view-to-clip separately.
- Wire the guide buffers into the RR evaluate; `pInSpecularHitDistance = nullptr` for now.
- Pass ordering per §5.2, with the celestials composite at display resolution.

*Acceptance:* on the 1-spp planet fixtures, RR output visually matches the 256-spp accumulated
reference within the thresholds the existing goldens use; stars survive (compare a night
fixture with `r_dlss_rr 0/1` — star count and PSF must be preserved because the composite is
post-RR); no boiling on the terminator; `r_denoiser` provably inert (flip it and diff two
frames — must be bit-identical).

### Stage 7 — Exposure texture, then specular hit distance (S + M, optional)

- 1×1 `R32F` exposure texture written alongside `exposure_state`, `r_dlss_auto_exposure 0`
  A/B'd against 1. Only worth doing if Stage 3/6 show adaptation lag or highlight instability.
- Either a real reflection-ray trace for `specular_hit_distance`, or specular motion vectors.
  A/B all three (proxy / real / specular-MVs / none) on the ocean and the metal hero scene
  before choosing; the current proxy stays out of the RR input until it wins that comparison.

---

## 9. Determinism, goldens, and honest limits

`docs/DETERMINISM_BASELINE.md` records 30/30 fixtures bit-identical run-to-run on the compute
path. DLSS cannot join that set, and pretending otherwise would poison the harness:

- The reconstruction is a closed neural network in `nvngx_dlss*.dll`, and the NVIDIA RTX SDK
  licence explicitly permits over-the-air updates to those components. Output therefore changes
  with the driver and with the DLL, without a repo change.
- The preset that the runtime picks under `r_dlss_preset default` is itself version-dependent.

**Policy:** no golden cell may be captured with `r_dlss != off`. `r_dlss` should be forced to
`off` in the capture path the same way the harness already pins seeds and denoiser kinds
(`Engine.cpp:14852`-region records `denoiser_label` into the capture name — DLSS mode belongs in
that label too, precisely so that an accidental DLSS capture is *named* differently and cannot
be mistaken for a reference). This is also the strongest independent argument for finishing
NRD: RR is the better-looking denoiser, NRD is the *checkable* one.

### Things this document could not verify and that must be checked at bringup

1. Whether NGX SR accepts linear view depth without a flag, and how it treats the `1.0e10`
   sky sentinel (`PathTrace.slang:8116`).
2. Whether `NGX_DLSS_GET_OPTIMAL_SETTINGS` signals an unsupported mode with zero dimensions.
3. Whether DLSS-RR ignores the alpha channel of the diffuse-albedo texture (the engine stores
   the aerial-perspective demod guide there, `PathTrace.slang:8234`).
4. Whether `pInAlpha` / `pInOutputAlpha` in `NVSDK_NGX_VK_DLSSD_Eval_Params` are mandatory.
5. Whether `h0.normal` (`PathTrace.slang:8060`) is the normal-mapped shading normal on every
   material path, or the geometric normal on some.
6. Which render-preset letters `v310.7.0` implements, for SR and for RR separately.
7. The jitter Y sign (§4.3) — settled by observation, not by reading.
8. Whether the Debug preset needs the `_dbg` import libraries.

---

## 10. Sources

- NVIDIA DLSS SDK, `github.com/NVIDIA/DLSS`, tag `v310.7.0` (2026-06-23): `include/nvsdk_ngx_defs.h`
  (`NVSDK_NGX_PerfQuality_Value`, `NVSDK_NGX_Feature_RayReconstruction = 13`),
  `include/nvsdk_ngx_vk.h` (the `NVSDK_NGX_VULKAN_*` surface and the "call before
  `vkCreateInstance` / `vkCreateDevice`" requirement), `include/nvsdk_ngx_helpers.h`
  (`NGX_DLSS_GET_OPTIMAL_SETTINGS`), `include/nvsdk_ngx_helpers_vk.h`
  (`NVSDK_NGX_VK_DLSS_Eval_Params`), `include/nvsdk_ngx_helpers_dlssd_vk.h`
  (`NVSDK_NGX_VK_DLSSD_Eval_Params`), `LICENSE.txt` (NVIDIA RTX SDKs License).
- NVIDIA Streamline, `github.com/NVIDIA-RTX/Streamline`, tag `v2.12.0` (2026-06-23):
  `docs/ProgrammingGuideDLSS.md` (required tags, `slDLSSGetOptimalSettings`, auto-exposure),
  `docs/ProgrammingGuideDLSS_RR.md` (RR input tags, `EnvBRDFApprox2`, `normalRoughnessMode`,
  "completely overrides DLSS (Super Resolution)", no DRS, HDR requirement), `license.txt` (MIT).
- NVIDIA DLSS Programming Guide (jitter in pixel space, Halton recommended, sequence length
  `8 * (native/render)^2`, jitter shares the motion-vector direction convention).
- Karis, "Real Shading in Unreal Engine 4", SIGGRAPH 2013 course notes — split-sum
  environment BRDF, the basis for the specular-albedo derivation in Stage 5.
- In-repo: `cmake/Dependencies.cmake` (NRD FetchContent pattern), `CMakeLists.txt:31-59`
  (option style), `docs/NEXTGEN_PLAN.md` §2 item 5 (the Streamline proposal this dissents from)
  and §4a, `docs/DETERMINISM_BASELINE.md`, `AGENTS.md` (NVIDIA-exclusive platform charter).
