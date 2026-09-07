// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
//
// Render-scale upscale (r_render_scale) -- the PLACEHOLDER resolve that
// turns the engine's internal-resolution image into the swapchain-
// resolution image.
//
// WHY THIS EXISTS
// ---------------
// r_render_scale decouples the internal render extent from the
// presentation extent so DLSS (Super Resolution / DLAA / Ray
// Reconstruction) has somewhere to plug in. Everything upstream of this
// kernel -- the path tracer, its accumulator, every denoiser G-buffer,
// the SVGF chain, the celestial composite -- now runs at the INTERNAL
// extent; this kernel is the single point where the pipeline crosses
// back to the output extent. When the DLSS integration lands it
// replaces this dispatch (and consumes the internal-extent HDR + depth
// + motion buffers plus the reported per-frame jitter offset instead);
// until then a plain bilinear magnify is the honest stand-in. It is
// deliberately NOT a sharpening / edge-directed / FSR-style filter:
// this pass is scaffolding, and a clever spatial upscaler here would
// only make the DLSS A/B harder to read.
//
// At r_render_scale = 1.0 the engine does not dispatch this kernel at
// all -- the path tracer writes the swapchain directly, exactly as it
// did before render scaling existed. That is what keeps the golden
// matrix bit-identical at the default.
//
// COLOUR SPACE
// ------------
// Both images hold sRGB-ENCODED LDR in UNORM8 (PathTrace.slang applies
// srgb_oetf() itself on the SPIR-V path -- Vulkan storage-image writes
// do not auto-encode for an sRGB swapchain format). Interpolating
// encoded values directly would filter in a non-linear space and darken
// gradients around bright/dark edges, so this kernel decodes to linear,
// filters, and re-encodes. The transfer functions are byte-for-byte the
// ones in PathTrace.slang (srgbToLinear / srgb_oetf), duplicated here
// rather than imported for the same reason every other small kernel in
// this tree duplicates them: each kernel stays a self-contained
// translation unit with no module-link dependency.
//
// BINDINGS
// --------
// Rides the shared Vulkan descriptor-set layout by engine texture slot:
//   engine slot 0 -> vk::binding(0)  dst = swapchain      (OUTPUT extent)
//   engine slot 1 -> vk::binding(1)  src = present_ldr    (INTERNAL extent)
// Slot 1 is `accum_hdr` for the path tracer; reusing it here is the same
// pattern StarsComposite.slang uses (the shared layout only fixes the
// DESCRIPTOR TYPE per binding -- both are storage images -- and each
// kernel declares the image format of whatever view the engine actually
// binds for that dispatch). Both views really are 8-bit UNORM, so the
// rgba8 format decoration matches; VK_FORMAT_B8G8R8A8_UNORM (the
// swapchain) has no distinct SPIR-V image format and is spelled rgba8
// too -- channel order is a view-side property, see Clear.slang.

[[vk::binding(0, 0), vk::image_format("rgba8")]] RWTexture2D<float4> dst;
[[vk::binding(1, 0), vk::image_format("rgba8")]] RWTexture2D<float4> src;

// 16 bytes -- well under VulkanDevice::kPushSplitOffset (112), so this
// kernel never touches the Frame-UBO spill path.
[[vk::push_constant]]
cbuffer Push {
    uint2 src_dim;      // internal render extent (what `src` holds)
    uint2 dst_dim;      // swapchain extent (what this kernel writes)
};

float3 srgbToLinear(float3 c) {
    float3 lo = c / 12.92;
    float3 hi = pow(max((c + 0.055) / 1.055, 0.0), float3(2.4));
    return select(c <= 0.04045, lo, hi);
}

float3 srgb_oetf(float3 c) {
    c = max(c, float3(0.0, 0.0, 0.0));
    float3 lin  = c * 12.92;
    float3 expo = 1.055 * pow(c, float3(1.0 / 2.4)) - 0.055;
    return select(c < 0.0031308, lin, expo);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID) {
    if (any(tid >= dst_dim)) return;

    int2 sd = int2(max(src_dim, uint2(1, 1)));

    // Pixel-centre-aligned map from destination to source. The +0.5 /
    // -0.5 pair is what makes the two grids agree on where the image
    // edges are: without it a 2x magnify samples half a texel off and
    // the picture creeps up-left as the scale changes.
    float2 sxy = (float2(tid) + 0.5) * (float2(src_dim) / float2(dst_dim));
    float2 fxy = sxy - 0.5;
    int2   i0  = int2(floor(fxy));
    float2 t   = fxy - float2(i0);

    // Clamp-to-edge addressing. These are storage-image loads, not
    // sampler fetches, so the border behaviour is ours to define.
    int2 p00 = clamp(i0,                 int2(0, 0), sd - int2(1, 1));
    int2 p10 = clamp(i0 + int2(1, 0),    int2(0, 0), sd - int2(1, 1));
    int2 p01 = clamp(i0 + int2(0, 1),    int2(0, 0), sd - int2(1, 1));
    int2 p11 = clamp(i0 + int2(1, 1),    int2(0, 0), sd - int2(1, 1));

    float3 c00 = srgbToLinear(src[uint2(p00)].rgb);
    float3 c10 = srgbToLinear(src[uint2(p10)].rgb);
    float3 c01 = srgbToLinear(src[uint2(p01)].rgb);
    float3 c11 = srgbToLinear(src[uint2(p11)].rgb);

    float3 a = lerp(c00, c10, t.x);
    float3 b = lerp(c01, c11, t.x);
    float3 lin = lerp(a, b, t.y);

    dst[tid] = float4(srgb_oetf(lin), 1.0);
}
