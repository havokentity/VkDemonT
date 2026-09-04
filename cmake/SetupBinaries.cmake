# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
# Auto-downloads the binary toolchain bits that don't fit in FetchContent.
# Platform-aware: each target builds with the right slangc binary.

# --- Slang (shader compiler) -----------------------------------------------
# Release archives are published per-platform at
#   https://github.com/shader-slang/slang/releases/download/v<ver>/...
# Names follow slang-<ver>-<os>-<arch>.{tar.gz,zip}.
set(PT_SLANG_VERSION 2026.8 CACHE STRING "Slang release tag")
set(_slang_dir ${CMAKE_SOURCE_DIR}/third_party/slang)

if(WIN32)
    set(_slang_archive_name "slang-${PT_SLANG_VERSION}-windows-x86_64.zip")
    set(_slang_exe_name "slangc.exe")
elseif(UNIX)
    set(_slang_archive_name "slang-${PT_SLANG_VERSION}-linux-x86_64.tar.gz")
    set(_slang_exe_name "slangc")
else()
    message(FATAL_ERROR "Unsupported platform for Slang download")
endif()

set(_slangc_path "${_slang_dir}/bin/${_slang_exe_name}")

if(NOT EXISTS "${_slangc_path}")
    set(_slang_url
        "https://github.com/shader-slang/slang/releases/download/v${PT_SLANG_VERSION}/${_slang_archive_name}")
    set(_slang_archive "${CMAKE_BINARY_DIR}/${_slang_archive_name}")
    message(STATUS "Downloading Slang ${PT_SLANG_VERSION} (${_slang_archive_name}) ...")
    file(DOWNLOAD "${_slang_url}" "${_slang_archive}"
         SHOW_PROGRESS STATUS _dl)
    list(GET _dl 0 _dl_code)
    if(NOT _dl_code EQUAL 0)
        message(FATAL_ERROR "Slang download failed: ${_dl}")
    endif()
    file(MAKE_DIRECTORY "${_slang_dir}")
    # Pick extraction flags by archive type. Modern CMake's `tar xf`
    # does auto-detect compression, but we've seen edge cases on older
    # 3.20-ish toolchains and busybox-tar fallbacks where `xf` silently
    # produces an empty extraction for .tar.gz inputs. Be explicit:
    # `xzf` for gzip tarballs, `xf` for plain zips. The Linux Slang
    # release is .tar.gz; the Windows release is .zip.
    if(_slang_archive MATCHES "\\.tar\\.gz$")
        set(_tar_flags xzf)
    else()
        set(_tar_flags xf)
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar ${_tar_flags} "${_slang_archive}"
                    WORKING_DIRECTORY "${_slang_dir}"
                    RESULT_VARIABLE _tar_rc)
    if(NOT _tar_rc EQUAL 0)
        message(FATAL_ERROR "Slang extract failed: rc=${_tar_rc}")
    endif()
    file(REMOVE "${_slang_archive}")
endif()

set(PT_SLANGC_BIN "${_slangc_path}" CACHE FILEPATH "slangc path")
