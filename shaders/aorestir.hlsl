// Ditto Relight (experimental), ReSTIR GI in screen space (flags2 8, ao_restir; D3D12): the main pass keeps, by weighted
// reservoir sampling among the occluders that lit the pixel, one of them - its offset from the pixel (view space), its
// light (over the cosine it was seen at) and its weight W, the reservoir's candidates M (the samples) -; this pass reuses
// them (Bitterli 2020, Ouyang 2021, simplified - no visibility re-test, no Jacobian: biased toward smoothness):
//   axis 0 (temporal): this frame's reservoir and the last frame's at the reprojected pixel (the game's motion vectors; its
//     depth within 5 %; its M at most 20 x the samples), merged - each x the target p^ = the luma of its light x the
//     cosine at this pixel; written as the next frame's history;
//   axis 1 (spatial): that one and 4 neighbours' within 4 - 16 pixels on this surface (depth within 10 %, normals within
//     ~25 degrees), their samples moved to this pixel's frame of reference; the GI then the chosen sample's light x its
//     cosine x W - in place of the main pass' sum, the AO kept.
// The pixel's GI from up to 20 frames and 5 pixels of samples, one chosen by how bright it is: a small bright light the
// spiral finds now and then kept, no longer flickering.
// t0 = the normal and depth at the main pass' size (t7 packed); axis 0: t1, t2 this frame's reservoir (A: offset, W;
// B: light, M), t3, t4 the last frame's, t5 motion, t6 the last frame's depth (the temporal smoothing's) -> u0, u1;
// axis 1: t1, t2 the temporal one, t3 the main pass' result -> u0 its result with the GI resampled.
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> RA : register(t1);
Texture2D<float4> RB : register(t2);
Texture2D<float4> T3 : register(t3);
Texture2D<float4> PB : register(t4);
Texture2D<float2> Mv : register(t5);
Texture2D<float> PrevD : register(t6);
AOFMT("rgba32f") RWTexture2D<float4> OutA : register(u0);
AOFMT("rgba16f") RWTexture2D<float4> OutB : register(u1);

struct Rsv {
    float3 d, c;
    float w, m;
};
float phat(float3 c, float3 d, float3 n) {
    float l = dot(d, d);
    return l > 1e-12 ? dot(c, float3(0.2126, 0.7152, 0.0722)) * saturate(dot(n, d * rsqrt(l))) : 0.0;
}
void merge(inout Rsv r, float3 d, float3 c, float W, float M, float3 n, inout uint seed) {
    float w = phat(c, d, n) * W * M;
    r.w += w, r.m += M;
    if (w > 0.0 && rndNext(seed) * r.w < w) r.d = d, r.c = c;
}
float finalW(Rsv r, float3 n) {
    float p = phat(r.c, r.d, n);
    return p > 0.0 && r.m > 0.0 ? r.w / (r.m * p) : 0.0;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rs)) return;
    float4 nd = LOAD_ND(Nd, id);
    bool spatial = axis > 0.5;
    if (nd.w <= 0.0) {
        if (spatial) OutA[id] = T3.Load(int3(id, 0));
        else OutA[id] = 0.0, OutB[id] = 0.0;
        return;
    }
    float3 n = nd.xyz;
    float2 uv = (float2(id) + 0.5) * rsRcp;
    float3 P = viewPos(uv, nd.w);
    uint seed = (id.x * 1973u + id.y * 9277u + (uint)frame * 26699u) | 1u;
    seed += spatial ? 0x68e31da4u : 0u;
    Rsv r;
    r.d = 0.0, r.c = 0.0, r.w = 0.0, r.m = 0.0;
    float4 a = RA.Load(int3(id, 0)), b = RB.Load(int3(id, 0));
    merge(r, a.xyz, b.rgb, a.w, b.a, n, seed);
    int2 m = int2(rs) - 1;
    if (!spatial) {
        if (temporal > 0.0) {
            float2 mv = Mv.Load(int3(clamp(int2(uv * mvSize), int2(0, 0), int2(mvSize) - 1), 0));
            float2 puv = uv + mv * mvScale;
            if (all(puv > 0.0) && all(puv < 1.0)) {
                int2 q = clamp(int2(puv * rs), int2(0, 0), m);
                float hd = PrevD.Load(int3(q, 0));
                if (hd > 0.0 && abs(hd - nd.w) < 0.05 * nd.w) {
                    float4 pa = T3.Load(int3(q, 0)), pb = PB.Load(int3(q, 0));
                    merge(r, pa.xyz, pb.rgb, pa.w, min(pb.a, 20.0 * samples), n, seed);
                }
            }
        }
        OutA[id] = float4(r.d, finalW(r, n)), OutB[id] = float4(r.c, r.m);
        return;
    }
    [unroll] for (int k = 0; k < 4; k++) {
        float ang = 6.2831853 * rndNext(seed), rad = 4.0 + 12.0 * rndNext(seed);
        int2 q = clamp(int2(id) + int2(round(float2(cos(ang), sin(ang)) * rad)), int2(0, 0), m);
        float4 qn = LOAD_ND(Nd, q);
        if (qn.w <= 0.0 || abs(qn.w - nd.w) > 0.1 * nd.w || dot(qn.xyz, n) < 0.9) continue;
        float4 qa = RA.Load(int3(q, 0)), qb = RB.Load(int3(q, 0));
        float3 d = viewPos((float2(q) + 0.5) * rsRcp, qn.w) + qa.xyz - P;  // (its sample from this pixel)
        if (dot(d, n) <= 0.0) continue;
        merge(r, d, qb.rgb, qa.w, qb.a, n, seed);
    }
    float W = finalW(r, n);
    float l = dot(r.d, r.d);
    float3 gi = l > 1e-12 ? r.c * saturate(dot(n, r.d * rsqrt(l))) * W : 0.0;
    OutA[id] = float4(gi, T3.Load(int3(id, 0)).a);
}
