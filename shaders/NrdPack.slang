// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
//
// NRD front-end packing kernel (issue #50).
//
// Translates the engine's G-buffer set into the four inputs NVIDIA's
// RayTracingDenoiser asks a RELAX_DIFFUSE instance for:
//
//   IN_MV                    <- motion_tex, verbatim (RG16F, pixel-space,
//                               prev - curr). NRD converts pixels to UV
//                               with CommonSettings::motionVectorScale =
//                               (1/w, 1/h, 0), so no shader-side scaling.
//   IN_VIEWZ                 <- depth_tex, verbatim (R32F linear view Z,
//                               positive along camera forward -- which is
//                               what NRD wants: it converts every input
//                               matrix to left-handed internally, and
//                               ml::Geometry::ReconstructViewPosition then
//                               reads viewZ as +Z-forward).
//   IN_NORMAL_ROUGHNESS      <- normal_tex, oct-packed per NRD's
//                               NRD_NORMAL_ENCODING = 2
//                               (R10_G10_B10_A2_UNORM) layout. See
//                               nrdPackNormalRoughness below.
//   IN_DIFF_RADIANCE_HITDIST <- denoise_color, DEMODULATED by the same
//                               guide the in-house SVGF chain uses
//                               (G = T*A + (1-T)), plus the first-bounce
//                               hit distance in .a.
//
// WHY A SEPARATE COPY RATHER THAN BINDING THE ENGINE TEXTURES DIRECTLY.
// NRD reads its non-pool inputs through SRV (VK_DESCRIPTOR_TYPE_SAMPLED_
// IMAGE) descriptors, and VulkanDevice::CreateTexture only ever requests
// VK_IMAGE_USAGE_STORAGE_BIT | TRANSFER_SRC | TRANSFER_DST -- no
// SAMPLED_BIT. Every image NRD sees therefore has to be one the NRD
// wrapper allocated itself with SAMPLED|STORAGE. Since a copy is
// unavoidable, this kernel does the copy AND the packing in one pass.
//
// DEMODULATION. NRD wants the same thing SVGF wants: the incident
// lighting, not the reflected radiance, so its edge stops filter light
// transport instead of fighting albedo texture detail. The engine
// already derives an aerial-perspective-aware guide for exactly that,
//     G = T * A + (1 - T)
// with A = albedo_tex.rgb and T = albedo_tex.a (camera->surface
// atmospheric transmittance, min over RGB). The derivation lives in
// PathTrace.slang's albedo G-buffer write site and DenoiseTemporal.slang.
// It is reused verbatim here -- NRD does NOT need a demodulation of its
// own, and inventing a second one would mean two divisors that have to
// stay in lockstep. NrdUnpack.slang multiplies the SAME clamped guide
// back on the far side of the denoiser.
//
// SKY PIXELS. The path tracer writes albedo float3(0) and viewZ 1e10 on
// a primary miss. Sky radiance is emissive, not reflected, so demod is
// bypassed for those pixels exactly as DenoiseTemporal.slang does. NRD
// additionally skips every pixel with viewZ >= CommonSettings::
// denoisingRange, which is why NrdUnpack.slang forwards the ORIGINAL
// noisy colour for sky rather than reading NRD's (never-written) output.

// All bindings are storage images on a dedicated 9-slot descriptor set
// layout owned by VulkanNrdLibDenoiser -- this kernel is Vulkan-only
// (the NRD library has no Metal backend), so there is no MSL variant to
// keep under Metal's 8-RW-texture cap.
[[vk::binding(0, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> color_in;     // denoise_color: .rgb noisy HDR, .a first-bounce hitT
[[vk::binding(1, 0), vk::image_format("r32f")]]    RWTexture2D<float>  depth_in;     // depth_tex: linear view Z (1e10 = sky)
[[vk::binding(2, 0), vk::image_format("rg16f")]]   RWTexture2D<float2> motion_in;    // motion_tex: pixel-space (prev - curr)
[[vk::binding(3, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> normal_in;    // normal_tex: world normal .xyz (float3(0) = sky)
[[vk::binding(4, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> albedo_in;    // albedo_tex: .rgb albedo A, .a transmittance T

[[vk::binding(5, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> out_normal_roughness;  // IN_NORMAL_ROUGHNESS
[[vk::binding(6, 0), vk::image_format("r32f")]]    RWTexture2D<float>  out_viewz;             // IN_VIEWZ
[[vk::binding(7, 0), vk::image_format("rg16f")]]   RWTexture2D<float2> out_mv;                // IN_MV
[[vk::binding(8, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> out_radiance_hitdist;  // IN_DIFF_RADIANCE_HITDIST

[[vk::push_constant]]
cbuffer Push {
    uint  width;
    uint  height;
    uint  demod_enabled;   // 1 -> divide by the SVGF demod guide
    uint  _pad0;
};

// Kept in lockstep with DenoiseTemporal.slang / DenoiseAtrous.slang /
// DenoiseRemod.slang. The remod multiply in NrdUnpack.slang MUST use
// the same two constants or demod <-> remod stops round-tripping.
static const float kAlbedoSkyEps = 1.0 / 65536.0;
static const float kDemodEps     = 0.01;

// Half-precision ceiling. NRD's RELAX_FrontEnd_PackRadianceAndHitDist
// sanitises to this range (see NRD.hlsli); IN_DIFF_RADIANCE_HITDIST is
// an RGBA16F texture, so anything above it would store as +inf and
// poison every a-trous tap that touches the pixel.
static const float kNrdFp16Max = 65504.0;

bool isSkyAlbedo(float3 a) {
    return (a.r + a.g + a.b) <= kAlbedoSkyEps;
}

float3 demodGuide(float4 a) {
    return lerp(float3(1.0), a.rgb, saturate(a.a));
}

// NRD_FrontEnd_PackNormalAndRoughness for
// NRD_NORMAL_ENCODING == NRD_NORMAL_ENCODING_R10G10B10A2_UNORM (2) and
// NRD_ROUGHNESS_ENCODING == NRD_ROUGHNESS_ENCODING_LINEAR (1) -- the two
// values cmake/Dependencies.cmake compiles the NRD shaders with.
// Transcribed from NRD.hlsli's _NRD_EncodeNormalRoughness101010; the
// C++ side asserts GetLibraryDesc()->normalEncoding / roughnessEncoding
// still match at instance creation so a CMake change can't silently
// desync this.
//
// Note the storage format here is RGBA16F, not R10G10B10A2_UNORM: NRD
// never inspects the texture format (the integration owns it), the
// encoded values are all in [0, 1] by construction, and fp16's ~11-bit
// mantissa in that range is at least as precise as 10-bit UNORM. Using
// a core-required Vulkan storage format avoids depending on
// shaderStorageImageExtendedFormats, which this device does not enable.
float4 nrdPackNormalRoughness(float3 n, float roughness, float material_id) {
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-9);

    float3 r;
    r.y = n.y * 0.5 + 0.5;
    r.x = n.x * 0.5 + r.y;
    r.y -= n.x * 0.5;

    // Roughness can't be 0 -- its sign carries n.z's sign.
    roughness = max(roughness, 1.5 / 512.0);
    float s = (n.z < 0.0) ? -roughness : roughness;
    r.z = s * 0.5 + 0.5;

    return float4(saturate(r), saturate(material_id / 3.0));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID) {
    if (tid.x >= width || tid.y >= height) return;

    const float  z   = depth_in[tid];
    const float4 nrm = normal_in[tid];
    const float4 alb = albedo_in[tid];
    const float4 col = color_in[tid];

    // --- IN_VIEWZ / IN_MV: straight copies into SAMPLED-capable images.
    out_viewz[tid] = z;
    out_mv[tid]    = motion_in[tid];

    // --- IN_NORMAL_ROUGHNESS ------------------------------------------
    // Sky / miss pixels carry the float3(0) sentinel. NRD ignores them
    // (viewZ >= denoisingRange) but the encode still has to produce a
    // finite unit vector, so substitute a camera-facing normal rather
    // than normalising a zero vector into NaNs that could leak through
    // a bilinear tap on the boundary pixel.
    const float  n_len2 = dot(nrm.xyz, nrm.xyz);
    const float3 n      = (n_len2 > 1e-8) ? (nrm.xyz * rsqrt(n_len2))
                                          : float3(0.0, 0.0, 1.0);
    // Roughness is pinned to 1.0 (fully rough). RELAX_DIFFUSE derives
    // its normal-edge-stop from the DIFFUSE lobe (RELAX_Atrous calls
    // GetNormalWeightParam2 with a hard-coded 1.0 for the diffuse
    // signal) and its roughness-based rejection is specular-only, so
    // the channel is inert for this denoiser. The engine's real
    // roughness G-buffer (binding 25) is allocated for the MetalFX
    // kinds only; wiring it here would cost a G-buffer per frame to
    // feed a value RELAX_DIFFUSE never reads. Revisit if
    // RELAX_DIFFUSE_SPECULAR is ever added.
    out_normal_roughness[tid] = nrdPackNormalRoughness(n, 1.0, 0.0);

    // --- IN_DIFF_RADIANCE_HITDIST -------------------------------------
    float3 rad = col.rgb;
    if (demod_enabled != 0u && !isSkyAlbedo(alb.rgb)) {
        rad /= max(demodGuide(alb), float3(kDemodEps, kDemodEps, kDemodEps));
    }

    // RELAX_FrontEnd_PackRadianceAndHitDist(..., sanitize = true).
    if (any(isnan(rad)) || any(isinf(rad))) rad = float3(0.0);
    rad = clamp(rad, float3(0.0), float3(kNrdFp16Max));

    // .a is the first-bounce hit distance the path tracer recorded
    // (PathTrace.slang, write_nrd_hitdist). 0 means "unknown", which
    // is NRD's documented sentinel. Note the RELAX_DIFFUSE
    // configuration this integration runs disables the pre-pass, the
    // only diffuse consumer of this channel -- see the
    // diffusePrepassBlurRadius comment in VulkanNrdLibDenoiser.cpp.
    // Packed anyway so the front-end format is exactly what
    // RELAX_FrontEnd_PackRadianceAndHitDist specifies.
    float hit_dist = col.a;
    if (isnan(hit_dist) || isinf(hit_dist)) hit_dist = 0.0;
    hit_dist = clamp(hit_dist, 0.0, kNrdFp16Max);

    out_radiance_hitdist[tid] = float4(rad, hit_dist);
}
