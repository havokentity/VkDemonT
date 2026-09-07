# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
# Third-party dependencies via FetchContent.
# Pin versions; use SYSTEM to suppress warnings from foreign headers.

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# --- glm: vector math (header-only) ----------------------------------------
FetchContent_Declare(glm
    URL           https://github.com/g-truc/glm/archive/refs/tags/1.0.1.tar.gz
    URL_HASH      SHA256=9f3174561fd26904b23f0db5e560971cbf9b3cbda0b280f04d5c379d03bf234c
    SYSTEM
)

# --- fmt: formatted output (until libc++ std::print is everywhere) ---------
FetchContent_Declare(fmt
    URL           https://github.com/fmtlib/fmt/archive/refs/tags/11.2.0.tar.gz
    URL_HASH      SHA256=bc23066d87ab3168f27cef3e97d545fa63314f5c79df5ea444d41d56f962c6af
    SYSTEM
)

# --- mimalloc: persistent heap allocator -----------------------------------
# We want the static lib only; do NOT override system malloc -- we call
# mi_malloc explicitly from PersistentHeap so we keep accounting honest.
set(MI_BUILD_SHARED  OFF CACHE BOOL "" FORCE)
set(MI_BUILD_STATIC  ON  CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT  OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(MI_OVERRIDE      OFF CACHE BOOL "" FORCE)
set(MI_INSTALL_TOPLEVEL OFF CACHE BOOL "" FORCE)
# Under PT_ENABLE_SANITIZERS, route mimalloc's internal heap accounting
# through ASan's __asan_poison_memory_region / __asan_unpoison_memory_region
# so ASan sees the same red zones mimalloc maintains internally. Without
# this, ASan flags every mi_free as a "double-free" because mimalloc's
# segment-cache reuses freed slots before ASan has marked them poisoned,
# and every mi_malloc as "use-after-free" symmetrically. MI_TRACK_ASAN is
# mimalloc 2.1+'s blessed integration for sanitizer builds.
if(PT_ENABLE_SANITIZERS)
    set(MI_TRACK_ASAN ON CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(mimalloc
    URL           https://github.com/microsoft/mimalloc/archive/refs/tags/v2.1.7.tar.gz
    URL_HASH      SHA256=0eed39319f139afde8515010ff59baf24de9e47ea316a315398e8027d198202d
    SYSTEM
)

# --- Tracy: profiler client (optional) -------------------------------------
if(PT_ENABLE_TRACY)
    set(TRACY_ENABLE     ON  CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND  ON  CACHE BOOL "" FORCE)  # only profile when client connects
    set(TRACY_NO_BROADCAST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(tracy
        URL           https://github.com/wolfpld/tracy/archive/refs/tags/v0.11.1.tar.gz
        URL_HASH      SHA256=2c11ca816f2b756be2730f86b0092920419f3dabc7a7173829ffd897d91888a1
        SYSTEM
    )
endif()

# --- glfw: window + input --------------------------------------------------
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    URL           https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
    URL_HASH      SHA256=c038d34200234d071fae9345bc455e4a8f2f544ab60150765d7704e08f3dac01
    SYSTEM
)

# --- enkiTS: job system ----------------------------------------------------
# v1.11's TaskScheduler.cpp uses std::is_pod which C++23 removed.  Patch
# during fetch.  Also: enkiTS uses old-style include_directories(), so the
# include path doesn't propagate via link; we re-add it after MakeAvailable.
set(ENKITS_BUILD_C_INTERFACE OFF CACHE BOOL "" FORCE)
set(ENKITS_BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)
set(ENKITS_BUILD_SHARED      OFF CACHE BOOL "" FORCE)
set(ENKITS_INSTALL           OFF CACHE BOOL "" FORCE)
FetchContent_Declare(enkits
    URL           https://github.com/dougbinks/enkiTS/archive/refs/tags/v1.11.tar.gz
    URL_HASH      SHA256=b57a782a6a68146169d29d180d3553bfecb9f1a0e87a5159082331920e7d297e
    SYSTEM
    PATCH_COMMAND  ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/cmake/patch_enkits.cmake
)

# --- civetweb: HTTP + WebSocket server -------------------------------------
set(CIVETWEB_ENABLE_WEBSOCKETS ON  CACHE BOOL "" FORCE)
set(CIVETWEB_ENABLE_SSL        OFF CACHE BOOL "" FORCE)
set(CIVETWEB_ENABLE_CXX        OFF CACHE BOOL "" FORCE)
set(CIVETWEB_BUILD_TESTING     OFF CACHE BOOL "" FORCE)
set(CIVETWEB_ENABLE_LUA        OFF CACHE BOOL "" FORCE)
set(CIVETWEB_ENABLE_DUKTAPE    OFF CACHE BOOL "" FORCE)
set(CIVETWEB_ENABLE_SERVER_EXECUTABLE OFF CACHE BOOL "" FORCE)
set(CIVETWEB_INSTALL_EXECUTABLE OFF CACHE BOOL "" FORCE)
set(CIVETWEB_ENABLE_ASAN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(civetweb
    URL           https://github.com/civetweb/civetweb/archive/refs/tags/v1.16.tar.gz
    URL_HASH      SHA256=f0e471c1bf4e7804a6cfb41ea9d13e7d623b2bcc7bc1e2a4dd54951a24d60285
    SYSTEM
)

# --- tomlplusplus: TOML parsing (config files later phases) ----------------
FetchContent_Declare(tomlplusplus
    URL           https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
    URL_HASH      SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155
    SYSTEM
)

# --- VulkanMemoryAllocator: simplifies Vk allocation lifetime --------------
# Header-only.  Used by the Vulkan backend (P4+).
FetchContent_Declare(vma
    URL           https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.1.0.tar.gz
    URL_HASH      SHA256=ae134ecc37c55634f108e926f85d5d887b670360e77cd107affaf3a9539595f2
    SYSTEM
)

# --- nlohmann_json: WS protocol payloads -----------------------------------
# JSON_NOEXCEPTION turns throws into abort() so the library plays nice with
# our -fno-exceptions build.
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    URL           https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
    URL_HASH      SHA256=0d8ef5af7f9794e3263480193c491549b2ba6cc74bb018906202ada498a79406
    SYSTEM
)

# --- embree: REMOVED ---------------------------------------------------------
# Embree existed solely to give the software RHI a CPU BVH + ray-triangle
# intersector. That backend is gone (the engine is Windows/Vulkan/NVIDIA-only),
# and nothing else ever linked it -- the remaining references across the tree
# were all comments. Dropping it removes the slowest dependency in the build:
# the from-source FetchContent path took ~10-15 minutes, which is why
# cmake/EmbreeBinary.cmake and .github/workflows/prebuild-embree.yml existed to
# cache a prebuilt artefact. All three are deleted together.

# --- manifold: mesh CSG (P9 headline) --------------------------------------
# Robust manifold-mesh boolean ops (union/intersect/subtract). Builds with
# CMake via FetchContent. We disable everything except the core C++ lib --
# no python bindings, no tests, no fuzzing, no native parallelism (TBB)
# since the job system will run booleans on a worker thread anyway.
set(MANIFOLD_TEST       OFF CACHE BOOL "" FORCE)
set(MANIFOLD_PYBIND     OFF CACHE BOOL "" FORCE)
set(MANIFOLD_CBIND      OFF CACHE BOOL "" FORCE)
set(MANIFOLD_JSBIND     OFF CACHE BOOL "" FORCE)
set(MANIFOLD_DEBUG      OFF CACHE BOOL "" FORCE)
set(MANIFOLD_EXPORT     OFF CACHE BOOL "" FORCE)
set(MANIFOLD_DOWNLOADS  OFF CACHE BOOL "" FORCE)
set(MANIFOLD_PAR            OFF CACHE BOOL "" FORCE)
set(MANIFOLD_CROSS_SECTION  OFF CACHE BOOL "" FORCE)
FetchContent_Declare(manifold
    URL           https://github.com/elalish/manifold/archive/refs/tags/v3.1.1.tar.gz
    URL_HASH      SHA256=1e47f69a96fe228a953e6bfce99657b6d278ed98a822e950322f722adf2e74ed
    SYSTEM
)

# --- NRD: NVIDIA RayTracingDenoiser (Vulkan-only, opt-in) ------------------
# Issue #50 -- stage 1 (scaffolding). Activated only when:
#   - PT_ENABLE_NRD = ON               (user opt-in; default OFF)
#   - PT_ENABLE_VULKAN_BACKEND = ON    (NRD's only supported backend in this
#                                        repo; the DX12 path isn't wired in)
# We set PT_NRD_ACTIVE based on the AND of those two; the Vulkan backend's
# CMakeLists checks PT_NRD_ACTIVE (not PT_ENABLE_NRD directly) so a stale
# PT_ENABLE_NRD=ON in the cache on Mac doesn't try to link a missing target.
#
# Vendoring strategy: FetchContent (consistent with every other dep in this
# file). v4.17.3 is the latest release as of 2026-05 -- pinned by tag + SHA256
# of the GitHub archive tarball. NRD itself ships its CMake which recursively
# fetches MathLib + ShaderMake (their hashes are inside NRD's CMakeLists);
# we don't have to mirror those here.
#
# Defaults overridden for our build:
#   NRD_STATIC_LIBRARY=ON   -- we link statically; one .a in libdemont rather
#                              than a runtime .so dependency.
#   NRD_EMBEDS_SPIRV_SHADERS=ON  -- needed for the Vulkan path (default ON).
#   NRD_EMBEDS_DXIL_SHADERS=OFF  -- we don't compile a DX12 backend.
#   NRD_EMBEDS_DXBC_SHADERS=OFF  -- ditto.
#   NRD_NRI=OFF             -- NRD's bundled higher-level abstraction layer
#                              (Render Interface). We integrate against the
#                              raw NRD instance API instead; NRI would mean
#                              shipping a second graphics-API shim alongside
#                              the engine's own VulkanDevice.
set(PT_NRD_ACTIVE OFF)
if(PT_ENABLE_NRD AND NOT PT_ENABLE_VULKAN_BACKEND)
    message(STATUS "PT_ENABLE_NRD requires PT_ENABLE_VULKAN_BACKEND; NRD denoiser inactive.")
endif()
if(PT_ENABLE_NRD AND PT_ENABLE_VULKAN_BACKEND)
    set(NRD_STATIC_LIBRARY        ON  CACHE BOOL "" FORCE)
    set(NRD_EMBEDS_SPIRV_SHADERS  ON  CACHE BOOL "" FORCE)
    set(NRD_EMBEDS_DXIL_SHADERS   OFF CACHE BOOL "" FORCE)
    set(NRD_EMBEDS_DXBC_SHADERS   OFF CACHE BOOL "" FORCE)
    set(NRD_NRI                   OFF CACHE BOOL "" FORCE)
    # Normal+roughness encoding -- 2 is NRD's default (R10G10B10A2_UNORM oct
    # packed). We match the default so a future swap to NRD-encoded normals
    # in the path tracer doesn't have to re-derive the constants.
    set(NRD_NORMAL_ENCODING       "2" CACHE STRING "" FORCE)
    set(NRD_ROUGHNESS_ENCODING    "1" CACHE STRING "" FORCE)
    FetchContent_Declare(nrd
        URL       https://github.com/NVIDIA-RTX/NRD/archive/refs/tags/v4.17.3.tar.gz
        URL_HASH  SHA256=8bfc3cbb78c5977404044b263ab984d1b02c23e04860d6a63f22b74c0cfc9e23
        SYSTEM
    )
    set(PT_NRD_ACTIVE ON)
    message(STATUS "NRD denoiser: enabled (v4.17.3 via FetchContent)")
endif()

# --- DLSS: Super Resolution / DLAA / Ray Reconstruction (Vulkan-only) ------
# Activated only when:
#   - PT_ENABLE_DLSS = ON              (default ON; owner decision, see the
#                                       option() comment in CMakeLists.txt)
#   - PT_ENABLE_VULKAN_BACKEND = ON    (DLSS is a Vulkan/NGX path here)
# Consumers check PT_DLSS_ACTIVE, never PT_ENABLE_DLSS directly, so a stale
# =ON in the cache on a no-Vulkan host is harmless -- the same pattern
# PT_NRD_ACTIVE uses above.
#
# STRUCTURALLY DIFFERENT FROM EVERY OTHER DEPENDENCY IN THIS FILE, and the
# difference is why this is not a FetchContent_MakeAvailable:
#
#   * DLSS ships PREBUILT BINARIES, not source. There is nothing to compile.
#     Consumption is: add include/ to the include path, link one import
#     library, and copy the runtime DLLs next to demont.exe. That is an
#     IMPORTED target, so we Populate and wire it by hand rather than
#     add_subdirectory() a project that has no CMakeLists.
#   * The binaries are real files in git, not LFS pointers -- the repo has no
#     .gitattributes, verified, which is the thing that would otherwise make a
#     tag-tarball fetch silently deliver stubs instead of DLLs.
#   * The tarball is LARGE (several hundred MB for the ~100 MB we use):
#     lib/Windows_x86_64/rel/ alone is nvngx_dlss.dll 59.0 MB +
#     nvngx_dlssd.dll 40.9 MB + nvngx_dlssg.dll 7.5 MB, and the dev/
#     debug-overlay variants add ~125 MB more. Accepted deliberately -- the
#     owner's call is that build size does not matter here -- but it is the
#     reason a first configure with this ON is slow.
#
# LICENSING, stated rather than implied. These are NVIDIA-RTX-SDK-licensed
# binaries. Redistribution is permitted as part of an application with
# material additional functionality beyond the SDK (a path tracer qualifies),
# and attribution -- crediting NVIDIA and showing the NVIDIA Marks in an
# about box / credits -- is required before a public release ships with this
# enabled. There is no fee and no approval gate. Note that "Streamline is
# MIT" does not change any of this: Streamline's MIT source loads these same
# licensed binaries.
set(PT_DLSS_ACTIVE OFF)
if(PT_ENABLE_DLSS AND NOT PT_ENABLE_VULKAN_BACKEND)
    message(STATUS "PT_ENABLE_DLSS requires PT_ENABLE_VULKAN_BACKEND; DLSS inactive.")
endif()
if(PT_ENABLE_DLSS AND PT_ENABLE_VULKAN_BACKEND AND NOT WIN32)
    message(STATUS "PT_ENABLE_DLSS: only the Windows_x86_64 SDK layout is wired; DLSS inactive.")
endif()
if(PT_ENABLE_DLSS AND PT_ENABLE_VULKAN_BACKEND AND WIN32)
    # URL_HASH is deliberately absent until the tag is pinned at bringup, and
    # this is a KNOWN GAP rather than an oversight: every other fetch in this
    # file pins a SHA256, and this one must too before it is trusted. The
    # first successful configure prints the computed hash (see below) so it
    # can be pasted in. Until then the fetch is reproducible by tag but not
    # verified against tampering.
    # SOURCE_SUBDIR names a directory that deliberately does not exist. The
    # DLSS repo has no CMakeLists.txt -- it is a binary drop, not a project --
    # so MakeAvailable must populate it WITHOUT trying to add_subdirectory().
    # Pointing SOURCE_SUBDIR at a non-existent path is the documented way to
    # get that, and it is why this is not the bare FetchContent_Populate()
    # that CMP0169 deprecates.
    FetchContent_Declare(dlss
        GIT_REPOSITORY https://github.com/NVIDIA/DLSS.git
        GIT_TAG        v310.7.0
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  cmake-does-not-build-this
        SYSTEM
    )
    FetchContent_MakeAvailable(dlss)

    set(PT_DLSS_INCLUDE_DIR "${dlss_SOURCE_DIR}/include")
    # nvsdk_ngx_d.lib is the DYNAMIC-CRT import library. The project builds
    # with clang-cl against the dynamic runtime, so nvsdk_ngx_s.lib (static
    # CRT) would be the wrong one and would produce CRT-mismatch link errors
    # rather than anything self-explanatory.
    # CONFIG-DEPENDENT, and this is not cosmetic: the Debug CRT
    # (MDd_DynamicDebug, _ITERATOR_DEBUG_LEVEL=2) cannot link a Release import
    # library. Getting it wrong produces ten LNK2038 mismatches and LNK1319 at
    # the final link -- which is exactly how CI failed the first time this was
    # built in Debug, since local development here is Release-only.
    #
    # docs/DLSS_INTEGRATION_PLAN.md section 7.2 flagged this as "[unverified]
    # whether the _dbg variants are needed for the Debug preset; check at
    # bringup". They are. This is that check.
    #
    # The SDK also ships _iterator0 / _iterator1 variants for projects that
    # override _ITERATOR_DEBUG_LEVEL. We do not, so the plain _dbg build (IDL
    # 2, the MSVC Debug default) is the right one.
    set(PT_DLSS_IMPORT_LIB     "${dlss_SOURCE_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
    set(PT_DLSS_IMPORT_LIB_DBG "${dlss_SOURCE_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib")
    # The runtime DLLs that must sit next to demont.exe. nvngx_dlssg.dll
    # (Frame Generation) is deliberately NOT shipped: it is out of scope, and
    # shipping an unused 7.5 MB licensed binary invites questions.
    set(PT_DLSS_RUNTIME_DLLS
        "${dlss_SOURCE_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll"
        "${dlss_SOURCE_DIR}/lib/Windows_x86_64/rel/nvngx_dlssd.dll")

    if(NOT EXISTS "${PT_DLSS_IMPORT_LIB}")
        message(WARNING
            "PT_ENABLE_DLSS: expected import library not found at "
            "${PT_DLSS_IMPORT_LIB} -- the SDK layout has moved. DLSS inactive.")
    else()
        add_library(dlss_ngx STATIC IMPORTED GLOBAL)
        set_target_properties(dlss_ngx PROPERTIES
            IMPORTED_LOCATION             "${PT_DLSS_IMPORT_LIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${PT_DLSS_INCLUDE_DIR}")
        # IMPORTED_LOCATION_DEBUG wins for a Debug build and falls back to the
        # plain IMPORTED_LOCATION otherwise. MAP_IMPORTED_CONFIG_* points the
        # optimised-with-symbols configs at the Release library, since they use
        # the release CRT.
        if(EXISTS "${PT_DLSS_IMPORT_LIB_DBG}")
            set_target_properties(dlss_ngx PROPERTIES
                IMPORTED_LOCATION_DEBUG              "${PT_DLSS_IMPORT_LIB_DBG}"
                MAP_IMPORTED_CONFIG_RELWITHDEBINFO   "Release"
                MAP_IMPORTED_CONFIG_MINSIZEREL       "Release")
        else()
            message(WARNING
                "PT_ENABLE_DLSS: debug import library not found at "
                "${PT_DLSS_IMPORT_LIB_DBG} -- a Debug build will fail to link "
                "with LNK2038 CRT mismatches.")
        endif()
        set(PT_DLSS_ACTIVE ON)
        message(STATUS "DLSS: enabled (v310.7.0, NVIDIA RTX SDK licence) -- ${dlss_SOURCE_DIR}")
    endif()
endif()

# --- doctest: unit test framework (header-only) ----------------------------
# Single-header testing framework. Fast compile (the framework header
# itself is ~7000 lines but only the TU declaring DOCTEST_CONFIG_IMPLEMENT
# pays the framework-implementation parse cost), MIT licensed, supports
# the usual TEST_CASE / SUBCASE / CHECK / REQUIRE / SECTION idioms. Picked
# over Catch2 / GoogleTest specifically because the compile-time overhead
# is much lower -- meaningful when test TUs proliferate across the
# pt_math / pt_csg / pt_renderer / pt_console modules.
#
# Only fetched when PT_BUILD_TESTS is ON (default ON; CI release builds
# pass -DPT_BUILD_TESTS=OFF). Gating it here avoids paying the (small)
# FetchContent populate cost on configures that don't need tests.
if(PT_BUILD_TESTS)
    FetchContent_Declare(doctest
        URL           https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
        URL_HASH      SHA256=632ed2c05a7f53fa961381497bf8069093f0d6628c5f26286161fbd32a560186
        SYSTEM
    )
endif()

FetchContent_MakeAvailable(glm fmt mimalloc glfw enkits civetweb tomlplusplus nlohmann_json)

if(PT_BUILD_TESTS)
    FetchContent_MakeAvailable(doctest)
endif()

# Cross-platform flags for silencing third-party warnings.  Vendored
# libraries (Embree's kernels, civetweb's sha1.inl, Tracy's sprintf use,
# etc.) emit thousands of warnings we can't fix without forking.  We
# silence them at target scope so our own code stays under strict
# warning levels.  Earlier versions of this file used GCC-style "-w" /
# "-UDEBUG" unconditionally, which MSVC silently ignored -- civetweb
# alone leaked ~137 C5045 warnings into the Windows release log on
# v0.3.15.  Use the matching MSVC flag form when CMAKE_CXX_COMPILER_ID
# is MSVC (cl.exe).  clang-cl reports as "Clang" so it falls through
# to the unix branch; clang-cl accepts "-w" / "-UDEBUG" in addition to
# the MSVC-style flags anyway, so either branch works for it.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(PT_DEP_WARN_SILENCE_FLAG /w)
    # /U<symbol> with no space -- if we used `/U DEBUG` here, CMake's
    # set() would create a two-element list and target_compile_options
    # would forward `/U` and `DEBUG` as separate cl.exe args.  cl.exe
    # then treats `DEBUG` as a source-input filename rather than the
    # symbol to undefine, and the DEBUG macro stays defined.  Joined
    # form matches the documented MSVC syntax and the -UDEBUG semantics
    # on the GCC/Clang side below.
    set(PT_DEP_UNDEF_DEBUG_FLAGS /UDEBUG)
else()
    set(PT_DEP_WARN_SILENCE_FLAG -w)
    set(PT_DEP_UNDEF_DEBUG_FLAGS -UDEBUG)
endif()

if(PT_ENABLE_VULKAN_BACKEND)
    FetchContent_MakeAvailable(vma)
endif()

# NRD: bring the `NRD` static-library target into the build alongside its
# auto-generated NRDShaders custom_target (compiles HLSL -> SPIRV via
# ShaderMake at build time). Block scope so NRD's `-Werror` doesn't escape
# into the rest of our build (NRD's CMakeLists adds `-Werror` PRIVATE on
# its own target, so this is technically redundant, but the block also
# isolates any CMP-policy / CMAKE_CXX_STANDARD touches inside NRD's
# subtree the same way we do for Embree / Manifold).
#
# Also: silence NRD's vendored third-party warnings against our strict
# warning level (MathLib + ShaderMakeBlob are also compiled here, with
# their own warning profiles we don't want polluting our build log).
if(PT_NRD_ACTIVE)
    block()
        FetchContent_MakeAvailable(nrd)
    endblock()
    if(TARGET NRD)
        target_compile_options(NRD PRIVATE ${PT_DEP_WARN_SILENCE_FLAG})
    endif()
    # NRD's transitive targets are a mixed bag of library kinds: MathLib is
    # an INTERFACE library (header-only) and ShaderMake ships both a real
    # executable and a UTILITY/custom target depending on the version.
    # target_compile_options() is a hard CMake ERROR on both of those
    # ("may only set INTERFACE properties on INTERFACE targets" /
    # "called with non-compilable target type"), so filter by TYPE before
    # touching them rather than by name.
    foreach(_t MathLib ShaderMake ShaderMakeBlob)
        if(TARGET ${_t})
            get_target_property(_t_type ${_t} TYPE)
            if(_t_type STREQUAL "STATIC_LIBRARY"  OR
               _t_type STREQUAL "SHARED_LIBRARY"  OR
               _t_type STREQUAL "MODULE_LIBRARY"  OR
               _t_type STREQUAL "OBJECT_LIBRARY"  OR
               _t_type STREQUAL "EXECUTABLE")
                target_compile_options(${_t} PRIVATE ${PT_DEP_WARN_SILENCE_FLAG})
            endif()
        endif()
    endforeach()
endif()

# Manifold reads the global TRACY_ENABLE cache var (same one we set for our
# Tracy fetch) and tries to download its own copy of tracy when it sees ON.
# Shadow it OFF for just manifold's configure step -- our renderer doesn't
# need manifold-internal profiling, and tracy still ships normally for our
# own targets via the unconditional fetch below.
block()
    set(TRACY_ENABLE OFF)
    FetchContent_MakeAvailable(manifold)
endblock()

# civetweb prints a flood of "*** ... worker_thread_run:..." trace lines
# whenever its `DEBUG` macro is defined (which CMake's Debug build type
# turns on). NDEBUG doesn't help -- the gate is `#if defined(DEBUG)`.
# Pass -UDEBUG to undefine it for this target only.
#
# civetweb uses C11 features (_Static_assert, etc.) but its CMakeLists
# doesn't request C11, so Apple Clang warns -Wpre-c11-compat on every
# build. Right fix is to give it the standard it actually uses, not
# silence the warning. C_STANDARD_REQUIRED ensures the compile fails
# loudly if a future bump pulls in an even newer C feature.
if(TARGET civetweb-c-library)
    # PT_DEP_UNDEF_DEBUG_FLAGS: kill civetweb's worker-thread trace flood
    #     under cmake Debug (DEBUG macro gate inside civetweb's source).
    # C_STANDARD 11: civetweb uses _Static_assert (a C11 feature).
    # PT_DEP_WARN_SILENCE_FLAG: civetweb's source has ~100 warnings (extra-
    #     semis in sha1.inl, sprintf deprecation, format mismatches, alloca
    #     usage on Mac; ~137 C5045 Spectre mitigation notes on MSVC) we
    #     can't fix without forking. Target-scoped suppresses only for
    #     this library; our own targets stay strict.
    target_compile_options(civetweb-c-library
        PRIVATE ${PT_DEP_UNDEF_DEBUG_FLAGS} ${PT_DEP_WARN_SILENCE_FLAG})
    set_target_properties(civetweb-c-library PROPERTIES
        C_STANDARD          11
        C_STANDARD_REQUIRED ON)
endif()
if(PT_ENABLE_TRACY)
    FetchContent_MakeAvailable(tracy)
    # Tracy uses deprecated sprintf in TracyProfiler/TracySocket/...
    # Same policy as civetweb -- vendored third-party C++ we don't
    # maintain, suppress at target scope.
    if(TARGET TracyClient)
        target_compile_options(TracyClient PRIVATE ${PT_DEP_WARN_SILENCE_FLAG})
    endif()
endif()

# enkiTS publishes its include path via the directory-scope command
# include_directories(), which is local to its own CMakeLists.txt.  Promote
# it to a target property so consumers picking up enkiTS via
# target_link_libraries actually see TaskScheduler.h.
if(TARGET enkiTS)
    target_include_directories(enkiTS PUBLIC ${enkits_SOURCE_DIR}/src)
endif()
