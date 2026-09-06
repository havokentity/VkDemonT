# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
# --- Window library (separate from the executable so engine can link it) ---
# On non-Windows platforms we compile a stub TU that provides the same
# C-extern symbols as no-ops so the Vulkan backend's link is satisfied
# without a native window runtime.
set(_pt_window_sources
    Window.cpp
    Gamepad.cpp
)
if(WIN32)
    # Native Win32 console overlay (GDI + monospace font, child window
    # attached to the GLFW HWND). Lives in the OS window hierarchy, not
    # the rendering pipeline.
    list(APPEND _pt_window_sources
        ConsoleOverlay_Win32.cpp
        PerfOverlay_Win32.cpp)
else()
    list(APPEND _pt_window_sources
        ConsoleOverlay_Stub.cpp
        PerfOverlay_Stub.cpp)
endif()

add_library(pt_app_window STATIC ${_pt_window_sources})

target_include_directories(pt_app_window PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(pt_app_window
    PUBLIC  pt_core
    PUBLIC  pt_console
    PUBLIC  glfw
    PUBLIC  fmt::fmt
)

if(WIN32)
    # GDI / user32 / kernel32. user32 is implicitly linked by MSVC
    # but list it explicitly so the dependency is visible.
    target_link_libraries(pt_app_window PUBLIC user32 gdi32)
endif()

pt_set_compiler_options(pt_app_window)

# --- Executable ------------------------------------------------------------
add_executable(demont
    main.cpp
)

target_link_libraries(demont PRIVATE
    # pt_engine PUBLIC-links to pt_core, pt_console, pt_app_window,
    # pt_rhi, pt_renderer, so they propagate transitively. Listing
    # them here AS WELL was producing the linker's "ignoring
    # duplicate libraries" warning every build.
    pt_engine
)

pt_set_compiler_options(demont)
