// Ditto Relight (experimental), the spatial filter (render size; after FastGI's): the main pass' occlusion and bounce
// light along one axis a pass (axis: x, then y) - the centre and 4 taps spatial x 2 pixels apart (weights 0.75, 0.375),
// each x geoWeight: its depth and normal near the centre's. Before the temporal smoothing (aotemporal.hlsl).
// t0 = the prepass' normal and depth, t1 = the main pass' (GI rgb, occlusion a); u0 = the filtered.
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Res : register(t1);
RWTexture2D<float4> Out : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rs)) return;
    float4 center = Res.Load(int3(id, 0));
    float4 nd = Nd.Load(int3(id, 0));
    if (nd.w <= 0.0) { Out[id] = center; return; }
    int2 ax = axis > 0.5 ? int2(0, 1) : int2(1, 0), m = int2(rs) - 1;
    float4 sum = center;
    float ws = 1.0;
    [unroll] for (int o = -2; o <= 2; o++) {
        if (o == 0) continue;
        // (the distance rounded before the sign: fractional radii symmetric)
        int off = int(sign(float(o)) * floor(abs(float(o)) * spatial * 2.0 + 0.5));
        int2 q = clamp(int2(id) + ax * off, int2(0, 0), m);
        float4 qn = Nd.Load(int3(q, 0));
        float w = (abs(o) == 1 ? 0.75 : 0.375) * geoWeight(nd.w, qn.w, nd.xyz, qn.xyz);
        sum += Res.Load(int3(q, 0)) * w;
        ws += w;
    }
    Out[id] = sum / ws;
}
