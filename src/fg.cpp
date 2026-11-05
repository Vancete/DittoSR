// DittoSR's frame generation on D3D12 (fg.h): the field pass with the FG prepare, the dilation and the warp with the FG
// dispatch. Its own root signature (16 root constants: shaders/fg_common.hlsli; a table of 5 SRVs and a UAV) and
// descriptor ring, as dittosr.cpp's.
#include "fg.h"

#include <initializer_list>
#include <string.h>

#include "dittosr.h"

#include "cs_fg_dilate.h"
#include "cs_fg_field.h"
#include "cs_fg_warp.h"

namespace {

constexpr UINT kSrvs = 5, kTable = kSrvs + 1;
constexpr UINT kTablesPerFrame = 8, kFramesInRing = 64;

struct FgConstants {  // (shaders/fg_common.hlsli's cbuffer C, in order)
    float size[2], sizeRcp[2], mvToUv[2], mvRes[2], jitCancel[2];
    float nearZ, farZ, depthInv, depthInf, t, hasPrev;
};
static_assert(sizeof(FgConstants) == 16 * 4, "the root constants");

// a game's resource as a shader reads it (or the given state), and back: barriers only where its state differs
struct GameRes {
    ID3D12Resource *r;
    D3D12_RESOURCE_STATES from, to;
};
void game_to(ID3D12GraphicsCommandList *cl, GameRes &g, D3D12_RESOURCE_STATES need) {
    g.to = need;
    if (g.from == need || (need != D3D12_RESOURCE_STATE_UNORDERED_ACCESS && (g.from & need) == need)) { g.to = g.from; return; }
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g.r, b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = g.from, b.Transition.StateAfter = need;
    cl->ResourceBarrier(1, &b);
}
void game_back(ID3D12GraphicsCommandList *cl, GameRes &g) {
    if (g.to == g.from) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g.r, b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = g.to, b.Transition.StateAfter = g.from;
    cl->ResourceBarrier(1, &b);
    g.to = g.from;
}
void uav_barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV, b.UAV.pResource = r;
    cl->ResourceBarrier(1, &b);
}

}  // namespace

DittoFG::DittoFG(ID3D12Device *device, uint32_t flags, uint32_t displayW, uint32_t displayH)
    : dev_(device), flags_(flags), dw_(displayW), dh_(displayH) {
    dev_->AddRef();
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV, ranges[0].NumDescriptors = kSrvs;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV, ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = kSrvs;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = sizeof(FgConstants) / 4;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2, params[1].DescriptorTable.pDescriptorRanges = ranges;
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rd = {};
    rd.NumParameters = 2, rd.pParameters = params, rd.NumStaticSamplers = 1, rd.pStaticSamplers = &samp;
    ID3DBlob *blob = nullptr, *err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
        ditto_log("frame generation: root signature: %s\n", err ? (const char *)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return;
    }
    HRESULT hr = dev_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig_));
    blob->Release();
    if (FAILED(hr)) { ditto_log("frame generation: CreateRootSignature 0x%08x\n", (unsigned)hr); return; }
    struct { const BYTE *code; size_t size; ID3D12PipelineState **pso; } cs[3] = {
        {g_cs_fg_field, sizeof g_cs_fg_field, &psoField_}, {g_cs_fg_dilate, sizeof g_cs_fg_dilate, &psoDilate_},
        {g_cs_fg_warp, sizeof g_cs_fg_warp, &psoWarp_}};
    for (auto &c : cs) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = rootSig_, pd.CS.pShaderBytecode = c.code, pd.CS.BytecodeLength = c.size;
        if (FAILED(hr = dev_->CreateComputePipelineState(&pd, IID_PPV_ARGS(c.pso))))
            { ditto_log("frame generation: CreateComputePipelineState 0x%08x\n", (unsigned)hr); return; }
    }
    heapSize_ = kTable * kTablesPerFrame * kFramesInRing;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, hd.NumDescriptors = heapSize_, hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(hr = dev_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_))))
        { ditto_log("frame generation: CreateDescriptorHeap 0x%08x\n", (unsigned)hr); return; }
    descInc_ = dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ok_ = true;
}

DittoFG::~DittoFG() {
    for (Tex *t : {&field_[0], &field_[1], &field_[2], &dil_, &prev_}) drop(*t);
    for (IUnknown *u : {(IUnknown *)psoField_, (IUnknown *)psoDilate_, (IUnknown *)psoWarp_, (IUnknown *)rootSig_, (IUnknown *)heap_})
        if (u) u->Release();
    dev_->Release();
}

bool DittoFG::makeTex(Tex &t, uint32_t w, uint32_t h, DXGI_FORMAT f, bool uav) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w, rd.Height = h, rd.DepthOrArraySize = 1, rd.MipLevels = 1, rd.Format = f;
    rd.SampleDesc.Count = 1, rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    t.state = D3D12_RESOURCE_STATE_COMMON;
    HRESULT hr = dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, t.state, nullptr, IID_PPV_ARGS(&t.res));
    if (FAILED(hr)) return ditto_log("frame generation: CreateCommittedResource %ux%u 0x%08x\n", w, h, (unsigned)hr), false;
    t.res->SetName(L"DittoSR FG");
    t.fmt = f, t.w = w, t.h = h;
    return true;
}

void DittoFG::drop(Tex &t) {
    if (t.res) t.res->Release();
    t = Tex();
}

void DittoFG::transition(ID3D12GraphicsCommandList *cl, Tex &t, D3D12_RESOURCE_STATES to) {
    if (t.state == to) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t.res, b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = t.state, b.Transition.StateAfter = to;
    cl->ResourceBarrier(1, &b);
    t.state = to;
}

D3D12_GPU_DESCRIPTOR_HANDLE DittoFG::table(ID3D12Resource *const *srv, const DXGI_FORMAT *srvFmt, int nSrv, ID3D12Resource *uav, DXGI_FORMAT uavFmt) {
    if (heapNext_ + kTable > heapSize_) heapNext_ = 0;
    UINT base = heapNext_;
    heapNext_ += kTable;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)base * descInc_, gpu.ptr += (UINT64)base * descInc_;
    for (UINT i = 0; i < kTable; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = {cpu.ptr + (SIZE_T)i * descInc_};
        if (i < kSrvs) {
            ID3D12Resource *r = (int)i < nSrv ? srv[i] : nullptr;
            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = r ? srvFmt[i] : DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            dev_->CreateShaderResourceView(r, &sd, h);
        } else {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
            ud.Format = uav ? uavFmt : DXGI_FORMAT_R8G8B8A8_UNORM;
            ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            dev_->CreateUnorderedAccessView(uav, nullptr, &ud, h);
        }
    }
    return gpu;
}

void DittoFG::prepare(const ffxDispatchDescFrameGenerationPrepare *d) {
    auto *cl = (ID3D12GraphicsCommandList *)d->commandList;
    auto *depth = (ID3D12Resource *)d->depth.resource, *mv = (ID3D12Resource *)d->motionVectors.resource;
    uint32_t rw = d->renderSize.width, rh = d->renderSize.height;
    if (!ok_ || !cl || !depth || !mv || !rw || !rh) return;
    if (field_[0].w != rw || field_[0].h != rh) {  // (a new render size: new fields, none valid)
        for (Tex *t : {&field_[0], &field_[1], &field_[2], &dil_}) drop(*t);
        bool made = true;
        for (Tex *t : {&field_[0], &field_[1], &field_[2], &dil_}) made = made && makeTex(*t, rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
        for (uint64_t &id : fieldId_) id = ~0ull;
        if (!made) return;
        ditto_log("frame generation: fields %ux%u\n", rw, rh);
    }
    int slot = (int)(d->frameID % 3);
    Tex &f = field_[slot];
    FgConstants k = {};
    k.size[0] = (float)rw, k.size[1] = (float)rh, k.sizeRcp[0] = 1.0f / rw, k.sizeRcp[1] = 1.0f / rh;
    bool displayMv = (flags_ & FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) != 0;
    k.mvRes[0] = displayMv ? (float)dw_ : (float)rw, k.mvRes[1] = displayMv ? (float)dh_ : (float)rh;
    k.mvToUv[0] = d->motionVectorScale.x / k.mvRes[0], k.mvToUv[1] = d->motionVectorScale.y / k.mvRes[1];
    if (flags_ & FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION)
        k.jitCancel[0] = (prevJit_[0] - d->jitterOffset.x) / rw, k.jitCancel[1] = (prevJit_[1] - d->jitterOffset.y) / rh;
    prevJit_[0] = d->jitterOffset.x, prevJit_[1] = d->jitterOffset.y;
    k.nearZ = d->cameraNear > 0.0f ? d->cameraNear : 0.1f, k.farZ = d->cameraFar > k.nearZ ? d->cameraFar : 1e6f;
    k.depthInv = (flags_ & FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED) ? 1.0f : 0.0f;
    k.depthInf = (flags_ & FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE) || d->cameraFar <= 0.0f ? 1.0f : 0.0f;
    const D3D12_RESOURCE_STATES SR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    GameRes gd = {depth, ditto_d3d_state(d->depth.state), {}}, gm = {mv, ditto_d3d_state(d->motionVectors.state), {}};
    game_to(cl, gd, SR), game_to(cl, gm, SR);
    transition(cl, f, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap *heaps[1] = {heap_};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(rootSig_);
    cl->SetPipelineState(psoField_);
    cl->SetComputeRoot32BitConstants(0, sizeof k / 4, &k, 0);
    ID3D12Resource *s[2] = {depth, mv};
    DXGI_FORMAT sf[2] = {ditto_view_format(depth->GetDesc().Format, false), ditto_view_format(mv->GetDesc().Format, false)};
    cl->SetComputeRootDescriptorTable(1, table(s, sf, 2, f.res, f.fmt));
    cl->Dispatch((rw + 7) / 8, (rh + 7) / 8, 1);
    transition(cl, f, SR);
    game_back(cl, gm), game_back(cl, gd);
    fieldId_[slot] = d->frameID;
}

ffxReturnCode_t DittoFG::generate(const ffxDispatchDescFrameGeneration *d) {
    auto *cl = (ID3D12GraphicsCommandList *)d->commandList;
    auto *present = (ID3D12Resource *)d->presentColor.resource;
    uint32_t n = d->numGeneratedFrames < 1 ? 1 : d->numGeneratedFrames > 4 ? 4 : d->numGeneratedFrames;
    if (!ok_ || !cl || !present) return FFX_API_RETURN_ERROR_PARAMETER;
    for (uint32_t i = 0; i < n; i++)
        if (!d->outputs[i].resource) return FFX_API_RETURN_ERROR_PARAMETER;
    D3D12_RESOURCE_DESC pd = present->GetDesc();
    if (prev_.w != (uint32_t)pd.Width || prev_.h != pd.Height || prev_.fmt != pd.Format) {
        drop(prev_);
        if (!makeTex(prev_, (uint32_t)pd.Width, pd.Height, pd.Format, false)) return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
        prevId_ = ~0ull;
        ditto_log("frame generation: images %llux%u, format %d\n", (unsigned long long)pd.Width, pd.Height, (int)pd.Format);
    }
    int slot = (int)(d->frameID % 3), ps = (int)((d->frameID + 2) % 3);
    bool warp = !d->reset && fieldId_[slot] == d->frameID && prevId_ + 1 == d->frameID && dil_.res;
    bool hasPrevField = warp && fieldId_[ps] + 1 == d->frameID;
    const D3D12_RESOURCE_STATES SR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    GameRes gp = {present, ditto_d3d_state(d->presentColor.state), {}};
    if (warp) {
        Tex &fn = field_[slot];
        ID3D12DescriptorHeap *heaps[1] = {heap_};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(rootSig_);
        // the dilation (render size)
        FgConstants k = {};
        k.size[0] = (float)fn.w, k.size[1] = (float)fn.h, k.sizeRcp[0] = 1.0f / fn.w, k.sizeRcp[1] = 1.0f / fn.h;
        transition(cl, dil_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl->SetPipelineState(psoDilate_);
        cl->SetComputeRoot32BitConstants(0, sizeof k / 4, &k, 0);
        {
            ID3D12Resource *s[1] = {fn.res};
            DXGI_FORMAT sf[1] = {fn.fmt};
            cl->SetComputeRootDescriptorTable(1, table(s, sf, 1, dil_.res, dil_.fmt));
        }
        cl->Dispatch((fn.w + 7) / 8, (fn.h + 7) / 8, 1);
        transition(cl, dil_, SR);
        transition(cl, prev_, SR);
        game_to(cl, gp, SR);
        // the in-between images (output size), at 1 / (n + 1), 2 / (n + 1) ...
        DXGI_FORMAT cf = ditto_view_format(pd.Format, true);  // (raw - an sRGB image's encoded values, written as read)
        cl->SetPipelineState(psoWarp_);
        for (uint32_t i = 0; i < n; i++) {
            auto *out = (ID3D12Resource *)d->outputs[i].resource;
            D3D12_RESOURCE_DESC od = out->GetDesc();
            GameRes go = {out, ditto_d3d_state(d->outputs[i].state), {}};
            game_to(cl, go, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            k = FgConstants();
            k.size[0] = (float)od.Width, k.size[1] = (float)od.Height, k.sizeRcp[0] = 1.0f / od.Width, k.sizeRcp[1] = 1.0f / od.Height;
            k.t = (float)(i + 1) / (float)(n + 1), k.hasPrev = hasPrevField ? 1.0f : 0.0f;
            cl->SetComputeRoot32BitConstants(0, sizeof k / 4, &k, 0);
            ID3D12Resource *s[5] = {prev_.res, present, fn.res, dil_.res, hasPrevField ? field_[ps].res : fn.res};
            DXGI_FORMAT sf[5] = {cf, cf, fn.fmt, dil_.fmt, fn.fmt};
            cl->SetComputeRootDescriptorTable(1, table(s, sf, 5, out, ditto_view_format(od.Format, true)));
            cl->Dispatch(((uint32_t)od.Width + 7) / 8, (od.Height + 7) / 8, 1);
            uav_barrier(cl, out);
            game_back(cl, go);
        }
        generated_ += n;
    } else {  // (no previous image or field yet, or a reset: this frame's image as each in-between one)
        game_to(cl, gp, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (uint32_t i = 0; i < n; i++) {
            GameRes go = {(ID3D12Resource *)d->outputs[i].resource, ditto_d3d_state(d->outputs[i].state), {}};
            game_to(cl, go, D3D12_RESOURCE_STATE_COPY_DEST);
            cl->CopyResource(go.r, present);
            game_back(cl, go);
        }
    }
    // this frame's image kept: the next one's previous
    game_back(cl, gp);
    game_to(cl, gp, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(cl, prev_, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(prev_.res, present);
    transition(cl, prev_, SR);
    game_back(cl, gp);
    prevId_ = d->frameID;
    return FFX_API_RETURN_OK;
}
