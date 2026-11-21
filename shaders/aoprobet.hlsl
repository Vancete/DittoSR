// Ditto Relight (experimental), the screen probes' temporal accumulation (ao_gi_probes; D3D12): each probe (its pixel
// moved a frame within its 4 x 4) blended with the last frame's probes where its pixel was - reprojected by the game's motion
// vectors onto the last frame's probe pixels (their own offset then), bilinear, each tap rejected by depth -, clamped to its
// 3 x 3 probes' range (+ a quarter); 0.85 the history's share, less in motion. No valid history (temporal 0): this frame's.
// t0 = this frame's probes (rgb the GI, a the depth), t1 = the history, t4 = the game's motion vectors; u0 = the new history.
#include "ao_common.hlsli"

Texture2D<float4> Cur : register(t0);
Texture2D<float4> Hist : register(t1);
Texture2D<float2> Mv : register(t4);
AOFMT("rgba16f") RWTexture2D<float4> Out : register(u0);
// (directional probes, flags2 64: the light's mean direction, accumulated alike)
Texture2D<float4> CurDir : register(t2);
Texture2D<float4> HistDir : register(t3);
AOFMT("rgba16f") RWTexture2D<float4> OutDir : register(u1);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    uint2 pd = probeDims(), md = mainDims();
    if (any(id >= pd)) return;
    float4 c = Cur.Load(int3(id, 0));
    float z = c.a;
    bool dr = flag2On(64u);
    float4 cd = dr ? CurDir.Load(int3(id, 0)) : 0.0;
    if (dr) OutDir[id] = cd;
    if (z <= 0.0 || temporal <= 0.0) { Out[id] = c; return; }
    int2 pm = int2(pd) - 1;
    uint2 s = min(id * 4u + probeJitter(), md - 1u);
    float2 uv = (float2(s) + 0.5) / float2(md);
    float2 mv = Mv.Load(int3(clamp(int2(uv * mvSize), int2(0, 0), int2(mvSize) - 1), 0));
    float2 puv = uv + mv * mvScale;
    if (any(puv <= 0.0) || any(puv >= 1.0)) { Out[id] = c; return; }
    float2 f = (puv * float2(md) - 0.5 - float2(probeJitterAt(frame - 1.0))) / 4.0;
    float2 b = floor(f), fr = f - b;
    float3 acc = 0.0, accd = 0.0;
    float ws = 0.0;
    [unroll] for (int k = 0; k < 4; k++) {
        int2 o = int2(k & 1, k >> 1), q = clamp(int2(b) + o, int2(0, 0), pm);
        float4 h = Hist.Load(int3(q, 0));
        float2 bl = lerp(1.0 - fr, fr, float2(o));
        float w = bl.x * bl.y * step(0.00001, h.a) * saturate(1.0 - abs(h.a - z) / (z * 0.05 + 0.02));
        acc += h.rgb * w, ws += w;
        if (dr) accd += HistDir.Load(int3(q, 0)).xyz * w;
    }
    if (ws < 0.05) { Out[id] = c; return; }
    float3 hist = acc / ws, lo = c.rgb, hi = c.rgb;
    [unroll] for (int j = 0; j < 9; j++) {
        int2 q = clamp(int2(id) + int2(j % 3 - 1, j / 3 - 1), int2(0, 0), pm);
        float4 n = Cur.Load(int3(q, 0));
        if (n.a > 0.0 && abs(n.a - z) < z * 0.1) lo = min(lo, n.rgb), hi = max(hi, n.rgb);
    }
    float3 mg = (hi - lo) * 0.25 + 0.001;
    hist = clamp(hist, max(lo - mg, 0.0), hi + mg);
    float motion = length((puv - uv) * float2(md));
    float w = 0.92 * saturate(ws) / (1.0 + motion * 0.05);
    Out[id] = float4(lerp(c.rgb, hist, w), z);
    if (dr) {
        float3 d = lerp(cd.xyz, accd / ws, w);
        OutDir[id] = float4(dot(d, d) > 1e-12 ? normalize(d) : (float3)0.0, z);
    }
}
