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
// Ray Reconstruction. Separate headers, separate eval-params struct,
// separate NGX feature ID (NVSDK_NGX_Feature_RayReconstruction = 13) and
// a separate runtime DLL (nvngx_dlssd.dll, which cmake/Dependencies.cmake
// already copies beside demont.exe alongside nvngx_dlss.dll).
#include <nvsdk_ngx_defs_dlssd.h>
// The API-AGNOSTIC dlssd helper as well as the Vulkan one: the optimal-
// settings entry point (NGX_DLSSD_GET_OPTIMAL_SETTINGS) lives only in
// the former, because the query is a parameter-set callback with no
// graphics-API surface. It forward-declares the D3D types it names, so
// including it in a Vulkan TU pulls in no Direct3D headers -- and the
// SDK's own guide (SS5.3) says to include it "wherever they have included
// nvsdk_ngx_defs.h".
#include <nvsdk_ngx_helpers_dlssd.h>
#include <nvsdk_ngx_helpers_dlssd_vk.h>
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

// Shared by the extension queries and the RR capability probe: build the
// discovery info NGX wants for one feature.
//
// `feature` is a parameter rather than a hardcoded SuperSampling because
// Ray Reconstruction is a DIFFERENT NGX feature and is entitled to
// different Vulkan extension requirements. Asking only about
// SuperSampling and then creating an RR feature is the failure mode this
// file's header warns about: NGX initialises fine and the create then
// fails with an opaque result, or the driver faults.
NVSDK_NGX_FeatureDiscoveryInfo MakeDiscoveryInfo(
    NVSDK_NGX_ProjectIdDescription& desc_storage,
    NVSDK_NGX_Feature               feature) {
    desc_storage.ProjectId     = kProjectId;
    desc_storage.EngineType    = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    desc_storage.EngineVersion = kEngineVersion;

    NVSDK_NGX_FeatureDiscoveryInfo info{};
    info.SDKVersion                = NVSDK_NGX_Version_API;
    info.FeatureID                 = feature;
    info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    info.Identifier.v.ProjectDesc  = desc_storage;
    info.ApplicationDataPath       = kAppDataPath;
    info.FeatureInfo               = nullptr;
    return info;
}

// The two features whose Vulkan requirements must BOTH be satisfied at
// device-creation time. Enabling RR's extensions costs nothing on a
// session that never turns RR on -- an enabled-but-unused device
// extension is free -- whereas NOT enabling them cannot be repaired
// later, because the device is already created by the time anyone knows
// whether r_dlss_rr will be set.
constexpr NVSDK_NGX_Feature kNeededFeatures[] = {
    NVSDK_NGX_Feature_SuperSampling,
    NVSDK_NGX_Feature_RayReconstruction,
};

const char* FeatureName(NVSDK_NGX_Feature f) {
    return (f == NVSDK_NGX_Feature_RayReconstruction) ? "Ray Reconstruction"
                                                      : "Super Resolution";
}

// Append `name` to `out` unless it is already there. The two feature
// queries overlap heavily (both want the same core Vulkan extensions),
// and handing vkCreateDevice a list with duplicates is a validation
// error, not a harmless redundancy.
void AppendUnique(std::vector<std::string>& out, const char* name) {
    if (name == nullptr || name[0] == '\0') return;
    for (const auto& s : out) {
        if (s == name) return;
    }
    out.emplace_back(name);
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
    if (rr_params_ != nullptr) {
        NVSDK_NGX_VULKAN_DestroyParameters(rr_params_);
        rr_params_ = nullptr;
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
    for (const NVSDK_NGX_Feature feature : kNeededFeatures) {
        NVSDK_NGX_ProjectIdDescription desc{};
        const NVSDK_NGX_FeatureDiscoveryInfo info = MakeDiscoveryInfo(desc, feature);

        std::uint32_t          count = 0;
        VkExtensionProperties* props = nullptr;
        const NVSDK_NGX_Result r =
            NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&info, &count, &props);
        if (NgxFailed(r) || props == nullptr) {
            // Not fatal, and deliberately not an error-level line: on a
            // machine with no NVIDIA driver this is the expected answer and
            // the engine is about to run perfectly well without DLSS.
            // Per-feature rather than fatal-for-both: a runtime that knows
            // Super Resolution but not Ray Reconstruction answers exactly
            // this way for RR, and that machine should still get SR.
            LOG_INFO("DLSS: instance-extension query for {} returned {} -- "
                     "that feature will report itself unavailable; the engine "
                     "starts normally",
                     FeatureName(feature), ResultText(r));
            continue;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            AppendUnique(out, props[i].extensionName);
        }
    }
#endif
    return out;
}

std::vector<std::string> VulkanNgxUpscaler::RequiredDeviceExtensions(
    VkInstance instance, VkPhysicalDevice phys) {
    std::vector<std::string> out;
#if defined(PT_ENABLE_DLSS)
    if (instance == VK_NULL_HANDLE || phys == VK_NULL_HANDLE) return out;

    for (const NVSDK_NGX_Feature feature : kNeededFeatures) {
        NVSDK_NGX_ProjectIdDescription desc{};
        const NVSDK_NGX_FeatureDiscoveryInfo info = MakeDiscoveryInfo(desc, feature);

        std::uint32_t          count = 0;
        VkExtensionProperties* props = nullptr;
        const NVSDK_NGX_Result r =
            NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
                instance, phys, &info, &count, &props);
        if (NgxFailed(r) || props == nullptr) {
            LOG_INFO("DLSS: device-extension query for {} returned {} -- that "
                     "feature will report itself unavailable; the engine "
                     "starts normally",
                     FeatureName(feature), ResultText(r));
            continue;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            AppendUnique(out, props[i].extensionName);
        }
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
// Ray Reconstruction capability
// ---------------------------------------------------------------------
//
// TWO independent questions, because they fail for different reasons and
// a user who hits one should not be told the other:
//
//   1. Does the hardware + driver support the RR feature at all?
//      NVSDK_NGX_VULKAN_GetFeatureRequirements answers that, and returns
//      a bitfield saying WHICH of driver / adapter / OS said no.
//   2. Is the RR runtime actually loadable? RR ships in its own DLL
//      (nvngx_dlssd.dll, distinct from SR's nvngx_dlss.dll), and a build
//      missing it -- or carrying one older than this SDK -- has an NGX
//      capability set with no DLSSD optimal-settings callback in it.
//      That absent callback is exactly what the SDK's own
//      NGX_DLSSD_GET_OPTIMAL_SETTINGS tests before it does anything, so
//      testing it here is not a heuristic; it is the documented probe.
//
// Latched: both causes are deterministic for the life of the process.
bool VulkanNgxUpscaler::RayReconstructionAvailable() {
#if !defined(PT_ENABLE_DLSS)
    return false;
#else
    if (rr_probed_) return rr_available_;
    // Super Resolution has to be up first: RR shares the NGX runtime,
    // the capability parameter set and the feature parameter set with
    // it. Init() logs its own failure once.
    if (!Init()) {
        rr_probed_ = true;
        return false;
    }
    rr_probed_ = true;

    if (device_ != nullptr &&
        device_->RawInstance() != VK_NULL_HANDLE &&
        device_->RawPhysicalDevice() != VK_NULL_HANDLE) {
        NVSDK_NGX_ProjectIdDescription desc{};
        const NVSDK_NGX_FeatureDiscoveryInfo info =
            MakeDiscoveryInfo(desc, NVSDK_NGX_Feature_RayReconstruction);
        NVSDK_NGX_FeatureRequirement req{};
        const NVSDK_NGX_Result rq = NVSDK_NGX_VULKAN_GetFeatureRequirements(
            device_->RawInstance(), device_->RawPhysicalDevice(), &info, &req);
        if (NgxFailed(rq)) {
            LOG_WARN("DLSS: Ray Reconstruction requirement query failed: {}. "
                     "r_dlss_rr falls back to off; Super Resolution is "
                     "unaffected.", ResultText(rq));
            return false;
        }
        if (req.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported) {
            const unsigned bits = static_cast<unsigned>(req.FeatureSupported);
            LOG_WARN("DLSS: this GPU/driver does not support Ray "
                     "Reconstruction (reason bits 0x{:x}:{}{}{}{}{}). "
                     "r_dlss_rr falls back to off; Super Resolution is "
                     "unaffected.",
                     bits,
                     (bits & NVSDK_NGX_FeatureSupportResult_CheckNotPresent)
                         ? " check-not-present" : "",
                     (bits & NVSDK_NGX_FeatureSupportResult_DriverVersionUnsupported)
                         ? " driver-too-old" : "",
                     (bits & NVSDK_NGX_FeatureSupportResult_AdapterUnsupported)
                         ? " gpu-unsupported" : "",
                     (bits & NVSDK_NGX_FeatureSupportResult_OSVersionBelowMinimumSupported)
                         ? " os-too-old" : "",
                     (bits & NVSDK_NGX_FeatureSupportResult_NotImplemented)
                         ? " not-implemented" : "");
            return false;
        }
    }

    // The DLL half. GetVoidPointer leaves `cb` untouched when the key is
    // absent, so it is initialised to null rather than trusted to be.
    void* cb = nullptr;
    NVSDK_NGX_Parameter_GetVoidPointer(
        caps_params_, NVSDK_NGX_Parameter_DLSSDOptimalSettingsCallback, &cb);
    if (cb == nullptr) {
        LOG_WARN("DLSS: the NGX runtime exposes no Ray Reconstruction "
                 "entry point -- nvngx_dlssd.dll is missing from beside "
                 "demont.exe, or is older than this SDK. r_dlss_rr falls "
                 "back to off; Super Resolution is unaffected.");
        return false;
    }

    rr_available_ = true;
    LOG_INFO("DLSS: Ray Reconstruction available on {}", device_->DeviceName());
    return true;
#endif
}

// ---------------------------------------------------------------------
// The optimal-settings query -- the whole point of the integration
// ---------------------------------------------------------------------

bool VulkanNgxUpscaler::QueryOptimalSettings(UpscalerMode  mode,
                                             std::uint32_t display_width,
                                             std::uint32_t display_height,
                                             bool          ray_reconstruction,
                                             UpscalerSettings& out) {
    out = UpscalerSettings{};
    if (mode == UpscalerMode::Off) return false;
    if (display_width == 0 || display_height == 0) return false;
    if (!Init()) return false;

#if !defined(PT_ENABLE_DLSS)
    (void)ray_reconstruction;
    return false;
#else
    // Asking the RR runtime for an extent it will never be asked to
    // render is how the create-vs-feed mismatch this file exists to
    // prevent would come back. If RR is not available the caller must
    // not be handed an RR-shaped answer at all.
    if (ray_reconstruction && !RayReconstructionAvailable()) return false;

    unsigned int opt_w = 0, opt_h = 0;
    unsigned int max_w = 0, max_h = 0;
    unsigned int min_w = 0, min_h = 0;
    float        sharpness = 0.0f;

    // Two entry points, one ladder. DLSS-RR supports exactly the same
    // PerfQualityValues as SR (DLSS-RR Integration Guide SS3.2) and
    // returns the same extents today -- but it publishes its own
    // callback, and following it is what keeps this integration correct
    // on the day they diverge.
    const NVSDK_NGX_Result r =
        ray_reconstruction
            ? NGX_DLSSD_GET_OPTIMAL_SETTINGS(
                  caps_params_,
                  display_width, display_height,
                  ToNgxQuality(mode),
                  &opt_w, &opt_h, &max_w, &max_h, &min_w, &min_h, &sharpness)
            : NGX_DLSS_GET_OPTIMAL_SETTINGS(
                  caps_params_,
                  display_width, display_height,
                  ToNgxQuality(mode),
                  &opt_w, &opt_h, &max_w, &max_h, &min_w, &min_h, &sharpness);
    if (NgxFailed(r)) {
        LOG_WARN("DLSS: {} optimal-settings query failed for mode `{}` at "
                 "{}x{}: {}",
                 ray_reconstruction ? "Ray Reconstruction" : "Super Resolution",
                 ModeName(mode), display_width, display_height,
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
    created_rr_        = false;
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
                                      bool            auto_exposure,
                                      bool            ray_reconstruction) {
#if !defined(PT_ENABLE_DLSS)
    (void)cb; (void)mode; (void)render_w; (void)render_h;
    (void)display_w; (void)display_h; (void)hdr; (void)auto_exposure;
    (void)ray_reconstruction;
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

    NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
    if (ray_reconstruction) {
        // Lazily allocate RR's own parameter set -- see the member's
        // comment for why it must not share Super Resolution's.
        if (rr_params_ == nullptr) {
            const NVSDK_NGX_Result pr =
                NVSDK_NGX_VULKAN_AllocateParameters(&rr_params_);
            if (NgxFailed(pr) || rr_params_ == nullptr) {
                LOG_ERROR("DLSS: AllocateParameters for Ray Reconstruction "
                          "failed: {}. r_dlss_rr falls back to off; Super "
                          "Resolution is unaffected.", ResultText(pr));
                rr_params_    = nullptr;
                rr_probed_    = true;
                rr_available_ = false;
                return false;
            }
        }

        NVSDK_NGX_DLSSD_Create_Params create{};
        // DLUnified is the only denoise mode the SDK implements
        // (nvsdk_ngx_defs_dlssd.h, and DLSS-RR Integration Guide SS5.3);
        // Denoise_Mode_Off would be asking the denoising feature not to
        // denoise, which is what `r_dlss_rr 0` already expresses by
        // creating the SR feature instead.
        create.InDenoiseMode   = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
        // UNPACKED: the path tracer writes normal_tex (RGBA16F) and
        // roughness_tex (R32F) as SEPARATE textures. Packed would mean
        // roughness in normals.w, which is NRD's encoding, not this
        // engine's -- plan SS5.3 keeps the shader writing the unpacked
        // pair precisely so RR and NRD stop fighting over one texture.
        create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Unpacked;
        // LINEAR, and this is the field that makes RR fit this renderer
        // without a conversion pass. PathTrace.slang's depth_tex holds
        // linear camera-space depth (`h0.t * dot(rd0, fwd)`), not clip
        // depth, and RR accepts exactly that when told (DLSS-RR
        // Integration Guide SS3.4.7: "Either view-space depth or HW depth
        // can be provided"). Super Resolution has no equivalent knob,
        // which is why its create path has nothing like this line.
        create.InUseHWDepth    = NVSDK_NGX_DLSS_Depth_Type_Linear;
        create.InWidth               = render_w;
        create.InHeight              = render_h;
        create.InTargetWidth         = display_w;
        create.InTargetHeight        = display_h;
        create.InPerfQualityValue    = ToNgxQuality(mode);
        create.InFeatureCreateFlags  = flags;
        create.InEnableOutputSubrects = false;

        r = NGX_VULKAN_CREATE_DLSSD_EXT1(
            device_->RawDevice(), cb,
            /*creation node mask*/ 1u, /*visibility node mask*/ 1u,
            &feature_, rr_params_, &create);
    } else {
        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature.InWidth             = render_w;
        create.Feature.InHeight            = render_h;
        create.Feature.InTargetWidth       = display_w;
        create.Feature.InTargetHeight      = display_h;
        create.Feature.InPerfQualityValue  = ToNgxQuality(mode);
        create.InFeatureCreateFlags        = flags;
        create.InEnableOutputSubrects      = false;

        r = NGX_VULKAN_CREATE_DLSS_EXT(
            cb, /*creation node mask*/ 1u, /*visibility node mask*/ 1u,
            &feature_, feature_params_, &create);
    }
    if (NgxFailed(r) || feature_ == nullptr) {
        // An RR create failure must NOT latch failed_: that flag turns
        // Available() off forever, and Super Resolution -- a different
        // feature that may be perfectly healthy -- is the thing the
        // caller degrades to. Latch the RR-specific flag instead so the
        // next frame comes back as SR rather than as nothing.
        LOG_ERROR("DLSS: {} feature creation failed for mode `{}` "
                  "({}x{} -> {}x{}): {}. Falling back to {}.",
                  ray_reconstruction ? "Ray Reconstruction" : "Super Resolution",
                  ModeName(mode), render_w, render_h, display_w, display_h,
                  ResultText(r),
                  ray_reconstruction ? "Super Resolution with the engine's "
                                       "own denoiser"
                                     : "r_dlss off");
        feature_ = nullptr;
        if (ray_reconstruction) {
            rr_probed_    = true;
            rr_available_ = false;
        } else {
            failed_ = true;
        }
        return false;
    }

    created_mode_      = mode;
    created_render_w_  = render_w;
    created_render_h_  = render_h;
    created_display_w_ = display_w;
    created_display_h_ = display_h;
    created_hdr_       = hdr;
    created_auto_exp_  = auto_exposure;
    created_rr_        = ray_reconstruction;

    LOG_INFO("DLSS: {} feature created -- mode `{}`, render {}x{} -> display "
             "{}x{} (ratio {:.4f}), flags: HDR={} auto_exposure={} "
             "MVLowRes=1 MVJittered=0 DepthInverted=0{}",
             ray_reconstruction ? "Ray Reconstruction" : "Super Resolution",
             ModeName(mode), render_w, render_h, display_w, display_h,
             (display_w > 0) ? float(render_w) / float(display_w) : 0.0f,
             hdr ? 1 : 0, auto_exposure ? 1 : 0,
             ray_reconstruction ? ", denoise=DLUnified roughness=unpacked "
                                  "depth=linear"
                                : "");
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

    // RR must NOT silently degrade to SR here. By the time this runs the
    // engine has already stood its denoiser chain down (that is what
    // selecting the RR denoiser kind means), so quietly running SR would
    // present a raw 1-spp path-traced frame -- noise, not a missing
    // feature. Refuse instead, and let the caller's documented
    // "returns false -> do not present output" contract handle it.
    if (d.ray_reconstruction && !RayReconstructionAvailable()) {
        static bool s_logged_rr_unavail = false;
        if (!s_logged_rr_unavail) {
            LOG_ERROR("DLSS: Ray Reconstruction was requested for this frame "
                      "but is unavailable -- refusing the evaluate rather "
                      "than running Super Resolution over an undenoised "
                      "frame. See the preceding `DLSS:` line for the reason.");
            s_logged_rr_unavail = true;
        }
        return false;
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
    // RR's four MANDATORY guides (DLSS-RR Integration Guide SS3.4.1-3.4.4).
    // Refuse rather than pass nulls: RR with a missing guide is not a
    // degraded RR, it is an undefined one, and the caller's denoiser is
    // already off. specular_hit_distance is NOT in this list -- it is
    // genuinely optional (SS3.4.9, "only needed if Specular Motion Vectors
    // are not provided").
    if (d.ray_reconstruction &&
        (d.albedo_in.id == 0 || d.specular_albedo_in.id == 0 ||
         d.normal_in.id == 0 || d.roughness_in.id == 0)) {
        static bool s_logged_rr_guides = false;
        if (!s_logged_rr_guides) {
            LOG_ERROR("DLSS: Ray Reconstruction is missing a mandatory guide "
                      "buffer (albedo={} specular_albedo={} normal={} "
                      "roughness={}) -- refusing the evaluate. The engine "
                      "allocates these off r_dlss_rr; a zero here means the "
                      "allocation gate and the evaluate disagree.",
                      d.albedo_in.id, d.specular_albedo_in.id,
                      d.normal_in.id, d.roughness_in.id);
            s_logged_rr_guides = true;
        }
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
        (created_auto_exp_  != d.auto_exposure)   ||
        // Toggling r_dlss_rr swaps WHICH NGX feature is live, so it is
        // as much a re-create as a resize is.
        (created_rr_        != d.ray_reconstruction);
    if (needs_create) {
        if (!CreateFeature(cb, d.mode, d.render_width, d.render_height,
                           d.display_width, d.display_height,
                           d.hdr, d.auto_exposure, d.ray_reconstruction)) {
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

    const bool ok = d.ray_reconstruction ? EvaluateRayReconstruction(cb, d)
                                         : EvaluateSuperResolution(cb, d);
    if (!ok) return false;

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

// ---------------------------------------------------------------------
// Super Resolution / DLAA evaluate
// ---------------------------------------------------------------------

bool VulkanNgxUpscaler::EvaluateSuperResolution(VkCommandBuffer cb,
                                                const UpscaleDesc& d) {
#if !defined(PT_ENABLE_DLSS)
    (void)cb; (void)d;
    return false;
#else
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
            LOG_ERROR("DLSS: Super Resolution evaluate failed: {}. The "
                      "frame's upscale did not run.", ResultText(r));
            s_logged_eval_fail = true;
        }
        return false;
    }
    return true;
#endif
}

// ---------------------------------------------------------------------
// Ray Reconstruction evaluate
// ---------------------------------------------------------------------
//
// The guide buffers, and what each one's convention was checked against.
// Every field below was verified against `DLSS-RR Integration Guide`
// (SWE-DLSS-001-PGRF, shipped in the SDK at doc/), not inferred from
// NRD or Streamline documentation -- the two disagree in at least one
// place that matters (see the hit-distance note).
bool VulkanNgxUpscaler::EvaluateRayReconstruction(VkCommandBuffer cb,
                                                  const UpscaleDesc& d) {
#if !defined(PT_ENABLE_DLSS)
    (void)cb; (void)d;
    return false;
#else
    NVSDK_NGX_Resource_VK color   = MakeResource(device_, d.color_in,  false);
    NVSDK_NGX_Resource_VK depth   = MakeResource(device_, d.depth_in,  false);
    NVSDK_NGX_Resource_VK motion  = MakeResource(device_, d.motion_in, false);
    NVSDK_NGX_Resource_VK output  = MakeResource(device_, d.output,    true);
    NVSDK_NGX_Resource_VK albedo  = MakeResource(device_, d.albedo_in, false);
    NVSDK_NGX_Resource_VK spec_a  = MakeResource(device_, d.specular_albedo_in, false);
    NVSDK_NGX_Resource_VK normal  = MakeResource(device_, d.normal_in, false);
    NVSDK_NGX_Resource_VK rough   = MakeResource(device_, d.roughness_in, false);
    NVSDK_NGX_Resource_VK spec_hd{};
    const bool have_spec_hd = (d.specular_hit_distance_in.id != 0) &&
                              (d.world_to_view != nullptr) &&
                              (d.view_to_clip  != nullptr);
    if (have_spec_hd) {
        spec_hd = MakeResource(device_, d.specular_hit_distance_in, false);
    }

    NVSDK_NGX_VK_DLSSD_Eval_Params eval{};
    // ---- The four mandatory guides (SS3.4.1 - 3.4.4) -------------------
    // Diffuse albedo: the diffuse component of reflectance. NOT an sRGB
    // format -- the engine's denoise_albedo is RGBA16F linear, which is
    // what SS3.4.1 asks for ("at this time sRGB formats are not
    // supported").
    eval.pInDiffuseAlbedo  = &albedo;
    // Specular albedo: "the average specular reflectivity given a view
    // direction" (SS3.4.2), i.e. the split-sum integrated reflectance the
    // producer computes with envBrdfSplitSum -- NOT raw F0. The guide's
    // appendix gives NVIDIA's own EnvBRDFApprox2 for this; the Karis /
    // Lazarov fit already in PathTrace.slang is the same quantity.
    eval.pInSpecularAlbedo = &spec_a;
    // Normals: "Shading Normals (Normalized). Can be View Space or World
    // Space" (SS3.4.3). The engine writes world space, which that sentence
    // explicitly permits, so no transform is needed here.
    eval.pInNormals        = &normal;
    // Roughness: LINEAR roughness, single channel (SS3.4.4). The feature
    // was created with Roughness_Mode_Unpacked, so this is read from its
    // own texture rather than from normals.w.
    eval.pInRoughness      = &rough;

    // ---- The same core inputs Super Resolution takes -------------------
    eval.pInColor          = &color;
    eval.pInOutput         = &output;
    // Depth is LINEAR camera-space, and the feature was created with
    // Depth_Type_Linear to say so. This is the one place RR is easier to
    // satisfy than SR: SR has no such knob.
    eval.pInDepth          = &depth;
    eval.pInMotionVectors  = &motion;
    eval.InJitterOffsetX   = d.jitter_x;
    eval.InJitterOffsetY   = d.jitter_y;
    eval.InRenderSubrectDimensions.Width  = d.render_width;
    eval.InRenderSubrectDimensions.Height = d.render_height;
    eval.InReset    = d.reset_history ? 1 : 0;
    eval.InMVScaleX = d.mv_scale_x;
    eval.InMVScaleY = d.mv_scale_y;
    eval.InPreExposure          = d.auto_exposure ? 0.0f : d.pre_exposure;
    eval.InFrameTimeDeltaInMsec = d.frame_time_delta_ms;

    // ---- Specular hit distance, and the convention check ---------------
    //
    // THE FIELD THIS INTEGRATION WAS MOST LIKELY TO GET WRONG, because
    // the producer was written against NRD/Streamline docs before the SDK
    // was fetched. The verdict, from SS3.4.9 verbatim: "This is the World
    // Space distance between the Specular Ray Origin and Hit Point.
    // Specular Ray Origin must be on the Primary Surface."
    //
    // That is exactly what PathTrace.slang writes -- a mirror-direction
    // traceScene launched FROM the primary hit, storing hr.t, the
    // distance from that hit. The assumed convention was right and the
    // producer needed no change.
    //
    // Two things the struct's own comment would have misled us about, and
    // why the PDF rather than the header is cited above:
    //   * the header files these fields under "OPTIONAL - only for
    //     research purposes", but the guide documents them as a
    //     first-class input with a debug-overlay layer of their own (SS8.1
    //     item 6). The header's block comment is stale, not the API.
    //   * the matrices are documented as "Row Major Order and left
    //     multiplication". That is the SAME byte layout as glm's
    //     column-major right-multiplied matrices (each is the other's
    //     transpose, and swapping storage order transposes), so they are
    //     passed straight through. Transposing here would be the bug.
    //
    // Supplied INSTEAD of specular motion vectors, which the guide makes
    // alternatives: "This is only needed if Specular Motion Vectors are
    // not provided." This engine has no specular motion field.
    //
    // The const_cast is safe and unavoidable: NGX's struct takes float*
    // and the helper only reads through it (it hands the pointer to
    // NVSDK_NGX_Parameter_SetVoidPointer).
    if (have_spec_hd) {
        eval.pInSpecularHitDistance = &spec_hd;
        eval.pInWorldToViewMatrix   = const_cast<float*>(d.world_to_view);
        eval.pInViewToClipMatrix    = const_cast<float*>(d.view_to_clip);
    }

    // rr_params_, not feature_params_: RR's own set, guaranteed non-null
    // here because CreateFeature allocated it before the feature this
    // evaluate is running against could exist.
    const NVSDK_NGX_Result r =
        NGX_VULKAN_EVALUATE_DLSSD_EXT(cb, feature_, rr_params_, &eval);
    if (NgxFailed(r)) {
        static bool s_logged_rr_eval_fail = false;
        if (!s_logged_rr_eval_fail) {
            LOG_ERROR("DLSS: Ray Reconstruction evaluate failed: {}. The "
                      "frame was neither denoised nor upscaled.",
                      ResultText(r));
            s_logged_rr_eval_fail = true;
        }
        return false;
    }

    // One line, once, recording that RR actually ran and with which
    // optional guides. This is the evidence a bug report needs to
    // distinguish "RR is on" from "r_dlss_rr is set".
    static bool s_logged_rr_running = false;
    if (!s_logged_rr_running) {
        LOG_INFO("DLSS: Ray Reconstruction evaluating -- render {}x{} -> "
                 "display {}x{}, guides: diffuse_albedo + specular_albedo + "
                 "normal(world) + roughness(linear, unpacked){}. RR is the "
                 "denoiser for this frame; the engine's chain is standing "
                 "down.",
                 d.render_width, d.render_height,
                 d.display_width, d.display_height,
                 have_spec_hd ? " + specular_hit_distance(world units, from "
                                "the primary hit) with world_to_view / "
                                "view_to_clip"
                              : " (no specular hit distance -- reflections "
                                "reproject less well in motion)");
        s_logged_rr_running = true;
    }
    return true;
#endif
}

}  // namespace pt::rhi::vk
