// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
//
// NVIDIA RayTracingDenoiser (NRD) library integration -- issue #50.
// See VulkanNrdLibDenoiser.h for the REBLUR-vs-RELAX rationale and the
// input-plumbing contract. This file is the mechanics:
//
//   Init()   CreateInstance(RELAX_DIFFUSE)
//            -> descriptor set layouts derived from
//               LibraryDesc::spirvBindingOffsets + InstanceDesc's
//               register bases
//            -> one VkPipeline per InstanceDesc::pipelines[] entry from
//               the embedded SPIR-V
//            -> permanent + transient texture pools, the four NRD
//               inputs and the one NRD output, all SAMPLED|STORAGE and
//               all permanently in VK_IMAGE_LAYOUT_GENERAL
//            -> a host-coherent constant-buffer ring
//            -> the NrdPack / NrdUnpack compute pipelines
//
//   Encode() NrdPack -> SetCommonSettings -> SetDenoiserSettings ->
//            GetComputeDispatches -> replay the DispatchDesc list ->
//            NrdUnpack
//
// The SPIR-V register shifts are NOT hard-coded here: NRD compiles its
// HLSL with --sRegShift/--bRegShift/--uRegShift/--tRegShift (0/2/3/20 as
// of v4.17.3) and reports the result through
// GetLibraryDesc()->spirvBindingOffsets. Every binding number below is
// computed from that plus InstanceDesc's register bases, so an NRD
// version bump that moves them keeps working.

#if defined(PT_ENABLE_NRD)

#include "VulkanNrdLibDenoiser.h"
#include "VulkanDevice.h"

#include "../core/Log.h"

#include <NRD.h>

#include <algorithm>
#include <cstring>

extern "C" {
extern const unsigned char shader_NrdPack_spirv_data[];
extern const unsigned long shader_NrdPack_spirv_size;
extern const unsigned char shader_NrdUnpack_spirv_data[];
extern const unsigned long shader_NrdUnpack_spirv_size;
}

namespace pt::rhi::vk {

namespace {

const char* NrdResultStr(nrd::Result r) {
    switch (r) {
        case nrd::Result::SUCCESS:               return "SUCCESS";
        case nrd::Result::FAILURE:               return "FAILURE";
        case nrd::Result::INVALID_ARGUMENT:      return "INVALID_ARGUMENT";
        case nrd::Result::UNSUPPORTED:           return "UNSUPPORTED";
        case nrd::Result::NON_UNIQUE_IDENTIFIER: return "NON_UNIQUE_IDENTIFIER";
        default:                                 return "UNKNOWN";
    }
}

// Stable, arbitrary identifier for our single RELAX_DIFFUSE denoiser.
// Only has to be unique within this nrd::Instance.
constexpr nrd::Identifier kRelaxDiffuseId = 0x52454C58u;  // 'RELX'

// NRD only ever asks for formats it can express; anything unmapped is a
// hard error rather than a silent substitution (a wrong pool format
// produces garbage that looks like a denoiser bug).
VkFormat ToVkFormat(nrd::Format f) {
    switch (f) {
        case nrd::Format::R8_UNORM:            return VK_FORMAT_R8_UNORM;
        case nrd::Format::R8_SNORM:            return VK_FORMAT_R8_SNORM;
        case nrd::Format::R8_UINT:             return VK_FORMAT_R8_UINT;
        case nrd::Format::R8_SINT:             return VK_FORMAT_R8_SINT;
        case nrd::Format::RG8_UNORM:           return VK_FORMAT_R8G8_UNORM;
        case nrd::Format::RG8_SNORM:           return VK_FORMAT_R8G8_SNORM;
        case nrd::Format::RG8_UINT:            return VK_FORMAT_R8G8_UINT;
        case nrd::Format::RG8_SINT:            return VK_FORMAT_R8G8_SINT;
        case nrd::Format::RGBA8_UNORM:         return VK_FORMAT_R8G8B8A8_UNORM;
        case nrd::Format::RGBA8_SNORM:         return VK_FORMAT_R8G8B8A8_SNORM;
        case nrd::Format::RGBA8_UINT:          return VK_FORMAT_R8G8B8A8_UINT;
        case nrd::Format::RGBA8_SINT:          return VK_FORMAT_R8G8B8A8_SINT;
        case nrd::Format::RGBA8_SRGB:          return VK_FORMAT_R8G8B8A8_SRGB;
        case nrd::Format::R16_UNORM:           return VK_FORMAT_R16_UNORM;
        case nrd::Format::R16_SNORM:           return VK_FORMAT_R16_SNORM;
        case nrd::Format::R16_UINT:            return VK_FORMAT_R16_UINT;
        case nrd::Format::R16_SINT:            return VK_FORMAT_R16_SINT;
        case nrd::Format::R16_SFLOAT:          return VK_FORMAT_R16_SFLOAT;
        case nrd::Format::RG16_UNORM:          return VK_FORMAT_R16G16_UNORM;
        case nrd::Format::RG16_SNORM:          return VK_FORMAT_R16G16_SNORM;
        case nrd::Format::RG16_UINT:           return VK_FORMAT_R16G16_UINT;
        case nrd::Format::RG16_SINT:           return VK_FORMAT_R16G16_SINT;
        case nrd::Format::RG16_SFLOAT:         return VK_FORMAT_R16G16_SFLOAT;
        case nrd::Format::RGBA16_UNORM:        return VK_FORMAT_R16G16B16A16_UNORM;
        case nrd::Format::RGBA16_SNORM:        return VK_FORMAT_R16G16B16A16_SNORM;
        case nrd::Format::RGBA16_UINT:         return VK_FORMAT_R16G16B16A16_UINT;
        case nrd::Format::RGBA16_SINT:         return VK_FORMAT_R16G16B16A16_SINT;
        case nrd::Format::RGBA16_SFLOAT:       return VK_FORMAT_R16G16B16A16_SFLOAT;
        case nrd::Format::R32_UINT:            return VK_FORMAT_R32_UINT;
        case nrd::Format::R32_SINT:            return VK_FORMAT_R32_SINT;
        case nrd::Format::R32_SFLOAT:          return VK_FORMAT_R32_SFLOAT;
        case nrd::Format::RG32_UINT:           return VK_FORMAT_R32G32_UINT;
        case nrd::Format::RG32_SINT:           return VK_FORMAT_R32G32_SINT;
        case nrd::Format::RG32_SFLOAT:         return VK_FORMAT_R32G32_SFLOAT;
        case nrd::Format::RGB32_UINT:          return VK_FORMAT_R32G32B32_UINT;
        case nrd::Format::RGB32_SINT:          return VK_FORMAT_R32G32B32_SINT;
        case nrd::Format::RGB32_SFLOAT:        return VK_FORMAT_R32G32B32_SFLOAT;
        case nrd::Format::RGBA32_UINT:         return VK_FORMAT_R32G32B32A32_UINT;
        case nrd::Format::RGBA32_SINT:         return VK_FORMAT_R32G32B32A32_SINT;
        case nrd::Format::RGBA32_SFLOAT:       return VK_FORMAT_R32G32B32A32_SFLOAT;
        case nrd::Format::R10_G10_B10_A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case nrd::Format::R10_G10_B10_A2_UINT:  return VK_FORMAT_A2B10G10R10_UINT_PACK32;
        case nrd::Format::R11_G11_B10_UFLOAT:   return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case nrd::Format::R9_G9_B9_E5_UFLOAT:   return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
        default:                                return VK_FORMAT_UNDEFINED;
    }
}

std::uint32_t DivideUp(std::uint32_t x, std::uint32_t y) {
    return (y == 0u) ? x : ((x + y - 1u) / y);
}

std::uint32_t AlignUp(std::uint32_t v, std::uint32_t a) {
    return (a <= 1u) ? v : (((v + a - 1u) / a) * a);
}

VkShaderModule MakeModule(VkDevice dev, const void* bytes, std::size_t n) {
    // Mirrors VulkanDenoiser.cpp's MakeModule: SPIR-V arrives as
    // byte-aligned storage (NRD's embedded blobs are `const uint8_t[]`,
    // ours come from EmbedFile.cmake), and pCode wants 4-byte-aligned
    // words, so copy into an aligned vector first.
    if (n == 0 || (n % sizeof(std::uint32_t)) != 0) {
        LOG_ERROR("VulkanNrdLibDenoiser: invalid SPIR-V byte length {}", n);
        return VK_NULL_HANDLE;
    }
    std::vector<std::uint32_t> aligned(n / sizeof(std::uint32_t));
    std::memcpy(aligned.data(), bytes, n);
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = n;
    ci.pCode    = aligned.data();
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

// Push blocks for the two Slang helper kernels. Must match the
// [[vk::push_constant]] cbuffers in NrdPack.slang / NrdUnpack.slang.
struct PackPush {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t demod_enabled;
    std::uint32_t pad0;
};
static_assert(sizeof(PackPush) == 16, "PackPush layout");

struct UnpackPush {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t demod_enabled;
    std::uint32_t pad0;
    float         denoising_range;
    float         pad1;
    float         pad2;
    float         pad3;
};
static_assert(sizeof(UnpackPush) == 32, "UnpackPush layout");

// Depth beyond which a pixel is "sky" as far as NRD is concerned.
// PathTrace.slang stamps 1e10 into depth_tex on a primary miss (the
// sentinel its own sky gates key on), so this has to sit strictly
// between "furthest real geometry" and that sentinel. 1e9 m is 150x
// wider than the widest planetary shot this renderer sets up (a 400 km
// orbit sees ground at ~1e6 m) and 10x below the sentinel, so no real
// surface is ever dropped and no sky pixel is ever denoised.
// NrdUnpack.slang is handed the same number and forwards the original
// path-traced colour for everything at or beyond it -- which is the
// right answer anyway, because this renderer's sky and celestials are
// analytic and denoising them is what smears them.
constexpr float kDenoisingRange = 1.0e9f;

}  // namespace

// ---------------------------------------------------------------------
// Construction / teardown
// ---------------------------------------------------------------------

VulkanNrdLibDenoiser::~VulkanNrdLibDenoiser() {
    DestroyAll();
}

std::uint32_t VulkanNrdLibDenoiser::FindMemoryType(std::uint32_t type_bits,
                                                   VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(device_->RawPhysicalDevice(), &mp);
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) == 0u) continue;
        if ((mp.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    return UINT32_MAX;
}

bool VulkanNrdLibDenoiser::CreateOwnedImage(std::uint32_t w, std::uint32_t h,
                                            VkFormat fmt, const char* debug_label,
                                            OwnedImage& out) {
    VkDevice dev = device_->RawDevice();
    out = OwnedImage{};
    if (w == 0 || h == 0 || fmt == VK_FORMAT_UNDEFINED) {
        LOG_ERROR("VulkanNrdLibDenoiser: refusing to create image '{}' ({}x{}, fmt={})",
                  debug_label, w, h, static_cast<int>(fmt));
        return false;
    }

    VkImageCreateInfo ici{};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = fmt;
    ici.extent        = { w, h, 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    // SAMPLED is the whole reason these images can't come from
    // VulkanDevice::CreateTexture: NRD binds every non-output resource
    // through an SRV, and the RHI's texture path only asks for STORAGE.
    ici.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ici, nullptr, &out.image) != VK_SUCCESS) {
        LOG_ERROR("VulkanNrdLibDenoiser: vkCreateImage('{}') failed", debug_label);
        return false;
    }

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(dev, out.image, &mr);
    const std::uint32_t mt = FindMemoryType(mr.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) {
        LOG_ERROR("VulkanNrdLibDenoiser: no device-local memory type for '{}'", debug_label);
        DestroyOwnedImage(out);
        return false;
    }
    VkMemoryAllocateInfo mai{};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize  = mr.size;
    mai.memoryTypeIndex = mt;
    if (vkAllocateMemory(dev, &mai, nullptr, &out.memory) != VK_SUCCESS) {
        LOG_ERROR("VulkanNrdLibDenoiser: vkAllocateMemory({} B) failed for '{}'",
                  static_cast<std::uint64_t>(mr.size), debug_label);
        DestroyOwnedImage(out);
        return false;
    }
    vkBindImageMemory(dev, out.image, out.memory, 0);

    VkImageViewCreateInfo vci{};
    vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image    = out.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = fmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.layerCount = 1;
    vci.subresourceRange.levelCount = 1;
    if (vkCreateImageView(dev, &vci, nullptr, &out.view) != VK_SUCCESS) {
        LOG_ERROR("VulkanNrdLibDenoiser: vkCreateImageView('{}') failed", debug_label);
        DestroyOwnedImage(out);
        return false;
    }

    out.format = fmt;
    out.width  = w;
    out.height = h;
    return true;
}

void VulkanNrdLibDenoiser::DestroyOwnedImage(OwnedImage& img) {
    VkDevice dev = device_ ? device_->RawDevice() : VK_NULL_HANDLE;
    if (dev == VK_NULL_HANDLE) { img = OwnedImage{}; return; }
    if (img.view   != VK_NULL_HANDLE) vkDestroyImageView(dev, img.view, nullptr);
    if (img.image  != VK_NULL_HANDLE) vkDestroyImage(dev, img.image, nullptr);
    if (img.memory != VK_NULL_HANDLE) vkFreeMemory(dev, img.memory, nullptr);
    img = OwnedImage{};
}

void VulkanNrdLibDenoiser::DestroySizedResources() {
    for (OwnedImage& i : pool_) DestroyOwnedImage(i);
    pool_.clear();
    DestroyOwnedImage(in_mv_);
    DestroyOwnedImage(in_normal_roughness_);
    DestroyOwnedImage(in_viewz_);
    DestroyOwnedImage(in_radiance_);
    DestroyOwnedImage(out_radiance_);
    cached_w_ = 0;
    cached_h_ = 0;
    needs_layout_init_ = true;
}

void VulkanNrdLibDenoiser::DestroyAll() {
    if (device_ == nullptr) { nrd_inst_ = nullptr; return; }
    VkDevice dev = device_->RawDevice();
    if (dev != VK_NULL_HANDLE) {
        // One stall covers every destroy below. Individual vkDestroy*
        // calls have no implicit wait, so the GPU must be idle before
        // any of the images / pipelines can be released.
        device_->WaitIdle();

        DestroySizedResources();

        for (VkPipeline p : nrd_pipelines_) {
            if (p != VK_NULL_HANDLE) vkDestroyPipeline(dev, p, nullptr);
        }
        nrd_pipelines_.clear();

        if (pack_pipe_   != VK_NULL_HANDLE) vkDestroyPipeline(dev, pack_pipe_, nullptr);
        if (unpack_pipe_ != VK_NULL_HANDLE) vkDestroyPipeline(dev, unpack_pipe_, nullptr);
        pack_pipe_ = unpack_pipe_ = VK_NULL_HANDLE;

        if (nrd_pipe_layout_    != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, nrd_pipe_layout_, nullptr);
        if (pack_pipe_layout_   != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, pack_pipe_layout_, nullptr);
        if (unpack_pipe_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, unpack_pipe_layout_, nullptr);
        nrd_pipe_layout_ = pack_pipe_layout_ = unpack_pipe_layout_ = VK_NULL_HANDLE;

        if (nrd_res_layout_   != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, nrd_res_layout_, nullptr);
        if (nrd_cb_layout_    != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, nrd_cb_layout_, nullptr);
        if (nrd_empty_layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, nrd_empty_layout_, nullptr);
        if (pack_layout_      != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, pack_layout_, nullptr);
        if (unpack_layout_    != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, unpack_layout_, nullptr);
        nrd_res_layout_ = nrd_cb_layout_ = nrd_empty_layout_ = VK_NULL_HANDLE;
        pack_layout_ = unpack_layout_ = VK_NULL_HANDLE;

        if (nrd_dpool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, nrd_dpool_, nullptr);
        if (aux_dpool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, aux_dpool_, nullptr);
        nrd_dpool_ = aux_dpool_ = VK_NULL_HANDLE;
        nrd_res_sets_.clear();
        nrd_cb_set_ = VK_NULL_HANDLE;
        for (int i = 0; i < kAuxSetRing; ++i) {
            pack_sets_[i]   = VK_NULL_HANDLE;
            unpack_sets_[i] = VK_NULL_HANDLE;
        }

        if (sampler_nearest_ != VK_NULL_HANDLE) vkDestroySampler(dev, sampler_nearest_, nullptr);
        if (sampler_linear_  != VK_NULL_HANDLE) vkDestroySampler(dev, sampler_linear_, nullptr);
        sampler_nearest_ = sampler_linear_ = VK_NULL_HANDLE;

        if (cb_buffer_ != VK_NULL_HANDLE) {
            if (cb_mapped_ != nullptr) vkUnmapMemory(dev, cb_memory_);
            vkDestroyBuffer(dev, cb_buffer_, nullptr);
            vkFreeMemory(dev, cb_memory_, nullptr);
        }
        cb_buffer_ = VK_NULL_HANDLE;
        cb_memory_ = VK_NULL_HANDLE;
        cb_mapped_ = nullptr;
    }

    if (nrd_inst_ != nullptr) {
        nrd::DestroyInstance(*nrd_inst_);
        nrd_inst_ = nullptr;
    }
    ready_ = false;
}

// ---------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------

bool VulkanNrdLibDenoiser::CreateInstanceOnce() {
    if (nrd_inst_ != nullptr) return true;

    const nrd::LibraryDesc* lib = nrd::GetLibraryDesc();
    if (lib == nullptr) {
        LOG_ERROR("VulkanNrdLibDenoiser: nrd::GetLibraryDesc returned null "
                  "-- the NRD static link is broken");
        return false;
    }
    LOG_INFO("VulkanNrdLibDenoiser: NRD v{}.{}.{} "
             "(normalEncoding={}, roughnessEncoding={}, "
             "spirv shifts s={} b={} u={} t={})",
             lib->versionMajor, lib->versionMinor, lib->versionBuild,
             static_cast<int>(lib->normalEncoding),
             static_cast<int>(lib->roughnessEncoding),
             lib->spirvBindingOffsets.samplerOffset,
             lib->spirvBindingOffsets.constantBufferOffset,
             lib->spirvBindingOffsets.storageTextureAndBufferOffset,
             lib->spirvBindingOffsets.textureOffset);

    // NrdPack.slang hard-codes one encoding pair (the two values
    // cmake/Dependencies.cmake compiles NRD's shaders with). If a
    // version bump or a cache edit moves them, the packed normals would
    // decode to garbage inside NRD with no error anywhere -- so refuse
    // loudly instead.
    if (lib->normalEncoding != nrd::NormalEncoding::R10_G10_B10_A2_UNORM ||
        lib->roughnessEncoding != nrd::RoughnessEncoding::LINEAR) {
        LOG_ERROR("VulkanNrdLibDenoiser: NRD was built with normalEncoding={} / "
                  "roughnessEncoding={}, but NrdPack.slang encodes for "
                  "R10_G10_B10_A2_UNORM (2) / LINEAR (1). Fix "
                  "NRD_NORMAL_ENCODING / NRD_ROUGHNESS_ENCODING in "
                  "cmake/Dependencies.cmake or update nrdPackNormalRoughness.",
                  static_cast<int>(lib->normalEncoding),
                  static_cast<int>(lib->roughnessEncoding));
        return false;
    }

    nrd::DenoiserDesc relax{};
    relax.identifier = kRelaxDiffuseId;
    relax.denoiser   = nrd::Denoiser::RELAX_DIFFUSE;

    nrd::InstanceCreationDesc ci{};
    ci.denoisers    = &relax;
    ci.denoisersNum = 1;
    // allocationCallbacks left default (new/delete). RELAX_DIFFUSE's
    // host-side state is a few tens of KB; routing it through the
    // engine's PersistentHeap would buy nothing measurable.

    nrd::Instance* inst = nullptr;
    const nrd::Result r = nrd::CreateInstance(ci, inst);
    if (r != nrd::Result::SUCCESS || inst == nullptr) {
        LOG_ERROR("VulkanNrdLibDenoiser: nrd::CreateInstance(RELAX_DIFFUSE) failed ({})",
                  NrdResultStr(r));
        return false;
    }
    nrd_inst_ = inst;
    return true;
}

bool VulkanNrdLibDenoiser::BuildNrdPipelineObjects() {
    VkDevice dev = device_->RawDevice();
    const nrd::LibraryDesc*  lib = nrd::GetLibraryDesc();
    const nrd::InstanceDesc* id  = nrd::GetInstanceDesc(*nrd_inst_);
    if (lib == nullptr || id == nullptr) {
        LOG_ERROR("VulkanNrdLibDenoiser: null Library/InstanceDesc after CreateInstance");
        return false;
    }

    res_space_index_      = id->resourcesSpaceIndex;
    cb_space_index_       = id->constantBufferAndSamplersSpaceIndex;
    permanent_pool_size_  = id->permanentPoolSize;
    texture_binding_base_ = lib->spirvBindingOffsets.textureOffset + id->resourcesBaseRegisterIndex;
    storage_binding_base_ = lib->spirvBindingOffsets.storageTextureAndBufferOffset +
                            id->resourcesBaseRegisterIndex;
    const std::uint32_t sampler_base = lib->spirvBindingOffsets.samplerOffset +
                                       id->samplersBaseRegisterIndex;
    const std::uint32_t cb_binding   = lib->spirvBindingOffsets.constantBufferOffset +
                                       id->constantBufferRegisterIndex;

    // ---- samplers ---------------------------------------------------
    // NRD asks for exactly two, both clamp-to-edge, differing only in
    // filter. They become immutable samplers in the constant-buffer
    // set layout so the per-frame descriptor writes never touch them.
    auto make_sampler = [&](VkFilter filter, VkSampler& out) -> bool {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = filter;
        si.minFilter    = filter;
        si.mipmapMode   = (filter == VK_FILTER_LINEAR) ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                       : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod       = VK_LOD_CLAMP_NONE;
        si.borderColor  = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        return vkCreateSampler(dev, &si, nullptr, &out) == VK_SUCCESS;
    };
    if (!make_sampler(VK_FILTER_NEAREST, sampler_nearest_) ||
        !make_sampler(VK_FILTER_LINEAR,  sampler_linear_)) {
        LOG_ERROR("VulkanNrdLibDenoiser: vkCreateSampler failed");
        return false;
    }

    std::vector<VkSampler> immutable(id->samplersNum, VK_NULL_HANDLE);
    for (std::uint32_t i = 0; i < id->samplersNum; ++i) {
        immutable[i] = (id->samplers[i] == nrd::Sampler::LINEAR_CLAMP)
                           ? sampler_linear_ : sampler_nearest_;
    }

    // ---- resources set layout (space = resourcesSpaceIndex) ---------
    // DXC emits one binding per HLSL register (t0 -> textureOffset + 0,
    // t1 -> textureOffset + 1, ...), NOT a single array binding, so the
    // layout mirrors that one-for-one. PARTIALLY_BOUND because any
    // given pipeline only declares a prefix of each range.
    const std::uint32_t tex_num = id->descriptorPoolDesc.perSetTexturesMaxNum;
    const std::uint32_t sto_num = id->descriptorPoolDesc.perSetStorageTexturesMaxNum;
    {
        std::vector<VkDescriptorSetLayoutBinding> b;
        b.reserve(tex_num + sto_num);
        for (std::uint32_t i = 0; i < tex_num; ++i) {
            VkDescriptorSetLayoutBinding e{};
            e.binding         = texture_binding_base_ + i;
            e.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            e.descriptorCount = 1;
            e.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            b.push_back(e);
        }
        for (std::uint32_t i = 0; i < sto_num; ++i) {
            VkDescriptorSetLayoutBinding e{};
            e.binding         = storage_binding_base_ + i;
            e.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            e.descriptorCount = 1;
            e.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            b.push_back(e);
        }
        std::vector<VkDescriptorBindingFlags> flags(b.size(),
                                                    VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
        VkDescriptorSetLayoutBindingFlagsCreateInfo bf{};
        bf.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        bf.bindingCount  = static_cast<std::uint32_t>(flags.size());
        bf.pBindingFlags = flags.data();

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.pNext        = &bf;
        ci.bindingCount = static_cast<std::uint32_t>(b.size());
        ci.pBindings    = b.data();
        if (vkCreateDescriptorSetLayout(dev, &ci, nullptr, &nrd_res_layout_) != VK_SUCCESS) {
            LOG_ERROR("VulkanNrdLibDenoiser: NRD resources set layout failed "
                      "({} textures + {} storage images)", tex_num, sto_num);
            return false;
        }
    }

    // ---- constant buffer + samplers set layout ----------------------
    {
        std::vector<VkDescriptorSetLayoutBinding> b;
        for (std::uint32_t i = 0; i < id->samplersNum; ++i) {
            VkDescriptorSetLayoutBinding e{};
            e.binding            = sampler_base + i;
            e.descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLER;
            e.descriptorCount    = 1;
            e.stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT;
            e.pImmutableSamplers = &immutable[i];
            b.push_back(e);
        }
        VkDescriptorSetLayoutBinding cbb{};
        cbb.binding         = cb_binding;
        // DYNAMIC so one descriptor plus a per-dispatch offset serves
        // the whole ring, which is exactly the shape NRD's own
        // integration uses (a root CBV with a streamed offset).
        cbb.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        cbb.descriptorCount = 1;
        cbb.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        b.push_back(cbb);

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = static_cast<std::uint32_t>(b.size());
        ci.pBindings    = b.data();
        if (vkCreateDescriptorSetLayout(dev, &ci, nullptr, &nrd_cb_layout_) != VK_SUCCESS) {
            LOG_ERROR("VulkanNrdLibDenoiser: NRD cbuffer/sampler set layout failed");
            return false;
        }
    }

    // ---- pipeline layout --------------------------------------------
    // HLSL `spaceN` maps to Vulkan descriptor set N, so the set array is
    // indexed by NRD's two space indices (0 and 1 in v4.17.3). Any gap
    // gets an empty layout rather than assuming adjacency.
    {
        VkDescriptorSetLayoutCreateInfo eci{};
        eci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        if (vkCreateDescriptorSetLayout(dev, &eci, nullptr, &nrd_empty_layout_) != VK_SUCCESS) {
            LOG_ERROR("VulkanNrdLibDenoiser: empty set layout failed");
            return false;
        }
        const std::uint32_t max_space = std::max(res_space_index_, cb_space_index_);
        std::vector<VkDescriptorSetLayout> sets(max_space + 1u, nrd_empty_layout_);
        sets[res_space_index_] = nrd_res_layout_;
        sets[cb_space_index_]  = nrd_cb_layout_;

        VkPipelineLayoutCreateInfo plci{};
        plci.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = static_cast<std::uint32_t>(sets.size());
        plci.pSetLayouts    = sets.data();
        if (vkCreatePipelineLayout(dev, &plci, nullptr, &nrd_pipe_layout_) != VK_SUCCESS) {
            LOG_ERROR("VulkanNrdLibDenoiser: NRD pipeline layout failed");
            return false;
        }
    }

    // ---- pipelines ---------------------------------------------------
    nrd_pipelines_.assign(id->pipelinesNum, VK_NULL_HANDLE);
    for (std::uint32_t i = 0; i < id->pipelinesNum; ++i) {
        const nrd::PipelineDesc& pd = id->pipelines[i];
        if (pd.computeShaderSPIRV.bytecode == nullptr || pd.computeShaderSPIRV.size == 0) {
            LOG_ERROR("VulkanNrdLibDenoiser: pipeline {} ('{}') has no SPIR-V -- was NRD "
                      "built with NRD_EMBEDS_SPIRV_SHADERS=ON?", i, pd.shaderIdentifier);
            return false;
        }
        VkShaderModule m = MakeModule(dev, pd.computeShaderSPIRV.bytecode,
                                      static_cast<std::size_t>(pd.computeShaderSPIRV.size));
        if (m == VK_NULL_HANDLE) {
            LOG_ERROR("VulkanNrdLibDenoiser: shader module failed for pipeline {} ('{}')",
                      i, pd.shaderIdentifier);
            return false;
        }
        VkPipelineShaderStageCreateInfo s{};
        s.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        s.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        s.module = m;
        s.pName  = id->shaderEntryPoint;   // "NRD_CS_MAIN"
        VkComputePipelineCreateInfo cpci{};
        cpci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.layout = nrd_pipe_layout_;
        cpci.stage  = s;
        const VkResult vr = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci,
                                                     nullptr, &nrd_pipelines_[i]);
        vkDestroyShaderModule(dev, m, nullptr);
        if (vr != VK_SUCCESS) {
            LOG_ERROR("VulkanNrdLibDenoiser: vkCreateComputePipelines({} '{}') = {}",
                      i, pd.shaderIdentifier, static_cast<int>(vr));
            return false;
        }
    }

    // ---- descriptor pool + set ring ----------------------------------
    // setsMaxNum is NRD's per-frame ceiling. Multiply by the swapchain's
    // frames-in-flight (2) plus a frame of slack, so a set is never
    // rewritten while an earlier submission might still be reading it.
    {
        const std::uint32_t ring =
            std::max<std::uint32_t>(id->descriptorPoolDesc.setsMaxNum, 1u) * 3u + 8u;
        std::vector<VkDescriptorPoolSize> ps;
        if (tex_num > 0) {
            ps.push_back({ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, ring * tex_num });
        }
        if (sto_num > 0) {
            ps.push_back({ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, ring * sto_num });
        }
        ps.push_back({ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1u });
        if (id->samplersNum > 0) {
            ps.push_back({ VK_DESCRIPTOR_TYPE_SAMPLER, id->samplersNum });
        }
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets       = ring + 1u;   // + the constant-buffer set
        dpci.poolSizeCount = static_cast<std::uint32_t>(ps.size());
        dpci.pPoolSizes    = ps.data();
        if (vkCreateDescriptorPool(dev, &dpci, nullptr, &nrd_dpool_) != VK_SUCCESS) {
            LOG_ERROR("VulkanNrdLibDenoiser: NRD descriptor pool failed (ring={})", ring);
            return false;
        }

        std::vector<VkDescriptorSetLayout> layouts(ring, nrd_res_layout_);
        nrd_res_sets_.assign(ring, VK_NULL_HANDLE);
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = nrd_dpool_;
        ai.descriptorSetCount = ring;
        ai.pSetLayouts        = layouts.data();
        if (vkAllocateDescriptorSets(dev, &ai, nrd_res_sets_.data()) != VK_SUCCESS) {
            LOG_ERROR("VulkanNrdLibDenoiser: vkAllocateDescriptorSets({}) failed", ring);
            return false;
        }

        VkDescriptorSetAllocateInfo cbai{};
        cbai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        cbai.descriptorPool     = nrd_dpool_;
        cbai.descriptorSetCount = 1;
        cbai.pSetLayouts        = &nrd_cb_layout_;
        if (vkAllocateDescriptorSets(dev, &cbai, &nrd_cb_set_) != VK_SUCCESS) {
            LOG_ERROR("VulkanNrdLibDenoiser: cbuffer descriptor set alloc failed");
            return false;
        }
        cb_slots_ = ring;
    }

    return true;
}

bool VulkanNrdLibDenoiser::BuildConstantBuffer() {
    VkDevice dev = device_->RawDevice();
    const nrd::InstanceDesc* id = nrd::GetInstanceDesc(*nrd_inst_);
    if (id == nullptr) return false;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device_->RawPhysicalDevice(), &props);
    const std::uint32_t align =
        static_cast<std::uint32_t>(std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 1));

    cb_stride_ = AlignUp(std::max<std::uint32_t>(id->constantBufferMaxDataSize, 16u), align);
    if (cb_stride_ > props.limits.maxUniformBufferRange) {
        LOG_ERROR("VulkanNrdLibDenoiser: NRD constant slice {} B exceeds "
                  "maxUniformBufferRange {} B", cb_stride_, props.limits.maxUniformBufferRange);
        return false;
    }
    if (cb_slots_ == 0) cb_slots_ = 64;
    const VkDeviceSize total = static_cast<VkDeviceSize>(cb_stride_) * cb_slots_;

    VkBufferCreateInfo bci{};
    bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size        = total;
    bci.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &bci, nullptr, &cb_buffer_) != VK_SUCCESS) {
        LOG_ERROR("VulkanNrdLibDenoiser: NRD constant buffer create failed ({} B)",
                  static_cast<std::uint64_t>(total));
        return false;
    }
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(dev, cb_buffer_, &mr);
    const std::uint32_t mt = FindMemoryType(mr.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) {
        LOG_ERROR("VulkanNrdLibDenoiser: no host-visible coherent memory type "
                  "for the NRD constant buffer");
        return false;
    }
    VkMemoryAllocateInfo mai{};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize  = mr.size;
    mai.memoryTypeIndex = mt;
    if (vkAllocateMemory(dev, &mai, nullptr, &cb_memory_) != VK_SUCCESS) {
        LOG_ERROR("VulkanNrdLibDenoiser: NRD constant buffer memory alloc failed");
        return false;
    }
    vkBindBufferMemory(dev, cb_buffer_, cb_memory_, 0);
    if (vkMapMemory(dev, cb_memory_, 0, VK_WHOLE_SIZE, 0, &cb_mapped_) != VK_SUCCESS) {
        LOG_ERROR("VulkanNrdLibDenoiser: NRD constant buffer map failed");
        cb_mapped_ = nullptr;
        return false;
    }

    // Point the (single, dynamic) constant descriptor at the buffer.
    VkDescriptorBufferInfo bi{};
    bi.buffer = cb_buffer_;
    bi.offset = 0;
    bi.range  = cb_stride_;
    const nrd::LibraryDesc* lib = nrd::GetLibraryDesc();
    VkWriteDescriptorSet w{};
    w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet          = nrd_cb_set_;
    w.dstBinding      = lib->spirvBindingOffsets.constantBufferOffset + id->constantBufferRegisterIndex;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    w.pBufferInfo     = &bi;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    return true;
}

bool VulkanNrdLibDenoiser::BuildAuxPipelines() {
    VkDevice dev = device_->RawDevice();

    auto make_layout = [&](std::uint32_t binding_count, std::uint32_t push_size,
                           VkDescriptorSetLayout& dsl, VkPipelineLayout& pl) -> bool {
        std::vector<VkDescriptorSetLayoutBinding> b(binding_count);
        for (std::uint32_t i = 0; i < binding_count; ++i) {
            b[i].binding         = i;
            b[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            b[i].descriptorCount = 1;
            b[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = binding_count;
        ci.pBindings    = b.data();
        if (vkCreateDescriptorSetLayout(dev, &ci, nullptr, &dsl) != VK_SUCCESS) return false;
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.size       = push_size;
        VkPipelineLayoutCreateInfo plci{};
        plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount         = 1;
        plci.pSetLayouts            = &dsl;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges    = &pcr;
        return vkCreatePipelineLayout(dev, &plci, nullptr, &pl) == VK_SUCCESS;
    };

    if (!make_layout(9, sizeof(PackPush), pack_layout_, pack_pipe_layout_) ||
        !make_layout(5, sizeof(UnpackPush), unpack_layout_, unpack_pipe_layout_)) {
        LOG_ERROR("VulkanNrdLibDenoiser: NrdPack/NrdUnpack layout creation failed");
        return false;
    }

    auto build = [&](const unsigned char* blob, std::size_t n,
                     VkPipelineLayout layout, VkPipeline& out, const char* label) -> bool {
        VkShaderModule m = MakeModule(dev, blob, n);
        if (m == VK_NULL_HANDLE) return false;
        VkPipelineShaderStageCreateInfo s{};
        s.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        s.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        s.module = m;
        s.pName  = "main";
        VkComputePipelineCreateInfo cpci{};
        cpci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.layout = layout;
        cpci.stage  = s;
        const VkResult vr = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &out);
        vkDestroyShaderModule(dev, m, nullptr);
        if (vr != VK_SUCCESS) {
            LOG_ERROR("VulkanNrdLibDenoiser: vkCreateComputePipelines({}) = {}",
                      label, static_cast<int>(vr));
            return false;
        }
        return true;
    };
    if (!build(shader_NrdPack_spirv_data, shader_NrdPack_spirv_size,
               pack_pipe_layout_, pack_pipe_, "NrdPack")) return false;
    if (!build(shader_NrdUnpack_spirv_data, shader_NrdUnpack_spirv_size,
               unpack_pipe_layout_, unpack_pipe_, "NrdUnpack")) return false;

    // Aux descriptor pool: kAuxSetRing pack sets + kAuxSetRing unpack
    // sets. One of each per frame, 2 frames in flight -- 8 is generous.
    VkDescriptorPoolSize ps{};
    ps.type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    ps.descriptorCount = static_cast<std::uint32_t>(kAuxSetRing) * (9u + 5u);
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets       = static_cast<std::uint32_t>(kAuxSetRing) * 2u;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes    = &ps;
    if (vkCreateDescriptorPool(dev, &dpci, nullptr, &aux_dpool_) != VK_SUCCESS) {
        LOG_ERROR("VulkanNrdLibDenoiser: aux descriptor pool failed");
        return false;
    }
    std::vector<VkDescriptorSetLayout> pl(kAuxSetRing, pack_layout_);
    std::vector<VkDescriptorSetLayout> ul(kAuxSetRing, unpack_layout_);
    VkDescriptorSetAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = aux_dpool_;
    ai.descriptorSetCount = kAuxSetRing;
    ai.pSetLayouts        = pl.data();
    if (vkAllocateDescriptorSets(dev, &ai, pack_sets_) != VK_SUCCESS) {
        LOG_ERROR("VulkanNrdLibDenoiser: NrdPack descriptor sets failed");
        return false;
    }
    ai.pSetLayouts = ul.data();
    if (vkAllocateDescriptorSets(dev, &ai, unpack_sets_) != VK_SUCCESS) {
        LOG_ERROR("VulkanNrdLibDenoiser: NrdUnpack descriptor sets failed");
        return false;
    }
    return true;
}

bool VulkanNrdLibDenoiser::BuildSizedResources(std::uint32_t w, std::uint32_t h) {
    const nrd::InstanceDesc* id = nrd::GetInstanceDesc(*nrd_inst_);
    if (id == nullptr) return false;

    const std::uint32_t total = id->permanentPoolSize + id->transientPoolSize;
    pool_.assign(total, OwnedImage{});
    for (std::uint32_t i = 0; i < total; ++i) {
        const nrd::TextureDesc& td = (i < id->permanentPoolSize)
                                         ? id->permanentPool[i]
                                         : id->transientPool[i - id->permanentPoolSize];
        const VkFormat fmt = ToVkFormat(td.format);
        if (fmt == VK_FORMAT_UNDEFINED) {
            LOG_ERROR("VulkanNrdLibDenoiser: unmapped NRD pool format {} at slot {}",
                      static_cast<int>(td.format), i);
            return false;
        }
        const std::uint32_t tw = std::max(DivideUp(w, td.downsampleFactor), 1u);
        const std::uint32_t th = std::max(DivideUp(h, td.downsampleFactor), 1u);
        if (!CreateOwnedImage(tw, th, fmt,
                              (i < id->permanentPoolSize) ? "nrd_permanent" : "nrd_transient",
                              pool_[i])) {
            return false;
        }
    }

    // App-side NRD I/O. Formats chosen so every one is a core-required
    // Vulkan storage format (no shaderStorageImageExtendedFormats):
    //   IN_NORMAL_ROUGHNESS is RGBA16F rather than the
    //   R10G10B10A2_UNORM the encoding is named for -- NRD never
    //   inspects the format, the encoded values are all in [0, 1], and
    //   fp16 in that range beats 10-bit UNORM. See NrdPack.slang.
    if (!CreateOwnedImage(w, h, VK_FORMAT_R16G16_SFLOAT,       "nrd_in_mv",       in_mv_)          ||
        !CreateOwnedImage(w, h, VK_FORMAT_R16G16B16A16_SFLOAT, "nrd_in_nr",       in_normal_roughness_) ||
        !CreateOwnedImage(w, h, VK_FORMAT_R32_SFLOAT,          "nrd_in_viewz",    in_viewz_)       ||
        !CreateOwnedImage(w, h, VK_FORMAT_R16G16B16A16_SFLOAT, "nrd_in_radiance", in_radiance_)    ||
        !CreateOwnedImage(w, h, VK_FORMAT_R16G16B16A16_SFLOAT, "nrd_out_radiance", out_radiance_)) {
        return false;
    }

    cached_w_ = w;
    cached_h_ = h;
    needs_layout_init_   = true;
    // Fresh (or recycled) device memory: NRD's permanent pool holds
    // garbage until CLEAR_AND_RESTART zeroes it.
    force_accum_restart_ = true;
    return true;
}

bool VulkanNrdLibDenoiser::Init(std::uint32_t width, std::uint32_t height) {
    if (device_ == nullptr || width == 0 || height == 0) return false;
    if (ready_ && cached_w_ == width && cached_h_ == height) return true;

    // A latched failure only unlatches on a genuine resize: nothing
    // about a rejected CreateInstance / pipeline build becomes true by
    // asking again at the same dimensions.
    if (failed_ && failed_w_ == width && failed_h_ == height) return false;

    auto fail = [&]() -> bool {
        DestroyAll();
        failed_   = true;
        failed_w_ = width;
        failed_h_ = height;
        ready_    = false;
        return false;
    };

    if (!CreateInstanceOnce()) return fail();

    // Size-independent half is built once; a resize only redoes the
    // texture pools below.
    if (nrd_pipe_layout_ == VK_NULL_HANDLE) {
        if (!BuildNrdPipelineObjects()) return fail();
        if (!BuildConstantBuffer())     return fail();
        if (!BuildAuxPipelines())       return fail();
    } else {
        device_->WaitIdle();
        DestroySizedResources();
    }

    if (!BuildSizedResources(width, height)) return fail();

    // Fires on every real (re)build only -- Init() returns at the top
    // when it is already ready at these dimensions, so this is a
    // startup + resize log, not a per-frame one.
    {
        const nrd::InstanceDesc* id = nrd::GetInstanceDesc(*nrd_inst_);
        LOG_INFO("VulkanNrdLibDenoiser: RELAX_DIFFUSE ready at {}x{} "
                 "(pipelines={}, permanent={}, transient={}, "
                 "cbuf/dispatch<={} B, per-set textures={} storage={}, sets/frame={})",
                 width, height,
                 id ? id->pipelinesNum : 0u,
                 id ? id->permanentPoolSize : 0u,
                 id ? id->transientPoolSize : 0u,
                 id ? id->constantBufferMaxDataSize : 0u,
                 id ? id->descriptorPoolDesc.perSetTexturesMaxNum : 0u,
                 id ? id->descriptorPoolDesc.perSetStorageTexturesMaxNum : 0u,
                 id ? id->descriptorPoolDesc.setsMaxNum : 0u);
    }

    frame_index_        = 0;
    have_prev_matrices_ = false;
    failed_             = false;
    ready_              = true;
    return true;
}

// ---------------------------------------------------------------------
// Encode
// ---------------------------------------------------------------------

void VulkanNrdLibDenoiser::ComputeBarrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb{};
    mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
}

void VulkanNrdLibDenoiser::RecordInitialLayoutTransitions(VkCommandBuffer cb) {
    std::vector<VkImageMemoryBarrier> bs;
    bs.reserve(pool_.size() + 5);
    auto add = [&](const OwnedImage& img) {
        if (img.image == VK_NULL_HANDLE) return;
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask       = 0;
        b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = img.image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.layerCount = 1;
        b.subresourceRange.levelCount = 1;
        bs.push_back(b);
    };
    for (const OwnedImage& i : pool_) add(i);
    add(in_mv_);
    add(in_normal_roughness_);
    add(in_viewz_);
    add(in_radiance_);
    add(out_radiance_);
    if (bs.empty()) return;
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr,
                         static_cast<std::uint32_t>(bs.size()), bs.data());
}

void VulkanNrdLibDenoiser::Encode(VkCommandBuffer cb, const EncodeInputs& in) {
    if (!ready_ || nrd_inst_ == nullptr || device_ == nullptr) return;
    VkDevice dev = device_->RawDevice();

    const std::uint32_t w = cached_w_;
    const std::uint32_t h = cached_h_;
    const std::uint32_t gx = (w + 7u) / 8u;
    const std::uint32_t gy = (h + 7u) / 8u;

    if (needs_layout_init_) {
        RecordInitialLayoutTransitions(cb);
        needs_layout_init_ = false;
    }

    const int aux_slot = next_aux_set_;
    next_aux_set_ = (next_aux_set_ + 1) % kAuxSetRing;

    // ---- 1. NrdPack ---------------------------------------------------
    {
        const VkImageView views[9] = {
            device_->LookupImageView(in.color_in),
            device_->LookupImageView(in.depth_in),
            device_->LookupImageView(in.motion_in),
            device_->LookupImageView(in.normal_in),
            device_->LookupImageView(in.albedo_in),
            in_normal_roughness_.view,
            in_viewz_.view,
            in_mv_.view,
            in_radiance_.view,
        };
        // albedo_in may legitimately be handle 0 when the engine hasn't
        // allocated it; the layout still needs a valid view, so fall
        // back to the colour view and turn demod off below.
        VkImageView patched[9];
        for (int i = 0; i < 9; ++i) {
            patched[i] = (views[i] != VK_NULL_HANDLE) ? views[i] : views[0];
        }
        if (patched[0] == VK_NULL_HANDLE) {
            LOG_WARN("VulkanNrdLibDenoiser::Encode: color_in view lookup miss (id={})",
                     in.color_in.id);
            return;
        }

        VkDescriptorImageInfo infos[9]{};
        VkWriteDescriptorSet  writes[9]{};
        for (int i = 0; i < 9; ++i) {
            infos[i].imageView   = patched[i];
            infos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = pack_sets_[aux_slot];
            writes[i].dstBinding      = static_cast<std::uint32_t>(i);
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo      = &infos[i];
        }
        vkUpdateDescriptorSets(dev, 9, writes, 0, nullptr);

        PackPush push{};
        push.width         = w;
        push.height        = h;
        push.demod_enabled = (in.demod_enabled && views[4] != VK_NULL_HANDLE) ? 1u : 0u;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pack_pipe_);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pack_pipe_layout_,
                                0, 1, &pack_sets_[aux_slot], 0, nullptr);
        vkCmdPushConstants(cb, pack_pipe_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(push), &push);
        vkCmdDispatch(cb, gx, gy, 1);
    }
    ComputeBarrier(cb);

    // ---- 2. NRD settings ----------------------------------------------
    const bool restart = in.reset_history || force_accum_restart_;
    force_accum_restart_ = false;
    if (restart) {
        frame_index_        = 0;
        have_prev_matrices_ = false;
    }

    nrd::CommonSettings cs{};
    if (in.view_to_clip != nullptr) {
        std::memcpy(cs.viewToClipMatrix, in.view_to_clip, sizeof(cs.viewToClipMatrix));
    } else {
        cs.viewToClipMatrix[0] = cs.viewToClipMatrix[5] =
            cs.viewToClipMatrix[10] = cs.viewToClipMatrix[15] = 1.0f;
    }
    if (in.world_to_view != nullptr) {
        std::memcpy(cs.worldToViewMatrix, in.world_to_view, sizeof(cs.worldToViewMatrix));
    } else {
        cs.worldToViewMatrix[0] = cs.worldToViewMatrix[5] =
            cs.worldToViewMatrix[10] = cs.worldToViewMatrix[15] = 1.0f;
    }
    if (have_prev_matrices_) {
        std::memcpy(cs.viewToClipMatrixPrev,  prev_view_to_clip_,  sizeof(cs.viewToClipMatrixPrev));
        std::memcpy(cs.worldToViewMatrixPrev, prev_world_to_view_, sizeof(cs.worldToViewMatrixPrev));
    } else {
        std::memcpy(cs.viewToClipMatrixPrev,  cs.viewToClipMatrix,  sizeof(cs.viewToClipMatrixPrev));
        std::memcpy(cs.worldToViewMatrixPrev, cs.worldToViewMatrix, sizeof(cs.worldToViewMatrixPrev));
    }

    // motion_tex is "prev_pixel - curr_pixel" in PIXELS; NRD applies
    // "pixelUvPrev = pixelUv + mv.xy * motionVectorScale.xy" in UV, so
    // the scale is the reciprocal render size. .z stays 0 (2D motion).
    cs.motionVectorScale[0] = 1.0f / static_cast<float>(w);
    cs.motionVectorScale[1] = 1.0f / static_cast<float>(h);
    cs.motionVectorScale[2] = 0.0f;
    cs.isMotionVectorInWorldSpace = false;

    // NRD asserts jitter is inside [-0.5, 0.5]; PathTrace's Halton
    // sequence already is, but clamp so a future jitter change can't
    // trip an assert deep inside the library.
    cs.cameraJitter[0] = std::clamp(in.jitter_x, -0.5f, 0.5f);
    cs.cameraJitter[1] = std::clamp(in.jitter_y, -0.5f, 0.5f);
    cs.cameraJitterPrev[0] = have_prev_matrices_ ? prev_jitter_[0] : cs.cameraJitter[0];
    cs.cameraJitterPrev[1] = have_prev_matrices_ ? prev_jitter_[1] : cs.cameraJitter[1];

    cs.resourceSize[0]     = static_cast<std::uint16_t>(w);
    cs.resourceSize[1]     = static_cast<std::uint16_t>(h);
    cs.resourceSizePrev[0] = cs.resourceSize[0];
    cs.resourceSizePrev[1] = cs.resourceSize[1];
    cs.rectSize[0]         = cs.resourceSize[0];
    cs.rectSize[1]         = cs.resourceSize[1];
    cs.rectSizePrev[0]     = cs.resourceSize[0];
    cs.rectSizePrev[1]     = cs.resourceSize[1];

    cs.denoisingRange   = kDenoisingRange;
    cs.frameIndex       = frame_index_;
    cs.accumulationMode = restart
                              // CLEAR_AND_RESTART also zeroes the pool,
                              // which matters on frame 0 and after a
                              // resize: the permanent textures were just
                              // allocated and hold whatever the driver
                              // left in that memory.
                              ? nrd::AccumulationMode::CLEAR_AND_RESTART
                              : nrd::AccumulationMode::CONTINUE;

    nrd::Result r = nrd::SetCommonSettings(*nrd_inst_, cs);
    if (r != nrd::Result::SUCCESS) {
        if (!logged_dispatch_error_) {
            LOG_ERROR("VulkanNrdLibDenoiser: SetCommonSettings failed ({})", NrdResultStr(r));
            logged_dispatch_error_ = true;
        }
        return;
    }

    nrd::RelaxSettings rs{};
    // 1 spp path tracing produces fireflies that survive the a-trous
    // chain and then smear through temporal accumulation; RELAX's
    // anti-firefly pass is the cheap fix and is off by default.
    rs.enableAntiFirefly = true;
    // Probabilistic lobe selection means a pixel's hit distance can be
    // absent, which is what HitDistanceReconstructionMode is for -- but
    // reconstruction requires Bayer-dithered lobe selection with the
    // probability clamped into [1/4, 3/4], which this integrator does
    // not do. Leaving it OFF and letting RELAX's documented
    // "hitDist == 0" fallback handle the gaps is the honest choice.
    rs.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::OFF;
    // PRE-PASS OFF. This is the one RELAX default that had to change,
    // and it is a signal-shape mismatch rather than a taste call.
    //
    // RELAX_PrePass is a Poisson-disc spatial blur applied BEFORE
    // temporal accumulation, with radius = diffusePrepassBlurRadius *
    // hitDist / (hitDist + frustumSize). Its premise is that the noisy
    // input is INDIRECT radiance obtained by probabilistic lobe
    // selection, so hit distance is a good proxy for the signal's
    // spatial frequency: light gathered from far away is low frequency
    // and safe to pre-blur.
    //
    // This engine's megakernel does not produce that signal. One pixel
    // carries direct sun NEE from the primary hit AND the indirect
    // bounce, summed, and IN_DIFF_RADIANCE_HITDIST can only describe
    // one of them (the indirect segment -- see PathTrace.slang's
    // write_nrd_hitdist). Sky-visible pixels therefore report a very
    // large hit distance while carrying a very high-frequency direct
    // term: the sun shadow. Measured on tests/goldens/scenes/
    // cornell_csg.cfg at 512x384, the stock 30 px radius washed the
    // sphere's contact shadow out almost completely and erased the
    // metal box's top-face reflection, and it did NOT recover by frame
    // 150 -- the pre-pass runs every frame, so the damage is done
    // before accumulation can help. Rescaling the radius for the render
    // height (30 * h/1080) halved the damage but did not remove it;
    // 0 restores the shadow with no visible noise cost, because the
    // 5-iteration a-trous chain downstream is doing the spatial work
    // anyway and it IS variance-gated.
    //
    // NVIDIA document 0 as the way to disable this pass, so this is a
    // supported configuration, not a workaround. Revisit if the
    // integrator ever splits direct and indirect into separate signals
    // (then the indirect one gets a real hit distance and the pre-pass
    // premise holds), or if RELAX_DIFFUSE_SPECULAR lands.
    rs.diffusePrepassBlurRadius = 0.0f;
    r = nrd::SetDenoiserSettings(*nrd_inst_, kRelaxDiffuseId, &rs);
    if (r != nrd::Result::SUCCESS) {
        if (!logged_dispatch_error_) {
            LOG_ERROR("VulkanNrdLibDenoiser: SetDenoiserSettings failed ({})", NrdResultStr(r));
            logged_dispatch_error_ = true;
        }
        return;
    }

    // ---- 3. Replay NRD's dispatch list ---------------------------------
    const nrd::DispatchDesc* dispatches = nullptr;
    std::uint32_t dispatch_num = 0;
    const nrd::Identifier ident = kRelaxDiffuseId;
    r = nrd::GetComputeDispatches(*nrd_inst_, &ident, 1, dispatches, dispatch_num);
    if (r != nrd::Result::SUCCESS || dispatches == nullptr) {
        if (!logged_dispatch_error_) {
            LOG_ERROR("VulkanNrdLibDenoiser: GetComputeDispatches failed ({})", NrdResultStr(r));
            logged_dispatch_error_ = true;
        }
        return;
    }

    const nrd::InstanceDesc* id = nrd::GetInstanceDesc(*nrd_inst_);
    std::uint32_t cb_offset_prev = 0;

    std::vector<VkDescriptorImageInfo> infos;
    std::vector<VkWriteDescriptorSet>  writes;

    for (std::uint32_t di = 0; di < dispatch_num; ++di) {
        const nrd::DispatchDesc& dd = dispatches[di];
        if (dd.pipelineIndex >= nrd_pipelines_.size()) continue;

        const nrd::PipelineDesc& pd = id->pipelines[dd.pipelineIndex];

        infos.clear();
        writes.clear();
        infos.reserve(dd.resourcesNum);
        writes.reserve(dd.resourcesNum);

        VkDescriptorSet set = nrd_res_sets_[nrd_set_cursor_];
        nrd_set_cursor_ = (nrd_set_cursor_ + 1u) %
                          static_cast<std::uint32_t>(nrd_res_sets_.size());

        bool resource_miss = false;
        std::uint32_t n = 0;
        for (std::uint32_t ri = 0; ri < pd.resourceRangesNum && !resource_miss; ++ri) {
            const nrd::ResourceRangeDesc& range = pd.resourceRanges[ri];
            const bool is_storage = (range.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE);
            for (std::uint32_t j = 0; j < range.descriptorsNum; ++j, ++n) {
                if (n >= dd.resourcesNum) { resource_miss = true; break; }
                const nrd::ResourceDesc& rd = dd.resources[n];

                const OwnedImage* img = nullptr;
                switch (rd.type) {
                    case nrd::ResourceType::PERMANENT_POOL:
                        if (rd.indexInPool < pool_.size()) img = &pool_[rd.indexInPool];
                        break;
                    case nrd::ResourceType::TRANSIENT_POOL:
                        if (static_cast<std::size_t>(rd.indexInPool) + permanent_pool_size_ < pool_.size()) {
                            img = &pool_[rd.indexInPool + permanent_pool_size_];
                        }
                        break;
                    case nrd::ResourceType::IN_MV:                    img = &in_mv_;               break;
                    case nrd::ResourceType::IN_NORMAL_ROUGHNESS:      img = &in_normal_roughness_; break;
                    case nrd::ResourceType::IN_VIEWZ:                 img = &in_viewz_;            break;
                    case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST: img = &in_radiance_;         break;
                    case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:img = &out_radiance_;        break;
                    default:                                          img = nullptr;               break;
                }
                if (img == nullptr || img->view == VK_NULL_HANDLE) {
                    if (!logged_dispatch_error_) {
                        LOG_ERROR("VulkanNrdLibDenoiser: dispatch '{}' wants unbound resource "
                                  "type {} (indexInPool={}) -- RELAX_DIFFUSE's resource set "
                                  "changed under the integration",
                                  dd.name ? dd.name : "?", static_cast<int>(rd.type),
                                  rd.indexInPool);
                        logged_dispatch_error_ = true;
                    }
                    resource_miss = true;
                    break;
                }

                VkDescriptorImageInfo ii{};
                ii.imageView   = img->view;
                ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                infos.push_back(ii);

                VkWriteDescriptorSet wr{};
                wr.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                wr.dstSet          = set;
                wr.dstBinding      = (is_storage ? storage_binding_base_ : texture_binding_base_) + j;
                wr.descriptorCount = 1;
                wr.descriptorType  = is_storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                writes.push_back(wr);
            }
        }
        if (resource_miss) return;

        // pImageInfo has to point at storage that outlives the reserve()
        // growth above, so wire it up only once both vectors are final.
        for (std::size_t i = 0; i < writes.size(); ++i) writes[i].pImageInfo = &infos[i];
        if (!writes.empty()) {
            vkUpdateDescriptorSets(dev, static_cast<std::uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }

        // Stream this dispatch's constants, or reuse the previous slice
        // when NRD says nothing changed.
        std::uint32_t cb_offset = cb_offset_prev;
        if (dd.constantBufferDataSize != 0 && !dd.constantBufferDataMatchesPreviousDispatch) {
            cb_offset = cb_cursor_ * cb_stride_;
            cb_cursor_ = (cb_cursor_ + 1u) % cb_slots_;
            std::memcpy(static_cast<std::uint8_t*>(cb_mapped_) + cb_offset,
                        dd.constantBufferData,
                        std::min<std::uint32_t>(dd.constantBufferDataSize, cb_stride_));
            cb_offset_prev = cb_offset;
        }

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, nrd_pipelines_[dd.pipelineIndex]);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, nrd_pipe_layout_,
                                res_space_index_, 1, &set, 0, nullptr);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, nrd_pipe_layout_,
                                cb_space_index_, 1, &nrd_cb_set_, 1, &cb_offset);
        // NRD's passes chain hard (temporal reads what the pre-pass
        // wrote, a-trous ping-pongs, history clamping reads both), so
        // every dispatch needs the previous one's writes visible. One
        // global memory dependency per dispatch is the simple correct
        // answer with everything already in GENERAL.
        ComputeBarrier(cb);
        vkCmdDispatch(cb, dd.gridWidth, dd.gridHeight, 1);
    }

    ComputeBarrier(cb);

    // ---- 4. NrdUnpack --------------------------------------------------
    {
        const VkImageView out_view    = device_->LookupImageView(in.output);
        const VkImageView albedo_view = device_->LookupImageView(in.albedo_in);
        const VkImageView depth_view  = device_->LookupImageView(in.depth_in);
        const VkImageView color_view  = device_->LookupImageView(in.color_in);
        if (out_view == VK_NULL_HANDLE || depth_view == VK_NULL_HANDLE ||
            color_view == VK_NULL_HANDLE) {
            LOG_WARN("VulkanNrdLibDenoiser::Encode: unpack view lookup miss "
                     "(out={} depth={} color={})",
                     in.output.id, in.depth_in.id, in.color_in.id);
            return;
        }
        const VkImageView views[5] = {
            out_radiance_.view,
            (albedo_view != VK_NULL_HANDLE) ? albedo_view : color_view,
            depth_view,
            color_view,
            out_view,
        };
        VkDescriptorImageInfo infos5[5]{};
        VkWriteDescriptorSet  writes5[5]{};
        for (int i = 0; i < 5; ++i) {
            infos5[i].imageView   = views[i];
            infos5[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            writes5[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes5[i].dstSet          = unpack_sets_[aux_slot];
            writes5[i].dstBinding      = static_cast<std::uint32_t>(i);
            writes5[i].descriptorCount = 1;
            writes5[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes5[i].pImageInfo      = &infos5[i];
        }
        vkUpdateDescriptorSets(dev, 5, writes5, 0, nullptr);

        UnpackPush push{};
        push.width           = w;
        push.height          = h;
        push.demod_enabled   = (in.demod_enabled && albedo_view != VK_NULL_HANDLE) ? 1u : 0u;
        push.denoising_range = kDenoisingRange;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, unpack_pipe_);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, unpack_pipe_layout_,
                                0, 1, &unpack_sets_[aux_slot], 0, nullptr);
        vkCmdPushConstants(cb, unpack_pipe_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(push), &push);
        vkCmdDispatch(cb, gx, gy, 1);
    }

    // Publish this frame's camera for next frame's reprojection.
    if (in.view_to_clip  != nullptr) std::memcpy(prev_view_to_clip_,  in.view_to_clip,  sizeof(prev_view_to_clip_));
    if (in.world_to_view != nullptr) std::memcpy(prev_world_to_view_, in.world_to_view, sizeof(prev_world_to_view_));
    prev_jitter_[0] = cs.cameraJitter[0];
    prev_jitter_[1] = cs.cameraJitter[1];
    have_prev_matrices_ = true;
    ++frame_index_;
}

}  // namespace pt::rhi::vk

#endif  // PT_ENABLE_NRD
