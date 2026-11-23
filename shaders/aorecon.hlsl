// Ditto Relight (experimental), the checkerboard's spread to render size (D3D12; flags2 1024): the main pass computed every
// other pixel of each row this frame, the other half the next. Each render pixel: computed this frame - its own value;
// else its 4 neighbours beside it (computed), each x geoWeight (its depth and normal) x 1 / (1 + its distance^2), none
// close: the nearest in depth. The filter and the temporal smoothing after it, at render size.
// t0 = the render-size normal and depth (t7 packed), t1 = the main pass' result (half width); u0 = render size.
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
    if (ckFresh(p)) { Out[id] = Src.Load(int3(p.x >> 1, p.y, 0)); return; }
    float4 acc = 0.0, best = 0.0;
    float ws = 0.0, bd = 1e30;
    [unroll] for (int k = 0; k < 4; k++) {
        const int2 sd[4] = {int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
        int2 q = clamp(p + sd[k], int2(0, 0), m);  // (the computed pixel; at the image's edge, clamped onto itself)
        if (!ckFresh(q)) continue;
        int2 t = int2(q.x >> 1, q.y);
        float4 qn = LOAD_ND(Nd, q), v = Src.Load(int3(t, 0));
        float2 dd = float2(q - p);
        float w = qn.w > 0.0 ? geoWeight(z, qn.w, nd.xyz, qn.xyz) / (1.0 + dot(dd, dd)) : 0.0;
        acc += v * w, ws += w;
        float e = abs(qn.w - z);
        if (qn.w > 0.0 && e < bd) bd = e, best = v;
    }
    Out[id] = ws > 1e-4 ? acc / ws : best;
}
