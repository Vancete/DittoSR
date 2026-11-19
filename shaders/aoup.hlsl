// Ditto Relight (experimental), the half-resolution main pass back to render size (halfRes on): each pixel from the 4
// half-size texels around it - each the pixel halfOff computed this frame in its 2 x 2 - x geoWeight (its depth and
// normal near this pixel's: no light across an edge) x 1 / (1 + its distance^2); none close: the nearest in depth.
// The pixels not computed this frame thus take their neighbours'; the temporal smoothing and DittoSR's accumulation
// bring in the frames' other pixels.
// t0 = the prepass' normal and depth, t1 = the half-size main pass; u0 = render size.
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Half : register(t1);
AOFMT("rgba16f") RWTexture2D<float4> Out : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rs)) return;
    float4 nd = Nd.Load(int3(id, 0));
    if (nd.w <= 0.0) { Out[id] = float4(0.0, 0.0, 0.0, 1.0); return; }
    uint hk = (uint)halfOff;
    int2 off = int2(hk & 1u, hk >> 1u), hm = int2(((uint2)rs + 1u) >> 1) - 1, m = int2(rs) - 1;
    int2 hb = int2(floor((float2(id) - float2(off)) * 0.5));
    float4 acc = 0.0, best = 0.0;
    float ws = 0.0, bd = 1e30;
    [unroll] for (int k = 0; k < 4; k++) {
        int2 h = clamp(hb + int2(k & 1, k >> 1), int2(0, 0), hm);
        int2 s = min(h * 2 + off, m);
        float4 sn = Nd.Load(int3(s, 0));
        float4 v = Half.Load(int3(h, 0));
        float2 dd = float2(s - int2(id));
        float w = geoWeight(nd.w, sn.w, nd.xyz, sn.xyz) / (1.0 + dot(dd, dd));
        acc += v * w;
        ws += w;
        float e = abs(sn.w - nd.w);
        if (sn.w > 0.0 && e < bd) bd = e, best = v;
    }
    Out[id] = ws > 1e-4 ? acc / ws : best;
}
