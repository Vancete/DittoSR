// The passes' constants (shaders/common.hlsli's cbuffer C, in order), shared by the D3D12 and Vulkan drivers: root
// constants on D3D12, a uniform buffer on Vulkan.
#pragma once
#include <math.h>

#include "dittosr.h"

struct DittoConstants {
    float rs[2], os[2], rsRcp[2], osRcp[2];
    float jit[2], ratio, width;
    float alphaRest, conf, reset, gammaRest;
    float react, reactiveScale, stabStep, depthInverted;
    float mvScale[2], mvSize[2];
    float jitCancel[2], preExposure, hdr;
    float exposureScale, hasExposure, hasReactive, debugView;
    float indicator, motionGamma, motionShare, motionMinMax;
    float antiShimmer, sharpness, changeScale, shareFar;
    float shareNear, encodePower, motionNear, splitClip;
    float history10, frameWeight, splitX, autoReactive;
    float autoExposure, historySharpen, fusedSharpen;
    float envTol, envDecay, envGain, envRest;
    float rampLo, rampHi, envRel, envCap, alphaSlow;
    float sharpenMode;
};
static_assert(sizeof(DittoConstants) == 61 * 4, "the root constants");

// One upscale's constants from the settings and the frame (DittoFrame or DittoFrameVk: the same numeric fields).
// prevJit: the previous frame's jitter, updated (the vectors' jitter cancellation).
template <class Frame>
void ditto_fill_constants(DittoConstants &k, const DittoSettings &s, const Frame &fr, bool reset, bool hasExposure,
                          bool hasReactive, bool history10, float prevJit[2]) {
    uint32_t rw = fr.rw, rh = fr.rh, ow = fr.ow, oh = fr.oh;
    k = DittoConstants();
    k.rs[0] = (float)rw, k.rs[1] = (float)rh, k.os[0] = (float)ow, k.os[1] = (float)oh;
    k.rsRcp[0] = 1.0f / rw, k.rsRcp[1] = 1.0f / rh, k.osRcp[0] = 1.0f / ow, k.osRcp[1] = 1.0f / oh;
    k.jit[0] = fr.jitX * s.jitterSignX, k.jit[1] = fr.jitY * s.jitterSignY;
    k.ratio = (float)ow / (float)rw;
    // (the narrow Gaussian narrower at low ratios: 0.875 x at 1.5x and below, as it is from 2x - there each output pixel
    // has samples close more often, and the narrower estimate is sharper and steadier at once: at 1.5x, 0.8 -> 0.7 gave
    // the swaying grass's sharpness 0.62 -> 0.76, the pans' shimmer 7.1 -> 6.0 and 7.7 -> 5.7, PSNR +0.15 - 0.36 dB)
    float lo = k.ratio <= 1.5f ? 0.0f : k.ratio >= 2.0f ? 1.0f : (k.ratio - 1.5f) / 0.5f;
    k.width = s.width * (0.875f + 0.125f * lo);
    k.alphaRest = s.alphaRest, k.conf = s.conf, k.reset = reset ? 1.0f : 0.0f, k.gammaRest = s.gammaRest;
    k.react = s.react, k.reactiveScale = s.reactiveScale * fr.reactiveMul;
    k.mvScale[0] = s.mvSign * fr.mvToUv[0], k.mvScale[1] = s.mvSign * fr.mvToUv[1];
    k.mvSize[0] = fr.mvRes[0], k.mvSize[1] = fr.mvRes[1];
    if (fr.jitteredMv) k.jitCancel[0] = (prevJit[0] - k.jit[0]) / rw, k.jitCancel[1] = (prevJit[1] - k.jit[1]) / rh;
    prevJit[0] = k.jit[0], prevJit[1] = k.jit[1];
    k.depthInverted = fr.depthInverted ? 1.0f : 0.0f;
    k.preExposure = fr.preExposure > 0.0f ? fr.preExposure : 1.0f;
    k.hdr = fr.hdr ? 1.0f : 0.0f;
    k.exposureScale = s.exposureScale * fr.exposureMul, k.hasExposure = hasExposure ? 1.0f : 0.0f;
    k.hasReactive = hasReactive ? 1.0f : 0.0f;
    k.debugView = (float)s.debugView, k.indicator = s.indicator ? 1.0f : 0.0f;
    k.motionGamma = s.motionGamma, k.motionShare = s.motionShare, k.motionMinMax = s.motionMinMax;
    k.sharpness = s.sharpness, k.changeScale = s.changeScale;
    k.shareFar = s.shareFar, k.shareNear = s.shareNear, k.encodePower = s.encodePower;
    k.motionNear = s.motionNear, k.splitClip = s.splitClip;
    k.history10 = history10 ? 1.0f : 0.0f;
    k.frameWeight = s.frameWeight < 0.25f ? 0.25f : s.frameWeight > 4.0f ? 4.0f : s.frameWeight;
    // (below 1.5x the weight in use - the user's or FSR 3.1's settings' - x 0.5 at 1x, easing back by 1.5x: at 1x every
    // pixel has a sample near it each frame, and at 1.5x's weight each one took too much of it - the panning grass
    // shimmered more than at 1.5x (5.13 against 4.05). Halved: 2.92, its PSNR +0.64 dB, the still grass +0.49 dB, the
    // glints' shimmer 1.54 -> 0.89, the tiles' moving square as it was (-0.05 dB). The menu shows the value as set)
    float r15 = k.ratio <= 1.0f ? 0.0f : k.ratio >= 1.5f ? 1.0f : (k.ratio - 1.0f) / 0.5f;
    k.frameWeight *= 0.5f + 0.5f * r15;
    k.autoReactive = hasReactive ? 0.0f : s.autoReactive;  // (the game's own mask wins)
    k.stabStep = s.stabStep > 0.01f ? s.stabStep : 0.34f;
    // (upscale.hlsl: 0.03 up to 2x, 0.05 from 3x, the extra bounded by the samples' range - unbounded, at 2x beyond 0.02
    // the tiles' zone plate drifted over the frames (-0.12 dB at 0.025); bounded, 0.03 holds it and 0.04 not)
    float hr = k.ratio <= 2.0f ? 0.0f : k.ratio >= 3.0f ? 1.0f : k.ratio - 2.0f;
    k.historySharpen = s.historySharpen > 0.0f ? (0.03f + 0.02f * hr) * s.historySharpen : 0.0f;
    // (the anti-shimmer by the ratio unless set: 0.4 up to 2x, 0.2 from 3x - there it held back the new frame's detail,
    // the image slower to get sharp again after a turn, and the history's own detail is made up more)
    // (0.6 to 0.45 now, with the base share higher in slow motion - alphaSlow)
    float as = s.antiShimmer >= 0.0f ? s.antiShimmer : 0.6f - 0.15f * hr;
    k.antiShimmer = as > 0.9f ? 0.9f : as;
    // (the sharpening in the upscale, its own pass skipped: D3D12 without the comparison - set by the driver)
    k.fusedSharpen = 0.0f;
    k.rampLo = s.rampLo, k.rampHi = s.rampHi, k.envRel = s.envRel, k.alphaSlow = s.alphaSlow;
    // (the envelope's cap by the ratio: half above 2x, 0.25 from 3x - there what it lets in is a coarser estimate)
    k.envCap = s.envCap * (1.0f - 0.5f * hr);
    // (the far share halved by 3x: there 8 pixels in 9 have no sample near each frame, and at 2x's far share their smooth
    // estimate is half of what a still pixel holds - harness, 3x: a pan's sharpness 0.73 -> 0.93 of the reference's, its
    // shimmer 3.79 -> 3.06, the still image 33.14 -> 33.40 dB and its shimmer 1.89 -> 1.56; the tiles 29.50 -> 29.37)
    k.shareFar *= 1.0f - 0.5f * hr;
    k.envTol = s.envTol, k.envDecay = s.envDecay, k.envGain = s.envGain, k.envRest = s.envRest;
    k.sharpenMode = s.sharpenMode == 1 || s.sharpenMode == 2 ? (float)s.sharpenMode : 0.0f;
}

// Ditto Relight's constants (shaders/ao_common.hlsli's cbuffer A, in order) - root constants on D3D12, a uniform
// buffer on Vulkan - and their fill from the settings and the frame (DittoFrame or DittoFrameVk); floatColour: the
// game's colour a float format (HDR read). temporal (the history valid) and axis (the filter's pass) set by the driver.
struct AoConstants {
    float rs[2], rsRcp[2], tanX, tanY, zNear, zFar, inverted, frame, aoStrength, giStrength, radiusWorld, radiusScreen, rays,
        contact, giDark, sCurve, pivot, temporal, spatial, axis, mvScale[2], mvSize[2], giCeiling, planeW, mips, aoPow, flags;
    uint32_t flags2;  // (bits: 1 the lookup table; the GI tests - ao_common.hlsli)
    float cmips, samples, halfRes, fadeZ;
    float curveX[8], curveY[8], curveY2[8], curveN;  // (the curve: its points, the spline's second derivatives; 0: none)
};
static_assert(sizeof(AoConstants) == 61 * 4, "Relight's constants (D3D12: within the 61 root constants)");
static_assert(sizeof(AoConstants) <= sizeof(DittoConstants), "Relight's constants");

template <class Frame>
void ditto_fill_ao(AoConstants &ka, const DittoSettings &s, const Frame &fr, bool floatColour, uint64_t frames) {
    ka = AoConstants();
    uint32_t rw = fr.rw, rh = fr.rh;
    ka.rs[0] = (float)rw, ka.rs[1] = (float)rh, ka.rsRcp[0] = 1.0f / rw, ka.rsRcp[1] = 1.0f / rh;
    float fov = fr.fovY > 0.01f && fr.fovY < 3.1f ? fr.fovY : 1.0472f;  // (none given - XeSS: 60 degrees)
    ka.tanY = tanf(0.5f * fov), ka.tanX = ka.tanY * (float)rw / (float)rh;
    ka.zNear = fr.zNear > 0.0f ? fr.zNear : 0.1f, ka.zFar = fr.zFar > 0.0f ? fr.zFar : 100000.0f;
    ka.inverted = fr.depthInverted ? 1.0f : 0.0f, ka.frame = (float)(frames % 1024);
    ka.aoStrength = s.aoStrength < 0.0f ? 0.0f : s.aoStrength > 1.0f ? 1.0f : s.aoStrength;
    ka.giStrength = s.giStrength < 0.0f ? 0.0f : s.giStrength > 4.0f ? 4.0f : s.giStrength;
    ka.giDark = s.giDark < 0.0f ? 0.0f : s.giDark > 4.0f ? 4.0f : s.giDark;
    ka.sCurve = s.sCurve < 0.0f ? 0.0f : s.sCurve > 1.0f ? 1.0f : s.sCurve;
    ka.giCeiling = s.giCeiling < 0.0f ? 0.0f : s.giCeiling > 4.0f ? 4.0f : s.giCeiling;
    ka.planeW = s.aoPlaneFilter ? 1.0f : 0.0f, ka.mips = s.aoMips ? 1.0f : 0.0f;
    ka.aoPow = s.aoPower ? 1.0f : 0.0f;
    // (flags: 1 backface boost, 2 decorrelated noise)
    ka.flags = (float)((s.aoBackBoost ? 1 : 0) | (s.aoDecorrelate ? 2 : 0) | (s.aoAmbientOnly ? 256 : 0) | (s.aoEmitterCos ? 512 : 0) |
                       (s.aoBetterNormals ? 2048 : 0) | (s.aoGiWide ? 16384 : 0) | (s.aoGiLong ? 32768 : 0) |
                       (s.aoNoise8 ? 524288 : 0) | (s.aoGiFalloff ? 1048576 : 0));
    ka.flags2 = s.aoLut ? 1u : 0u, ka.cmips = s.aoColorMips && s.aoMips ? 1.0f : 0.0f;
    ka.samples = s.aoSamples < 4.0f ? 4.0f : s.aoSamples > 32.0f ? 32.0f : floorf(s.aoSamples + 0.5f);
    // (the AO's fade: from fadeZ view units on - its metres over the camera's metres a unit)
    ka.fadeZ = s.aoFadeMeters > 0.0f ? s.aoFadeMeters / (fr.metersPerUnit > 0.0f ? fr.metersPerUnit : 1.0f) : 0.0f;
    if (!ditto_curve_line(s)) {
        int n = s.curveN > 8 ? 8 : s.curveN;
        for (int i = 0; i < n; i++) ka.curveX[i] = s.curveX[i], ka.curveY[i] = s.curveY[i];
        ditto_curve_y2(n, ka.curveX, ka.curveY, ka.curveY2);
        ka.curveN = (float)n;
    }
    ka.pivot = fr.hdr || floatColour ? 0.071f : 0.3f;  // (30 % of the brightness: linear HDR 0.3 ^ 2.2, or [0, 1] colour)
    ka.radiusWorld = s.aoRadius, ka.radiusScreen = s.aoScreen > 0.01f ? s.aoScreen : 0.5f;
    ka.rays = s.aoRays < 4.0f ? 4.0f : s.aoRays > 32.0f ? 32.0f : floorf(s.aoRays + 0.5f);
    ka.contact = s.aoContact < 0.0f ? 0.0f : s.aoContact > 4.0f ? 4.0f : s.aoContact;
    ka.spatial = s.aoSpatial < 1.0f ? 1.0f : s.aoSpatial > 2.0f ? 2.0f : s.aoSpatial;
    ka.mvScale[0] = s.mvSign * fr.mvToUv[0], ka.mvScale[1] = s.mvSign * fr.mvToUv[1];
    ka.mvSize[0] = fr.mvRes[0], ka.mvSize[1] = fr.mvRes[1];
}
