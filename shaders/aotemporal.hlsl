// Ditto Relight (experimental), the temporal smoothing (render size; after FastGI's temporal resolve, on
// the filtered occlusion and bounce light (aofilter.hlsl)): blended with their history where the pixel was - x temporal, less
// with the image's motion. The history reprojected by the game's motion vectors; its 4 taps each rejected by its depth
// against this one's (2 %) before the bilinear, the result clamped to the current 5-tap neighbourhood's range - the
// main pass' noise (per pixel and per frame) evened out over the frames, DittoSR's own accumulation taking the rest.
// t0 = the prepass' normal and depth, t1 = the filtered (GI rgb, occlusion a), t2 = the history, t3 = its depth,
// t4 = the game's motion vectors; u0 = this frame's (the next one's history, and the composition's), u1 = its depth
// (its history's length in the low 4 bits: hdPack).
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Res : register(t1);
Texture2D<float4> Hist : register(t2);
Texture2D<float> HistDepth : register(t3);
Texture2D<float2> Mv : register(t4);
AOFMT("rgba16f") RWTexture2D<float4> Out : register(u0);
AOFMT("r32f") RWTexture2D<float> OutDepth : register(u1);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rs)) return;
    float4 current = Res.Load(int3(id, 0));
    float expected = LOAD_ND(Nd, id).w;
    OutDepth[id] = hdPack(expected, 0u);
    if (expected <= 0.0 || temporal <= 0.0) { Out[id] = current; return; }
    int2 m = int2(rs) - 1;
    float2 uv = (float2(id) + 0.5) * rsRcp;
    float2 mv = Mv.Load(int3(clamp(int2(uv * mvSize), int2(0, 0), int2(mvSize) - 1), 0));
    float2 puv = uv + mv * mvScale;
    if (any(puv <= 0.0) || any(puv >= 1.0)) { Out[id] = current; return; }
    float2 pixel = puv * rs - 0.5, base = floor(pixel), fr = pixel - base;
    float4 history = 0.0;
    float ws = 0.0, bw = -1.0;
    uint cnt = 0u;
    [unroll] for (int k = 0; k < 4; k++) {
        int2 o = int2(k & 1, k >> 1), q = clamp(int2(base) + o, int2(0, 0), m);
        float hd = HistDepth.Load(int3(q, 0));
        float2 bl = lerp(1.0 - fr, fr, float2(o));
        float w = bl.x * bl.y * step(0.00001, hd) * saturate(1.0 - abs(hd - expected) / max(0.025, expected * 0.02));
        history += Hist.Load(int3(q, 0)) * w;
        if (w > bw) bw = w, cnt = hdCount(hd);  // (the history's length: its heaviest tap's)
        ws += w;
    }
    if (ws < 0.05) { Out[id] = current; return; }
    history /= ws;
    // (the current 5-tap neighbourhood's bounds, on this surface: fewer trails from lighting and objects changing)
    float4 lo = 1e30, hi = -1e30;
    const int2 offs[4] = {int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
    [unroll] for (int i = 0; i < 4; i++) {
        int2 q = clamp(int2(id) + offs[i], int2(0, 0), m);
        float d = LOAD_ND(Nd, q).w;
        if (d > 0.0 && abs(d - expected) < max(0.025, expected * 0.01)) {
            float4 t = Res.Load(int3(q, 0));
            lo = min(lo, t), hi = max(hi, t);
        }
    }
    lo = min(lo, current), hi = max(hi, current);
    float4 margin = (hi - lo) * 0.15 + 0.001;
    history = clamp(history, max(0.0, lo - margin), hi + margin);
    // (less persistence in motion on screen)
    float motion = length((puv - uv) * rs);
    // (the weight by the history's length, n / (n + 1): fast where it starts over, long where it has held - the AO's up to
    // 15 frames (0.94), the GI's up to 31 (0.97))
    uint na = min(cnt, 15u);
    float tw = min(0.95, float(na) / float(na + 1u)), tg = min(0.97, float(cnt) / float(cnt + 1u));
    float sm = saturate(ws) / (1.0 + motion * 0.1);
    OutDepth[id] = hdPack(expected, cnt + 1u);
    Out[id] = float4(lerp(current.rgb, history.rgb, tg * sm), lerp(current.a, history.a, tw * sm));
}
