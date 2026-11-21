// DittoSR on D3D11 (dittosr_dx11.h): convert (render size; with the change against the history in an extra group) ->
// motion (render size; at 1x fused into the upscale) -> upscale (output size) -> sharpen (output size), the same
// textures as on D3D12 - the history, the stability levels, the convert and motion passes' outputs each a ping-pong pair.
// D3D11 tracks the hazards between passes itself; a pass binds its UAVs before its shader resources, so a texture the
// last pass wrote is unbound from its UAV slot before it is read.
#include "dittosr_dx11.h"

#include <string.h>

#include "constants.h"
#include "dxgi_formats.h"

#include "cs11_convert.h"
#include "cs11_motion.h"
#include "cs11_sharpen.h"
#include "cs11_upscale.h"
#include "cs11_upscale_as.h"
#include "cs11_upscale_as_fm.h"
#include "cs11_upscale_fm.h"

namespace {

constexpr UINT kSrvs = 11, kUavs = 5;      // (t8 - t10, u3, u4: the 1x upscale's motion pass)
constexpr UINT kUpGX = 16, kUpGY = 8;      // (the upscale's group: upscale.hlsl GX, GY)
constexpr UINT kCbSize = (sizeof(DittoConstants) + 15) & ~15u;

template <class T> void release(T *&p) {
    if (p) p->Release(), p = nullptr;
}

// The compute stage's bindings DittoSR uses, as the game left them
struct SavedCs {
    ID3D11ComputeShader *cs = nullptr;
    ID3D11Buffer *cb = nullptr;
    ID3D11SamplerState *samp = nullptr;
    ID3D11ShaderResourceView *srv[kSrvs] = {};
    ID3D11UnorderedAccessView *uav[kUavs] = {};
    void save(ID3D11DeviceContext *c) {
        c->CSGetShader(&cs, nullptr, nullptr);
        c->CSGetConstantBuffers(0, 1, &cb);
        c->CSGetSamplers(0, 1, &samp);
        c->CSGetShaderResources(0, kSrvs, srv);
        c->CSGetUnorderedAccessViews(0, kUavs, uav);
    }
    void restore(ID3D11DeviceContext *c) {
        ID3D11UnorderedAccessView *none[kUavs] = {};
        c->CSSetUnorderedAccessViews(0, kUavs, none, nullptr);
        c->CSSetShaderResources(0, kSrvs, srv);
        UINT keep[kUavs] = {~0u, ~0u, ~0u, ~0u, ~0u};  // (-1: append / counter offsets as they were)
        c->CSSetUnorderedAccessViews(0, kUavs, uav, keep);
        c->CSSetShader(cs, nullptr, 0);
        c->CSSetConstantBuffers(0, 1, &cb);
        c->CSSetSamplers(0, 1, &samp);
        release(cs), release(cb), release(samp);
        for (auto *&v : srv) release(v);
        for (auto *&v : uav) release(v);
    }
};

DXGI_FORMAT tex_format(ID3D11Resource *r, D3D11_TEXTURE2D_DESC *out = nullptr) {
    D3D11_TEXTURE2D_DESC d = {};
    ID3D11Texture2D *t = nullptr;
    if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&t))) t->GetDesc(&d), t->Release();
    if (out) *out = d;
    return d.Format;
}

}  // namespace

DittoSR11::DittoSR11(ID3D11Device *device, const DittoCreateInfo &ci, const DittoSettings &s) : dev_(device), ci_(ci), s_(s) {
    dev_->AddRef();
    ok_ = init();
}

DittoSR11::~DittoSR11() {
    releaseTextures();
    for (Times &t : times_) release(t.disjoint), release(t.start), release(t.end);
    for (ID3D11ComputeShader **c : {&csConvert_, &csMotion_, &csUpscale_, &csUpscaleAs_, &csSharpen_, &csUpscaleFm_[0], &csUpscaleFm_[1]}) release(*c);
    release(cb_), release(sampler_), release(dev_);
}

bool DittoSR11::init() {
    if (dev_->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
        return ditto_log("D3D11: feature level 0x%x - DittoSR needs 11_0\n", (unsigned)dev_->GetFeatureLevel()), false;
    struct { const BYTE *code; size_t size; ID3D11ComputeShader **cs; const char *name; } cs[] = {
        {g_cs11_convert, sizeof g_cs11_convert, &csConvert_, "convert"},
        {g_cs11_motion, sizeof g_cs11_motion, &csMotion_, "motion"},
        {g_cs11_upscale, sizeof g_cs11_upscale, &csUpscale_, "upscale"},
        {g_cs11_upscale_as, sizeof g_cs11_upscale_as, &csUpscaleAs_, "upscale (anti-shimmer)"},
        {g_cs11_sharpen, sizeof g_cs11_sharpen, &csSharpen_, "sharpen"},
    };
    for (auto &c : cs) {
        HRESULT hr = dev_->CreateComputeShader(c.code, c.size, nullptr, c.cs);
        if (FAILED(hr)) return ditto_log("D3D11 CreateComputeShader(%s) 0x%08x\n", c.name, (unsigned)hr), false;
    }
    // (the 1x variants: optional - where one fails, the motion pass as at other ratios)
    if (FAILED(dev_->CreateComputeShader(g_cs11_upscale_fm, sizeof g_cs11_upscale_fm, nullptr, &csUpscaleFm_[0]))) csUpscaleFm_[0] = nullptr;
    if (FAILED(dev_->CreateComputeShader(g_cs11_upscale_as_fm, sizeof g_cs11_upscale_as_fm, nullptr, &csUpscaleFm_[1]))) csUpscaleFm_[1] = nullptr;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = kCbSize, bd.Usage = D3D11_USAGE_DYNAMIC, bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER, bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    HRESULT hr = dev_->CreateBuffer(&bd, nullptr, &cb_);
    if (FAILED(hr)) return ditto_log("D3D11 CreateBuffer (constants) 0x%08x\n", (unsigned)hr), false;
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER, sd.MaxLOD = D3D11_FLOAT32_MAX;
    hr = dev_->CreateSamplerState(&sd, &sampler_);
    if (FAILED(hr)) return ditto_log("D3D11 CreateSamplerState 0x%08x\n", (unsigned)hr), false;
    D3D11_QUERY_DESC dq = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, tq = {D3D11_QUERY_TIMESTAMP, 0};
    for (Times &t : times_)
        if (FAILED(dev_->CreateQuery(&dq, &t.disjoint)) || FAILED(dev_->CreateQuery(&tq, &t.start)) || FAILED(dev_->CreateQuery(&tq, &t.end)))
            release(t.disjoint), release(t.start), release(t.end);
    IDXGIDevice *dx = nullptr;
    if (SUCCEEDED(dev_->QueryInterface(__uuidof(IDXGIDevice), (void **)&dx))) {
        IDXGIAdapter *ad = nullptr;
        DXGI_ADAPTER_DESC desc = {};
        if (SUCCEEDED(dx->GetAdapter(&ad))) ad->GetDesc(&desc), ad->Release();
        ditto_log("GPU: %ls (vendor 0x%04x), D3D11 feature level 0x%x\n", desc.Description, desc.VendorId, (unsigned)dev_->GetFeatureLevel());
        dx->Release();
    }
    return true;
}

bool DittoSR11::makeTex(Tex &t, uint32_t w, uint32_t h, DXGI_FORMAT f) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w, td.Height = h, td.MipLevels = 1, td.ArraySize = 1, td.Format = f, td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT, td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    HRESULT hr = dev_->CreateTexture2D(&td, nullptr, &t.res);
    if (FAILED(hr)) return ditto_log("D3D11 CreateTexture2D %ux%u fmt %d 0x%08x\n", w, h, (int)f, (unsigned)hr), false;
    if (FAILED(hr = dev_->CreateShaderResourceView(t.res, nullptr, &t.srv)) || FAILED(hr = dev_->CreateUnorderedAccessView(t.res, nullptr, &t.uav))) {
        ditto_log("D3D11 views of a %ux%u fmt %d texture 0x%08x\n", w, h, (int)f, (unsigned)hr);
        return dropTex(t), false;
    }
    t.fmt = f, t.w = w, t.h = h;
    return true;
}

void DittoSR11::dropTex(Tex &t) {
    release(t.uav), release(t.srv), release(t.res);
    t = Tex();
}

void DittoSR11::releaseTextures() {
    for (Tex *t : {&mda_[0], &mda_[1], &hist_[0], &hist_[1], &aux_[0], &aux_[1], &stab_[0], &stab_[1], &env_[0], &env_[1], &change1_, &expo_[0], &expo_[1]})
        dropTex(*t);
    texValid_ = false;
}

bool DittoSR11::ensureTextures(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, DXGI_FORMAT hf, DXGI_FORMAT sf, bool *made) {
    *made = false;
    if (texValid_ && mda_[0].w == rw && mda_[0].h == rh && hist_[0].w == ow && hist_[0].h == oh && hist_[0].fmt == hf && stab_[0].fmt == sf) return true;
    *made = true;
    releaseTextures();  // (D3D11 keeps a texture alive while the GPU still uses it)
    bool ok = makeTex(mda_[0], rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT) && makeTex(mda_[1], rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
              makeTex(hist_[0], ow, oh, hf) && makeTex(hist_[1], ow, oh, hf) &&
              makeTex(aux_[0], rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM) && makeTex(aux_[1], rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM) &&
              makeTex(env_[0], rw, rh, DXGI_FORMAT_R8G8_UNORM) && makeTex(env_[1], rw, rh, DXGI_FORMAT_R8G8_UNORM) &&
              makeTex(stab_[0], ow, oh, sf) && makeTex(stab_[1], ow, oh, sf) &&
              makeTex(change1_, 1, 1, DXGI_FORMAT_R16_FLOAT) && makeTex(expo_[0], 1, 1, DXGI_FORMAT_R32_FLOAT) &&
              makeTex(expo_[1], 1, 1, DXGI_FORMAT_R32_FLOAT);
    if (!ok) return releaseTextures(), false;
    texValid_ = true;
    ditto_log("textures: render %ux%u, output %ux%u, history %s (D3D11)\n", rw, rh, ow, oh, hf == DXGI_FORMAT_R10G10B10A2_UNORM ? "RGB10A2" : "RGBA16F");
    return true;
}

// The game's output takes the result as a typed UAV store in its own format: it must be bound for unordered access,
// and the driver must take typed stores in that format. Checked and logged once per format.
bool DittoSR11::outputWritable(const D3D11_TEXTURE2D_DESC &od) {
    bool uav = (od.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
    if (od.Format == outFmt_ && uav == outUav_) return outOk_;
    outFmt_ = od.Format, outUav_ = uav;
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 fs = {view_format(od.Format, true)};
    UINT s1 = 0;
    bool store = SUCCEEDED(dev_->CheckFormatSupport(fs.InFormat, &s1)) && (s1 & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) &&
                 SUCCEEDED(dev_->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &fs, sizeof fs)) &&
                 (fs.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE);
    outOk_ = uav && store;
    ditto_log("output: dxgi %d, %s, typed UAV stores %s%s\n", (int)od.Format,
              uav ? "bound for unordered access" : "NOT bound for unordered access", store ? "supported" : "NOT reported supported",
              outOk_ ? "" : " - DittoSR does not write it");
    return outOk_;
}

// The ring slot written kTimeRing frames ago, if the GPU is done with it (never waited for)
void DittoSR11::readTimes(ID3D11DeviceContext *imm) {
    Times &t = times_[frames_ % kTimeRing];
    if (!t.disjoint || t.frame == ~0ull) return;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
    UINT64 a = 0, b = 0;
    if (imm->GetData(t.disjoint, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || dj.Disjoint || !dj.Frequency) return;
    if (imm->GetData(t.start, &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
        imm->GetData(t.end, &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        return;
    double ms = b > a ? (double)(b - a) * 1000.0 / (double)dj.Frequency : 0.0;
    if (ms > 0.0 && ms < 1000.0) g_stats.upscaleMs = (float)ms;
}

bool DittoSR11::dispatch(const DittoFrame11 &fr, bool forceReset) {
    if (!ok_ || !fr.ctx || !fr.color || !fr.mv || !fr.output || !fr.rw || !fr.rh || !fr.ow || !fr.oh) return false;
    ID3D11DeviceContext *ctx = fr.ctx;
    D3D11_TEXTURE2D_DESC cd, dd = {}, md, od;
    DXGI_FORMAT cfmt = tex_format(fr.color, &cd), mfmt = tex_format(fr.mv, &md), ofmt = tex_format(fr.output, &od);
    DXGI_FORMAT dfmt = fr.depth ? tex_format(fr.depth, &dd) : DXGI_FORMAT_UNKNOWN;
    if (!cfmt || !mfmt || !ofmt) {
        static int logged;
        if (logged++ < 3) ditto_log("D3D11: the colour, motion vectors and output must be 2D textures\n");
        return false;
    }
    if (!outputWritable(od)) return false;
    uint32_t rw = fr.rw, rh = fr.rh, ow = fr.ow, oh = fr.oh;
    DXGI_FORMAT hfmt = s_.history10 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
    DXGI_FORMAT sfmt = s_.antiShimmer != 0.0f ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
    bool resized;
    if (!ensureTextures(rw, rh, ow, oh, hfmt, sfmt, &resized)) return false;
    bool reset = fr.reset || forceReset || resized;

    // the game's resources' views, made for this upscale (the game may hand other textures any frame)
    ID3D11ShaderResourceView *vColor = nullptr, *vDepth = nullptr, *vMv = nullptr, *vExpo = nullptr, *vReac = nullptr;
    ID3D11UnorderedAccessView *vOut = nullptr;
    auto srv = [&](ID3D11Resource *r, DXGI_FORMAT f, ID3D11ShaderResourceView **v) {
        if (!r) return true;
        D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = view_format(f, false), d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, d.Texture2D.MipLevels = 1;
        HRESULT hr = dev_->CreateShaderResourceView(r, &d, v);
        if (FAILED(hr)) ditto_log("D3D11 shader resource view of a fmt %d texture 0x%08x\n", (int)f, (unsigned)hr), *v = nullptr;
        return SUCCEEDED(hr);
    };
    DXGI_FORMAT efmt = fr.exposure ? tex_format(fr.exposure) : DXGI_FORMAT_UNKNOWN, rfmt = fr.reactive ? tex_format(fr.reactive) : DXGI_FORMAT_UNKNOWN;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = view_format(ofmt, true), ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    bool views = srv(fr.color, cfmt, &vColor) && srv(fr.depth, dfmt, &vDepth) && srv(fr.mv, mfmt, &vMv) &&
                 SUCCEEDED(dev_->CreateUnorderedAccessView(fr.output, &ud, &vOut));
    // (an exposure or reactive mask that can't be viewed: none)
    if (fr.exposure && efmt) srv(fr.exposure, efmt, &vExpo);
    if (fr.reactive && rfmt) srv(fr.reactive, rfmt, &vReac);
    if (!views) {
        for (IUnknown *v : {(IUnknown *)vColor, (IUnknown *)vDepth, (IUnknown *)vMv, (IUnknown *)vExpo, (IUnknown *)vReac, (IUnknown *)vOut})
            if (v) v->Release();
        return false;
    }

    // the constants: one set for every pass
    DittoConstants k;
    // (the auto exposure: the game set the flag and gave no exposure - measured in the convert pass, its 1 x 1 the
    // exposure the passes read)
    bool autoExpo = !vExpo && fr.autoExposure && rw >= 16;
    ditto_fill_constants(k, s_, fr, reset, vExpo != nullptr || autoExpo, vReac != nullptr, hist_[0].fmt == DXGI_FORMAT_R10G10B10A2_UNORM, prevJit_);
    k.autoExposure = autoExpo ? 1.0f : 0.0f;
    // (the sharpening in the upscale up to 2.5x, its own pass skipped - not with a debug view or RCAS: dittosr.cpp)
    k.fusedSharpen = !s_.debugView && !k.sharpenMode && s_.fusedSharpen > 0.0f && k.ratio <= 2.55f ? s_.fusedSharpen : 0.0f;
    if (!fr.hdr && float_format(cfmt)) {
        if (!hdrFromFormat_) ditto_log("colour: float format %d without the HDR flag - read as HDR\n", (int)cfmt), hdrFromFormat_ = true;
        k.hdr = 1.0f;
    }
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(ctx->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        for (IUnknown *v : {(IUnknown *)vColor, (IUnknown *)vDepth, (IUnknown *)vMv, (IUnknown *)vExpo, (IUnknown *)vReac, (IUnknown *)vOut})
            if (v) v->Release();
        return false;
    }
    memcpy(map.pData, &k, sizeof k);
    ctx->Unmap(cb_, 0);

    ID3D11Device *ctxDev = nullptr;
    ctx->GetDevice(&ctxDev);
    ID3D11DeviceContext *imm = nullptr;
    if (ctxDev) ctxDev->GetImmediateContext(&imm), ctxDev->Release();
    if (imm) readTimes(imm);
    Times &tq = times_[frames_ % kTimeRing];
    bool timed = tq.disjoint != nullptr;
    if (timed) ctx->Begin(tq.disjoint), ctx->End(tq.start);

    SavedCs saved;
    saved.save(ctx);
    ctx->CSSetConstantBuffers(0, 1, &cb_);
    ctx->CSSetSamplers(0, 1, &sampler_);
    auto run = [&](ID3D11ComputeShader *cs, ID3D11ShaderResourceView *const (&s)[kSrvs], ID3D11UnorderedAccessView *const (&u)[kUavs], UINT gx, UINT gy) {
        // (the UAVs first: what the last pass wrote leaves its UAV slot before it's read)
        ctx->CSSetUnorderedAccessViews(0, kUavs, u, nullptr);
        ctx->CSSetShaderResources(0, kSrvs, s);
        ctx->CSSetShader(cs, nullptr, 0);
        ctx->Dispatch(gx, gy, 1);
    };

    cur_ ^= 1;
    Tex &mdaC = mda_[cur_], &mdaP = mda_[cur_ ^ 1], &histC = hist_[cur_], &histP = hist_[cur_ ^ 1];
    Tex &auxC = aux_[cur_], &auxP = aux_[cur_ ^ 1], &stabC = stab_[cur_], &stabP = stab_[cur_ ^ 1];
    Tex &envC = env_[cur_], &envP = env_[cur_ ^ 1], &expoC = expo_[cur_], &expoP = expo_[cur_ ^ 1];
    // (the convert pass's t5: the game's exposure, or the last frame's auto exposure; the later passes read this frame's)
    ID3D11ShaderResourceView *expoIn = autoExpo ? expoP.srv : vExpo, *expo = autoExpo ? expoC.srv : vExpo;

    // convert (render size), and in its extra row's first group the change against the history (1 x 1)
    {
        ID3D11ShaderResourceView *s[kSrvs] = {vDepth, vMv, vColor, histP.srv, nullptr, expoIn};
        ID3D11UnorderedAccessView *u[kUavs] = {mdaC.uav, change1_.uav, autoExpo ? expoC.uav : nullptr};
        run(csConvert_, s, u, (rw + 7) / 8, (rh + 7) / 8 + 1);
    }
    // motion (render size) - at 1x in the upscale
    bool as = s_.antiShimmer != 0.0f;
    ID3D11ComputeShader *fm = rw == ow && rh == oh ? csUpscaleFm_[as ? 1 : 0] : nullptr;
    if (!fm) {
        ID3D11ShaderResourceView *s[kSrvs] = {auxP.srv, mdaC.srv, vColor, mdaP.srv, envP.srv, expo};
        ID3D11UnorderedAccessView *u[kUavs] = {auxC.uav, envC.uav};
        run(csMotion_, s, u, (rw + 7) / 8, (rh + 7) / 8);
    }
    // upscale (output size): the history, the stability levels (and the game's output: fused or a debug view)
    {
        ID3D11ShaderResourceView *s[kSrvs] = {histP.srv, mdaC.srv, vColor, fm ? nullptr : auxC.srv, change1_.srv, expo, vReac, stabP.srv,
                                              fm ? auxP.srv : nullptr, fm ? mdaP.srv : nullptr, fm ? envP.srv : nullptr};
        ID3D11UnorderedAccessView *u[kUavs] = {histC.uav, vOut, stabC.uav, fm ? auxC.uav : nullptr, fm ? envC.uav : nullptr};
        run(fm ? fm : as ? csUpscaleAs_ : csUpscale_, s, u, (ow + kUpGX - 1) / kUpGX, (oh + kUpGY - 1) / kUpGY);
    }
    // sharpen (output size): the history into the game's output (not fused, not a debug view, not off)
    if (!s_.debugView && k.fusedSharpen <= 0.0f && k.sharpenMode < 1.5f) {
        ID3D11ShaderResourceView *s[kSrvs] = {histC.srv, mdaC.srv, nullptr, nullptr, nullptr, expo, nullptr, stabC.srv};
        ID3D11UnorderedAccessView *u[kUavs] = {nullptr, vOut};
        run(csSharpen_, s, u, (ow + 7) / 8, (oh + 7) / 8);
    }
    saved.restore(ctx);
    if (timed) ctx->End(tq.end), ctx->End(tq.disjoint), tq.frame = frames_;
    if (imm) imm->Release();
    for (IUnknown *v : {(IUnknown *)vColor, (IUnknown *)vDepth, (IUnknown *)vMv, (IUnknown *)vExpo, (IUnknown *)vReac, (IUnknown *)vOut})
        if (v) v->Release();
    frames_++;
    return true;
}
