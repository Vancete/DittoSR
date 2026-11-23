// Ditto Relight (experimental), the spatial filter (the main pass' size), before the temporal smoothing (aotemporal.hlsl):
// CORTAO's filter pass, its large filter - the centre and 6 symmetric pairs (3, 0), (6, 0), (3, 3), (0, 3), (0, 6),
// (3, -3) pixels (x spatial), its "cheap, sparse blur" that prefilters its output for the temporal filters after it.
// Each tap's depth against the centre's plane at the tap (a slanted floor no longer rejected for its slope) and its
// normal's, exp2(-error); each pair x the lower of its two weights (contact shadows kept). Flags 16384: the GI also
// from 3 pairs 10 pixels out.
// t0 = the prepass' normal and depth, t1 = the main pass' (GI rgb, occlusion a); u0 = the filtered.
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Res : register(t1);
AOFMT("rgba16f") RWTexture2D<float4> Out : register(u0);

// a tap's weight against the centre (c: its view position, nd its normal and depth): CORTAO's
float tapWeight(float3 c, float4 nd, int2 q, float4 qn) {
    if (qn.w <= 0.0) return 0.0;
    return planeWeight(c, nd.xyz, (float2(q) + 0.5) * rsRcp, qn.w, qn.xyz);
}

// (checkerboard fused, flags2 1024 + 8192: Res the half-width checkerboard - each tap moved to the computed pixel beside
// it (x flipped), the centre where not computed its 2 computed neighbours' mean by depth and normal: no spread pass)
bool ckFused() { return flag2On(1024u) && flag2On(8192u); }
bool ckFresh(int2 q) { return (uint(q.x) & 1u) == ((uint(q.y) + (uint)frame) & 1u); }
int2 ckTap(int2 q) {
    if (!ckFused() || ckFresh(q)) return q;
    int x = q.x ^ 1;
    return int2(x > int(rs.x) - 1 ? q.x - 1 : x, q.y);
}
float4 resAt(int2 q) { return Res.Load(int3(ckFused() ? int2(q.x >> 1, q.y) : q, 0)); }

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rs)) return;
    float4 nd = LOAD_ND(Nd, id);
    float4 center;
    if (ckFused() && !ckFresh(int2(id))) {
        float4 ca = 0.0, cb = 0.0;
        float wsum = 0.0, bd = 1e30;
        [unroll] for (int k = -1; k <= 1; k += 2) {
            int2 q = int2(clamp(int(id.x) + k, 0, int(rs.x) - 1), int(id.y));
            if (!ckFresh(q)) continue;
            float4 qn = LOAD_ND(Nd, q), v = resAt(q);
            float w = qn.w > 0.0 && nd.w > 0.0 ? geoWeight(nd.w, qn.w, nd.xyz, qn.xyz) : 0.0;
            ca += v * w, wsum += w;
            float e = abs(qn.w - nd.w);
            if (qn.w > 0.0 && e < bd) bd = e, cb = v;
        }
        center = wsum > 1e-4 ? ca / wsum : cb;
    } else center = resAt(int2(id));
    if (nd.w <= 0.0) { Out[id] = center; return; }
    int2 m = int2(rs) - 1, p = int2(id);
    float3 c = viewPos((float2(id) + 0.5) * rsRcp, nd.w);
    float4 sum = center;
    float ws = 1.0;
    {  // (CORTAO's sparse filter: 6 symmetric pairs)
        const int2 offs[6] = {int2(3, 0), int2(6, 0), int2(3, 3), int2(0, 3), int2(0, 6), int2(3, -3)};
        [unroll] for (int k = 0; k < 6; k++) {
            int2 o = int2(round(float2(offs[k]) * spatial));
            int2 qa = ckTap(clamp(p + o, int2(0, 0), m)), qb = ckTap(clamp(p - o, int2(0, 0), m));
            float4 na = LOAD_ND(Nd, qa), nb = LOAD_ND(Nd, qb);
            float wa = tapWeight(c, nd, qa, na), wb = tapWeight(c, nd, qb, nb);
            wa = wb = min(wa, wb);
            sum += resAt(qa) * wa + resAt(qb) * wb;
            ws += wa + wb;
        }
        float3 gs = sum.rgb;
        float gw = ws;
        if (flagOn(16384u)) {  // (flags 16384: the GI also from 3 pairs 10 pixels out, 60 degrees apart - the AO kept to
            // the near ones. 6 pairs at twice the near ones' distance: the same steadiness for twice the time; the set turned
            // every other frame, or FastGI's cheaper weights: less steady)
            const int2 wide[3] = {int2(10, 0), int2(5, 9), int2(-5, 9)};
            [unroll] for (int k = 0; k < 3; k++) {
                int2 o = int2(round(float2(wide[k]) * spatial));
                int2 qa = ckTap(clamp(p + o, int2(0, 0), m)), qb = ckTap(clamp(p - o, int2(0, 0), m));
                float4 na = LOAD_ND(Nd, qa), nb = LOAD_ND(Nd, qb);
                float wa = tapWeight(c, nd, qa, na), wb = tapWeight(c, nd, qb, nb);
                wa = wb = min(wa, wb);
                gs += resAt(qa).rgb * wa + resAt(qb).rgb * wb;
                gw += wa + wb;
            }
        }
        Out[id] = float4(gs / gw, sum.a / ws);
        return;
    }
}
