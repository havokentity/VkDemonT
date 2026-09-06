// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
//
// Step 1 go/no-go probe (docs/STEP1_RT_PIPELINE_DESIGN.md sections 1 and 10,
// risk R1): PathTrace.slang's main() compiled as a VK_KHR_ray_tracing_pipeline
// RAYGENERATION stage instead of a compute kernel.
//
// This file deliberately contains no shader code of its own. It defines the
// switches and #includes the megakernel source, so the raygen is the SAME
// text the compute kernel is built from -- every #if PT_RT_PIPELINE block in
// PathTrace.slang is the whole difference between the two builds, and the
// A/B in section 9 of the design compares the two outputs bit for bit. The
// alternative (copying main() into a second file) would have started
// drifting the moment either copy was edited.
//
// What the switches select:
//   PT_RT_PIPELINE   1 -> raygeneration entry point; the bounce-loop trace
//                        goes through the hardware pipeline (TraceRay
//                        against the stub hit group / miss record in
//                        PathTraceRtStubs.slang) while shadow / transmittance
//                        legs stay inline RayQuery, as the design's R3 says.
//   PT_RT_HITOBJECT  0 -> plain TraceRay + payload (the form this probe
//                        ships); 1 -> HitObject::TraceRay + accessor decode,
//                        the design's 1a shape, which Slang 2026.8 cannot
//                        yet emit as valid SPIR-V (see ptTraceHardware in
//                        PathTrace.slang for the two findings).
//   PT_PROBE_VARIANT 0 | 1 | 2 -> the P0 / P1 / P2 escalation of section 1,
//                        supplied by the CMake record (src/rhi_vulkan/
//                        CMakeLists.txt), not defaulted here, so a raygen
//                        build always states which variant it is.
#define PT_RT_PIPELINE 1
#ifndef PT_RT_HITOBJECT
#define PT_RT_HITOBJECT 0
#endif
#ifndef PT_PROBE_VARIANT
#error "PathTraceRaygen.slang needs -DPT_PROBE_VARIANT=<0|1|2> (see src/rhi_vulkan/CMakeLists.txt)"
#endif
#include "PathTrace.slang"
