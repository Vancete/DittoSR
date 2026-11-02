// The shared parts: the log (dittosr.log next to the DLL) and the settings (dittosr.ini next to it).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <share.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "dittosr.h"

DittoConfig g_cfg;
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

// dittosr.ini: [dittosr] enabled, sharpness, frame_weight (F9 reads them again); [debug] enabled (F10, the log in
// full), indicator. Every other key - the reconstruction's knobs, the performance switches, the debug views - is still
// read from [dittosr] when present, for tuning, but the shipped file leaves them out (README.md, "Advanced keys").
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
    DittoSettings &s = c.s;
    s = DittoSettings();
    s.frameWeight = ini_float(ini, "frame_weight", s.frameWeight);
    s.alphaRest = ini_float(ini, "alpha_rest", s.alphaRest);
    s.conf = ini_float(ini, "conf", s.conf);
    s.gammaRest = ini_float(ini, "gamma_rest", s.gammaRest);
    s.width = ini_float(ini, "width", s.width);
    s.react = ini_float(ini, "react", s.react);
    s.edge = ini_float(ini, "edge", s.edge);
    s.lockLines = ini_float(ini, "lock_lines", s.lockLines);
    s.reactiveScale = ini_float(ini, "reactive_scale", s.reactiveScale);
    s.exposureScale = ini_float(ini, "exposure_scale", s.exposureScale);
    s.jitterSignX = ini_float(ini, "jitter_sign_x", s.jitterSignX);
    s.jitterSignY = ini_float(ini, "jitter_sign_y", s.jitterSignY);
    s.mvSign = ini_float(ini, "mv_sign", s.mvSign);
    s.fovHDeg = ini_float(ini, "fov_h_deg", s.fovHDeg);
    s.motionGamma = ini_float(ini, "motion_gamma", s.motionGamma);
    s.motionShare = ini_float(ini, "motion_share", s.motionShare);
    s.motionMinMax = ini_float(ini, "motion_minmax", s.motionMinMax);
    s.depthMode = GetPrivateProfileIntA("dittosr", "depth_mode", s.depthMode, ini);
    s.sharpness = ini_float(ini, "sharpness", s.sharpness);
    s.changeScale = ini_float(ini, "change_scale", s.changeScale);
    s.shareFar = ini_float(ini, "share_far", s.shareFar);
    s.shareNear = ini_float(ini, "share_near", s.shareNear);
    s.encodePower = ini_float(ini, "encode_power", s.encodePower);
    s.motionWidth = ini_float(ini, "motion_width", s.motionWidth);
    s.motionNear = ini_float(ini, "motion_near", s.motionNear);
    s.splitClip = ini_float(ini, "split_clip", s.splitClip);
    s.history10 = GetPrivateProfileIntA("dittosr", "history_10bit", s.history10, ini);
    s.fp16 = GetPrivateProfileIntA("dittosr", "fp16", s.fp16, ini);
    s.indicator = GetPrivateProfileIntA("debug", "indicator", GetPrivateProfileIntA("dittosr", "indicator", 0, ini), ini) != 0;
    // (debug_view: 1 the motion, 2 the new frame's share, 3 the stability level, 4 clip / cut / confidence; debug_motion=1
    // is debug_view=1)
    s.debugView = GetPrivateProfileIntA("dittosr", "debug_view", GetPrivateProfileIntA("dittosr", "debug_motion", 0, ini), ini);
    ditto_log("config: enabled %d, debug %d, sharpness %g, frame_weight %g\n", c.enabled, c.debug, s.sharpness, s.frameWeight);
    if (!c.debug) return;
    ditto_log("config: toggle key 0x%02x, indicator %d, debug_view %d, exposure_scale %g, reactive_scale %g, "
              "alpha_rest %g, conf %g, width %g, react %g, edge %g, lock_lines %g, jitter sign %g %g, mv_sign %g, fov_h_deg %g, "
              "motion gamma %g share %g minmax %g, depth_mode %d, sharpness %g\n",
              c.toggleKey, s.indicator, s.debugView, s.exposureScale, s.reactiveScale, s.alphaRest, s.conf, s.width,
              s.react, s.edge, s.lockLines, s.jitterSignX, s.jitterSignY, s.mvSign, s.fovHDeg, s.motionGamma, s.motionShare,
              s.motionMinMax, s.depthMode, s.sharpness);
}

bool ditto_poll_reload() {
    static bool down;
    bool now = g_cfg.reloadKey && (GetAsyncKeyState(g_cfg.reloadKey) & 0x8000);
    bool pressed = now && !down;
    down = now;
    if (pressed) ditto_log("reload: dittosr.ini\n"), load_config();
    return pressed;
}

void ditto_init(void *module, const char *logName) {
    snprintf(g_logName, sizeof g_logName, "%s", logName);
    GetModuleFileNameA((HMODULE)module, g_dir, MAX_PATH);
    char *s = strrchr(g_dir, '\\');
    if (s) s[1] = 0;
    load_config();
    ditto_log("%s loaded from %s\n", DITTOSR_VERSION_NAME, g_dir);
}
