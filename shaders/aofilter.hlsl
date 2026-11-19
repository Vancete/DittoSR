// Ditto Relight (experimental), the spatial filter (render size), before the temporal smoothing (aotemporal.hlsl):
//   axis 0 / 1 (FastGI's): the main pass' occlusion and bounce light along one axis a pass (x, then y) - the centre and
// 4 taps spatial x 2 pixels apart (weights 0.75, 0.375), each x geoWeight: its depth and normal near the centre's.
//   axis 2 (CORTAO's filter pass, its large filter): one pass - the centre and 6 symmetric pairs (3, 0), (6, 0), (3, 3),
// (0, 3), (0, 6), (3, -3) pixels (x spatial), each x planeWeight; CORTAO's "cheap, sparse blur" that prefilters its
// output for the temporal filters after it.
//   planeW > 0 (CORTAO's weights, either filter): each tap's depth against the centre's plane at the tap (a slanted floor
// no longer rejected for its slope) and its normal's, exp2(-error); each pair x the lower of its two weights (CORTAO's
// filter symmetry: contact shadows kept).
// t0 = the prepass' normal and depth, t1 = the main pass' (GI rgb, occlusion a); u0 = the filtered.
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Res : register(t1);
AOFMT("rgba16f") RWTexture2D<float4> Out : register(u0);

// a tap's weight against the centre (c: its view position, nd its normal and depth): CORTAO's (planeW) or FastGI's
float tapWeight(float3 c, float4 nd, int2 q, float4 qn, bool cortao) {
    if (qn.w <= 0.0) return 0.0;
    if (cortao) return planeWeight(c, nd.xyz, (float2(q) + 0.5) * rsRcp, qn.w, qn.xyz);
    return geoWeight(nd.w, qn.w, nd.xyz, qn.xyz);
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rs)) return;
    float4 center = Res.Load(int3(id, 0));
    float4 nd = LOAD_ND(Nd, id);
    if (nd.w <= 0.0) { Out[id] = center; return; }
    int2 m = int2(rs) - 1, p = int2(id);
    float3 c = viewPos((float2(id) + 0.5) * rsRcp, nd.w);
    bool cw = planeW > 0.5;
    float4 sum = center;
    float ws = 1.0;
    if (axis > 1.5) {  // (CORTAO's sparse filter: 6 symmetric pairs, its weights alone)
        const int2 offs[6] = {int2(3, 0), int2(6, 0), int2(3, 3), int2(0, 3), int2(0, 6), int2(3, -3)};
        [unroll] for (int k = 0; k < 6; k++) {
            int2 o = int2(round(float2(offs[k]) * spatial));
            int2 qa = clamp(p + o, int2(0, 0), m), qb = clamp(p - o, int2(0, 0), m);
            float4 na = LOAD_ND(Nd, qa), nb = LOAD_ND(Nd, qb);
            float wa = tapWeight(c, nd, qa, na, true), wb = tapWeight(c, nd, qb, nb, true);
            if (cw) wa = wb = min(wa, wb);
            sum += Res.Load(int3(qa, 0)) * wa + Res.Load(int3(qb, 0)) * wb;
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
                int2 qa = clamp(p + o, int2(0, 0), m), qb = clamp(p - o, int2(0, 0), m);
                float4 na = LOAD_ND(Nd, qa), nb = LOAD_ND(Nd, qb);
                float wa = tapWeight(c, nd, qa, na, true), wb = tapWeight(c, nd, qb, nb, true);
                if (cw) wa = wb = min(wa, wb);
                gs += Res.Load(int3(qa, 0)).rgb * wa + Res.Load(int3(qb, 0)).rgb * wb;
                gw += wa + wb;
            }
        }
        Out[id] = float4(gs / gw, sum.a / ws);
        return;
    }
    int2 ax = axis > 0.5 ? int2(0, 1) : int2(1, 0);
    [unroll] for (int k = 1; k <= 2; k++) {
        // (the distance rounded: fractional radii symmetric)
        int off = int(floor(float(k) * spatial * 2.0 + 0.5));
        int2 qa = clamp(p + ax * off, int2(0, 0), m), qb = clamp(p - ax * off, int2(0, 0), m);
        float4 na = LOAD_ND(Nd, qa), nb = LOAD_ND(Nd, qb);
        float wa = tapWeight(c, nd, qa, na, cw), wb = tapWeight(c, nd, qb, nb, cw);
        if (cw) wa = wb = min(wa, wb);
        float g = k == 1 ? 0.75 : 0.375;
        sum += (Res.Load(int3(qa, 0)) * wa + Res.Load(int3(qb, 0)) * wb) * g;
        ws += (wa + wb) * g;
    }
    Out[id] = sum / ws;
}
