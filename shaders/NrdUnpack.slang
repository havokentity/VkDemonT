// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
//
// NRD back-end unpacking kernel (issue #50).
//
// Reads OUT_DIFF_RADIANCE_HITDIST from a RELAX_DIFFUSE instance and
// produces the linear-HDR image the engine's bloom + tonemap chain
// expects in `post_denoise_hdr`.
//
// Three things happen here:
//
//  1. RELAX_BackEnd_UnpackRadiance is the identity in NRD 4.17 (see
//     NRD.hlsli) -- the denoised radiance is just .rgb. .a carries the
//     diffuse history length in frames, which nothing downstream wants,
//     so it is dropped and replaced with 1.0 (the alpha every other
//     write site in this engine's HDR chain stores).
//
//  2. REMODULATION. NrdPack.slang divided the noisy radiance by the
//     engine's SVGF demodulation guide G = T*A + (1-T); this multiplies
//     the SAME clamped guide back. The clamp (kDemodEps) has to match
//     bit-for-bit or the round trip is lossy on dark channels -- see
//     DenoiseRemod.slang, which had exactly that bug.
//
//  3. SKY PASSTHROUGH. NRD does not write output pixels whose viewZ is
//     outside CommonSettings::denoisingRange (NRDDescs.h states this
//     for every denoiser). The engine's primary-miss depth sentinel is
//     1e10, deliberately past the range we configure, so OUT_DIFF_
//     RADIANCE_HITDIST holds stale/garbage there. Those pixels get the
//     original path-traced colour instead. That is not a compromise:
//     the sky, stars, sun and moon are analytic in this renderer, so
//     the un-denoised value is the CORRECT value -- running them
//     through a temporal accumulator is what smears celestials in the
//     first place (issue #46).

[[vk::binding(0, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> nrd_out_in;   // OUT_DIFF_RADIANCE_HITDIST
[[vk::binding(1, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> albedo_in;    // albedo_tex: .rgb albedo A, .a transmittance T
[[vk::binding(2, 0), vk::image_format("r32f")]]    RWTexture2D<float>  depth_in;     // depth_tex: linear view Z
[[vk::binding(3, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> color_src;    // denoise_color: the original noisy HDR
[[vk::binding(4, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> color_out;    // post_denoise_hdr

[[vk::push_constant]]
cbuffer Push {
    uint  width;
    uint  height;
    uint  demod_enabled;
    uint  _pad0;
    float denoising_range;   // mirrors CommonSettings::denoisingRange
    float _pad1;
    float _pad2;
    float _pad3;
};

// Kept in lockstep with NrdPack.slang / DenoiseTemporal.slang.
static const float kAlbedoSkyEps = 1.0 / 65536.0;
static const float kDemodEps     = 0.01;

bool isSkyAlbedo(float3 a) {
    return (a.r + a.g + a.b) <= kAlbedoSkyEps;
}

float3 demodGuide(float4 a) {
    return lerp(float3(1.0), a.rgb, saturate(a.a));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID) {
    if (tid.x >= width || tid.y >= height) return;

    const float z = depth_in[tid];

    // Outside NRD's denoising range -> NRD never touched the pixel.
    if (!(z < denoising_range)) {         // NaN-safe: NaN takes this branch
        color_out[tid] = float4(max(color_src[tid].rgb, float3(0.0)), 1.0);
        return;
    }

    float3 rgb = nrd_out_in[tid].rgb;
    if (any(isnan(rgb)) || any(isinf(rgb))) rgb = float3(0.0);

    const float4 alb = albedo_in[tid];
    if (demod_enabled != 0u && !isSkyAlbedo(alb.rgb)) {
        rgb *= max(demodGuide(alb), float3(kDemodEps, kDemodEps, kDemodEps));
    }

    color_out[tid] = float4(max(rgb, float3(0.0)), 1.0);
}
