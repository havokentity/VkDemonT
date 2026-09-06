// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
//
// Step 1 probe (docs/STEP1_RT_PIPELINE_DESIGN.md section 1): the ONE miss
// and ONE closest-hit entry the probe's ray-tracing pipeline links next to
// the PathTraceRaygen.slang raygen. Both are stubs on purpose: in the
// design's 1a shape the raygen decodes the hit itself with the compute
// kernel's verbatim code, so the only job of these two stages is to hand
// the raygen the same values RayQuery's Committed*() accessors returned.
// The real closest-hit (terrain / mesh decode) and miss (sky) modules are
// 1b work and do not exist yet.
//
// Compiled twice by src/rhi_vulkan/CMakeLists.txt, once per entry:
//   -entry ptMiss       -stage miss
//   -entry ptClosestHit -stage closesthit
// Neither reads a descriptor, so the shared pipeline layout's bindings are
// untouched by these stages (they still carry the RT stage flags, section 8).
#include "PathTraceRtPayload.slang"

[shader("miss")]
void ptMiss(inout PtRtPayload p) {
    p.t = kPtRtMissT;
}

[shader("closesthit")]
void ptClosestHit(inout PtRtPayload p, in BuiltInTriangleIntersectionAttributes attr) {
    p.t    = RayTCurrent();
    p.prim = PrimitiveIndex();
    p.inst = InstanceID();
    p.bary = attr.barycentrics;
    p.o2w  = ObjectToWorld4x3();
}
