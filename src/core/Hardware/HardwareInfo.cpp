// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "HardwareInfo.h"
#include "../Log.h"

#include <fmt/format.h>

#include <cstdint>
#include <thread>
#include <cstring>
#include <string>
#include <vector>

namespace pt::hw {

namespace {

Info g_info;
bool g_populated = false;

// Detailed CPU introspection is not yet implemented on Windows; size the
// JobSystem from hardware_concurrency() instead. Leaving cpu_pcores at its
// default 1 would size JobSystem::Init(0) to a SINGLE worker on a 16-core
// box -- every CSG bake and ParallelFor serialized onto one thread.
// hardware_concurrency() counts logical cores (SMT included), which slightly
// over-provisions vs a physical-core count, but a few extra workers beat a
// 1/16th-throughput job system.
void PopulatePlatform(Info& info) {
    const unsigned n = std::thread::hardware_concurrency();
    info.cpu_pcores = (n > 0u) ? static_cast<int>(n) : 1;
    info.cpu_ecores = 0;
    LOG_WARN("HardwareInfo: detailed hardware detection is not implemented "
             "on this platform; using hardware_concurrency() = {} for "
             "worker sizing", n);
}

}  // namespace

void Populate() {
    if (g_populated) return;
    PopulatePlatform(g_info);
    g_populated = true;
}

const Info& GetInfo() { return g_info; }
Info&       MutableInfo() { return g_info; }

}  // namespace pt::hw
