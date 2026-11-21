// DittoSR as an NGX runtime on D3D11: the NVSDK_NGX_D3D11_* exports of nvngx.dll that run DLSS (super sampling) -
// init, create, evaluate, release, shutdown - answered by DittoSR (dittosr_dx11.cpp). It's the same DLL as the FidelityFX
// API provider, shipped as nvngx.dll for OptiScaler's custom DLSS folder (custom\<name>\dlss\): OptiScaler hands it the
// game's DLSS parameters as they are, and DittoSR then runs natively on the game's D3D11 device. NGX's parameter objects
// (GetParameters, AllocateParameters...) are the caller's: DittoSR only reads the ones it is given.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <mutex>
#include <set>
#include <stdio.h>
#include <string.h>

#include "dittosr.h"
#include "dittosr_dx11.h"
#include "ngx_abi.h"

namespace {

std::mutex g_mu;
int g_inits;  // (the inits not shut down yet)

struct Feature {
    NVSDK_NGX_Handle handle;  // (first: the NVSDK_NGX_Handle * the caller holds is this Feature's address)
    DittoSR11 *ditto = nullptr;
    DittoCreateInfo ci = {};
    uint32_t ngxFlags = 0;
    uint64_t evaluations = 0;
    float lastSharp = -1.0f;
    uint32_t lastRw = 0, lastRh = 0, lastOw = 0, lastOh = 0, sizeLogs = 0;
    bool subrectLogged = false;
};
std::set<Feature *> g_features;
unsigned int g_nextId = 1;

Feature *find(const NVSDK_NGX_Handle *h) {
    auto it = g_features.find((Feature *)h);
    return it == g_features.end() ? nullptr : *it;
}

// this DLL in OptiScaler's custom provider folder: the sharpness the evaluation carries is its slider's
bool from_optiscaler() {
    char d[260];
    snprintf(d, sizeof d, "%s", g_dir);
    _strlwr_s(d, sizeof d);
    return strstr(d, "\\custom\\") != nullptr;
}

ID3D11Resource *get_res(const NVSDK_NGX_Parameter *p, const char *name) {
    ID3D11Resource *r = nullptr;
    if (p->Get(name, &r) != NVSDK_NGX_Result_Success || !r) {
        void *v = nullptr;
        r = p->Get(name, &v) == NVSDK_NGX_Result_Success ? (ID3D11Resource *)v : nullptr;
    }
    return r;
}

uint32_t get_u(const NVSDK_NGX_Parameter *p, const char *name, uint32_t def) {
    unsigned int u = 0;
    if (p->Get(name, &u) == NVSDK_NGX_Result_Success) return u;
    int i = 0;
    return p->Get(name, &i) == NVSDK_NGX_Result_Success ? (uint32_t)i : def;
}

float get_f(const NVSDK_NGX_Parameter *p, const char *name, float def) {
    float f = 0.0f;
    return p->Get(name, &f) == NVSDK_NGX_Result_Success ? f : def;
}

void log_res(const char *name, ID3D11Resource *r) {
    if (!r) return ditto_log("  %-10s none\n", name);
    ID3D11Texture2D *t = nullptr;
    D3D11_TEXTURE2D_DESC d = {};
    if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&t))) t->GetDesc(&d), t->Release();
    ditto_log("  %-10s %ux%u dxgi %d, bind 0x%x\n", name, d.Width, d.Height, (int)d.Format, d.BindFlags);
}

NVSDK_NGX_Result init(const char *how, ID3D11Device *dev) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!dev) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    g_inits++;
    ditto_log("NGX D3D11 init (%s), device %p\n", how, (void *)dev);
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result shutdown() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_inits > 0) g_inits--;
    ditto_log("NGX D3D11 shutdown (%zu features still alive)\n", g_features.size());
    return NVSDK_NGX_Result_Success;
}

}  // namespace

#define NGX_EXPORT extern "C" __declspec(dllexport)

NGX_EXPORT NVSDK_NGX_Result NVSDK_NGX_D3D11_Init(unsigned long long, const wchar_t *, ID3D11Device *dev, const void *, int) {
    return init("app id", dev);
}
NGX_EXPORT NVSDK_NGX_Result NVSDK_NGX_D3D11_Init_Ext(unsigned long long, const wchar_t *, ID3D11Device *dev, int, const void *) {
    return init("app id, ext", dev);
}
NGX_EXPORT NVSDK_NGX_Result NVSDK_NGX_D3D11_Init_ProjectID(const char *, int, const char *, const wchar_t *, ID3D11Device *dev, int,
                                                          const void *) {
    return init("project id", dev);
}
NGX_EXPORT NVSDK_NGX_Result NVSDK_NGX_D3D11_Shutdown() { return shutdown(); }
NGX_EXPORT NVSDK_NGX_Result NVSDK_NGX_D3D11_Shutdown1(ID3D11Device *, unsigned int *remaining) {
    NVSDK_NGX_Result r = shutdown();
    if (remaining) *remaining = (unsigned)g_inits;
    return r;
}

NGX_EXPORT NVSDK_NGX_Result NVSDK_NGX_D3D11_GetFeatureRequirements(IDXGIAdapter *, const NVSDK_NGX_FeatureDiscoveryInfo *info,
                                                                   NVSDK_NGX_FeatureRequirement *out) {
    if (!info || !out) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    if (info->FeatureID != NVSDK_NGX_Feature_SuperSampling) return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    out->FeatureSupported = NVSDK_NGX_FeatureSupportResult_Supported, out->MinHWArchitecture = 0;
    strcpy_s(out->MinOSVersion, "10.0.10240.16384");
    return NVSDK_NGX_Result_Success;
}

NGX_EXPORT NVSDK_NGX_Result NVSDK_NGX_D3D11_GetScratchBufferSize(NVSDK_NGX_Feature, const NVSDK_NGX_Parameter *, size_t *size) {
    if (size) *size = 0;  // (DittoSR makes its own textures)
    return NVSDK_NGX_Result_Success;
}

NGX_EXPORT NVSDK_NGX_Result NVSDK_NGX_D3D11_CreateFeature(ID3D11DeviceContext *ctx, NVSDK_NGX_Feature id, NVSDK_NGX_Parameter *p,
                                                         NVSDK_NGX_Handle **out) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!ctx || !p || !out) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    if (id != NVSDK_NGX_Feature_SuperSampling) {
        ditto_log("NGX: feature %d is not DittoSR's (super sampling only)\n", (int)id);
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    }
    if (!g_cfg.enabled) return ditto_log("NGX: dittosr.ini's enabled=0 - no feature\n"), NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    ID3D11Device *dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return NVSDK_NGX_Result_FAIL_PlatformError;
    Feature *f = new Feature();
    f->ngxFlags = get_u(p, NGX_DLSS_Feature_Create_Flags, 0);
    uint32_t rw = get_u(p, NGX_Width, 0), rh = get_u(p, NGX_Height, 0), ow = get_u(p, NGX_OutWidth, 0), oh = get_u(p, NGX_OutHeight, 0);
    // (DLSS's flags as FSR 3.1's, which DittoCreateInfo carries)
    uint32_t fl = 0, nf = f->ngxFlags;
    if (nf & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) fl |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    if (!(nf & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes)) fl |= FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
    if (nf & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) fl |= FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
    if (nf & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) fl |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    if (nf & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) fl |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    f->ci = {fl, rw, rh, ow, oh};
    ditto_log("NGX feature: DLSS flags 0x%x (hdr %d, low-res mv %d, jittered mv %d, depth inverted %d, sharpening %d, auto exposure %d), "
              "render %ux%u, output %ux%u, device %p\n",
              nf, !!(nf & NVSDK_NGX_DLSS_Feature_Flags_IsHDR), !!(nf & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes),
              !!(nf & NVSDK_NGX_DLSS_Feature_Flags_MVJittered), !!(nf & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted),
              !!(nf & NVSDK_NGX_DLSS_Feature_Flags_DoSharpening), !!(nf & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure), rw, rh, ow, oh, (void *)dev);
    f->ditto = new DittoSR11(dev, f->ci, g_cfg.s);
    dev->Release();
    if (!f->ditto->ok()) {
        ditto_log("DittoSR (D3D11): setup failed\n");
        delete f->ditto, delete f;
        return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
    }
    ditto_overlay_install_window();
    f->handle.Id = g_nextId++;
    g_features.insert(f);
    *out = &f->handle;
    ditto_log("NGX feature %u: DittoSR (D3D11)\n", f->handle.Id);
    return NVSDK_NGX_Result_Success;
}

NGX_EXPORT NVSDK_NGX_Result NVSDK_NGX_D3D11_ReleaseFeature(NVSDK_NGX_Handle *h) {
    std::lock_guard<std::mutex> lk(g_mu);
    Feature *f = find(h);
    if (!f) return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    ditto_log("NGX feature %u released after %llu evaluations (%llu DittoSR)\n", f->handle.Id, (unsigned long long)f->evaluations,
              (unsigned long long)f->ditto->frames());
    g_features.erase(f);
    delete f->ditto;
    delete f;
    return NVSDK_NGX_Result_Success;
}

NGX_EXPORT NVSDK_NGX_Result NVSDK_NGX_D3D11_EvaluateFeature(ID3D11DeviceContext *ctx, const NVSDK_NGX_Handle *h, const NVSDK_NGX_Parameter *p,
                                                           PFN_NVSDK_NGX_ProgressCallback) {
    std::lock_guard<std::mutex> lk(g_mu);
    Feature *f = find(h);
    if (!f) return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    if (!ctx || !p) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    DittoFrame11 fr;
    fr.ctx = ctx;
    fr.color = get_res(p, NGX_Color), fr.depth = get_res(p, NGX_Depth), fr.mv = get_res(p, NGX_MotionVectors);
    fr.output = get_res(p, NGX_Output), fr.exposure = get_res(p, NGX_ExposureTexture), fr.reactive = get_res(p, NGX_FSR_Reactive);
    if (!fr.color || !fr.mv || !fr.output) {
        static int logged;
        if (logged++ < 5) ditto_log("NGX evaluate: missing input (colour %p, motion vectors %p, output %p)\n", (void *)fr.color, (void *)fr.mv, (void *)fr.output);
        return NVSDK_NGX_Result_FAIL_MissingInput;
    }
    // the sizes: the render subrect's, else the feature's (as created, or as the parameters hold now); the output's
    // the feature's, else the output texture's
    D3D11_TEXTURE2D_DESC od = {};
    {
        ID3D11Texture2D *t = nullptr;
        if (SUCCEEDED(fr.output->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&t))) t->GetDesc(&od), t->Release();
    }
    fr.rw = get_u(p, NGX_DLSS_Render_Subrect_Width, 0), fr.rh = get_u(p, NGX_DLSS_Render_Subrect_Height, 0);
    if (!fr.rw || !fr.rh) fr.rw = get_u(p, NGX_Width, f->ci.maxRenderW), fr.rh = get_u(p, NGX_Height, f->ci.maxRenderH);
    fr.ow = get_u(p, NGX_OutWidth, f->ci.maxUpscaleW), fr.oh = get_u(p, NGX_OutHeight, f->ci.maxUpscaleH);
    if (!fr.ow || !fr.oh) fr.ow = od.Width, fr.oh = od.Height;
    if (!f->subrectLogged && (get_u(p, NGX_DLSS_Input_Color_Subrect_Base_X, 0) || get_u(p, NGX_DLSS_Input_Color_Subrect_Base_Y, 0) ||
                              get_u(p, NGX_DLSS_Output_Subrect_Base_X, 0) || get_u(p, NGX_DLSS_Output_Subrect_Base_Y, 0))) {
        ditto_log("NGX evaluate: the inputs or the output start away from 0, 0 (subrect bases) - DittoSR reads and writes from 0, 0\n");
        f->subrectLogged = true;
    }
    // (DLSS's conventions are FSR's: the jitter in render pixels, the vectors x their scale in pixels of their resolution)
    fr.jitX = get_f(p, NGX_Jitter_Offset_X, 0.0f), fr.jitY = get_f(p, NGX_Jitter_Offset_Y, 0.0f);
    bool displayMv = (f->ci.flags & FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) != 0;
    fr.mvRes[0] = displayMv ? (float)fr.ow : (float)fr.rw, fr.mvRes[1] = displayMv ? (float)fr.oh : (float)fr.rh;
    float sx = get_f(p, NGX_MV_Scale_X, 1.0f), sy = get_f(p, NGX_MV_Scale_Y, 1.0f);
    fr.mvToUv[0] = sx / fr.mvRes[0], fr.mvToUv[1] = sy / fr.mvRes[1];
    fr.jitteredMv = (f->ci.flags & FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION) != 0;
    fr.depthInverted = (f->ci.flags & FFX_UPSCALE_ENABLE_DEPTH_INVERTED) != 0;
    fr.hdr = (f->ci.flags & FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE) != 0;
    fr.autoExposure = (f->ci.flags & FFX_UPSCALE_ENABLE_AUTO_EXPOSURE) != 0;
    float pe = get_f(p, NGX_DLSS_Pre_Exposure, 1.0f), es = get_f(p, NGX_DLSS_Exposure_Scale, 1.0f);
    fr.preExposure = pe > 0.0f ? pe : 1.0f, fr.exposureMul = es > 0.0f ? es : 1.0f;
    fr.reset = get_u(p, NGX_Reset, 0) != 0;

    // the settings: dittosr.ini's (F9 reads it again), the sharpness the evaluation's when it comes from the user
    // (OptiScaler's slider: 0 there is "no sharpening", so dittosr.ini's is kept)
    ditto_poll_reload();
    DittoSettings s = ditto_settings();
    g_stats.fsrFrameWeight = -1.0f;
    bool gs = g_cfg.gameSharpness > 0 || (g_cfg.gameSharpness < 0 && from_optiscaler());
    float sharp = get_f(p, NGX_Sharpness, 0.0f);
    if (gs && sharp > 0.0f) s.sharpness = sharp > 1.0f ? 1.0f : sharp;
    if (s.sharpness != f->lastSharp) {
        ditto_log("settings: sharpness %g%s, frame weight %g\n", s.sharpness, gs && sharp > 0.0f ? " (the evaluation's)" : "", s.frameWeight);
        f->lastSharp = s.sharpness;
    }
    f->ditto->set_settings(s);

    bool sizes = fr.rw != f->lastRw || fr.rh != f->lastRh || fr.ow != f->lastOw || fr.oh != f->lastOh;
    f->lastRw = fr.rw, f->lastRh = fr.rh, f->lastOw = fr.ow, f->lastOh = fr.oh;
    if (f->evaluations < (uint64_t)g_cfg.logDispatches || (sizes && f->sizeLogs++ < 32)) {
        ditto_log("evaluate %llu: render %ux%u -> %ux%u, jitter %.4f %.4f, mv scale %.2f %.2f, reset %d, sharpness %.2f, pre-exposure %g, "
                  "exposure scale %g, context %p\n",
                  (unsigned long long)f->evaluations, fr.rw, fr.rh, fr.ow, fr.oh, fr.jitX, fr.jitY, sx, sy, fr.reset, sharp, pe, es, (void *)ctx);
        log_res("color", fr.color), log_res("depth", fr.depth), log_res("motion", fr.mv), log_res("exposure", fr.exposure);
        log_res("reactive", fr.reactive), log_res("output", fr.output);
    }
    f->evaluations++;
    ditto_overlay_tick();
    ditto_stats_update(3, fr.rw, fr.rh, fr.ow, fr.oh, true, false, false);
    if (!f->ditto->dispatch(fr, false)) {
        static int fails;
        if (fails++ < 5) ditto_log("DittoSR (D3D11) could not run this evaluation\n");
        return NVSDK_NGX_Result_Fail;
    }
    uint64_t n = f->ditto->frames();
    if (n == 1 || n % 3600 == 0) ditto_log("DittoSR ran (frame %llu, D3D11)\n", (unsigned long long)n);
    return NVSDK_NGX_Result_Success;
}
