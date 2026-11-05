// DittoSR's frame generation on D3D12: the in-between images FSR 3.1's frame generation asks for, made by
// letsgo-recomp's motion-field warp (shaders/fg_*.hlsl) instead of AMD's interpolation. AMD's frame generation context
// and its swapchain stay - the presentation, the pacing, the UI composition are AMD's -: the game's (or OptiScaler's)
// frame generation callback dispatches the frame generation on the context, through this DLL, and DittoSR answers it.
//   prepare: with the FG prepare dispatch (the game's command list) - this frame's motion field from its depth and
// motion vectors. generate: with the FG dispatch (the swapchain's command list) - the field dilated, then each
// in-between image warped from the previous frame's image and this one's, this one's kept for the next.
#pragma once
#include <d3d12.h>
#include <stdint.h>

#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_framegeneration.h"

class DittoFG {
public:
    // flags: FfxApiCreateContextFramegenerationFlags
    DittoFG(ID3D12Device *device, uint32_t flags, uint32_t displayW, uint32_t displayH);
    ~DittoFG();
    bool ok() const { return ok_; }
    void prepare(const ffxDispatchDescFrameGenerationPrepare *d);
    ffxReturnCode_t generate(const ffxDispatchDescFrameGeneration *d);
    uint64_t generated() const { return generated_; }

private:
    struct Tex {
        ID3D12Resource *res = nullptr;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        uint32_t w = 0, h = 0;
    };
    bool makeTex(Tex &t, uint32_t w, uint32_t h, DXGI_FORMAT f, bool uav);
    void drop(Tex &t);
    void transition(ID3D12GraphicsCommandList *cl, Tex &t, D3D12_RESOURCE_STATES to);
    D3D12_GPU_DESCRIPTOR_HANDLE table(ID3D12Resource *const *srv, const DXGI_FORMAT *srvFmt, int nSrv, ID3D12Resource *uav, DXGI_FORMAT uavFmt);

    ID3D12Device *dev_ = nullptr;
    uint32_t flags_ = 0, dw_ = 0, dh_ = 0;
    bool ok_ = false;
    ID3D12RootSignature *rootSig_ = nullptr;
    ID3D12PipelineState *psoField_ = nullptr, *psoDilate_ = nullptr, *psoWarp_ = nullptr;
    ID3D12DescriptorHeap *heap_ = nullptr;
    uint32_t heapSize_ = 0, heapNext_ = 0, descInc_ = 0;
    Tex field_[3], dil_, prev_;
    uint64_t fieldId_[3] = {~0ull, ~0ull, ~0ull};  // (the frame each slot's field is for: frameID % 3)
    uint64_t prevId_ = ~0ull;                      // (the frame prev_ holds the image of)
    float prevJit_[2] = {0, 0};
    uint64_t generated_ = 0;
};
