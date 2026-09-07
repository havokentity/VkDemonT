// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
//
// Host mirror of the DLSS Ray Reconstruction guide-buffer contract.
//
// WHY THIS FILE EXISTS
//
// docs/DLSS_INTEGRATION_PLAN.md section 2.3 lists what blocks RR. Four of
// those items are inputs the engine produces and were producing wrongly:
//
//   1. the specular guide trio was gated on MetalFX denoiser kinds no live
//      backend could select, so it was allocated, written and read by
//      nobody (fixed by repointing the gate at DlssRayReconstructionRequested);
//   2. specular_albedo held raw F0, but RR wants the SPLIT-SUM INTEGRATED
//      specular reflectance F0*A + B;
//   3. specular_hit_distance held `primary_t * (1 - roughness)`, which is
//      not a distance;
//   4. exposure existed only as a storage-buffer scalar, and NGX takes a
//      1x1 R32F texture.
//
// None of it can be checked by a golden. The default config is r_dlss off,
// so none of these buffers is even allocated on the golden path -- that is
// the point (the change must be inert for existing users) and it is also
// why the only thing a golden could prove here is that nothing changed.
// What CAN be pinned without a GPU is the physics of the quantity and the
// source-text contract, which is what this file does.
//
// COUNT OCCURRENCES, DO NOT TEST FOR PRESENCE (see pt_planet_shader_test):
// a needle that appears once must still appear exactly once after someone
// copies the block, and a needle that must be GONE has to be asserted at
// zero rather than merely "not obviously there".

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

namespace {

// Read a source file with line endings normalised to LF, so the multi-line
// needles below match on a Windows checkout with core.autocrlf as well as
// on Linux CI.
std::string Slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    REQUIRE_MESSAGE(f.good(), "cannot open ", path);
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c != '\r') out.push_back(c);
    }
    return out;
}

std::size_t Count(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    std::size_t n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size())) {
        ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Host mirror of PathTrace.slang's envBrdfSplitSumAB / envBrdfSplitSum.
//
// Kept character-identical in its coefficients to the shader, and the
// SHADER_MIRROR test case below asserts each coefficient line still appears
// there exactly once -- a mirror that silently drifts from the thing it
// mirrors is worse than no mirror.
// ---------------------------------------------------------------------------

struct AB { float a, b; };

AB EnvBrdfSplitSumAB(float roughness, float n_dot_v) {
    const float c0[4] = {-1.0f, -0.0275f, -0.572f,  0.022f};
    const float c1[4] = { 1.0f,  0.0425f,  1.040f, -0.040f};
    const float r_in  = std::clamp(roughness, 0.0f, 1.0f);
    const float v_in  = std::clamp(n_dot_v,   0.0f, 1.0f);
    float r[4];
    for (int i = 0; i < 4; ++i) r[i] = r_in * c0[i] + c1[i];
    const float a004 =
        std::min(r[0] * r[0], std::exp2(-9.28f * v_in)) * r[0] + r[1];
    return AB{-1.04f * a004 + r[2], 1.04f * a004 + r[3]};
}

float EnvBrdfSplitSum(float f0, float roughness, float n_dot_v) {
    const AB ab = EnvBrdfSplitSumAB(roughness, n_dot_v);
    return std::clamp(f0 * ab.a + ab.b, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// Numerical reference: Karis's IntegrateBRDF, the Monte-Carlo evaluation of
// the same split-sum integral the analytic form approximates.
//
//   A = E[(1 - Fc) * G_Vis],  B = E[Fc * G_Vis]
//
// over GGX-importance-sampled half-vectors, with Fc = (1 - v.h)^5 the
// Schlick factor separated out and G_Vis = G * (v.h) / ((n.h)(n.v)). G is
// the separable Smith masking-shadowing with the IBL k = alpha/2 that Karis
// specifies for this integral. Hammersley (i/N, radicalInverse(i)) for the
// sample sequence -- deterministic, so this test cannot flake.
//
// This reference is itself approximate at the extremes (the estimator is
// ill-conditioned as n.v -> 0, where G_Vis divides by n.v, and the
// separable Smith underestimates at roughness -> 1). The tolerances below
// are therefore stated as MEASURED agreement over the domain that matters
// rather than as a claim that either side is exact.
// ---------------------------------------------------------------------------

float RadicalInverseVdC(unsigned bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

AB ReferenceSplitSumAB(float roughness, float n_dot_v, int samples) {
    const float vx = std::sqrt(std::max(0.0f, 1.0f - n_dot_v * n_dot_v));
    const float vz = n_dot_v;
    const float alpha = roughness * roughness;   // engine + fit convention
    const float k = alpha * 0.5f;                // Karis IBL Smith k
    float a = 0.0f, b = 0.0f;
    for (int i = 0; i < samples; ++i) {
        const float u1 = (static_cast<float>(i) + 0.5f) / static_cast<float>(samples);
        const float u2 = RadicalInverseVdC(static_cast<unsigned>(i));
        const float phi = 6.28318530717958648f * u1;
        const float cos_t =
            std::sqrt((1.0f - u2) / (1.0f + (alpha * alpha - 1.0f) * u2));
        const float sin_t = std::sqrt(std::max(0.0f, 1.0f - cos_t * cos_t));
        const float hx = sin_t * std::cos(phi);
        const float hz = cos_t;
        const float voh = vx * hx + vz * hz;
        const float lz  = 2.0f * voh * hz - vz;
        const float nol = std::max(0.0f, lz);
        const float noh = std::max(0.0f, hz);
        const float vh  = std::max(0.0f, voh);
        if (nol > 0.0f && noh > 0.0f) {
            const float g1v = n_dot_v / (n_dot_v * (1.0f - k) + k);
            const float g1l = nol / (nol * (1.0f - k) + k);
            const float gvis = g1v * g1l * vh / (noh * n_dot_v);
            const float fc = std::pow(1.0f - vh, 5.0f);
            a += (1.0f - fc) * gvis;
            b += fc * gvis;
        }
    }
    return AB{a / static_cast<float>(samples), b / static_cast<float>(samples)};
}

float ReferenceSplitSum(float f0, float roughness, float n_dot_v, int samples) {
    const AB ab = ReferenceSplitSumAB(roughness, n_dot_v, samples);
    return std::clamp(f0 * ab.a + ab.b, 0.0f, 1.0f);
}

// Fresnel reflectance at normal incidence for an air -> medium interface.
// The engine spells this ((1 - n)/(1 + n))^2 at every dielectric shading
// site, and the specular guide now spells it the same way.
constexpr float F0FromIor(float n) {
    return ((1.0f - n) / (1.0f + n)) * ((1.0f - n) / (1.0f + n));
}

// Water at the engine's default r_water_ior. The MAT_WATER shading branch
// names 0.02 as "accurate F0 for water at 0 deg"; this is where that comes
// from, and it is what the guide must reproduce at normal incidence.
constexpr float kWaterIor = 1.333f;
constexpr float kGlassIor = 1.5f;

}  // namespace

// ===========================================================================
// 1. The quantity itself: split-sum integrated reflectance, not F0.
// ===========================================================================

TEST_CASE("split-sum reduces to F0 at normal incidence") {
    // At n.v = 1 the Schlick factor (1 - v.h)^5 vanishes for the specular
    // peak, so the bias term B goes to ~0 and A goes to ~1 for a smooth
    // surface: R_s -> F0. This is the ONE configuration where raw F0 was a
    // correct answer, and it is why handing F0 to the consumer looked
    // plausible in a head-on screenshot.
    for (float f0 : {F0FromIor(kWaterIor), F0FromIor(kGlassIor), 0.9f}) {
        const float rs = EnvBrdfSplitSum(f0, 0.0f, 1.0f);
        CHECK(std::fabs(rs - f0) < 0.01f);
    }
    // Water's normal-incidence value IS the 0.02 the water BRDF names.
    CHECK(std::fabs(F0FromIor(kWaterIor) - 0.0204f) < 0.0005f);
    // Glass at IOR 1.5 IS the familiar 0.04 the old guide hardcoded.
    CHECK(std::fabs(F0FromIor(kGlassIor) - 0.04f) < 0.0005f);
}

TEST_CASE("split-sum rises toward total reflection at grazing incidence") {
    // THE DEFECT THIS REPLACES. Raw F0 says a water surface reflects 2% of
    // the environment at every angle. It reflects nearly all of it at the
    // horizon -- which on a planet is most of the ocean, and is exactly the
    // band a denoiser handed F0 would demodulate by 0.02 and crush.
    const float f0 = F0FromIor(kWaterIor);
    const float smooth = 0.0f;

    // Monotone rise as the view lies down toward the surface.
    float prev = EnvBrdfSplitSum(f0, smooth, 1.0f);
    for (float nov : {0.8f, 0.6f, 0.4f, 0.2f, 0.1f, 0.05f, 0.02f, 0.0f}) {
        const float rs = EnvBrdfSplitSum(f0, smooth, nov);
        CHECK(rs > prev);
        prev = rs;
    }
    // At 0.05 (about 87 degrees off the normal -- an ocean horizon from any
    // camera above it) the reflectance is >= 35x F0, and by n.v = 0 it is
    // total. Measured: 0.763 at n.v = 0.05, 1.000 at n.v = 0.
    CHECK(EnvBrdfSplitSum(f0, smooth, 0.05f) >= 35.0f * f0);
    CHECK(std::fabs(EnvBrdfSplitSum(f0, smooth, 0.0f) - 1.0f) < 1e-6f);
    // Which is the factor the plan quotes: wrong by up to ~1/F0.
    CHECK(EnvBrdfSplitSum(f0, smooth, 0.0f) / f0 > 40.0f);
}

TEST_CASE("split-sum is energy-conserving everywhere") {
    // A reflectance above 1 returns more light than arrived, and a consumer
    // that DIVIDES by this term would amplify instead of demodulate. The
    // fit overshoots by ~4% in the (roughness 0, n.v 0) corner where the
    // true value is exactly 1, which is what the clamp is for -- so this
    // also pins that the clamp is still there.
    for (int ri = 0; ri <= 64; ++ri) {
        const float rough = static_cast<float>(ri) / 64.0f;
        for (int vi = 0; vi <= 64; ++vi) {
            const float nov = static_cast<float>(vi) / 64.0f;
            for (float f0 : {0.0f, 0.02f, 0.04f, 0.5f, 1.0f}) {
                const float rs = EnvBrdfSplitSum(f0, rough, nov);
                CHECK(rs >= 0.0f);
                CHECK(rs <= 1.0f);
            }
        }
    }
    // Unclamped, the corner really does exceed 1 -- the clamp is load-
    // bearing, not decorative.
    const AB ab = EnvBrdfSplitSumAB(0.0f, 0.0f);
    CHECK(0.02f * ab.a + ab.b > 1.0f);
}

TEST_CASE("analytic fit tracks the numerically integrated split-sum") {
    // Agreement is stated as MEASURED, over the domain the engine actually
    // renders: low-F0 dielectrics (water, glass, the planet limb). Both
    // sides are approximations -- see ReferenceSplitSumAB's comment -- so
    // this pins that they agree, not that either is exact. The bounds below
    // are the measured worst case with ~25% headroom; a regression to raw
    // F0 misses them by an order of magnitude (F0 vs 0.78 at the horizon).
    constexpr int kSamples = 16384;
    double sum_abs = 0.0;
    int    n       = 0;
    float  worst   = 0.0f;
    for (int ri = 0; ri <= 20; ++ri) {
        const float rough = static_cast<float>(ri) / 20.0f;
        for (int vi = 1; vi <= 20; ++vi) {   // n.v = 0 is undefined for the reference
            const float nov = static_cast<float>(vi) / 20.0f;
            for (float f0 : {F0FromIor(kWaterIor), F0FromIor(kGlassIor)}) {
                const float fit = EnvBrdfSplitSum(f0, rough, nov);
                const float ref = ReferenceSplitSum(f0, rough, nov, kSamples);
                const float d   = std::fabs(fit - ref);
                worst = std::max(worst, d);
                sum_abs += d;
                ++n;
            }
        }
    }
    const double mean_abs = sum_abs / static_cast<double>(n);
    INFO("mean |fit - ref| = " << mean_abs << ", worst = " << worst);
    CHECK(worst    < 0.20);    // measured 0.155
    CHECK(mean_abs < 0.05);    // measured ~0.02
}

TEST_CASE("the ocean roughness fold lands on the fixture's own stated value") {
    // tests/goldens/scenes/planet_ocean_orbit.cfg states, in its own words,
    // that at 12 m/s wind and a 2 460 m footprint "alpha^2 is exactly Cox &
    // Munk's sigma^2 ... i.e. alpha 0.2539, i.e. an engine roughness of
    // 0.5038". That is an independent anchor for the guide's fold, written
    // down before this work existed: the shader takes oceanBrdfAlpha2's
    // alpha^2 to the FOURTH root to get the perceptual roughness, and it has
    // to land on 0.5038 or the convention is wrong somewhere.
    //
    // Cox & Munk 1954, mean-square slope of a wind-roughened sea:
    //     sigma^2 = 0.003 + 0.00512 * U   (U in m/s)
    const float u = 12.0f;
    const float sigma2 = 0.003f + 0.00512f * u;         // = alpha^2
    const float alpha  = std::sqrt(sigma2);             // GGX alpha
    const float r      = std::sqrt(alpha);              // perceptual, alpha = r^2
    CHECK(std::fabs(alpha - 0.2539f) < 5e-4f);
    CHECK(std::fabs(r     - 0.5038f) < 5e-4f);
    // Which is what `sqrt(sqrt(alpha2))` computes, i.e. the shader's fold.
    CHECK(std::fabs(std::sqrt(std::sqrt(sigma2)) - r) < 1e-6f);
    // And it is emphatically NOT 0 -- the value h0.roughness carries for
    // MAT_WATER, and what the guide would have published without the fold.
    // A consumer told the open ocean is a perfect mirror at 2.5 km/pixel
    // would try to reconstruct a pinpoint sun image where the frame has a
    // glitter patch tens of degrees across.
    CHECK(r > 0.5f);
}

TEST_CASE("Lambert must not receive the split-sum bias term") {
    // With F0 = 0 the fit still returns its B term, which rises toward 1 at
    // grazing incidence. Feeding a matte surface through the fit would
    // therefore tell the consumer it mirrors the horizon -- the same class
    // of error, in the other direction, as telling water it does not. The
    // shader excludes Lambert instead of passing F0 = 0 through; this pins
    // WHY that exclusion is not redundant.
    const float bias_at_grazing = EnvBrdfSplitSum(0.0f, 0.0f, 0.05f);
    CHECK(bias_at_grazing > 0.5f);
}

// ===========================================================================
// 2. Source-text contract. The shader is compiled to SPIR-V and embedded at
//    C++ build time; nothing here executes it, so the checks are on the text.
// ===========================================================================

TEST_CASE("shader mirror: the fit's coefficients have not drifted") {
    const std::string pt = Slurp(PT_SHADER_PATHTRACE_PATH);
    // Exactly one definition, with exactly the coefficients mirrored above.
    CHECK(Count(pt, "float2 envBrdfSplitSumAB(float roughness, float n_dot_v)") == 1);
    CHECK(Count(pt, "const float4 c0 = float4(-1.0, -0.0275, -0.572,  0.022);") == 1);
    CHECK(Count(pt, "const float4 c1 = float4( 1.0,  0.0425,  1.040, -0.040);") == 1);
    CHECK(Count(pt, "min(r.x * r.x, exp2(-9.28 * clamp(n_dot_v, 0.0, 1.0)))") == 1);
    CHECK(Count(pt, "return float2(-1.04, 1.04) * a004 + r.zw;") == 1);
    // And exactly one place that forms F0*A + B, energy-clamped.
    CHECK(Count(pt, "return clamp(f0 * ab.x + ab.y, float3(0.0), float3(1.0));") == 1);
    // The citation is part of the contract in this repo.
    CHECK(Count(pt, "Karis, \"Real Shading in") >= 1);
    CHECK(Count(pt, "\"Getting More Physical in") >= 1);
}

TEST_CASE("specular_albedo writes the integrated reflectance, not F0") {
    const std::string pt = Slurp(PT_SHADER_PATHTRACE_PATH);
    // The guide write goes through the split-sum.
    CHECK(Count(pt, "spec_albedo = envBrdfSplitSum(f0, guide_rough, n_dot_v);") == 1);
    CHECK(Count(pt, "specular_albedo_tex.Store(tid, float4(spec_albedo, 0.0));") == 1);
    // The old raw-F0 write is gone in both of its halves.
    CHECK(Count(pt, "specular_albedo_tex.Store(tid, float4(f0, 0.0));") == 0);
    CHECK(Count(pt, "else if (h0.mat == MAT_DIELECTRIC) f0 = float3(0.04);") == 0);
    // Dielectric and water F0 now come from the same IOR expression the
    // shading branches refract with, rather than a hardcoded 0.04.
    CHECK(Count(pt, "float r0 = (1.0 - n) / (1.0 + n);") == 1);
    CHECK(Count(pt, "float rw = (1.0 - nw) / (1.0 + nw);") == 1);
    CHECK(Count(pt, "water_params0.w") >= 1);
}

TEST_CASE("specular_hit_distance is a real trace, not the smoothness proxy") {
    const std::string pt = Slurp(PT_SHADER_PATHTRACE_PATH);
    // The proxy is gone. `h0.t * smoothness` was not a distance: it
    // collapsed to 0 on rough surfaces and returned the PRIMARY distance on
    // mirrors, which is the one value the reflected image provably is not.
    CHECK(Count(pt, "float smoothness = 1.0 - saturate(h0.roughness);") == 0);
    CHECK(Count(pt, "dist = h0.t * smoothness;") == 0);
    // A real reflection ray, offset and t_min'd like every other secondary
    // ray in this renderer.
    CHECK(Count(pt, "float3 refl_d = reflect(rd0, gn);") == 1);
    CHECK(Count(pt, "HitInfo hr = traceScene(refl_o, refl_d, 1.0, cone_r);") == 1);
    CHECK(Count(pt, "ptRayOrigin(hit_p, gn, cone_r, refl_d);") == 1);
    // The normal is faced against the incoming ray first, or the
    // reflected ray would start by going into the surface it left.
    CHECK(Count(pt, "float3 gn = (dot(h0.normal, rd0) < 0.0) ? h0.normal : -h0.normal;") == 1);
    // Miss -> the sky sentinel depth_tex already uses, not 0. A reflected
    // sky is at infinity and has no parallax; 0 would manufacture some.
    CHECK(Count(pt, "dist = hr.hit ? hr.t : 1.0e10;") == 1);
    CHECK(Count(pt, "depth_view = 1.0e10;") == 1);   // the sentinel's origin
}

TEST_CASE("roughness guide is perceptual roughness, with the ocean fold") {
    const std::string pt = Slurp(PT_SHADER_PATHTRACE_PATH);
    // The engine's convention is alpha = r^2 -- asserted at its two
    // definition sites, because that is what makes h.roughness already the
    // "linear roughness" RR and NRD document and why no remap is applied.
    CHECK(Count(pt, "float alpha = r * r;") == 1);              // anisoAlpha
    CHECK(Count(pt, "float a  = base_roughness * base_roughness;") == 1);
    // Water stores 0 and gets all its roughness from wave slope at shading
    // time; the guide runs the same Cox & Munk fold or it would call the
    // open ocean a mirror from orbit. alpha^2 -> r is the fourth root.
    CHECK(Count(pt, "float alpha2_g = oceanBrdfAlpha2(res_g);") == 1);
    CHECK(Count(pt, "guide_rough = saturate(sqrt(sqrt(max(alpha2_g, 0.0))));") == 1);
    // Lambert has no lobe, so its roughness field is meaningless -> 1.0.
    CHECK(Count(pt, "guide_rough = 1.0;") == 1);
    CHECK(Count(pt, "roughness_tex.Store(tid, guide_rough);") == 1);
}

TEST_CASE("the guide trio is gated on the resolved RR kind, not on a dead denoiser kind") {
    const std::string eng = Slurp(PT_ENGINE_CPP_PATH);
    // One gate, and it reads the RESOLVED denoiser kind.
    //
    // This pinned `DlssRayReconstructionRequested()` (raw cvar intent) when it
    // was written, which was the right shape at the time -- it replaced a
    // MetalFX enumeration no live backend could satisfy, which is how these
    // buffers became dead code. Wiring the NGX RR evaluate then showed cvar
    // intent was still wrong, one step subtler: the SIBLING gates for normal
    // and albedo enumerate denoiser KINDS, so `r_dlss_rr 1` with
    // `r_denoiser off` allocated the specular trio while leaving two of RR's
    // four MANDATORY guides nonexistent. Resolving to the kind puts all four
    // on the same footing, and additionally means a GPU that cannot do RR
    // does not allocate ~50 MB of buffers nothing will read.
    // Pinned as two single-line substrings rather than one multi-line
    // literal: this file has CRLF line endings, so a literal newline
    // escape embedded in the needle would never match.
    CHECK(Count(eng, "const bool want_specular_guidance_gbuffers =") == 1);
    // TWO sites resolve to the RR kind, and both are meant to. The guide gate
    // above decides whether the buffers exist; rr_is_denoiser decides whether
    // the SVGF/NRD/OptiX chain stands down so RR receives raw Monte-Carlo
    // radiance instead of an already-denoised frame. Pinning the count at 2
    // rather than 1 is the point: if either disappears, RR is either fed
    // buffers it does not have or fed a signal something else already
    // filtered.
    CHECK(Count(eng, "const bool rr_is_denoiser =") == 1);
    CHECK(Count(eng, "(denoiser_kind_ == DenoiserKind::DlssRayReconstruction);") == 2);
    // The dead kinds may still be NAMED in a comment that records why the
    // trio was unreachable -- that history is worth keeping. What must not
    // come back is a live comparison against them.
    CHECK(Count(eng, "denoiser_kind_ == DenoiserKind::MetalFX") == 0);
    CHECK(Count(eng, "denoiser_kind_ == DenoiserKind::SvgfBasicMetalFx") == 0);
    CHECK(Count(eng, "denoiser_kind_ == DenoiserKind::SvgfAtrousMetalFx") == 0);
}

// ===========================================================================
// 3. The exposure texture (plan item 4).
// ===========================================================================

TEST_CASE("exposure is published as a 1x1 R32F texture as well as a buffer") {
    const std::string ae  = Slurp(PT_SHADER_AUTOEXPOSURE_PATH);
    const std::string tm  = Slurp(PT_SHADER_TONEMAP_PATH);
    const std::string eng = Slurp(PT_ENGINE_CPP_PATH);
    const std::string vk  = Slurp(PT_VULKAN_DEVICE_PATH);

    // The image is declared and written in the SAME invocation that writes
    // the scalar, so the two views cannot drift.
    CHECK(Count(ae,
        "[[vk::binding(48, 0), vk::image_format(\"r32f\")]] RWTexture2D<float> exposure_tex;") == 1);
    CHECK(Count(ae, "exposure_state[0] = current;") == 1);
    CHECK(Count(ae, "exposure_tex[uint2(0, 0)] = current;") == 1);

    // It is the PRE-tonemap multiplier, which is what NGX asks for: the
    // scalar scales linear radiance and the curve is applied after. Pinned
    // at the tonemap sites so a future change to `exposure * tonemap(c)`
    // would fail here rather than silently invalidate the DLSS input.
    CHECK(Count(tm, "tonemapDispatch(c * exposure_state[0], tonemap_op)") == 3);
    CHECK(Count(tm, "tonemapDispatch(c, tonemap_op) * exposure_state[0]") == 0);

    // Host side: the descriptor layout carries binding 48, engine texture
    // slot 19 maps to it, and the manual-exposure path keeps the image in
    // step (AutoExposure does not run when r_auto_exposure is 0).
    CHECK(Count(vk, "add_binding(48, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);") == 1);
    // The descriptor POOL is sized per storage-image binding, so it has to
    // move in lockstep with the layout or a fully-populated set fails with
    // VK_ERROR_OUT_OF_POOL_MEMORY -- which only shows up on the frames that
    // bind everything, i.e. exactly the DLSS ones.
    CHECK(Count(vk, "VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);") == 19);
    CHECK(Count(vk, "VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,           kTotalSets * 19 + 4") == 1);
    CHECK(Count(vk, "48, // engine slot 19 -> shader binding 48 (exposure_tex, DLSS)") == 1);
    CHECK(Count(vk, "static constexpr std::uint32_t kNumTexSlots = 20;") == 1);
    CHECK(Count(eng, "cb->BindStorageTexture(19, pt::rhi::TextureHandle{exposure_texture_id_});") == 1);
    CHECK(Count(eng, "device_->WriteTexture(pt::rhi::TextureHandle{exposure_texture_id_},") == 2);
    // Allocated only when DLSS is asked for -- the whole point of this work
    // was that a buffer nobody consumes is a buffer nobody validates.
    CHECK(Count(eng, "const bool want_exposure_tex = DlssRequested();") == 1);
    CHECK(Count(eng, ".debug_name = \"dlss_exposure\",") == 1);
}
