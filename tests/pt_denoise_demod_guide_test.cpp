// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
//
// Host mirror of the SVGF demodulation-guide contract.
//
// WHY THIS FILE EXISTS
//
// The in-house SVGF chain divides denoise_color by an albedo guide on the
// way in (DenoiseTemporal.slang) and multiplies the SAME guide back on the
// way out (DenoiseAtrous.slang final pass, or DenoiseRemod.slang). On a
// planet the primary-hit radiance is C = T*A*E + S -- attenuated surface
// radiance plus additive in-scatter -- so dividing by the raw albedo A
// sends S/A to tens of times the physical radiance on a dark, skewed
// surface, and the filter then blends that signal across pixels of a
// different albedo and re-multiplies by the centre one: the magenta /
// green band over distant water and ridges at a low sun. The guide is
// therefore G = T*A + (1 - T), with T (camera->surface atmospheric
// transmittance) carried in albedo_tex.a by PathTrace.slang.
//
// None of that can be checked by a golden on the software backend: the
// software tracer is a hand-written CPU renderer that does not execute the
// Slang, and no Vulkan SVGF golden cell exists on this tree yet. What CAN
// be pinned without a GPU is
//
//   1. the guide is ONE formula, spelled identically in the three kernels
//      that must agree (a divide and a multiply that disagree are a hue
//      shift on every surface, not a crash);
//   2. every demod / remod site uses the guide and none has drifted back
//      to the raw albedo;
//   3. PathTrace writes T into .a at its single albedo_tex producer, and
//      the dead MetalFX floor that used to live there is gone;
//   4. the other consumers of albedo_tex still read .rgb as the raw
//      albedo (RestirFinal shades with it), which is why the guide is
//      assembled in the denoiser instead of being written into .rgb;
//   5. the property the guide is derived from -- C / G bounded by the
//     physical radiances for every albedo and every transmittance, where
//     C / A is not.
//
// COUNT OCCURRENCES, DO NOT TEST FOR PRESENCE (see pt_planet_shader_test).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

namespace {

// Read a source file with line endings normalised to LF. The one
// multi-line needle below (the guide body) must match on a Windows
// checkout with core.autocrlf as well as on Linux CI; the single-line
// needles do not care.
std::string Slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    REQUIRE_MESSAGE(f.good(), "cannot open ", path);
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

// Remove `//`-to-end-of-line comments so prose quoting a statement does
// not count as the statement.
std::string StripLineComments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    bool in_comment = false;
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (!in_comment && src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            in_comment = true;
        }
        if (src[i] == '\n') in_comment = false;
        if (!in_comment) out.push_back(src[i]);
    }
    return out;
}

std::size_t CountOccurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos;
         at = hay.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

// The three kernels that must agree, in chain order.
struct Kernel {
    const char* name;
    const char* path;
};
const Kernel kKernels[] = {
    { "DenoiseTemporal", PT_SHADER_DENOISE_TEMPORAL_PATH },
    { "DenoiseAtrous",   PT_SHADER_DENOISE_ATROUS_PATH },
    { "DenoiseRemod",    PT_SHADER_DENOISE_REMOD_PATH },
};

// The guide, character for character. Three copies are tolerated only
// because they are pinned identical here; a fourth spelling is a bug.
const char* kGuideDef =
    "float3 demodGuide(float4 a) {\n"
    "    return lerp(float3(1.0), a.rgb, saturate(a.a));\n"
    "}";
const char* kGuidedClamp  = "max(demodGuide(a), float3(kDemodEps, kDemodEps, kDemodEps))";
const char* kRawClamp     = "max(a, float3(kDemodEps, kDemodEps, kDemodEps))";
const char* kDemodEpsDecl = "static const float kDemodEps = 0.01;";

}  // namespace

TEST_CASE("the demod guide is one formula, declared once in each SVGF kernel") {
    for (const Kernel& k : kKernels) {
        INFO(std::string(k.name));
        const std::string src = StripLineComments(Slurp(k.path));
        CHECK(CountOccurrences(src, kGuideDef) == 1);
        // The eps floor under the guide is shared too: a kernel with a
        // different floor multiplies back a different number than was
        // divided.
        CHECK(CountOccurrences(src, kDemodEpsDecl) == 1);
        // Exactly one site per kernel applies the clamped guide -- the
        // divide in Temporal, the multiply-back in Atrous and Remod --
        // and none has drifted back to the raw albedo.
        CHECK(CountOccurrences(src, kGuidedClamp) == 1);
        CHECK(CountOccurrences(src, kRawClamp) == 0);
    }
    // Temporal divides; Atrous / Remod multiply. Pin the operator so a
    // future edit cannot swap one for the other and still pass above.
    const std::string temporal = StripLineComments(Slurp(PT_SHADER_DENOISE_TEMPORAL_PATH));
    const std::string atrous   = StripLineComments(Slurp(PT_SHADER_DENOISE_ATROUS_PATH));
    const std::string remod    = StripLineComments(Slurp(PT_SHADER_DENOISE_REMOD_PATH));
    CHECK(CountOccurrences(temporal, std::string("c /= ") + kGuidedClamp) == 1);
    CHECK(CountOccurrences(atrous,   std::string("filtered_color *= ") + kGuidedClamp) == 1);
    CHECK(CountOccurrences(remod,    std::string("demod.rgb * ") + kGuidedClamp) == 1);
    // The guide reads the full float4 -- .a is the transmittance. A
    // kernel that fetches only .rgb into `a` has either stopped calling
    // the guide or rebuilt a float4 with a = 0, which is G = 1: no
    // demodulation at all, and no compile error to say so.
    for (const Kernel& k : kKernels) {
        INFO(std::string(k.name));
        const std::string src = StripLineComments(Slurp(k.path));
        CHECK(CountOccurrences(src, "float3 a = albedo_tex[") == 0);
        CHECK(CountOccurrences(src, "float3 a     = albedo_tex[") == 0);
    }
}

TEST_CASE("PathTrace writes the guide transmittance into albedo_tex.a at its one producer") {
    const std::string src = StripLineComments(Slurp(PT_SHADER_PATHTRACE_PATH));
    // One producer of albedo_tex in the whole path tracer.
    CHECK(CountOccurrences(src, "albedo_tex[tid] =") == 1);
    CHECK(CountOccurrences(src, "albedo_tex[tid] = float4(hit_albedo, guide_trans);") == 1);
    // The scalar is the MIN over channels -- the direction that never
    // under-states the hazed fraction (see the derivation at the write
    // site). A mean or a luminance here would re-open a bounded but real
    // blue-channel overshoot.
    CHECK(CountOccurrences(src, "guide_trans = min(T.r, min(T.g, T.b));") == 1);
    CHECK(CountOccurrences(src, "atmosphericTransmittance(ro0, rd0, h0.t)") == 1);
    // The MetalFX-era floor is gone, and nothing writes a zero .a for a
    // surface hit any more.
    CHECK(CountOccurrences(src, "kMetalFxDemodFloor") == 0);
    CHECK(CountOccurrences(src, "float4(guide, 0.0)") == 0);
    CHECK(CountOccurrences(src, "float4(hit_albedo, 0.0)") == 0);
}

TEST_CASE("the other albedo_tex consumers still read the raw albedo from .rgb") {
    // RestirFinal shades analytic lights with the G-buffer albedo. It must
    // keep reading .rgb -- which is the whole reason T lives in .a and the
    // guide is assembled in the denoiser rather than written into .rgb.
    const std::string restir = StripLineComments(Slurp(PT_SHADER_RESTIR_FINAL_PATH));
    CHECK(CountOccurrences(restir, "albedo_tex[tid.xy].rgb") == 1);
    CHECK(CountOccurrences(restir, "albedo_tex[tid.xy].a") == 0);
    CHECK(CountOccurrences(restir, "demodGuide") == 0);
}

TEST_CASE("C / G is bounded by the physical radiances for every albedo and transmittance; C / A is not") {
    // The model the guide is derived from, per channel:
    //   C = T*A*E + (1 - T)*L
    // E = incident lighting at the surface, L = haze source radiance, T
    // = per-channel transmittance (Rayleigh: blue attenuates most), Tm =
    // min over channels (what PathTrace stores). Claim: for A in [0,1],
    // C / (Tm*A + (1 - Tm)) <= max(E, L). Proof sketch, per channel:
    // Tm*A + (1 - Tm) >= T*A + (1 - T) because (T - Tm)(1 - A) >= 0,
    // and the numerator is <= (T*A + (1 - T)) * max(E, L).
    const double E[3] = { 40.0, 45.0, 50.0 };    // surface lighting
    const double L[3] = { 60.0, 75.0, 95.0 };    // sky-coloured haze
    const double eps  = 0.01;                    // kDemodEps
    double worst_guided_over = 0.0;              // max over grid of (C/G)/max(E,L)
    double worst_raw_over    = 0.0;              // same for the raw divide
    // Rayleigh-like split: T_b <= T_g <= T_r, walked from vacuum-clear to
    // fully hazed.
    for (int it = 0; it <= 20; ++it) {
        const double tm  = it / 20.0;                 // blue channel
        const double T[3] = { std::pow(tm, 0.5), std::pow(tm, 0.7), tm };
        // Albedos from pure black through the near-black skewed cases to
        // the ocean shell's 1.0; per-channel skew on the dark end.
        for (int ia = 0; ia <= 40; ++ia) {
            const double a_base = ia / 40.0;
            const double A[3] = { std::min(1.0, a_base * 0.5),
                                  std::min(1.0, a_base * 0.8),
                                  a_base };
            for (int c = 0; c < 3; ++c) {
                const double C      = T[c] * A[c] * E[c] + (1.0 - T[c]) * L[c];
                const double bound  = std::max(E[c], L[c]);
                const double guide  = std::max(tm * A[c] + (1.0 - tm), eps);
                const double raw    = std::max(A[c], eps);
                worst_guided_over = std::max(worst_guided_over, (C / guide) / bound);
                worst_raw_over    = std::max(worst_raw_over,    (C / raw)   / bound);
            }
        }
    }
    // The guided demod never exceeds the physical radiances (1e-9 is
    // double rounding, not tolerance).
    CHECK(worst_guided_over <= 1.0 + 1e-9);
    // The raw divide reaches 1/eps = 100x of them on a near-black hazy
    // pixel -- the signal SVGF was blending across albedo seams.
    CHECK(worst_raw_over > 50.0);
    // And the guide degenerates to the albedo when there is no air: with
    // T = 1 in every channel the guided and the raw divide are the same
    // operations on the same numbers -- bit-identical, not merely close --
    // which is what keeps every non-atmosphere scene unchanged.
    for (int ia = 0; ia <= 40; ++ia) {
        const double a = ia / 40.0;
        for (int c = 0; c < 3; ++c) {
            const double C     = 1.0 * a * E[c] + (1.0 - 1.0) * L[c];
            const double guide = std::max(1.0 * a + (1.0 - 1.0), eps);
            const double raw   = std::max(a, eps);
            CHECK(C / guide == C / raw);
        }
    }
}
