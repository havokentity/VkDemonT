// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

// The temporal-upscaler seam (docs/DLSS_INTEGRATION_PLAN.md SS1.5).
//
// WHY THIS FILE EXISTS AS A SEAM RATHER THAN AS DLSS CALLS IN THE ENGINE
// ---------------------------------------------------------------------
// The plan chose direct NGX over Streamline (SS1.1-1.4) and paid for that
// choice with one promise: everything Streamline-specific (or FSR-,
// XeSS-, or Frame-Generation-specific) later lands as a SECOND
// implementation of this interface, replacing exactly one file
// (src/rhi_vulkan/VulkanNgxUpscaler.cpp) and nothing else. The engine
// side -- the cvars, the G-buffer allocation, the render/display extent
// split, the pass ordering -- is identical either way, so none of that
// work is wasted if the decision is revisited.
//
// The struct style deliberately mirrors Device::DenoiseDesc
// (src/rhi/Device.h): a flat plain-old-data description of one
// invocation, with per-field comments carrying the FORMAT and the
// CONVENTION rather than just the name, because every historical bug in
// this area has been a convention mismatch (motion-vector sign, jitter
// sign, linear-vs-clip depth) rather than a plumbing mistake.
//
// WHAT LIVES HERE AND WHAT DOES NOT
// ---------------------------------
// Here:     the vocabulary types (mode, queried settings, one evaluate
//           description) and the abstract interface.
// Not here: anything that names NGX, Vulkan, or a preset ratio. The
//           ratios in particular are documentation and NOT constants --
//           see UpscalerSettings.

#include "Handles.h"

#include <cstdint>

namespace pt::rhi {

// The user-facing mode ladder, matching `r_dlss`'s allowed_values
// one-for-one. Ordinals here are OURS -- they are deliberately NOT the
// NVSDK_NGX_PerfQuality_Value ordinals (which are, in declaration order,
// MaxPerf / Balanced / MaxQuality / UltraPerformance / UltraQuality /
// DLAA -- i.e. neither sorted by ratio nor matching any sane UI order).
// The backend translates; nothing above the RHI should know NVIDIA's
// numbering.
//
// UltraQuality is absent on purpose. It exists in NVIDIA's enum but has
// historically not been implemented by the runtime, and offering a mode
// that resolves to "unavailable" is worse than not offering it
// (docs/DLSS_INTEGRATION_PLAN.md SS3).
enum class UpscalerMode : std::uint8_t {
    Off = 0,
    Dlaa,               // ratio 1.0 -- anti-aliasing only, costs time
    Quality,
    Balanced,
    Performance,
    UltraPerformance,
};

// Answer to "what render extent does this mode want for this display
// extent?" -- the result of NGX_DLSS_GET_OPTIMAL_SETTINGS, or of
// whatever the equivalent query is on a future backend.
//
// THE RENDER EXTENT IS QUERIED, NEVER COMPUTED. The published per-axis
// ratios (2/3, 0.58, 1/2, 1/3) are documentation of what this query
// currently returns; they are not constants to multiply by. The feature
// is created for the size the query returned, and a mismatch between the
// size the upscaler was created for and the size it is fed is an ERROR,
// not a rounding nuisance. Anything that recomputes an extent from a
// ratio has reintroduced that bug.
struct UpscalerSettings {
    // The extent the renderer must run at. Zero when `supported` is
    // false -- callers must treat a zero extent as "mode unavailable"
    // and fall back to Off, never as something to clamp up to 1.
    std::uint32_t render_width  = 0;
    std::uint32_t render_height = 0;
    // Dynamic-resolution bounds the runtime reports. Recorded because
    // the query returns them; this engine does NOT use dynamic
    // resolution (the accumulator, the ReSTIR reservoir ring and the
    // golden harness all assume a stable extent, and DLSS Ray
    // Reconstruction does not support DRS at all), so render_* above is
    // fixed per mode.
    std::uint32_t min_width  = 0;
    std::uint32_t min_height = 0;
    std::uint32_t max_width  = 0;
    std::uint32_t max_height = 0;
    // The runtime's recommended sharpness for this mode. Reported for
    // completeness; DLSS's own sharpening is deprecated in the SDK this
    // integrates against (NVSDK_NGX_DLSS_Feature_Flags_DoSharpening is
    // marked SR_DEPRECATED_SHARPENING) so nothing consumes it today.
    float sharpness = 0.0f;
    // False when the query failed, the mode is not implemented by the
    // installed runtime, or the returned extent was degenerate. The
    // caller's contract is to fall back to Off with ONE log line.
    bool supported = false;
};

// One upscale invocation. Everything is at the RENDER extent except
// `output`, which is at the DISPLAY extent -- that asymmetry is the
// whole point of the pass, so the field comments say which is which
// every time.
struct UpscaleDesc {
    // ---- Required inputs (render extent) -------------------------------
    // Linear HDR radiance for THIS FRAME -- not accumulated. Same
    // contract as DenoiseDesc::color_in: a temporal upscaler does its
    // own accumulation and a running mean fed into it is antithetical to
    // that (docs/DLSS_INTEGRATION_PLAN.md SS4.4).
    TextureHandle color_in;
    // R32F LINEAR camera-space depth (`h0.t * dot(rd0, fwd)`), NOT clip
    // space. This is what PathTrace.slang actually writes -- see its
    // depth_tex declaration. The sky sentinel is large (1.0e10); a
    // backend that needs a bounded value must clamp it itself and say so.
    TextureHandle depth_in;
    // RG16F motion, PIXEL space, direction `prev - curr` ("add this to
    // the current position to get the previous one"). Jitter-FREE: the
    // reprojection matrices are built host-side from the unsheared
    // camera basis, so the jitter never enters them.
    TextureHandle motion_in;

    // ---- Required output (DISPLAY extent) ------------------------------
    // Linear HDR at the display extent. Must NOT alias color_in.
    TextureHandle output;

    // ---- Extents -------------------------------------------------------
    // Must equal the extent the feature was created with. The backend
    // re-creates the feature when either pair changes; it must never
    // silently evaluate at a size it was not created for.
    std::uint32_t render_width   = 0;
    std::uint32_t render_height  = 0;
    std::uint32_t display_width  = 0;
    std::uint32_t display_height = 0;

    // ---- The jitter contract (docs/DLSS_INTEGRATION_PLAN.md SS4) -------
    // The frame's sub-pixel offset in RENDER-RESOLUTION PIXELS, range
    // [-0.5, 0.5], the SAME offset for every pixel in the frame, and the
    // sequence over frames must tile the pixel footprint. The upscaler's
    // entire premise is that this number honestly describes where inside
    // the pixel the frame was sampled; reporting an offset the renderer
    // did not actually use is the one input mistake that cannot be
    // recovered from downstream.
    float jitter_x = 0.0f;
    float jitter_y = 0.0f;

    // ---- Optional / tuning ---------------------------------------------
    // Motion-vector scale. {1,1} for the pixel-space motion this engine
    // writes. A backend consuming normalised motion would want
    // {width, height} here instead.
    float mv_scale_x = 1.0f;
    float mv_scale_y = 1.0f;
    // Clear the upscaler's temporal history. Driven from
    // !prev_frame_valid_, exactly as DenoiseDesc::reset_history is.
    bool reset_history = false;
    // color_in / output hold linear HDR rather than display-referred
    // LDR. True on this engine's default path (r_hdr_pipeline 1).
    // A backend must refuse to engage rather than silently produce
    // garbage when it needs HDR and this is false.
    bool hdr = true;
    // Let the upscaler meter the scene's exposure itself. Default true
    // because this engine's exposure_state is a POST-tonemap scalar in a
    // storage buffer rather than the pre-exposure texture NGX expects,
    // and because the radiance here is in physical units spanning many
    // decades (a night sky and a noon desert differ by ~1e6), where a
    // mis-scaled exposure hint is worse than none.
    bool auto_exposure = true;
    // Scalar the colour was already divided by, when the caller
    // pre-exposes. 1.0 = not pre-exposed. Ignored when auto_exposure.
    float pre_exposure = 1.0f;
    // Frame time in milliseconds. Used by the runtime to reason about
    // how far the scene moved between samples. 0 = unknown.
    float frame_time_delta_ms = 0.0f;
    // Which quality mode this evaluate belongs to. The backend compares
    // it against the mode the feature was created with and re-creates on
    // a change rather than evaluating a stale feature.
    UpscalerMode mode = UpscalerMode::Off;

    // ---- Ray Reconstruction seam (a separate workstream owns this) -----
    //
    // DELIBERATELY DECLARED AND DELIBERATELY UNUSED by the Super
    // Resolution / DLAA path. RR is not "SR plus a flag": it REPLACES
    // the denoiser as well as the upscaler, takes the raw noisy radiance
    // rather than a denoised image, and is created through a different
    // NGX feature with a different eval-params struct. The fields are
    // here so the RR workstream extends this seam instead of forking it,
    // and so the composition rule ("RR is a mode of DLSS, not an
    // independent feature") is expressible in one struct.
    //
    // A backend that does not implement RR must IGNORE ray_reconstruction
    // rather than half-honour it: silently running SR when RR was asked
    // for would leave the caller's denoiser turned off and the image
    // noisy, which looks like a quality regression rather than a missing
    // feature.
    bool          ray_reconstruction = false;
    TextureHandle albedo_in;                  // diffuse albedo, .rgb
    TextureHandle specular_albedo_in;         // integrated specular reflectance
    TextureHandle normal_in;                  // world-space shading normal
    TextureHandle roughness_in;               // R32F linear roughness
    TextureHandle specular_hit_distance_in;   // optional; 0 = not supplied
    // Column-major 4x4, 16 floats each. RR wants the two factors
    // separately; the engine's push carries only the combined
    // view*proj, so these are passed alongside rather than derived.
    const float*  world_to_view = nullptr;
    const float*  view_to_clip  = nullptr;
};

// The seam itself. One implementation exists today
// (src/rhi_vulkan/VulkanNgxUpscaler); a Streamline or FSR port is a
// second implementation of exactly this and nothing more.
//
// Lifetime: owned by the backend Device, created lazily on the first
// frame a mode other than Off is requested, destroyed with the device.
class Upscaler {
public:
    virtual ~Upscaler() = default;

    // Human-readable implementation name for log lines ("DLSS (NGX)").
    virtual const char* Name() const = 0;

    // True once the runtime is initialised AND the hardware/driver
    // reports the feature as usable. False forever after any
    // initialisation failure -- failures here are deterministic
    // (missing DLL, unsupported GPU, driver too old), so retrying every
    // frame has no path to succeed and would only spam the log.
    virtual bool Available() const = 0;

    // Ask the runtime what render extent `mode` wants for this display
    // extent. Returns false (and leaves out.supported false) when the
    // mode is unavailable. MUST NOT be answered from a table.
    virtual bool QueryOptimalSettings(UpscalerMode  mode,
                                      std::uint32_t display_width,
                                      std::uint32_t display_height,
                                      UpscalerSettings& out) = 0;

    // Record the upscale into the caller's in-flight command buffer.
    // Creates or re-creates the underlying feature when the mode or
    // either extent has changed since the last call. Returns false if
    // the evaluate did not happen, in which case the caller must NOT
    // present `output` -- nothing wrote it.
    virtual bool Evaluate(const UpscaleDesc& d) = 0;

    // Release the feature but keep the runtime initialised. Called when
    // the mode returns to Off so an unused feature's VRAM (tens to
    // hundreds of MB) goes back.
    virtual void ReleaseFeature() = 0;
};

}  // namespace pt::rhi
