# Step 1 design: escape the megakernel (VK_KHR_ray_tracing_pipeline + SER + pipeline libraries)

Repo `F:\MyRepos\vkdemont` @ `integrate/polish-vulkan` (0f55539). All line numbers below are against the tree as read on 2026-09-06; `shaders/PathTrace.slang` is 11,453 lines today (the NEXTGEN_PLAN numbers are ~220 lines stale because the polish branches landed after it was written). Target box: RTX 5090, driver 616.56, Vulkan 1.4.351, SDK 1.4.341.1, Slang 2026.8 (bundled). Nothing here has been built or edited; this is a design.

---

## 1. Summary and the go/no-go probe

**What changes.** The one 11,453-line compute megakernel (`main()` at `PathTrace.slang:7889`, spp loop L8283, bounce loop L8561, `traceScene` L3282) becomes a `VK_KHR_ray_tracing_pipeline` with one **raygen** (the per-pixel driver: G-buffer pass, spp loop, bounce-loop control, the five software intersection tiers, accumulation and every output write), **two closest-hit** shaders (terrain, mesh: geometry + material decode for TLAS triangles), **four miss** shaders (one per sky mode, selected by `MissShaderIndex`), **four "software-hit" miss records** (planet body, ocean shell, analytic prim, SDF -- the software tiers' shading entries, reached through `HitObject::MakeMiss(index)` so they participate in reordering without the NV-only `MakeHit`), and **ten callables** (Lambert / metal / dielectric / water BSDF blocks, the cloud march, the atmospheric in-scatter march, the underwater in-scatter march, `skyPhysical`, the shadow/transmittance chain, and the SDF sphere-trace). Modules are assembled from four `VK_KHR_pipeline_library` libraries so a shader edit rebuilds one library, and each module is small enough for the driver to compile at `-O2`. Shadow rays stay inline `RayQuery`. SER (`VK_EXT_ray_tracing_invocation_reorder`, plus the NV extension enabled for Step 2's `GetClusterID`) reorders on the merged HitObject with a 5-bit hint at the per-bounce point and a 3-bit hint at a second, per-sample point before the volumetric marches.

**What does not change.** The shared descriptor "god" layout (`VulkanDevice.cpp:1344-1605`), the 112 B push prefix + 2048 B `Frame` UBO spill (`VulkanDevice.h:356-396`), the per-dispatch descriptor ring, the `Frame` UBO slice ring, the pipeline cache file, the engine's slot tables, every G-buffer write, the accumulation math, the PRNG seeding (`pcgHash(tid, frame_index)` L7894) -- and therefore, in phase 1a, every pixel.

**The go/no-go probe (2 days, day 1-2 of 1a).** Before any module split, compile the *raygen-only* port (the 1a skeleton: full `main()` + software tiers + `HitObject.TraceRay` against stub CH/miss) at **`-O2`**, and answer the one question the plan flags as the central unknown: does a raygen that is still ~50% of the old megakernel re-trip the driver miscompile? Three escalating variants (P0 loop skeleton + HW trace + Lambert; P1 + all BSDF branches + `transmittance`; P2 = full `main()`) each go through: `vkCreateRayTracingPipelinesKHR` must return inside 120 s (the `[noinline]` experiment made pipeline creation never finish, `cmake/Slang.cmake:58-60`); 16x16 for 60 frames with no event 153 / `VK_ERROR_DEVICE_LOST` (`Device::IsDeviceLost`, `Device.h:250`); then 1280x720. Decision rule in section 10. If P2 passes, 1a ships at `-O2` as a bonus and 1b is about performance, not survival; if only P0/P1 pass, the marches and the water branch are pulled forward into callables during 1a; if P0 fails, the loop skeleton itself is what the driver miscompiles, that is the minimal bug report NVIDIA needs, and the wavefront plan B (section 10) starts immediately.

---

## 2. Frame graph before / after

Today (Vulkan, `r_denoiser svgf_atrous` + star split, the common dev path). Pass labels are the `GpuPassMark` strings in `Engine.cpp`; the dashed box is dead code on Vulkan (`use_engine_tonemap = ... && backend_is_metal`, `Engine.cpp:12873-12877`, block at L13527-14290).

```
 OceanCascades (untimed, L8540)
   |
 [PathTrace]  compute 8x8, PathTrace.slang main()          L8553-12213
   |  writes: accum_hdr, denoise_color, depth/motion/normal/albedo(.a=guide T),
   |          cloud_trans_tex, shadow_vis_buf, reservoir_curr_buf, output(swap, denoiser-off)
   +-> [CloudsRaymarch] (r_clouds_mode==raymarched only)   L12242
   +-> [RestirTemporal] [RestirSpatial] [RestirFinal]      L12500/12619/12674
   +-> [AutoExpose]                                        L12753
   +-> Denoise(SvgfNoFinalize) -> post_denoise_hdr          L13465
   +-> [StarsComposite]                                    L13308/13470
   +-> [Bloom] pyramid                                     L13075
   +-> Denoise(FinalizeOnly): bloom + tonemap_op + sRGB -> swapchain   L13476-13479
   +-> [EditorOverlay] [PerfOverlay]                       L14571 / L14780
   .
   . - - - - dead on Vulkan (use_engine_tonemap == false) - - - - - - - - - - - -
   . [SigmaShadow] [HeightFog] [CloudsComposite] [Aurora] [ParticleComposite]
   . [GodRays] [Tonemap]                                   L13576 ... L14272
   . - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
```

After Step 1 (1b/1c). Only the first box changes shape; everything downstream is untouched in Step 1 (section 11 says what the follow-up wires).

```
 OceanCascades
   |
 [PathTrace]  vkCmdTraceRaysKHR(w, h, 1)  -- ONE pipeline, 4 libraries
   |
   |  raygen  PathTraceRaygen.slang
   |    G-buffer pass (unjittered primary, guide T)         <- was L7900-8225
   |    for s in spp:                                       <- L8283
   |      for b in bounces:                                 <- L8561
   |        sw tiers: planes, planet body, ocean shell, analytic BVH  (raygen)
   |                  SDF clusters -> CallShader(kCallSdf)   (only if sdf_params.x>0)
   |        HitObject ho = HitObject.TraceRay(tlas, ..., TMax = t_sw)
   |        merge by t  -> ho | MakeMiss(kMissSw*) | MakeMiss(sky_mode) | MakeNop
   |        ReorderThread(ho, hint5, 5)                      (1c)
   |        HitObject.Invoke(ho, payload)  ->  CH terrain | CH mesh | miss sky | sw-hit entries
   |        ocean shell displacement, aerial-perspective bookkeeping   (raygen)
   |        CallShader(kCallBsdf[mat], io)  -> NEE/ReSTIR/sample; inside: CallShader(kCallVisibility) for shadow rays (RayQuery)
   |      ReorderThread(hint3, 3)                            (1c, once per sample)
   |      CallShader(kCallAtmoInScatter) / (kCallCloudMarch) / (kCallUnderwater)
   |    accumulate, sanitize, write accum_hdr/denoise_color/G-buffers/output
   |
   +-> (unchanged) CloudsRaymarch, ReSTIR x3, AutoExpose, Denoise, StarsComposite, Bloom,
       FinalizeOnly, overlays.  Barrier after TraceRays: RAY_TRACING_SHADER -> COMPUTE (new Stage enum)
```

---

## 3. Module decomposition

Sizing method: the `-O0` megakernel SPIR-V is 336 KB with functions out-of-line; `-O1+` inflates it to 6.6 MB by inlining everything into `main` (`cmake/Slang.cmake:50-56`), a 20x inflation dominated by loop unrolling and by the same helpers being inlined at many call sites. "Est. SPIR-V" below is the module's share of the 336 KB out-of-line mass scaled by an inflation factor of 3-6x for its own inlining at `-O2` (the raygen keeps the most call sites, so it takes the top of that range). These are estimates to be replaced by the probe's `spirv-dis` byte counts and `VK_KHR_pipeline_executable_properties` register/spill statistics (enable it in debug builds; `vkGetPipelineExecutableStatisticsKHR` on NVIDIA reports register count and spill bytes per stage -- the instrument for every "register pressure" claim below).

| # | Module (file) | Stage / SBT record | Contents | Absorbs (PathTrace.slang) | Src lines | Est. SPIR-V @-O2 | Why this boundary |
|---|---|---|---|---|---|---|---|
| R | `PathTraceRaygen.slang` | raygeneration, 1 record | pixel setup + seed (L7889-7894); G-buffer pass (L7900-8225) incl. `guide_trans` (L8130-8135); spp loop (L8283-11229): jitter, hero wavelength, medium init (L8444), bounce loop control (L8561), software tiers (planes L3351, planet body L3397-3448, ocean shell L3503-3531, analytic BVH L3534-3566), HW trace + merge, ocean displacement (L8588-8628), primary_t/shadow-vis (L8630-8706), water segment Beer-Lambert (L8734-8744), miss-branch control flags (MIS/skip/ocean_sun_nee L8770-8873), cone accumulation (L8883-8890), `applyPbrTextures`/`resolveMatExt` (L8898-8905), emission/outline (L8920-8950), clearcoat + SSS (L8967-9033, rare), ReSTIR stamp (L9044), BSDF dispatch via CallShader, aerial perspective (L10015-10053), volumetric gates (L10088-10091, L10510-10512), spectral fold (L11216-11228); accumulate + every output write (L11230-11452) | ~3,600 (+~700 moved helpers) | 0.8-1.5 MB (P2 probe measures) | Must own per-path registers (throughput, radiance, seed, cone, medium, MIS flags, accumulators) and every `RWTexture2D` write; the plan's 1a contract is "raygen = today's main()" so the A/B is bitwise |
| H1 | `PathTraceHitTerrain.slang` | closesthit, hit group 1 | `terrain_indices`/`loadTerrainVert` fetch + bary interpolation, `terrainSurfaceAlbedo` (L1044, land-cover raster `ptLandAlbedoSample` L874 + snowline), `terrainRoughnessFold` (L797, needs cone width: passed in via payload), writes `PtPayload` | L3645-3699, L797-1043 | ~350 | 60-120 KB | Divergence-free (every lane hits terrain), memory-bound (3 vertex fetches + raster taps); the natural SER "bucket" and the Step-2 landing site for ClusterID |
| H2 | `PathTraceHitMesh.slang` | closesthit, hit group 0 | mesh position/UV fetch, face normal via `ObjectToWorld`, per-instance material from `InstanceDesc` (L744-785), writes `PtPayload` | L3702-3909 | ~250 | 40-80 KB | Separate from terrain so the terrain CH never links the CSG mesh path |
| M0-M3 | `PathTraceMissSky.slang` (4 entries) | miss records 0..3 | M0 gradient + HDRI (`env_map` bilinear, L7011-7041; gradient L7004-7005), M1 procedural (`procSky` L4394, `sunDisc` L4546, `moonDisc` L4651, `starsOnly` L3933, `procSkyInlineSunDiscAndHalo` L4257), M2 Hosek (`hosekSky` L4322 + discs), M3 physical (`CallShader(kCallSkyPhysical)` + `starsPhysical` L6938 + `sunDiscPhysical` L6855 + moon). Each takes 2 payload flag bits (peel celestials for `composite_celestials_active` L8838-8859; `skyColorNoSunDisc` for `ocean_sun_nee_prev` L8869) and writes `payload.radiance` | L6962-7117 (`skyColor`, `skyColorNoSunDisc`), L8837-8870, sky helpers L3933-4810 | ~1,900 total (shared `PathTraceSky.slang` module) | 30-250 KB each (M3 smallest: the march is a callable) | `sun_and_mode.w` is dispatch-uniform, so `MissShaderIndex = mode` costs nothing and keeps four small modules instead of one with a switch; the `skyColorBase` ambient tap (L7055) and HDRI haze (L10051) call the same helpers from the callables |
| S0-S3 | `PathTraceSwHit.slang` (4 entries) | miss records 4..7 ("software hits") | S0 planet body shading record (albedo from land cover L3432-3437, Lambert), S1 ocean shell (MAT_WATER record + `oceanRayMarchShell` L7797-7883 + `oceanCascadeResolvedAll`/`oceanBrdfAlpha2` L7498-7519 -> t/normal/foam/alpha2), S2 analytic prim (re-runs `testAnalyticPrim` L3002 for the single winning index to fill the full `HitInfo` incl. emission/prim_id/uv/tex tiles), S3 SDF (normal + material from the winning cluster). Input: the raygen's compact `SwHit {t, kind, index}` in the payload; output: `PtPayload` | L3397-3448, L3503-3531, L8588-8616, L3002-3098 | ~500 | 40-150 KB | Lets the software tiers be *reordered* with the EXT extension: `MakeMiss(kMissSw*, ray)` is portable (`shader-execution-reordering.md` L219-234), `MakeHit` is NV-only (L156) and requires a valid TLAS primitive, which the planet sphere is not |
| C0 | `PathTraceCallBsdf.slang::callLambert` | callable 0 | analytic-light NEE + ReSTIR WRS (L9070-9189), env-map NEE + MIS (L9191+), sun NEE + sigma capture, `skyColorBase` ambient tap (L9445), cosine sample (L9449-9461). Shadow rays via `CallShader(kCallVisibility)` | L9049-9462 | ~420 | 150-300 KB | The largest and most divergent shading block; everything after `h.mat == MAT_LAMBERT` |
| C1 | `...::callMetal` | callable 1 | isotropic mirror + `sampleAnisoGgx` (L2733) | L9463-9488 | ~60 (+aniso 70) | <40 KB | Tiny; separate so the metal path never links NEE |
| C2 | `...::callDielectric` | callable 2 | Cauchy IOR, Fresnel, refract, medium push/pop | L9489-9560 | ~80 | <40 KB | as above |
| C3 | `...::callWater` | callable 3 | water BRDF, Fresnel/Snell, foam, Cox-Munk rough lobe, water-leaving radiance setup, medium stack | L9562-9990 (+ helpers L7293-7349) | ~450 | 100-200 KB | Second-largest branch; only ocean pixels take it |
| C4 | `PathTraceCallMarch.slang::callCloudMarch` | callable 4 | the inline cloud/smoke march (`cloud_density_at` via the existing `PathTraceCloud` module, light steps, multi-scatter octaves, `trans_eye_c`) -> {v_color_c, trans_eye_c} | L10513-11000 | ~490 | 150-300 KB | Iterative, register-heavy, ~10% of pixels at the horizon: the textbook SER/callable candidate; plan item 8 wants it as a callable anyway |
| C5 | `...::callAtmoInScatter` | callable 5 | Mie/Rayleigh haze march (`trans_eye`, Cornette-Shanks/HG phase, per-sample `transmittance` via C8) -> v_color | L10092-10500 | ~400 | 100-200 KB | Runs on every hit pixel in procedural/physical modes; separate from the cloud march because their sampling rates differ (L10054-10063) |
| C6 | `...::callUnderwaterInScatter` | callable 6 | Planetary P7 underwater in-scatter march | L11005-11206 | ~200 | 60-120 KB | Only submerged-camera pixels |
| C7 | `PathTraceSky.slang::callSkyPhysical` | callable 7 | `skyPhysical` shell march (L6743-6826, loop L6793) | L6743-6826 | ~90 (+atmo helpers L6300-6660) | 60-120 KB | Called from M3 (full budget), from C0's ambient tap at 1/4 budget (L7066-7067) and from C3's sky reflection; one implementation, three callers |
| C8 | `PathTraceCallVisibility.slang::callTransmittance` | callable 8 | `transmittance()` (L7132-7274): cloud optical depth (L6223), the `kMaxRefract` dielectric/water chain, each leg = software tiers + inline `RayQuery` (L3620-3634); returns float3 | L7132-7274, L3282-3634 (tier copy), L6223-6300 | ~800 | 150-300 KB | One home for the shadow-ray version of `traceScene` so the four BSDF callables and C5/C6 do not each link the tiers + SDF code; nested CallShader from a callable is legal (DXR and SPIR-V both allow `CallShader`/`OpExecuteCallableKHR` in the callable stage) |
| C9 | `PathTraceCallSdf.slang::callSdfTrace` | callable 9 | `traceSdfClusters` (L3160-3260) + `SdfPrimitives`/`SdfFractals` modules (1,444 + 397 lines, `[unroll(SDF_MAX_NODES)]` L3130) -> `SwHit` | L3100-3260 | ~160 (+1,841 module) | 200-600 KB | Iterative sphere-trace with unrolled node loops is the single biggest `-O2` inflater in the tiers; invoked only when `sdf_params.x > 0` (dispatch-uniform, zero cost on every planet fixture); keeps ~1,800 lines out of the raygen |

Total: 1 raygen + 2 CH + 8 miss-record entries + 10 callables = 21 shader groups.

What is deliberately **not** split: clearcoat (L8967-8999) and SSS (L9011-9033) stay in raygen -- 70 lines, gated on `mat_ext` bits that no planet fixture sets; `applyPbrTextures`/`resolveMatExt` stay in raygen because they mutate `HitInfo` between the hit and the BSDF and are cheap.

---

## 4. Payload, attributes, HitObject, and what stays in raygen registers

**Payload (`PtPayload`, 48 B, `maxPipelineRayPayloadSize = 48`).** Written by H1/H2/S0-S3/M0-M3, read once by raygen and expanded into the existing `HitInfo` (L2540-2601, 32 dwords / 128 B) which the rest of the loop keeps using unchanged.

```slang
struct PtPayload {                 // 48 B, 12 dwords
    float3 normal;        // 12  world-space shading normal exactly as L3657 / L3718 compute it
    float  t;             //  4  hit distance (== HitObject.GetRayTCurrent for HW hits; sw t for S0-S3)
    float3 albedo;        // 12  linear RGB (terrain: land cover + snowline; mesh: desc or atlas-ready flat)
    float  roughness;     //  4  hit-specific (terrain fold with cone width, L3686-3687)
    float2 uv;            //  8  mesh UV for the atlas (0 for terrain); S1 reuses .x = foam, .y = alpha2
    uint   kind_mat;      //  4  [0:3] hit kind {0 miss,1 terrain,2 mesh,3 planet,4 ocean,5 analytic,6 sdf,7 nop}
                          //     [4:7] MAT_*, [8:15] mat_ext bits, [16:23] flags (in: peel-celestials,
                          //     no-sun-disc, is-primary; out: has-emission), [24:31] reserved
    uint   inst_or_cluster; // 4  InstanceID today (raygen re-reads InstanceDesc for ior / tex_tiles / uv_scale /
                          //     sigma2, L744-785); Step 2 stores HitObject.GetClusterID() here
};
```
Miss shaders reuse the same struct: `normal.xyz` carries the sky radiance, `t = 1e30`, kind = 0. `emission`, `prim_id`, `tex_tiles`, `uv_scale`, `tex_lod`, `ior` and the Wave-9 lobe params are **not** in the payload: for HW hits they are per-instance constants re-read from `instance_desc` (a 96 B fetch that every lane in the chunk shares, L763-785); for analytic prims S2 re-runs `testAnalyticPrim` for the one winning index and the raygen unpacks its full record. That is what keeps the payload at 12 dwords instead of the 32 of `HitInfo`. `guide_trans` for the SVGF demod (`albedo_tex.a`, L8130-8135) is computed in raygen from `h0.t` and never crosses a stage boundary.

**Hit attributes.** Only triangles are traced in Step 1: `BuiltInTriangleIntersectionAttributes` (float2 barycentrics, 8 B) against the 32 B `maxRayHitAttributeSize` verified in the plan; `maxPipelineRayHitAttributeSize = 8`. No custom intersection shaders, so Slang's unsupported `SV_IntersectionAttributes` on SPIR-V (`a2-01-spirv-target-specific.md:78`) is irrelevant until analytic prims move into AABB geometry (section 5).

**HitObject.** Opaque, driver-owned. Exactly one live per bounce iteration, assigned on every control-flow path (a hard rule under the NV extension, `shader-execution-reordering.md:14`; harmless under EXT). Software tiers produce `MakeMiss(kMissSw*, ray)` / `MakeNop()`, never a HitObject copy.

**Callable IO structs** (`CallableDataKHR`; ≤ 128 B each). `LambertIO` (in: hit_pt, nf, albedo, throughput, seed, medium, cone {width, spread}, shutter_t01, b, pixel index, flags; out: radiance_add, sample_sun_direct/shadowed, new ro/rd, throughput, seed, prev_lambert_brdf_pdf, flags) = 124 B. `VisibilityIO` (ro, rd, t_max, shutter_t01, cone, start_medium -> float3) = 44 B. `MarchIO` (primary_ro, primary_rd, primary_t, seed, sun_elev, phase params, out v_color + trans) = 64 B.

**Raygen-resident per-path state (registers), never in a payload.** seed (1 dword), throughput (3), radiance (3), ro/rd (6), cone width+spread (2), medium stack (1, L2184-2270), flag word (skip_sky_on_miss, prev_was_lambert_nee, ocean_sun_nee, wl_pending, primary_in_water: 1), prev_lambert_brdf_pdf (1), hero_lambda + spectral_weight (4), wl_amp/wl_rate (6), sample_sun_direct/shadowed (6), primary_ro/rd/t (7), per-frame accumulators frame_radiance/sun x2/cloud_trans/shadow_vis (11) = ~52 dwords, plus the expanded `HitInfo` (32) live only between Invoke and the BSDF call. The **ReSTIR reservoir** (64 B, `RestirReservoir.slang:47-65`) is built and stored inside the Lambert block (L9112-9177) and never survives a bounce -- it costs C0 registers, not raygen registers.

---

## 5. The five software intersection tiers: merge design

Decision for 1a: **(a) keep all five in raygen and merge by `t` with the hardware HitObject**, evolving to a hybrid (c) later. Concretely, per bounce:

```slang
HitInfo h = defaults;                       // L3284-3325 verbatim
// tier 1-4 exactly as today: planes L3351, planet body L3397-3448, ocean shell L3503-3531,
// analytic BVH L3534-3566  -> h.t, h.mat, sw_kind, sw_index
if (sdf_params.x != 0u) { SdfIO io = {ro, rd, t_min, h.t}; CallShader(kCallSdf, io); if (io.hit && io.t < h.t) {...} }   // tier 5
RayDesc r = { ro_mesh, t_min, rd, h.t };    // TMax = current closest, as RayQuery does at L3625
PtPayload p; HitObject ho = HitObject::MakeNop();
if (tlas_present != 0u)
    ho = HitObject::TraceRay(scene_tlas, RAY_FLAG_NONE, 0xFFu, /*sbtOffset*/0u, /*sbtStride*/1u, sky_miss_index(), r, p);
if (ho.IsHit()) { /* HW closest under TMax: wins by construction */ }
else if (sw_kind != kNone) ho = HitObject::MakeMiss(kMissSwBase + sw_kind, r);   // planet/ocean/analytic/sdf shading entry
else if (needs_sky)        ho = HitObject::MakeMiss(sky_miss_index(), r);        // the traced miss, or MakeMiss when tlas absent
else                       ho = HitObject::MakeNop();                            // e.g. underwater void L8760-8767
ReorderThread(ho, hint5, 5u);              // 1c only
HitObject::Invoke(scene_tlas, ho, p);      // CH / sky miss / sw-hit entry fills p; raygen expands p -> HitInfo
```
`TMax = h.t` is the same pruning `RayQuery` performs today (L3625), so the HW result under that bound is the same committed triangle; the `mesh_shift` origin (L3615-3618) is applied identically. In 1a the raygen decodes the HW hit itself from `ho.GetInstanceID()/GetPrimitiveIndex()/GetAttributes<BuiltInTriangleIntersectionAttributes>()/GetRayTCurrent()` with the L3635-3909 code verbatim (no CH content, no Invoke), which is the plan's "compact hit record" without a payload round trip; 1b moves that decode into H1/H2 and turns the accessor reads into `Invoke`.

**Planet sphere and ocean shell** stay software forever. Both are "infinite-extent" singletons by design (L3364-3367: a 6,371 km AABB around every leaf ruins the BVH; L3501-3502) -- an AABB in the TLAS would be entered by every ray and would run an intersection shader for exactly the two `intersectSphere` calls the raygen does now, with worse register behaviour (intersection shaders run inside traversal, where SER cannot help). Their shading, however, does move out of the raygen into S0/S1 so they are reordered as their own buckets.

**SDF sphere-trace** stays software but leaves the raygen as callable C9: it is iterative (`iter_budget` L3163, per-cluster AABB slab test then a march, fractal dispatch L3180-3230) and its `[unroll(SDF_MAX_NODES)]` node loops (L3130) are the worst `-O2` inflater in the tiers; an intersection shader would put that loop inside traversal. As a callable it is invoked only on the uniform `sdf_params.x != 0` branch, so every planet fixture pays nothing.

**Analytic prims (finite spheres/boxes/quads) and the analytic BVH** are the tier that *should* become procedural AABB geometry with an intersection shader (one AABB per prim, `ReportHit` with a small attribute struct, hit group 2) -- later, once the SBT exists; that removes `testAnalyticPrim` + the 32-deep BVH stack (L3545-3565) from the raygen and lets the hardware cull them. Not in Step 1 because the fixtures that exercise them (`cornell_csg`, `sdf_smin_row`, `pbr_textured`, the only three Vulkan golden cells, `tests/CMakeLists.txt:1332,1344,3227`) are the ones the A/B relies on being untouched.

---

## 6. SER hint specification (1c)

Ground rules from the bundled doc (`shader-execution-reordering.md:774-805`): the HitObject is the primary key (shader-table index = hit kind / sky mode / software kind in this SBT, plus instance and primitive for locality); `CoherenceHint` is secondary; use the fewest bits that enumerate the values; every thread must pass the same bit count (max 16). NVIDIA's whitepaper guidance is the same: hint bits should encode divergence the HitObject cannot see.

**Reorder point A -- per bounce, after the merge, before `Invoke`** (the material decode and the BSDF callable both run coherent):

| bit | meaning | source | why it predicts divergent work |
|---|---|---|---|
| 0 | `b == 0` (primary segment) | loop counter | ReSTIR stamp/WRS (L9044, L9101), sigma shadow-vis ray (L8666), unclamped vs `clampIndirect` paths, G-buffer-related writes |
| 1 | `ptMediumIsWater(medium)` | medium stack | water Beer-Lambert (L8741), underwater sky override (L8749-8767), `transmittance` start medium |
| 2 | cloud march pending this sample | `b==0 && inline_cloud_march_active && cloudLayerInterval(...)` (L10535: one slab/shell test, cheap) | the 490-line march is the single most expensive divergent block and only horizon/cloud pixels take it |
| 3-4 | material class {Lambert, metal, dielectric, water} | terrain/planet -> Lambert, ocean -> water, mesh -> `asuint(instance_desc[6*ho.GetInstanceID()+2].x)` (16 B read, L778) | selects C0..C3; the HitObject only distinguishes hit *kind* |

`ReorderThread(ho, hint, 5u)`. Sky mode and `denoiser_enabled` are dispatch-uniform (push/Frame constants) and carry no entropy; bounce depth beyond bit 0 changes only clamping.

**Reorder point B -- once per sample, before the volumetric block** (L10088), `ReorderThread(hint, 3u)` with no HitObject (the two-argument overload, doc L797-799):

| bit | meaning | gate it mirrors |
|---|---|---|
| 0 | atmospheric in-scatter march will run | `(march_mie || march_ray) && span_ok_v` (L10314) |
| 1 | cloud/smoke march will run | `inline_cloud_march_active && t_out > t_in` (L10513, L10569) |
| 2 | underwater in-scatter will run | Planetary P7 gate (L11005) |

Point B exists because the marches run *after* the bounce loop, per sample, so point A's grouping has been scrambled by intervening bounces; moving the cloud march before the loop would change the order of `randf(seed)` calls and break the pixel-identity acceptance. Two reorders per bounce-1 sample and one per later bounce is inside NVIDIA's "a few per ray" guidance; a `r_pt_ser 0|1` push flag skips both calls (`ReorderThread` is a no-op statement, the pipeline is the same) so the ≥20% claim is measured in one build.

---

## 7. SBT, pipeline libraries, cache, and the RHI API

**Shader groups and SBT layout** (handle 32 B verified; NVIDIA reports `shaderGroupBaseAlignment 64`, `shaderGroupHandleAlignment 32` -- log both in Step 0 with the reorder/cluster properties). No shader-record data: every stage binds the shared descriptor set, so record stride = 32 B.

```
region   record  group (library)                 index used by
raygen   0       R  (LibRaygen)                  --
miss     0..3    M0 gradient/HDRI, M1 proc, M2 hosek, M3 physical (LibMiss)   MissShaderIndex = uint(sun_and_mode.w) (mode 0,1 -> 0)
         4..7    S0 planet, S1 ocean, S2 analytic, S3 sdf (LibMiss)           MakeMiss(4 + sw_kind)
hit      0       H2 mesh CH      (LibHit)         instanceShaderBindingTableRecordOffset = InstanceDesc.kind (0)
         1       H1 terrain CH   (LibHit)         ... = kInstKindTerrain (1), L741-742
callable 0..9    C0..C9 (LibCallable)             CallShader(k)
```
`TraceRay(..., sbtRecordOffset 0, sbtRecordStride 1, ...)`; hit-group index = instance offset. `TLASInstance` gains `std::uint32_t sbt_offset` (`Resources.h:111-116`) which `BuildVkInstances` (`VulkanDevice.h:766`) writes into `instanceShaderBindingTableRecordOffset`; `PlanetTerrain` sets 1, the CSG mesh 0. In 1a both hit groups point at the same stub CH so a wrong offset is harmless.

**Pipeline libraries** (`VK_KHR_pipeline_library`, feature `rayTracingPipeline`): four `VK_PIPELINE_CREATE_LIBRARY_BIT_KHR` pipelines -- LibRaygen {R}, LibHit {H1,H2}, LibMiss {M0-3,S0-3}, LibCallable {C0-9} -- each with the identical `VkRayTracingPipelineInterfaceCreateInfoKHR{ maxPipelineRayPayloadSize 48, maxPipelineRayHitAttributeSize 8 }`, then one final `vkCreateRayTracingPipelinesKHR` with `VkPipelineLibraryCreateInfoKHR{4 libraries}`, zero own stages, `maxPipelineRayRecursionDepth = 1` (nothing below raygen calls `TraceRay`; shadow rays are `RayQuery`). Group indices in the linked pipeline follow library link order, so a host-side table (`kRtGroupOrder[]`) is the single source of truth for SBT offsets and is unit-tested (section 9). A shader edit rebuilds one library and relinks (milliseconds); the driver's per-shader compile happens at library creation, which is also where the 3-4 minute cold sweep goes: build the four libraries **in parallel** (`vkCreateRayTracingPipelinesKHR` is externally synchronized only on the cache, which is internally synchronized) instead of the serial `build_pipeline` sweep (`VulkanDevice.cpp:1685-1856`); cold time becomes max(library) rather than the sum, and `-O2` modules of the sizes in section 3 should each compile in seconds. Stack size: `vkGetRayTracingShaderGroupStackSizeKHR` per group, `vkCmdSetRayTracingPipelineStackSizeKHR(raygen + max(miss, hit) + 2 * max(callable))` -- the factor 2 is the nested C0 -> C8 visibility call.

**Pipeline cache.** `vkCreateRayTracingPipelinesKHR` takes the same `pipeline_cache_` (`VulkanDevice.cpp:2058-2114`, `%LOCALAPPDATA%/demont/pipeline.cache`); libraries and the linked pipeline are cached individually, so a warm start is unchanged in shape. Expect the blob to grow by the sum of the module sizes; nothing else changes.

**RHI additions** (`src/rhi/Resources.h`, `Device.h`, `CommandBuffer.h`; Vulkan impl in a new `src/rhi_vulkan/VulkanRtPipeline.{h,cpp}`):

```cpp
// Resources.h
enum class RtStage : std::uint8_t { RayGen, Miss, ClosestHit, AnyHit, Intersection, Callable };
struct RtShaderDesc     { std::string_view name; RtStage stage; std::span<const std::uint8_t> spirv; std::string_view entry = "main"; };
struct RtHitGroupDesc   { std::string_view name; std::uint32_t closest_hit = kRtNone; std::uint32_t any_hit = kRtNone;
                          std::uint32_t intersection = kRtNone; bool procedural = false; };          // indices into shaders
struct RtLibraryDesc    { std::string_view name; std::span<const RtShaderDesc> shaders; std::span<const RtHitGroupDesc> hit_groups; };
struct RtInterface      { std::uint32_t payload_bytes = 48; std::uint32_t attribute_bytes = 8; std::uint32_t max_recursion = 1;
                          std::uint32_t max_callable_depth = 2; };
struct RayTracingPipelineDesc {
    std::string_view name; RtInterface iface;
    std::span<const RtLibraryHandle> libraries;           // link order == group order
    std::span<const std::uint32_t> miss_order, hit_group_order, callable_order;   // group ids -> SBT record order
    std::uint32_t raygen_group = 0;
};
struct TLASInstance { AccelStructHandle blas; float transform[12]; std::uint32_t instance_id = 0; std::uint32_t mask = 0xFF;
                      std::uint32_t sbt_offset = 0; };                                   // NEW field
enum class BarrierDesc::Stage : std::uint8_t { ComputeRead, ComputeWrite, Transfer, Present, RayTracingRead, RayTracingWrite };  // NEW members

// Device.h
virtual bool             SupportsRayTracingPipeline() const { return false; }
virtual RtLibraryHandle  CreateRtLibrary(const RtLibraryDesc&)              { return {}; }   // async, like compute; id==0 while building
virtual bool             ReplaceRtLibrary(RtLibraryHandle, const RtLibraryDesc&) { return false; }   // dev hot-reload: rebuild one library, relink
virtual PipelineHandle   CreateRayTracingPipeline(const RayTracingPipelineDesc&) { return {}; }
virtual bool             RayTracingPipelineReady(PipelineHandle) const      { return false; }
virtual void             DestroyRtLibrary(RtLibraryHandle) {}

// CommandBuffer.h
virtual void BindRayTracingPipeline(PipelineHandle) {}
virtual void TraceRays(std::uint32_t w, std::uint32_t h, std::uint32_t d) {}   // uses the bound RT pipeline's SBT
```
Vulkan sketch: `VulkanRtPipelineEntry { VkPipeline pipeline; std::vector<VkPipeline> libs; BufferEntry sbt; VkStridedDeviceAddressRegionKHR raygen, miss, hit, callable; std::uint32_t stack_size; }`. `VulkanCommandBuffer::TraceRays` duplicates `Dispatch` (L530-733) exactly -- descriptor-ring slot (L545), image/buffer/accel writes (L639-667), Frame-UBO slice memcpy (L669-709) -- then `vkCmdBindDescriptorSets(VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, ...)`, `vkCmdPushConstants(cb, layout, kRtPushStages, 0, to_push, push_buf_)`, `vkCmdSetRayTracingPipelineStackSizeKHR`, `vkCmdTraceRaysKHR(&raygen, &miss, &hit, &callable, w, h, d)`. Refactor the body of `Dispatch` into `BindSharedSetAndPush(bind_point, stage_flags)` so the two cannot drift. New function pointers: `vkCreateRayTracingPipelinesKHR`, `vkGetRayTracingShaderGroupHandlesKHR`, `vkGetRayTracingShaderGroupStackSizeKHR`, `vkCmdSetRayTracingPipelineStackSizeKHR`, `vkCmdTraceRaysKHR`. SBT buffer: `VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | SHADER_DEVICE_ADDRESS_BIT`, device-local, one upload after link, regions aligned to `shaderGroupBaseAlignment`. Engine dispatch site (`Engine.cpp:8553-12213`): `cb->BindRayTracingPipeline(...)` + `cb->TraceRays(fc.width, fc.height, 1)` under a new `r_pt_pipeline {compute, rt}` cvar; everything else at that site (binds, `PushConstants` L12208) is unchanged, which is the point of reusing the layout.

**Slang / CMake.** `pt_compile_slang` already takes `STAGE` (`cmake/Slang.cmake:166-168`, `-stage` L233) and Slang 2026.8 accepts `raygeneration | intersection | anyhit | closesthit | miss | callable` (`command-line-slangc-reference.md:1230-1235`); entry points carry `[shader("raygeneration")]` etc. and the multiple miss/callable entries in one file are selected with `-entry`. Add `-capability spirv_1_6+SPV_KHR_ray_tracing+SPV_NV_shader_invocation_reorder` on RT stages (`SPV_EXT_shader_invocation_reorder` needs SPIR-V ≥ 1.5, atoms L623-625; NV inherits EXT, atoms L701-703, so one flag covers `ReorderThread`/`MakeMiss`/`FromRayQuery` today and `GetClusterID` in Step 2). Shared declarations (all `[[vk::binding]]` globals, `Push`, `Frame`, `HitInfo`, `PtPayload`, medium stack, ray-cone helpers) move into a `PathTraceCommon.slang` module compiled with `pt_compile_slang_module` (L118-158); explicit `vk::binding` numbers survive module import, which keeps every module's SPIR-V on the same binding numbers -- verify on the probe by `spirv-dis | grep Binding`. `PT_SLANGC_OPT` becomes per-target: `-O2` for RT stages, `-O0` retained for the compute kernel until it is deleted. The `norq` variant (`src/rhi_vulkan/CMakeLists.txt:129-133`) and `PT_SPIRV_NO_RAYQUERY` are deleted with the compute kernel: the engine is NVIDIA-exclusive.

---

## 8. Descriptor / push-constant plan

**Reuse the shared layout with RT stage flags; no new layout.** A `VkPipelineLayout` is bind-point agnostic; the only per-stage facts in it are `VkDescriptorSetLayoutBinding::stageFlags` (L1353, today `COMPUTE_BIT`) and `VkPushConstantRange::stageFlags` (L1608). Both become `COMPUTE | RAYGEN | CLOSEST_HIT | MISS | CALLABLE` (add `ANY_HIT | INTERSECTION` when analytic prims move to AABBs). `vkCmdPushConstants` must then pass exactly those flags (fix L728, which hard-codes `COMPUTE_BIT`, to use the layout's range). Everything else already fits: `UPDATE_AFTER_BIND | PARTIALLY_BOUND` on every binding (L1565-1567) and `descriptorBindingAccelerationStructureUpdateAfterBind` (L1017, L1125) are stage-independent; the `MUTABLE_EXT` binding 2 with `{STORAGE_IMAGE, STORAGE_BUFFER, ACCELERATION_STRUCTURE}` (L1302-1306, L1365-1369) is read as an acceleration structure by RT stages exactly as `RayQuery` reads it now; `TraceRays` takes one slot from the 24-per-frame descriptor ring (`kDispatchSetsPerFrame`, `VulkanDevice.h:632`) and one 2 KB Frame-UBO slice (L1658-1668), same as a dispatch. Storage images (`RWTexture2D` with `vk::image_format`) are legal in every RT stage. The slot tables `kSlotToTexBinding`/`kSlotToBufBinding` (L185-276) are untouched; no binding number moves.

**Push constants.** The 112 B prefix (`kPushSplitOffset`, `VulkanDevice.h:356`; `Push` cbuffer `PathTrace.slang:1135-1151`) and the 2048 B `Frame` spill at binding 14 (L1871-2045) are read by every RT stage through the shared `PathTraceCommon` module. The full `PtPush` is 832 B by the shader comment (L1123), ~2064 B by the host (`Types.h` comment on `kMaxPushConstantBytes`); the three static_asserts (`Engine.cpp:11885, 11926, 11985`) keep guarding it. Nothing about the RT pipeline needs more push space: SBT indices are compile-time constants, hit kinds come from the SBT, sky mode from `sun_and_mode.w`.

**Barriers.** `VulkanCommandBuffer::Barrier` (L735+) translates the coarse `Stage` enum to `COMPUTE_SHADER` masks; add `RayTracingWrite/Read -> VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR` and emit `{RayTracingWrite -> ComputeRead}` after `TraceRays` at the sites that today emit `{ComputeWrite -> ComputeRead}` after the PathTrace dispatch (L12245, L13498). `UpdateTLASInstances` ordering (`Device.h:89-92`) already covers the build -> trace edge on the queue.

**Device creation.** Add to `dexts` (L1032-1061): `VK_KHR_ray_tracing_pipeline`, `VK_KHR_pipeline_library`, `VK_KHR_ray_tracing_maintenance1`, `VK_EXT_ray_tracing_invocation_reorder`, `VK_NV_ray_tracing_invocation_reorder`, `VK_KHR_ray_tracing_position_fetch` (optional, for `GetTriangleVertexPositions`); chain `VkPhysicalDeviceRayTracingPipelineFeaturesKHR{rayTracingPipeline, rayTracingPipelineTraceRaysIndirect}`, `...RayTracingInvocationReorderFeaturesEXT{rayTracingInvocationReorder}`, `...RayTracingInvocationReorderFeaturesNV`, `...RayTracingMaintenance1FeaturesKHR`; query `...RayTracingPipelinePropertiesKHR` and `...RayTracingInvocationReorderPropertiesEXT` (must read `REORDER_MODE_REORDER_EXT`, verified live in the plan) and log them beside the existing RT-presence line (L946). `ai.apiVersion` -> 1.4 (Step 0, L815). `rt_pipeline_supported_` gates `SupportsRayTracingPipeline()`; the engine falls back to the compute kernel while it exists (1a/1b) and refuses to start without it afterwards.

---

## 9. Migration sequence, acceptance tests, and the A/B harness

**Determinism baseline (before 1a, half a day).** The golden matrix is bit-identical across runs on this host for the software cells (`tests/CMakeLists.txt:1478-1481`), but the Vulkan cells have never been measured and the harness comment records BLAS-build entropy on Metal at low frame counts (L1143-1146). Run every fixture below twice on the *compute* path with `r_capture_seed 1` (resets `frame_index_` and the accumulator, `Engine.cpp:2405-2416`; the PRNG seeds only from `(pixel, frame_index)`, L7894) and `imgdiff --max-delta 0 --mean-delta 0 --fail-percent 0`. Fixtures that are not bit-stable against themselves get the compute-vs-compute delta recorded as their floor; the 1a criterion for them is "RT-vs-compute ≤ compute-vs-compute floor", and "= 0" for the rest. Expected: 0 everywhere except possibly the terrain fixtures during residency settling (`planet_settling_` holds `frame_index_`, L9722).

**The A/B harness** (`tests/CMakeLists.txt`, new `pt_add_ab_cell(SCENE ...)`): two `pt_render_one_frame` runs per scene (`tools/pt_render_one_frame`, args at L7-9: `--scene --backend vulkan --denoiser off --frames N --out --extra "r_capture_seed 1; r_pt_pipeline compute|rt"`), then `imgdiff a b --max-delta 0 --mean-delta 0 --fail-percent 0 --diff`. Frames: 8 for non-terrain fixtures, 64 (the golden default, L1157-1159) for terrain so residency settles; `--denoiser off` so `accum_hdr` is the capture source and no denoiser history enters. Scenes (all in `tests/goldens/scenes/`): `planet_surface`, `planet_aerial`, `planet_hillslope_rock`, `planet_horizon_clouds`, `planet_horizon_clouds_planar`, `planet_land_orbit`, `planet_land_orbit_ocean`, `planet_lod_seam`, `planet_ocean_boat`, `planet_ocean_orbit`, `planet_ocean_orbit_smooth`, `planet_ocean_snell`, `planet_underwater_column`, `orbital_limb`, `orbital_terminator`, `civil_twilight`, `civil_twilight_no_ozone`, `shell_crossing`, `clouds_godrays`, `bsc_night_clouds`, `sky_hosek`, `procedural_noon/evening/dawn`, plus the three existing Vulkan golden scenes `cornell_csg`, `sdf_smin_row`, `pbr_textured` (TLAS mesh, SDF, textured analytic prims -- the non-planet tiers). Second run of the same harness with `--denoiser svgf_atrous` and `--extra "r_restir 1"` on `planet_surface`, `light_primitives_mixed`, `planet_ocean_boat` to cover the G-buffer pass, `albedo_tex.a`, `cloud_trans_tex`, `shadow_vis_buf` and the reservoir write.

**1a -- raygen-only port (6-8 d incl. the 2-day probe).** Deliverables: RHI + Vulkan RT pipeline/SBT (section 7), `PathTraceCommon.slang` extraction, `PathTraceRaygen.slang` = `main()` with `HitObject.TraceRay` + accessor decode, stub CH/miss, `r_pt_pipeline`, the A/B cells, the probe report (sizes, register/spill stats, pipeline-creation times at `-O0` and `-O2`, TDR result per variant).
Acceptance: (1) every A/B cell passes at threshold 0 (or at its recorded floor) at `-O0`; (2) `AccelGpuStallCount` (`Device.h:114`) unchanged per frame on `planet_surface` (the RT pipeline touches no build path); (3) tier-4 GPU timer for the "PathTrace" pass (label L8553, `Device.h:521-571`) within ±10% of the compute kernel on `planet_surface`, `orbital_limb`, `cornell_csg`; (4) validation layers clean (the layout stage-flag change is the likely VUID source); (5) the probe verdict recorded in HANDOFF.md.
Source-contract pins that must stay green with no edits: `pt_planet_shader` (mirrors the shader's planet-altitude/shell math, `tests/CMakeLists.txt:552-571` -- it reads `PathTraceMath.slang`, an existing module the split does not touch; confirm the path it opens), `pt_atmosphere` (the physical-sky shell clamp the L10292-10308 comment cites as its only pin), `pt_denoise_demod_guide` (the `G = T·A + (1−T)` guide, written by the raygen G-buffer pass L8130-8135), `pt_cloud_march_step` (on the rejected branch; if it lands, it pins the step weight L10752, which C4 must keep verbatim). New unit tests: `pt_rt_sbt_layout` (group-order table -> record offsets/strides against synthetic `shaderGroupBaseAlignment/HandleAlignment` values, all four regions, 64 B alignment), `pt_rt_payload_layout` (sizeof/offsetof pins on the host mirror of `PtPayload`/IO structs vs a `slangc -reflection-json` dump checked into the test), and `rhi_rt_pipeline_test` (modelled on `rhi_accel_update_test.cpp`: build the pipeline, `TraceRays` 16x16 against the `AccelProbe` scene, compare hit distances with the existing `accel_probe` RayQuery kernel at 0 ulp).

**1b -- the split (8-10 d).** Deliverables: H1/H2, M0-M3, S0-S3, C0-C9, four libraries, parallel library build, hot-reload of one library (`ReplaceRtLibrary`), `-O2` on every RT stage, deletion of the compute kernel + `norq` + `PT_SLANGC_OPT=-O0` once the soak passes.
Acceptance: (1) `PT_SLANGC_OPT=-O2` builds, links inside 60 s cold, and renders **10 minutes at 3840x2160 on the orbit->surface flight** (new `tests/goldens/scenes/planet_flight_orbit_surface.cfg` using the cam-bookmark path the residency test paces; run as `demont --smoke-frames=36000`) with `IsDeviceLost()==false` and no event 153 in the Windows event log; (2) the A/B harness still passes at threshold 0 against the 1a `-O0` images -- the split relocates code, it does not change arithmetic, and a non-zero diff is a bug to fix, not a tolerance to raise (if the driver's `-O2` fma contraction moves a bit, the diff must be ≤ 1 8-bit level on ≤ 0.01% of pixels and the cause named in the report); (3) "PathTrace" pass time ≤ 0.6x the `-O0` compute kernel on `planet_surface` (target; report the number either way); (4) per-module `VK_KHR_pipeline_executable_properties` stats attached: no module spills > 64 B; (5) the three Vulkan golden cells unchanged; (6) the owed Vulkan golden cells (`planet_surface__vulkan__off`, `orbital_limb__vulkan__off`, `planet_ocean_boat__vulkan__svgf_atrous`) generated from the 1a images with owner sign-off, so 1b/1c have a committed pin. `-O0` stays a documented CMake fallback for one release, not the default.

**1c -- SER (3-5 d).** Deliverables: the two reorder points, `r_pt_ser`, `MakeNop` for the void paths.
Acceptance: (1) pixel-identical to 1b (same A/B, threshold 0: `ReorderThread` changes scheduling, not results -- and `MakeMiss`-with-index changes which entry runs, not what it computes); (2) ≥ 20% reduction in "PathTrace" pass time with `r_pt_ser 1` vs `0` on the horizon-heavy fixtures `planet_horizon_clouds`, `orbital_limb`, `sunset_horizon`, `planet_aerial` at 1920x1080 and 3840x2160 (report all eight numbers); (3) no regression > 2% on `planet_surface` noon (the coherent case, where reorder overhead is pure cost).

---

## 10. Risks, the early probe, and the wavefront fallback

**R1 -- the raygen alone re-trips the driver miscompile at `-O2` (the central unknown).** Probe as in section 1, run on day 1-2 of 1a, three variants, each at `-O0` and `-O2`, each timed for pipeline creation (abort criterion 120 s) and soaked 60 frames at 16x16 then 1280x720. Decision rule: P2 passes -> 1a ships at `-O2`, 1b is a performance step; P1 passes, P2 fails -> the marches and the water branch become callables inside 1a (pull C3-C6 forward; the A/B still holds because callables do not change arithmetic); P0 fails -> file the driver bug with the P0 SPIR-V (the smallest repro NVIDIA can act on; Step 0's bug filing gets a better artifact), keep 1a at `-O0` only if P0 passes at `-O0`, and start plan B. Time-box: 2 days including the report.

**R2 -- payload/register pressure in the raygen.** ~52 live dwords of path state plus a 32-dword `HitInfo`; if the executable statistics show > 128 registers or spills, the mitigations in order: (i) shrink `HitInfo` to the 12-dword payload and read the rare fields on demand (emission, Wave-9 params), (ii) move `applyPbrTextures` into H2, (iii) move the G-buffer pass into a separate raygen record dispatched first (two `TraceRays` per frame). None change pixels.

**R3 -- `RayQuery` inside RT stages.** `VK_KHR_ray_query` allows ray queries in every shader stage (the feature is stage-independent; SPIR-V `RayQueryKHR` is valid in `RayGenerationKHR` and `CallableKHR` execution models), and Slang emits it from a raygen/callable when the `rayquery` atom is satisfied. Cost is what it is today: the inline traversal holds the calling shader's registers, and is not reordered. That is the plan's 1a choice and it is the right one for visibility rays; the alternative (a second `TraceRay` with a shadow-miss, recursion depth 2) would put the `kMaxRefract` dielectric chain (up to 16 legs, `w2j_row1.w`, L1170-1173, L7165) into recursive `TraceRay` calls with the payload copied per leg. Not worth it; revisit only if the executable stats show the query's live-range is what spills the raygen. `TraceRay` from a callable is allowed in SPIR-V but not in DXR; the design does not rely on it.

**R4 -- Slang 2026.8 gaps.** `MakeHit` NV-only (doc L156) -> avoided via `MakeMiss` records. `FromRayQuery` EXT-only (doc L281) -> not needed. `SV_IntersectionAttributes` unsupported on SPIR-V (a2-01 L78) -> no custom intersection shaders in Step 1; when analytic prims move to AABBs, pass attributes through `ReportHit<attr_t>`. `HitObject` variable rules under NV (doc L14) -> one HitObject per iteration, assigned on all paths; the probe compiles with the NV capability precisely to surface this early. Module-scope `vk::binding` globals across imported modules -> verified by the `spirv-dis` binding check in the probe. `-O2` on `-emit-ir` modules (`Slang.cmake:150`) -> keep module and entry optimisation levels equal, as the linker requires matching preprocessor state today (L128-134).

**R5 -- TLAS-hit bit identity between `RayQuery` and `TraceRay`.** Same RT-core traversal, same `TMax`, same committed closest triangle; if the determinism baseline shows a non-zero compute-vs-compute floor on TLAS fixtures, that floor is BLAS-build entropy and the criterion is relaxed to it, for those fixtures only.

**R6 -- pipeline creation time / cache growth.** Parallel library builds and `-O2` modules should cut the cold sweep from 3-4 min to tens of seconds; if the linked pipeline still takes minutes, the cause is the raygen and R1's mitigations apply.

**Plan B -- wavefront compute (Laine, Karras, Aila 2013).** Trigger: P0 fails at `-O2` and at `-O0`; or after 1b the raygen still spills and the trace pass is > 0.8x the `-O0` kernel; or the linked pipeline cannot be created inside 5 min cold. Layout (each kernel small, compiles at `-O2`, reuses the shared layout; needs `vkCmdDispatchIndirect` in the RHI, `BufferUsage::Indirect` already exists in `Types.h`):

```
K0 WfGenerate   per pixel: camera ray + G-buffer pass          -> PathState[N] (96 B: ro, rd, throughput, radiance, seed, cone, medium, flags, depth, spp)
loop b in 0..max_bounces (host-recorded, indirect counts):
  K1 WfTrace      RayQuery + sw tiers -> HitRec[N] (48 B = PtPayload) ; appends path ids to queue[kind*4 + matclass]
  K2 WfShade_*    one kernel per (kind, mat): Lambert / metal / dielectric / water / sky-miss / sw-hit  (= C0..C3, M*, S*)
                  emits ExtRay[N] and ShadowRay[M] (48 B: path id, ro, rd, tmax, radiance-if-visible)
  K3 WfShadow     transmittance() per ShadowRay (RayQuery + refraction chain) -> atomic add into PathState.radiance
  compaction: ExtRay queue -> next K1 input; terminated paths -> K5
K4 WfVolumetric  per primary sample: atmosphere / cloud / underwater marches (C4-C6 bodies) using primary_t
K5 WfAccumulate  per pixel: spectral fold, sanitize, accum_hdr, denoise_color, outputs (L11230-11452)
```
Memory: 3840x2160 = 8.3 M paths x (96 + 48 + 48) B ≈ 1.6 GB at 1 spp -- run in four screen tiles of 2.1 M paths (≈ 400 MB) or accept it on a 32 GB card. Dispatch count ≈ max_bounces x 8 + 3 ≈ 67 per frame: `kDispatchSetsPerFrame` (24, `VulkanDevice.h:632`) and the Frame-UBO ring grow to 96. Determinism: the atomic radiance adds in K3 break bitwise identity; make K3 write per-shadow-ray results and let K5 sum them in queue order, which restores it. The arXiv 2605.27323 measurement (+16% on Vulkan HW-RT from wavefront alone) is the expected win, without SER. Do not run both plans: the wavefront's shade kernels are the same code bodies as the callables (C0-C6), so whichever plan wins, the module extraction of 1b is not wasted.

---

## 11. Where post-process lands

Step 1 wires **nothing new** downstream: after `TraceRays` the frame is exactly today's graph (section 2), with two mechanical changes -- the `RayTracingWrite -> ComputeRead` barrier at the two sites that today emit a compute-compute barrier after PathTrace (L12245, L13498), and the "PathTrace" `GpuPassMark` label kept so the perf overlay and the acceptance timers line up. The denoiser-off swapchain write stays inside the raygen (L11442-11452) so `--denoiser off` captures are unchanged, and `denoise_color`/G-buffers feed `Denoise()` exactly as before.

The dead chain is a separate task the HANDOFF already names (HANDOFF.md "Systemic finding"). The target frame graph for that follow-up, all compute passes, inserted into the existing dual-Denoise path (`Engine.cpp:13459-13479`):

```
TraceRays -> [CloudsRaymarch?] -> ReSTIR x3 -> AutoExpose
  -> Denoise(SvgfNoFinalize) -> post_denoise_hdr
  -> SigmaShadow (shadow_vis_buf demod re-add)          <- from the dead block L13576
  -> HeightFog -> CloudsComposite -> Aurora -> ParticleComposite -> GodRays   <- L13678 .. L14238, each reading post_denoise_hdr + depth_tex
  -> StarsComposite (already here, L13470)
  -> Bloom pyramid (already here)
  -> Denoise(FinalizeOnly): bloom + tonemap_op + sRGB OETF -> swapchain      <- retire Tonemap.slang; DenoiseFinalize already implements the operators (Device.h:347-354)
  -> EditorOverlay -> PerfOverlay
```
Only two things in that follow-up touch Step 1's output contract: the composites need `depth_tex`/`cloud_trans_tex`, which the raygen keeps writing in Step 1 (L8029, L11397), and the lens flare/`CloudsComposite` reworks blocked in HANDOFF wait on this chain, not on the RT pipeline.

---

## 12. File-by-file change list and estimates

New files:

| File | ~Lines | Notes |
|---|---|---|
| `shaders/PathTraceCommon.slang` (module) | ~2,300 moved | bindings L39-736, `Push`/`Frame` L1135-2045, materials/medium L2148-2270, `HitInfo` L2540-2601, `PtPayload` + IO structs, `InstanceDesc`/terrain loaders L644-785, cone helpers |
| `shaders/PathTraceSky.slang` (module) | ~2,000 moved | L3933-4810 sky/celestial helpers, L6300-7117 atmosphere + `skyPhysical` + `skyColor*`, `callSkyPhysical` entry |
| `shaders/PathTraceRaygen.slang` | ~3,800 | `main()` L7889-11453 as raygen; software tiers L3282-3634 (minus SDF) |
| `shaders/PathTraceHitTerrain.slang` | ~150 (+imports) | L3645-3699 |
| `shaders/PathTraceHitMesh.slang` | ~250 | L3702-3909 |
| `shaders/PathTraceMissSky.slang` | ~200 | 4 entries over the sky module; peel/no-sun-disc flags |
| `shaders/PathTraceSwHit.slang` | ~500 | S0-S3; ocean shell march L7680-7883 + `testAnalyticPrim` L3002-3098 |
| `shaders/PathTraceCallBsdf.slang` | ~1,150 | C0-C3 from L9049-9990 + Wave-9 helpers L2733-2833 |
| `shaders/PathTraceCallMarch.slang` | ~1,100 | C4-C6 from L10092-11206 |
| `shaders/PathTraceCallVisibility.slang` | ~850 | C8: `transmittance` L7132-7274 + tier copy + `cloud_optical_depth` L6223 |
| `shaders/PathTraceCallSdf.slang` | ~180 | C9 over the existing `SdfPrimitives`/`SdfFractals` modules |
| `src/rhi_vulkan/VulkanRtPipeline.{h,cpp}` | ~700 | libraries, link, SBT, stack size, group-order table, parallel build worker |
| `tests/pt_rt_sbt_test.cpp`, `tests/pt_rt_payload_test.cpp`, `tests/rhi_rt_pipeline_test.cpp` | ~150 / ~120 / ~250 | section 9 |
| `tests/goldens/scenes/planet_flight_orbit_surface.cfg` | ~40 | the 10-minute soak path |
| `tests/goldens/Windows/*__vulkan__*.png` | 3 cells | owed goldens, owner sign-off |

Modified files:

| File | ~Lines changed | What |
|---|---|---|
| `cmake/Slang.cmake` | ~50 | per-target opt level, `-capability` for RT stages, `ENTRY` per record, drop the `-O0` mandate comment after 1b |
| `src/rhi_vulkan/CMakeLists.txt` | ~110 | 21 `pt_compile_slang` records with `STAGE`/`ENTRY`, new modules, delete `norq` (L129-133) |
| `src/rhi/Resources.h` | +100 | section 7 structs, `TLASInstance::sbt_offset`, barrier stages |
| `src/rhi/Device.h` | +45 | RT pipeline/library verbs |
| `src/rhi/CommandBuffer.h` | +15 | `BindRayTracingPipeline`, `TraceRays` |
| `src/rhi_vulkan/VulkanDevice.h` | +130 | pfns, `rt_pipeline_supported_`, RT entries map, `BindSharedSetAndPush` |
| `src/rhi_vulkan/VulkanDevice.cpp` | ~650 | extensions/features/properties (L936-1163), stage flags (L1353, L1608, L728), `Dispatch` refactor (L530-733), barrier stages (L735+), `BuildVkInstances` sbt offset, parallel library build in the worker (L1685-1856), `TraceRays`, `apiVersion` 1.4 (L815) |
| `src/engine/Engine.cpp` | ~180 | `r_pt_pipeline`, `r_pt_ser` cvars; dispatch site L8553-12213 (bind + `TraceRays`); barrier sites L12245/L13498; pipeline-handle resolve (L6483-6547) for the RT pipeline; `PlanetTerrain`/mesh instance `sbt_offset` |
| `src/engine/Engine.h` | +12 | ids/cvars |
| `src/engine/PlanetTerrain.cpp` | +5 | `sbt_offset = kInstKindTerrain` on chunk instances |
| `tests/CMakeLists.txt` | ~140 | `pt_add_ab_cell`, the 30 A/B cells, 3 new unit/device tests, the soak target |
| `HANDOFF.md`, `docs/NEXTGEN_PLAN.md` | ~60 | probe verdict, measured numbers, the `-O0` fallback note |

Deleted (end of 1b, after the soak): `shaders/PathTrace.slang` (11,453 lines, superseded by the modules above), the `PathTrace_norq` variant and `PT_SPIRV_NO_RAYQUERY` (`PathTrace.slang:41-50`, CMake L129-133), the compute `build_pipeline("pathtrace")` (L1755-1759), `r_pt_pipeline compute`.

Engineer-day estimates: determinism baseline 0.5 d; probe 2 d; 1a 6-8 d (RHI + Vulkan RT 3, common-module extraction + raygen port + CMake 2, harness/cvars/A-B/report 1-3); 1b 8-10 d (sky/common modules 2, CH x2 1, miss x4 + sw-hit x4 1.5, BSDF callables + visibility 2.5, marches + skyPhysical 1.5, libraries/parallel build/hot-reload 1.5, `-O2` soak + timing + goldens 1); 1c 3-5 d. Total 20-26 d, inside the plan's XL (15-25 d) band with the probe's early exit as the risk control.
