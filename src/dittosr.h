// DittoSR on D3D12: the convert, motion, upscale and sharpening passes recorded into the game's command list. The inputs are
// described API-neutrally (DittoFrame, in FSR 3.1's conventions); the FidelityFX API front end fills it.
#pragma once
#include <d3d12.h>
#include <stdint.h>

#include <vector>

#include "ffx_api/ffx_upscale.h"

// the version both front ends report (the FidelityFX API provider, the libxess.dll proxy)
#define DITTOSR_VERSION "0.5.4"
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
    // sharpening pass then runs at every ratio (no fused sharpening); 2 off (no sharpening, no pass: the upscale writes
    // the output)
    int sharpenMode = 0;
    // EXPERIMENTAL (D3D12 and Vulkan), Ditto Relight: ambient occlusion and a one-bounce global illumination after CORTAO
    // (aomain.hlsl), applied to the colour before the upscale - the occlusion x 0.75, the bounce light x 2; their reach:
    // aoRadius view units (0: no limit in the world), at most aoScreen of the screen's height; aoRays rays a pixel
    // (4 - 32); aoContact: how much more the rays blocked by the nearest occluders darken (0 - 4: contact shadows)
    bool relight = false;
    // frame generation (the game's or OptiScaler's FSR 3.1 one, AMD's DLL alongside): AMD's in-between images in place of
    // DittoSR's (the menu's Frame generation row; ini frame_gen_amd)
    bool fgAmd = false;
    float aoRadius = 0.0f, aoScreen = 0.5f, aoContact = 2.0f;
    // ... the occlusion's strength (0 - 1), the bounce light's (0 - 4), and the bounce light x giDark where the pixel is
    // dark (0 - 4: 1 the same everywhere; above, the shadows lifted more by what surrounds them; below, kept darker)
    float aoStrength = 0.75f, giStrength = 2.0f, giDark = 1.0f;
    // ... the bounce light's ceiling (0 - 4; 0 none): added at most giCeiling x the pixel's own colour, saturating
    // smoothly - a dark material no longer veiled by what a bright surface bounces onto it (aocompose.hlsl)
    float giCeiling = 2.0f;
    // ... CORTAO's ideas (tests; off: as before): its filter weights (each tap's depth against the centre's plane, its
    // normal, pairs symmetric), its sparse filter (one pass, 6 pairs) in place of FastGI's, depth mips (the far samples
    // from a sub-sampled chain), the AO as a power (pow(occlusion, aoStrength) in place of a lerp), backface boost
    bool aoMips = true, aoBackBoost = true;
    // ... more of CORTAO's (tests): its occluder lookup table (32 rays a pixel, the table built on the GPU for
    // DittoSR's rays), the main pass in 16 x 16 groups (CORTAO's cache advice), colour mips for the GI's far occluders
    // ... the main pass' samples a pixel (4 - 32: fewer, noisier; more, slower), and the main pass at half size (one
    // pixel of each 2 x 2 a frame, brought back by depth and normal - about a quarter of its cost)
    float aoSamples = 16.0f;
    bool aoHalf = true;
    // ... with it (D3D12; the menu's Resolution): 0 half (the same pixel of each 2 x 2), 2 checkerboard (every other pixel,
    // alternating, spread to render size - aorecon.hlsl). (Tried and removed: 1 half rotating - a different pixel of each
    // 2 x 2 each frame: its cost for half's look in motion)
    int aoResMode = 0;
    // ... the checkerboard cheaper (D3D12; the menu's toggles - off): aoCkGiHalf the GI on half its pixels (the others the
    // history's), aoCkFuse its fill done by the sparse filter (no pass of its own); aoFull2Pass the unified filter in 2
    // passes at render size (spacing 2 and 6, not 2, 4, 8)
    bool aoCkGiHalf = false, aoCkFuse = false, aoFull2Pass = false;  // (FastGI's: the filter and the temporal smoothing at half size too, the result brought back at the end)
    // ... decorrelated noise (each random dimension of the main pass its own IGN moved along its own direction a frame:
    // no fixed pattern - the bands of light a thin light left down a wall) - a test
    // ... FidelityFX CACAO's edge-sensitive blur after the temporal smoothing (aocacao.hlsl; D3D12): on the accumulated AO
    // and GI at the main pass' size, the history left as it is - aoCacaoPasses of it (1 - 8; CACAO's default 2) - a test
    bool aoCacao = true;
    // ... (D3D12) the unified filter: the a-trous filter on the AO and the GI together after the temporal smoothing, in place
    // of the GI's a-trous and CACAO (the menu's toggle)
    bool aoUnified = true;
    int aoCacaoPasses = 2;
    // ... (D3D12, on by default - ini keys) aoPackNd the normal and depth packed (8 bytes a texel in place of 16: the
    // octahedral normal, the depth); aoUpCompose the half size's upsample in the composition (FastGI's: no render-size
    // pass between); aoFuse passes fused (the 4 depth mips in one, the half size's normal and depth by the main pass,
    // CACAO's 2 passes in one); aoHistLen the temporal smoothing's weight by its history's length (n / (n + 1) up to
    // 0.94: fast where it starts over, long where it has held - the harness' corner flicker 0.26 -> 0.18). Tried and
    // removed: the sparse filter from groupshared memory (slower), adaptive samples (fewer where the history is long:
    // the thin light's bands back, no time saved)
    // ... on by default (also the menu's toggles under Relight): aoAmbientOnly the AO on the ambient light alone (GTAO's
    // multi-bounce, faded on bright pixels); aoEmitterCos each occluder's light x (1 + the cosine of its surface toward
    // the pixel) / 2; aoBetterNormals the prepass' normals by Turanszki's 2-pixel extrapolation. (Tried and removed:
    // occluder thickness, a variance clip, a motion-tolerant history, a history-guided blur)
    bool aoEmitterCos = true, aoBetterNormals = true;
    // ... the GI steadier (on by default - the menu's toggles under Relight): aoGiWide a wider spatial filter for the GI
    // (3 pairs 10 pixels out; the AO's kept: the harness' GI flicker -24 %); aoGiLong a longer history for the GI (up to 31 frames: -18 %).
    // (Tried and removed: the GI's colour from the colour mips, its anti-firefly)
    bool aoGiWide = true;
    // ... (D3D12) aoHalfPrep the prepass at half size (with half resolution and the mips: no render-size normal and depth);
    // aoCurveLut the curve from a baked table (4096 entries over [0, 16]) in place of its spline a pixel and channel
    // ... on by default: aoNoise8 FastGI's noise cycle, the main pass' noise the same every 8
    // frames (the frames' mean a fixed one); aoGiFalloff FastGI's falloff, each occluder's light x (1 - (d / reach)^2);
    // aoFadeMeters (100, the menu's) the AO faded out from that many metres to twice that (0: none; the camera's metres a unit from FSR
    // 3.1's dispatch, 1 where it gives none)
    bool aoGiFalloff = true;
    // ... (D3D12) aoBlueNoise the main pass' noise from a 64 x 64 blue noise mask stepped by the golden ratio a frame, in
    // place of IGN (and of the decorrelated noise, Vulkan's). (Tried and removed: samples guided toward bright areas,
    // screen probes and their directional variant)
    bool aoBlueNoise = true;
    // ... (D3D12; on by default) aoEdgeFill the GI made up for
    // the samples off the screen, aoAtrous an a-trous filter on the GI (luma edge-stopping; the menu's toggle), 
    // aoGiInterleave the GI on half the pixels a frame (the rest the last result's), aoFarCheap half
    // the samples past twice the AO's fade distance. (Tried and removed: the better albedo, ReSTIR GI, stochastic reflections, multi-bounce, sky light, a source clamp, the mips
    // in groupshared memory)
    bool aoEdgeFill = true, aoAtrous = true, aoGiInterleave = true, aoFarCheap = true;
    // ... the local exposure (D3D12; 0 off - the menu's slider, 0.5; aocompose.hlsl)
    float fxLocalExp = 0.5f;
    float aoFadeMeters = 100.0f;
    // ... the curve (the menu's curves editor; ini curve = "x,y x,y ..."): curveN points (2 - 8) by x in [0, 1], each
    // channel through their natural cubic spline in a perceptual [0, 1] (aocompose.hlsl) - the line: none. By default
    // a darker low end, the mids slightly lifted (the curve tuned in Assetto Corsa EVO)
    int curveN = 5;
    float curveX[8] = {0.0f, 0.1195f, 0.2516f, 0.4858f, 1.0f}, curveY[8] = {0.0f, 0.08962f, 0.261f, 0.522f, 1.0f};
    // ... their noise evened out after FastGI's: a spatial filter (aofilter.hlsl: 5 taps x then y, by depth and normal)
    // its taps aoSpatial x 2 pixels apart (1 - 2), then a temporal smoothing (aotemporal.hlsl: their history, reprojected
    // by the game's motion vectors, x aoTemporalWeight (0 - 0.95), less in motion)
    float aoSpatial = 1.0f;
    bool aoTemporal = true;
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

// The curve's spline (the menu draws it, the composition applies it): the natural cubic spline's second derivatives
// y2 at the n points (x increasing); the curve at t, flat before and after its ends, in [0, 1]; the line: none
inline bool ditto_curve_line(const DittoSettings &s) {
    return s.curveN < 2 || (s.curveN == 2 && s.curveX[0] == 0.0f && s.curveY[0] == 0.0f && s.curveX[1] == 1.0f && s.curveY[1] == 1.0f);
}
inline void ditto_curve_y2(int n, const float *x, const float *y, float *y2) {
    float u[8] = {};
    y2[0] = 0.0f, u[0] = 0.0f;
    for (int i = 1; i < n - 1; i++) {
        float sig = (x[i] - x[i - 1]) / (x[i + 1] - x[i - 1]), p = sig * y2[i - 1] + 2.0f;
        y2[i] = (sig - 1.0f) / p;
        float d = (y[i + 1] - y[i]) / (x[i + 1] - x[i]) - (y[i] - y[i - 1]) / (x[i] - x[i - 1]);
        u[i] = (6.0f * d / (x[i + 1] - x[i - 1]) - sig * u[i - 1]) / p;
    }
    y2[n - 1] = 0.0f;
    for (int k = n - 2; k >= 0; k--) y2[k] = y2[k] * y2[k + 1] + u[k];
}
inline float ditto_curve_at(int n, const float *x, const float *y, const float *y2, float t) {
    if (t <= x[0]) return y[0];
    if (t >= x[n - 1]) return y[n - 1];
    int k = 0;
    for (int i = 1; i + 1 < n; i++)
        if (t >= x[i]) k = i;
    float h = x[k + 1] - x[k] > 1e-5f ? x[k + 1] - x[k] : 1e-5f, a = (x[k + 1] - t) / h, b = (t - x[k]) / h;
    float v = a * y[k] + b * y[k + 1] + ((a * a * a - a) * y2[k] + (b * b * b - b) * y2[k + 1]) * h * h / 6.0f;
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

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
    float metersPerUnit = 0;  // (FSR 3.1's viewSpaceToMetersFactor; 0: unknown - 1 assumed)
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
    // (now: released at once - the GPU idle; else retired: released kRetire frames later, when the frames in flight
    // that use them are surely done - a new format or size from the menu otherwise freed them under the GPU)
    void releaseTextures(bool now = false);
    void retire(Tex &t, bool now);
    void collectRetired(bool all);
    struct Retired { ID3D12Resource *res; uint64_t frame; };
    std::vector<Retired> retired_;
    static constexpr uint64_t kRetire = 8;
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
    ID3D12PipelineState *psoAoPrep_ = nullptr, *psoAoTemporal_ = nullptr, *psoAoComp_ = nullptr, *psoAoFilter_ = nullptr;
    ID3D12PipelineState *psoAoMain16_ = nullptr, *psoAoLut_ = nullptr, *psoAoCacao_ = nullptr,
                         *psoAoMipAll_ = nullptr, *psoAoCacao2_ = nullptr, *psoAoCurve_ = nullptr,
                         *psoAoAtrous_ = nullptr, *psoAoLocal_ = nullptr, *psoAoRecon_ = nullptr, *psoAoMain16H_ = nullptr;  // (the main pass (16 x 16; FP16), the mips in one pass, CACAO's 2 passes in one, the curve's table, the GI's a-trous, the area's luma)  // (EXPERIMENTAL: Ditto Relight)
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
    Tex aoNd_, aoRes_, aoF_, aoCol_;  // (EXPERIMENTAL, Ditto Relight: normal and depth, occlusion and bounce, the filter's x, the colour)
    // (... their temporal smoothing's history and its depth, ping-ponged (aoCur_ this frame's); aoOk_: valid, aoLast_:
    // the frame it was written)
    Tex aoH_[2], aoHD_[2];
    Tex aoMip_[4];  // (... the depth mips 1 - 4: mips on)
    Tex aoLut_, aoHalf_;  // (... the occluder lookup table - aoLutMade_: built -, the half-size main pass' target)
    // (... half resolution, FastGI's: the filter's scratch, the normal and depth, the temporal smoothing's history and its
    // depth, all at half size; aoLastHalf_: the last frame's mode - the history is either size's)
    Tex aoHalfF_, aoNdHalf_, aoHh_[2], aoHDh_[2];
    Tex aoBlue_, aoBlueUp_;  // (the blue noise mask and its upload buffer - aoBlueMade_: copied)
    Tex aoLocal_;  // (the area's luma - the local exposure: level 3's size)  // (the GI's probes; the blue noise mask and its upload buffer - aoBlueMade_: copied)
    bool aoBlueMade_ = false;
    Tex aoCurveLut_;  // (ao_curve_lut: the curve's table, 4096 x 1 - curveKey_: the curve it was built for)
    float curveKey_[26] = {};
    Tex aoNdP_, aoNdHalfP_;  // (ao_pack_nd: the normal and depth packed, render and half size)
    int aoLastMode_ = -1;  // (the last frame's resolution mode - the history is one mode's)
    Tex aoCk_;  // (checkerboard: the main pass' half-width target)
    // (the F8 capture: per frame, the output's and Relight's colour's readback copies; capN_ the frames copied (-1: no
    // capture), capLast_ the frame of the last - written out kCapWait frames later, the GPU done with them)
    struct Cap {
        ID3D12Resource *rb = nullptr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        uint32_t w = 0, h = 0;
    };
    static constexpr int kCapFrames = 8, kCapWait = 6;
    Cap capOut_[kCapFrames], capRel_[kCapFrames];
    int capN_ = -1;
    uint64_t capLast_ = 0;
    bool capKeyDown_ = false;
    bool capCopy(ID3D12GraphicsCommandList *cl, ID3D12Resource *src, Cap &c);
    void capWrite();
    bool aoLutMade_ = false;
    int aoCur_ = 0;
    bool aoOk_ = false;
    uint64_t aoLast_ = ~0ull;
    bool split_ = false;
    int cur_ = 0;
    bool texValid_ = false;
    uint64_t frames_ = 0;
    float prevJit_[2] = {0, 0};
    // the passes' GPU time (the menu's graph): five timestamps a frame (the start, after the convert, the motion and
    // the upscale passes, the end) into a ring, read back kTimeRing frames later; log_passes=1: each pass's mean logged
    // (stamps: 0 the start, 1 - 4 after convert, motion, upscale, sharpen; 5 - 13 after Relight's prepass, mips, main
    // pass, half normal and depth, filter, temporal smoothing, CACAO, upsample, composition - all written every frame)
    static constexpr uint32_t kTimeRing = 8, kStamps = 14;
    double passMs_[4] = {0, 0, 0, 0}, aoMs_[10] = {};
    uint32_t passN_ = 0;
    ID3D12QueryHeap *timeQ_ = nullptr;
    ID3D12Resource *timeRb_ = nullptr;
    const uint64_t *timeMap_ = nullptr;
    double tickMs_ = 0.0;  // (ms per timestamp tick; 0: no timing)
};

// An FSR 3.1 upscale dispatch as a DittoFrame (false: a resource missing)
bool ditto_frame_from_ffx(const ffxDispatchDescUpscale *d, const DittoCreateInfo &ci, DittoFrame *f);

// FidelityFX's resource state as D3D12's; a typed format to view a (possibly typeless, sRGB or depth) resource with
D3D12_RESOURCE_STATES ditto_d3d_state(uint32_t s);
DXGI_FORMAT ditto_view_format(DXGI_FORMAT f, bool uav);

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
    // F8: the next 8 frames captured (D3D12) - the output and Ditto Relight's colour, raw files in dittosr-capture\
    // next to the DLL (test/capture.py turns them into PNGs and measures the flicker); 0: none
    int captureKey = 0;  // (capture_key=0x77: F8 - off by default, F8 is many games' quick load)
    int captureFrame = 0;  // (capture_frame=N: the capture started at the Nth upscale - the harness has no keys)
    bool compare = false;  // ([dittosr] compare=1: from the start)
    // the ratios the FidelityFX API answers the quality modes with: quality, balanced, performance, ultra performance
    // (0: FSR 3.1's 1.5, 1.7, 2, 3) - games that ask, not those that work them out themselves
    float ratio[4] = {0, 0, 0, 0};
    // the sharpness the FidelityFX API's dispatch carries (when it enables sharpening) in place of dittosr.ini's: -1 only
    // when this DLL runs from OptiScaler's custom provider folder (its sharpness slider), 1 always, 0 never
    int gameSharpness = -1;
    // frame generation: 1 DittoSR's in-between images when the game (or OptiScaler) turns FSR 3.1's on (AMD's swapchain
    // still presents them: its DLL alongside); 0 AMD's own interpolation
    int frameGen = 1;
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
    // frame generation: 1 DittoSR's own context (no AMD's DLL), 2 DittoSR's alongside AMD's (either's images); 0 none;
    // fgQpc its last dispatch (QueryPerformanceCounter), fgAmdNow whose images it gave (AMD's)
    volatile int fgMode = 0;
    volatile int64_t fgQpc = 0;
    volatile bool fgAmdNow = false;
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
