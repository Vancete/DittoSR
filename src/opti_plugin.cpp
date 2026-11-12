// DittoSR's tab in OptiScaler's menu, through its plugin API (optiscaler/optiscaler_plugin.h). OptiScaler calls
// OptiPluginInit when it loads this DLL as a custom upscaler (custom\<name>\ffx or ffx_vk). The tab holds what
// OptiScaler's own settings don't reach - the frame weight, the anti-shimmer, the motion detail, the debug views, the
// indicator - kept in OptiScaler.ini's [Plugin.<name>] under dittosr.ini's key names, over dittosr.ini's values (a key
// missing there: dittosr.ini's value). They apply from the next upscale; OptiScaler's Save INI, or the tab's Save,
// writes them.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <math.h>
#include <stdio.h>

#include "dittosr.h"
#include "optiscaler/optiscaler_plugin.h"

namespace {

const OptiPluginApi *g_api;
OptiPlugin *g_plugin;
bool g_unsaved;

const char *const kKeys[] = {"frame_weight", "frame_weight_override", "anti_shimmer", "history_sharpen", "debug_view", "indicator"};

// the anti-shimmer's steps, as the F7 menu's (-1: by the ratio)
const float kShimmer[] = {-1.0f, 0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
const char *const kShimmerNames[] = {"Auto", "Off", "0.1", "0.2", "0.3", "0.4", "0.5", "0.6"};
const int kShimmerCount = sizeof kShimmer / sizeof kShimmer[0];

// (the motion view, 1, from dittosr.ini's debug_view only, as in the F7 menu: shown when set there, not picked here)
const char *const kDebugNames[] = {"Off", "Motion", "New frame's share", "Stability", "Clip / cut / confidence"};
const bool kDebugDisabled[] = {false, true, false, false, false};
const int kDebugCount = sizeof kDebugNames / sizeof kDebugNames[0];

int shimmer_index(float v) {
    int best = 0;
    for (int i = 0; i < kShimmerCount; i++)
        if (fabsf(kShimmer[i] - v) < fabsf(kShimmer[best] - v)) best = i;
    return best;
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// [Plugin.<name>]'s values over s (dittosr.ini's): a key missing, or "auto", keeps s's
void apply_ini(DittoSettings &s) {
    s.frameWeight = clampf(g_api->GetFloat(g_plugin, "frame_weight", s.frameWeight), 0.25f, 4.0f);
    s.frameWeightOverride = g_api->GetBool(g_plugin, "frame_weight_override", s.frameWeightOverride);
    s.antiShimmer = g_api->GetFloat(g_plugin, "anti_shimmer", s.antiShimmer);
    s.historySharpen = clampf(g_api->GetFloat(g_plugin, "history_sharpen", s.historySharpen), 0.0f, 2.0f);
    int dv = g_api->GetInt(g_plugin, "debug_view", s.debugView);
    s.debugView = dv < 0 || dv >= kDebugCount ? 0 : dv;
    s.indicator = g_api->GetBool(g_plugin, "indicator", s.indicator);
}

void draw_tab(void *) {
    const OptiPluginApi *a = g_api;
    DittoSettings s = ditto_settings();
    bool changed = false;

    a->Section("Image");
    // (without the override the slider shows the weight in use: FSR 3.1's settings' when they set one, else DittoSR's)
    float fsr = g_stats.fsrFrameWeight;
    bool fromFsr = fsr >= 0.0f;
    if (a->Checkbox("Override##frameWeight", &s.frameWeightOverride)) {
        if (s.frameWeightOverride && fromFsr) {  // (the slider starts from the weight in use: no jump)
            s.frameWeight = fsr;
            a->SetFloat(g_plugin, "frame_weight", s.frameWeight);
        }
        a->SetBool(g_plugin, "frame_weight_override", s.frameWeightOverride), changed = true;
    }
    a->ItemTooltip("Off: OptiScaler's FSR preset and Velocity Factor / Reactive Scale set the frame weight\n"
                   "(Stability 1, Motion 3; at Default, DittoSR's own).\n"
                   "On: the slider below, whatever OptiScaler's FSR settings say.");
    a->Disabled(!s.frameWeightOverride);
    float shown = s.frameWeightOverride || !fromFsr ? s.frameWeight : fsr;
    const char *fmt = s.frameWeightOverride ? "%.2f" : fromFsr ? "%.2f (OptiScaler's FSR)" : "%.2f (DittoSR's)";
    if (a->SliderFloat("Frame weight", &shown, 0.25f, 4.0f, fmt) && s.frameWeightOverride) {
        s.frameWeight = clampf(shown, 0.25f, 4.0f);
        a->SetFloat(g_plugin, "frame_weight", s.frameWeight), changed = true;
    }
    a->ItemTooltip("The weight of each new frame against the accumulated image (2 by default).\n"
                   "Lower: steadier and smoother, slightly blurrier with more trailing in motion.\n"
                   "Higher: crisper in motion, more shimmer and noise.");
    a->Disabled(false);

    int as = shimmer_index(s.antiShimmer);
    if (a->Combo("Anti-shimmer", &as, kShimmerNames, kShimmerCount, nullptr) && as >= 0 && as < kShimmerCount) {
        s.antiShimmer = kShimmer[as];
        a->SetFloat(g_plugin, "anti_shimmer", s.antiShimmer), changed = true;
    }
    a->ItemTooltip("Where the new frame keeps flipping around the accumulated image (the jittered samples'\n"
                   "aliasing), up to this share less of it: less shimmer, a little softer fine detail in motion.\n"
                   "Auto: 0.4 up to 2x, 0.2 from 3x.");

    if (a->SliderFloat("Motion detail", &s.historySharpen, 0.0f, 2.0f, "%.2f")) {
        s.historySharpen = clampf(s.historySharpen, 0.0f, 2.0f);
        a->SetFloat(g_plugin, "history_sharpen", s.historySharpen), changed = true;
    }
    a->ItemTooltip("The accumulated image's detail made up for its resampling blur in motion,\n"
                   "x its default strength (1). 0: off.");

    a->Section("Debug");
    int dv = s.debugView < 0 || s.debugView >= kDebugCount ? 0 : s.debugView;
    if (a->Combo("Debug view", &dv, kDebugNames, kDebugCount, kDebugDisabled) && dv >= 0 && dv < kDebugCount) {
        s.debugView = dv;
        a->SetInt(g_plugin, "debug_view", dv), changed = true;
    }
    a->ItemTooltip("Shows one of the upscale's inner values in place of the image:\n"
                   "the new frame's share, the stability level, or clip (red) / cut (green) / confidence (blue).");
    if (a->Checkbox("Indicator", &s.indicator)) a->SetBool(g_plugin, "indicator", s.indicator), changed = true;
    a->ItemTooltip("A small pink DittoSR face in the top-left corner while DittoSR upscales");

    a->Section("Settings");
    if (g_unsaved) a->TextColored(1.0f, 0.8f, 0.0f, 1.0f, "Not saved yet: Save here, or Save INI in OptiScaler's menu");
    else a->TextDisabled("Kept in OptiScaler.ini, over dittosr.ini's values");
    a->Spacing();
    if (a->Button("Save")) {
        bool ok = a->SaveSettings(g_plugin);
        if (ok) g_unsaved = false;
        a->Log(g_plugin, ok ? OptiLog_Info : OptiLog_Error, ok ? "Settings saved" : "Could not save the settings");
    }
    a->ItemTooltip("Writes OptiScaler.ini (as its Save INI does)");
    a->SameLine();
    if (a->Button("Defaults")) {
        for (const char *k : kKeys) a->ResetValue(g_plugin, k);
        ditto_request_reload();  // (dittosr.ini's values back from the next upscale, the defaults without it)
        g_unsaved = true;
    }
    a->ItemTooltip("Removes these settings from OptiScaler.ini: dittosr.ini's values, or the defaults without one.\n"
                   "Save to make it permanent.");

    if (changed) ditto_set_settings(s), g_unsaved = true;
}

}  // namespace

extern "C" __declspec(dllexport) bool OptiPluginInit(const OptiPluginApi *api, OptiPlugin *plugin) {
    if (!api || !plugin || api->version < 1) return false;
    g_api = api, g_plugin = plugin;
    {
        DittoSettings s = ditto_settings();
        apply_ini(s);
        ditto_set_settings(s);
    }
    g_settingsOverride = apply_ini;  // (F9's reload of dittosr.ini keeps them)
    bool ok = api->RegisterTab(plugin, "DittoSR", "DittoSR's settings: frame weight, anti-shimmer, motion detail, debug views",
                               draw_tab, nullptr);
    ditto_log("OptiScaler plugin API %u: %s\n", api->version, ok ? "the DittoSR tab registered" : "the tab could not be registered");
    return ok;
}
