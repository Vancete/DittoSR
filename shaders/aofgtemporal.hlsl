// Ditto Relight (experimental), FastGI's temporal resolve (giMode 1, temporal smoothing; its low resolution): the
// filtered GI blended with its history where the pixel was - x fgTemporal, less with the image's motion. FastGI
// reprojects with the previous frame's camera; DittoSR has none, so the game's motion vectors (objects too), and each
// history tap's depth against this one's (FastGI: against the point's depth in the previous view - 1 %; here 2 %,
// the camera's own motion left in it). The 4 taps rejected one by one before the bilinear, the result clamped to the
// current 5-tap neighbourhood's range.
// t0 = the prepass' normal and depth, t1 = the filtered GI, t2 = the history (the last frame's output), t3 = the game's
// motion vectors; u0 = this frame's (the next one's history, and the composition's GI).
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Gi : register(t1);
Texture2D<float4> Hist : register(t2);
Texture2D<float2> Mv : register(t3);
RWTexture2D<float4> Out : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    uint2 ls = fgLow();
    if (any(id >= ls)) return;
    float4 current = Gi.Load(int3(id, 0));
    if (current.a <= 0.0 || fgTemporal <= 0.0) { Out[id] = current; return; }
    int2 p = fgFull(int2(id)), m = int2(ls) - 1;
    float2 uv = (float2(p) + 0.5) * rsRcp;
    float2 mv = Mv.Load(int3(clamp(int2(uv * fgMvSize), int2(0, 0), int2(fgMvSize) - 1), 0));
    float2 puv = uv + mv * fgMvScale;
    if (any(puv <= 0.0) || any(puv >= 1.0)) { Out[id] = current; return; }
    float expected = current.a;
    float2 pixel = puv * float2(ls) - 0.5, base = floor(pixel), fr = pixel - base;
    float3 history = 0.0;
    float ws = 0.0;
    [unroll] for (int k = 0; k < 4; k++) {
        int2 o = int2(k & 1, k >> 1);
        float4 tap = Hist.Load(int3(clamp(int2(base) + o, int2(0, 0), m), 0));
        float2 bl = lerp(1.0 - fr, fr, float2(o));
        float w = bl.x * bl.y * step(0.00001, tap.a) * saturate(1.0 - abs(tap.a - expected) / max(0.025, expected * 0.02));
        history += tap.rgb * w;
        ws += w;
    }
    if (ws < 0.05) { Out[id] = current; return; }
    history /= ws;
    // (the current 5-tap neighbourhood's bounds: fewer trails from lighting and objects changing)
    float3 lo = current.rgb, hi = current.rgb;
    const int2 offs[4] = {int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
    [unroll] for (int i = 0; i < 4; i++) {
        float4 tap = Gi.Load(int3(clamp(int2(id) + offs[i], int2(0, 0), m), 0));
        if (tap.a > 0.0 && abs(tap.a - current.a) < max(0.025, current.a * 0.01)) { lo = min(lo, tap.rgb); hi = max(hi, tap.rgb); }
    }
    float3 margin = (hi - lo) * 0.15 + 0.001;
    history = clamp(history, max(0.0, lo - margin), hi + margin);
    // (less persistence in motion on screen; this frame's depth kept)
    float motion = length((puv - uv) * float2(ls));
    float w = fgTemporal * saturate(ws) / (1.0 + motion * 0.1);
    Out[id] = float4(lerp(current.rgb, history, w), current.a);
}
