// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
//
// DLSS Super Resolution / DLAA on the NGX Vulkan API -- the one
// implementation of the src/rhi/Upscaler.h seam
// (docs/DLSS_INTEGRATION_PLAN.md SS1.5).
//
// ---------------------------------------------------------------------
// THE THING THIS FILE EXISTS TO GET RIGHT
// ---------------------------------------------------------------------
// The render extent is QUERIED, never computed.
// NGX_DLSS_GET_OPTIMAL_SETTINGS is asked, once per (mode, display
// extent) pair, what render extent the mode wants; the feature is then
// created for exactly that extent and evaluated at exactly that extent.
// The published per-axis ratios (2/3, 0.58, 1/2, 1/3) appear NOWHERE in
// this file. They are documentation of what the query currently returns
// -- a future runtime is free to return something else, and this
// integration follows it automatically. A mismatch between the size the
// feature was created for and the size it is fed is an error, not a
// rounding nuisance, so Evaluate() re-creates rather than tolerating one.
//
// ---------------------------------------------------------------------
// WHAT NGX REQUIRES OF DEVICE CREATION, WHICH IS THE NON-OBVIOUS PART
// ---------------------------------------------------------------------
// NGX needs Vulkan instance AND device extensions that must be enabled
// at vkCreateInstance / vkCreateDevice time -- long before anything in
// this class is constructed. That is why the two static
// Required*Extensions() helpers below exist and why VulkanDevice calls
// them from inside its own creation path. Getting this wrong does not
// produce a clean error: NGX initialisation succeeds and the feature
// creation then fails with an opaque result, or the driver faults.
//
// The plan does not mention this requirement at all (it costs NGX its
// "you can bolt it on later" advantage over Streamline, which owns
// device creation through its own proxy layer). It is recorded here
// because it is the single most likely thing for a future refactor of
// VulkanDevice's creation sequence to silently break.
//
// ---------------------------------------------------------------------
// FAILURE POLICY
// ---------------------------------------------------------------------
// Every failure is LATCHED and reported through Available() == false.
// Initialisation failures here are deterministic -- no DLSS DLL beside
// the executable, a GPU without tensor cores, a driver older than the
// SDK's minimum -- so retrying per frame has no path to succeed and
// would only fill the log. One line, once, naming which check failed,
// then the engine falls back to `r_dlss off`. Same shape as the OptiX
// and NRD unavailability paths.

#pragma once

#include "rhi/Upscaler.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

// The whole class compiles out on a build without the SDK. Consumers
// still see the type (VulkanDevice holds a unique_ptr to it), so the
// declaration stays; only the members that name NGX types are gated.
struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

namespace pt::rhi::vk {

class VulkanDevice;

class VulkanNgxUpscaler final : public pt::rhi::Upscaler {
public:
    explicit VulkanNgxUpscaler(VulkanDevice* dev);
    ~VulkanNgxUpscaler() override;

    VulkanNgxUpscaler(const VulkanNgxUpscaler&)            = delete;
    VulkanNgxUpscaler& operator=(const VulkanNgxUpscaler&) = delete;

    // ---- Upscaler seam --------------------------------------------------
    const char* Name() const override { return "DLSS (NGX)"; }
    bool        Available() const override { return ready_ && !failed_; }
    bool        RayReconstructionAvailable() override;
    bool        QueryOptimalSettings(UpscalerMode mode,
                                     std::uint32_t display_width,
                                     std::uint32_t display_height,
                                     bool          ray_reconstruction,
                                     UpscalerSettings& out) override;
    bool        Evaluate(const UpscaleDesc& d) override;
    void        ReleaseFeature() override;

    // Bring up the NGX runtime and query the SuperSampling capability.
    // Idempotent, and self-guarding against re-entry after a failure:
    // the second call returns the latched answer without touching NGX.
    // Returns Available().
    bool Init();

    // ---- Device-creation requirements (see the header comment) ----------
    //
    // Both return an EMPTY list on a build without the SDK, or when the
    // query fails -- in which case DLSS will report itself unavailable
    // later, which is the correct degradation. Neither is allowed to
    // make device creation fail: an engine that cannot start because an
    // optional upscaler could not enumerate its extensions is worse than
    // one that starts without the upscaler.
    //
    // Instance extensions: must be added to VkInstanceCreateInfo.
    // NGX does not need to be initialised first.
    static std::vector<std::string> RequiredInstanceExtensions();
    // Device extensions: must be added to VkDeviceCreateInfo. Needs the
    // instance and the chosen physical device, so it is called after
    // physical-device selection and before vkCreateDevice.
    static std::vector<std::string> RequiredDeviceExtensions(
        VkInstance instance, VkPhysicalDevice phys);

private:
    // Tear down the NGX feature handle. Waits for the device to go idle
    // first: the feature owns GPU resources the in-flight frames may
    // still be reading, and NGX's release is not queue-ordered.
    void DestroyFeature();

    // (Re)create the DLSS feature for `mode` at the given extents,
    // recording the creation into `cb`. Returns false and latches
    // failed_ on an NGX error.
    //
    // `ray_reconstruction` picks the feature: DLSS-RR
    // (NVSDK_NGX_Feature_RayReconstruction, created through
    // NGX_VULKAN_CREATE_DLSSD_EXT1) rather than DLSS-SR. Only ONE of the
    // two is ever live, because RR is documented to override SR outright
    // ("when DLSS-RR is enabled, it effectively overrides DLSS-SR
    // execution" -- DLSS-RR Integration Guide SS3.2), so holding both
    // would burn a feature's worth of VRAM on one that never runs.
    bool CreateFeature(VkCommandBuffer cb,
                       UpscalerMode    mode,
                       std::uint32_t   render_w,
                       std::uint32_t   render_h,
                       std::uint32_t   display_w,
                       std::uint32_t   display_h,
                       bool            hdr,
                       bool            auto_exposure,
                       bool            ray_reconstruction);

    // The DLSS-SR and DLSS-RR halves of Evaluate(). Split because they
    // fill different eval-params structs through different helpers; the
    // shared work (validation, the create-invariant, the barriers) stays
    // in Evaluate() so it cannot drift between the two.
    bool EvaluateSuperResolution(VkCommandBuffer cb, const UpscaleDesc& d);
    bool EvaluateRayReconstruction(VkCommandBuffer cb, const UpscaleDesc& d);

    VulkanDevice* device_ = nullptr;

    // NGX runtime state. Void-pointer-free: the forward declarations at
    // the top of this header keep the NGX headers out of every TU that
    // includes VulkanDevice.h.
    NVSDK_NGX_Parameter* caps_params_    = nullptr;  // GetCapabilityParameters
    NVSDK_NGX_Parameter* feature_params_ = nullptr;  // AllocateParameters (SR)
    // A SECOND parameter set, for Ray Reconstruction only, allocated
    // lazily on the first RR create.
    //
    // Not an optimisation -- a correctness guard. An NGX parameter set is
    // a persistent string-keyed bag, and nothing clears a key once set.
    // The RR create writes DLSS.Denoise.Mode, DLSS.Roughness.Mode and
    // DLSS.Use.HW.Depth into it; the RR evaluate adds a dozen DLSSD.*
    // resource pointers. Reusing one set would leave all of that behind
    // when the user toggles r_dlss_rr back to 0 and a Super Resolution
    // feature is created from the same bag -- with a stale
    // "DLSS.Denoise.Mode = DLUnified" sitting in it, and stale pointers
    // to stack-local resource descriptors from an earlier frame. Two
    // sets make that impossible rather than merely unlikely.
    NVSDK_NGX_Parameter* rr_params_      = nullptr;  // AllocateParameters (RR)
    NVSDK_NGX_Handle*    feature_        = nullptr;

    // What the live feature was CREATED with. Evaluate() compares the
    // incoming desc against these and re-creates on any difference --
    // this is the guard that makes "created for one size, fed another"
    // impossible rather than merely unlikely.
    UpscalerMode  created_mode_      = UpscalerMode::Off;
    std::uint32_t created_render_w_  = 0;
    std::uint32_t created_render_h_  = 0;
    std::uint32_t created_display_w_ = 0;
    std::uint32_t created_display_h_ = 0;
    bool          created_hdr_       = true;
    bool          created_auto_exp_  = true;
    // SR vs RR. Part of the create-invariant for the same reason the
    // extents are: they are two different features behind one handle,
    // and evaluating one through the other's helper is not an error NGX
    // is obliged to catch.
    bool          created_rr_        = false;

    bool init_attempted_ = false;   // Init() ran (successfully or not)
    bool ngx_inited_     = false;   // NVSDK_NGX_VULKAN_Init succeeded
    bool ready_          = false;   // caps say the feature is usable
    bool failed_         = false;   // latched: never try again this session

    // Ray Reconstruction capability, probed lazily and latched. Separate
    // from ready_/failed_ because RR failing is NOT Super Resolution
    // failing: the SR path must keep working on a machine whose driver
    // or DLL cannot do RR.
    bool rr_probed_    = false;
    bool rr_available_ = false;
};

}  // namespace pt::rhi::vk
