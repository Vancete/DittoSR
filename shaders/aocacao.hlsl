// Ditto Relight (experimental), FidelityFX CACAO's edge-sensitive blur (MIT; ffx_cacao.hlsl: FFX_CACAO_CalculateEdges,
// FFX_CACAO_CalcBlurredSampleF16_4) after the temporal smoothing (ao_cacao on), on the accumulated occlusion and bounce
// light - the history left as it is -, at the main pass' size (half or render size), one of its passes a dispatch (2 by
// default, ping-ponged):
//   - each pixel's 4 edges (left, right, top, bottom) from the view depths - slope-aware: a neighbour's step against the
//     centre, or against the opposite neighbour's (a plane's slope is no edge), x 1 / (0.04 x the centre's depth),
//     1.3 - that, clamped - x the normals' (saturate(n . n' + 0.5)), quantised to 4 levels as CACAO packs them, then +
//     its inverse sharpness (1 - 0.98);
//   - the blur: (0.5 x the centre + the 4 neighbours x their edges) / (0.5 + the edges) - a cross one pixel wide, each
//     pass reaching one pixel further.
// t0 = the prepass' normal and depth, t1 = the occlusion and bounce light; u0 = blurred.
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Res : register(t1);
AOFMT("rgba16f") RWTexture2D<float4> Out : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rs)) return;
    int2 p = int2(id), m = int2(rs) - 1;
    float4 c = Res.Load(int3(p, 0));
    float4 nd = Nd.Load(int3(p, 0));
    if (nd.w <= 0.0) { Out[id] = c; return; }
    const int2 offs[4] = {int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};  // (left, right, top, bottom)
    float4 z, nn;
    float4 v[4];
    [unroll] for (int k = 0; k < 4; k++) {
        int2 q = clamp(p + offs[k], int2(0, 0), m);
        float4 qn = Nd.Load(int3(q, 0));
        z[k] = qn.w > 0.0 ? qn.w : 1e9;  // (the sky: an edge)
        nn[k] = qn.w > 0.0 ? saturate(dot(nd.xyz, qn.xyz) + 0.5) : 0.0;
        v[k] = Res.Load(int3(q, 0));
    }
    float4 e = z - nd.w;
    e = min(abs(e), abs(e + e.yxwz));
    e = saturate(1.3 - e / (nd.w * 0.040)) * nn;
    e = saturate(round(saturate(e) * 3.05) / 3.0 + 0.02);  // (CACAO's 2-bit edges, + its inverse sharpness 1 - 0.98)
    float4 sum = c * 0.5;
    float ws = 0.5;
    [unroll] for (int k = 0; k < 4; k++) sum += v[k] * e[k], ws += e[k];
    Out[id] = sum / ws;
}
