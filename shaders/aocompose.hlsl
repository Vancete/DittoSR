// Ditto Relight (experimental), the composition (render size): the main pass' occlusion and bounce light
//   - through a sparse 3 x 3 (2 pixels apart) weighted by depth and normal (CORTAO's prefilter idea),
//   - then accumulated over the frames: the last frame's result where this pixel was (the game's motion vectors), unless
//     the depth there differs by over 5 % (something else was there: a disocclusion), x 0.85 - each frame's samples
//     land elsewhere (their noise per pixel and per frame), the mean of ~7 frames what the eye should see,
//   - onto the game's colour, which every later pass reads in its place: colour x lerp(1, occlusion, aoStrength) +
//     giStrength x albedo x bounce light. The albedo, unknown (the colour is albedo x light): the colour's hue at 0.6 of
//     its brightest channel, grey 0.5 where dark. Where the pixel is dark (its luma from 0.25 down to 0, the game's
//     units), the bounce light x up to giDark: above 1 the shadows lifted more by what surrounds them, below kept dark.
//   - an S-curve on its luma (sCurve > 0): in stops from mid-grey e, e + sCurve tanh(e / 2) - x (1 + sCurve / 2) near
//     mid-grey, at most sCurve stops further out: lights brighter, shadows darker; the colour scaled, its hue kept.
// t0 = the prepass' normal and depth, t1 = the game's colour, t2 = the main pass', t3 = the game's motion vectors,
// t4 = the last frame's normal and depth, t5 = the last frame's accumulation; u0 = the colour (RGBA16F), u1 = this
// frame's accumulation.
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Color : register(t1);
Texture2D<float4> Res : register(t2);
Texture2D<float2> Mv : register(t3);
Texture2D<float4> PrevNd : register(t4);
Texture2D<float4> PrevAcc : register(t5);
RWTexture2D<float4> Out : register(u0);
RWTexture2D<float4> AccOut : register(u1);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rs)) return;
    int2 p = int2(id), m = int2(rs) - 1;
    float4 c = Color.Load(int3(p, 0));
    float4 nd = Nd.Load(int3(p, 0));
    if (nd.w <= 0.0) { Out[id] = c; AccOut[id] = float4(0.0, 0.0, 0.0, 1.0); return; }
    float4 acc = 0.0;
    float ws = 0.0;
    [unroll] for (int j = -1; j <= 1; j++) {
        [unroll] for (int i = -1; i <= 1; i++) {
            int2 q = clamp(p + 2 * int2(i, j), int2(0, 0), m);
            float4 qn = Nd.Load(int3(q, 0));
            if (qn.w <= 0.0) continue;
            float wd = exp2(-abs(qn.w - nd.w) / nd.w * 64.0);
            float wn = saturate(dot(qn.xyz, nd.xyz));
            wn *= wn, wn *= wn, wn *= wn;
            float w = wd * wn * (i == 0 && j == 0 ? 2.0 : 1.0);
            acc += Res.Load(int3(q, 0)) * w;
            ws += w;
        }
    }
    float4 r = ws > 0.0 ? acc / ws : Res.Load(int3(p, 0));
    // the accumulation: where the pixel was, its depth then within 5 % of now (and on screen)
    if (histOk > 0.5) {
        float2 uv = (float2(id) + 0.5) * rsRcp;
        float2 puv = uv + Mv.SampleLevel(linearClamp, uv, 0.0) * mvToUv;
        if (all(puv > 0.0) && all(puv < 1.0)) {
            float pz = PrevNd.Load(int3(int2(puv * rs), 0)).w;
            if (pz > 0.0 && abs(pz - nd.w) < 0.05 * nd.w) r = lerp(PrevAcc.SampleLevel(linearClamp, puv, 0.0), r, 0.15);
        }
    }
    AccOut[id] = r;
    float3 col = max(c.rgb, 0.0);
    float mx = max(col.r, max(col.g, col.b));
    float3 albedo = lerp((float3)0.5, 0.6 * col / max(mx, 1e-4), saturate(mx * 8.0));
    float dark = 1.0 - smoothstep(0.0, 0.25, dot(col, float3(0.2126, 0.7152, 0.0722)));
    float3 o = col * lerp(1.0, r.a, aoStrength) + giStrength * lerp(1.0, giDark, dark) * albedo * r.rgb;
    if (sCurve > 0.0) {
        float l = dot(o, float3(0.2126, 0.7152, 0.0722));
        if (l > 1e-6) {
            float ev = log2(l / pivot);
            o *= exp2(sCurve * tanh(0.5 * ev));
        }
    }
    Out[id] = float4(o, c.a);
}
