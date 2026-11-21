// Ditto Relight (experimental), the screen probes' spatial filter (ao_gi_probes; D3D12): each probe's GI with its 3 x 3
// probes' - 1, 0.5, 0.25 by their place, each x how near its depth is to this one's (within 5 %) - before the interpolation.
// t0 = the probes (accumulated: rgb the GI, a the depth); u0 = filtered.
#include "ao_common.hlsli"

Texture2D<float4> Src : register(t0);
AOFMT("rgba16f") RWTexture2D<float4> Out : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    uint2 pd = probeDims();
    if (any(id >= pd)) return;
    float4 c = Src.Load(int3(id, 0));
    if (c.a <= 0.0) { Out[id] = c; return; }
    int2 pm = int2(pd) - 1;
    float3 acc = c.rgb;
    float ws = 1.0;
    [unroll] for (int j = 0; j < 9; j++) {
        if (j == 4) continue;
        int2 o = int2(j % 3 - 1, j / 3 - 1), q = clamp(int2(id) + o, int2(0, 0), pm);
        float4 n = Src.Load(int3(q, 0));
        float w = (o.x != 0 && o.y != 0 ? 0.25 : 0.5) * step(0.00001, n.a) * saturate(1.0 - abs(n.a - c.a) / (c.a * 0.05 + 0.02));
        acc += n.rgb * w, ws += w;
    }
    Out[id] = float4(acc / ws, c.a);
}
