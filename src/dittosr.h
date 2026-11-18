// DittoSR on D3D12: the convert, motion, upscale and sharpening passes recorded into the game's command list. The inputs are
// described API-neutrally (DittoFrame, in FSR 3.1's conventions); the FidelityFX API front end fills it.
#pragma once
#include <d3d12.h>
#include <stdint.h>

#include "ffx_api/ffx_upscale.h"

// the version both front ends report (the FidelityFX API provider, the libxess.dll proxy)
#define DITTOSR_VERSION "0.5.0"
#define DITTOSR_VERSION_NAME "DittoSR " DITTOSR_VERSION

struct DittoSettings {
    // a stable pixel's share of each new frame at rest: some 30 frames of history - what stands still is steady. What
    // moves with no motion vector of its own (grass swaying in its vertex shader) would show where it had been all that
    // time: the envelope below finds it. (0.035 until 0.4.2; 0.15 for a while after, for that grass: the still image shimmered)
    float alphaRest = 0.1f;
    float conf = 0.05f;        // the confidence the narrow estimate is taken alone from (originally 0.3)
    float gammaRest = 3.0f;    // the history clip width at rest (1.5 until 0.4.2; 0 in the ini: the default)
    float width = 0.8f;        // the narrow Gaussian's width from 2x (x 0.875 at 1.5x and below; originally 1.4 - at 2.3x a blur as wide as a grass blade)
    float react = 1.0f;        // the reaction to changes
    float reactiveScale = 1.0f;
    float exposureScale = 1.0f;
    float jitterSignX = 1.0f, jitterSignY = 1.0f;
    float mvSign = 1.0f;
    float motionGamma = 1.5f;  // the history clip width while moving (originally 0.9)
    float motionShare = 0.15f;  // the new frame's base share while moving (originally 0.3)
    float motionMinMax = 1.0f; // how far the box is held within the samples' min / max while moving
    float sharpness = 0.6f;    // at rest 0 soft, 0.6 as reconstructed, 1 sharpened; moving, sharpened by it (sharpen.hlsl)
    // the sharpening's filter: 0 CAS; 1 RCAS (FSR 1's robust CAS: one weight for the 3 channels, less on noise) - the
    // sharpening pass then runs at every ratio (no fused sharpening)
    int sharpenMode = 0;
    // EXPERIMENTAL (D3D12), Ditto Relight: ambient occlusion and a one-bounce global illumination after CORTAO
    // (aomain.hlsl), applied to the colour before the upscale - the occlusion in full, the bounce light x 2; their reach:
    // aoRadius view units (0: no limit in the world), at most aoScreen of the screen's height; aoRays rays a pixel (4 - 32); aoContact:
    // how much more the rays blocked by the nearest occluders darken (0 - 4: contact shadows)
    bool relight = false;
    float aoRadius = 0.0f, aoScreen = 0.5f, aoRays = 8.0f, aoContact = 2.0f;
    // ... the occlusion's strength (0 - 1), the bounce light's (0 - 4), and the bounce light x giDark where the pixel is
    // dark (0 - 4: 1 the same everywhere; above, the shadows lifted more by what surrounds them; below, kept darker)
    float aoStrength = 1.0f, giStrength = 2.0f, giDark = 0.5f;
    // ... a curve on the lights (0 - 1; 0 off): the luma from 30 % of the brightness up brighter - starting flat, at most
    // sCurve stops; below, as it is
    float sCurve = 0.3f;
    // ... the temporal smoothing (FastGI's; aotemporal.hlsl): the AO and GI blended with their history (reprojected
    // by the game's motion vectors) x aoTemporalWeight (0 - 0.95), less in motion - Ditto's AO and GI at render size,
    // FastGI's GI at its own resolution
    bool aoTemporal = true;
    float aoTemporalWeight = 0.85f;
    // ... the GI: 0 Ditto's (the occluders' colour), 1 FastGI's (aofg*.hlsl; giStrength and giDark unused) - the AO the
    // same. FastGI's options, its defaults (its units: the game's view units - FastGI's metres): its resolution's
    // divisor (2 Half, 3 Third, 4 Quarter), rays (1 - 8), steps (4 - 32), intensity (0 - 4), radius, thickness,
    // radiance clamp, max distance (1 - 60000), diffuse reflectance (0 - 1), refine hits, hit rejection (0 - 1), spatial
    // radius (1 - 2), normal bias (ini only, as FastGI hides it)
    int giMode = 0;
    int fgDiv = 2;
    float fgRays = 4.0f, fgSteps = 8.0f, fgIntensity = 0.6f, fgRadius = 3.0f, fgThickness = 0.25f, fgClamp = 4.0f;
    float fgMaxDist = 60.0f, fgReflectance = 0.5f, fgRejection = 0.75f, fgSpatial = 1.0f;
    float fgNormalBias = 0.03f;
    bool fgRefine = true;
    float frameWeight = 2.0f;  // x the new frame's share, 0.25 - 4: lower, more persistence (smoother, blurrier in motion)
    // the frame weight above wins over the one FSR 3.1's velocity factor and reactiveness scale set (OptiScaler's
    // presets and sliders); false: those set it whenever they are not both at 1
    bool frameWeightOverride = false;
    float changeScale = 1.0f;  // how far the share of the image that changed drives the reaction
    // the new frame's share x shareFar where no jittered sample landed close to the pixel, x shareNear where one did
    // (originally 0.6 / 1.4 - at 2.3x most frames have no sample close: their blurred estimate blurred the history)
    // (shareFar 0.05: the pixels no sample landed near all but hold their history. What moves with no motion vector
    // then shows new in the pixels a sample reached and old in the others, a comb along its edges - The Legend of Zelda:
    // Ocarina of Time's grass at 2x, the camera still (its Longshot port) - which the envelope below takes care of; at
    // 1, tried for it, a pan's sharpness fell from 0.77 to 0.47 of the reference's and its shimmer tripled)
    float shareFar = 0.2f;  // (0.05 until 0.4.2 and a while after; 0.2 by eye, with the rest share 0.1 and the rest clip 3; x 0.5 by 3x - constants.h)
    float shareNear = 3.0f;
    float encodePower = 0.5f;  // the encoding's power after Reinhard (0.5: sqrt)
    // while moving (from 4 output pixels a frame) shareNear falls to motionNear: each frame's pattern of pixels a sample
    // landed near shimmers
    float motionNear = 1.5f;
    // the history's clip on its low frequencies alone (the 5 taps' mean): the current samples' aliasing in fine detail
    // no longer drags the history's accumulated detail toward it - only a real change, far out of the box, drops it
    float splitClip = 1.0f;
    // the history RGB10A2 (half the bandwidth of RGBA16F: the upscale reads and writes it at output size, the sharpening
    // reads it again - what an integrated GPU's shared memory feels most), its stores dithered; 0: RGBA16F
    int history10 = 1;
    // the FP16 shaders (the colour work in half precision), where the GPU has native 16-bit math: 1 always, -1 on AMD
    // only, 0 never - the default: on the ROG Ally they were slower than FP32 (2x 0.1 ms), on an RTX 5070 Ti ~7 %
    int fp16 = 0;
    // the reactive mask derived where the game gives none (particles, transparencies, flashes: more of the new frame),
    // its strength; 0: off
    float autoReactive = 0.5f;  // (1 until 0.3.0: at 0.5 the image gets sharp again a little sooner after a turn)
    // the history's stability level: the step a pixel climbs each frame a sample lands near it (1: stable). Kept in its
    // own R8 texture (the RGB10A2 history's alpha has 2 bits; R8G8 with the anti-shimmer's state, always on Vulkan)
    float stabStep = 0.34f;
    // the anti-shimmer (0 off; -1, the default: 0.4 up to 2x, 0.2 from 3x): where the sign of the new frame's
    // difference from the history keeps alternating
    // (the jittered samples' aliasing), up to this share less of the new frame. Less shimmer (-10..-25 %), a little
    // softer fine detail in motion. The stability texture becomes R8G8 (the sign's running mean in .y)
    // (-1: by the ratio, 0.6 up to 2x and 0.45 from 3x - 0.4 and 0.2 until 0.4.2: the base share is higher in slow
    // motion now, alphaSlow)
    float antiShimmer = -1.0f;
    // the history's detail made up for its resampling blur in motion (upscale.hlsl): x its default strength (0.03 up to
    // 2x, 0.05 from 3x); 0 off
    float historySharpen = 1.0f;
    // the sharpening in the upscale, its own pass skipped (up to 2.5x - above it the separate pass): the gain
    // of the detail term; 0 the separate pass. On the ROG Ally 0.3 ms less at 1080p 2x
    float fusedSharpen = 1.0f;
    // the envelope (motion.hlsl): each render pixel's luma range over the last frames - a min / max that narrows by
    // envDecay of its width a frame, carried where the pixel went. A static pixel's jittered samples bounce within it,
    // however aliased; a luma outside it by more than envTol + envRel x its width is something new there - what moved
    // with no motion vector of its own, arriving - and raises the reactive mask by envGain per unit, up to envCap (0.5,
    // half of it from 3x: what it lets in is a coarser estimate there), at rest only (envRest; fading out from half an
    // output pixel a frame). envGain 0: off. Harness, 2x, thick blades swaying, the camera still: 0.31 of a blade's
    // contrast shown against 0.20 without it, the still grass's shimmer 1.12 against 1.10
    float envTol = 0.03f, envRel = 0.25f, envDecay = 0.05f, envGain = 40.0f, envCap = 0.5f, envRest = 1.0f;
    // the base share in slow motion: alphaSlow from rampLo to rampHi output pixels a frame (then on to the moving share
    // by 8) - a slow pan's history is resampled every frame, and more of the new frame keeps it sharp (the tiles at 1.8
    // px a frame, 2x: 30.5 -> 31.6 dB); alphaSlow 0: one ramp from the rest share to the moving one over rampLo..rampHi
    float alphaSlow = 0.15f, rampLo = 0.25f, rampHi = 1.0f;
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
    bool reset = false;
    float fovY = 0, zNear = 0, zFar = 0;  // the camera (FSR 3.1's dispatch; 0: unknown) - the experimental AO / GI
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
    // the comparison: the output's right half left to what the game's own upscaler wrote there first
    void set_split(bool on) { split_ = on; }

private:
    struct Tex {
        ID3D12Resource *res = nullptr;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        uint32_t w = 0, h = 0;
    };
    bool createPipelines();
    // (false: they could not be made; *made: a new set - the history starts again)
    bool ensureTextures(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, DXGI_FORMAT historyFormat, DXGI_FORMAT stabFormat, bool *made);
    void releaseTextures();
    bool makeTex(Tex &t, uint32_t w, uint32_t h, DXGI_FORMAT f);
    void transition(ID3D12GraphicsCommandList *cl, Tex &t, D3D12_RESOURCE_STATES to);
    D3D12_GPU_DESCRIPTOR_HANDLE table(ID3D12Resource *const *srv, const DXGI_FORMAT *srvFmt, ID3D12Resource *const *uav,
                                      const DXGI_FORMAT *uavFmt);

    ID3D12Device *dev_ = nullptr;
    DittoCreateInfo ci_;
    DittoSettings s_;
    bool ok_ = false;
    ID3D12RootSignature *rootSig_ = nullptr;
    ID3D12PipelineState *psoConvert_ = nullptr, *psoMotion_ = nullptr, *psoUpscale_ = nullptr, *psoSharpen_ = nullptr;
    ID3D12PipelineState *psoUpscale16_ = nullptr, *psoSharpen16_ = nullptr;  // (null: no native 16-bit math)
    ID3D12PipelineState *psoUpscaleAs_ = nullptr, *psoUpscaleAs16_ = nullptr;  // (the anti-shimmer variants)
    // (the 1x variants, the motion pass fused in: [fp16][anti-shimmer]; null: the motion pass as at other ratios)
    ID3D12PipelineState *psoUpscaleFm_[2][2] = {};
    ID3D12PipelineState *psoAoPrep_ = nullptr, *psoAoMain_ = nullptr, *psoAoTemporal_ = nullptr, *psoAoComp_ = nullptr;  // (EXPERIMENTAL: Ditto Relight)
    ID3D12PipelineState *psoAoFgTrace_ = nullptr, *psoAoFgBlur_ = nullptr, *psoAoFgTemporal_ = nullptr;  // (... FastGI's GI)
    bool lastFp16_ = false;
    bool hdrFromFormat_ = false;  // (logged: a float colour without the HDR flag)
    // the game's output last checked (outputWritable): its format, whether it allows unordered access, and the verdict
    DXGI_FORMAT outFmt_ = DXGI_FORMAT_UNKNOWN;
    bool outUav_ = false, outOk_ = false;
    bool outputWritable(const D3D12_RESOURCE_DESC &od);
    bool amd_ = false;  // (the device's vendor is AMD: fp16 = -1 picks the FP16 shaders)
    ID3D12DescriptorHeap *heap_ = nullptr;
    uint32_t heapSize_ = 0, heapNext_ = 0, descInc_ = 0;
    Tex mda_[2], hist_[2], aux_[2], stab_[2], change1_, env_[2];
    Tex expo_[2];  // (the auto exposure, 1 x 1: the game asked for it and gave none)
    Tex aoNd_, aoRes_, aoCol_;  // (EXPERIMENTAL, Ditto Relight: normal and depth, occlusion and bounce, the colour)
    // (... their temporal smoothing's history and its depth, ping-ponged (aoCur_ this frame's); aoOk_: valid, aoLast_:
    // the frame it was written)
    Tex aoH_[2], aoHD_[2];
    int aoCur_ = 0;
    bool aoOk_ = false;
    uint64_t aoLast_ = ~0ull;
    // (... FastGI's GI at its low resolution: traced and filtered, the filter's scratch, the temporal history (written:
    // fgCur_); fgOk_: the history valid, fgLast_: the frame it was written)
    Tex aoFg_, aoFg2_, aoFgH_[2];
    int fgCur_ = 0;
    bool fgOk_ = false;
    uint64_t fgLast_ = ~0ull;
    bool split_ = false;
    int cur_ = 0;
    bool texValid_ = false;
    uint64_t frames_ = 0;
    float prevJit_[2] = {0, 0};
    // the passes' GPU time (the menu's graph): five timestamps a frame (the start, after the convert, the motion and
    // the upscale passes, the end) into a ring, read back kTimeRing frames later; log_passes=1: each pass's mean logged
    static constexpr uint32_t kTimeRing = 8, kStamps = 5;
    double passMs_[4] = {0, 0, 0, 0};
    uint32_t passN_ = 0;
    ID3D12QueryHeap *timeQ_ = nullptr;
    ID3D12Resource *timeRb_ = nullptr;
    const uint64_t *timeMap_ = nullptr;
    double tickMs_ = 0.0;  // (ms per timestamp tick; 0: no timing)
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
    bool logPasses = false;  // (log_passes=1: each pass's GPU time, its mean every 256 frames)
    // the comparison's key (0: none - the in-game menu switches it): the game's own upscaler (AMD's FSR, Intel's XeSS) on
    // the output's right half, DittoSR on the left; it needs the game's DLL alongside (<name>.orig.dll, libxess_orig.dll)
    int compareKey = 0;
    int menuKey = 0x76;  // (F7: the in-game menu - overlay.cpp)
    bool compare = false;  // ([dittosr] compare=1: from the start)
    // the ratios the FidelityFX API answers the quality modes with: quality, balanced, performance, ultra performance
    // (0: FSR 3.1's 1.5, 1.7, 2, 3) - games that ask, not those that work them out themselves
    float ratio[4] = {0, 0, 0, 0};
    // the sharpness the FidelityFX API's dispatch carries (when it enables sharpening) in place of dittosr.ini's: -1 only
    // when this DLL runs from OptiScaler's custom provider folder (its sharpness slider), 1 always, 0 never
    int gameSharpness = -1;
    DittoSettings s;
};
extern DittoConfig g_cfg;
extern char g_dir[260];  // (this DLL's folder, with the trailing backslash)
void ditto_init(void *module, const char *logName);
// the reload key pressed this frame, or the menu changed the settings: g_cfg read again from dittosr.ini (enabled and
// the keys included) or the menu's settings taken
bool ditto_poll_reload();
// The settings, shared with the menu (it runs on the game's present thread): a copy under a lock
DittoSettings ditto_settings();
void ditto_set_settings(const DittoSettings &s);  // (the menu: taken by the next upscale)
void ditto_request_reload();                      // (the menu: dittosr.ini read again by the next upscale)
bool ditto_save_settings();                       // (the menu: written to dittosr.ini - only what differs from the defaults)
// Applied over dittosr.ini's settings each time it is read (null: none) - OptiScaler's [Plugin.<name>], opti_plugin.cpp
extern void (*g_settingsOverride)(DittoSettings &s);
// The override's side of the menu's Save (null: none): s's value of key (dittosr.ini's name) put where the override
// keeps it, when it holds that key - true: dittosr.ini's left alone; key null: what it took, written out
extern bool (*g_settingsSave)(const DittoSettings &s, const char *key);

// What the upscale shows the menu (written each upscale, read on the present thread: single values, no lock)
struct DittoStats {
    volatile float upscaleMs = 0.0f;  // (DittoSR's passes on the GPU, a few frames late; 0: unknown)
    volatile uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
    volatile int api = 0;  // 1 the FidelityFX API (AMD's FSR), 2 XeSS
    volatile bool active = false, compare = false, hasReal = false;
    volatile int64_t lastQpc = 0;  // (QueryPerformanceCounter at the last upscale)
    // the frame weight FSR 3.1's velocity factor and reactiveness scale set in place of the settings' (OptiScaler's
    // presets and sliders; -1: none, the settings' own)
    volatile float fsrFrameWeight = -1.0f;
};
extern DittoStats g_stats;
// The menu's requests, taken by the next upscale: the other upscaler (the toggle), the comparison
extern volatile long g_reqToggle, g_reqCompare;
void ditto_stats_update(int api, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, bool active, bool compare, bool hasReal);
// The in-game menu (overlay.cpp), once per process. D3D12: DXGI's Present hooked, the menu drawn into the game's frame.
// Vulkan: a layered window of its own over the game's, drawn on the CPU; the upscale ticks its frame times.
void ditto_overlay_install(ID3D12Device *device);
void ditto_overlay_install_window();
void ditto_overlay_tick();
