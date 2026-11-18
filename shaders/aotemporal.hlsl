// Ditto Relight (experimental), the temporal smoothing (render size; after FastGI's temporal resolve, on
// the filtered occlusion and bounce light (aofilter.hlsl)): blended with their history where the pixel was - x temporal, less
// with the image's motion. The history reprojected by the game's motion vectors; its 4 taps each rejected by its depth
// against this one's (2 %) before the bilinear, the result clamped to the current 5-tap neighbourhood's range - the
// main pass' noise (per pixel and per frame) evened out over the frames, DittoSR's own accumulation taking the rest.
// t0 = the prepass' normal and depth, t1 = the filtered (GI rgb, occlusion a), t2 = the history, t3 = its depth,
// t4 = the game's motion vectors; u0 = this frame's (the next one's history, and the composition's), u1 = its depth.
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Res : register(t1);
Texture2D<float4> Hist : register(t2);
Texture2D<float> HistDepth : register(t3);
Texture2D<float2> Mv : register(t4);
RWTexture2D<float4> Out : register(u0);
RWTexture2D<float> OutDepth : register(u1);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rs)) return;
    float4 current = Res.Load(int3(id, 0));
    float expected = Nd.Load(int3(id, 0)).w;
    OutDepth[id] = expected;
    if (expected <= 0.0 || temporal <= 0.0) { Out[id] = current; return; }
    int2 m = int2(rs) - 1;
    float2 uv = (float2(id) + 0.5) * rsRcp;
    float2 mv = Mv.Load(int3(clamp(int2(uv * mvSize), int2(0, 0), int2(mvSize) - 1), 0));
    float2 puv = uv + mv * mvScale;
    if (any(puv <= 0.0) || any(puv >= 1.0)) { Out[id] = current; return; }
    float2 pixel = puv * rs - 0.5, base = floor(pixel), fr = pixel - base;
    float4 history = 0.0;
    float ws = 0.0;
    [unroll] for (int k = 0; k < 4; k++) {
        int2 o = int2(k & 1, k >> 1), q = clamp(int2(base) + o, int2(0, 0), m);
        float hd = HistDepth.Load(int3(q, 0));
        float2 bl = lerp(1.0 - fr, fr, float2(o));
        float w = bl.x * bl.y * step(0.00001, hd) * saturate(1.0 - abs(hd - expected) / max(0.025, expected * 0.02));
        history += Hist.Load(int3(q, 0)) * w;
        ws += w;
    }
    if (ws < 0.05) { Out[id] = current; return; }
    history /= ws;
    // (the current 5-tap neighbourhood's bounds, on this surface: fewer trails from lighting and objects changing)
    float4 lo = current, hi = current;
    const int2 offs[4] = {int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
    [unroll] for (int i = 0; i < 4; i++) {
        int2 q = clamp(int2(id) + offs[i], int2(0, 0), m);
        float d = Nd.Load(int3(q, 0)).w;
        if (d > 0.0 && abs(d - expected) < max(0.025, expected * 0.01)) {
            float4 t = Res.Load(int3(q, 0));
            lo = min(lo, t), hi = max(hi, t);
        }
    }
    float4 margin = (hi - lo) * 0.15 + 0.001;
    history = clamp(history, max(0.0, lo - margin), hi + margin);
    // (less persistence in motion on screen)
    float motion = length((puv - uv) * rs);
    float w = temporal * saturate(ws) / (1.0 + motion * 0.1);
    Out[id] = lerp(current, history, w);
}
