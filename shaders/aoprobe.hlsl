// Ditto Relight (experimental), screen probes (flags 4194304, ao_gi_probes; D3D12): the GI at every pixel of the main pass'
// size from the 4 probes around it (one in each 4 x 4 - its pixel moved a frame, probeJitter -, computed with twice the
// samples by the main pass' own dispatch) - bilinear by where they are, each x
// geoWeight (its depth and normal near this pixel's); none close: the nearest in depth. The AO (a) as the main pass left it.
// t0 = the normal and depth at the main pass' size (t7 packed), t1 = the probes; u0 = the main pass' result (rgb replaced).
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Probes : register(t1);
AOFMT("rgba16f") RWTexture2D<float4> Res : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rs)) return;
    float4 nd = LOAD_ND(Nd, id);
    float4 r = Res[id];
    if (nd.w <= 0.0) return;
    int2 pm = int2(((uint2)rs + 3u) >> 2) - 1, m = int2(rs) - 1;
    int2 j = int2(probeJitter());
    float2 f = (float2(id) - float2(j)) / 4.0;
    int2 b = int2(floor(f));
    float2 fr = f - float2(b);
    float3 acc = 0.0, best = 0.0;
    float ws = 0.0, bd = 1e30;
    [unroll] for (int k = 0; k < 4; k++) {
        int2 o = int2(k & 1, k >> 1), pq = clamp(b + o, int2(0, 0), pm), s = min(pq * 4 + j, m);
        float4 sn = LOAD_ND(Nd, s);
        float2 bl = lerp(1.0 - fr, fr, float2(o));
        float w = bl.x * bl.y * geoWeight(nd.w, sn.w, nd.xyz, sn.xyz) + 1e-6 * bl.x * bl.y;
        float3 v = Probes.Load(int3(pq, 0)).rgb;
        acc += v * w, ws += w;
        float e = abs(sn.w - nd.w);
        if (sn.w > 0.0 && e < bd) bd = e, best = v;
    }
    r.rgb = ws > 1e-4 ? acc / ws : best;
    Res[id] = r;
}
