// DittoSR as libxess.dll: Intel XeSS's API with DittoSR on xessD3D12Execute, for games whose FSR can't be reached (one
// compiled into the game, as Cyberpunk 2077's) but that offer XeSS. The game's own XeSS stays behind it
// (libxess_orig.dll): every context is created, initialised and queried there as usual - the game's handle is XeSS's
// own - and xess_forward.h forwards every export not defined here (XeSS on Vulkan and D3D11 included: those stay
// XeSS). The execute runs DittoSR instead; the toggle key switches back to XeSS live.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <map>
#include <mutex>
#include <stdio.h>

#include "dittosr.h"
#include "xess_api.h"
#include "xess_forward.h"

static std::mutex g_mu;

static PfnXessD3D12CreateContext o_create;
static PfnXessD3D12Init o_init;
static PfnXessD3D12Execute o_execute;
static PfnXessDestroyContext o_destroy;
static PfnXessSetScale o_setVelocityScale, o_setJitterScale;
static PfnXessSetFloat o_setExposureMultiplier, o_setMaxResponsiveMaskValue;

static bool load_original() {
    static bool tried, ok;
    if (tried) return ok;
    tried = true;
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%slibxess_orig.dll", g_dir);
    HMODULE m = LoadLibraryA(p);
    if (!m) return ditto_log("cannot load %s (error %lu)\n", p, GetLastError()), false;
    o_create = (PfnXessD3D12CreateContext)GetProcAddress(m, "xessD3D12CreateContext");
    o_init = (PfnXessD3D12Init)GetProcAddress(m, "xessD3D12Init");
    o_execute = (PfnXessD3D12Execute)GetProcAddress(m, "xessD3D12Execute");
    o_destroy = (PfnXessDestroyContext)GetProcAddress(m, "xessDestroyContext");
    o_setVelocityScale = (PfnXessSetScale)GetProcAddress(m, "xessSetVelocityScale");
    o_setJitterScale = (PfnXessSetScale)GetProcAddress(m, "xessSetJitterScale");
    o_setExposureMultiplier = (PfnXessSetFloat)GetProcAddress(m, "xessSetExposureMultiplier");
    o_setMaxResponsiveMaskValue = (PfnXessSetFloat)GetProcAddress(m, "xessSetMaxResponsiveMaskValue");
    ok = o_create && o_init && o_execute && o_destroy && o_setVelocityScale && o_setJitterScale && o_setExposureMultiplier &&
         o_setMaxResponsiveMaskValue;
    ditto_log("Intel's XeSS %s: %s\n", p, ok ? "loaded" : "missing exports");
    return ok;
}

// What we keep of each XeSS context (keyed by XeSS's own handle)
struct Ctx {
    ID3D12Device *device = nullptr;
    DittoSR *ditto = nullptr;
    xess_d3d12_init_params_t init = {};
    bool initialised = false;
    float velocityScale[2] = {1, 1}, jitterScale[2] = {1, 1}, exposureMultiplier = 1, maxResponsive = 1;
    bool useDitto = true;
    bool resetReal = false;  // (XeSS's history is stale after DittoSR ran)
    bool compare = false;    // (both: the game's XeSS on the output's right half, DittoSR on the left)
    bool compareSet = false;
    uint64_t executes = 0;
};
static std::map<xess_context_handle_t, Ctx> g_ctx;

static Ctx *find(xess_context_handle_t h) {
    auto it = g_ctx.find(h);
    return it == g_ctx.end() ? nullptr : &it->second;
}

static void log_res(const char *name, ID3D12Resource *r) {
    if (!r) return ditto_log("  %-10s none\n", name);
    D3D12_RESOURCE_DESC d = r->GetDesc();
    ditto_log("  %-10s %llux%u dxgi %d\n", name, (unsigned long long)d.Width, d.Height, (int)d.Format);
}

// XeSS's inputs as DittoSR's (FSR 3.1's conventions)
static bool frame_from_xess(const Ctx &c, ID3D12GraphicsCommandList *cl, const xess_d3d12_execute_params_t *e, DittoFrame *f) {
    *f = DittoFrame();
    f->cl = cl;
    f->color = e->pColorTexture, f->mv = e->pVelocityTexture, f->depth = e->pDepthTexture, f->output = e->pOutputTexture;
    uint32_t fl = c.init.initFlags;
    f->exposure = (fl & XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE) ? e->pExposureScaleTexture : nullptr;
    f->reactive = (fl & XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK) ? e->pResponsivePixelMaskTexture : nullptr;
    if (!f->color || !f->mv || !f->output) return false;
    // (XeSS's rule: every input in NON_PIXEL_SHADER_RESOURCE, the output in UNORDERED_ACCESS - DittoFrame's defaults)
    D3D12_RESOURCE_DESC cd = f->color->GetDesc();
    f->rw = e->inputWidth ? e->inputWidth : (uint32_t)cd.Width;
    f->rh = e->inputHeight ? e->inputHeight : cd.Height;
    f->ow = c.init.outputResolution.x, f->oh = c.init.outputResolution.y;
    if (!f->ow || !f->oh) {
        D3D12_RESOURCE_DESC od = f->output->GetDesc();
        f->ow = (uint32_t)od.Width, f->oh = od.Height;
    }
    // (XeSS's jitter has FSR's sign: the real XeSS in the harness is best with it, 31.8 dB against 26.3 flipped)
    f->jitX = e->jitterOffsetX * c.jitterScale[0], f->jitY = e->jitterOffsetY * c.jitterScale[1];
    bool hiRes = (fl & XESS_INIT_FLAG_HIGH_RES_MV) != 0;
    f->mvRes[0] = hiRes ? (float)f->ow : (float)f->rw, f->mvRes[1] = hiRes ? (float)f->oh : (float)f->rh;
    if (fl & XESS_INIT_FLAG_USE_NDC_VELOCITY)  // (NDC: 2 a width, y up)
        f->mvToUv[0] = 0.5f * c.velocityScale[0], f->mvToUv[1] = -0.5f * c.velocityScale[1];
    else  // (pixels: the previous position = this one + mv)
        f->mvToUv[0] = c.velocityScale[0] / f->mvRes[0], f->mvToUv[1] = c.velocityScale[1] / f->mvRes[1];
    f->jitteredMv = (fl & XESS_INIT_FLAG_JITTERED_MV) != 0;
    f->depthInverted = (fl & XESS_INIT_FLAG_INVERTED_DEPTH) != 0;
    f->hdr = !(fl & XESS_INIT_FLAG_LDR_INPUT_COLOR);
    f->exposureMul = (e->exposureScale > 0.0f ? e->exposureScale : 1.0f) * c.exposureMultiplier;
    f->reactiveMul = c.maxResponsive > 0.0f ? 1.0f / c.maxResponsive : 1.0f;
    f->reset = e->resetHistory != 0;
    return true;
}

extern "C" {

__declspec(dllexport) xess_result_t xessD3D12CreateContext(ID3D12Device *device, xess_context_handle_t *ctx) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!load_original()) return XESS_RESULT_ERROR_CANT_LOAD_LIBRARY;
    xess_result_t r = o_create(device, ctx);
    ditto_log("xessD3D12CreateContext: %d, context %p, device %p\n", r, ctx ? (void *)*ctx : nullptr, (void *)device);
    if (r >= XESS_RESULT_SUCCESS && ctx && *ctx) {
        Ctx &c = g_ctx[*ctx];
        c = Ctx();
        c.device = device;
    }
    return r;
}

__declspec(dllexport) xess_result_t xessD3D12Init(xess_context_handle_t h, const xess_d3d12_init_params_t *p) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!load_original()) return XESS_RESULT_ERROR_CANT_LOAD_LIBRARY;
    xess_result_t r = o_init(h, p);
    Ctx *c = find(h);
    if (p) {
        uint32_t f = p->initFlags;
        ditto_log("xessD3D12Init: %d, output %ux%u, quality %d, flags 0x%x (high-res mv %d, inverted depth %d, exposure texture %d, "
                  "responsive mask %d, ndc velocity %d, external heap %d, ldr %d, jittered mv %d, auto exposure %d)\n",
                  r, p->outputResolution.x, p->outputResolution.y, (int)p->qualitySetting, f, !!(f & XESS_INIT_FLAG_HIGH_RES_MV),
                  !!(f & XESS_INIT_FLAG_INVERTED_DEPTH), !!(f & XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE),
                  !!(f & XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK), !!(f & XESS_INIT_FLAG_USE_NDC_VELOCITY),
                  !!(f & XESS_INIT_FLAG_EXTERNAL_DESCRIPTOR_HEAP), !!(f & XESS_INIT_FLAG_LDR_INPUT_COLOR),
                  !!(f & XESS_INIT_FLAG_JITTERED_MV), !!(f & XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE));
    }
    if (!c || !p || !g_cfg.enabled) return r;
    c->init = *p, c->initialised = true;
    if (!c->ditto) {
        // (XeSS gives no flags at creation: DittoFrame carries them per execute, from the init flags)
        DittoCreateInfo ci = {0, 0, 0, p->outputResolution.x, p->outputResolution.y};
        c->ditto = new DittoSR(c->device, ci, g_cfg.s);
        if (!c->ditto->ok()) ditto_log("DittoSR: pipelines failed, XeSS instead\n"), delete c->ditto, c->ditto = nullptr;
    }
    c->useDitto = c->ditto != nullptr;
    ditto_log("context %p: %s\n", (void *)h, c->ditto ? "DittoSR (D3D12)" : "XeSS");
    return r;
}

__declspec(dllexport) xess_result_t xessD3D12Execute(xess_context_handle_t h, ID3D12GraphicsCommandList *cl, const xess_d3d12_execute_params_t *e) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!load_original()) return XESS_RESULT_ERROR_CANT_LOAD_LIBRARY;
    Ctx *c = find(h);
    if (!c || !e || !c->initialised) return o_execute(h, cl, e);
    // the toggle (a key press: the other upscaler from this frame on, its history started again)
    static bool down;
    bool now = g_cfg.toggleKey && (GetAsyncKeyState(g_cfg.toggleKey) & 0x8000);
    bool switched = false;
    if (now && !down && c->ditto) {
        c->useDitto = !c->useDitto, switched = true;
        if (!c->useDitto) c->resetReal = true;
        ditto_log("toggle: %s from execute %llu\n", c->useDitto ? "DittoSR" : "the game's XeSS", (unsigned long long)c->executes);
    }
    down = now;
    // the comparison (a key press, or compare=1): the game's XeSS draws the whole output first, DittoSR then its left half
    if (!c->compareSet) c->compare = g_cfg.compare && c->ditto, c->compareSet = true, c->resetReal = c->compare;
    static bool cdown;
    bool cnow = g_cfg.compareKey && (GetAsyncKeyState(g_cfg.compareKey) & 0x8000);
    if (cnow && !cdown && c->ditto) {
        c->compare = !c->compare;
        if (c->compare) c->resetReal = true;
        ditto_log("comparison %s from execute %llu (DittoSR left, the game's XeSS right)\n", c->compare ? "on" : "off", (unsigned long long)c->executes);
    }
    cdown = cnow;
    if (ditto_poll_reload() && c->ditto) c->ditto->set_settings(g_cfg.s);
    if (c->executes < (uint64_t)g_cfg.logDispatches) {
        ditto_log("execute %llu: input %ux%u, jitter %.4f %.4f, exposure scale %g, reset %u, bases color %u,%u mv %u,%u depth %u,%u "
                  "output %u,%u, heap %p; scales velocity %g %g jitter %g %g, exposure multiplier %g, max responsive %g\n",
                  (unsigned long long)c->executes, e->inputWidth, e->inputHeight, e->jitterOffsetX, e->jitterOffsetY, e->exposureScale,
                  e->resetHistory, e->inputColorBase.x, e->inputColorBase.y, e->inputMotionVectorBase.x, e->inputMotionVectorBase.y,
                  e->inputDepthBase.x, e->inputDepthBase.y, e->outputColorBase.x, e->outputColorBase.y, (void *)e->pDescriptorHeap,
                  c->velocityScale[0], c->velocityScale[1], c->jitterScale[0], c->jitterScale[1], c->exposureMultiplier, c->maxResponsive);
        log_res("color", e->pColorTexture), log_res("velocity", e->pVelocityTexture), log_res("depth", e->pDepthTexture);
        log_res("exposure", e->pExposureScaleTexture), log_res("responsive", e->pResponsivePixelMaskTexture), log_res("output", e->pOutputTexture);
    }
    c->executes++;
    DittoFrame fr;
    if (c->useDitto && c->ditto && frame_from_xess(*c, cl, e, &fr)) {
        if (c->compare) {  // (the game's XeSS first, on the whole output: DittoSR's left half then goes over it)
            xess_d3d12_execute_params_t r = *e;
            if (c->resetReal) r.resetHistory = 1, c->resetReal = false;
            o_execute(h, cl, &r);
        }
        c->ditto->set_split(c->compare);
        if (c->ditto->dispatch(fr, switched)) {
            uint64_t n = c->ditto->frames();
            if (n == 1 || n % 3600 == 0) ditto_log("DittoSR ran (frame %llu)\n", (unsigned long long)n);
            return XESS_RESULT_SUCCESS;
        }
        static int fails;
        if (fails++ < 5) ditto_log("DittoSR could not run this execute: XeSS instead\n");
    }
    if (c->resetReal) {
        c->resetReal = false;
        xess_d3d12_execute_params_t r = *e;
        r.resetHistory = 1;
        return o_execute(h, cl, &r);
    }
    return o_execute(h, cl, e);
}

__declspec(dllexport) xess_result_t xessDestroyContext(xess_context_handle_t h) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!load_original()) return XESS_RESULT_ERROR_CANT_LOAD_LIBRARY;
    if (Ctx *c = find(h)) {
        ditto_log("context %p destroyed after %llu executes (%llu DittoSR)\n", (void *)h, (unsigned long long)c->executes,
                  (unsigned long long)(c->ditto ? c->ditto->frames() : 0));
        delete c->ditto;
        g_ctx.erase(h);
    }
    return o_destroy(h);
}

// (the scales the game sets: kept for DittoSR, then XeSS's own)
__declspec(dllexport) xess_result_t xessSetVelocityScale(xess_context_handle_t h, float x, float y) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!load_original()) return XESS_RESULT_ERROR_CANT_LOAD_LIBRARY;
    if (Ctx *c = find(h)) c->velocityScale[0] = x, c->velocityScale[1] = y;
    ditto_log("xessSetVelocityScale %g %g\n", x, y);
    return o_setVelocityScale(h, x, y);
}

__declspec(dllexport) xess_result_t xessSetJitterScale(xess_context_handle_t h, float x, float y) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!load_original()) return XESS_RESULT_ERROR_CANT_LOAD_LIBRARY;
    if (Ctx *c = find(h)) c->jitterScale[0] = x, c->jitterScale[1] = y;
    ditto_log("xessSetJitterScale %g %g\n", x, y);
    return o_setJitterScale(h, x, y);
}

__declspec(dllexport) xess_result_t xessSetExposureMultiplier(xess_context_handle_t h, float m) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!load_original()) return XESS_RESULT_ERROR_CANT_LOAD_LIBRARY;
    if (Ctx *c = find(h)) c->exposureMultiplier = m;
    ditto_log("xessSetExposureMultiplier %g\n", m);
    return o_setExposureMultiplier(h, m);
}

__declspec(dllexport) xess_result_t xessSetMaxResponsiveMaskValue(xess_context_handle_t h, float v) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!load_original()) return XESS_RESULT_ERROR_CANT_LOAD_LIBRARY;
    if (Ctx *c = find(h)) c->maxResponsive = v;
    ditto_log("xessSetMaxResponsiveMaskValue %g\n", v);
    return o_setMaxResponsiveMaskValue(h, v);
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        ditto_init(h, "dittosr-xess");
    }
    return TRUE;
}
