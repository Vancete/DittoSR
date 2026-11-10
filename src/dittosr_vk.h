// DittoSR on Vulkan: the same passes as dittosr.cpp (D3D12), compiled to SPIR-V, recorded into the game's command
// buffer. Everything comes through the device the game hands the FidelityFX API (its vkGetDeviceProcAddr); nothing
// beyond Vulkan 1.1 core is assumed - no extension, no optional feature: every storage image has its format spelled
// out (the game's output through the upscale and the sharpening pass, one pipeline per output format), the stability
// levels and an RGB10A2 history are Vulkan's extended storage formats (every desktop GPU has them; checked), the
// shaders FP32.
#pragma once
#include <stdint.h>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "dittosr.h"

// One upscale on Vulkan: the game's images, the layouts they are in, and the same numbers as DittoFrame
struct DittoImageVk {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;  // (a depth format: viewed through its depth aspect)
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
};
struct DittoFrameVk {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    DittoImageVk color, depth, mv, exposure, reactive, output;
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
    float jitX = 0, jitY = 0;
    float mvToUv[2] = {0, 0};
    float mvRes[2] = {0, 0};
    bool jitteredMv = false, depthInverted = false, hdr = true;
    float exposureMul = 1.0f;
    float preExposure = 1.0f;
    float reactiveMul = 1.0f;
    bool reset = false;
};

class DittoSRVk {
public:
    DittoSRVk(VkDevice device, VkPhysicalDevice phys, PFN_vkGetDeviceProcAddr gdpa, const DittoCreateInfo &ci, const DittoSettings &s);
    ~DittoSRVk();
    bool ok() const { return ok_; }
    // Records the passes into f.cb. forceReset: the history starts again (a switch from the game's own upscaler).
    bool dispatch(const DittoFrameVk &f, bool forceReset);
    uint64_t frames() const { return frames_; }
    void set_settings(const DittoSettings &s) { s_ = s; }
    // the comparison: the output's right half left to what the game's own upscaler wrote there first
    void set_split(bool on) { split_ = on; }

private:
    struct Img {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t w = 0, h = 0;
        bool fresh = true;  // (still UNDEFINED: the first use moves it to GENERAL and clears it)
    };
    struct Garbage {  // (destroyed once the GPU is surely done with it: kRing frames later)
        uint64_t frame;
        VkImageView view;
        Img img;
    };
    bool init();
    bool makeImage(Img &t, uint32_t w, uint32_t h, VkFormat f);
    void dropImage(Img &t);
    // (false: they could not be made; *made: a new set - the history starts again)
    bool ensureTextures(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, VkFormat histFmt, bool *made);
    VkImageView gameView(const DittoImageVk &i);
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags want);
    void collect(bool all);

    VkDevice dev_;
    VkPhysicalDevice phys_;
    DittoCreateInfo ci_;
    DittoSettings s_;
    bool ok_ = false;
    struct Fns;
    Fns *fn_ = nullptr;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkPipeline psoConvert_ = VK_NULL_HANDLE, psoMotion_ = VK_NULL_HANDLE;
    // (per output format: rgba16f, rgba8, rgb10a2, r11g11b10f, rgba32f; the upscale per history format too: RGB10A2, RGBA16F)
    VkPipeline psoUpscale_[2][5] = {}, psoSharpen_[5] = {};
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sets_;
    VkBuffer ubo_ = VK_NULL_HANDLE;
    VkDeviceMemory uboMem_ = VK_NULL_HANDLE;
    uint8_t *uboMap_ = nullptr;
    uint32_t uboStride_ = 256;
    Img mda_[2], hist_[2], aux_[2], stab_[2], change1_, dummy_;
    Img expo_[2];  // (the auto exposure, 1 x 1: the game asked for it and gave none)
    bool hist10_ = false;  // (RGB10A2 is a storage format on this device: the history can be 10 bits)
    bool split_ = false;
    std::vector<Garbage> garbage_;
    int cur_ = 0;
    bool texValid_ = false;
    uint64_t frames_ = 0;
    float prevJit_[2] = {0, 0};
    bool hdrFromFormat_ = false;  // (logged: a float colour without the HDR flag)
    // the passes' GPU time (the menu's graph): two timestamps a frame into a ring, read back kTimeRing frames later
    static constexpr uint32_t kTimeRing = 8;
    VkQueryPool timeQ_ = VK_NULL_HANDLE;
    double tickMs_ = 0.0;  // (ms per timestamp tick; 0: no timing)
};

// An FSR 3.1 upscale dispatch (Vulkan backend) as a DittoFrameVk (false: a resource missing or an unknown format)
bool ditto_frame_from_ffx_vk(const struct ffxDispatchDescUpscale *d, const DittoCreateInfo &ci, DittoFrameVk *f);
