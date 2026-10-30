// The passes' constants (shaders/common.hlsli's cbuffer C, in order), shared by the D3D12 and Vulkan drivers: root
// constants on D3D12, a uniform buffer on Vulkan.
#pragma once
#include <math.h>

#include "dittosr.h"

struct DittoConstants {
    float rs[2], os[2], rsRcp[2], osRcp[2];
    float jit[2], ratio, width;
    float alphaRest, conf, reset, gammaRest;
    float react, edge, lockLines, reactiveScale;
    float mvScale[2], mvSize[2];
    float jitCancel[2], tanHalfFovH, depthInverted;
    float preExposure, hdr, exposureScale, hasExposure;
    float hasReactive, mode, debugView, indicator;
    float motionGamma, motionShare, motionMinMax, depthMode;
    float sharpness, changeScale, shareFar, shareNear;
    float encodePower, motionWidth, motionNear, splitClip;
    float history10, frameWeight, pad[2];
};
static_assert(sizeof(DittoConstants) == 52 * 4, "the root constants");

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
    k.ratio = (float)ow / (float)rw, k.width = s.width;
    k.alphaRest = s.alphaRest, k.conf = s.conf, k.reset = reset ? 1.0f : 0.0f, k.gammaRest = s.gammaRest;
    k.react = s.react, k.edge = s.edge, k.lockLines = s.lockLines, k.reactiveScale = s.reactiveScale * fr.reactiveMul;
    k.mvScale[0] = s.mvSign * fr.mvToUv[0], k.mvScale[1] = s.mvSign * fr.mvToUv[1];
    k.mvSize[0] = fr.mvRes[0], k.mvSize[1] = fr.mvRes[1];
    if (fr.jitteredMv) k.jitCancel[0] = (prevJit[0] - k.jit[0]) / rw, k.jitCancel[1] = (prevJit[1] - k.jit[1]) / rh;
    prevJit[0] = k.jit[0], prevJit[1] = k.jit[1];
    // (no field of view from the API: the ini's horizontal one)
    k.tanHalfFovH = fr.fovV > 0.01f && fr.fovV < 3.1f ? tanf(0.5f * fr.fovV) * (float)rw / (float)rh
                                                      : tanf(0.5f * s.fovHDeg * 3.14159265f / 180.0f);
    k.depthInverted = fr.depthInverted ? 1.0f : 0.0f;
    k.preExposure = fr.preExposure > 0.0f ? fr.preExposure : 1.0f;
    k.hdr = fr.hdr ? 1.0f : 0.0f;
    k.exposureScale = s.exposureScale * fr.exposureMul, k.hasExposure = hasExposure ? 1.0f : 0.0f;
    k.hasReactive = hasReactive ? 1.0f : 0.0f;
    k.debugView = (float)s.debugView, k.indicator = s.indicator ? 1.0f : 0.0f;
    k.motionGamma = s.motionGamma, k.motionShare = s.motionShare, k.motionMinMax = s.motionMinMax;
    k.depthMode = (float)s.depthMode, k.sharpness = s.sharpness, k.changeScale = s.changeScale;
    k.shareFar = s.shareFar, k.shareNear = s.shareNear, k.encodePower = s.encodePower;
    k.motionWidth = s.motionWidth, k.motionNear = s.motionNear, k.splitClip = s.splitClip;
    k.history10 = history10 ? 1.0f : 0.0f;
    k.frameWeight = s.frameWeight < 0.25f ? 0.25f : s.frameWeight > 4.0f ? 4.0f : s.frameWeight;
}
