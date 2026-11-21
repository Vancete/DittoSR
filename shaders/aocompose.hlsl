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
//     the sky too.
// t0 = the prepass' normal and depth, t1 = the game's colour, t2 = the occlusion and bounce light;
// u0 = the colour (RGBA16F).
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Color : register(t1);
Texture2D<float4> Res : register(t2);
Texture2D<float> GDepth : register(t3);    // (the game's depth: flags 131072)
Texture2D<float> CurveLut : register(t4);  // (the curve's table: flags 262144)
Texture2D<uint4> Mip3c : register(t5);    // (better albedo, flags2 16: the mips' level 3)
AOFMT("rgba16f") RWTexture2D<float4> Out : register(u0);

#include "ao_up.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rs)) return;
    int2 p = int2(id);
    float4 c = Color.Load(int3(p, 0));
    float4 nd = ndAt(GDepth, p);
    float3 o = c.rgb;
    if (nd.w > 0.0) {
        float4 r = flagOn(8u) ? upHalf(Res, p, nd) : Res.Load(int3(p, 0));  // (flags 8: t2 the half size's - upsampled here)
        float3 col = max(c.rgb, 0.0);
        float mx = max(col.r, max(col.g, col.b));
        float3 albedo = lerp((float3)0.5, 0.6 * col / max(mx, 1e-4), saturate(mx * 8.0));
        if (flag2On(16u) && mips > 0.5) {
            // (better albedo, flags2 16: the colour over its area's luma - the lighting there, retinex's way - x 0.5, a mid
            // grey's albedo where the pixel is as lit as its area. The area: level 3's 3 x 3 texels around the pixel (8 x 8
            // each), each x a Gaussian by its distance x how near its depth is to the pixel's (another surface - a light bar
            // on the ceiling above a wall - left out), its luma at most 4 x the pixel's own: a bright light in the area no
            // longer darkens the whole cell it falls in - the blocks around light bars)
            const float3 Y = float3(0.2126, 0.7152, 0.0722);
            uint2 d3 = ((uint2)rs + 7u) >> 3;
            float2 t = (float2(p) + 0.5) * rsRcp * float2(d3) - 0.5;
            int2 tb = int2(floor(t + 0.5));
            float lp = dot(col, Y), la = 0.0, wa = 0.0;
            [unroll] for (int k = 0; k < 9; k++) {
                int2 q = clamp(tb + int2(k % 3 - 1, k / 3 - 1), int2(0, 0), int2(d3) - 1);
                float3 c3;
                float4 m = mipDecode(Mip3c.Load(int3(q, 0)), c3);
                float2 dd = float2(q) - t;
                float w = exp(-dot(dd, dd) * 0.7) * (m.w > 0.0 ? saturate(1.0 - abs(m.w - nd.w) / (nd.w * 0.05 + 1e-3)) : 0.0);
                la += min(dot(c3, Y), 4.0 * lp + 1e-3) * w, wa += w;
            }
            // (its strength - flags2's bits 16 - 23: the old guess to this)
            if (wa > 1e-3) albedo = lerp(albedo, saturate(0.5 * col / max(la / wa, 1e-4)), float((flags2 >> 16) & 255u) / 255.0);
        }
        float dark = 1.0 - smoothstep(0.0, 0.25, dot(col, float3(0.2126, 0.7152, 0.0722)));
        float3 b = giStrength * lerp(1.0, giDark, dark) * albedo * r.rgb;
        if (giCeiling > 0.0) {
            float3 cap = giCeiling * col + 1e-6;
            b = cap * (1.0 - exp(-b / cap));
        }
        float occ = aoPow > 0.5 ? pow(saturate(r.a), aoStrength) : lerp(1.0, r.a, aoStrength);  // (CORTAO: pow for its intensity)
        float3 occ3 = occ;
        if (flagOn(256u)) {
            // (AO on the ambient light alone: GTAO's multi-bounce fit (Jimenez 2016) by the albedo's guess - a light
            // material's shade lifted by its own bounces -, and faded out on bright pixels (from 55 % of the perceived
            // brightness up: direct light, emissive surfaces - not the ambient light the occlusion takes away))
            float a = saturate(r.a);
            float3 A = 2.0404 * albedo - 0.3324, B = -4.7951 * albedo + 0.6417, C = 2.7552 * albedo + 0.6903;
            float3 mb = max(a, ((a * A + B) * a + C) * a);
            occ3 = aoPow > 0.5 ? pow(saturate(mb), aoStrength) : lerp(1.0, mb, aoStrength);
            float ly = dot(col, float3(0.2126, 0.7152, 0.0722));
            float lp = pivot < 0.2 ? pow(max(ly, 0.0), 1.0 / 2.2) : ly;
            occ3 = lerp(occ3, 1.0, smoothstep(0.55, 0.9, lp));
        }
        if (fadeZ > 0.0) occ3 = lerp(occ3, 1.0, saturate((nd.w - fadeZ) / fadeZ));  // (the AO faded from fadeZ to twice it)
        o = col * occ3 + b;
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
    if (curveN > 1.5) {
        if (flagOn(262144u) && all(o <= 16.0)) {  // (262144: the baked table, by the square root - aocurve.hlsl)
            float3 u = (sqrt(max(o, 0.0) / 16.0) * 4095.0 + 0.5) / 4096.0;
            o = float3(CurveLut.SampleLevel(linearClamp, float2(u.r, 0.5), 0.0), CurveLut.SampleLevel(linearClamp, float2(u.g, 0.5), 0.0),
                       CurveLut.SampleLevel(linearClamp, float2(u.b, 0.5), 0.0));
        } else {
            float3 t = lin ? pow(max(o, 0.0), 1.0 / 2.2) : max(o, 0.0);
            t = float3(curveAt(t.r), curveAt(t.g), curveAt(t.b));
            o = lin ? pow(t, 2.2) : t;
        }
    }
    Out[id] = float4(o, c.a);
}
