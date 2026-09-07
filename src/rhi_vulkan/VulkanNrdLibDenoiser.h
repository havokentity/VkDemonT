// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
//
// NVIDIA RayTracingDenoiser (NRD) library integration -- issue #50.
//
// This is the real thing: an nrd::Instance carrying a RELAX_DIFFUSE
// denoiser, its pipelines built from the SPIR-V NRD embeds, its
// permanent + transient texture pools allocated, and a per-frame
// GetComputeDispatches() walker that translates NRD's DispatchDesc list
// into vkCmdBindDescriptorSets + vkCmdDispatch. It replaces the v0.3.30
// scaffold that only called CreateInstance and then passed the noisy
// image through unchanged.
//
// ---------------------------------------------------------------------
// WHY RELAX AND NOT REBLUR
// ---------------------------------------------------------------------
// NRD ships two radiance denoisers. They are not interchangeable and the
// choice is forced by what this renderer actually produces:
//
//   * REBLUR is recurrent-blur based. Its whole filter footprint is
//     driven by a NORMALISED hit distance -- the application must call
//     REBLUR_FrontEnd_GetNormHitDist(hitDist, viewZ, hitDistParams,
//     roughness) and hand over a curve
//         f = (A + viewZ * B) * lerp(C, 1, smc)
//     whose three constants have to be tuned to the scene's scale.
//     This engine is a planetary path tracer: one scene has a Cornell
//     box at metre scale and the next has a camera in a 400 km orbit
//     looking at ground hundreds of kilometres away, with a 1e10 sky
//     sentinel. There is no single (A, B, C) that is right for both, and
//     a wrong one either mushes the image or does nothing. REBLUR is
//     also tuned for lower-variance input (well importance-sampled RT
//     with a few spp) -- it over-blurs a 1-spp path trace.
//
//   * RELAX is an SVGF derivative: temporal accumulation with a
//     variance-driven a-trous chain, exactly the family this engine's
//     in-house denoiser already implements. It takes the RAW hit
//     distance (RELAX_FrontEnd_PackRadianceAndHitDist is literally
//     float4(radiance, hitDist)), so there is no normalisation curve to
//     get wrong, and NVIDIA position it as the path-tracing / high-
//     variance choice. Its documented "unknown hit distance" fallback is
//     the value 0, which means the integration degrades honestly on
//     pixels where the tracer could not produce a distance.
//
// RELAX_DIFFUSE (not RELAX_DIFFUSE_SPECULAR) because the megakernel
// produces ONE combined radiance estimate per pixel -- there is no
// diffuse/specular path split to feed the two-signal variant, and
// synthesising one would be inventing data.
//
// The other happy consequence: RELAX_DIFFUSE's whole texture pool is
// RGBA16_SFLOAT / RGBA8_UNORM / R8_UNORM / R32_SFLOAT, all of which are
// core-required Vulkan storage formats. REBLUR's pool reaches for
// R10_G10_B10_A2_UNORM and R11_G11_B10_UFLOAT, which would have needed
// shaderStorageImageExtendedFormats enabled on the device.
//
// ---------------------------------------------------------------------
// WHAT NRD NEEDS THAT THE ENGINE ALREADY HAD
// ---------------------------------------------------------------------
//   IN_MV        <- motion_tex. RG16F, pixel-space, prev - curr. NRD's
//                   contract is "pixelUvPrev = pixelUv + mv * mvScale",
//                   so CommonSettings::motionVectorScale = (1/w, 1/h, 0)
//                   converts. Direction and Y-orientation already match
//                   (PathTrace flips NDC Y when it forms the delta).
//   IN_VIEWZ     <- depth_tex. R32F linear view Z, POSITIVE along camera
//                   forward. That is what NRD wants: it converts every
//                   supplied matrix to left-handed internally and then
//                   reconstructs view position as p.z = viewZ with +Z
//                   forward (ml.hlsli, Geometry::ReconstructViewPosition).
//   IN_NORMAL_ROUGHNESS <- normal_tex, re-encoded (see NrdPack.slang).
//   albedo guide <- albedo_tex .rgb/.a, reused verbatim.
//
// WHAT IT NEEDED THAT THE ENGINE DID NOT PRODUCE
//   * A packed IN_NORMAL_ROUGHNESS. The engine has a world-space normal
//     G-buffer but no roughness alongside it and no oct packing.
//     NrdPack.slang does the encode; roughness is pinned to 1.0 because
//     RELAX_DIFFUSE's normal weight uses the diffuse lobe and its
//     roughness rejection is specular-only.
//   * A first-bounce hit distance. Added as a runtime-gated write into
//     denoise_color.a (PathTrace.slang, `write_nrd_hitdist`) -- a
//     channel nothing downstream reads.
//   * SAMPLED-capable images. NRD reads its inputs through SRVs, and
//     VulkanDevice::CreateTexture never requests VK_IMAGE_USAGE_SAMPLED
//     _BIT. Every image NRD touches is therefore allocated by this class
//     with SAMPLED|STORAGE.
//
// The engine's SVGF demodulation guide G = T*A + (1-T) IS reusable and
// IS reused: NRD wants demodulated radiance for exactly the same reason
// SVGF does. NrdPack divides by it, NrdUnpack multiplies it back, and
// both use DenoiseTemporal.slang's constants so the round trip matches.
//
// ---------------------------------------------------------------------
// LIFECYCLE
// ---------------------------------------------------------------------
// Lazy: VulkanDevice::Denoise constructs the object on the first
// Kind::Nrd frame and calls Init(w, h). NRD 4.17 bakes no resolution
// into the instance (that moved to CommonSettings::resourceSize), but
// the texture pools are sized to w x h, so a resize tears the GPU
// objects down and rebuilds them while KEEPING the nrd::Instance.
// A hard failure latches: Ready() stays false, VulkanDevice reports
// SupportsNrdLibrary() == false, and the engine downgrades to SVGF.

#pragma once

#if defined(PT_ENABLE_NRD)

#include "../rhi/Handles.h"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// Forward-declare NRD's opaque instance so <NRD.h> stays out of every
// translation unit that includes this header.
namespace nrd { struct Instance; }

namespace pt::rhi::vk {

class VulkanDevice;

class VulkanNrdLibDenoiser {
public:
    explicit VulkanNrdLibDenoiser(VulkanDevice* parent) : device_(parent) {}
    ~VulkanNrdLibDenoiser();

    VulkanNrdLibDenoiser(const VulkanNrdLibDenoiser&)            = delete;
    VulkanNrdLibDenoiser& operator=(const VulkanNrdLibDenoiser&) = delete;

    // Everything Encode() needs from the RHI's DenoiseDesc, without
    // dragging rhi/Device.h into this header.
    struct EncodeInputs {
        TextureHandle color_in;    // denoise_color   (RGBA16F, .a = hitT)
        TextureHandle depth_in;    // depth_tex       (R32F linear view Z)
        TextureHandle motion_in;   // motion_tex      (RG16F, prev - curr px)
        TextureHandle normal_in;   // normal_tex      (RGBA16F world normal)
        TextureHandle albedo_in;   // albedo_tex      (RGBA16F, .a = T)
        TextureHandle output;      // post_denoise_hdr (RGBA16F)
        const float*  world_to_view = nullptr;  // column-major 4x4
        const float*  view_to_clip  = nullptr;  // column-major 4x4
        float         jitter_x      = 0.0f;     // [-0.5, 0.5] pixels
        float         jitter_y      = 0.0f;
        bool          reset_history = false;
        bool          demod_enabled = false;
    };

    // Build the nrd::Instance + every GPU object at `width x height`.
    // Idempotent on size match; rebuilds the size-dependent half on a
    // resize. Returns false on hard failure, which LATCHES: a second
    // call at the same size returns false without retrying, because
    // nothing about a failed CreateInstance / vkCreateComputePipelines
    // becomes true by asking again.
    bool Init(std::uint32_t width, std::uint32_t height);

    // True once Init() has succeeded and nothing has invalidated it.
    bool Ready() const { return ready_; }

    // True once a hard failure has latched. VulkanDevice reports the
    // negation through Device::SupportsNrdLibrary so the engine can
    // downgrade `r_denoiser nrd` to SVGF with a log line.
    bool Failed() const { return failed_; }

    // Record the whole chain onto `cb`:
    //   NrdPack  -> NRD RELAX_DIFFUSE dispatches -> NrdUnpack
    // Leaves the denoised linear-HDR image in `in.output`. The caller
    // owns everything before (the path-tracer barrier) and after (the
    // bloom + tonemap finalize).
    void Encode(VkCommandBuffer cb, const EncodeInputs& in);

private:
    // One image this class owns outright: NRD pool slots and the
    // app-side NRD I/O textures. All are created SAMPLED|STORAGE and
    // live in VK_IMAGE_LAYOUT_GENERAL for their whole lifetime, so the
    // per-dispatch barriers are memory-only.
    struct OwnedImage {
        VkImage        image  = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView    view   = VK_NULL_HANDLE;
        VkFormat       format = VK_FORMAT_UNDEFINED;
        std::uint32_t  width  = 0;
        std::uint32_t  height = 0;
    };

    bool CreateInstanceOnce();
    bool BuildNrdPipelineObjects();     // layouts, pipelines, samplers (size-independent)
    bool BuildSizedResources(std::uint32_t w, std::uint32_t h);
    bool BuildAuxPipelines();           // NrdPack / NrdUnpack (size-independent)
    bool BuildConstantBuffer();

    bool CreateOwnedImage(std::uint32_t w, std::uint32_t h, VkFormat fmt,
                          const char* debug_label, OwnedImage& out);
    void DestroyOwnedImage(OwnedImage& img);

    void DestroySizedResources();
    void DestroyAll();

    // One-shot UNDEFINED -> GENERAL transition for every owned image.
    void RecordInitialLayoutTransitions(VkCommandBuffer cb);

    // Global compute->compute memory barrier. NRD's own integration
    // emits per-resource barriers; with every image permanently in
    // GENERAL there is no layout work to do, so one execution +
    // memory dependency per dispatch is both sufficient and simpler
    // to keep correct.
    static void ComputeBarrier(VkCommandBuffer cb);

    std::uint32_t FindMemoryType(std::uint32_t type_bits,
                                 VkMemoryPropertyFlags props) const;

    VulkanDevice*  device_   = nullptr;
    nrd::Instance* nrd_inst_ = nullptr;

    bool ready_  = false;
    bool failed_ = false;
    bool needs_layout_init_ = true;

    std::uint32_t cached_w_ = 0;
    std::uint32_t cached_h_ = 0;
    // Dimensions the last failed Init() was asked for, so a resize can
    // legitimately retry while a same-size retry stays suppressed.
    std::uint32_t failed_w_ = 0;
    std::uint32_t failed_h_ = 0;

    // CommonSettings::frameIndex. Must advance by exactly 1 per frame.
    std::uint32_t frame_index_ = 0;
    // Forces AccumulationMode::CLEAR_AND_RESTART on the next Encode.
    // Set whenever the texture pools were (re)allocated: the permanent
    // pool holds whatever the driver left in that memory, and NRD's
    // history passes would happily read it as last frame's result.
    // Cleared after one Encode.
    bool force_accum_restart_ = true;
    // Previous-frame matrices, kept here because the engine only hands
    // us the current ones.
    float prev_world_to_view_[16] {};
    float prev_view_to_clip_[16]  {};
    float prev_jitter_[2] {};
    bool  have_prev_matrices_ = false;

    // ---- NRD pipeline objects (size-independent) --------------------
    VkDescriptorSetLayout nrd_res_layout_  = VK_NULL_HANDLE;  // space = resourcesSpaceIndex
    VkDescriptorSetLayout nrd_cb_layout_   = VK_NULL_HANDLE;  // space = constantBufferAndSamplersSpaceIndex
    VkDescriptorSetLayout nrd_empty_layout_ = VK_NULL_HANDLE; // gap filler if the two spaces aren't adjacent
    VkPipelineLayout      nrd_pipe_layout_ = VK_NULL_HANDLE;
    std::vector<VkPipeline> nrd_pipelines_;
    VkSampler             sampler_nearest_ = VK_NULL_HANDLE;
    VkSampler             sampler_linear_  = VK_NULL_HANDLE;
    VkDescriptorPool      nrd_dpool_       = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> nrd_res_sets_;
    std::uint32_t         nrd_set_cursor_  = 0;
    VkDescriptorSet       nrd_cb_set_      = VK_NULL_HANDLE;

    // Cached from InstanceDesc so the hot loop doesn't re-query.
    std::uint32_t texture_binding_base_ = 0;
    std::uint32_t storage_binding_base_ = 0;
    std::uint32_t res_space_index_      = 0;
    std::uint32_t cb_space_index_       = 1;
    std::uint32_t permanent_pool_size_  = 0;

    // ---- NRD constant buffer ring -----------------------------------
    VkBuffer       cb_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory cb_memory_ = VK_NULL_HANDLE;
    void*          cb_mapped_ = nullptr;
    std::uint32_t  cb_stride_ = 0;   // aligned per-dispatch slice
    std::uint32_t  cb_slots_  = 0;
    std::uint32_t  cb_cursor_ = 0;

    // ---- Owned images -----------------------------------------------
    std::vector<OwnedImage> pool_;          // permanent then transient
    OwnedImage in_mv_;
    OwnedImage in_normal_roughness_;
    OwnedImage in_viewz_;
    OwnedImage in_radiance_;
    OwnedImage out_radiance_;

    // ---- NrdPack / NrdUnpack ----------------------------------------
    VkDescriptorSetLayout pack_layout_        = VK_NULL_HANDLE;
    VkPipelineLayout      pack_pipe_layout_   = VK_NULL_HANDLE;
    VkPipeline            pack_pipe_          = VK_NULL_HANDLE;
    VkDescriptorSetLayout unpack_layout_      = VK_NULL_HANDLE;
    VkPipelineLayout      unpack_pipe_layout_ = VK_NULL_HANDLE;
    VkPipeline            unpack_pipe_        = VK_NULL_HANDLE;
    VkDescriptorPool      aux_dpool_          = VK_NULL_HANDLE;
    static constexpr int  kAuxSetRing         = 8;
    VkDescriptorSet       pack_sets_[kAuxSetRing]   {};
    VkDescriptorSet       unpack_sets_[kAuxSetRing] {};
    int                   next_aux_set_       = 0;

    // One-shot log gate so a per-frame dispatch failure can't spam the
    // log (Init's summary is naturally one-shot -- Init returns early
    // when it is already ready at the requested dimensions).
    bool logged_dispatch_error_ = false;
};

}  // namespace pt::rhi::vk

#endif  // PT_ENABLE_NRD
