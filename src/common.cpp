// The shared parts: the log (dittosr.log next to the DLL) and the settings (dittosr.ini next to it).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <share.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <mutex>

#include "dittosr.h"

DittoConfig g_cfg;
DittoStats g_stats;
volatile long g_reqToggle, g_reqCompare;
static std::mutex g_sMu;  // (g_cfg.s: the upscale's thread and the menu's, the game's present thread)
static volatile long g_menuChanged, g_reqReload;
char g_dir[260];
static char g_logName[32] = "dittosr";
static FILE *g_log;

void ditto_log(const char *fmt, ...) {
    if (!g_log) {
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s%s.log", g_dir, g_logName);
        g_log = _fsopen(p, "w", _SH_DENYWR);
        if (!g_log) return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fflush(g_log);
}

static float ini_float(const char *ini, const char *key, float def) {
    char b[64], d[32];
    snprintf(d, sizeof d, "%g", def);
    GetPrivateProfileStringA("dittosr", key, d, b, sizeof b, ini);
    return (float)atof(b);
}

void (*g_settingsOverride)(DittoSettings &s);
bool (*g_settingsSave)(const DittoSettings &s, const char *key);

// dittosr.ini: [dittosr] enabled, sharpness, frame_weight (F9 reads them again); [debug] enabled (F10, the log in
// full), indicator. Every other key - the reconstruction's knobs, the performance switches, the debug views - is still
// read from [dittosr] when present, for tuning, but the shipped file leaves them out (README.md, "Advanced keys").
// The override (OptiScaler's [Plugin.<name>]) then goes over the file's values, and the result is stored at once: a
// menu's change made meanwhile is not lost between the two.
static void load_config() {
    char ini[MAX_PATH];
    snprintf(ini, sizeof ini, "%sdittosr.ini", g_dir);
    DittoConfig &c = g_cfg;
    c.enabled = GetPrivateProfileIntA("dittosr", "enabled", 1, ini) != 0;
    c.debug = GetPrivateProfileIntA("debug", "enabled", 0, ini) != 0;
    char b[32];
    GetPrivateProfileStringA("dittosr", "toggle_key", "0x79", b, sizeof b, ini);
    c.toggleKey = c.debug ? (int)strtol(b, nullptr, 0) : 0;
    GetPrivateProfileStringA("dittosr", "reload_key", "0x78", b, sizeof b, ini);
    c.reloadKey = (int)strtol(b, nullptr, 0);  // (always: the values tuned live)
    c.logDispatches = GetPrivateProfileIntA("dittosr", "log_dispatches", c.debug ? 3 : 0, ini);
    c.logPasses = GetPrivateProfileIntA("dittosr", "log_passes", 0, ini) != 0;
    DittoSettings s;
    s.frameWeight = ini_float(ini, "frame_weight", s.frameWeight);
    s.frameWeightOverride = GetPrivateProfileIntA("dittosr", "frame_weight_override", s.frameWeightOverride, ini) != 0;
    s.alphaRest = ini_float(ini, "alpha_rest", s.alphaRest);
    s.conf = ini_float(ini, "conf", s.conf);
    s.gammaRest = ini_float(ini, "gamma_rest", s.gammaRest);
    if (s.gammaRest <= 0.0f) s.gammaRest = DittoSettings().gammaRest;  // (0: the default, as before it had a slider)
    s.width = ini_float(ini, "width", s.width);
    s.react = ini_float(ini, "react", s.react);
    s.reactiveScale = ini_float(ini, "reactive_scale", s.reactiveScale);
    s.exposureScale = ini_float(ini, "exposure_scale", s.exposureScale);
    s.jitterSignX = ini_float(ini, "jitter_sign_x", s.jitterSignX);
    s.jitterSignY = ini_float(ini, "jitter_sign_y", s.jitterSignY);
    s.mvSign = ini_float(ini, "mv_sign", s.mvSign);
    s.motionGamma = ini_float(ini, "motion_gamma", s.motionGamma);
    s.motionShare = ini_float(ini, "motion_share", s.motionShare);
    s.motionMinMax = ini_float(ini, "motion_minmax", s.motionMinMax);
    s.sharpness = ini_float(ini, "sharpness", s.sharpness);
    s.changeScale = ini_float(ini, "change_scale", s.changeScale);
    s.shareFar = ini_float(ini, "share_far", s.shareFar);
    s.shareNear = ini_float(ini, "share_near", s.shareNear);
    s.encodePower = ini_float(ini, "encode_power", s.encodePower);
    s.motionNear = ini_float(ini, "motion_near", s.motionNear);
    s.splitClip = ini_float(ini, "split_clip", s.splitClip);
    s.history10 = GetPrivateProfileIntA("dittosr", "history_10bit", s.history10, ini);
    s.fp16 = GetPrivateProfileIntA("dittosr", "fp16", s.fp16, ini);
    s.autoReactive = ini_float(ini, "auto_reactive", s.autoReactive);
    s.stabStep = ini_float(ini, "stab_step", s.stabStep);
    s.antiShimmer = ini_float(ini, "anti_shimmer", s.antiShimmer);
    s.historySharpen = ini_float(ini, "history_sharpen", s.historySharpen);
    s.fusedSharpen = ini_float(ini, "fused_sharpen", s.fusedSharpen);
    s.sharpenMode = GetPrivateProfileIntA("dittosr", "sharpen_mode", s.sharpenMode, ini);
    if (s.sharpenMode < 0 || s.sharpenMode > 2) s.sharpenMode = 0;
    s.relight = GetPrivateProfileIntA("dittosr", "relight", s.relight, ini) != 0;
    s.aoRadius = ini_float(ini, "ao_radius", s.aoRadius), s.aoScreen = ini_float(ini, "ao_screen", s.aoScreen);
    s.aoRays = ini_float(ini, "ao_rays", s.aoRays), s.aoContact = ini_float(ini, "ao_contact", s.aoContact);
    s.aoStrength = ini_float(ini, "ao_strength", s.aoStrength), s.giStrength = ini_float(ini, "gi_strength", s.giStrength);
    s.giDark = ini_float(ini, "gi_dark", s.giDark), s.giCeiling = ini_float(ini, "gi_ceiling", s.giCeiling);
    s.aoTemporal = GetPrivateProfileIntA("dittosr", "ao_temporal", s.aoTemporal, ini) != 0;
    s.aoTemporalWeight = ini_float(ini, "ao_temporal_weight", s.aoTemporalWeight);
    s.aoSpatial = ini_float(ini, "ao_spatial_radius", s.aoSpatial);
    s.aoPlaneFilter = GetPrivateProfileIntA("dittosr", "ao_plane_filter", s.aoPlaneFilter, ini) != 0;
    s.aoSparseFilter = GetPrivateProfileIntA("dittosr", "ao_sparse_filter", s.aoSparseFilter, ini) != 0;
    s.aoMips = GetPrivateProfileIntA("dittosr", "ao_mips", s.aoMips, ini) != 0;
    s.aoPower = GetPrivateProfileIntA("dittosr", "ao_power", s.aoPower, ini) != 0;
    s.aoBackBoost = GetPrivateProfileIntA("dittosr", "ao_backface_boost", s.aoBackBoost, ini) != 0;
    s.aoLut = GetPrivateProfileIntA("dittosr", "ao_lut", s.aoLut, ini) != 0;
    s.aoWg16 = GetPrivateProfileIntA("dittosr", "ao_workgroup16", s.aoWg16, ini) != 0;
    s.aoColorMips = GetPrivateProfileIntA("dittosr", "ao_color_mips", s.aoColorMips, ini) != 0;
    s.aoSamples = ini_float(ini, "ao_samples", s.aoSamples);
    s.aoHalf = GetPrivateProfileIntA("dittosr", "ao_half", s.aoHalf, ini) != 0;
    s.aoDecorrelate = GetPrivateProfileIntA("dittosr", "ao_decorrelate", s.aoDecorrelate, ini) != 0;
    s.aoCacao = GetPrivateProfileIntA("dittosr", "ao_cacao", s.aoCacao, ini) != 0;
    s.aoCacaoPasses = GetPrivateProfileIntA("dittosr", "ao_cacao_passes", s.aoCacaoPasses, ini);
    s.aoPackNd = GetPrivateProfileIntA("dittosr", "ao_pack_nd", s.aoPackNd, ini) != 0;
    s.aoUpCompose = GetPrivateProfileIntA("dittosr", "ao_up_compose", s.aoUpCompose, ini) != 0;
    s.aoFuse = GetPrivateProfileIntA("dittosr", "ao_fuse", s.aoFuse, ini) != 0;
    s.aoHistLen = GetPrivateProfileIntA("dittosr", "ao_history_length", s.aoHistLen, ini) != 0;
    s.aoAmbientOnly = GetPrivateProfileIntA("dittosr", "ao_ambient_only", s.aoAmbientOnly, ini) != 0;
    s.aoEmitterCos = GetPrivateProfileIntA("dittosr", "ao_emitter_cosine", s.aoEmitterCos, ini) != 0;
    s.aoBetterNormals = GetPrivateProfileIntA("dittosr", "ao_better_normals", s.aoBetterNormals, ini) != 0;
    s.aoGiWide = GetPrivateProfileIntA("dittosr", "ao_gi_wide", s.aoGiWide, ini) != 0;
    s.aoGiLong = GetPrivateProfileIntA("dittosr", "ao_gi_long_history", s.aoGiLong, ini) != 0;
    s.aoHalfPrep = GetPrivateProfileIntA("dittosr", "ao_half_prepass", s.aoHalfPrep, ini) != 0;
    s.aoCurveLut = GetPrivateProfileIntA("dittosr", "ao_curve_lut", s.aoCurveLut, ini) != 0;
    s.fgAmd = GetPrivateProfileIntA("dittosr", "frame_gen_amd", s.fgAmd, ini) != 0;
    s.aoNoise8 = GetPrivateProfileIntA("dittosr", "ao_noise_cycle8", s.aoNoise8, ini) != 0;
    s.aoGiFalloff = GetPrivateProfileIntA("dittosr", "ao_gi_falloff", s.aoGiFalloff, ini) != 0;
    s.aoBlueNoise = GetPrivateProfileIntA("dittosr", "ao_blue_noise", s.aoBlueNoise, ini) != 0;
    s.aoAlbedo2 = GetPrivateProfileIntA("dittosr", "ao_better_albedo", s.aoAlbedo2, ini) != 0;
    s.aoEdgeFill = GetPrivateProfileIntA("dittosr", "ao_edge_fill", s.aoEdgeFill, ini) != 0;
    s.aoAtrous = GetPrivateProfileIntA("dittosr", "ao_gi_atrous", s.aoAtrous, ini) != 0;
    s.aoFp16 = GetPrivateProfileIntA("dittosr", "ao_fp16", s.aoFp16, ini) != 0;
    s.aoGiInterleave = GetPrivateProfileIntA("dittosr", "ao_gi_interleave", s.aoGiInterleave, ini) != 0;
    s.aoFarCheap = GetPrivateProfileIntA("dittosr", "ao_far_cheap", s.aoFarCheap, ini) != 0;
    s.aoAlbedoStrength = ini_float(ini, "ao_albedo_strength", s.aoAlbedoStrength);
    s.fxLocalExp = ini_float(ini, "fx_local_exposure", s.fxLocalExp);
    s.aoFadeMeters = ini_float(ini, "ao_fade_distance", s.aoFadeMeters);
    {  // (the curve: "x,y x,y ...", by x, 2 - 8 points in [0, 1]; else the line)
        char cb[256];
        GetPrivateProfileStringA("dittosr", "curve", "", cb, sizeof cb, ini);
        float cx[8], cy[8];
        int n = 0, used = 0;
        const char *c = cb;
        while (n < 8 && sscanf(c, " %f , %f%n", &cx[n], &cy[n], &used) == 2) c += used, n++;
        bool ok = n >= 2;
        for (int i = 0; i < n && ok; i++) ok = cx[i] >= 0.0f && cx[i] <= 1.0f && cy[i] >= 0.0f && cy[i] <= 1.0f && (i == 0 || cx[i] > cx[i - 1]);
        if (ok) {
            s.curveN = n;
            for (int i = 0; i < n; i++) s.curveX[i] = cx[i], s.curveY[i] = cy[i];
        }
    }
    s.envTol = ini_float(ini, "env_tol", s.envTol), s.envDecay = ini_float(ini, "env_decay", s.envDecay);
    s.envGain = ini_float(ini, "env_gain", s.envGain), s.envRest = ini_float(ini, "env_rest", s.envRest);
    s.rampLo = ini_float(ini, "ramp_lo", s.rampLo), s.rampHi = ini_float(ini, "ramp_hi", s.rampHi);
    s.envCap = ini_float(ini, "env_cap", s.envCap);
    s.envRel = ini_float(ini, "env_rel", s.envRel), s.alphaSlow = ini_float(ini, "alpha_slow", s.alphaSlow);
    // (the upscale ratios the FidelityFX API answers a game's quality modes with: 0 FSR 3.1's)
    c.ratio[0] = ini_float(ini, "ratio_quality", 0.0f);
    c.ratio[1] = ini_float(ini, "ratio_balanced", 0.0f);
    c.ratio[2] = ini_float(ini, "ratio_performance", 0.0f);
    c.ratio[3] = ini_float(ini, "ratio_ultra_performance", 0.0f);
    GetPrivateProfileStringA("dittosr", "compare_key", "0", b, sizeof b, ini);
    c.compareKey = (int)strtol(b, nullptr, 0);
    GetPrivateProfileStringA("dittosr", "menu_key", "0x76", b, sizeof b, ini);
    c.menuKey = (int)strtol(b, nullptr, 0);
    GetPrivateProfileStringA("dittosr", "capture_key", "0", b, sizeof b, ini);
    c.captureKey = (int)strtol(b, nullptr, 0);
    c.captureFrame = GetPrivateProfileIntA("dittosr", "capture_frame", 0, ini);
    c.compare = GetPrivateProfileIntA("dittosr", "compare", 0, ini) != 0;
    c.gameSharpness = GetPrivateProfileIntA("dittosr", "game_sharpness", -1, ini);
    c.frameGen = GetPrivateProfileIntA("dittosr", "frame_gen", 1, ini);
    s.indicator = GetPrivateProfileIntA("debug", "indicator", GetPrivateProfileIntA("dittosr", "indicator", 0, ini), ini) != 0;
    // (debug_view: 1 the motion, 2 the new frame's share, 3 the stability level, 4 clip / cut / confidence)
    s.debugView = GetPrivateProfileIntA("dittosr", "debug_view", 0, ini);
    if (g_settingsOverride) g_settingsOverride(s);  // (outside the settings' lock: it calls into OptiScaler)
    {
        std::lock_guard<std::mutex> lk(g_sMu);
        c.s = s;
    }
    ditto_log("config: enabled %d, debug %d, sharpness %g, frame_weight %g, auto_reactive %g, ratios %g %g %g %g\n", c.enabled,
              c.debug, s.sharpness, s.frameWeight, s.autoReactive, c.ratio[0], c.ratio[1], c.ratio[2], c.ratio[3]);
    if (!c.debug) return;
    ditto_log("config: toggle key 0x%02x, indicator %d, debug_view %d, exposure_scale %g, reactive_scale %g, "
              "alpha_rest %g, conf %g, width %g, react %g, jitter sign %g %g, mv_sign %g, "
              "motion gamma %g share %g minmax %g, sharpness %g\n",
              c.toggleKey, s.indicator, s.debugView, s.exposureScale, s.reactiveScale, s.alphaRest, s.conf, s.width,
              s.react, s.jitterSignX, s.jitterSignY, s.mvSign, s.motionGamma, s.motionShare,
              s.motionMinMax, s.sharpness);
}

bool ditto_poll_reload() {
    static bool down;
    bool now = g_cfg.reloadKey && (GetAsyncKeyState(g_cfg.reloadKey) & 0x8000);
    bool pressed = (now && !down) || InterlockedExchange(&g_reqReload, 0);
    down = now;
    if (pressed) ditto_log("reload: dittosr.ini\n"), load_config();
    return pressed || InterlockedExchange(&g_menuChanged, 0);
}

DittoSettings ditto_settings() {
    std::lock_guard<std::mutex> lk(g_sMu);
    return g_cfg.s;
}

void ditto_set_settings(const DittoSettings &s) {
    {
        std::lock_guard<std::mutex> lk(g_sMu);
        g_cfg.s = s;
    }
    InterlockedExchange(&g_menuChanged, 1);
}

void ditto_request_reload() { InterlockedExchange(&g_reqReload, 1); }

// The menu's Save: each setting it shows into [dittosr] - sharpness and frame_weight always (the shipped file's), the
// others only when they differ from the defaults (removed when back at them: the file keeps its shipped keys alone).
// A setting the override holds (OptiScaler's [Plugin.<name>]) is saved there instead, dittosr.ini's value left as it was
// - else the two would hold the same value and the plugin tab's Defaults would bring nothing back.
bool ditto_save_settings() {
    char ini[MAX_PATH];
    snprintf(ini, sizeof ini, "%sdittosr.ini", g_dir);
    DittoSettings s = ditto_settings(), d;
    auto held = [&](const char *key) { return g_settingsSave && g_settingsSave(s, key); };
    auto put = [&](const char *key, float v, float def, bool always) {
        if (held(key)) return true;
        char b[32];
        snprintf(b, sizeof b, "%g", v);
        bool keep = always || v != def;
        return WritePrivateProfileStringA("dittosr", key, keep ? b : nullptr, ini) != 0 || !keep;
    };
    bool ok = put("sharpness", s.sharpness, d.sharpness, true);
    ok = put("frame_weight", s.frameWeight, d.frameWeight, true) && ok;
    ok = put("anti_shimmer", s.antiShimmer, d.antiShimmer, false) && ok;
    ok = put("frame_weight_override", s.frameWeightOverride ? 1.0f : 0.0f, d.frameWeightOverride ? 1.0f : 0.0f, false) && ok;
    ok = put("sharpen_mode", (float)s.sharpenMode, (float)d.sharpenMode, false) && ok;
    ok = put("relight", s.relight ? 1.0f : 0.0f, 0.0f, false) && ok;
    ok = put("ao_strength", s.aoStrength, d.aoStrength, false) && ok;
    ok = put("gi_strength", s.giStrength, d.giStrength, false) && ok;
    ok = put("gi_dark", s.giDark, d.giDark, false) && ok;
    ok = put("gi_ceiling", s.giCeiling, d.giCeiling, false) && ok;
    ok = put("ao_rays", s.aoRays, d.aoRays, false) && ok;
    ok = put("ao_radius", s.aoRadius, d.aoRadius, false) && ok;
    ok = put("ao_screen", s.aoScreen, d.aoScreen, false) && ok;
    ok = put("ao_contact", s.aoContact, d.aoContact, false) && ok;
    ok = put("ao_spatial_radius", s.aoSpatial, d.aoSpatial, false) && ok;
    auto b = [](bool v) { return v ? 1.0f : 0.0f; };
    ok = put("ao_plane_filter", b(s.aoPlaneFilter), b(d.aoPlaneFilter), false) && ok;
    ok = put("ao_sparse_filter", b(s.aoSparseFilter), b(d.aoSparseFilter), false) && ok;
    ok = put("ao_mips", b(s.aoMips), b(d.aoMips), false) && ok;
    ok = put("ao_power", b(s.aoPower), b(d.aoPower), false) && ok;
    ok = put("ao_backface_boost", b(s.aoBackBoost), b(d.aoBackBoost), false) && ok;
    ok = put("ao_lut", b(s.aoLut), b(d.aoLut), false) && ok;
    ok = put("ao_workgroup16", b(s.aoWg16), b(d.aoWg16), false) && ok;
    ok = put("ao_color_mips", b(s.aoColorMips), b(d.aoColorMips), false) && ok;
    ok = put("ao_samples", s.aoSamples, d.aoSamples, false) && ok;
    ok = put("ao_half", b(s.aoHalf), b(d.aoHalf), false) && ok;
    ok = put("ao_decorrelate", b(s.aoDecorrelate), b(d.aoDecorrelate), false) && ok;
    ok = put("ao_cacao", b(s.aoCacao), b(d.aoCacao), false) && ok;
    ok = put("ao_pack_nd", b(s.aoPackNd), b(d.aoPackNd), false) && ok;
    ok = put("ao_up_compose", b(s.aoUpCompose), b(d.aoUpCompose), false) && ok;
    ok = put("ao_fuse", b(s.aoFuse), b(d.aoFuse), false) && ok;
    ok = put("ao_history_length", b(s.aoHistLen), b(d.aoHistLen), false) && ok;
    ok = put("ao_ambient_only", b(s.aoAmbientOnly), b(d.aoAmbientOnly), false) && ok;
    ok = put("ao_emitter_cosine", b(s.aoEmitterCos), b(d.aoEmitterCos), false) && ok;
    ok = put("ao_better_normals", b(s.aoBetterNormals), b(d.aoBetterNormals), false) && ok;
    ok = put("ao_gi_wide", b(s.aoGiWide), b(d.aoGiWide), false) && ok;
    ok = put("ao_gi_long_history", b(s.aoGiLong), b(d.aoGiLong), false) && ok;
    ok = put("ao_half_prepass", b(s.aoHalfPrep), b(d.aoHalfPrep), false) && ok;
    ok = put("ao_curve_lut", b(s.aoCurveLut), b(d.aoCurveLut), false) && ok;
    ok = put("frame_gen_amd", b(s.fgAmd), b(d.fgAmd), false) && ok;
    ok = put("ao_noise_cycle8", b(s.aoNoise8), b(d.aoNoise8), false) && ok;
    ok = put("ao_gi_falloff", b(s.aoGiFalloff), b(d.aoGiFalloff), false) && ok;
    ok = put("ao_blue_noise", b(s.aoBlueNoise), b(d.aoBlueNoise), false) && ok;
    ok = put("ao_better_albedo", b(s.aoAlbedo2), b(d.aoAlbedo2), false) && ok;
    ok = put("ao_edge_fill", b(s.aoEdgeFill), b(d.aoEdgeFill), false) && ok;
    ok = put("ao_gi_atrous", b(s.aoAtrous), b(d.aoAtrous), false) && ok;
    ok = put("ao_fp16", b(s.aoFp16), b(d.aoFp16), false) && ok;
    ok = put("ao_gi_interleave", b(s.aoGiInterleave), b(d.aoGiInterleave), false) && ok;
    ok = put("ao_far_cheap", b(s.aoFarCheap), b(d.aoFarCheap), false) && ok;
    ok = put("ao_albedo_strength", s.aoAlbedoStrength, d.aoAlbedoStrength, false) && ok;
    ok = put("fx_local_exposure", s.fxLocalExp, d.fxLocalExp, false) && ok;
    ok = put("ao_fade_distance", s.aoFadeMeters, d.aoFadeMeters, false) && ok;
    {
        char cb[256] = "";
        size_t m = 0;
        for (int i = 0; i < s.curveN && i < 8; i++) m += snprintf(cb + m, sizeof cb - m, "%s%.4g,%.4g", i ? " " : "", s.curveX[i], s.curveY[i]);
        // (written when it differs from the default curve - the line too, then)
        bool same = s.curveN == d.curveN;
        for (int i = 0; i < s.curveN && same; i++) same = s.curveX[i] == d.curveX[i] && s.curveY[i] == d.curveY[i];
        if (!held("curve")) ok = WritePrivateProfileStringA("dittosr", "curve", same ? nullptr : cb, ini) != 0 || same;
    }
    ok = put("ao_temporal", s.aoTemporal ? 1.0f : 0.0f, d.aoTemporal ? 1.0f : 0.0f, false) && ok;
    ok = put("ao_temporal_weight", s.aoTemporalWeight, d.aoTemporalWeight, false) && ok;
    ok = put("auto_reactive", s.autoReactive, d.autoReactive, false) && ok;
    ok = put("debug_view", (float)s.debugView, (float)d.debugView, false) && ok;
    if (!held("indicator")) ok = WritePrivateProfileStringA("debug", "indicator", s.indicator ? "1" : "0", ini) != 0 && ok;
    if (g_settingsSave) ok = g_settingsSave(s, nullptr) && ok;  // (what the override took, written out)
    ditto_log("menu: settings saved to dittosr.ini%s\n", ok ? "" : " (failed)");
    return ok;
}

void ditto_stats_update(int api, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, bool active, bool compare, bool hasReal) {
    g_stats.api = api, g_stats.rw = rw, g_stats.rh = rh, g_stats.ow = ow, g_stats.oh = oh;
    g_stats.active = active, g_stats.compare = compare, g_stats.hasReal = hasReal;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    g_stats.lastQpc = t.QuadPart;
}

void ditto_init(void *module, const char *logName) {
    snprintf(g_logName, sizeof g_logName, "%s", logName);
    GetModuleFileNameA((HMODULE)module, g_dir, MAX_PATH);
    char *s = strrchr(g_dir, '\\');
    if (s) s[1] = 0;
    load_config();
    ditto_log("%s loaded from %s\n", DITTOSR_VERSION_NAME, g_dir);
}
