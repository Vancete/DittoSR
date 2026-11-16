// DittoSR's tab in OptiScaler's menu, through its plugin API (optiscaler/optiscaler_plugin.h). OptiScaler calls
// OptiPluginInit when it loads this DLL as a custom upscaler (custom\<name>\ffx or ffx_vk) - once per module, and it
// never unloads one. The tab holds what OptiScaler's own settings don't reach - the frame weight, the anti-shimmer, the
// motion detail, the debug views, the indicator - kept in OptiScaler.ini's [Plugin.<name>] under dittosr.ini's key names
// (and frame_weight_override, the tab's own), over dittosr.ini's values (a key missing there: dittosr.ini's value). They
// apply from the next upscale; OptiScaler's Save INI, the tab's Save, or the F7 menu's Save writes them.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dittosr.h"
#include "optiscaler/optiscaler_plugin.h"

namespace {

const OptiPluginApi *g_api;
OptiPlugin *g_plugin;
bool g_unsaved;  // (the tab's values changed since OptiScaler.ini was last written)

// the keys in [Plugin.<name>]: dittosr.ini's names, and the override the tab adds
const char kFrameWeight[] = "frame_weight", kFrameWeightOverride[] = "frame_weight_override", kAntiShimmer[] = "anti_shimmer",
           kHistorySharpen[] = "history_sharpen", kDebugView[] = "debug_view", kIndicator[] = "indicator";
const char *const kKeys[] = {kFrameWeight, kFrameWeightOverride, kAntiShimmer, kHistorySharpen, kDebugView, kIndicator};

// the anti-shimmer's steps, as the F7 menu's (-1: by the ratio)
const float kShimmer[] = {-1.0f, 0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f};
const char *const kShimmerNames[] = {"Auto", "Off", "0.1", "0.2", "0.3", "0.4", "0.5", "0.6", "0.7", "0.8", "0.9"};
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
    s.frameWeight = clampf(g_api->GetFloat(g_plugin, kFrameWeight, s.frameWeight), 0.25f, 4.0f);
    s.frameWeightOverride = g_api->GetBool(g_plugin, kFrameWeightOverride, s.frameWeightOverride);
    s.antiShimmer = g_api->GetFloat(g_plugin, kAntiShimmer, s.antiShimmer);
    s.historySharpen = clampf(g_api->GetFloat(g_plugin, kHistorySharpen, s.historySharpen), 0.0f, 2.0f);
    int dv = g_api->GetInt(g_plugin, kDebugView, s.debugView);
    s.debugView = dv < 0 || dv >= kDebugCount ? 0 : dv;
    s.indicator = g_api->GetBool(g_plugin, kIndicator, s.indicator);
}

// The F7 menu's Save (common.cpp ditto_save_settings), key by key: a key [Plugin.<name>] holds takes s's value there -
// true, dittosr.ini's left alone; one it doesn't hold, false (dittosr.ini's). key null: what was taken, written out.
bool save_held(const DittoSettings &s, const char *key) {
    static bool taken;
    if (!key) {
        if (!taken) return true;
        bool ok = g_api->SaveSettings(g_plugin);
        if (ok) taken = false, g_unsaved = false;
        return ok;
    }
    char v[32];
    if (!g_api->GetString(g_plugin, key, v, sizeof v, nullptr)) return false;  // (missing or "auto")
    if (!strcmp(key, kFrameWeight)) g_api->SetFloat(g_plugin, key, s.frameWeight);
    else if (!strcmp(key, kAntiShimmer)) g_api->SetFloat(g_plugin, key, s.antiShimmer);
    else if (!strcmp(key, kHistorySharpen)) g_api->SetFloat(g_plugin, key, s.historySharpen);
    else if (!strcmp(key, kDebugView)) g_api->SetInt(g_plugin, key, s.debugView);
    else if (!strcmp(key, kIndicator)) g_api->SetBool(g_plugin, key, s.indicator);
    else return false;
    taken = true;
    return true;
}

void draw_tab(void *) {
    const OptiPluginApi *a = g_api;
    DittoSettings s = ditto_settings();
    bool changed = false;

    a->Section("Image");
    // (the slider shows the weight in use: FSR 3.1's settings' when they set one and the override is off, else
    // DittoSR's own - editable then, whether overridden or not: nothing else sets it)
    float fsr = g_stats.fsrFrameWeight;
    bool fromFsr = fsr >= 0.0f;
    if (a->Checkbox("Override##frameWeight", &s.frameWeightOverride)) {
        if (s.frameWeightOverride && fromFsr) {  // (the slider starts from the weight in use: no jump)
            s.frameWeight = fsr;
            a->SetFloat(g_plugin, kFrameWeight, s.frameWeight);
        }
        a->SetBool(g_plugin, kFrameWeightOverride, s.frameWeightOverride), changed = true;
    }
    a->ItemTooltip("Off: OptiScaler's FSR preset and Velocity Factor / Reactive Scale set the frame weight\n"
                   "(Stability 1, Motion 3); at Default, the slider below does.\n"
                   "On: the slider below, whatever OptiScaler's FSR settings say.");
    bool editable = s.frameWeightOverride || !fromFsr;
    a->Disabled(!editable);
    float shown = editable ? s.frameWeight : fsr;
    const char *fmt = s.frameWeightOverride ? "%.2f" : fromFsr ? "%.2f (OptiScaler's FSR)" : "%.2f (DittoSR's)";
    if (a->SliderFloat("Frame weight", &shown, 0.25f, 4.0f, fmt) && editable) {
        s.frameWeight = clampf(shown, 0.25f, 4.0f);
        a->SetFloat(g_plugin, kFrameWeight, s.frameWeight), changed = true;
    }
    a->ItemTooltip("The weight of each new frame against the accumulated image (2 by default).\n"
                   "Lower: steadier and smoother, slightly blurrier with more trailing in motion.\n"
                   "Higher: crisper in motion, more shimmer and noise.");
    a->Disabled(false);

    int as = shimmer_index(s.antiShimmer);
    if (a->Combo("Anti-shimmer", &as, kShimmerNames, kShimmerCount, nullptr) && as >= 0 && as < kShimmerCount) {
        s.antiShimmer = kShimmer[as];
        a->SetFloat(g_plugin, kAntiShimmer, s.antiShimmer), changed = true;
    }
    a->ItemTooltip("Where the new frame keeps flipping around the accumulated image (the jittered samples'\n"
                   "aliasing), up to this share less of it: less shimmer, a little softer fine detail in motion.\n"
                   "Auto: 0.8 up to 2x, 0.4 from 3x.");

    if (a->SliderFloat("Motion detail", &s.historySharpen, 0.0f, 2.0f, "%.2f")) {
        s.historySharpen = clampf(s.historySharpen, 0.0f, 2.0f);
        a->SetFloat(g_plugin, kHistorySharpen, s.historySharpen), changed = true;
    }
    a->ItemTooltip("The accumulated image's detail made up for its resampling blur in motion,\n"
                   "x its default strength (1). 0: off.");

    a->Section("Blend");
    // (the F7 menu's three: not kept in OptiScaler.ini - its Save writes them to dittosr.ini)
    a->Disabled(s.blend042);
    float ar = s.blend042 ? 0.035f : s.alphaRest, sf = s.blend042 ? 0.05f : s.shareFar, gr = s.blend042 ? 1.5f : s.gammaRest;
    if (a->SliderFloat("Rest blend", &ar, 0.01f, 0.2f, "%.3f") && !s.blend042) s.alphaRest = clampf(ar, 0.01f, 0.2f), changed = true;
    a->ItemTooltip("Each new frame's share where nothing moves (0.1). Higher: what moves with no motion\n"
                   "vectors follows sooner, the still image shimmers more.");
    if (a->SliderFloat("Blend with no sample", &sf, 0.0f, 1.0f, "%.2f") && !s.blend042) s.shareFar = clampf(sf, 0.0f, 1.0f), changed = true;
    a->ItemTooltip("x that share in the pixels none of the frame's samples landed near (0.4).\n"
                   "Higher: less detail kept from the earlier frames, more shimmer in a pan.");
    if (a->SliderFloat("Rest clip", &gr, 0.5f, 3.0f, "%.2f") && !s.blend042) s.gammaRest = clampf(gr, 0.5f, 3.0f), changed = true;
    a->ItemTooltip("How far from the new frame's samples the accumulated image may stay at rest (3).\n"
                   "Lower: less ghosting, more shimmer.");
    a->Disabled(false);
    if (a->Checkbox("0.4.2 blend (compare)", &s.blend042)) changed = true;  // (not kept: a comparison)
    a->ItemTooltip("0.4.2's blend in place of the new one, whatever the three sliders above say,\n"
                   "to compare them live. Not saved.");

    a->Section("Debug");
    int dv = s.debugView < 0 || s.debugView >= kDebugCount ? 0 : s.debugView;
    if (a->Combo("Debug view", &dv, kDebugNames, kDebugCount, kDebugDisabled) && dv >= 0 && dv < kDebugCount) {
        s.debugView = dv;
        a->SetInt(g_plugin, kDebugView, dv), changed = true;
    }
    a->ItemTooltip("Shows one of the upscale's inner values in place of the image:\n"
                   "the new frame's share, the stability level, or clip (red) / cut (green) / confidence (blue).");
    if (a->Checkbox("Indicator", &s.indicator)) a->SetBool(g_plugin, kIndicator, s.indicator), changed = true;
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
    g_settingsSave = save_held;      // (the F7 menu's Save writes them here, not into dittosr.ini)
    bool ok = api->RegisterTab(plugin, "DittoSR", "DittoSR's settings: frame weight, anti-shimmer, motion detail, debug views",
                               draw_tab, nullptr);
    ditto_log("OptiScaler plugin API %u: %s\n", api->version, ok ? "the DittoSR tab registered" : "the tab could not be registered");
    return ok;
}
