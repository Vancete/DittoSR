// DittoSR on D3D12 (dittosr.h), as compute shaders: convert (render size; with the change against the history in an
// extra group) -> motion (render size: the motion checks, the derived reactive mask) -> upscale (output size) -> sharpen
// (output size, but with a debug view), the history, the stability levels, the convert and motion passes' outputs each a
// ping-pong pair. The history is RGB10A2 or RGBA16F (rgb encoded: common.hlsli enc), the stability levels R8.
#include "dittosr.h"
#include "constants.h"

#include <dxgi1_4.h>

#include <initializer_list>
#include <math.h>
#include <string.h>

#include "cs_motion.h"
#include "cs_convert.h"
#include "cs_sharpen.h"
#include "cs_aoprep.h"
#include "cs_aomain.h"
#include "cs_aotemporal.h"
#include "cs_aofgtrace.h"
#include "cs_aofgblur.h"
#include "cs_aofgtemporal.h"
#include "cs_aocompose.h"
#include "cs_upscale.h"
#include "cs_upscale16.h"
#include "cs_upscale_as.h"
#include "cs_upscale16_as.h"
#include "cs_upscale_fm.h"
#include "cs_upscale_as_fm.h"
#include "cs_upscale16_fm.h"
#include "cs_upscale16_as_fm.h"
#include "cs_sharpen16.h"

namespace {

constexpr UINT kConstants = sizeof(DittoConstants) / 4;
constexpr UINT kSrvs = 11, kUavs = 5, kTable = kSrvs + kUavs;  // (t8 - t10, u3, u4: the 1x upscale's motion pass)
constexpr UINT kTablesPerFrame = 5;
constexpr UINT kFramesInRing = 256;
constexpr UINT kUpGX = 16, kUpGY = 8;  // (the upscale's group: upscale.hlsl GX, GY)  // (descriptors are rewritten 256 frames later: long past the GPU's use)

D3D12_RESOURCE_STATES d3d_state(uint32_t s) {
    switch (s) {
    case FFX_API_RESOURCE_STATE_COMMON: return D3D12_RESOURCE_STATE_COMMON;
    case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case FFX_API_RESOURCE_STATE_COMPUTE_READ: return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_PIXEL_READ: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ:
        return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_COPY_SRC: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case FFX_API_RESOURCE_STATE_COPY_DEST: return D3D12_RESOURCE_STATE_COPY_DEST;
    case FFX_API_RESOURCE_STATE_GENERIC_READ: return D3D12_RESOURCE_STATE_GENERIC_READ;
    case FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT: return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    case FFX_API_RESOURCE_STATE_PRESENT: return D3D12_RESOURCE_STATE_PRESENT;
    case FFX_API_RESOURCE_STATE_RENDER_TARGET: return D3D12_RESOURCE_STATE_RENDER_TARGET;
    default: return D3D12_RESOURCE_STATE_COMMON;
    }
}

// A typed format to view a (possibly typeless or depth) resource with
DXGI_FORMAT view_format(DXGI_FORMAT f, bool uav) {
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return uav ? DXGI_FORMAT_R8G8B8A8_UNORM : f;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return uav ? DXGI_FORMAT_B8G8R8A8_UNORM : f;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
    case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
    default: return f;
    }
}

// A float colour format: values above 1 (HDR)
bool float_format(DXGI_FORMAT f) {
    switch (view_format(f, false)) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: return true;
    default: return false;
    }
}

}  // namespace

float ditto_ratio_from_quality(uint32_t q) {
    switch (q) {
    case FFX_UPSCALE_QUALITY_MODE_NATIVEAA: return 1.0f;
    // (dittosr.ini's ratio_* where set, between 1 and 4)
    case FFX_UPSCALE_QUALITY_MODE_QUALITY: return g_cfg.ratio[0] >= 1.0f && g_cfg.ratio[0] <= 4.0f ? g_cfg.ratio[0] : 1.5f;
    case FFX_UPSCALE_QUALITY_MODE_BALANCED: return g_cfg.ratio[1] >= 1.0f && g_cfg.ratio[1] <= 4.0f ? g_cfg.ratio[1] : 1.7f;
    case FFX_UPSCALE_QUALITY_MODE_PERFORMANCE: return g_cfg.ratio[2] >= 1.0f && g_cfg.ratio[2] <= 4.0f ? g_cfg.ratio[2] : 2.0f;
    case FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE: return g_cfg.ratio[3] >= 1.0f && g_cfg.ratio[3] <= 4.0f ? g_cfg.ratio[3] : 3.0f;
    default: return 0.0f;
    }
}

int32_t ditto_jitter_phase_count(uint32_t renderWidth, uint32_t displayWidth) {
    float r = renderWidth ? (float)displayWidth / (float)renderWidth : 1.0f;
    return (int32_t)(8.0f * r * r);
}

static float halton(int32_t i, int32_t b) {
    float f = 1, r = 0;
    for (; i > 0; i /= b) f /= (float)b, r += f * (float)(i % b);
    return r;
}

void ditto_jitter_offset(int32_t index, int32_t phaseCount, float *x, float *y) {
    if (phaseCount <= 0) phaseCount = 1;
    int32_t i = (index % phaseCount + phaseCount) % phaseCount + 1;
    if (x) *x = halton(i, 2) - 0.5f;
    if (y) *y = halton(i, 3) - 0.5f;
}

DittoSR::DittoSR(ID3D12Device *device, const DittoCreateInfo &ci, const DittoSettings &s) : dev_(device), ci_(ci), s_(s) {
    dev_->AddRef();
    ok_ = createPipelines();
    if (!ok_) return;
    // (the timestamps: their frequency from a direct queue of the game's device - the game's own queue is not ours to see)
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *q = nullptr;
    UINT64 hz = 0;
    if (SUCCEEDED(dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) q->GetTimestampFrequency(&hz), q->Release();
    D3D12_QUERY_HEAP_DESC hd = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kStamps * kTimeRing, 0};
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, bd.Width = 8 * kStamps * kTimeRing, bd.Height = 1, bd.DepthOrArraySize = 1;
    bd.MipLevels = 1, bd.SampleDesc.Count = 1, bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_RANGE none = {0, 0};
    if (hz && SUCCEEDED(dev_->CreateQueryHeap(&hd, IID_PPV_ARGS(&timeQ_))) &&
        SUCCEEDED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&timeRb_))) &&
        SUCCEEDED(timeRb_->Map(0, &none, (void **)&timeMap_)))
        tickMs_ = 1000.0 / (double)hz;
    ditto_overlay_install(dev_);
}

DittoSR::~DittoSR() {
    releaseTextures();
    if (timeRb_) timeRb_->Release();
    if (timeQ_) timeQ_->Release();
    if (psoConvert_) psoConvert_->Release();
    if (psoMotion_) psoMotion_->Release();
    for (auto &a : psoUpscaleFm_)
        for (ID3D12PipelineState *p : a)
            if (p) p->Release();
    if (psoUpscale_) psoUpscale_->Release();
    if (psoSharpen_) psoSharpen_->Release();
    if (psoAoPrep_) psoAoPrep_->Release();
    if (psoAoMain_) psoAoMain_->Release();
    if (psoAoTemporal_) psoAoTemporal_->Release();
    if (psoAoFgTrace_) psoAoFgTrace_->Release();
    if (psoAoFgBlur_) psoAoFgBlur_->Release();
    if (psoAoFgTemporal_) psoAoFgTemporal_->Release();
    if (psoAoComp_) psoAoComp_->Release();
    if (psoUpscale16_) psoUpscale16_->Release();
    if (psoUpscaleAs_) psoUpscaleAs_->Release();
    if (psoUpscaleAs16_) psoUpscaleAs16_->Release();
    if (psoSharpen16_) psoSharpen16_->Release();
    if (rootSig_) rootSig_->Release();
    if (heap_) heap_->Release();
    if (dev_) dev_->Release();
}

bool DittoSR::createPipelines() {
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV, ranges[0].NumDescriptors = kSrvs;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV, ranges[1].NumDescriptors = kUavs;
    ranges[1].OffsetInDescriptorsFromTableStart = kSrvs;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = kConstants;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2, params[1].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd = {};
    rd.NumParameters = 2, rd.pParameters = params, rd.NumStaticSamplers = 1, rd.pStaticSamplers = &samp;
    ID3DBlob *blob = nullptr, *err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
        ditto_log("root signature: %s\n", err ? (const char *)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return false;
    }
    HRESULT hr = dev_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig_));
    blob->Release();
    if (FAILED(hr)) return ditto_log("CreateRootSignature 0x%08x\n", (unsigned)hr), false;
    struct { const BYTE *code; size_t size; ID3D12PipelineState **pso; const char *name; } cs[12] = {
        {g_cs_convert, sizeof g_cs_convert, &psoConvert_, "convert"},
        {g_cs_motion, sizeof g_cs_motion, &psoMotion_, "motion"},
        {g_cs_upscale, sizeof g_cs_upscale, &psoUpscale_, "upscale"},
        {g_cs_sharpen, sizeof g_cs_sharpen, &psoSharpen_, "sharpen"},
        {g_cs_upscale_as, sizeof g_cs_upscale_as, &psoUpscaleAs_, "upscale (anti-shimmer)"},
        {g_cs_aoprep, sizeof g_cs_aoprep, &psoAoPrep_, "ao prepass"},
        {g_cs_aomain, sizeof g_cs_aomain, &psoAoMain_, "ao"},
        {g_cs_aotemporal, sizeof g_cs_aotemporal, &psoAoTemporal_, "ao temporal"},
        {g_cs_aofgtrace, sizeof g_cs_aofgtrace, &psoAoFgTrace_, "fastgi trace"},
        {g_cs_aofgblur, sizeof g_cs_aofgblur, &psoAoFgBlur_, "fastgi filter"},
        {g_cs_aofgtemporal, sizeof g_cs_aofgtemporal, &psoAoFgTemporal_, "fastgi temporal"},
        {g_cs_aocompose, sizeof g_cs_aocompose, &psoAoComp_, "ao compose"},
    };
    for (auto &c : cs) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = rootSig_;
        pd.CS.pShaderBytecode = c.code, pd.CS.BytecodeLength = c.size;
        hr = dev_->CreateComputePipelineState(&pd, IID_PPV_ARGS(c.pso));
        if (FAILED(hr)) return ditto_log("CreateComputePipelineState(%s) 0x%08x\n", c.name, (unsigned)hr), false;
    }
    // the FP16 variants (SM 6.2, native 16-bit types): only where the GPU runs 16-bit math natively - else FP32 throughout
    D3D12_FEATURE_DATA_SHADER_MODEL sm = {D3D_SHADER_MODEL_6_2};
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4 = {};
    bool native16 = SUCCEEDED(dev_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm)) && sm.HighestShaderModel >= D3D_SHADER_MODEL_6_2 &&
                    SUCCEEDED(dev_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &o4, sizeof o4)) && o4.Native16BitShaderOpsSupported;
    if (native16) {
        struct { const BYTE *code; size_t size; ID3D12PipelineState **pso; } cs16[3] = {
            {g_cs_upscale16, sizeof g_cs_upscale16, &psoUpscale16_}, {g_cs_sharpen16, sizeof g_cs_sharpen16, &psoSharpen16_},
            {g_cs_upscale16_as, sizeof g_cs_upscale16_as, &psoUpscaleAs16_}};
        for (auto &c : cs16) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
            pd.pRootSignature = rootSig_;
            pd.CS.pShaderBytecode = c.code, pd.CS.BytecodeLength = c.size;
            if (FAILED(dev_->CreateComputePipelineState(&pd, IID_PPV_ARGS(c.pso)))) *c.pso = nullptr;
        }
        if (!psoUpscale16_ || !psoSharpen16_ || !psoUpscaleAs16_) {
            if (psoUpscale16_) psoUpscale16_->Release(), psoUpscale16_ = nullptr;
            if (psoUpscaleAs16_) psoUpscaleAs16_->Release(), psoUpscaleAs16_ = nullptr;
            if (psoSharpen16_) psoSharpen16_->Release(), psoSharpen16_ = nullptr;
        }
    }
    // the 1x variants (the motion pass fused in): optional - where one fails, the motion pass as at other ratios
    {
        struct { const BYTE *code; size_t size; ID3D12PipelineState **pso; } fm[4] = {
            {g_cs_upscale_fm, sizeof g_cs_upscale_fm, &psoUpscaleFm_[0][0]}, {g_cs_upscale_as_fm, sizeof g_cs_upscale_as_fm, &psoUpscaleFm_[0][1]},
            {g_cs_upscale16_fm, sizeof g_cs_upscale16_fm, &psoUpscaleFm_[1][0]}, {g_cs_upscale16_as_fm, sizeof g_cs_upscale16_as_fm, &psoUpscaleFm_[1][1]}};
        for (int i = 0; i < 4; i++) {
            if (i >= 2 && !psoUpscale16_) break;
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
            pd.pRootSignature = rootSig_;
            pd.CS.pShaderBytecode = fm[i].code, pd.CS.BytecodeLength = fm[i].size;
            if (FAILED(dev_->CreateComputePipelineState(&pd, IID_PPV_ARGS(fm[i].pso)))) *fm[i].pso = nullptr;
        }
    }
    // (the vendor, for fp16 = -1)
    IDXGIFactory4 *fac = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) {
        IDXGIAdapter1 *ad = nullptr;
        DXGI_ADAPTER_DESC1 desc = {};
        if (SUCCEEDED(fac->EnumAdapterByLuid(dev_->GetAdapterLuid(), IID_PPV_ARGS(&ad)))) ad->GetDesc1(&desc), ad->Release();
        amd_ = desc.VendorId == 0x1002;
        ditto_log("GPU: %ls (vendor 0x%04x)\n", desc.Description, desc.VendorId);
        fac->Release();
    }
    ditto_log("FP16 shaders: %s\n", psoUpscale16_ ? "available" : native16 ? "failed, FP32" : "no native 16-bit math on this GPU, FP32");
    // (the textures DittoSR writes as UAVs: typed stores to them are required at feature level 11_0 - a driver that
    // reports otherwise is logged, for a report from hardware we can't test on)
    for (DXGI_FORMAT f : {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8_UNORM,
                          DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R32_FLOAT}) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {f};
        if (FAILED(dev_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)) ||
            !(fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
            ditto_log("format dxgi %d: typed UAV stores not reported supported\n", (int)f);
    }
    heapSize_ = kTable * kTablesPerFrame * kFramesInRing;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, hd.NumDescriptors = heapSize_;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = dev_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_));
    if (FAILED(hr)) return ditto_log("CreateDescriptorHeap 0x%08x\n", (unsigned)hr), false;
    descInc_ = dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

bool DittoSR::makeTex(Tex &t, uint32_t w, uint32_t h, DXGI_FORMAT f) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w, rd.Height = h, rd.DepthOrArraySize = 1, rd.MipLevels = 1, rd.Format = f;
    rd.SampleDesc.Count = 1, rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    t.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    HRESULT hr = dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, t.state, nullptr, IID_PPV_ARGS(&t.res));
    if (FAILED(hr)) return ditto_log("CreateCommittedResource %ux%u 0x%08x\n", w, h, (unsigned)hr), false;
    t.res->SetName(L"DittoSR");
    t.fmt = f, t.w = w, t.h = h;
    return true;
}

void DittoSR::releaseTextures() {
    for (Tex *t : {&mda_[0], &mda_[1], &hist_[0], &hist_[1], &aux_[0], &aux_[1], &stab_[0], &stab_[1], &change1_, &expo_[0], &expo_[1],
                   &env_[0], &env_[1], &aoNd_, &aoRes_, &aoCol_, &aoH_[0], &aoH_[1], &aoHD_[0], &aoHD_[1], &aoFg_, &aoFg2_, &aoFgH_[0], &aoFgH_[1]})
        if (t->res) t->res->Release(), *t = Tex();
    texValid_ = false;
}

bool DittoSR::ensureTextures(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, DXGI_FORMAT hf, DXGI_FORMAT sf, bool *made) {
    *made = false;
    if (texValid_ && mda_[0].w == rw && mda_[0].h == rh && hist_[0].w == ow && hist_[0].h == oh && hist_[0].fmt == hf && stab_[0].fmt == sf) return true;
    *made = true;
    // (a new size: the game waits for the GPU before it changes its resolution, as FSR requires; a new history format:
    // the reload key - a frame's hitch)
    releaseTextures();
    bool ok = makeTex(mda_[0], rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT) && makeTex(mda_[1], rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
              makeTex(hist_[0], ow, oh, hf) && makeTex(hist_[1], ow, oh, hf) &&
              makeTex(aux_[0], rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM) && makeTex(aux_[1], rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM) &&
              makeTex(env_[0], rw, rh, DXGI_FORMAT_R8G8_UNORM) && makeTex(env_[1], rw, rh, DXGI_FORMAT_R8G8_UNORM) &&
              makeTex(stab_[0], ow, oh, sf) && makeTex(stab_[1], ow, oh, sf) &&
              makeTex(change1_, 1, 1, DXGI_FORMAT_R16_FLOAT) && makeTex(expo_[0], 1, 1, DXGI_FORMAT_R32_FLOAT) &&
              makeTex(expo_[1], 1, 1, DXGI_FORMAT_R32_FLOAT);
    if (!ok) return releaseTextures(), false;
    texValid_ = true;
    ditto_log("textures: render %ux%u, output %ux%u, history %s\n", rw, rh, ow, oh, hf == DXGI_FORMAT_R10G10B10A2_UNORM ? "RGB10A2" : "RGBA16F");
    return true;
}

// The game's output takes DittoSR's result as a typed UAV store in its own format: it must allow unordered access, and
// the driver must take typed stores in that format (required at feature level 11_0 for the formats games hand FSR, so
// both should hold - a report of black output on Polaris, its output R11G11B10_FLOAT, asked). Checked and logged once
// per format; false: not written blind - DittoSR steps aside (the game's FSR instead, where there is one).
bool DittoSR::outputWritable(const D3D12_RESOURCE_DESC &od) {
    bool uav = (od.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
    if (od.Format == outFmt_ && uav == outUav_) return outOk_;
    outFmt_ = od.Format, outUav_ = uav;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {view_format(od.Format, true)};
    bool store = SUCCEEDED(dev_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)) &&
                 (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) && (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
    outOk_ = uav && store;
    ditto_log("output: dxgi %d, %s, typed UAV stores %s%s\n", (int)od.Format,
              uav ? "unordered access allowed" : "NOT created for unordered access", store ? "supported" : "NOT reported supported",
              outOk_ ? "" : " - DittoSR does not write it");
    return outOk_;
}

void DittoSR::transition(ID3D12GraphicsCommandList *cl, Tex &t, D3D12_RESOURCE_STATES to) {
    if (t.state == to) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t.res, b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = t.state, b.Transition.StateAfter = to;
    cl->ResourceBarrier(1, &b);
    t.state = to;
}

D3D12_GPU_DESCRIPTOR_HANDLE DittoSR::table(ID3D12Resource *const *srv, const DXGI_FORMAT *srvFmt, ID3D12Resource *const *uav,
                                           const DXGI_FORMAT *uavFmt) {
    if (heapNext_ + kTable > heapSize_) heapNext_ = 0;
    UINT base = heapNext_;
    heapNext_ += kTable;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)base * descInc_, gpu.ptr += (UINT64)base * descInc_;
    for (UINT i = 0; i < kTable; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = {cpu.ptr + (SIZE_T)i * descInc_};
        if (i < kSrvs) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = srv[i] ? view_format(srvFmt[i], false) : DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            dev_->CreateShaderResourceView(srv[i], &sd, h);
        } else {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
            ID3D12Resource *r = uav[i - kSrvs];
            ud.Format = r ? view_format(uavFmt[i - kSrvs], true) : DXGI_FORMAT_R8G8B8A8_UNORM;
            ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            dev_->CreateUnorderedAccessView(r, nullptr, &ud, h);
        }
    }
    return gpu;
}

bool ditto_frame_from_ffx(const ffxDispatchDescUpscale *d, const DittoCreateInfo &ci, DittoFrame *f) {
    *f = DittoFrame();
    f->cl = (ID3D12GraphicsCommandList *)d->commandList;
    f->color = (ID3D12Resource *)d->color.resource, f->depth = (ID3D12Resource *)d->depth.resource;
    f->mv = (ID3D12Resource *)d->motionVectors.resource, f->output = (ID3D12Resource *)d->output.resource;
    f->exposure = (ID3D12Resource *)d->exposure.resource, f->reactive = (ID3D12Resource *)d->reactive.resource;
    if (!f->cl || !f->color || !f->depth || !f->mv || !f->output) return false;
    f->colorState = d3d_state(d->color.state), f->depthState = d3d_state(d->depth.state), f->mvState = d3d_state(d->motionVectors.state);
    f->exposureState = d3d_state(d->exposure.state), f->reactiveState = d3d_state(d->reactive.state);
    f->outputState = d3d_state(d->output.state);
    D3D12_RESOURCE_DESC cd = f->color->GetDesc(), od = f->output->GetDesc();
    f->rw = d->renderSize.width ? d->renderSize.width : (uint32_t)cd.Width;
    f->rh = d->renderSize.height ? d->renderSize.height : cd.Height;
    f->ow = d->upscaleSize.width ? d->upscaleSize.width : ci.maxUpscaleW;
    f->oh = d->upscaleSize.height ? d->upscaleSize.height : ci.maxUpscaleH;
    if (!f->ow || !f->oh) f->ow = (uint32_t)od.Width, f->oh = od.Height;
    f->jitX = d->jitterOffset.x, f->jitY = d->jitterOffset.y;
    bool displayMv = (ci.flags & FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) != 0;
    f->mvRes[0] = displayMv ? (float)f->ow : (float)f->rw, f->mvRes[1] = displayMv ? (float)f->oh : (float)f->rh;
    // (FSR: the previous uv = uv + mv x scale / the vectors' resolution)
    f->mvToUv[0] = d->motionVectorScale.x / f->mvRes[0], f->mvToUv[1] = d->motionVectorScale.y / f->mvRes[1];
    f->jitteredMv = (ci.flags & FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION) != 0;
    f->depthInverted = (ci.flags & FFX_UPSCALE_ENABLE_DEPTH_INVERTED) != 0;
    f->hdr = (ci.flags & FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE) != 0;
    f->preExposure = d->preExposure > 0.0f ? d->preExposure : 1.0f;
    f->fovY = d->cameraFovAngleVertical, f->zNear = d->cameraNear, f->zFar = d->cameraFar;
    f->reset = d->reset;
    return true;
}

bool DittoSR::dispatch(const DittoFrame &fr, bool forceReset) {
    if (!ok_ || !fr.cl || !fr.color || !fr.mv || !fr.output || !fr.rw || !fr.rh || !fr.ow || !fr.oh) return false;
    ID3D12GraphicsCommandList *cl = fr.cl;
    ID3D12Resource *color = fr.color, *depth = fr.depth, *mv = fr.mv, *out = fr.output, *expo = fr.exposure, *reac = fr.reactive;
    // (no depth - an API that leaves it out: the convert pass reads 0 everywhere: the centre's motion, no disocclusion)
    D3D12_RESOURCE_DESC cd = color->GetDesc(), dd = depth ? depth->GetDesc() : D3D12_RESOURCE_DESC{}, md = mv->GetDesc(), od = out->GetDesc();
    uint32_t rw = fr.rw, rh = fr.rh, ow = fr.ow, oh = fr.oh;
    if (!outputWritable(od)) return false;
    DXGI_FORMAT hfmt = s_.history10 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
    // (the stability levels R8, or R8G8 with the anti-shimmer's state)
    DXGI_FORMAT sfmt = s_.antiShimmer != 0.0f ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
    bool resized;
    if (!ensureTextures(rw, rh, ow, oh, hfmt, sfmt, &resized)) return false;
    bool reset = fr.reset || forceReset || resized;

    // the game's resources: read as shader resources, the output as a UAV, then back to the states they came in
    struct Use { ID3D12Resource *r; D3D12_RESOURCE_STATES from, to; } uses[6];
    int nu = 0;
    auto use = [&](ID3D12Resource *r, D3D12_RESOURCE_STATES s, D3D12_RESOURCE_STATES need) {
        if (!r) return;
        if ((s & need) == need && !(need == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && s != need)) return;
        D3D12_RESOURCE_STATES to = need == D3D12_RESOURCE_STATE_UNORDERED_ACCESS ? need : (s & D3D12_RESOURCE_STATE_DEPTH_READ) | need;
        uses[nu++] = {r, s, to};
    };
    use(color, fr.colorState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    use(depth, fr.depthState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    use(mv, fr.mvState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    use(expo, fr.exposureState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    use(reac, fr.reactiveState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    use(out, fr.outputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_BARRIER bs[6] = {};
    for (int i = 0; i < nu; i++) {
        bs[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bs[i].Transition.pResource = uses[i].r, bs[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bs[i].Transition.StateBefore = uses[i].from, bs[i].Transition.StateAfter = uses[i].to;
    }
    if (nu) cl->ResourceBarrier(nu, bs);
    // (the passes' GPU time: the slot written kTimeRing - 1 frames ago read, this frame's started; a compute or direct
    // list both take timestamps)
    uint32_t ts = (uint32_t)(frames_ % kTimeRing);
    if (tickMs_ > 0.0) {
        const uint64_t *st = timeMap_ + kStamps * ts;
        uint64_t a = st[0], b = st[kStamps - 1];
        if (frames_ >= kTimeRing && b > a && (b - a) * tickMs_ < 1000.0) {
            g_stats.upscaleMs = (float)((b - a) * tickMs_);
            if (g_cfg.logPasses) {
                for (uint32_t i = 0; i < 4; i++) passMs_[i] += st[i + 1] >= st[i] ? (st[i + 1] - st[i]) * tickMs_ : 0.0;
                if (++passN_ == 256) {
                    ditto_log("passes: convert %.3f, motion %.3f, upscale %.3f, sharpen %.3f, all %.3f ms (GPU, the mean of 256)\n",
                              passMs_[0] / 256, passMs_[1] / 256, passMs_[2] / 256, passMs_[3] / 256,
                              (passMs_[0] + passMs_[1] + passMs_[2] + passMs_[3]) / 256);
                    passMs_[0] = passMs_[1] = passMs_[2] = passMs_[3] = 0.0, passN_ = 0;
                }
            }
        }
        cl->EndQuery(timeQ_, D3D12_QUERY_TYPE_TIMESTAMP, kStamps * ts);
    }

    // the constants
    DittoConstants k;
    // (the auto exposure: the game set FSR's flag and gave no exposure - measured in the convert pass, its 1 x 1 the
    // exposure the passes read)
    bool autoExpo = !expo && (ci_.flags & FFX_UPSCALE_ENABLE_AUTO_EXPOSURE) && rw >= 16;
    ditto_fill_constants(k, s_, fr, reset, expo != nullptr || autoExpo, reac != nullptr, hist_[0].fmt == DXGI_FORMAT_R10G10B10A2_UNORM, prevJit_);
    k.autoExposure = autoExpo ? 1.0f : 0.0f;
    if (split_) k.splitX = (float)(ow / 2);
    // (the sharpening in the upscale: its own pass skipped - not with the comparison, nor a debug view)
    // (up to 2.5x: above it the separate pass - its moving clamp to the output's 4 neighbours holds the shimmer of a
    // 3x pan, which the fused term's bound by the samples' range cannot: grass at 3x 2.70 against 2.24, as sharp)
    // (RCAS: the sharpening pass at every ratio - the fused term is the blend's detail, not a filter of the output)
    k.fusedSharpen = !split_ && !s_.debugView && !k.sharpenMode && s_.fusedSharpen > 0.0f && k.ratio <= 2.55f ? s_.fusedSharpen : 0.0f;
    // (a float colour declared LDR is read as HDR: as [0, 1] colour it would be clipped at 1 - Witcher 3 through
    // OptiScaler, its highlights flattened)
    if (!fr.hdr && float_format(cd.Format)) {
        if (!hdrFromFormat_) ditto_log("colour: float format %d without the HDR flag - read as HDR\n", (int)cd.Format), hdrFromFormat_ = true;
        k.hdr = 1.0f;
    }
    bool fp16 = psoUpscale16_ && (s_.fp16 > 0 || (s_.fp16 < 0 && amd_));
    if (fp16 != lastFp16_ || frames_ == 0) ditto_log("shaders: %s\n", fp16 ? "FP16" : "FP32"), lastFp16_ = fp16;

    cur_ ^= 1;
    Tex &mdaC = mda_[cur_], &mdaP = mda_[cur_ ^ 1], &histC = hist_[cur_], &histP = hist_[cur_ ^ 1];
    Tex &auxC = aux_[cur_], &auxP = aux_[cur_ ^ 1], &stabC = stab_[cur_], &stabP = stab_[cur_ ^ 1];
    ID3D12DescriptorHeap *heaps[1] = {heap_};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(rootSig_);
    auto run = [&](ID3D12PipelineState *pso, ID3D12Resource *const srv[kSrvs], const DXGI_FORMAT sf[kSrvs], ID3D12Resource *const uav[kUavs],
                   const DXGI_FORMAT uf[kUavs], UINT gx, UINT gy) {
        cl->SetPipelineState(pso);
        cl->SetComputeRoot32BitConstants(0, kConstants, &k, 0);
        cl->SetComputeRootDescriptorTable(1, table(srv, sf, uav, uf));
        cl->Dispatch(gx, gy, 1);
    };
    DXGI_FORMAT rf = reac ? reac->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
    const DXGI_FORMAT X = DXGI_FORMAT_UNKNOWN;
    Tex &expoC = expo_[cur_], &expoP = expo_[cur_ ^ 1];
    ID3D12Resource *expoIn = expo;  // (the convert pass's t5: the game's, or the last frame's auto exposure)
    if (autoExpo) {
        transition(cl, expoP, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(cl, expoC, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        expoIn = expoP.res;
    }
    DXGI_FORMAT exfIn = expoIn ? expoIn->GetDesc().Format : DXGI_FORMAT_UNKNOWN;

    // (EXPERIMENTAL, Ditto Relight: ambient occlusion and global illumination after CORTAO - aoprep, aomain, aocompose at
    // render size - into aoCol_, the colour every later pass reads in the game's place; their own root constants)
    DXGI_FORMAT cfmt = cd.Format;
    if (s_.relight && depth && !s_.debugView) {
        bool made = aoNd_.res && aoNd_.w == rw && aoNd_.h == rh;
        if (!made) {
            for (Tex *t : {&aoNd_, &aoRes_, &aoCol_, &aoH_[0], &aoH_[1], &aoHD_[0], &aoHD_[1], &aoFg_, &aoFg2_, &aoFgH_[0], &aoFgH_[1]})
                if (t->res) t->res->Release(), *t = Tex();
            made = makeTex(aoNd_, rw, rh, DXGI_FORMAT_R32G32B32A32_FLOAT) && makeTex(aoRes_, rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
                   makeTex(aoCol_, rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT);
            aoOk_ = false;
        }
        if (made) {
            struct AoConstants {
                float rs[2], rsRcp[2], tanX, tanY, zNear, zFar, inverted, frame, aoStrength, giStrength, radiusWorld, radiusScreen, rays,
                    contact, giDark, sCurve, pivot, aoTemporal, giMode, fgDiv, fgRays, fgSteps, fgRadius, fgThick, fgBias, fgGain, fgClamp,
                    fgMaxDist, fgRefine, fgReject, fgSpatial, fgTemporal, fgPhase, fgAxis, fgMvScale[2], fgMvSize[2], pad[2];
            } ka = {};
            ka.rs[0] = (float)rw, ka.rs[1] = (float)rh, ka.rsRcp[0] = 1.0f / rw, ka.rsRcp[1] = 1.0f / rh;
            float fov = fr.fovY > 0.01f && fr.fovY < 3.1f ? fr.fovY : 1.0472f;  // (none given - XeSS: 60 degrees)
            ka.tanY = tanf(0.5f * fov), ka.tanX = ka.tanY * (float)rw / (float)rh;
            ka.zNear = fr.zNear > 0.0f ? fr.zNear : 0.1f, ka.zFar = fr.zFar > 0.0f ? fr.zFar : 100000.0f;
            ka.inverted = fr.depthInverted ? 1.0f : 0.0f, ka.frame = (float)(frames_ % 1024);
            ka.aoStrength = s_.aoStrength < 0.0f ? 0.0f : s_.aoStrength > 1.0f ? 1.0f : s_.aoStrength;
            ka.giStrength = s_.giStrength < 0.0f ? 0.0f : s_.giStrength > 4.0f ? 4.0f : s_.giStrength;
            ka.giDark = s_.giDark < 0.0f ? 0.0f : s_.giDark > 4.0f ? 4.0f : s_.giDark;
            ka.sCurve = s_.sCurve < 0.0f ? 0.0f : s_.sCurve > 1.0f ? 1.0f : s_.sCurve;
            ka.pivot = fr.hdr || float_format(cd.Format) ? 0.071f : 0.3f;  // (30 % of the brightness: linear HDR 0.3 ^ 2.2, or [0, 1] colour)
            ka.radiusWorld = s_.aoRadius, ka.radiusScreen = s_.aoScreen > 0.01f ? s_.aoScreen : 0.15f;
            ka.rays = s_.aoRays < 4.0f ? 4.0f : s_.aoRays > 32.0f ? 32.0f : floorf(s_.aoRays + 0.5f);
            ka.contact = s_.aoContact < 0.0f ? 0.0f : s_.aoContact > 4.0f ? 4.0f : s_.aoContact;
            ka.giMode = s_.giMode == 1 ? 1.0f : 0.0f;
            // (FastGI's options, clamped as FastGI clamps them)
            ka.fgDiv = (float)(s_.fgDiv < 2 ? 2 : s_.fgDiv > 4 ? 4 : s_.fgDiv);
            ka.fgRays = s_.fgRays < 1.0f ? 1.0f : s_.fgRays > 8.0f ? 8.0f : floorf(s_.fgRays + 0.5f);
            ka.fgSteps = s_.fgSteps < 4.0f ? 4.0f : s_.fgSteps > 32.0f ? 32.0f : floorf(s_.fgSteps + 0.5f);
            ka.fgRadius = s_.fgRadius > 0.1f ? s_.fgRadius : 0.1f, ka.fgThick = s_.fgThickness > 0.01f ? s_.fgThickness : 0.01f;
            ka.fgBias = s_.fgNormalBias > 0.001f ? s_.fgNormalBias : 0.001f;
            ka.fgGain = (s_.fgIntensity < 0.0f ? 0.0f : s_.fgIntensity > 4.0f ? 4.0f : s_.fgIntensity) * (s_.fgReflectance < 0.0f ? 0.0f : s_.fgReflectance > 1.0f ? 1.0f : s_.fgReflectance);
            ka.fgClamp = s_.fgClamp > 0.1f ? s_.fgClamp : 0.1f, ka.fgMaxDist = (s_.fgMaxDist < 1.0f ? 1.0f : s_.fgMaxDist > 60000.0f ? 60000.0f : s_.fgMaxDist);
            ka.fgRefine = s_.fgRefine ? 1.0f : 0.0f, ka.fgReject = (s_.fgRejection < 0.0f ? 0.0f : s_.fgRejection > 1.0f ? 1.0f : s_.fgRejection);
            ka.fgSpatial = (s_.fgSpatial < 1.0f ? 1.0f : s_.fgSpatial > 2.0f ? 2.0f : s_.fgSpatial);
            ka.fgPhase = (float)(frames_ % 8) * 0.618033989f;  // (the noise moving: DittoSR's accumulation averages it)
            ka.fgMvScale[0] = s_.mvSign * fr.mvToUv[0], ka.fgMvScale[1] = s_.mvSign * fr.mvToUv[1];
            ka.fgMvSize[0] = fr.mvRes[0], ka.fgMvSize[1] = fr.mvRes[1];
            UINT dv = (UINT)ka.fgDiv, hw = (rw + dv - 1) / dv, hh = (rh + dv - 1) / dv;  // (FastGI's GI: its low resolution)
            auto runA = [&](ID3D12PipelineState *pso, ID3D12Resource *const srv[kSrvs], const DXGI_FORMAT sf[kSrvs], ID3D12Resource *const uav[kUavs],
                            const DXGI_FORMAT uf[kUavs], bool half = false) {
                cl->SetPipelineState(pso);
                cl->SetComputeRoot32BitConstants(0, sizeof ka / 4, &ka, 0);
                cl->SetComputeRootDescriptorTable(1, table(srv, sf, uav, uf));
                cl->Dispatch(((half ? hw : rw) + 7) / 8, ((half ? hh : rh) + 7) / 8, 1);
            };
            // (FastGI's: its intensity 0 skips its GI - none added; its textures made at its resolution, the history
            // reset with them)
            bool fg = ka.giMode > 0.5f && ka.fgGain > 0.0f;
            bool fgT = fg && s_.aoTemporal;
            if (fg && !(aoFg_.res && aoFg_.w == hw && aoFg_.h == hh)) {
                for (Tex *t : {&aoFg_, &aoFg2_, &aoFgH_[0], &aoFgH_[1]})
                    if (t->res) t->res->Release(), *t = Tex();
                fg = makeTex(aoFg_, hw, hh, DXGI_FORMAT_R16G16B16A16_FLOAT) && makeTex(aoFg2_, hw, hh, DXGI_FORMAT_R16G16B16A16_FLOAT);
                fgOk_ = false;
            }
            if (fgT && !aoFgH_[0].res)
                fgT = makeTex(aoFgH_[0], hw, hh, DXGI_FORMAT_R16G16B16A16_FLOAT) && makeTex(aoFgH_[1], hw, hh, DXGI_FORMAT_R16G16B16A16_FLOAT);
            fgT = fgT && fg;
            // (the history valid: written the last frame, at this resolution, no reset)
            bool fgValid = fgT && fgOk_ && fgLast_ + 1 == frames_ && !reset;
            float tw = s_.aoTemporalWeight < 0.0f ? 0.0f : s_.aoTemporalWeight > 0.95f ? 0.95f : s_.aoTemporalWeight;
            ka.fgTemporal = fgValid ? tw : 0.0f;
            // (Ditto's AO and GI's temporal smoothing: its history at render size, made when first on; valid as FastGI's)
            bool aoT = s_.aoTemporal;
            if (aoT && !aoH_[0].res)
                aoT = makeTex(aoH_[0], rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT) && makeTex(aoH_[1], rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
                      makeTex(aoHD_[0], rw, rh, DXGI_FORMAT_R32_FLOAT) && makeTex(aoHD_[1], rw, rh, DXGI_FORMAT_R32_FLOAT);
            bool aoValid = aoT && aoOk_ && aoLast_ + 1 == frames_ && !reset;
            ka.aoTemporal = aoValid ? tw : 0.0f;
            Tex *fgOut = &aoFg_;
            if (fg && ka.giStrength <= 0.0f) ka.giStrength = 1.0f;  // (Ditto's GI strength unused: not 0, the passes run)
            if (!fg && ka.giMode > 0.5f) ka.giStrength = 0.0f;  // (FastGI's intensity 0, or no textures: no GI)
            ka.giMode = fg ? 1.0f : 0.0f;
            transition(cl, aoNd_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            {
                ID3D12Resource *s[kSrvs] = {depth};
                DXGI_FORMAT f[kSrvs] = {dd.Format};
                ID3D12Resource *u[kUavs] = {aoNd_.res};
                DXGI_FORMAT uf[kUavs] = {aoNd_.fmt};
                runA(psoAoPrep_, s, f, u, uf);
            }
            transition(cl, aoNd_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            transition(cl, aoRes_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            {
                ID3D12Resource *s[kSrvs] = {aoNd_.res, color};
                DXGI_FORMAT f[kSrvs] = {aoNd_.fmt, cd.Format};
                ID3D12Resource *u[kUavs] = {aoRes_.res};
                DXGI_FORMAT uf[kUavs] = {aoRes_.fmt};
                runA(psoAoMain_, s, f, u, uf);
            }
            transition(cl, aoRes_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (fg) {  // (FastGI's GI: Trace, BlurHorizontal, BlurVertical, Temporal - at its low resolution)
                transition(cl, aoFg_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                {
                    ID3D12Resource *s[kSrvs] = {aoNd_.res, color, depth};
                    DXGI_FORMAT f[kSrvs] = {aoNd_.fmt, cd.Format, dd.Format};
                    ID3D12Resource *u[kUavs] = {aoFg_.res};
                    DXGI_FORMAT uf[kUavs] = {aoFg_.fmt};
                    runA(psoAoFgTrace_, s, f, u, uf, true);
                }
                for (int axis = 0; axis < 2; axis++) {  // (x into the scratch, y back)
                    Tex &src = axis ? aoFg2_ : aoFg_, &dst = axis ? aoFg_ : aoFg2_;
                    transition(cl, src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    transition(cl, dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    ka.fgAxis = (float)axis;
                    ID3D12Resource *s[kSrvs] = {aoNd_.res, src.res};
                    DXGI_FORMAT f[kSrvs] = {aoNd_.fmt, src.fmt};
                    ID3D12Resource *u[kUavs] = {dst.res};
                    DXGI_FORMAT uf[kUavs] = {dst.fmt};
                    runA(psoAoFgBlur_, s, f, u, uf, true);
                }
                transition(cl, aoFg_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if (fgT) {
                    Tex &cur = aoFgH_[fgCur_], &prev = aoFgH_[fgCur_ ^ 1];
                    transition(cl, prev, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    transition(cl, cur, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    // (no valid history: never read - the filtered GI in its place, its weight 0)
                    ID3D12Resource *s[kSrvs] = {aoNd_.res, aoFg_.res, fgValid ? prev.res : aoFg_.res, mv};
                    DXGI_FORMAT f[kSrvs] = {aoNd_.fmt, aoFg_.fmt, fgValid ? prev.fmt : aoFg_.fmt, md.Format};
                    ID3D12Resource *u[kUavs] = {cur.res};
                    DXGI_FORMAT uf[kUavs] = {cur.fmt};
                    runA(psoAoFgTemporal_, s, f, u, uf, true);
                    transition(cl, cur, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    fgOut = &cur;
                    fgCur_ ^= 1, fgOk_ = true, fgLast_ = frames_;
                }
            }
            if (!fgT) fgOk_ = false;
            Tex *aoOut = &aoRes_;
            if (aoT) {  // (the temporal smoothing, render size: no valid history - never read, the main pass' in its place)
                Tex &cur = aoH_[aoCur_], &prev = aoH_[aoCur_ ^ 1], &curD = aoHD_[aoCur_], &prevD = aoHD_[aoCur_ ^ 1];
                transition(cl, prev, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                transition(cl, prevD, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                transition(cl, cur, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                transition(cl, curD, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                ID3D12Resource *s[kSrvs] = {aoNd_.res, aoRes_.res, aoValid ? prev.res : aoRes_.res, prevD.res, mv};
                DXGI_FORMAT f[kSrvs] = {aoNd_.fmt, aoRes_.fmt, aoValid ? prev.fmt : aoRes_.fmt, prevD.fmt, md.Format};
                ID3D12Resource *u[kUavs] = {cur.res, curD.res};
                DXGI_FORMAT uf[kUavs] = {cur.fmt, curD.fmt};
                runA(psoAoTemporal_, s, f, u, uf);
                transition(cl, cur, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                aoOut = &cur;
                aoCur_ ^= 1, aoOk_ = true, aoLast_ = frames_;
            } else {
                aoOk_ = false;
            }
            transition(cl, aoCol_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            {
                ID3D12Resource *s[kSrvs] = {aoNd_.res, color, aoOut->res, fg ? fgOut->res : nullptr};
                DXGI_FORMAT f[kSrvs] = {aoNd_.fmt, cd.Format, aoOut->fmt, fgOut->fmt};
                ID3D12Resource *u[kUavs] = {aoCol_.res};
                DXGI_FORMAT uf[kUavs] = {aoCol_.fmt};
                runA(psoAoComp_, s, f, u, uf);
            }
            transition(cl, aoCol_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            color = aoCol_.res, cfmt = aoCol_.fmt;
        }
    }

    // convert (render size), and in its extra row's first group the change against the history (1 x 1)
    transition(cl, mdaC, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(cl, change1_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(cl, histP, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    {
        ID3D12Resource *s[kSrvs] = {depth, mv, color, histP.res, nullptr, expoIn};
        DXGI_FORMAT f[kSrvs] = {dd.Format, md.Format, cfmt, histP.fmt, X, exfIn};
        ID3D12Resource *u[kUavs] = {mdaC.res, change1_.res, autoExpo ? expoC.res : nullptr};
        DXGI_FORMAT uf[kUavs] = {mdaC.fmt, change1_.fmt, autoExpo ? expoC.fmt : X};
        run(psoConvert_, s, f, u, uf, (rw + 7) / 8, (rh + 7) / 8 + 1);
    }
    if (tickMs_ > 0.0) cl->EndQuery(timeQ_, D3D12_QUERY_TYPE_TIMESTAMP, kStamps * ts + 1);
    // (the later passes read this frame's exposure: the auto one just measured, or the game's)
    if (autoExpo) transition(cl, expoC, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE), expo = expoC.res;
    DXGI_FORMAT exf = expo ? expo->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
    transition(cl, mdaC, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cl, change1_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cl, mdaP, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cl, auxP, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // motion (render size): the motion checks, the derived reactive mask - at 1x in the upscale (fm: one render pixel an
    // output pixel, its checks no longer repeated; the colour's 3x3 in the upscale's tile already; one pass, one read of
    // the convert pass' and of the colour less - 0.2 ms at 4K)
    bool as = s_.antiShimmer != 0.0f;
    ID3D12PipelineState *fmPso = rw == ow && rh == oh ? psoUpscaleFm_[fp16 ? 1 : 0][as ? 1 : 0] : nullptr;
    Tex &envC = env_[cur_], &envP = env_[cur_ ^ 1];  // (the luma envelope: the previous frame's, this one's)
    transition(cl, envP, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cl, envC, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(cl, auxC, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!fmPso) {
        ID3D12Resource *s[kSrvs] = {auxP.res, mdaC.res, color, mdaP.res, envP.res, expo};
        DXGI_FORMAT f[kSrvs] = {auxP.fmt, mdaC.fmt, cfmt, mdaP.fmt, envP.fmt, exf};
        ID3D12Resource *u[kUavs] = {auxC.res, envC.res};
        DXGI_FORMAT uf[kUavs] = {auxC.fmt, envC.fmt};
        run(psoMotion_, s, f, u, uf, (rw + 7) / 8, (rh + 7) / 8);
        transition(cl, auxC, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (tickMs_ > 0.0) cl->EndQuery(timeQ_, D3D12_QUERY_TYPE_TIMESTAMP, kStamps * ts + 2);
    // upscale (output size): the history, the stability levels (and the game's output, a debug view)
    transition(cl, histC, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(cl, stabC, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(cl, stabP, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    {
        // (fm: t3 unused - the motion pass' own inputs t8 - t10, its outputs u3, u4)
        ID3D12Resource *s[kSrvs] = {histP.res, mdaC.res, color, fmPso ? nullptr : auxC.res, change1_.res, expo, reac, stabP.res,
                                    fmPso ? auxP.res : nullptr, fmPso ? mdaP.res : nullptr, fmPso ? envP.res : nullptr};
        DXGI_FORMAT f[kSrvs] = {histP.fmt, mdaC.fmt, cfmt, fmPso ? X : auxC.fmt, change1_.fmt, exf, rf, stabP.fmt, auxP.fmt, mdaP.fmt, envP.fmt};
        ID3D12Resource *u[kUavs] = {histC.res, out, stabC.res, fmPso ? auxC.res : nullptr, fmPso ? envC.res : nullptr};
        DXGI_FORMAT uf[kUavs] = {histC.fmt, od.Format, stabC.fmt, auxC.fmt, envC.fmt};
        run(fmPso ? fmPso : fp16 ? (as ? psoUpscaleAs16_ : psoUpscale16_) : (as ? psoUpscaleAs_ : psoUpscale_), s, f, u, uf, (ow + kUpGX - 1) / kUpGX,
            (oh + kUpGY - 1) / kUpGY);
    }
    if (fmPso) transition(cl, auxC, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (tickMs_ > 0.0) cl->EndQuery(timeQ_, D3D12_QUERY_TYPE_TIMESTAMP, kStamps * ts + 3);
    transition(cl, histC, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cl, stabC, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // sharpen (output size): the history into the game's output
    if (!s_.debugView && k.fusedSharpen <= 0.0f) {  // (at sharpness 0 a soft blur; a debug view or fused: the upscale writes it)
        ID3D12Resource *s[kSrvs] = {histC.res, mdaC.res, nullptr, nullptr, nullptr, expo, nullptr, stabC.res};
        DXGI_FORMAT f[kSrvs] = {histC.fmt, mdaC.fmt, X, X, X, exf, X, stabC.fmt};
        ID3D12Resource *u[kUavs] = {nullptr, out};
        DXGI_FORMAT uf[kUavs] = {X, od.Format};
        run(fp16 ? psoSharpen16_ : psoSharpen_, s, f, u, uf, (ow + 7) / 8, (oh + 7) / 8);
    }

    // the game's resources back to their states (the output's UAV writes done first)
    for (int i = 0; i < nu; i++) {
        bs[i].Transition.StateBefore = uses[i].to, bs[i].Transition.StateAfter = uses[i].from;
    }
    D3D12_RESOURCE_BARRIER ub = {};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV, ub.UAV.pResource = out;
    cl->ResourceBarrier(1, &ub);
    if (nu) cl->ResourceBarrier(nu, bs);
    if (tickMs_ > 0.0) {
        cl->EndQuery(timeQ_, D3D12_QUERY_TYPE_TIMESTAMP, kStamps * ts + 4);
        cl->ResolveQueryData(timeQ_, D3D12_QUERY_TYPE_TIMESTAMP, kStamps * ts, kStamps, timeRb_, 8 * kStamps * ts);
    }
    frames_++;
    return true;
}
