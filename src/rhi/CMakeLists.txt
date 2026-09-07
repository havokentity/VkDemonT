# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
add_library(pt_rhi STATIC
    Device.cpp
)

target_include_directories(pt_rhi PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pt_rhi PUBLIC pt_core)

if(PT_ENABLE_VULKAN_BACKEND)
    target_link_libraries(pt_rhi PUBLIC pt_rhi_vulkan)
    target_compile_definitions(pt_rhi PUBLIC PT_HAS_VULKAN_BACKEND=1)
endif()

pt_set_compiler_options(pt_rhi)
