// Ditto Relight (experimental), the composition (render size): the occlusion and bounce light
//   - blurred along y (aoblur.hlsl did x: a 17 x 17 depth- and normal-weighted Gaussian in all),
//   - onto the game's colour, which every later pass reads in its place: colour x lerp(1, occlusion, aoStrength) +
//     giStrength x albedo x bounce light. The albedo, unknown (the colour is albedo x light): the colour's hue at 0.6 of
//     its brightest channel, grey 0.5 where dark. Where the pixel is dark (its luma from 0.25 down to 0, the game's
//     units), the bounce light x up to giDark: above 1 the shadows lifted more by what surrounds them, below kept dark.
//   - a curve on the lights (sCurve > 0): from 30 % of the brightness up (pivot - 30 % perceived, gamma 2.2), e stops over
//     it brightened by sCurve tanh(e / 2)^2 stops - starting flat (no kink where it begins), at most sCurve stops; below
//     the pivot as it is. The colour scaled, its hue kept.
// t0 = the prepass' normal and depth, t1 = the game's colour, t2 = the blur's first half, t3 = FastGI's (giMode 1);
// u0 = the colour (RGBA16F).
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Color : register(t1);
Texture2D<float4> Res : register(t2);
Texture2D<float4> Gi : register(t3);  // (giMode 1: FastGI's, filtered (and resolved), at its low resolution)
RWTexture2D<float4> Out : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rs)) return;
    int2 p = int2(id);
    float4 c = Color.Load(int3(p, 0));
    float4 nd = Nd.Load(int3(p, 0));
    if (nd.w <= 0.0) { Out[id] = c; return; }
    float4 r = aoBlur(Res, Nd, p, int2(0, (int)blurStep), nd);
    if (giMode > 0.5) {
        // (FastGI's Composite: its GI up from its low resolution - the 2 x 2 around the pixel, bilinear x fgGeo: none
        // where no tap is this surface, not the background's light on a thin foreground - x intensity x reflectance)
        int2 hm = int2(fgLow()) - 1;
        float2 lp = (float2(p) + 0.5) * rsRcp * float2(fgLow()) - 0.5, f = frac(lp);
        int2 b = int2(floor(lp));
        float3 acc = 0.0;
        float ws = 0.0;
        [unroll] for (int k = 0; k < 4; k++) {
            int2 o = int2(k & 1, k >> 1), q = clamp(b + o, int2(0, 0), hm);
            float4 t = Gi.Load(int3(q, 0));
            float2 bw = lerp(1.0 - f, f, float2(o));
            float w = bw.x * bw.y * fgGeo(nd.w, t.a, nd.xyz, Nd.Load(int3(fgFull(q), 0)).xyz);
            acc += t.rgb * w;
            ws += w;
        }
        r.rgb = acc / max(ws, 1e-4) * fgGain;
    }
    float3 col = max(c.rgb, 0.0);
    float mx = max(col.r, max(col.g, col.b));
    float3 albedo = lerp((float3)0.5, 0.6 * col / max(mx, 1e-4), saturate(mx * 8.0));
    float dark = 1.0 - smoothstep(0.0, 0.25, dot(col, float3(0.2126, 0.7152, 0.0722)));
    // (FastGI's GI added as FastGI does: x its intensity and diffuse reflectance (above), no albedo)
    float3 o = giMode > 0.5 ? col * lerp(1.0, r.a, aoStrength) + r.rgb
                            : col * lerp(1.0, r.a, aoStrength) + giStrength * lerp(1.0, giDark, dark) * albedo * r.rgb;
    if (sCurve > 0.0) {
        float l = dot(o, float3(0.2126, 0.7152, 0.0722));
        if (l > pivot) {
            float t = tanh(0.5 * log2(l / pivot));
            o *= exp2(sCurve * t * t);
        }
    }
    Out[id] = float4(o, c.a);
}
