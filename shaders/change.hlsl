// DittoSR's fast settling: mode 0 = a 4 x 4 image, each pixel 16 points of a 16 x 16 grid over the
// render image - the share of them whose colour this frame differs from the history where the point was by over 0.1
// in luma; mode 1 = that image averaged into one pixel, the one texel the upscale reads. A scene change, a menu
// closing, a fade: most of the image at once - the upscale's reaction then goes further.
// t0 = the history (mode 1: the 4 x 4 image), t1 = the convert pass's (motion), t2 = the colour.
#include "common.hlsli"

Texture2D<float4> Hist : register(t0);
Texture2D<float4> Mda : register(t1);
Texture2D<float4> Color : register(t2);
VKFMT("r32f") RWTexture2D<float> Out : register(u0);

[numthreads(4, 4, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (mode > 0.5) {
        if (any(id != 0)) return;
        float s = 0.0;
        for (int i = 0; i < 16; i++) s += Hist.Load(int3(i % 4, i / 4, 0)).r;
        Out[id] = s / 16.0;
        return;
    }
    if (any(id >= 4)) return;
    float e = encExposure();
    int2 p = int2(id), r = int2(rs);
    float n = 0.0, c = 0.0;
    for (int i = 0; i < 16; i++) {
        float2 g = (float2(p * 4 + int2(i % 4, i / 4)) + 0.5) / 16.0;
        int2 q = int2(g * float2(r));
        float2 puv = (float2(q) + 0.5) * rsRcp - 0.5 * Mda.Load(int3(q, 0)).xy;
        if (any(puv < 0.0) || any(puv > 1.0)) continue;
        float3 cur = enc(Color.Load(int3(q, 0)).rgb, e);
        float d = abs(dot(cur - Hist.SampleLevel(linearClamp, puv, 0.0).rgb, float3(0.25, 0.5, 0.25)));
        c += step(0.1, d); n += 1.0;
    }
    Out[id] = n > 0.0 ? c / n : 0.0;
}
