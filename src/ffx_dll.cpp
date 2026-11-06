// DittoSR as a FidelityFX API provider: amd_fidelityfx_dx12.dll (FidelityFX SDK 1.1, FSR 3.1 - also what OptiScaler
// loads for its FSR 3.1 option), amd_fidelityfx_loader_dx12.dll (SDK 2.x) or amd_fidelityfx_vk.dll (FSR 3.1 on Vulkan),
// the same DLL under each name. It exports the API (ffxCreateContext / ffxConfigure / ffxQuery / ffxDispatch /
// ffxDestroyContext) and answers the upscaler's part itself: DX12 and Vulkan upscale contexts (by the backend the game
// describes), their dispatches, the quality-mode and jitter queries, the version queries.
//   AMD's DLL alongside, renamed <this DLL's name>.orig.dll, is optional. With it: every other effect (frame generation,
// its swap chain) goes to it, an upscale context also gets AMD's FSR (the toggle key switches between them live; the
// reactive mask generation goes to it), and a version override naming one of AMD's providers gets AMD's alone. Without
// it: the upscaler alone - other effects are refused (FFX_API_RETURN_NO_PROVIDER), as a game sees a DLL without them.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <mutex>
#include <set>
#include <stdio.h>
#include <string.h>

#include "dittosr.h"
#include "dittosr_vk.h"
#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_upscale.h"
#include "ffx_api/vk/ffx_api_vk.h"

// (ffx_api_dx12.h's backend description)
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 0x0000002u
struct ffxCreateBackendDX12Desc {
    ffxCreateContextDescHeader header;
    ID3D12Device *device;
};

// DittoSR's provider id in the version queries ("DITTO", version 1)
static const uint64_t kVersionId = 0x00444954544f0001ull;
// (the name with an FSR version first: OptiScaler reads a provider's version as the first x.y.z in its name, and sends
// FSR 3.1's settings - its Velocity Factor slider: DittoSR's frame weight - only to 3.1.1 and above)
static const char *const kVersionName = "DittoSR (FSR 3.1.5 API) " DITTOSR_VERSION;

static std::mutex g_mu;
static HMODULE g_self;

// ---- AMD's DLL, when it is alongside
static PfnFfxCreateContext o_create;
static PfnFfxDestroyContext o_destroy;
static PfnFfxConfigure o_configure;
static PfnFfxQuery o_query;
static PfnFfxDispatch o_dispatch;

static bool load_original() {
    static bool tried, ok;
    if (tried) return ok;
    tried = true;
    char p[MAX_PATH];
    GetModuleFileNameA(g_self, p, MAX_PATH);
    char *dot = strrchr(p, '.');
    if (!dot) return false;
    snprintf(dot, (size_t)(p + MAX_PATH - dot), ".orig.dll");
    if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) return ditto_log("no %s: DittoSR alone (no frame generation)\n", p), false;
    HMODULE m = LoadLibraryA(p);
    if (!m) return ditto_log("cannot load %s (error %lu): DittoSR alone\n", p, GetLastError()), false;
    o_create = (PfnFfxCreateContext)GetProcAddress(m, "ffxCreateContext");
    o_destroy = (PfnFfxDestroyContext)GetProcAddress(m, "ffxDestroyContext");
    o_configure = (PfnFfxConfigure)GetProcAddress(m, "ffxConfigure");
    o_query = (PfnFfxQuery)GetProcAddress(m, "ffxQuery");
    o_dispatch = (PfnFfxDispatch)GetProcAddress(m, "ffxDispatch");
    ok = o_create && o_destroy && o_configure && o_query && o_dispatch;
    ditto_log("AMD's DLL %s: %s\n", p, ok ? "loaded" : "missing exports, DittoSR alone");
    return ok;
}

// ---- our upscale contexts
struct UpscaleCtx {
    ffxContext real = nullptr;  // AMD's FSR (the original DLL's), or null
    DittoSR *ditto = nullptr;      // (D3D12)
    DittoSRVk *dittoVk = nullptr;  // (Vulkan)
    DittoCreateInfo ci = {};
    bool useDitto = true;
    bool resetReal = false;  // (FSR's history is stale after DittoSR ran)
    bool compare = false;    // (both: AMD's FSR on the output's right half, DittoSR on the left)
    uint64_t dispatches = 0;
    // FSR 3.1's velocity factor and reactiveness scale as configured (-1: never) - OptiScaler's sliders and presets: the
    // frame weight (frame_weight_of)
    float velocity = -1.0f, reactiveness = -1.0f;
    float lastSharp = -1.0f, lastWeight = -1.0f;  // (the overrides last applied, for the log)
    uint32_t lastRw = 0, lastRh = 0, lastOw = 0, lastOh = 0, sizeLogs = 0;  // (the sizes last seen, for the log)
};

// The frame weight from FSR 3.1's settings, so OptiScaler's presets land on DittoSR's steps: both at 1 (its Default)
// DittoSR's own (-1: the ini's, 2 by default); the velocity factor below 1 steadier (2 x it: Stability's 0.5 is 1); at 1
// the reactiveness scale below 1 more responsive (2 + 2 x (1 - it): Motion's 0.5 is 3, 0 is 4)
static float frame_weight_of(const UpscaleCtx *c) {
    float v = c->velocity >= 0.0f ? c->velocity : 1.0f, r = c->reactiveness >= 0.0f ? c->reactiveness : 1.0f;
    if (v >= 1.0f && r >= 1.0f) return -1.0f;
    float w = v < 1.0f ? 2.0f * v : 2.0f + 2.0f * (1.0f - r);
    return w < 0.25f ? 0.25f : w > 4.0f ? 4.0f : w;
}

// this DLL in OptiScaler's custom provider folder (...\custom\<name>\ffx\): its sharpness slider is the user's
static bool from_optiscaler() {
    static int r = -1;
    if (r < 0) {
        char d[260];
        snprintf(d, sizeof d, "%s", g_dir);
        _strlwr_s(d, sizeof d);
        r = strstr(d, "\\custom\\") != nullptr;
    }
    return r == 1;
}
static std::set<UpscaleCtx *> g_ctx;

static UpscaleCtx *ours(ffxContext *c) {
    if (!c || !*c) return nullptr;
    auto it = g_ctx.find((UpscaleCtx *)*c);
    return it == g_ctx.end() ? nullptr : *it;
}

static const ffxApiHeader *find_desc(const ffxApiHeader *h, ffxStructType_t type) {
    for (; h; h = h->pNext)
        if (h->type == type) return h;
    return nullptr;
}

static void log_chain(const char *what, const ffxApiHeader *h) {
    char b[512];
    int n = 0;
    for (; h && n < (int)sizeof b - 24; h = h->pNext) n += snprintf(b + n, sizeof b - n, " 0x%llx", (unsigned long long)h->type);
    b[n] = 0;
    ditto_log("%s:%s\n", what, b);
}

static void log_res(const char *name, const FfxApiResource &r, bool vk) {
    if (!r.resource) return ditto_log("  %-12s none\n", name);
    if (vk)
        return ditto_log("  %-12s %ux%u (ffx fmt%u, usage 0x%x, state 0x%x)\n", name, r.description.width, r.description.height,
                         r.description.format, r.description.usage, r.state);
    D3D12_RESOURCE_DESC d = ((ID3D12Resource *)r.resource)->GetDesc();
    ditto_log("  %-12s %llux%u dxgi %d (ffx fmt%u, state 0x%x)\n", name, (unsigned long long)d.Width, d.Height, (int)d.Format,
              r.description.format, r.state);
}

// the upscaler's queries without a context (FSR 3.1's formulas), and the version ones; false: not one of them
static bool own_query(ffxContext *context, ffxQueryDescHeader *desc, ffxReturnCode_t *rc) {
    *rc = FFX_API_RETURN_OK;
    switch (desc->type) {
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE: {
        auto *q = (ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode *)desc;
        float r = ditto_ratio_from_quality(q->qualityMode);
        if (r <= 0.0f) *rc = FFX_API_RETURN_ERROR_PARAMETER;
        else if (q->pOutUpscaleRatio) *q->pOutUpscaleRatio = r;
        return true;
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE: {
        auto *q = (ffxQueryDescUpscaleGetRenderResolutionFromQualityMode *)desc;
        float r = ditto_ratio_from_quality(q->qualityMode);
        if (r <= 0.0f) return *rc = FFX_API_RETURN_ERROR_PARAMETER, true;
        if (q->pOutRenderWidth) *q->pOutRenderWidth = (uint32_t)((float)q->displayWidth / r);
        if (q->pOutRenderHeight) *q->pOutRenderHeight = (uint32_t)((float)q->displayHeight / r);
        return true;
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT: {
        auto *q = (ffxQueryDescUpscaleGetJitterPhaseCount *)desc;
        if (q->pOutPhaseCount) *q->pOutPhaseCount = ditto_jitter_phase_count(q->renderWidth, q->displayWidth);
        return true;
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET: {
        auto *q = (ffxQueryDescUpscaleGetJitterOffset *)desc;
        ditto_jitter_offset(q->index, q->phaseCount, q->pOutX, q->pOutY);
        return true;
    }
    case FFX_API_QUERY_DESC_TYPE_GET_VERSIONS: {
        // (the upscaler's providers: DittoSR first - the default - then AMD's, when its DLL is alongside)
        auto *q = (ffxQueryDescGetVersions *)desc;
        if (q->createDescType != FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE) return false;
        uint64_t cap = q->outputCount ? *q->outputCount : 0, n = 1;
        if (cap > 0) {
            if (q->versionIds) q->versionIds[0] = kVersionId;
            if (q->versionNames) q->versionNames[0] = kVersionName;
        }
        if (load_original()) {
            uint64_t rest = cap > 1 ? cap - 1 : 0;
            ffxQueryDescGetVersions o = *q;
            o.outputCount = &rest;
            o.versionIds = q->versionIds && cap > 1 ? q->versionIds + 1 : nullptr;
            o.versionNames = q->versionNames && cap > 1 ? q->versionNames + 1 : nullptr;
            if (o_query(nullptr, &o.header) == FFX_API_RETURN_OK) n += rest;
        }
        if (q->outputCount) *q->outputCount = cap ? (n < cap ? n : cap) : n;
        return true;
    }
    case FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION: {
        UpscaleCtx *c = ours(context);
        if (!c || !c->useDitto) return false;
        auto *q = (ffxQueryGetProviderVersion *)desc;
        q->versionId = kVersionId, q->versionName = kVersionName;
        return true;
    }
    }
    return false;
}

extern "C" {

FFX_API_ENTRY ffxReturnCode_t ffxCreateContext(ffxContext *context, ffxCreateContextDescHeader *desc, const ffxAllocationCallbacks *memCb) {
    std::lock_guard<std::mutex> lk(g_mu);
    bool amd = load_original();
    if (!context || !desc) return FFX_API_RETURN_ERROR_PARAMETER;
    log_chain("ffxCreateContext", desc);
    if (desc->type != FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE) return amd ? o_create(context, desc, memCb) : FFX_API_RETURN_NO_PROVIDER;
    // (enabled=0, or a version override naming one of AMD's providers: AMD's FSR alone)
    auto *ov = (const ffxOverrideVersion *)find_desc(desc, FFX_API_DESC_TYPE_OVERRIDE_VERSION);
    if (amd && (!g_cfg.enabled || (ov && ov->versionId != kVersionId))) return o_create(context, desc, memCb);
    const ffxCreateContextDescUpscale *u = (const ffxCreateContextDescUpscale *)desc;
    auto *be = (const ffxCreateBackendDX12Desc *)find_desc(desc, FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12);
    auto *bv = (const ffxCreateBackendVKDesc *)find_desc(desc, FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK);
    if ((!be || !be->device) && (!bv || !bv->vkDevice || !bv->vkPhysicalDevice || !bv->vkDeviceProcAddr)) {
        ditto_log("upscale context without a DX12 or Vulkan device: %s\n", amd ? "AMD's FSR" : "refused");
        return amd ? o_create(context, desc, memCb) : FFX_API_RETURN_NO_PROVIDER;
    }
    UpscaleCtx *c = new UpscaleCtx();
    c->ci = {u->flags, u->maxRenderSize.width, u->maxRenderSize.height, u->maxUpscaleSize.width, u->maxUpscaleSize.height};
    ditto_log("upscale context: flags 0x%x (hdr %d, display mv %d, jittered mv %d, depth inverted %d, infinite %d, auto exposure %d, "
              "non-linear %d), max render %ux%u, max upscale %ux%u, device %p\n",
              u->flags, !!(u->flags & FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE), !!(u->flags & FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS),
              !!(u->flags & FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION), !!(u->flags & FFX_UPSCALE_ENABLE_DEPTH_INVERTED),
              !!(u->flags & FFX_UPSCALE_ENABLE_DEPTH_INFINITE), !!(u->flags & FFX_UPSCALE_ENABLE_AUTO_EXPOSURE),
              !!(u->flags & FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE), u->maxRenderSize.width, u->maxRenderSize.height,
              u->maxUpscaleSize.width, u->maxUpscaleSize.height, be ? (void *)be->device : (void *)bv->vkDevice);
    // (AMD's FSR as well, when its DLL is here: the toggle, the reactive mask generation)
    if (amd && o_create(&c->real, desc, memCb) != FFX_API_RETURN_OK) ditto_log("AMD's upscale context failed (DittoSR only)\n"), c->real = nullptr;
    if (be && be->device) {
        c->ditto = new DittoSR(be->device, c->ci, g_cfg.s);
        if (!c->ditto->ok()) ditto_log("DittoSR: pipelines failed%s\n", c->real ? ", AMD's FSR instead" : ""), delete c->ditto, c->ditto = nullptr;
    } else {
        c->dittoVk = new DittoSRVk(bv->vkDevice, bv->vkPhysicalDevice, bv->vkDeviceProcAddr, c->ci, g_cfg.s);
        if (!c->dittoVk->ok()) ditto_log("DittoSR (Vulkan): setup failed%s\n", c->real ? ", AMD's FSR instead" : ""), delete c->dittoVk, c->dittoVk = nullptr;
    }
    if (!c->ditto && !c->dittoVk && !c->real) return delete c, FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    c->useDitto = c->ditto || c->dittoVk;
    c->compare = g_cfg.compare && c->real && c->useDitto;
    if (g_cfg.compare) ditto_log("comparison: %s\n", c->compare ? "on (DittoSR left, AMD's FSR right)" : "needs AMD's DLL alongside, off");
    g_ctx.insert(c);
    *context = (ffxContext)c;
    ditto_log("upscale context %p: %s (AMD's FSR %p)\n", (void *)c, c->ditto ? "DittoSR (D3D12)" : c->dittoVk ? "DittoSR (Vulkan)" : "FSR", c->real);
    return FFX_API_RETURN_OK;
}

FFX_API_ENTRY ffxReturnCode_t ffxDestroyContext(ffxContext *context, const ffxAllocationCallbacks *memCb) {
    std::lock_guard<std::mutex> lk(g_mu);
    bool amd = load_original();
    UpscaleCtx *c = ours(context);
    if (!c) return amd ? o_destroy(context, memCb) : FFX_API_RETURN_ERROR_PARAMETER;
    ditto_log("upscale context %p destroyed after %llu dispatches (%llu DittoSR)\n", (void *)c, (unsigned long long)c->dispatches,
              (unsigned long long)(c->ditto ? c->ditto->frames() : c->dittoVk ? c->dittoVk->frames() : 0));
    if (c->real) o_destroy(&c->real, memCb);
    delete c->ditto;
    delete c->dittoVk;
    g_ctx.erase(c);
    delete c;
    *context = nullptr;
    return FFX_API_RETURN_OK;
}

FFX_API_ENTRY ffxReturnCode_t ffxConfigure(ffxContext *context, const ffxConfigureDescHeader *desc) {
    std::lock_guard<std::mutex> lk(g_mu);
    bool amd = load_original();
    UpscaleCtx *c = ours(context);
    if (!c) return amd ? o_configure(context, desc) : FFX_API_RETURN_OK;  // (the global debug configuration: nothing to do)
    // FSR 3.1's velocity factor and reactiveness scale (0 - 1, both 1 by default): DittoSR's frame weight - OptiScaler's
    // sliders and presets (frame_weight_of). The rest of FSR's key-value tuning: AMD's context
    if (desc && desc->type == FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE) {
        auto *kv = (const ffxConfigureDescUpscaleKeyValue *)desc;
        bool vel = kv->key == FFX_API_CONFIGURE_UPSCALE_KEY_FVELOCITYFACTOR, rea = kv->key == FFX_API_CONFIGURE_UPSCALE_KEY_FREACTIVENESSSCALE;
        if ((vel || rea) && kv->ptr) {
            float v = *(const float *)kv->ptr;
            v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
            (vel ? c->velocity : c->reactiveness) = v;
            float fw = frame_weight_of(c);
            if (fw < 0.0f) ditto_log("configure: %s %g -> frame weight: dittosr.ini's\n", vel ? "velocity factor" : "reactiveness scale", v);
            else ditto_log("configure: %s %g -> frame weight %g\n", vel ? "velocity factor" : "reactiveness scale", v, fw);
        }
    }
    return c->real ? o_configure(&c->real, desc) : FFX_API_RETURN_OK;
}

FFX_API_ENTRY ffxReturnCode_t ffxQuery(ffxContext *context, ffxQueryDescHeader *desc) {
    std::lock_guard<std::mutex> lk(g_mu);
    bool amd = load_original();
    if (!desc) return FFX_API_RETURN_ERROR_PARAMETER;
    UpscaleCtx *c = ours(context);
    static int logged;
    if (logged < 16) logged++, ditto_log("ffxQuery 0x%llx (context %p%s)\n", (unsigned long long)desc->type, context ? *context : nullptr, c ? ", ours" : "");
    ffxReturnCode_t rc;
    if ((g_cfg.enabled || !amd) && (!context || !*context || c) && own_query(context, desc, &rc)) return rc;
    if (!c) return amd ? o_query(context, desc) : FFX_API_RETURN_NO_PROVIDER;
    if (c->real) return o_query(&c->real, desc);
    if (desc->type == FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE) {
        auto *q = (ffxQueryDescUpscaleGetGPUMemoryUsage *)desc;
        if (q->gpuMemoryUsageUpscaler) q->gpuMemoryUsageUpscaler->totalUsageInBytes = 0, q->gpuMemoryUsageUpscaler->aliasableUsageInBytes = 0;
        return FFX_API_RETURN_OK;
    }
    return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
}

FFX_API_ENTRY ffxReturnCode_t ffxDispatch(ffxContext *context, const ffxDispatchDescHeader *desc) {
    std::lock_guard<std::mutex> lk(g_mu);
    bool amd = load_original();
    UpscaleCtx *c = ours(context);
    if (!c) return amd ? o_dispatch(context, desc) : FFX_API_RETURN_ERROR_PARAMETER;
    if (!desc) return FFX_API_RETURN_ERROR_PARAMETER;
    if (desc->type != FFX_API_DISPATCH_DESC_TYPE_UPSCALE) {
        if (c->real) return o_dispatch(&c->real, desc);
        // (the reactive mask generation without AMD's DLL: nothing drawn - DittoSR reads the game's reactive mask, if any)
        return desc->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE_GENERATEREACTIVEMASK ? FFX_API_RETURN_OK : FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    }
    const ffxDispatchDescUpscale *d = (const ffxDispatchDescUpscale *)desc;
    // the toggle (a key press: the other upscaler from this frame on, its history started again)
    static bool down;
    bool now = g_cfg.toggleKey && (GetAsyncKeyState(g_cfg.toggleKey) & 0x8000);
    bool switched = false;
    bool req = InterlockedExchange(&g_reqToggle, 0) != 0;  // (the menu's)
    if (((now && !down) || req) && (c->ditto || c->dittoVk) && c->real) {
        c->useDitto = !c->useDitto, switched = true;
        if (!c->useDitto) c->resetReal = true;
        ditto_log("toggle: %s from dispatch %llu\n", c->useDitto ? "DittoSR" : "AMD's FSR", (unsigned long long)c->dispatches);
    }
    down = now;
    // the comparison (a key press, or compare=1): AMD's FSR draws the whole output first, DittoSR then its left half
    static bool cdown;
    bool cnow = g_cfg.compareKey && (GetAsyncKeyState(g_cfg.compareKey) & 0x8000);
    bool creq = InterlockedExchange(&g_reqCompare, 0) != 0;
    if (((cnow && !cdown) || creq) && (c->ditto || c->dittoVk) && c->real) {
        c->compare = !c->compare;
        if (c->compare) c->resetReal = true;
        ditto_log("comparison %s from dispatch %llu (DittoSR left, AMD's FSR right)\n", c->compare ? "on" : "off", (unsigned long long)c->dispatches);
    }
    cdown = cnow;
    // the settings: dittosr.ini's (F9 reads it again), with the frame weight from the configured velocity factor and the
    // sharpness from the dispatch when it enables sharpening and comes from the user (OptiScaler's slider)
    ditto_poll_reload();
    {
        DittoSettings s = ditto_settings();
        float fw = frame_weight_of(c);
        if (fw >= 0.0f) s.frameWeight = fw;
        bool gs = g_cfg.gameSharpness > 0 || (g_cfg.gameSharpness < 0 && from_optiscaler());
        if (gs && d->enableSharpening) s.sharpness = d->sharpness < 0.0f ? 0.0f : d->sharpness > 1.0f ? 1.0f : d->sharpness;
        if (s.sharpness != c->lastSharp || s.frameWeight != c->lastWeight) {
            ditto_log("settings: sharpness %g%s, frame weight %g%s\n", s.sharpness, gs && d->enableSharpening ? " (the dispatch's)" : "",
                      s.frameWeight, fw >= 0.0f ? " (FSR 3.1's settings')" : "");
            c->lastSharp = s.sharpness, c->lastWeight = s.frameWeight;
        }
        if (c->ditto) c->ditto->set_settings(s);
        if (c->dittoVk) c->dittoVk->set_settings(s);
    }
    // (described in full: the first few, and each whose render or output size changed - a resolution change, an
    // upscale ratio override)
    bool sizes = d->renderSize.width != c->lastRw || d->renderSize.height != c->lastRh || d->upscaleSize.width != c->lastOw ||
                 d->upscaleSize.height != c->lastOh;
    c->lastRw = d->renderSize.width, c->lastRh = d->renderSize.height, c->lastOw = d->upscaleSize.width, c->lastOh = d->upscaleSize.height;
    if (c->dispatches < (uint64_t)g_cfg.logDispatches || (sizes && c->sizeLogs++ < 32)) {
        log_chain("ffxDispatch(upscale)", desc);
        ditto_log("dispatch %llu: render %ux%u -> %ux%u, jitter %.4f %.4f, mv scale %.2f %.2f, reset %d, sharpen %d (%.2f), "
                  "dt %.2f ms, pre-exposure %g, near %g, far %g, fov %.4f, m/unit %g, flags 0x%x, cmdlist %p\n",
                  (unsigned long long)c->dispatches, d->renderSize.width, d->renderSize.height, d->upscaleSize.width, d->upscaleSize.height,
                  d->jitterOffset.x, d->jitterOffset.y, d->motionVectorScale.x, d->motionVectorScale.y, d->reset, d->enableSharpening,
                  d->sharpness, d->frameTimeDelta, d->preExposure, d->cameraNear, d->cameraFar, d->cameraFovAngleVertical,
                  d->viewSpaceToMetersFactor, d->flags, d->commandList);
        bool vk = c->dittoVk != nullptr;
        log_res("color", d->color, vk), log_res("depth", d->depth, vk), log_res("motion", d->motionVectors, vk), log_res("exposure", d->exposure, vk);
        log_res("reactive", d->reactive, vk), log_res("transp/comp", d->transparencyAndComposition, vk), log_res("output", d->output, vk);
    }
    c->dispatches++;
    ditto_stats_update(1, d->renderSize.width, d->renderSize.height, d->upscaleSize.width, d->upscaleSize.height,
                       c->useDitto && (c->ditto || c->dittoVk), c->compare && c->real, c->real != nullptr);
    if (c->useDitto && (c->ditto || c->dittoVk)) {
        bool cmp = c->compare && c->real;
        if (cmp) {  // (AMD's FSR first, on the whole output: DittoSR's left half then goes over it)
            ffxDispatchDescUpscale r = *d;
            if (c->resetReal) r.reset = true, c->resetReal = false;
            o_dispatch(&c->real, &r.header);
        }
        if (c->ditto) c->ditto->set_split(cmp);
        if (c->dittoVk) c->dittoVk->set_split(cmp);
        bool ran = false;
        uint64_t n = 0;
        if (c->ditto) {
            DittoFrame fr;
            ran = ditto_frame_from_ffx(d, c->ci, &fr) && c->ditto->dispatch(fr, switched), n = c->ditto->frames();
        } else {
            DittoFrameVk fr;
            ran = ditto_frame_from_ffx_vk(d, c->ci, &fr) && c->dittoVk->dispatch(fr, switched), n = c->dittoVk->frames();
        }
        if (ran) {
            if (n == 1 || n % 3600 == 0) ditto_log("DittoSR ran (frame %llu)\n", (unsigned long long)n);
            return FFX_API_RETURN_OK;
        }
        static int fails;
        if (fails++ < 5) ditto_log("DittoSR could not run this dispatch%s\n", c->real ? ": AMD's FSR instead" : "");
    }
    if (!c->real) return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    if (c->resetReal) {
        c->resetReal = false;
        ffxDispatchDescUpscale r = *d;
        r.reset = true;
        return o_dispatch(&c->real, &r.header);
    }
    return o_dispatch(&c->real, desc);
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        g_self = h;
        ditto_init(h, "dittosr");
    }
    return TRUE;
}
