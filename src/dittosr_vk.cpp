// DittoSR on Vulkan (dittosr_vk.h), as dittosr.cpp on D3D12: convert (render size; with the change against the history
// and the auto exposure in an extra row of groups) -> motion (render size) -> upscale (output size) -> sharpen (output
// size; skipped when the upscale writes the output itself: fused, or a debug view). One descriptor set per pass, from a
// ring rewritten kRing frames later (as the D3D12 descriptor ring), the constants in a uniform buffer ring alongside.
// The internal images stay in GENERAL (cleared on their first use); the game's are moved to what the passes need and
// back to the layouts they came in.
#include "dittosr_vk.h"

#include <string.h>

#include "constants.h"
#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_upscale.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "spv_convert.h"
#include "spv_motion.h"
#include "spv_sharpen_r11g11b10f.h"
#include "spv_sharpen_rgb10a2.h"
#include "spv_sharpen_rgba16f.h"
#include "spv_sharpen_rgba32f.h"
#include "spv_sharpen_rgba8.h"
#include "spv_upscale_h10_r11g11b10f.h"
#include "spv_upscale_h10_rgb10a2.h"
#include "spv_upscale_h10_rgba16f.h"
#include "spv_upscale_h10_rgba32f.h"
#include "spv_upscale_h10_rgba8.h"
#include "spv_upscale_h16_r11g11b10f.h"
#include "spv_upscale_h16_rgb10a2.h"
#include "spv_upscale_h16_rgba16f.h"
#include "spv_upscale_h16_rgba32f.h"
#include "spv_upscale_h16_rgba8.h"

#include <initializer_list>

namespace {

constexpr uint32_t kRing = 64;          // (frames before a descriptor set, a constants slot or a view is reused)
constexpr uint32_t kSetsPerFrame = 4;   // convert, motion, upscale, sharpen
constexpr uint32_t kUbosPerFrame = 1;   // the constants
constexpr uint32_t kSampled = 8, kStorage = 3;
constexpr uint32_t kBindUbo = 0, kBindSampled = 10, kBindStorage = 20, kBindSampler = 30;  // (build.bat's -fvk-*-shift)

bool is_depth(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT ||
           f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D16_UNORM_S8_UINT;
}
bool has_stencil(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
}

// FidelityFX's surface format -> VkFormat (depth: the FidelityFX API names a depth buffer by its colour twin and
// marks it FFX_API_RESOURCE_USAGE_DEPTHTARGET)
VkFormat vk_format(uint32_t f, bool depth) {
    switch (f) {
    case FFX_API_SURFACE_FORMAT_R32G32B32A32_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R32G32B32A32_UINT: return VK_FORMAT_R32G32B32A32_UINT;
    case FFX_API_SURFACE_FORMAT_R16G16B16A16_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R32G32_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R8_UINT: return VK_FORMAT_R8_UINT;
    case FFX_API_SURFACE_FORMAT_R32_UINT: return depth ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_R32_UINT;
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case FFX_API_SURFACE_FORMAT_B8G8R8A8_TYPELESS:
    case FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
    case FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case FFX_API_SURFACE_FORMAT_R10G10B10A2_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case FFX_API_SURFACE_FORMAT_R16G16_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R16G16_UINT: return VK_FORMAT_R16G16_UINT;
    case FFX_API_SURFACE_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
    case FFX_API_SURFACE_FORMAT_R16_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R16_UINT: return VK_FORMAT_R16_UINT;
    case FFX_API_SURFACE_FORMAT_R16_UNORM: return depth ? VK_FORMAT_D16_UNORM : VK_FORMAT_R16_UNORM;
    case FFX_API_SURFACE_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
    case FFX_API_SURFACE_FORMAT_R8_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
    case FFX_API_SURFACE_FORMAT_R8G8_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
    case FFX_API_SURFACE_FORMAT_R8G8_UINT: return VK_FORMAT_R8G8_UINT;
    case FFX_API_SURFACE_FORMAT_R32_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R32_FLOAT: return depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R32_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}

// A float colour format: values above 1 (HDR)
bool float_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R32G32B32_SFLOAT:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: return true;
    default: return false;
    }
}

// the sharpening pipeline for an output format (-1: none - a format DittoSR cannot write)
int sharpen_variant(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 0;
    case VK_FORMAT_R8G8B8A8_UNORM: return 1;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return 2;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return 3;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 4;
    default: return -1;
    }
}

// FidelityFX's resource state -> the layout the image is in
VkImageLayout vk_layout(uint32_t s, bool depth) {
    switch (s) {
    case FFX_API_RESOURCE_STATE_COMPUTE_READ:
    case FFX_API_RESOURCE_STATE_PIXEL_READ:
    case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ:
        return depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case FFX_API_RESOURCE_STATE_COPY_SRC: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case FFX_API_RESOURCE_STATE_COPY_DEST: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case FFX_API_RESOURCE_STATE_PRESENT: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    case FFX_API_RESOURCE_STATE_RENDER_TARGET: return depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    default: return VK_IMAGE_LAYOUT_GENERAL;  // (UNORDERED_ACCESS, COMMON, GENERIC_READ)
    }
}

}  // namespace

struct DittoSRVk::Fns {
#define F(n) PFN_##n n = nullptr;
    F(vkCreateImage) F(vkDestroyImage) F(vkCreateImageView) F(vkDestroyImageView) F(vkGetImageMemoryRequirements)
    F(vkAllocateMemory) F(vkFreeMemory) F(vkBindImageMemory) F(vkCreateBuffer) F(vkDestroyBuffer)
    F(vkGetBufferMemoryRequirements) F(vkBindBufferMemory) F(vkMapMemory) F(vkUnmapMemory) F(vkCreateShaderModule)
    F(vkDestroyShaderModule) F(vkCreateDescriptorSetLayout) F(vkDestroyDescriptorSetLayout) F(vkCreatePipelineLayout)
    F(vkDestroyPipelineLayout) F(vkCreateComputePipelines) F(vkDestroyPipeline) F(vkCreateDescriptorPool)
    F(vkDestroyDescriptorPool) F(vkAllocateDescriptorSets) F(vkUpdateDescriptorSets) F(vkCreateSampler)
    F(vkDestroySampler) F(vkCmdBindPipeline) F(vkCmdBindDescriptorSets) F(vkCmdDispatch) F(vkCmdPipelineBarrier)
    F(vkDeviceWaitIdle) F(vkCmdClearColorImage) F(vkCreateQueryPool) F(vkDestroyQueryPool) F(vkCmdResetQueryPool)
    F(vkCmdWriteTimestamp) F(vkGetQueryPoolResults)
    // (instance-level, through the loader's exports: the same physical device handle)
    F(vkGetPhysicalDeviceMemoryProperties) F(vkGetPhysicalDeviceProperties) F(vkGetPhysicalDeviceFormatProperties)
#undef F
    VkPhysicalDeviceMemoryProperties mem = {};
};

DittoSRVk::DittoSRVk(VkDevice device, VkPhysicalDevice phys, PFN_vkGetDeviceProcAddr gdpa, const DittoCreateInfo &ci,
                     const DittoSettings &s)
    : dev_(device), phys_(phys), ci_(ci), s_(s) {
    fn_ = new Fns();
    bool all = true;
#define F(n) if (!(fn_->n = (PFN_##n)gdpa(dev_, #n))) ditto_log("vulkan: no %s\n", #n), all = false;
    F(vkCreateImage) F(vkDestroyImage) F(vkCreateImageView) F(vkDestroyImageView) F(vkGetImageMemoryRequirements)
    F(vkAllocateMemory) F(vkFreeMemory) F(vkBindImageMemory) F(vkCreateBuffer) F(vkDestroyBuffer)
    F(vkGetBufferMemoryRequirements) F(vkBindBufferMemory) F(vkMapMemory) F(vkUnmapMemory) F(vkCreateShaderModule)
    F(vkDestroyShaderModule) F(vkCreateDescriptorSetLayout) F(vkDestroyDescriptorSetLayout) F(vkCreatePipelineLayout)
    F(vkDestroyPipelineLayout) F(vkCreateComputePipelines) F(vkDestroyPipeline) F(vkCreateDescriptorPool)
    F(vkDestroyDescriptorPool) F(vkAllocateDescriptorSets) F(vkUpdateDescriptorSets) F(vkCreateSampler)
    F(vkDestroySampler) F(vkCmdBindPipeline) F(vkCmdBindDescriptorSets) F(vkCmdDispatch) F(vkCmdPipelineBarrier)
    F(vkDeviceWaitIdle) F(vkCmdClearColorImage) F(vkCreateQueryPool) F(vkDestroyQueryPool) F(vkCmdResetQueryPool)
    F(vkCmdWriteTimestamp) F(vkGetQueryPoolResults)
#undef F
    HMODULE vk = GetModuleHandleA("vulkan-1.dll");
    if (!vk) vk = LoadLibraryA("vulkan-1.dll");
    if (vk) {
        fn_->vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)GetProcAddress(vk, "vkGetPhysicalDeviceMemoryProperties");
        fn_->vkGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)GetProcAddress(vk, "vkGetPhysicalDeviceProperties");
        fn_->vkGetPhysicalDeviceFormatProperties = (PFN_vkGetPhysicalDeviceFormatProperties)GetProcAddress(vk, "vkGetPhysicalDeviceFormatProperties");
    }
    if (!fn_->vkGetPhysicalDeviceMemoryProperties) ditto_log("vulkan: vulkan-1.dll's vkGetPhysicalDeviceMemoryProperties missing\n"), all = false;
    ok_ = all && init();
}

DittoSRVk::~DittoSRVk() {
    if (!fn_) return;
    if (fn_->vkDeviceWaitIdle) fn_->vkDeviceWaitIdle(dev_);  // (the game destroys the context with the GPU idle; to be sure)
    if (ok_ || fn_->vkDestroyImage) {
        collect(true);
        for (Img *t : {&mda_[0], &mda_[1], &hist_[0], &hist_[1], &aux_[0], &aux_[1], &stab_[0], &stab_[1], &expo_[0], &expo_[1],
                       &change1_, &dummy_})
            dropImage(*t);
        collect(true);
        std::vector<VkPipeline> ps = {psoConvert_, psoMotion_};
        for (auto &h : psoUpscale_) ps.insert(ps.end(), h, h + 5);
        ps.insert(ps.end(), psoSharpen_, psoSharpen_ + 5);
        for (VkPipeline p : ps) if (p) fn_->vkDestroyPipeline(dev_, p, nullptr);
        if (timeQ_) fn_->vkDestroyQueryPool(dev_, timeQ_, nullptr);
        if (pipeLayout_) fn_->vkDestroyPipelineLayout(dev_, pipeLayout_, nullptr);
        if (setLayout_) fn_->vkDestroyDescriptorSetLayout(dev_, setLayout_, nullptr);
        if (pool_) fn_->vkDestroyDescriptorPool(dev_, pool_, nullptr);
        if (sampler_) fn_->vkDestroySampler(dev_, sampler_, nullptr);
        if (uboMap_) fn_->vkUnmapMemory(dev_, uboMem_);
        if (ubo_) fn_->vkDestroyBuffer(dev_, ubo_, nullptr);
        if (uboMem_) fn_->vkFreeMemory(dev_, uboMem_, nullptr);
    }
    delete fn_;
}

uint32_t DittoSRVk::memoryType(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < fn_->mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (fn_->mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

bool DittoSRVk::init() {
    fn_->vkGetPhysicalDeviceMemoryProperties(phys_, &fn_->mem);
    if (fn_->vkGetPhysicalDeviceProperties) {
        VkPhysicalDeviceProperties p = {};
        fn_->vkGetPhysicalDeviceProperties(phys_, &p);
        ditto_log("GPU: %s (vendor 0x%04x, Vulkan %u.%u)\n", p.deviceName, p.vendorID, VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion));
        uint32_t a = (uint32_t)p.limits.minUniformBufferOffsetAlignment;
        if (a > uboStride_) uboStride_ = a;
        // (the timestamps: every graphics and compute queue has them where timestampComputeAndGraphics - the game's
        // queue is not ours to ask)
        if (p.limits.timestampComputeAndGraphics && p.limits.timestampPeriod > 0.0f) tickMs_ = p.limits.timestampPeriod * 1e-6;
    }
    // the extended storage formats the passes write: the stability levels (R8G8), the history RGB10A2 (else RGBA16F)
    auto storage = [&](VkFormat f) {
        if (!fn_->vkGetPhysicalDeviceFormatProperties) return true;
        VkFormatProperties fp = {};
        fn_->vkGetPhysicalDeviceFormatProperties(phys_, f, &fp);
        return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    };
    if (!storage(VK_FORMAT_R8G8_UNORM)) return ditto_log("vulkan: R8G8 is not a storage format on this GPU (the stability levels)\n"), false;
    hist10_ = storage(VK_FORMAT_A2B10G10R10_UNORM_PACK32);
    if (!hist10_) ditto_log("vulkan: RGB10A2 is not a storage format on this GPU - the history RGBA16F\n");
    VkSamplerCreateInfo sc = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = sc.minFilter = VK_FILTER_LINEAR, sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sc.maxLod = VK_LOD_CLAMP_NONE;
    if (fn_->vkCreateSampler(dev_, &sc, nullptr, &sampler_) != VK_SUCCESS) return ditto_log("vulkan: vkCreateSampler failed\n"), false;
    // the set: the constants, 8 sampled images, 3 storage images, the sampler (build.bat's register shifts)
    VkDescriptorSetLayoutBinding b[1 + kSampled + kStorage + 1] = {};
    uint32_t n = 0;
    b[n++] = {kBindUbo, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    for (uint32_t i = 0; i < kSampled; i++) b[n++] = {kBindSampled + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    for (uint32_t i = 0; i < kStorage; i++) b[n++] = {kBindStorage + i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[n++] = {kBindSampler, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &sampler_};
    VkDescriptorSetLayoutCreateInfo lc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lc.bindingCount = n, lc.pBindings = b;
    if (fn_->vkCreateDescriptorSetLayout(dev_, &lc, nullptr, &setLayout_) != VK_SUCCESS) return ditto_log("vulkan: set layout failed\n"), false;
    VkPipelineLayoutCreateInfo plc = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plc.setLayoutCount = 1, plc.pSetLayouts = &setLayout_;
    if (fn_->vkCreatePipelineLayout(dev_, &plc, nullptr, &pipeLayout_) != VK_SUCCESS) return ditto_log("vulkan: pipeline layout failed\n"), false;
    struct { const unsigned char *code; size_t size; VkPipeline *pso; const char *name; } cs[] = {
        {g_spv_convert, sizeof g_spv_convert, &psoConvert_, "convert"},
        {g_spv_motion, sizeof g_spv_motion, &psoMotion_, "motion"},
        {g_spv_upscale_h10_rgba16f, sizeof g_spv_upscale_h10_rgba16f, &psoUpscale_[0][0], "upscale h10 rgba16f"},
        {g_spv_upscale_h10_rgba8, sizeof g_spv_upscale_h10_rgba8, &psoUpscale_[0][1], "upscale h10 rgba8"},
        {g_spv_upscale_h10_rgb10a2, sizeof g_spv_upscale_h10_rgb10a2, &psoUpscale_[0][2], "upscale h10 rgb10a2"},
        {g_spv_upscale_h10_r11g11b10f, sizeof g_spv_upscale_h10_r11g11b10f, &psoUpscale_[0][3], "upscale h10 r11g11b10f"},
        {g_spv_upscale_h10_rgba32f, sizeof g_spv_upscale_h10_rgba32f, &psoUpscale_[0][4], "upscale h10 rgba32f"},
        {g_spv_upscale_h16_rgba16f, sizeof g_spv_upscale_h16_rgba16f, &psoUpscale_[1][0], "upscale h16 rgba16f"},
        {g_spv_upscale_h16_rgba8, sizeof g_spv_upscale_h16_rgba8, &psoUpscale_[1][1], "upscale h16 rgba8"},
        {g_spv_upscale_h16_rgb10a2, sizeof g_spv_upscale_h16_rgb10a2, &psoUpscale_[1][2], "upscale h16 rgb10a2"},
        {g_spv_upscale_h16_r11g11b10f, sizeof g_spv_upscale_h16_r11g11b10f, &psoUpscale_[1][3], "upscale h16 r11g11b10f"},
        {g_spv_upscale_h16_rgba32f, sizeof g_spv_upscale_h16_rgba32f, &psoUpscale_[1][4], "upscale h16 rgba32f"},
        {g_spv_sharpen_rgba16f, sizeof g_spv_sharpen_rgba16f, &psoSharpen_[0], "sharpen rgba16f"},
        {g_spv_sharpen_rgba8, sizeof g_spv_sharpen_rgba8, &psoSharpen_[1], "sharpen rgba8"},
        {g_spv_sharpen_rgb10a2, sizeof g_spv_sharpen_rgb10a2, &psoSharpen_[2], "sharpen rgb10a2"},
        {g_spv_sharpen_r11g11b10f, sizeof g_spv_sharpen_r11g11b10f, &psoSharpen_[3], "sharpen r11g11b10f"},
        {g_spv_sharpen_rgba32f, sizeof g_spv_sharpen_rgba32f, &psoSharpen_[4], "sharpen rgba32f"},
    };
    for (auto &c : cs) {
        if (c.pso >= &psoUpscale_[0][0] && c.pso < &psoUpscale_[1][0] && !hist10_) continue;  // (no RGB10A2 history here)
        std::vector<uint32_t> words((c.size + 3) / 4);
        memcpy(words.data(), c.code, c.size);
        VkShaderModuleCreateInfo mc = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mc.codeSize = c.size, mc.pCode = words.data();
        VkShaderModule m;
        if (fn_->vkCreateShaderModule(dev_, &mc, nullptr, &m) != VK_SUCCESS) return ditto_log("vulkan: shader %s failed\n", c.name), false;
        VkComputePipelineCreateInfo pc = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pc.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
        pc.layout = pipeLayout_;
        VkResult r = fn_->vkCreateComputePipelines(dev_, VK_NULL_HANDLE, 1, &pc, nullptr, c.pso);
        fn_->vkDestroyShaderModule(dev_, m, nullptr);
        if (r != VK_SUCCESS) return ditto_log("vulkan: pipeline %s failed (%d)\n", c.name, (int)r), false;
    }
    // the descriptor ring and the constants ring
    uint32_t sets = kRing * kSetsPerFrame;
    VkDescriptorPoolSize ps[4] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sets}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sets * kSampled},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sets * kStorage}, {VK_DESCRIPTOR_TYPE_SAMPLER, sets}};
    VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = sets, pci.poolSizeCount = 4, pci.pPoolSizes = ps;
    if (fn_->vkCreateDescriptorPool(dev_, &pci, nullptr, &pool_) != VK_SUCCESS) return ditto_log("vulkan: descriptor pool failed\n"), false;
    sets_.resize(sets);
    std::vector<VkDescriptorSetLayout> layouts(sets, setLayout_);
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_, ai.descriptorSetCount = sets, ai.pSetLayouts = layouts.data();
    if (fn_->vkAllocateDescriptorSets(dev_, &ai, sets_.data()) != VK_SUCCESS) return ditto_log("vulkan: descriptor sets failed\n"), false;
    VkBufferCreateInfo bc = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size = (VkDeviceSize)uboStride_ * kRing * kUbosPerFrame, bc.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (fn_->vkCreateBuffer(dev_, &bc, nullptr, &ubo_) != VK_SUCCESS) return ditto_log("vulkan: buffer failed\n"), false;
    VkMemoryRequirements mr;
    fn_->vkGetBufferMemoryRequirements(dev_, ubo_, &mr);
    VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = mr.size, ma.memoryTypeIndex = memoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ma.memoryTypeIndex == UINT32_MAX || fn_->vkAllocateMemory(dev_, &ma, nullptr, &uboMem_) != VK_SUCCESS)
        return ditto_log("vulkan: buffer memory failed\n"), false;
    fn_->vkBindBufferMemory(dev_, ubo_, uboMem_, 0);
    if (fn_->vkMapMemory(dev_, uboMem_, 0, VK_WHOLE_SIZE, 0, (void **)&uboMap_) != VK_SUCCESS) return ditto_log("vulkan: map failed\n"), false;
    // (a 1 x 1 image for the sampled bindings a pass leaves empty)
    if (!makeImage(dummy_, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT)) return false;
    if (tickMs_ > 0.0) {
        VkQueryPoolCreateInfo qc = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qc.queryType = VK_QUERY_TYPE_TIMESTAMP, qc.queryCount = 2 * kTimeRing;
        if (fn_->vkCreateQueryPool(dev_, &qc, nullptr, &timeQ_) != VK_SUCCESS) timeQ_ = VK_NULL_HANDLE, tickMs_ = 0.0;
    }
    return true;
}

bool DittoSRVk::makeImage(Img &t, uint32_t w, uint32_t h, VkFormat f) {
    VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D, ic.format = f, ic.extent = {w, h, 1}, ic.mipLevels = 1, ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT, ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (fn_->vkCreateImage(dev_, &ic, nullptr, &t.image) != VK_SUCCESS) return ditto_log("vulkan: image %ux%u failed\n", w, h), false;
    VkMemoryRequirements mr;
    fn_->vkGetImageMemoryRequirements(dev_, t.image, &mr);
    VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = mr.size, ma.memoryTypeIndex = memoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ma.memoryTypeIndex == UINT32_MAX) ma.memoryTypeIndex = memoryType(mr.memoryTypeBits, 0);
    if (fn_->vkAllocateMemory(dev_, &ma, nullptr, &t.mem) != VK_SUCCESS) return ditto_log("vulkan: image memory failed\n"), false;
    fn_->vkBindImageMemory(dev_, t.image, t.mem, 0);
    VkImageViewCreateInfo vc = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = t.image, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = f;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (fn_->vkCreateImageView(dev_, &vc, nullptr, &t.view) != VK_SUCCESS) return ditto_log("vulkan: image view failed\n"), false;
    t.format = f, t.w = w, t.h = h, t.fresh = true;
    return true;
}

void DittoSRVk::dropImage(Img &t) {
    if (t.image || t.view) garbage_.push_back({frames_, VK_NULL_HANDLE, t});
    t = Img();
}

void DittoSRVk::collect(bool all) {
    size_t keep = 0;
    for (size_t i = 0; i < garbage_.size(); i++) {
        Garbage &g = garbage_[i];
        if (!all && g.frame + kRing > frames_) { garbage_[keep++] = g; continue; }
        if (g.view) fn_->vkDestroyImageView(dev_, g.view, nullptr);
        if (g.img.view) fn_->vkDestroyImageView(dev_, g.img.view, nullptr);
        if (g.img.image) fn_->vkDestroyImage(dev_, g.img.image, nullptr);
        if (g.img.mem) fn_->vkFreeMemory(dev_, g.img.mem, nullptr);
    }
    garbage_.resize(keep);
}

bool DittoSRVk::ensureTextures(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, VkFormat histFmt, bool *made) {
    *made = false;
    if (texValid_ && mda_[0].w == rw && mda_[0].h == rh && hist_[0].w == ow && hist_[0].h == oh && hist_[0].format == histFmt) return true;
    *made = true;
    for (Img *t : {&mda_[0], &mda_[1], &hist_[0], &hist_[1], &aux_[0], &aux_[1], &stab_[0], &stab_[1], &expo_[0], &expo_[1], &change1_})
        dropImage(*t);
    const VkFormat F16 = VK_FORMAT_R16G16B16A16_SFLOAT, S = VK_FORMAT_R8G8_UNORM, R32 = VK_FORMAT_R32_SFLOAT;
    texValid_ = makeImage(mda_[0], rw, rh, F16) && makeImage(mda_[1], rw, rh, F16) && makeImage(hist_[0], ow, oh, histFmt) &&
                makeImage(hist_[1], ow, oh, histFmt) && makeImage(aux_[0], rw, rh, VK_FORMAT_R8G8B8A8_UNORM) &&
                makeImage(aux_[1], rw, rh, VK_FORMAT_R8G8B8A8_UNORM) && makeImage(stab_[0], ow, oh, S) && makeImage(stab_[1], ow, oh, S) &&
                makeImage(expo_[0], 1, 1, R32) && makeImage(expo_[1], 1, 1, R32) && makeImage(change1_, 1, 1, R32);
    if (texValid_)
        ditto_log("textures: render %ux%u, output %ux%u, history %s, stability R8G8 (Vulkan)\n", rw, rh, ow, oh,
                  histFmt == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ? "RGB10A2" : "RGBA16F");
    return texValid_;
}

VkImageView DittoSRVk::gameView(const DittoImageVk &i) {
    if (!i.image) return VK_NULL_HANDLE;
    VkImageViewCreateInfo vc = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = i.image, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = i.format;
    vc.subresourceRange = {is_depth(i.format) ? (VkImageAspectFlags)VK_IMAGE_ASPECT_DEPTH_BIT : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (fn_->vkCreateImageView(dev_, &vc, nullptr, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    garbage_.push_back({frames_, v, Img()});  // (the view lives kRing frames: the command buffer runs well before)
    return v;
}

bool ditto_frame_from_ffx_vk(const ffxDispatchDescUpscale *d, const DittoCreateInfo &ci, DittoFrameVk *f) {
    *f = DittoFrameVk();
    f->cb = (VkCommandBuffer)d->commandList;
    auto img = [](const FfxApiResource &r, DittoImageVk &o) {
        if (!r.resource) return;
        bool depth = (r.description.usage & FFX_API_RESOURCE_USAGE_DEPTHTARGET) != 0;
        o.image = (VkImage)r.resource, o.format = vk_format(r.description.format, depth), o.layout = vk_layout(r.state, depth);
    };
    img(d->color, f->color), img(d->depth, f->depth), img(d->motionVectors, f->mv), img(d->exposure, f->exposure);
    img(d->reactive, f->reactive), img(d->output, f->output);
    if (!f->cb || !f->color.image || !f->mv.image || !f->output.image) return false;
    if (!f->color.format || !f->mv.format || !f->output.format || (f->depth.image && !f->depth.format)) return false;
    f->rw = d->renderSize.width ? d->renderSize.width : d->color.description.width;
    f->rh = d->renderSize.height ? d->renderSize.height : d->color.description.height;
    f->ow = d->upscaleSize.width ? d->upscaleSize.width : ci.maxUpscaleW;
    f->oh = d->upscaleSize.height ? d->upscaleSize.height : ci.maxUpscaleH;
    if (!f->ow || !f->oh) f->ow = d->output.description.width, f->oh = d->output.description.height;
    f->jitX = d->jitterOffset.x, f->jitY = d->jitterOffset.y;
    bool displayMv = (ci.flags & FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) != 0;
    f->mvRes[0] = displayMv ? (float)f->ow : (float)f->rw, f->mvRes[1] = displayMv ? (float)f->oh : (float)f->rh;
    f->mvToUv[0] = d->motionVectorScale.x / f->mvRes[0], f->mvToUv[1] = d->motionVectorScale.y / f->mvRes[1];
    f->jitteredMv = (ci.flags & FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION) != 0;
    f->depthInverted = (ci.flags & FFX_UPSCALE_ENABLE_DEPTH_INVERTED) != 0;
    f->hdr = (ci.flags & FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE) != 0;
    f->preExposure = d->preExposure > 0.0f ? d->preExposure : 1.0f;
    f->reset = d->reset;
    return true;
}

bool DittoSRVk::dispatch(const DittoFrameVk &fr, bool forceReset) {
    if (!ok_ || !fr.cb || !fr.rw || !fr.rh || !fr.ow || !fr.oh) return false;
    int variant = sharpen_variant(fr.output.format);
    if (variant < 0) {
        static int n;
        if (n++ < 3) ditto_log("vulkan: the output's format %d cannot be written (rgba16f, rgba8, rgb10a2, r11g11b10f, rgba32f)\n", (int)fr.output.format);
        return false;
    }
    collect(false);
    uint32_t rw = fr.rw, rh = fr.rh, ow = fr.ow, oh = fr.oh;
    // (the history RGB10A2 where the device writes it - half the bandwidth -, as on D3D12; else RGBA16F)
    VkFormat hfmt = s_.history10 && hist10_ ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_R16G16B16A16_SFLOAT;
    bool resized;
    if (!ensureTextures(rw, rh, ow, oh, hfmt, &resized)) return false;
    bool reset = fr.reset || forceReset || resized;
    VkCommandBuffer cb = fr.cb;

    // the game's images: views, then read as sampled images (in their read-only or general layout) and the output as
    // a storage image (GENERAL)
    VkImageView vColor = gameView(fr.color), vDepth = gameView(fr.depth), vMv = gameView(fr.mv), vExpo = gameView(fr.exposure),
                vReac = gameView(fr.reactive), vOut = gameView(fr.output);
    if (!vColor || !vMv || !vOut) return ditto_log("vulkan: a view of the game's images failed\n"), false;
    struct Use { const DittoImageVk *i; VkImageLayout to; } uses[6];
    int nu = 0;
    auto use = [&](const DittoImageVk &i, bool storage) {
        if (!i.image) return;
        VkImageLayout to = i.layout;
        if (storage) to = VK_IMAGE_LAYOUT_GENERAL;
        else if (to != VK_IMAGE_LAYOUT_GENERAL && to != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && to != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)
            to = is_depth(i.format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        uses[nu++] = {&i, to};
    };
    use(fr.color, false), use(fr.depth, false), use(fr.mv, false), use(fr.exposure, false), use(fr.reactive, false), use(fr.output, true);
    Img *internal[] = {&mda_[0], &mda_[1], &hist_[0], &hist_[1], &aux_[0], &aux_[1], &stab_[0], &stab_[1], &expo_[0], &expo_[1], &change1_, &dummy_};
    auto barrier = [&](bool in) {
        VkImageMemoryBarrier ib[6 + sizeof internal / sizeof internal[0]] = {};
        uint32_t n = 0;
        for (int i = 0; i < nu; i++) {
            VkImageLayout from = in ? uses[i].i->layout : uses[i].to, to = in ? uses[i].to : uses[i].i->layout;
            if (from == to) continue;
            VkImageMemoryBarrier &b = ib[n++];
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            b.oldLayout = from, b.newLayout = to;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            VkFormat f = uses[i].i->format;
            VkImageAspectFlags asp = is_depth(f) ? (VkImageAspectFlags)(VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil(f) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
                                                 : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT;
            b.image = uses[i].i->image, b.subresourceRange = {asp, 0, 1, 0, 1};
        }
        bool fresh = false;
        if (in)  // (the internal images' first use: UNDEFINED -> GENERAL, then cleared)
            for (Img *t : internal) {
                if (!t->fresh) continue;
                VkImageMemoryBarrier &b = ib[n++];
                b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b.srcAccessMask = 0, b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = t->image, b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                fresh = true;
            }
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        fn_->vkCmdPipelineBarrier(cb, in ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  in ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                  0, 1, &mb, 0, nullptr, n, ib);
        if (!fresh) return;
        // (zeros: the stability levels and the exposure read before anything wrote them start from nothing, not garbage)
        VkClearColorValue zero = {};
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (Img *t : internal)
            if (t->fresh) fn_->vkCmdClearColorImage(cb, t->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all), t->fresh = false;
        VkMemoryBarrier cm = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        cm.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, cm.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        fn_->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &cm, 0, nullptr, 0, nullptr);
    };
    barrier(true);
    // (the passes' GPU time: the slot written kTimeRing frames ago read - not ready yet: skipped -, this frame's started)
    uint32_t ts = (uint32_t)(frames_ % kTimeRing);
    if (timeQ_) {
        uint64_t t2[2] = {};
        if (frames_ >= kTimeRing &&
            fn_->vkGetQueryPoolResults(dev_, timeQ_, 2 * ts, 2, sizeof t2, t2, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
            t2[1] > t2[0] && (t2[1] - t2[0]) * tickMs_ < 1000.0)
            g_stats.upscaleMs = (float)((t2[1] - t2[0]) * tickMs_);
        fn_->vkCmdResetQueryPool(cb, timeQ_, 2 * ts, 2);
        fn_->vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timeQ_, 2 * ts);
    }

    // the constants: this frame's slot of the ring
    DittoConstants k;
    // (the auto exposure: the game set FSR's flag and gave no exposure - measured in the convert pass, its 1 x 1 the
    // exposure the passes read)
    bool autoExpo = !vExpo && (ci_.flags & FFX_UPSCALE_ENABLE_AUTO_EXPOSURE) && rw >= 16;
    ditto_fill_constants(k, s_, fr, reset, vExpo != VK_NULL_HANDLE || autoExpo, vReac != VK_NULL_HANDLE,
                         hfmt == VK_FORMAT_A2B10G10R10_UNORM_PACK32, prevJit_);
    k.autoExposure = autoExpo ? 1.0f : 0.0f;
    if (split_) k.splitX = (float)(ow / 2);
    // (the sharpening in the upscale, its own pass skipped - not with the comparison, nor a debug view; up to 2.5x, as
    // on D3D12)
    k.fusedSharpen = !split_ && !s_.debugView && s_.fusedSharpen > 0.0f && k.ratio <= 2.55f ? s_.fusedSharpen : 0.0f;
    // (a float colour declared LDR is read as HDR: as [0, 1] colour it would be clipped at 1)
    if (!fr.hdr && float_format(fr.color.format)) {
        if (!hdrFromFormat_) ditto_log("colour: float format %d without the HDR flag - read as HDR\n", (int)fr.color.format), hdrFromFormat_ = true;
        k.hdr = 1.0f;
    }
    uint32_t slot = (uint32_t)(frames_ % kRing);
    VkDeviceSize u0 = (VkDeviceSize)uboStride_ * (slot * kUbosPerFrame);
    memcpy(uboMap_ + u0, &k, sizeof k);

    cur_ ^= 1;
    Img &mdaC = mda_[cur_], &mdaP = mda_[cur_ ^ 1], &histC = hist_[cur_], &histP = hist_[cur_ ^ 1];
    Img &auxC = aux_[cur_], &auxP = aux_[cur_ ^ 1], &stabC = stab_[cur_], &stabP = stab_[cur_ ^ 1];
    Img &expoC = expo_[cur_], &expoP = expo_[cur_ ^ 1];
    uint32_t setIndex = slot * kSetsPerFrame;
    // one pass: its set written (sampled images - the dummy where none -, storage images, constants), then dispatched
    auto run = [&](VkPipeline pso, const VkImageView sampled[kSampled], VkImageLayout const sampledLayout[kSampled],
                   const VkImageView storage[kStorage], VkDeviceSize ubo, uint32_t gx, uint32_t gy) {
        VkDescriptorSet set = sets_[setIndex++];
        VkDescriptorImageInfo si[kSampled], ui[kStorage];
        VkDescriptorBufferInfo bi = {ubo_, ubo, sizeof(DittoConstants)};
        VkWriteDescriptorSet w[1 + kSampled + kStorage] = {};
        uint32_t n = 0;
        w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, kBindUbo, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &bi, nullptr};
        n++;
        for (uint32_t i = 0; i < kSampled; i++) {
            bool has = sampled[i] != VK_NULL_HANDLE;
            si[i] = {VK_NULL_HANDLE, has ? sampled[i] : dummy_.view, has ? sampledLayout[i] : VK_IMAGE_LAYOUT_GENERAL};
            w[n++] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, kBindSampled + i, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &si[i], nullptr, nullptr};
        }
        for (uint32_t i = 0; i < kStorage; i++) {
            if (!storage[i]) continue;
            ui[i] = {VK_NULL_HANDLE, storage[i], VK_IMAGE_LAYOUT_GENERAL};
            w[n++] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, kBindStorage + i, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ui[i], nullptr, nullptr};
        }
        fn_->vkUpdateDescriptorSets(dev_, n, w, 0, nullptr);
        fn_->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pso);
        fn_->vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1, &set, 0, nullptr);
        fn_->vkCmdDispatch(cb, gx, gy, 1);
    };
    auto compute_barrier = [&]() {
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        fn_->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    };
    auto layout_of = [&](const DittoImageVk &i) {
        for (int u = 0; u < nu; u++) if (uses[u].i == &i) return uses[u].to;
        return VK_IMAGE_LAYOUT_GENERAL;
    };
    const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL;
    VkImageLayout lColor = layout_of(fr.color), lDepth = layout_of(fr.depth), lMv = layout_of(fr.mv), lReac = layout_of(fr.reactive);
    // the exposure: the game's, or the auto exposure's - the convert pass reads the last frame's and measures this one's
    // into expoC (bound always: the shader's u2), the later passes read that
    VkImageView eIn = vExpo ? vExpo : autoExpo ? expoP.view : VK_NULL_HANDLE, e = vExpo ? vExpo : autoExpo ? expoC.view : VK_NULL_HANDLE;
    VkImageLayout lE = vExpo ? layout_of(fr.exposure) : G;

    // convert (render size), and in its extra row the change against the history (first group) and the auto exposure
    // (second): t0 depth, t1 motion, t2 colour, t3 previous history, t5 exposure -> u0 mda, u1 change, u2 exposure
    {
        VkImageView s[kSampled] = {vDepth, vMv, vColor, histP.view, VK_NULL_HANDLE, eIn, VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkImageLayout l[kSampled] = {lDepth, lMv, lColor, G, G, lE, G, G};
        VkImageView u[kStorage] = {mdaC.view, change1_.view, expoC.view};
        run(psoConvert_, s, l, u, u0, (rw + 7) / 8, (rh + 7) / 8 + 1);
    }
    compute_barrier();
    // motion (render size): t0 previous aux, t1 mda, t2 colour, t3 previous mda -> u0 aux
    {
        VkImageView s[kSampled] = {auxP.view, mdaC.view, vColor, mdaP.view, VK_NULL_HANDLE, e, VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkImageLayout l[kSampled] = {G, G, lColor, G, G, lE, G, G};
        VkImageView u[kStorage] = {auxC.view, VK_NULL_HANDLE, VK_NULL_HANDLE};
        run(psoMotion_, s, l, u, u0, (rw + 7) / 8, (rh + 7) / 8);
    }
    compute_barrier();
    // upscale (output size): t0 previous history, t1 mda, t2 colour, t3 aux, t4 change, t6 reactive, t7 previous
    // stability -> u0 history, u1 the game's output (fused, or a debug view), u2 stability
    {
        VkImageView s[kSampled] = {histP.view, mdaC.view, vColor, auxC.view, change1_.view, e, vReac, stabP.view};
        VkImageLayout l[kSampled] = {G, G, lColor, G, G, lE, lReac, G};
        VkImageView u[kStorage] = {histC.view, vOut, stabC.view};
        run(psoUpscale_[hfmt == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ? 0 : 1][variant], s, l, u, u0, (ow + 15) / 16, (oh + 7) / 8);  // (upscale.hlsl: 16 x 8 groups)
    }
    // sharpen (output size; the game's output): t0 history, t1 mda, t7 stability -> u1 output (a debug view or fused:
    // the upscale wrote it)
    if (!s_.debugView && k.fusedSharpen <= 0.0f) {
        compute_barrier();
        VkImageView s[kSampled] = {histC.view, mdaC.view, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, e, VK_NULL_HANDLE, stabC.view};
        VkImageLayout l[kSampled] = {G, G, G, G, G, lE, G, G};
        VkImageView u[kStorage] = {VK_NULL_HANDLE, vOut, VK_NULL_HANDLE};
        run(psoSharpen_[variant], s, l, u, u0, (ow + 7) / 8, (oh + 7) / 8);
    }
    // the game's images back to their layouts, the output's writes done first
    barrier(false);
    if (timeQ_) fn_->vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timeQ_, 2 * ts + 1);
    frames_++;
    return true;
}
