# Determinism baseline for the Step 1 A/B harness (compute path, native Vulkan)

Measured 2026-09-06 on the RTX 5090 (driver 616.56), tree `integrate/polish-vulkan`
(full default config, `slangc -O0`). Purpose: `docs/STEP1_RT_PIPELINE_DESIGN.md` §9 —
before the RT-pipeline port can be accepted as "bitwise identical to the compute
path", establish whether the compute path is bitwise identical to ITSELF run-to-run,
per fixture, so the A/B criterion is 0 where it can be and a measured floor elsewhere.

## Method

Every fixture rendered **twice** with identical settings on `--r-backend=vulkan`,
`r_capture_seed 1` (resets `frame_index_` + the accumulator so the pixel/frame PRNG
seeding restarts identically), the fixture's own cfg, and the denoiser off (capture
source `accum_hdr`) or `svgf_atrous` + `r_restir 1` (capture source `denoise_color`).
8 frames for non-terrain fixtures, 64 for terrain so residency settles. Pairs compared
with the repo's `imgdiff` at `--max-delta 0 --mean-delta 0 --fail-percent 0`. Terrain
runs also logged the residency settle line (chunks resident / BLAS builds) so that
entropy source is visible.

## Result: 30 / 30 cells bit-identical

| fixture | frames | denoiser | size | identical | max Δ | mean Δ | bad px % |
|---|---|---|---|---|---|---|---|
| `orbital_limb` | 8 | off | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `orbital_terminator` | 8 | off | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `civil_twilight` | 8 | off | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `civil_twilight_no_ozone` | 8 | off | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `shell_crossing` | 8 | off | 320x240 | yes | 0.0000 | 0.000000 | 0.000000 |
| `clouds_godrays` | 8 | off | 768x432 | yes | 0.0000 | 0.000000 | 0.000000 |
| `bsc_night_clouds` | 8 | off | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `sky_hosek` | 8 | off | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `procedural_noon` | 8 | off | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `procedural_evening` | 8 | off | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `procedural_dawn` | 8 | off | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `planet_ocean_orbit` | 8 | off | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `planet_ocean_orbit_smooth` | 8 | off | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `planet_ocean_snell` | 8 | off | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `planet_underwater_column` | 8 | off | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `cornell_csg` | 8 | off | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `sdf_smin_row` | 8 | off | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `pbr_textured` | 8 | off | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `planet_surface` | 64 | off | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `planet_aerial` | 64 | off | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `planet_hillslope_rock` | 64 | off | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `planet_horizon_clouds` | 64 | off | 640x360 | yes | 0.0000 | 0.000000 | 0.000000 |
| `planet_horizon_clouds_planar` | 64 | off | 640x360 | yes | 0.0000 | 0.000000 | 0.000000 |
| `planet_land_orbit` | 64 | off | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `planet_land_orbit_ocean` | 64 | off | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `planet_lod_seam` | 64 | off | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `planet_ocean_boat` | 64 | off | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `planet_surface` | 64 | svgf_atrous + restir | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `light_primitives_mixed` | 8 | svgf_atrous + restir | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |
| `planet_ocean_boat` | 64 | svgf_atrous + restir | 512x384 | yes | 0.0000 | 0.000000 | 0.000000 |

## Fixtures that are NOT self-stable

None. Every pair matched byte-for-byte (identical PNG md5 for run a and run b), including:

- the nine terrain fixtures at 64 frames, where residency settled to the same chunk set
  and BLAS-build count on both runs (e.g. `planet_hillslope_rock`: 894 chunks resident,
  967 BLAS builds, both runs; `planet_lod_seam`: 585 / 626) — the BLAS-build entropy the
  golden-harness comment records on Metal at low frame counts does **not** occur here;
- the three denoiser-on cells (SVGF temporal + a-trous with ReSTIR reservoirs), so the
  G-buffer pass, `albedo_tex.a`, `cloud_trans_tex`, `shadow_vis_buf` and the reservoir
  write are deterministic too;
- `clouds_godrays`, whose cfg sets its own seed (42) — overridden to 1 via `--extra`, which
  the harness must keep doing.

## Fixtures that failed to render

None (0 `DEVICE_LOST`, 0 errors, PNG written on all 60 runs).

## Conclusion

The native-Vulkan compute path is fully deterministic on this box under `r_capture_seed 1`.
The Step 1 acceptance criterion is therefore **`imgdiff --max-delta 0 --mean-delta 0
--fail-percent 0` on every fixture, with no per-fixture floor**: any non-zero RT-vs-compute
delta in 1a is a real difference to explain, not noise. (Raw logs: the session scratchpad
`determinism/` — `diffs.log`, `driver_stdout.log`, `results.tsv`, the a/b PNGs.)
