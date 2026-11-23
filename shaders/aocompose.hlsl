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
#if !DITTO_VK
Texture2D<float2> Local : register(t5);   // (the local exposure, flags2 16384: aolocal.hlsl's area luma)
#endif
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
        float4 r = flagOn(8u) ? upHalfGuided(Res, Color, p, nd) : Res.Load(int3(p, 0));  // (flags 8: t2 the half size's - upsampled here)
        float3 col = max(c.rgb, 0.0);
        float mx = max(col.r, max(col.g, col.b));
        float3 albedo = lerp((float3)0.5, 0.6 * col / max(mx, 1e-4), saturate(mx * 8.0));
        // (the local exposure (flags2 16384): its area's luma, aolocal.hlsl's, bilinear between level 3's texels, each x how
        // near its depth is to the pixel's (another surface left out) x e^(-(stops from the pixel)^2 / 2) (bilateral in
        // brightness: a light beside a wall no longer darkens it), the pixel itself a little - smooth across the texels' borders)
        float le = 0.0;
#if !DITTO_VK
        if (flag2On(16384u) && mips > 0.5) {
            float lp = dot(col, float3(0.2126, 0.7152, 0.0722));
            uint2 d3 = ((uint2)rs + 7u) >> 3;
            float2 t = (float2(p) + 0.5) * rsRcp * float2(d3) - 0.5, tb = floor(t), tf = t - tb;
            float we = 0.05;
            le = 0.05 * lp;
            [unroll] for (int k = 0; k < 4; k++) {
                int2 o2 = int2(k & 1, k >> 1);
                float2 m = Local.Load(int3(clamp(int2(tb) + o2, int2(0, 0), int2(d3) - 1), 0));
                float2 bl = lerp(1.0 - tf, tf, float2(o2));
                float w = bl.x * bl.y * (m.y > 0.0 ? saturate(1.0 - abs(m.y - nd.w) / (nd.w * 0.05 + 1e-3)) : 0.0) + 1e-6;
                float st = log2(max(m.x, 1e-5) / max(lp, 1e-5)), wr = w * exp(-0.5 * st * st);
                le += m.x * wr, we += wr;
            }
            le /= we;
        }
#endif
        float dark = 1.0 - smoothstep(0.0, 0.25, dot(col, float3(0.2126, 0.7152, 0.0722)));
        float3 b = giStrength * lerp(1.0, giDark, dark) * albedo * r.rgb;
        if (giCeiling > 0.0) {
            float3 cap = giCeiling * col + 1e-6;
            b = cap * (1.0 - exp(-b / cap));
        }
        float occ = pow(saturate(r.a), aoStrength);  // (CORTAO: pow for its intensity)
        float3 occ3 = occ;
        if (fadeZ > 0.0) occ3 = lerp(occ3, 1.0, saturate((nd.w - fadeZ) / fadeZ));  // (the AO faded from fadeZ to twice it)
        o = col * occ3 + b;
        if (flag2On(16384u) && le > 0.0) {
            // (local exposure, flags2 16384: x (a mid grey / its area's luma) ^ (0.5 x strength) - the shade lifted, the
            // glare brought down, the contrast within each area kept; params1's bits 0 - 7 its strength, x 4 at most either
            // way)
            float s = float(params1 & 255u) / 255.0;
            o *= pow(clamp((flag2On(2048u) ? 0.18 : 0.5) / max(le, 1e-4), 0.25, 4.0), 0.5 * s);
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
