// Ditto Relight (experimental), the GI's a-trous filter (flags2 128, ao_gi_atrous; D3D12): after the temporal smoothing,
// on the GI alone (the AO as it is) - a 3 x 3 at a spacing of axis pixels (1, then 2), each tap x 0.25 / 0.5 / 1 by its
// place x geoWeight (its depth and normal) x its luma's closeness to this pixel's (edge-stopping: e^(-|dl| / (0.25 x l +
// 0.02)) - the GI's own edges kept).
// t0 = the normal and depth at the main pass' size (t7 packed), t1 = the GI and AO; u0 = filtered.
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Res : register(t1);
AOFMT("rgba16f") RWTexture2D<float4> Out : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rs)) return;
    float4 c = Res.Load(int3(id, 0));
    float4 nd = LOAD_ND(Nd, id);
    if (nd.w <= 0.0) { Out[id] = c; return; }
    const float3 Y = float3(0.2126, 0.7152, 0.0722);
    float lc = dot(c.rgb, Y), sg = 0.25 * lc + 0.02;
    int st = max(int(axis + 0.5), 1);
    int2 m = int2(rs) - 1;
    float3 acc = 0.0;
    float ws = 0.0;
    [unroll] for (int j = 0; j < 9; j++) {
        int2 o = int2(j % 3 - 1, j / 3 - 1), q = clamp(int2(id) + o * st, int2(0, 0), m);
        float4 qn = LOAD_ND(Nd, q);
        float4 v = Res.Load(int3(q, 0));
        float w = (o.x == 0 ? 1.0 : 0.5) * (o.y == 0 ? 1.0 : 0.5) * (j == 4 ? 1.0 : geoWeight(nd.w, qn.w, nd.xyz, qn.xyz)) *
                  exp(-abs(dot(v.rgb, Y) - lc) / sg);
        acc += v.rgb * w, ws += w;
    }
    Out[id] = float4(acc / max(ws, 1e-6), c.a);
}
