// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
//
// See VulkanNgxUpscaler.h for the design notes. This file is the NGX
// call sequence and nothing else.

#include "VulkanNgxUpscaler.h"
#include "VulkanDevice.h"

#include "../core/Log.h"

#if defined(PT_ENABLE_DLSS)
// nvsdk_ngx_helpers_vk.h pulls in nvsdk_ngx_vk.h -> nvsdk_ngx.h, and
// expects <vulkan/vulkan.h> to already be visible (VulkanNgxUpscaler.h
// includes it above).
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>
#endif

#include <algorithm>

namespace pt::rhi::vk {

#if defined(PT_ENABLE_DLSS)
namespace {

// The application identity NGX keys its per-app tuning and its
// over-the-air model updates on. NVIDIA hands out numeric application
// IDs to partners; without one the documented path is a project GUID
// plus NVSDK_NGX_ENGINE_TYPE_CUSTOM, which is what this is. The GUID is
// arbitrary but must be STABLE -- changing it makes NGX treat the engine
// as a different application and discard whatever profile it had built.
constexpr const char* kProjectId    = "d2d9a1f4-6c5b-4f2e-9a3d-7e1c0b845f60";
constexpr const char* kEngineVersion = "0.1.0";

// Where NGX writes its logs and any downloaded model data. L"." keeps
// them beside the executable, which is where every other diagnostic this
// engine produces already goes.
constexpr const wchar_t* kAppDataPath = L".";

// Translate the RHI's mode ladder into NVIDIA's enum. The two orderings
// deliberately differ (see UpscalerMode's comment); this function is the
// only place that knows NVIDIA's.
NVSDK_NGX_PerfQuality_Value ToNgxQuality(UpscalerMode m) {
    switch (m) {
        case UpscalerMode::Dlaa:             return NVSDK_NGX_PerfQuality_Value_DLAA;
        case UpscalerMode::Quality:          return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        case UpscalerMode::Balanced:         return NVSDK_NGX_PerfQuality_Value_Balanced;
        case UpscalerMode::Performance:      return NVSDK_NGX_PerfQuality_Value_MaxPerf;
        case UpscalerMode::UltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        case UpscalerMode::Off:
        default:                             return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    }
}

const char* ModeName(UpscalerMode m) {
    switch (m) {
        case UpscalerMode::Dlaa:             return "dlaa";
        case UpscalerMode::Quality:          return "quality";
        case UpscalerMode::Balanced:         return "balanced";
        case UpscalerMode::Performance:      return "performance";
        case UpscalerMode::UltraPerformance: return "ultra_performance";
        case UpscalerMode::Off:
        default:                             return "off";
    }
}

// NVSDK_NGX_FAILED expands to `((value) & 0xFFF00000) == NVSDK_NGX_Result_Fail`,
// which compares an `unsigned int` (the masked value) against a signed
// enumerator and trips -Wsign-compare on every use. Wrapping it once
// with an explicit cast keeps the SDK's semantics byte-for-byte while
// letting this TU build under the project's warning settings -- better
// than a per-site pragma, and much better than editing the vendored
// header.
inline bool NgxFailed(NVSDK_NGX_Result r) {
    return (static_cast<std::uint32_t>(r) & 0xFFF00000u) ==
           static_cast<std::uint32_t>(NVSDK_NGX_Result_Fail);
}

// NGX results are an enum with a 0xBAD00000-style error space; the SDK
// ships NVSDK_NGX_GetResultAsString returning a wide string. Narrow it
// for the log, falling back to the raw code when the SDK has no text.
std::string ResultText(NVSDK_NGX_Result r) {
    const wchar_t* w = GetNGXResultAsString(r);
    std::string    s;
    if (w != nullptr) {
        for (const wchar_t* p = w; *p != L'\0'; ++p) {
            s.push_back((*p < 128) ? static_cast<char>(*p) : '?');
        }
    }
    if (s.empty()) s = "unknown";
    return s + " (0x" + [&] {
        static const char* kHex = "0123456789abcdef";
        auto               v    = static_cast<std::uint32_t>(r);
        std::string        h(8, '0');
        for (int i = 7; i >= 0; --i) { h[static_cast<std::size_t>(i)] = kHex[v & 0xFu]; v >>= 4; }
        return h;
    }() + ")";
}

// Shared by the two static extension queries: build the discovery info
// NGX wants for the SuperSampling feature.
NVSDK_NGX_FeatureDiscoveryInfo MakeDiscoveryInfo(
    NVSDK_NGX_ProjectIdDescription& desc_storage) {
    desc_storage.ProjectId     = kProjectId;
    desc_storage.EngineType    = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    desc_storage.EngineVersion = kEngineVersion;

    NVSDK_NGX_FeatureDiscoveryInfo info{};
    info.SDKVersion                = NVSDK_NGX_Version_API;
    info.FeatureID                 = NVSDK_NGX_Feature_SuperSampling;
    info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    info.Identifier.v.ProjectDesc  = desc_storage;
    info.ApplicationDataPath       = kAppDataPath;
    info.FeatureInfo               = nullptr;
    return info;
}

// Wrap one of the engine's textures as an NGX resource descriptor.
// `read_write` must be true for the output (NGX writes it through a
// storage descriptor) and false for every input.
NVSDK_NGX_Resource_VK MakeResource(VulkanDevice* dev,
                                   TextureHandle h,
                                   bool          read_write) {
    VkImageSubresourceRange range{};
    range.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel   = 0;
    range.levelCount     = 1;
    range.baseArrayLayer = 0;
    range.layerCount     = 1;

    const VkExtent2D ext = dev->LookupImageExtent(h);
    return NVSDK_NGX_Create_ImageView_Resource_VK(
        dev->LookupImageView(h),
        dev->LookupImage(h),
        range,
        dev->LookupImageFormat(h),
        ext.width,
        ext.height,
        read_write);
}

}  // namespace
#endif  // PT_ENABLE_DLSS

VulkanNgxUpscaler::VulkanNgxUpscaler(VulkanDevice* dev) : device_(dev) {}

VulkanNgxUpscaler::~VulkanNgxUpscaler() {
#if defined(PT_ENABLE_DLSS)
    DestroyFeature();
    if (feature_params_ != nullptr) {
        NVSDK_NGX_VULKAN_DestroyParameters(feature_params_);
        feature_params_ = nullptr;
    }
    // caps_params_ comes from GetCapabilityParameters and is owned by
    // the NGX runtime; Shutdown1 releases it. Destroying it explicitly
    // as well is a double free.
    caps_params_ = nullptr;
    if (ngx_inited_ && device_ != nullptr) {
        NVSDK_NGX_VULKAN_Shutdown1(device_->RawDevice());
        ngx_inited_ = false;
    }
#endif
}

// ---------------------------------------------------------------------
// Device-creation requirements
// ---------------------------------------------------------------------

std::vector<std::string> VulkanNgxUpscaler::RequiredInstanceExtensions() {
    std::vector<std::string> out;
#if defined(PT_ENABLE_DLSS)
    NVSDK_NGX_ProjectIdDescription desc{};
    const NVSDK_NGX_FeatureDiscoveryInfo info = MakeDiscoveryInfo(desc);

    std::uint32_t          count = 0;
    VkExtensionProperties* props = nullptr;
    const NVSDK_NGX_Result r =
        NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&info, &count, &props);
    if (NgxFailed(r) || props == nullptr) {
        // Not fatal, and deliberately not an error-level line: on a
        // machine with no NVIDIA driver this is the expected answer and
        // the engine is about to run perfectly well without DLSS.
        LOG_INFO("DLSS: instance-extension query returned {} -- DLSS will "
                 "report itself unavailable; the engine starts normally",
                 ResultText(r));
        return out;
    }
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        out.emplace_back(props[i].extensionName);
    }
#endif
    return out;
}

std::vector<std::string> VulkanNgxUpscaler::RequiredDeviceExtensions(
    VkInstance instance, VkPhysicalDevice phys) {
    std::vector<std::string> out;
#if defined(PT_ENABLE_DLSS)
    if (instance == VK_NULL_HANDLE || phys == VK_NULL_HANDLE) return out;

    NVSDK_NGX_ProjectIdDescription desc{};
    const NVSDK_NGX_FeatureDiscoveryInfo info = MakeDiscoveryInfo(desc);

    std::uint32_t          count = 0;
    VkExtensionProperties* props = nullptr;
    const NVSDK_NGX_Result r =
        NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
            instance, phys, &info, &count, &props);
    if (NgxFailed(r) || props == nullptr) {
        LOG_INFO("DLSS: device-extension query returned {} -- DLSS will "
                 "report itself unavailable; the engine starts normally",
                 ResultText(r));
        return out;
    }
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        out.emplace_back(props[i].extensionName);
    }
#else
    (void)instance;
    (void)phys;
#endif
    return out;
}

// ---------------------------------------------------------------------
// Runtime bring-up
// ---------------------------------------------------------------------

bool VulkanNgxUpscaler::Init() {
    if (init_attempted_) return Available();
    init_attempted_ = true;

#if !defined(PT_ENABLE_DLSS)
    // Build without the SDK. Not a failure worth a warning: the whole
    // point of PT_DLSS_ACTIVE is that consumers never branch on it.
    failed_ = true;
    return false;
#else
    if (device_ == nullptr || device_->RawDevice() == VK_NULL_HANDLE) {
        failed_ = true;
        return false;
    }

    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        kProjectId,
        NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        kEngineVersion,
        kAppDataPath,
        device_->RawInstance(),
        device_->RawPhysicalDevice(),
        device_->RawDevice());
    if (NgxFailed(r)) {
        // The overwhelmingly common cause is nvngx_dlss.dll not sitting
        // beside demont.exe -- the CMake post-build copy in
        // src/app/CMakeLists.txt is what puts it there -- so name that
        // explicitly rather than making the reader guess.
        LOG_WARN("DLSS: NVSDK_NGX_VULKAN_Init failed: {}. Check that "
                 "nvngx_dlss.dll sits beside demont.exe and that the "
                 "driver is recent enough. r_dlss falls back to off.",
                 ResultText(r));
        failed_ = true;
        return false;
    }
    ngx_inited_ = true;

    // Capability parameters, NOT AllocateParameters: the optimal-settings
    // callback pointer only exists on the capability set. Asking
    // NGX_DLSS_GET_OPTIMAL_SETTINGS with an allocated set returns
    // FAIL_OutOfDate, which reads like a driver problem and is not one.
    r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&caps_params_);
    if (NgxFailed(r) || caps_params_ == nullptr) {
        LOG_WARN("DLSS: GetCapabilityParameters failed: {}. "
                 "r_dlss falls back to off.", ResultText(r));
        failed_ = true;
        return false;
    }

    // Three separate questions, and they fail differently, so report
    // whichever one actually said no rather than a generic
    // "unsupported".
    int needs_driver_update = 0;
    NVSDK_NGX_Parameter_GetI(caps_params_,
        NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver_update);
    if (needs_driver_update != 0) {
        unsigned int major = 0, minor = 0;
        NVSDK_NGX_Parameter_GetUI(caps_params_,
            NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
        NVSDK_NGX_Parameter_GetUI(caps_params_,
            NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
        LOG_WARN("DLSS: the installed driver is too old for this SDK "
                 "(needs at least {}.{}). r_dlss falls back to off.",
                 major, minor);
        failed_ = true;
        return false;
    }

    int available = 0;
    NVSDK_NGX_Parameter_GetI(caps_params_,
        NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    if (available == 0) {
        NVSDK_NGX_Result init_result = NVSDK_NGX_Result_Fail;
        NVSDK_NGX_Parameter_GetI(caps_params_,
            NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult,
            reinterpret_cast<int*>(&init_result));
        LOG_WARN("DLSS: this GPU/driver does not support Super Resolution "
                 "({}). r_dlss falls back to off.", ResultText(init_result));
        failed_ = true;
        return false;
    }

    // Per-feature parameter set for create + evaluate. Separate from the
    // capability set on purpose: the capability set is the runtime's,
    // and writing per-frame eval state into it is not part of its
    // contract.
    r = NVSDK_NGX_VULKAN_AllocateParameters(&feature_params_);
    if (NgxFailed(r) || feature_params_ == nullptr) {
        LOG_WARN("DLSS: AllocateParameters failed: {}. "
                 "r_dlss falls back to off.", ResultText(r));
        failed_ = true;
        return false;
    }

    ready_ = true;
    LOG_INFO("DLSS: NGX initialised on {} -- Super Resolution available",
             device_->DeviceName());
    return true;
#endif
}

// ---------------------------------------------------------------------
// The optimal-settings query -- the whole point of the integration
// ---------------------------------------------------------------------

bool VulkanNgxUpscaler::QueryOptimalSettings(UpscalerMode  mode,
                                             std::uint32_t display_width,
                                             std::uint32_t display_height,
                                             UpscalerSettings& out) {
    out = UpscalerSettings{};
    if (mode == UpscalerMode::Off) return false;
    if (display_width == 0 || display_height == 0) return false;
    if (!Init()) return false;

#if !defined(PT_ENABLE_DLSS)
    return false;
#else
    unsigned int opt_w = 0, opt_h = 0;
    unsigned int max_w = 0, max_h = 0;
    unsigned int min_w = 0, min_h = 0;
    float        sharpness = 0.0f;

    const NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(
        caps_params_,
        display_width, display_height,
        ToNgxQuality(mode),
        &opt_w, &opt_h, &max_w, &max_h, &min_w, &min_h, &sharpness);
    if (NgxFailed(r)) {
        LOG_WARN("DLSS: optimal-settings query failed for mode `{}` at "
                 "{}x{}: {}", ModeName(mode), display_width, display_height,
                 ResultText(r));
        return false;
    }
    // A zero extent is how the runtime signals "this mode is defined in
    // the enum but I do not implement it" -- UltraQuality's historical
    // behaviour. Treat it as unavailable rather than clamping up to 1,
    // which would render a 1x1 frame and look like a different bug
    // entirely.
    if (opt_w == 0 || opt_h == 0) {
        LOG_WARN("DLSS: mode `{}` reported a zero render extent at {}x{} -- "
                 "the installed runtime does not implement it",
                 ModeName(mode), display_width, display_height);
        return false;
    }

    out.render_width  = opt_w;
    out.render_height = opt_h;
    out.min_width     = min_w;
    out.min_height    = min_h;
    out.max_width     = max_w;
    out.max_height    = max_h;
    out.sharpness     = sharpness;
    out.supported     = true;
    return true;
#endif
}

// ---------------------------------------------------------------------
// Feature lifetime
// ---------------------------------------------------------------------

void VulkanNgxUpscaler::DestroyFeature() {
#if defined(PT_ENABLE_DLSS)
    if (feature_ == nullptr) return;
    // The feature owns GPU resources that frames still in flight may be
    // reading. NGX's release is not queue-ordered, so the wait is the
    // only thing standing between a mode switch and a use-after-free.
    // Mode switches and resizes are user-driven and rare; the stall is
    // not on any hot path.
    if (device_ != nullptr && device_->RawDevice() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_->RawDevice());
    }
    NVSDK_NGX_VULKAN_ReleaseFeature(feature_);
    feature_ = nullptr;
    created_mode_      = UpscalerMode::Off;
    created_render_w_  = created_render_h_  = 0;
    created_display_w_ = created_display_h_ = 0;
#endif
}

void VulkanNgxUpscaler::ReleaseFeature() { DestroyFeature(); }

bool VulkanNgxUpscaler::CreateFeature(VkCommandBuffer cb,
                                      UpscalerMode    mode,
                                      std::uint32_t   render_w,
                                      std::uint32_t   render_h,
                                      std::uint32_t   display_w,
                                      std::uint32_t   display_h,
                                      bool            hdr,
                                      bool            auto_exposure) {
#if !defined(PT_ENABLE_DLSS)
    (void)cb; (void)mode; (void)render_w; (void)render_h;
    (void)display_w; (void)display_h; (void)hdr; (void)auto_exposure;
    return false;
#else
    DestroyFeature();

    int flags = NVSDK_NGX_DLSS_Feature_Flags_None;
    // IsHDR: the colour handed to DLSS is linear, unbounded radiance in
    // physical units, not display-referred [0,1]. This engine's default
    // (r_hdr_pipeline 1) is exactly that.
    if (hdr) flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    // AutoExposure: let DLSS meter the input itself. The engine's
    // exposure_state is a POST-tonemap scalar in a storage buffer, not
    // the 1x1 R32F pre-exposure texture NGX wants, and the radiance here
    // spans many decades (night sky to noon desert is ~1e6) -- a stale
    // or mis-scaled hint is worse than none. See the plan's SS2.1
    // caveat E for the staged alternative.
    if (auto_exposure) flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    // MVLowRes: the motion vectors are at the RENDER extent, not the
    // display extent. They are, and getting this wrong scales every
    // reprojection by the render ratio.
    flags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    // NOT set, deliberately:
    //   MVJittered    -- the motion vectors are jitter-free. The jitter
    //                    is a host-side shear of the camera basis, while
    //                    curr/prev_view_proj are built from the
    //                    UNSHEARED basis, so no jitter ever reaches the
    //                    reprojection (plan SS2.1 caveat D).
    //   DepthInverted -- the depth is linear camera-space distance, and
    //                    larger means further. DepthInverted describes
    //                    reversed-Z clip depth, which is a different
    //                    thing; setting it here would invert a
    //                    relationship that is already the right way up.

    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth             = render_w;
    create.Feature.InHeight            = render_h;
    create.Feature.InTargetWidth       = display_w;
    create.Feature.InTargetHeight      = display_h;
    create.Feature.InPerfQualityValue  = ToNgxQuality(mode);
    create.InFeatureCreateFlags        = flags;
    create.InEnableOutputSubrects      = false;

    const NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT(
        cb, /*creation node mask*/ 1u, /*visibility node mask*/ 1u,
        &feature_, feature_params_, &create);
    if (NgxFailed(r) || feature_ == nullptr) {
        LOG_ERROR("DLSS: feature creation failed for mode `{}` "
                  "({}x{} -> {}x{}): {}. r_dlss falls back to off.",
                  ModeName(mode), render_w, render_h, display_w, display_h,
                  ResultText(r));
        feature_ = nullptr;
        failed_  = true;
        return false;
    }

    created_mode_      = mode;
    created_render_w_  = render_w;
    created_render_h_  = render_h;
    created_display_w_ = display_w;
    created_display_h_ = display_h;
    created_hdr_       = hdr;
    created_auto_exp_  = auto_exposure;

    LOG_INFO("DLSS: feature created -- mode `{}`, render {}x{} -> display "
             "{}x{} (ratio {:.4f}), flags: HDR={} auto_exposure={} "
             "MVLowRes=1 MVJittered=0 DepthInverted=0",
             ModeName(mode), render_w, render_h, display_w, display_h,
             (display_w > 0) ? float(render_w) / float(display_w) : 0.0f,
             hdr ? 1 : 0, auto_exposure ? 1 : 0);
    return true;
#endif
}

// ---------------------------------------------------------------------
// Per-frame evaluate
// ---------------------------------------------------------------------

bool VulkanNgxUpscaler::Evaluate(const UpscaleDesc& d) {
#if !defined(PT_ENABLE_DLSS)
    (void)d;
    return false;
#else
    if (!Available())          return false;
    if (d.mode == UpscalerMode::Off) return false;
    if (device_ == nullptr)    return false;

    // Ray Reconstruction is a different NGX feature with a different
    // eval-params struct and a different relationship to the denoiser
    // chain; it is owned by a separate workstream. Honour the seam's
    // contract: ignore the flag rather than half-implementing it, and
    // say so exactly once so nobody concludes RR silently works.
    if (d.ray_reconstruction) {
        static bool s_logged_rr = false;
        if (!s_logged_rr) {
            LOG_WARN("DLSS: r_dlss_rr is set but Ray Reconstruction is not "
                     "implemented in this backend yet -- running Super "
                     "Resolution only. The denoiser chain is still doing "
                     "the denoising.");
            s_logged_rr = true;
        }
    }

    if (d.color_in.id == 0 || d.output.id == 0 ||
        d.depth_in.id == 0 || d.motion_in.id == 0) {
        LOG_WARN("DLSS: missing required inputs (color={} depth={} motion={} "
                 "out={}) -- skipping the upscale this frame",
                 d.color_in.id, d.depth_in.id, d.motion_in.id, d.output.id);
        return false;
    }
    if (d.render_width == 0 || d.render_height == 0 ||
        d.display_width == 0 || d.display_height == 0) {
        return false;
    }

    VkCommandBuffer cb = device_->CurrentRawCommandBuffer();
    if (cb == VK_NULL_HANDLE) return false;

    // THE INVARIANT. Any difference between what the feature was created
    // with and what it is about to be fed forces a re-create. NGX would
    // not necessarily error on a mismatch -- it might quietly produce a
    // wrong image -- so this is checked here rather than trusted to the
    // runtime.
    const bool needs_create =
        (feature_ == nullptr)                     ||
        (created_mode_      != d.mode)            ||
        (created_render_w_  != d.render_width)    ||
        (created_render_h_  != d.render_height)   ||
        (created_display_w_ != d.display_width)   ||
        (created_display_h_ != d.display_height)  ||
        (created_hdr_       != d.hdr)             ||
        (created_auto_exp_  != d.auto_exposure);
    if (needs_create) {
        if (!CreateFeature(cb, d.mode, d.render_width, d.render_height,
                           d.display_width, d.display_height,
                           d.hdr, d.auto_exposure)) {
            return false;
        }
    }

    // The engine's compute passes wrote colour / depth / motion; NGX is
    // about to read them from its own dispatches. The engine's
    // CommandBuffer::Barrier only covers passes it records itself, so
    // the hazard across the NGX boundary is ours to close. Global
    // memory barrier rather than per-image: NGX runs an opaque sequence
    // of dispatches over all three inputs, so there is nothing to be
    // gained from being precise here and a real risk in guessing which
    // accesses it uses.
    {
        VkMemoryBarrier mb{};
        mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cb,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
    }

    NVSDK_NGX_Resource_VK color  = MakeResource(device_, d.color_in,  false);
    NVSDK_NGX_Resource_VK depth  = MakeResource(device_, d.depth_in,  false);
    NVSDK_NGX_Resource_VK motion = MakeResource(device_, d.motion_in, false);
    NVSDK_NGX_Resource_VK output = MakeResource(device_, d.output,    true);

    NVSDK_NGX_VK_DLSS_Eval_Params eval{};
    eval.Feature.pInColor  = &color;
    eval.Feature.pInOutput = &output;
    // Sharpness is deprecated in this SDK
    // (NVSDK_NGX_DLSS_Feature_Flags_DoSharpening is marked
    // SR_DEPRECATED_SHARPENING); leaving it zero is the documented
    // "don't sharpen" value, not an oversight.
    eval.Feature.InSharpness = 0.0f;
    eval.pInDepth            = &depth;
    eval.pInMotionVectors    = &motion;
    // Render-resolution pixels, matching the motion vectors' direction
    // convention. The engine's jitter is a host-side camera shear
    // applied to EVERY ray the frame traces -- colour samples and the
    // depth/motion G-buffer pass alike -- so the number reported here is
    // the offset the frame was genuinely sampled at, which is the one
    // thing DLSS cannot recover from being lied to about.
    eval.InJitterOffsetX = d.jitter_x;
    eval.InJitterOffsetY = d.jitter_y;
    eval.InRenderSubrectDimensions.Width  = d.render_width;
    eval.InRenderSubrectDimensions.Height = d.render_height;
    eval.InReset    = d.reset_history ? 1 : 0;
    // Pixel-space motion needs no rescale. The helper substitutes 1.0
    // for a zero anyway, but state it rather than relying on that.
    eval.InMVScaleX = d.mv_scale_x;
    eval.InMVScaleY = d.mv_scale_y;
    eval.InPreExposure        = d.auto_exposure ? 0.0f : d.pre_exposure;
    eval.InFrameTimeDeltaInMsec = d.frame_time_delta_ms;

    const NVSDK_NGX_Result r =
        NGX_VULKAN_EVALUATE_DLSS_EXT(cb, feature_, feature_params_, &eval);
    if (NgxFailed(r)) {
        // Not latched: an evaluate can fail transiently (a resize race
        // that lands one frame with a stale view). Log once per session
        // so a persistent failure is visible without a per-frame spam
        // that would itself become the problem.
        static bool s_logged_eval_fail = false;
        if (!s_logged_eval_fail) {
            LOG_ERROR("DLSS: evaluate failed: {}. The frame's upscale did "
                      "not run.", ResultText(r));
            s_logged_eval_fail = true;
        }
        return false;
    }

    // NGX wrote `output` through its own dispatches; the engine's next
    // pass (the tonemap finalize) reads it.
    {
        VkMemoryBarrier mb{};
        mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    return true;
#endif
}

}  // namespace pt::rhi::vk
