// Ditto Relight (experimental), the composition (render size): the occlusion and bounce light
//   - filtered (aofilter.hlsl) and smoothed over the frames (aotemporal.hlsl, temporal smoothing on),
//   - onto the game's colour, which every later pass reads in its place: colour x lerp(1, occlusion, aoStrength) +
//     giStrength x albedo x bounce light. The albedo, unknown (the colour is albedo x light): the colour's hue at 0.6 of
//     its brightest channel, grey 0.5 where dark. Where the pixel is dark (its luma from 0.25 down to 0, the game's
//     units), the bounce light x up to giDark: above 1 the shadows lifted more by what surrounds them, below kept dark.
//     With giCeiling > 0 the bounce light b blended up to a ceiling, each channel: giCeiling x colour x (1 - e^(-b /
//     (giCeiling x colour))) - as added while small against the pixel, then saturating at giCeiling x its own colour:
//     a dark material (the albedo's grey guess wrong there) no longer veiled by a bright floor's bounce, a light one in
//     shade still lifted, by at most 1 + giCeiling.
//   - a curve on the lights (sCurve > 0): from 30 % of the brightness up (pivot - 30 % perceived, gamma 2.2), e stops over
//     it brightened by sCurve tanh(e / 2)^2 stops - starting flat (no kink where it begins), at most sCurve stops; below
//     the pivot as it is. The colour scaled, its hue kept.
//   - the curve (curveN > 0, the menu's curves editor): each channel through a natural cubic spline over its points,
//     in a perceptual [0, 1] (linear HDR: ^ 1 / 2.2; above 1, a slope of 1 past the curve's end) - the whole image,
//     the sky too. And the image's luma histogram before it (64 bins, a pixel of each 4 x 4), for the editor.
// t0 = the prepass' normal and depth, t1 = the game's colour, t2 = the occlusion and bounce light;
// u0 = the colour (RGBA16F), u1 = the histogram (64 x 1, cleared by aohist.hlsl).
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Color : register(t1);
Texture2D<float4> Res : register(t2);
AOFMT("rgba16f") RWTexture2D<float4> Out : register(u0);
AOFMT("r32ui") RWTexture2D<uint> Hist : register(u1);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rs)) return;
    int2 p = int2(id);
    float4 c = Color.Load(int3(p, 0));
    float4 nd = Nd.Load(int3(p, 0));
    float3 o = c.rgb;
    if (nd.w > 0.0) {
        float4 r = Res.Load(int3(p, 0));
        float3 col = max(c.rgb, 0.0);
        float mx = max(col.r, max(col.g, col.b));
        float3 albedo = lerp((float3)0.5, 0.6 * col / max(mx, 1e-4), saturate(mx * 8.0));
        float dark = 1.0 - smoothstep(0.0, 0.25, dot(col, float3(0.2126, 0.7152, 0.0722)));
        float3 b = giStrength * lerp(1.0, giDark, dark) * albedo * r.rgb;
        if (giCeiling > 0.0) {
            float3 cap = giCeiling * col + 1e-6;
            b = cap * (1.0 - exp(-b / cap));
        }
        float occ = aoPow > 0.5 ? pow(saturate(r.a), aoStrength) : lerp(1.0, r.a, aoStrength);  // (CORTAO: pow for its intensity)
        o = col * occ + b;
        if (sCurve > 0.0) {
            float l = dot(o, float3(0.2126, 0.7152, 0.0722));
            if (l > pivot) {
                float t = tanh(0.5 * log2(l / pivot));
                o *= exp2(sCurve * t * t);
            }
        }
    }
    // (the curve's domain: perceptual - linear HDR's pivot 0.071 against [0, 1] colour's 0.3)
    bool lin = pivot < 0.2;
    if ((id.x & 3u) == 0u && (id.y & 3u) == 0u) {
        float l = max(dot(o, float3(0.2126, 0.7152, 0.0722)), 0.0);
        float t = lin ? pow(l, 1.0 / 2.2) : l;
        InterlockedAdd(Hist[uint2(min((uint)(saturate(t) * 64.0), 63u), 0u)], 1u);
    }
    if (curveN > 1.5) {
        float3 t = lin ? pow(max(o, 0.0), 1.0 / 2.2) : max(o, 0.0);
        t = float3(curveAt(t.r), curveAt(t.g), curveAt(t.b));
        o = lin ? pow(t, 2.2) : t;
    }
    Out[id] = float4(o, c.a);
}
