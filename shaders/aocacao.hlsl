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

// a pixel's 4 edges (left, right, top, bottom) from its and its neighbours' normal and depth
float4 cacaoEdges(float4 nd, float4 ql, float4 qr, float4 qu, float4 qd) {
    float4 q[4] = {ql, qr, qu, qd};
    float4 z, nn;
    [unroll] for (int k = 0; k < 4; k++) {
        z[k] = q[k].w > 0.0 ? q[k].w : 1e9;  // (the sky: an edge)
        nn[k] = q[k].w > 0.0 ? saturate(dot(nd.xyz, q[k].xyz) + 0.5) : 0.0;
    }
    float4 e = z - nd.w;
    e = min(abs(e), abs(e + e.yxwz));
    e = saturate(1.3 - e / (nd.w * 0.040)) * nn;
    return saturate(round(saturate(e) * 3.05) / 3.0 + 0.02);  // (CACAO's 2-bit edges, + its inverse sharpness 1 - 0.98)
}
float4 cacaoBlur(float4 c, float4 e, float4 vl, float4 vr, float4 vu, float4 vd) {
    return (c * 0.5 + vl * e.x + vr * e.y + vu * e.z + vd * e.w) / (0.5 + e.x + e.y + e.z + e.w);
}

#ifndef CACAO_FUSED
[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rs)) return;
    int2 p = int2(id), m = int2(rs) - 1;
    float4 c = Res.Load(int3(p, 0));
    float4 nd = LOAD_ND(Nd, p);
    if (nd.w <= 0.0) { Out[id] = c; return; }
    int2 l = clamp(p + int2(-1, 0), 0, m), r = clamp(p + int2(1, 0), 0, m), u = clamp(p + int2(0, -1), 0, m), d = clamp(p + int2(0, 1), 0, m);
    float4 e = cacaoEdges(nd, LOAD_ND(Nd, l), LOAD_ND(Nd, r), LOAD_ND(Nd, u), LOAD_ND(Nd, d));
    Out[id] = cacaoBlur(c, e, Res.Load(int3(l, 0)), Res.Load(int3(r, 0)), Res.Load(int3(u, 0)), Res.Load(int3(d, 0)));
}
#else
// (the 2 passes: the group's 8 x 8 and a 2-pixel ring loaded once, the first pass over 10 x 10, the second over 8 x 8)
groupshared float4 sN[12][12], sV[12][12], s1[10][10];

[numthreads(8, 8, 1)]
void main(uint2 gid : SV_GroupID, uint2 lt : SV_GroupThreadID, uint li : SV_GroupIndex) {
    int2 m = int2(rs) - 1, o = int2(gid * 8u) - 2;
    for (uint k = li; k < 144u; k += 64u) {
        int2 t = int2(k % 12u, k / 12u), q = clamp(o + t, int2(0, 0), m);
        sN[t.y][t.x] = LOAD_ND(Nd, q);
        sV[t.y][t.x] = Res.Load(int3(q, 0));
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint k = li; k < 100u; k += 64u) {
        int2 t = int2(k % 10u, k / 10u) + 1;
        float4 nd = sN[t.y][t.x], c = sV[t.y][t.x];
        s1[t.y - 1][t.x - 1] = nd.w <= 0.0 ? c
            : cacaoBlur(c, cacaoEdges(nd, sN[t.y][t.x - 1], sN[t.y][t.x + 1], sN[t.y - 1][t.x], sN[t.y + 1][t.x]),
                        sV[t.y][t.x - 1], sV[t.y][t.x + 1], sV[t.y - 1][t.x], sV[t.y + 1][t.x]);
    }
    GroupMemoryBarrierWithGroupSync();
    uint2 id = gid * 8u + lt;
    if (any(id >= (uint2)rs)) return;
    int2 t = int2(lt) + 2, u = int2(lt) + 1;
    float4 nd = sN[t.y][t.x], c = s1[u.y][u.x];
    Out[id] = nd.w <= 0.0 ? c
        : cacaoBlur(c, cacaoEdges(nd, sN[t.y][t.x - 1], sN[t.y][t.x + 1], sN[t.y - 1][t.x], sN[t.y + 1][t.x]),
                    s1[u.y][u.x - 1], s1[u.y][u.x + 1], s1[u.y - 1][u.x], s1[u.y + 1][u.x]);
}
#endif
