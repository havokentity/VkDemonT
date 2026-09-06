// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
//
// Step 1 probe (docs/STEP1_RT_PIPELINE_DESIGN.md section 1): the ray payload
// shared by the raygen build of PathTrace.slang (via PathTraceRaygen.slang)
// and the stub closest-hit / miss entries in PathTraceRtStubs.slang. One
// definition, #included by both, so the two stages cannot disagree about
// its layout.
//
// The payload mirrors the RayQuery Committed*() reads the compute kernel's
// hit decode consumes, one for one, so the decode after the trace stays the
// verbatim code the design's 1a contract requires. It is 68 bytes: larger
// than the 48 B design payload because it carries the object-to-world
// matrix the mesh branch reads (the design moves that decode into the
// closest-hit stage in 1b; here the closest-hit is a stub that only copies).
#pragma once

struct PtRtPayload {
    float    t;        // committed hit distance; kPtRtMissT on a miss
    uint     prim;     // PrimitiveIndex()
    uint     inst;     // InstanceID(): the instance custom index, like CommittedInstanceID()
    float2   bary;     // BuiltInTriangleIntersectionAttributes.barycentrics
    float4x3 o2w;      // ObjectToWorld4x3()
};

// Sentinel the miss stub writes so the raygen can tell a miss from a hit.
// Above any TMax the engine ever passes (the sky miss uses 1e30 as "infinity"
// in PathTrace.slang, and the bounce loop never traces past it).
static const float kPtRtMissT = 1e30;
