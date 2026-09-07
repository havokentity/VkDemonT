# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
add_library(pt_rhi_vulkan STATIC
    VulkanDevice.cpp
    VulkanDenoiser.cpp
    # DLSS Super Resolution / DLAA on NGX. Compiled UNCONDITIONALLY,
    # unlike the NRD and OptiX wrappers below: VulkanDevice holds a
    # unique_ptr<VulkanNgxUpscaler> on every build and calls it from four
    # non-#if'd overrides, so the TU has to exist even when the SDK
    # doesn't. Without PT_ENABLE_DLSS every NGX call body compiles out
    # and the class reports Available() == false forever -- which is
    # exactly the "graceful degradation" contract r_dlss documents.
    VulkanNgxUpscaler.cpp
)

target_include_directories(pt_rhi_vulkan PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(pt_rhi_vulkan
    PUBLIC pt_core
    PUBLIC pt_app_window           # GLFW + (later) Cocoa bridge
    PUBLIC Vulkan::Vulkan
    PUBLIC glfw                    # for glfwCreateWindowSurface
)

# ---- NRD denoiser (NVIDIA RayTracingDenoiser, Vulkan, non-Apple) ----------
# Issue #50. Gated by PT_NRD_ACTIVE (set by the top-level Dependencies.cmake
# once the user has flipped PT_ENABLE_NRD=ON on a non-Apple build with the
# Vulkan backend on). When inactive the wrapper file isn't compiled, the
# NRD lib isn't linked, and PT_ENABLE_NRD is undefined in the source so
# the #if PT_ENABLE_NRD blocks inside VulkanNrdLibDenoiser.{h,cpp} compile
# out -- VulkanDenoiser.cpp is built either way (it is the in-house SVGF
# chain behind svgf_basic / svgf_atrous, it is the fallback `r_denoiser
# nrd` degrades to, and even on the NRD path its DenoiseFinalize stage
# does the bloom + tonemap + swapchain write; it has no PT_ENABLE_NRD
# gates).
# With PT_NRD_ACTIVE the `r_denoiser nrd` cvar value routes to
# VulkanNrdLibDenoiser (a real nrd::Instance running RELAX_DIFFUSE);
# without it the engine's Device::SupportsNrdLibrary() reports false and
# the cvar degrades to the in-house SVGF a-trous chain with a log line.
#
# The two Slang kernels that bridge the engine's G-buffer set to NRD's
# IN_*/OUT_* textures (NrdPack / NrdUnpack) are compiled further down,
# in a second PT_NRD_ACTIVE block -- pt_compile_slang isn't defined
# until cmake/Slang.cmake is included, which happens below.
if(PT_NRD_ACTIVE)
    target_sources(pt_rhi_vulkan PRIVATE VulkanNrdLibDenoiser.cpp)
    target_link_libraries(pt_rhi_vulkan PRIVATE NRD)
    target_compile_definitions(pt_rhi_vulkan PUBLIC PT_ENABLE_NRD=1)
endif()

# ---- DLSS Super Resolution / DLAA (NVIDIA NGX, Vulkan) -------------------
# Gated by PT_DLSS_ACTIVE, which the top-level Dependencies.cmake sets
# once the SDK has been fetched, the import library exists, and the
# Vulkan backend is on. VulkanNgxUpscaler.cpp is in the source list
# unconditionally (see the comment there); this block is only about the
# include path, the import library, and the PT_ENABLE_DLSS define that
# turns the NGX call bodies on.
#
# PUBLIC on the define rather than PRIVATE, matching PT_ENABLE_NRD: the
# engine reads it too, to decide whether "r_dlss requested on a build
# without DLSS" is the fallback reason it should name in the log.
#
# nvsdk_ngx_d.lib is a static import library for the DYNAMIC CRT, so it
# pairs with this project's clang-cl /MD build. It resolves the
# NVSDK_NGX_* symbols against _nvngx.dll, which the NVIDIA DRIVER
# provides -- the nvngx_dlss.dll copied beside demont.exe below is the
# feature snippet the driver then loads, not this library's own DLL.
if(PT_DLSS_ACTIVE)
    target_link_libraries(pt_rhi_vulkan PRIVATE dlss_ngx)
    target_compile_definitions(pt_rhi_vulkan PUBLIC PT_ENABLE_DLSS=1)
endif()

# ---- OptiX denoiser (NVIDIA only) -----------------------------------------
# Adds the OptiX denoiser strategy alongside the SVGF/NRD one. Gated by
# PT_OPTIX_ACTIVE (set by the top-level CMakeLists once CUDA Toolkit and
# OptiX SDK headers are both detected). When inactive the OptiX file isn't
# compiled, the CUDA libs aren't linked, and PT_ENABLE_OPTIX is undefined
# so the #if blocks in the source compile out.
if(PT_OPTIX_ACTIVE)
    target_sources(pt_rhi_vulkan PRIVATE VulkanOptixDenoiser.cpp)
    target_include_directories(pt_rhi_vulkan PRIVATE ${OPTIX_INCLUDE_DIR})
    target_link_libraries(pt_rhi_vulkan
        PRIVATE CUDA::cudart_static
        PRIVATE CUDA::cuda_driver
    )
    target_compile_definitions(pt_rhi_vulkan PUBLIC PT_ENABLE_OPTIX=1)
endif()

# Compile every compute kernel to SPIR-V so when the Vulkan backend
# does start instantiating Tonemap / BloomDown / BloomUp pipelines,
# the SPIR-V is already available alongside PathTrace + AutoExposure.
#
# Current state: only `pathtrace` and `autoexpose` are actually
# registered in VulkanDevice's pipeline map (they share the 16-binding
# unified descriptor layout + the engine's PtPush split). The other
# three SPIR-V blobs are compiled but no VkPipeline object is created
# for them yet -- they'd need their own descriptor layout + 576B
# push-constant split to land cleanly. Engine looks pipelines up by
# name and gets id=0 when a kernel isn't registered; the engine treats
# id=0 dispatches as silent no-ops, and PathTrace.slang carries an
# inline ACES tonemap so the Vulkan image is still tonemapped without
# the post-pass tonemap pipeline. (Mac builds use these shaders today
# via the Metal backend.)
include(${CMAKE_SOURCE_DIR}/cmake/Slang.cmake)
# Pre-compile Slang IR modules used by PathTrace.  Each module compiles
# in its own ninja job (parallel) and the resulting .slang-module is
# linked into PathTrace.spv via the entry-point compile's `import` +
# `-I <out_dir>` resolver.  Body-only iterations on PathTrace.slang
# skip the module re-parse, saving ~145ms / rebuild.
pt_compile_slang_module(SOURCE ${CMAKE_SOURCE_DIR}/shaders/PathTraceMath.slang)
# PathTraceCloud imports PathTraceMath since #257 (the spherical
# cloud-layer gate needs ptAltitudeAboveSphere), so the math module
# has to be built first and reachable on the include path.
pt_compile_slang_module(SOURCE ${CMAKE_SOURCE_DIR}/shaders/PathTraceCloud.slang
                        MODULE_DEPS PathTraceMath)
# --- SDF Phase 1 (#97) -----------------------------------------------------
# Analytic SDF library; same compile-module pattern.
pt_compile_slang_module(SOURCE ${CMAKE_SOURCE_DIR}/shaders/SdfPrimitives.slang)
# --- end SDF Phase 1 -------------------------------------------------------
# --- SDF Phase 3 (#99) -----------------------------------------------------
# Iconic-fractal DE library (Mandelbulb / Mandelbox / Apollonian).
pt_compile_slang_module(SOURCE ${CMAKE_SOURCE_DIR}/shaders/SdfFractals.slang)
# --- end SDF Phase 3 -------------------------------------------------------
# --- ReSTIR DI Phase A (issue #78) ----------------------------------------
# Shared Reservoir struct + WRS streaming primitives imported by
# PathTrace.slang's candidate-generation block AND by all three Restir
# compute kernels below.
pt_compile_slang_module(SOURCE ${CMAKE_SOURCE_DIR}/shaders/RestirReservoir.slang)
# --- end ReSTIR DI Phase A ------------------------------------------------

# --- Megakernel feature gates: slangc defines -----------------------------
# Same -DPT_*_ENABLED translation as the Metal CMakeLists (both targets
# must define the macros so the same Slang source compiles cleanly to
# both). Each _pt_*_def variable accumulates into EXTRA_DEFINES on every
# pt_compile_slang() invocation that builds PathTrace.slang variants.
if(PT_WATER_ENABLED)
    set(_pt_water_def "-DPT_WATER_ENABLED=1")
else()
    set(_pt_water_def "-DPT_WATER_ENABLED=0")
endif()
if(PT_LIGHT_TREE)
    set(_pt_light_tree_def "-DPT_LIGHT_TREE_ENABLED=1")
else()
    set(_pt_light_tree_def "-DPT_LIGHT_TREE_ENABLED=0")
endif()
if(PT_PLANET_ENABLED)
    set(_pt_planet_def "-DPT_PLANET_ENABLED=1")
else()
    set(_pt_planet_def "-DPT_PLANET_ENABLED=0")
endif()
# --- end Megakernel feature gates -----------------------------------------
pt_compile_slang(
    TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/PathTrace.slang
    STAGE compute ENTRY main TARGETS spirv
    EXTRA_DEFINES ${_pt_water_def} ${_pt_light_tree_def} ${_pt_planet_def}
    MODULE_DEPS PathTraceMath PathTraceCloud SdfPrimitives SdfFractals RestirReservoir)
# Second PathTrace SPIR-V variant without RayQuery, for Vulkan drivers that
# don't expose VK_KHR_ray_query.
# The two variants share the source file; PT_SPIRV_NO_RAYQUERY strips
# the scene_tlas binding + the HW RayQuery branch, so the SPIR-V module
# never declares the RayQueryKHR capability and loads cleanly on those
# drivers. VulkanDevice picks one at pipeline build time based on
# rt_supported_; see the PathTrace pipeline build site for the gate.
pt_compile_slang(
    TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/PathTrace.slang
    STAGE compute ENTRY main TARGETS spirv
    VARIANT norq EXTRA_DEFINES -DPT_SPIRV_NO_RAYQUERY ${_pt_water_def} ${_pt_light_tree_def} ${_pt_planet_def}
    MODULE_DEPS PathTraceMath PathTraceCloud SdfPrimitives SdfFractals RestirReservoir)
pt_compile_slang(
    TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/Tonemap.slang
    STAGE compute ENTRY main TARGETS spirv)
pt_compile_slang(
    TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/BloomDown.slang
    STAGE compute ENTRY main TARGETS spirv)
pt_compile_slang(
    TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/BloomUp.slang
    STAGE compute ENTRY main TARGETS spirv)
pt_compile_slang(
    TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/AutoExposure.slang
    STAGE compute ENTRY main TARGETS spirv)
# Render-scale resolve (r_render_scale). Bilinear magnify from the
# internal-extent LDR target to the swapchain; the placeholder that DLSS
# replaces. Declares only bindings 0 and 1, both storage images already
# in the shared descriptor-set layout, so no layout entry is added.
pt_compile_slang(
    TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/Upscale.slang
    STAGE compute ENTRY main TARGETS spirv)
pt_compile_slang(
    TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/PerfOverlay.slang
    STAGE compute ENTRY main TARGETS spirv)
# Editor 3D-transform gizmo overlay. Compute kernel that rasterizes
# world-space line segments into the swapchain. See
# shaders/EditorOverlay.slang for the binding contract.
pt_compile_slang(
    TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/EditorOverlay.slang
    STAGE compute ENTRY main TARGETS spirv)
# Stateless celestial composite (issue #46). Reads the post-denoise HDR
# target and additively writes sun / moon / BSC + procedural stars with
# aperture-sampled bokeh. On Vulkan this rides the shared 14-slot
# descriptor set layout via kSlotToTexBinding[] (slot 0 -> binding 0,
# slot 1 -> binding 1, slot 2 -> binding 6, slot 3 -> binding 7,
# slot 4 -> binding 8 -- see shader header for the rationale on each
# reuse). 224B push splits at kPushSplitOffset = 112 into hw push
# constants + a Frame UBO tail, mirroring PathTrace's split.
pt_compile_slang(
    TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/StarsComposite.slang
    STAGE compute ENTRY main TARGETS spirv)
# Wave 7 (#24) -- procedural raymarched cloud pre-pass + composite.
# Imports PathTraceCloud (the shared cloud-density module) so the
# raymarch and the inline cloud march sample the SAME procedural field.
# Both kernels compile to SPIR-V on Vulkan; pipeline registration is in
# VulkanDevice.cpp's pipeline-build worker. When r_clouds_mode ==
# pathtraced (default) the engine elides both dispatches.
pt_compile_slang(
    TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/CloudsRaymarch.slang
    STAGE compute ENTRY main TARGETS spirv
    MODULE_DEPS PathTraceCloud PathTraceMath)
pt_compile_slang(
    TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/CloudsComposite.slang
    STAGE compute ENTRY main TARGETS spirv)
# Planetary P5 (#259) / #133 Phase 2 -- the ocean cascade FFT pre-pass.
# COMPILED HERE BUT NOT DISPATCHED. The SPIR-V is built (and validated by
# spirv-val) so the kernel cannot rot into something Vulkan could never
# consume, but VulkanDevice builds no pipeline from it: the shared
# descriptor-set layout stops at binding 39 and this kernel declares
# 40..45, and a shader declaring a binding the layout lacks is a hard
# vkCreateComputePipelines failure even under PARTIALLY_BOUND. Adding the
# six entries is the whole of what a Vulkan port needs, and it is
# deliberately not done blind: no Vulkan device has executed any of this
# planetary arc (#290), and the ocean storage images are already
# non-square (grid x 3*grid, plus a metadata row) which VulkanDevice has
# never seen. Until then the Vulkan path takes the CPU cascade solver,
# which is what it has always run.
pt_compile_slang(
    TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/OceanCascades.slang
    STAGE compute ENTRY main TARGETS spirv)
# SVGF/NRD denoiser kernels. Each is its own SPIR-V blob; the Vulkan
# backend's VulkanNrdDenoiser builds a dedicated VkPipelineLayout +
# 2 VkPipeline handles from these and dispatches them in sequence.
# Metal builds skip these -- the Mac path uses MetalFX instead.
pt_compile_slang(
    TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/DenoiseTemporal.slang
    STAGE compute ENTRY main TARGETS spirv)
pt_compile_slang(
    TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/DenoiseAtrous.slang
    STAGE compute ENTRY main TARGETS spirv)
# Issue #119 -- SVGF albedo remodulation pass.
pt_compile_slang(
    TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/DenoiseRemod.slang
    STAGE compute ENTRY main TARGETS spirv)
pt_compile_slang(
    TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/DenoiseFinalize.slang
    STAGE compute ENTRY main TARGETS spirv)
# NRD front-end / back-end bridge kernels (issue #50). Compiled ONLY
# under PT_NRD_ACTIVE: nothing but VulkanNrdLibDenoiser.cpp references
# their embedded SPIR-V, so building them into the default (NRD-off)
# configuration would be dead weight in every binary.
if(PT_NRD_ACTIVE)
    pt_compile_slang(
        TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/NrdPack.slang
        STAGE compute ENTRY main TARGETS spirv)
    pt_compile_slang(
        TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/NrdUnpack.slang
        STAGE compute ENTRY main TARGETS spirv)
endif()
# --- ReSTIR DI, Vulkan dispatch (issue #23) --------------------------------
# The three resampling kernels behind PathTrace's WRS candidate
# generation: temporal reuse -> spatial reuse -> shadow test + Lambert
# composite. These were authored for Slang's Metal backend and never
# built for SPIR-V, which is the whole reason `r_restir 1` logged
# "ReSTIR NOT dispatching" -- VulkanDevice had no pipeline to resolve.
#
# All three ride the shared descriptor-set layout; their vk::binding
# numbers were rewritten from Metal declaration-order slots to real
# shared-layout positions (see the VULKAN BINDING NOTICE at the top of
# RestirTemporal.slang). The port also added storage-buffer bindings 49
# and 50 to that layout for the reservoir ping-pong, with the matching
# descriptor-pool bump in VulkanDevice.cpp.
#
# Every push block is <= 112 B (VulkanDevice::kPushSplitOffset), so none
# of them spills into the Frame UBO and none needs a `cbuffer Frame`.
# Engine.cpp static_asserts that for RestirTemporalPush, which sits at
# exactly 112.
pt_compile_slang(
    TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/RestirTemporal.slang
    STAGE compute ENTRY main TARGETS spirv
    MODULE_DEPS RestirReservoir)
pt_compile_slang(
    TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/RestirSpatial.slang
    STAGE compute ENTRY main TARGETS spirv
    MODULE_DEPS RestirReservoir)
# RestirFinal traces the survivor's shadow ray, so it gets the same
# rq / norq pair PathTrace does: the default blob declares RayQueryKHR
# + scene_tlas at binding 2, the norq blob declares neither and loads on
# drivers without VK_KHR_ray_query. VulkanDevice picks one at pipeline
# build time off rt_supported_, exactly as it does for pathtrace.
pt_compile_slang(
    TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/RestirFinal.slang
    STAGE compute ENTRY main TARGETS spirv
    MODULE_DEPS RestirReservoir PathTraceMath)
pt_compile_slang(
    TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/RestirFinal.slang
    STAGE compute ENTRY main TARGETS spirv
    VARIANT norq EXTRA_DEFINES -DPT_SPIRV_NO_RAYQUERY
    MODULE_DEPS RestirReservoir PathTraceMath)
# --- end ReSTIR DI, Vulkan dispatch ----------------------------------------
# --- Acceleration-structure probe (issue #254 P0 / issue #251) -------------
# Test-only ray-query kernel used by tests/rhi_accel_update_test.cpp to
# read CommittedInstanceID() back out of a multi-instance TLAS. Declares
# only bindings 2 (TLAS) and 3 (storage buffer), both already in the
# shared descriptor-set layout, so no layout entry is added. The pipeline
# is registered at runtime only when the driver reports ray query.
pt_compile_slang(
    TARGET pt_rhi_vulkan SOURCE ${CMAKE_SOURCE_DIR}/shaders/AccelProbe.slang
    STAGE compute ENTRY main TARGETS spirv)
# --- end acceleration-structure probe --------------------------------------

pt_set_compiler_options(pt_rhi_vulkan)
