// DittoSR on D3D12: the convert, change and upscale passes recorded into the game's command list. The inputs are
// described API-neutrally (DittoFrame, in FSR 3.1's conventions); the FidelityFX API front end fills it.
#pragma once
#include <d3d12.h>
#include <stdint.h>

#include "ffx_api/ffx_upscale.h"

// the version both front ends report (the FidelityFX API provider, the libxess.dll proxy)
#define DITTOSR_VERSION_NAME "DittoSR 0.2.4"

struct DittoSettings {
    float alphaRest = 0.035f;  // a stable pixel's share of each new frame at rest
    float conf = 0.05f;        // the confidence the narrow estimate is taken alone from (originally 0.3)
    float gammaRest = 0.0f;    // the history clip width at rest (0: 1.5)
    float width = 0.8f;        // the narrow Gaussian's width (originally 1.4 - at 2.3x a blur as wide as a grass blade)
    float react = 1.0f;        // the reaction to changes
    float edge = 1.0f;         // the edge-directed Gaussian
    float lockLines = 0.0f;    // thin lines kept
    float reactiveScale = 1.0f;
    float exposureScale = 1.0f;
    float jitterSignX = 1.0f, jitterSignY = 1.0f;
    float mvSign = 1.0f;
    float fovHDeg = 80.0f;     // the horizontal field of view when the API gives none, for the depth clip
    float motionGamma = 1.5f;  // the history clip width while moving (originally 0.9)
    float motionShare = 0.15f;  // the new frame's base share while moving (originally 0.3)
    float motionMinMax = 1.0f; // how far the box is held within the samples' min / max while moving
    int depthMode = 0;         // 0: disocclusion by the previous frame's depth; 1: GSR 2's depth clip
    float sharpness = 0.6f;    // at rest 0 soft, 0.6 as reconstructed, 1 sharpened; moving, sharpened by it (sharpen.hlsl)
    float frameWeight = 2.0f;  // x the new frame's share, 0.25 - 4: lower, more persistence (smoother, blurrier in motion)
    float changeScale = 1.0f;  // how far the share of the image that changed drives the reaction
    // the new frame's share x shareFar where no jittered sample landed close to the pixel, x shareNear where one did
    // (originally 0.6 / 1.4 - at 2.3x most frames have no sample close: their blurred estimate blurred the history)
    float shareFar = 0.05f;
    float shareNear = 3.0f;
    float encodePower = 0.5f;  // the encoding's power after Reinhard (0.5: sqrt)
    // while moving (from 4 output pixels a frame) the history holds little: the narrow Gaussian widens to motionWidth
    // (the colour only: the samples' confidence stays the narrow one's) - else the render samples' grid shows as a
    // stipple. And shareNear falls to motionNear: each frame's pattern of pixels a sample landed near shimmers
    float motionWidth = 1.6f;
    float motionNear = 1.5f;
    // the history's clip on its low frequencies alone (the 5 taps' mean): the current samples' aliasing in fine detail
    // no longer drags the history's accumulated detail toward it - only a real change, far out of the box, drops it
    float splitClip = 1.0f;
    // the history RGB10A2 (half the bandwidth of RGBA16F: the upscale reads and writes it at output size, the sharpening
    // reads it again - what an integrated GPU's shared memory feels most), its stores dithered; 0: RGBA16F
    int history10 = 1;
    // the FP16 shaders (the colour work in half precision), where the GPU has native 16-bit math: -1 on AMD only
    // (packed half math at twice the rate; on an RTX 5070 Ti they were ~7 % slower than FP32), 1 always, 0 never
    int fp16 = -1;
    bool indicator = false;
    int debugView = 0;  // 1: the motion; 2: the new frame's share; 3: the stability level; 4: clip / cut / confidence
};

// What the context was created with (ffxCreateContextDescUpscale)
struct DittoCreateInfo {
    uint32_t flags;  // FfxApiCreateContextUpscaleFlags
    uint32_t maxRenderW, maxRenderH, maxUpscaleW, maxUpscaleH;
};

// One upscale: the game's resources with the states they are in, and how to read them
struct DittoFrame {
    ID3D12GraphicsCommandList *cl = nullptr;
    ID3D12Resource *color = nullptr, *depth = nullptr, *mv = nullptr, *exposure = nullptr, *reactive = nullptr, *output = nullptr;
    D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          mvState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, exposureState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          reactiveState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0;  // render and output size
    float jitX = 0, jitY = 0;                 // FSR's convention: the sample at p + 0.5 stands for p + 0.5 - jitter (render px)
    float mvToUv[2] = {0, 0};                 // the previous uv = uv + mv x mvToUv
    float mvRes[2] = {0, 0};                  // the motion vectors' resolution over the image (render or output size)
    bool jitteredMv = false, depthInverted = false, hdr = true;
    float exposureMul = 1.0f;  // x the exposure texture (or alone), for the encoding
    float preExposure = 1.0f;
    float reactiveMul = 1.0f;  // x the reactive mask
    float fovV = 0.0f;         // vertical field of view (radians), 0: unknown
    bool reset = false;
};

class DittoSR {
public:
    DittoSR(ID3D12Device *device, const DittoCreateInfo &ci, const DittoSettings &s);
    ~DittoSR();
    bool ok() const { return ok_; }
    // Records the passes into f.cl. forceReset: the history starts again (a switch from the game's own upscaler).
    bool dispatch(const DittoFrame &f, bool forceReset);
    uint64_t frames() const { return frames_; }
    void set_settings(const DittoSettings &s) { s_ = s; }

private:
    struct Tex {
        ID3D12Resource *res = nullptr;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        uint32_t w = 0, h = 0;
    };
    bool createPipelines();
    bool ensureTextures(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, DXGI_FORMAT historyFormat);
    void releaseTextures();
    bool makeTex(Tex &t, uint32_t w, uint32_t h, DXGI_FORMAT f);
    void transition(ID3D12GraphicsCommandList *cl, Tex &t, D3D12_RESOURCE_STATES to);
    D3D12_GPU_DESCRIPTOR_HANDLE table(ID3D12Resource *const srv[7], const DXGI_FORMAT srvFmt[7], ID3D12Resource *const uav[2],
                                      const DXGI_FORMAT uavFmt[2]);

    ID3D12Device *dev_ = nullptr;
    DittoCreateInfo ci_;
    DittoSettings s_;
    bool ok_ = false;
    ID3D12RootSignature *rootSig_ = nullptr;
    ID3D12PipelineState *psoConvert_ = nullptr, *psoChange_ = nullptr, *psoUpscale_ = nullptr, *psoSharpen_ = nullptr;
    ID3D12PipelineState *psoUpscale16_ = nullptr, *psoSharpen16_ = nullptr;  // (null: no native 16-bit math)
    bool lastFp16_ = false;
    bool hdrFromFormat_ = false;  // (logged: a float colour without the HDR flag)
    bool amd_ = false;  // (the device's vendor is AMD: fp16 = -1 picks the FP16 shaders)
    ID3D12DescriptorHeap *heap_ = nullptr;
    uint32_t heapSize_ = 0, heapNext_ = 0, descInc_ = 0;
    Tex mda_[2], hist_[2], change4_, change1_;
    int cur_ = 0;
    bool texValid_ = false;
    uint64_t frames_ = 0;
    float prevJit_[2] = {0, 0};
};

// An FSR 3.1 upscale dispatch as a DittoFrame (false: a resource missing)
bool ditto_frame_from_ffx(const ffxDispatchDescUpscale *d, const DittoCreateInfo &ci, DittoFrame *f);

// The FSR 3.1 formulas for the upscale queries the game asks before creating anything
float ditto_ratio_from_quality(uint32_t q);
int32_t ditto_jitter_phase_count(uint32_t renderWidth, uint32_t displayWidth);
void ditto_jitter_offset(int32_t index, int32_t phaseCount, float *x, float *y);

// common.cpp: the log (dittosr-<name>.log next to the DLL) and dittosr.ini
void ditto_log(const char *fmt, ...);
struct DittoConfig {
    bool enabled = true;  // false: the game's own upscaler, untouched
    bool debug = false;   // [debug] enabled: the keys below and the log in full
    int toggleKey = 0x79;
    int reloadKey = 0x78;  // (F9: dittosr.ini read again while playing)
    int logDispatches = 3;
    DittoSettings s;
};
extern DittoConfig g_cfg;
extern char g_dir[260];  // (this DLL's folder, with the trailing backslash)
void ditto_init(void *module, const char *logName);
// the reload key pressed this frame: g_cfg read again from dittosr.ini (enabled and the keys included)
bool ditto_poll_reload();
