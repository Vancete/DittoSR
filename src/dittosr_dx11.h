// DittoSR on D3D11: the convert, motion, upscale and sharpening passes of dittosr.cpp (D3D12), compiled to SM 5.0 with
// FXC, run on the game's device context. Feature level 11_0 is all it needs: at most 5 UAVs a pass, no typed UAV loads
// (every pass writes its UAVs and reads its inputs as shader resources), the shaders FP32. The context's compute stage
// bindings DittoSR touches are put back as they were after each upscale. Ditto Relight (the AO / GI) and the F8 capture
// are D3D12's and Vulkan's.
#pragma once
#include <d3d11.h>
#include <stdint.h>

#include "dittosr.h"

// One upscale on D3D11: the game's resources, and the same numbers as DittoFrame
struct DittoFrame11 {
    ID3D11DeviceContext *ctx = nullptr;
    ID3D11Resource *color = nullptr, *depth = nullptr, *mv = nullptr, *exposure = nullptr, *reactive = nullptr, *output = nullptr;
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
    float jitX = 0, jitY = 0;
    float mvToUv[2] = {0, 0};
    float mvRes[2] = {0, 0};
    bool jitteredMv = false, depthInverted = false, hdr = true;
    float exposureMul = 1.0f;
    float preExposure = 1.0f;
    float reactiveMul = 1.0f;
    bool reset = false;
    bool autoExposure = false;  // (the game asked for it: measured by the convert pass when it gives no exposure)
};

class DittoSR11 {
public:
    DittoSR11(ID3D11Device *device, const DittoCreateInfo &ci, const DittoSettings &s);
    ~DittoSR11();
    bool ok() const { return ok_; }
    // Runs the passes on f.ctx. forceReset: the history starts again.
    bool dispatch(const DittoFrame11 &f, bool forceReset);
    uint64_t frames() const { return frames_; }
    void set_settings(const DittoSettings &s) { s_ = s; }

private:
    struct Tex {
        ID3D11Texture2D *res = nullptr;
        ID3D11ShaderResourceView *srv = nullptr;
        ID3D11UnorderedAccessView *uav = nullptr;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        uint32_t w = 0, h = 0;
    };
    bool init();
    bool makeTex(Tex &t, uint32_t w, uint32_t h, DXGI_FORMAT f);
    void dropTex(Tex &t);
    void releaseTextures();
    // (false: they could not be made; *made: a new set - the history starts again)
    bool ensureTextures(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, DXGI_FORMAT historyFormat, DXGI_FORMAT stabFormat, bool *made);
    bool outputWritable(const D3D11_TEXTURE2D_DESC &od);
    void readTimes(ID3D11DeviceContext *imm);

    ID3D11Device *dev_ = nullptr;
    DittoCreateInfo ci_;
    DittoSettings s_;
    bool ok_ = false;
    ID3D11ComputeShader *csConvert_ = nullptr, *csMotion_ = nullptr, *csUpscale_ = nullptr, *csUpscaleAs_ = nullptr, *csSharpen_ = nullptr;
    ID3D11ComputeShader *csUpscaleFm_[2] = {};  // (1x, the motion pass fused in: [anti-shimmer]; null: the motion pass)
    ID3D11Buffer *cb_ = nullptr;
    ID3D11SamplerState *sampler_ = nullptr;
    Tex mda_[2], hist_[2], aux_[2], stab_[2], env_[2], change1_, expo_[2];
    DXGI_FORMAT outFmt_ = DXGI_FORMAT_UNKNOWN;
    bool outUav_ = false, outOk_ = false;
    bool hdrFromFormat_ = false;
    int cur_ = 0;
    bool texValid_ = false;
    uint64_t frames_ = 0;
    float prevJit_[2] = {0, 0};
    // the passes' GPU time (the menu's graph): a disjoint query and two timestamps a frame into a ring, read kTimeRing
    // frames later
    static constexpr uint32_t kTimeRing = 8;
    struct Times { ID3D11Query *disjoint = nullptr, *start = nullptr, *end = nullptr; uint64_t frame = ~0ull; };
    Times times_[kTimeRing];
};
