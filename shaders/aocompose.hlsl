// Ditto Relight (experimental), the composition (render size): the occlusion and bounce light
//   - blurred along y (aoblur.hlsl did x: a 17 x 17 depth- and normal-weighted Gaussian in all),
//   - onto the game's colour, which every later pass reads in its place: colour x lerp(1, occlusion, aoStrength) +
//     giStrength x albedo x bounce light. The albedo, unknown (the colour is albedo x light): the colour's hue at 0.6 of
//     its brightest channel, grey 0.5 where dark. Where the pixel is dark (its luma from 0.25 down to 0, the game's
//     units), the bounce light x up to giDark: above 1 the shadows lifted more by what surrounds them, below kept dark.
//   - a curve on the lights (sCurve > 0): from 30 % of the brightness up (pivot - 30 % perceived, gamma 2.2), e stops over
//     it brightened by sCurve tanh(e / 2)^2 stops - starting flat (no kink where it begins), at most sCurve stops; below
//     the pivot as it is. The colour scaled, its hue kept.
// t0 = the prepass' normal and depth, t1 = the game's colour, t2 = the blur's first half; u0 = the colour (RGBA16F).
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Color : register(t1);
Texture2D<float4> Res : register(t2);
RWTexture2D<float4> Out : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rs)) return;
    int2 p = int2(id);
    float4 c = Color.Load(int3(p, 0));
    float4 nd = Nd.Load(int3(p, 0));
    if (nd.w <= 0.0) { Out[id] = c; return; }
    float4 r = aoBlur(Res, Nd, p, int2(0, 2), nd);
    float3 col = max(c.rgb, 0.0);
    float mx = max(col.r, max(col.g, col.b));
    float3 albedo = lerp((float3)0.5, 0.6 * col / max(mx, 1e-4), saturate(mx * 8.0));
    float dark = 1.0 - smoothstep(0.0, 0.25, dot(col, float3(0.2126, 0.7152, 0.0722)));
    float3 o = col * lerp(1.0, r.a, aoStrength) + giStrength * lerp(1.0, giDark, dark) * albedo * r.rgb;
    if (sCurve > 0.0) {
        float l = dot(o, float3(0.2126, 0.7152, 0.0722));
        if (l > pivot) {
            float t = tanh(0.5 * log2(l / pivot));
            o *= exp2(sCurve * t * t);
        }
    }
    Out[id] = float4(o, c.a);
}
