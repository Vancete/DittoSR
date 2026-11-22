// Ditto Relight (experimental), the spread to render size (D3D12; the resolution modes "half rotating" - flags2 4 - and
// "checkerboard" - flags2 1024). The main pass computed part of the pixels this frame - rotating: one of each 2 x 2, a
// different one each frame (4 frames: all of them; spread after its filter at half size); checkerboard: every other pixel,
// the other half the next frame (spread before the filter). Each render pixel: computed this frame - its own value; else
// its computed neighbours' (rotating: the 4 half texels around it; checkerboard: the 4 beside it), each x geoWeight (its
// depth and normal) x 1 / (1 + its distance^2), none close: the nearest in depth. The temporal smoothing after it (render
// size) then blends the frames' computed pixels: still, the image converges to every pixel computed.
// t0 = the render-size normal and depth (t7 packed), t1 = the main pass' result (rotating: half size; checkerboard: half
// width); u0 = render size.
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Src : register(t1);
AOFMT("rgba16f") RWTexture2D<float4> Out : register(u0);

bool ckFresh(int2 q) { return (uint(q.x) & 1u) == ((uint(q.y) + (uint)frame) & 1u); }

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rs)) return;
    int2 p = int2(id), m = int2(rs) - 1;
    float4 nd = LOAD_ND(Nd, p);
    float z = nd.w;
    if (z <= 0.0) { Out[id] = float4(0.0, 0.0, 0.0, 1.0); return; }
    if (flag2On(1024u)) {
        if (ckFresh(p)) { Out[id] = Src.Load(int3(p.x >> 1, p.y, 0)); return; }
    } else {
        uint2 off = rotOff(frame);
        if (all((uint2(p) & 1u) == off)) { Out[id] = Src.Load(int3(p >> 1, 0)); return; }
    }
    float4 acc = 0.0, best = 0.0;
    float ws = 0.0, bd = 1e30;
    [unroll] for (int k = 0; k < 4; k++) {
        int2 q, t;  // (the computed pixel, its texel)
        if (flag2On(1024u)) {
            const int2 sd[4] = {int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
            q = clamp(p + sd[k], int2(0, 0), m);
            if (!ckFresh(q)) continue;  // (at the image's edge, clamped onto itself)
            t = int2(q.x >> 1, q.y);
        } else {
            uint2 off = rotOff(frame);
            int2 hm = int2(((uint2)rs + 1u) >> 1) - 1, hb = int2(floor((float2(p) - float2(off)) * 0.5));
            t = clamp(hb + int2(k & 1, k >> 1), int2(0, 0), hm);
            q = min(t * 2 + int2(off), m);
        }
        float4 qn = LOAD_ND(Nd, q), v = Src.Load(int3(t, 0));
        float2 dd = float2(q - p);
        float w = qn.w > 0.0 ? geoWeight(z, qn.w, nd.xyz, qn.xyz) / (1.0 + dot(dd, dd)) : 0.0;
        acc += v * w, ws += w;
        float e = abs(qn.w - z);
        if (qn.w > 0.0 && e < bd) bd = e, best = v;
    }
    Out[id] = ws > 1e-4 ? acc / ws : best;
}
