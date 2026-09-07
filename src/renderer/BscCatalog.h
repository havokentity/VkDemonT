// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

// Yale Bright Star Catalog (BSC5) loader + J2000 equirectangular
// starmap rasteriser. The on-disk catalog (assets/stars/BSC5.dat) is
// the original Harvard CDC binary -- 28-byte header, 9110 fixed-width
// 32-byte records (HR number, RA/Dec in J2000 radians as float64,
// spectral type, V mag * 100, proper motions). We don't model proper
// motion -- a few arcsec/yr is invisible at any zoom we actually
// render, and the visual layout of constellations hasn't shifted
// noticeably since J2000 anyway.

#include <cstdint>
#include <string>
#include <vector>

namespace pt::stars {

struct Star {
    float ra_deg;    // J2000 right ascension, [0, 360)
    float dec_deg;   // J2000 declination, [-90, 90]
    float vmag;      // visual magnitude (lower = brighter)
};

// Load and parse BSC5 binary at `path`. On success returns a vector
// sorted by ascending vmag (brightest first), filtered to entries with
// finite RA/Dec and a sensible vmag. On failure returns an empty
// vector and writes a reason to `err` if non-null.
std::vector<Star> LoadBsc5(const std::string& path, std::string* err = nullptr);

// Rasterise the catalog into an RGBA16F equirectangular texture in the
// J2000 frame (RA across X in [0, 2pi], Dec across Y from +90 deg at
// y=0 down to -90 deg at y=H-1). Stars are splatted as Gaussian dots
// whose intensity follows the standard astronomical magnitude scale
// (each step of 1 mag ~= factor 2.512 in flux) and whose tint is
// derived from B-V color heuristics (we don't have B-V here, so we
// pick from a small palette by RA/Dec hash for visual variety).
//
// Output: row-major float-RGBA, len = W*H*4, as spectral radiance in
// W/m^2/sr at the map's own texel resolution -- the same radiometric
// units the rest of the sky uses (see kPtSolarIrradiance /
// kPtLegacySkyScale in PathTraceMath.slang). Energy-conserving: the sum
// of every texel a star touches, each weighted by that texel's solid
// angle, equals MagnitudeToIrradianceWm2(vmag) exactly -- so a consumer
// that footprint-averages the map recovers the star's true irradiance no
// matter what resolution it samples at.
//
// Caller passes this to the RHI as RGBA16F. Range check: with the optical
// PSF, Sirius (V = -1.46) peaks near 1.2e-2 W/m^2/sr, and the faintest
// catalogue entry (V = 9) near 7.7e-7. That faintest value is BELOW the
// half-float smallest normal (6.1e-5) and lands in the subnormal range
// (smallest subnormal 5.96e-8), so it carries roughly 8% relative
// quantisation rather than the usual 0.1%. That is acceptable and is
// recorded rather than hidden: a V = 9 star is ~1e-7 of a daylight sky
// and far below any visible threshold, so the error is invisible by
// construction. Anything that later makes such stars matter (a
// long-exposure astrophotography mode, say) needs a scaled or
// higher-precision map, not a quiet retune here.
void RasteriseJ2000Map(const std::vector<Star>& stars,
                       std::uint32_t W, std::uint32_t H,
                       std::vector<float>& out_rgba);

// --- shared point-source photometric scale (issue #281) ------------------
//
// These three functions ARE the starmap's photometry -- RasteriseJ2000Map
// calls them for every catalogue star. They are exposed so the planetarium
// (which cannot go through the baked equirectangular map, because planets
// move) can splat a planet with the identical magnitude -> irradiance and
// magnitude -> footprint relationship a star of the same apparent magnitude
// would get. Sharing one definition is the point: a planet at V = -2.7 must
// read exactly as bright as a catalogue star at V = -2.7, or the sky is
// lying about which object is brighter.

// V-band irradiance, in W/m^2, delivered at the observer by a point
// source of apparent visual magnitude `vmag`.
//
// Two cited pieces, no free parameters:
//   1. Pogson (1856), MNRAS 17, 12 -- one magnitude step is a factor
//      10^0.4 = 2.51188643 in flux, by definition.
//   2. Zero point -- a V = 0 source (Vega) delivers 3.19e-9 W/m^2 over
//      the V band. Bessell, Castelli & Plez (1998), A&A 333, 231 give
//      f_lambda = 3.63e-11 W/m^2/nm at 545 nm; across the V band's 88 nm
//      FWHM that is 3.19e-9 W/m^2. The independent route through the
//      standard 3640 Jy zero point (3.64e-23 W/m^2/Hz over an 8.83e13 Hz
//      effective width) gives 3.21e-9, agreeing to within 1%.
//
// RADIOMETRIC, not photometric, because that is what this engine speaks:
// kPtSolarIrradiance is 1360.8 W/m^2 (Kopp & Lean 2011) and the sky is
// carried in W/m^2/sr. Quoting stars in lux instead would be wrong by the
// luminous-efficacy factor (~700 for a Vega-like spectrum through the
// photopic curve), which is a far larger error than anything it would fix.
//
// Stars previously rode an arbitrary scale on which Vega was 4.0, so no
// exposure value could show the sky and the stars correctly at once. On
// the shared scale the behaviour falls out of the physics instead of
// being arranged: Vega contributes ~3.5e-4 W/m^2/sr of pixel radiance at
// a 60 deg / 384 px framing, which is ~2e-5 of a ~15 W/m^2/sr noon zenith
// (invisible, correctly) and ~1200x a true ~2.9e-7 W/m^2/sr moonless
// night sky (unmissable). No day-fade term is needed to hide stars in
// daylight, and none is used any more.
float MagnitudeToIrradianceWm2(float vmag);

// Angular sigma, in radians, of the point-spread function a point source
// is splatted with. Deliberately does NOT depend on magnitude: a star is
// a point, so its extent on the sky is set by the OPTICS, not by how
// bright it is. 1.5 arcmin (4.36e-4 rad) is the human eye's optical PSF
// for a point source.
//
// Bright stars still read as visibly larger on screen, which is correct
// and now automatic rather than authored: with a NORMALISED PSF, the
// wings of a bright star clear the display threshold further out. The
// previous magnitude-tiered sigma changed each star's TOTAL energy
// (energy goes as flux * sigma^2), so it silently contradicted the very
// magnitude it had just been handed.
//
// `sampling_floor_rad` raises sigma when the consumer's sampling grid is
// coarser than the optical PSF -- the map bake passes its texel pitch,
// the planet path passes the on-screen pixel angle. Widening the PSF
// this way is energy-preserving (the peak falls as the footprint grows),
// so it anti-aliases without brightening or dimming the source.
float PsfSigmaRad(float sampling_floor_rad);

// Johnson-Cousins B-V colour index -> linear (scene-referred, Rec.709
// primaries) RGB tint, normalised to unit Rec.709 luminance so the tint
// changes hue without changing how bright MagnitudeToIrradianceWm2 made it.
//
// Chain, both halves cited in the implementation:
//   1. B-V -> blackbody colour temperature, Ballesteros (2012).
//   2. T -> CIE 1931 (x, y) on the Planckian locus, Kim et al. (2002),
//      then xyY -> XYZ -> linear sRGB / Rec.709.
void BvToLinearSrgbTint(float bv, float out_rgb[3]);

}  // namespace pt::stars
