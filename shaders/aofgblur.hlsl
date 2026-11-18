// Ditto Relight (experimental), FastGI's filter (giMode 1, its low resolution): one axis a pass (fgAxis - x, then y),
// the centre and 4 taps fgSpatial apart (weights 0.75, 0.375), each x fgGeo - its depth and normal near the centre's.
// t0 = the prepass' normal and depth (render size), t1 = the GI; u0 = the filtered.
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Gi : register(t1);
RWTexture2D<float4> Out : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    uint2 ls = fgLow();
    if (any(id >= ls)) return;
    float4 center = Gi.Load(int3(id, 0));
    if (center.a <= 0.0) { Out[id] = 0.0; return; }
    float3 normal = Nd.Load(int3(fgFull(int2(id)), 0)).xyz;
    int2 axis = fgAxis > 0.5 ? int2(0, 1) : int2(1, 0), m = int2(ls) - 1;
    float3 sum = center.rgb;
    float ws = 1.0;
    [unroll] for (int o = -2; o <= 2; o++) {
        if (o == 0) continue;
        // (the distance rounded before the sign: fractional radii symmetric)
        int off = int(sign(float(o)) * floor(abs(float(o)) * fgSpatial + 0.5));
        int2 q = clamp(int2(id) + axis * off, int2(0, 0), m);
        float4 tap = Gi.Load(int3(q, 0));
        float w = (abs(o) == 1 ? 0.75 : 0.375) * fgGeo(center.a, tap.a, normal, Nd.Load(int3(fgFull(q), 0)).xyz);
        sum += tap.rgb * w;
        ws += w;
    }
    Out[id] = float4(sum / ws, center.a);
}
