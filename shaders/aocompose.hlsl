// Ditto Relight (experimental), the composition (render size): the main pass' occlusion and bounce light
//   - through a 4 x 4 weighted by depth and normal: the main pass' noise is a fixed 4 x 4 pattern (each pixel of a tile
//     its own spiral rotation, dither and ray rotation - the same every frame), and the 4 x 4 around a pixel holds every
//     one of them once - their mean, the noise gone where the surface is one; across an edge the weights keep it apart.
//     (No accumulation over the frames: it trailed behind what moved.)
//   - onto the game's colour, which every later pass reads in its place: colour x lerp(1, occlusion, aoStrength) +
//     giStrength x albedo x bounce light. The albedo, unknown (the colour is albedo x light): the colour's hue at 0.6 of
//     its brightest channel, grey 0.5 where dark. Where the pixel is dark (its luma from 0.25 down to 0, the game's
//     units), the bounce light x up to giDark: above 1 the shadows lifted more by what surrounds them, below kept dark.
//   - a curve on the lights (sCurve > 0): above mid-grey, e stops over it, e + sCurve tanh(e / 2) - x (1 + sCurve / 2)
//     just above mid-grey, at most sCurve stops further up; mid-grey and below as they are. The colour scaled, its hue kept.
// t0 = the prepass' normal and depth, t1 = the game's colour, t2 = the main pass'; u0 = the colour (RGBA16F).
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Color : register(t1);
Texture2D<float4> Res : register(t2);
RWTexture2D<float4> Out : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rs)) return;
    int2 p = int2(id), m = int2(rs) - 1;
    float4 c = Color.Load(int3(p, 0));
    float4 nd = Nd.Load(int3(p, 0));
    if (nd.w <= 0.0) { Out[id] = c; return; }
    float4 acc = 0.0;
    float ws = 0.0;
    [unroll] for (int j = -2; j <= 1; j++) {
        [unroll] for (int i = -2; i <= 1; i++) {
            int2 q = clamp(p + int2(i, j), int2(0, 0), m);
            float4 qn = Nd.Load(int3(q, 0));
            if (qn.w <= 0.0) continue;
            float wd = exp2(-abs(qn.w - nd.w) / nd.w * 64.0);
            float wn = saturate(dot(qn.xyz, nd.xyz));
            wn *= wn, wn *= wn, wn *= wn;
            float w = wd * wn;
            acc += Res.Load(int3(q, 0)) * w;
            ws += w;
        }
    }
    float4 r = ws > 0.0 ? acc / ws : Res.Load(int3(p, 0));
    float3 col = max(c.rgb, 0.0);
    float mx = max(col.r, max(col.g, col.b));
    float3 albedo = lerp((float3)0.5, 0.6 * col / max(mx, 1e-4), saturate(mx * 8.0));
    float dark = 1.0 - smoothstep(0.0, 0.25, dot(col, float3(0.2126, 0.7152, 0.0722)));
    float3 o = col * lerp(1.0, r.a, aoStrength) + giStrength * lerp(1.0, giDark, dark) * albedo * r.rgb;
    if (sCurve > 0.0) {
        float l = dot(o, float3(0.2126, 0.7152, 0.0722));
        if (l > pivot) o *= exp2(sCurve * tanh(0.5 * log2(l / pivot)));
    }
    Out[id] = float4(o, c.a);
}
