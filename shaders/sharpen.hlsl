// DittoSR's sharpening (output size): a contrast-adaptive sharpen of the history just written - in its perceptual
// domain - into the game's output. Each channel: the centre against its 4 neighbours, by as much as the neighbourhood
// leaves room before black or white (the room to the clip over the peak, square-rooted, as AMD's CAS): flat areas and
// strong edges are left almost alone, fine detail gains contrast without halos. The scale: at rest below 0.6 a soft
// blur, 0.25 x (sharpness - 0.6) (the reconstruction alone looked oversharpened: 0 soft, 0.6 as it is); above it a
// sharpening up to 0.125 at 1, more at higher ratios and lower output resolutions (below), x 0.5 - 1 by the stability
// level (a pixel just cut still holds the jittered samples' noise it would amplify); moving - from 2 output pixels a
// frame - 0.1 x sharpness: the wider reconstruction there softens the image (0.2 doubled the shimmer of an uneven pan).
// Above 2x the moving term grows with the ratio (x 2.25 at 3x): a moving object's detail is softer there - at 3x it was
// 12 % below 2x's against the reference, now level.
//   The comparison (splitX > 0): the game's own upscaler wrote the output first; this pass writes the left of splitX
// alone, a line between.
// t0 = the history (u0 of the upscale; Vulkan: a = the stability level), t1 = the convert pass's (motion), t7 = the
// stability levels (D3D12), u1 = the game's output.
#include "common.hlsli"

Texture2D<float4> History : register(t0);
Texture2D<float4> MotionDepthClip : register(t1);
#if !DITTO_VK
Texture2D<float> Stab : register(t7);
#endif
VKFMT(OUTFMT) RWTexture2D<float4> Output : register(u1);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)os)) return;
    if (splitX > 0.0 && float(id.x) >= splitX) {
        if (float(id.x) < splitX + 2.0) Output[id] = float4(dec((float3)0.85, encExposure()), 1.0);
        return;
    }
    float e = encExposure();
    int2 p = int2(id), m = int2(os) - 1;
    float4 c4 = History.Load(int3(p, 0));
#if !DITTO_VK
    c4.a = Stab.Load(int3(p, 0));
#endif
    float3 c = c4.rgb;
    float2 uv = (float2(id) + 0.5) * osRcp;
    float mlen = length(0.5 * MotionDepthClip.SampleLevel(linearClamp, uv, 0.0).xy * os);
    // (above 0.6 at rest, up to 0.125 - CAS's weakest - at 1, x up to 1.6 from 1.5x to 3x and from a 2160 to a 1080
    // line output: there each output pixel covers more of the scene and the reconstruction's softness shows more; 0.2
    // at most - CAS's strongest)
    float sh = saturate(sharpness);
    float boost = 1.0 + 0.3 * saturate((ratio - 1.5) / 1.5) + 0.3 * saturate((2160.0 - os.y) / 1080.0);
    float kr = sh < 0.6 ? 0.25 * (sh - 0.6) : min(0.125 * boost, 0.2) * (sh - 0.6) / 0.4;
    float km = 0.1 * sh * (1.0 + 1.25 * saturate(ratio - 2.0));
    float mv = saturate(mlen * 0.5);
    float k = min(lerp(kr > 0.0 ? kr * (0.5 + 0.5 * c4.a) : kr, km, mv), 0.2);  // (0.2: CAS's strongest)
    // (the filter at hf precision - half in the FP16 variant)
    hf3 ch = (hf3)c;
    hf3 n = (hf3)History.Load(int3(clamp(p + int2(0, -1), int2(0, 0), m), 0)).rgb;
    hf3 s = (hf3)History.Load(int3(clamp(p + int2(0, 1), int2(0, 0), m), 0)).rgb;
    hf3 w = (hf3)History.Load(int3(clamp(p + int2(-1, 0), int2(0, 0), m), 0)).rgb;
    hf3 ea = (hf3)History.Load(int3(clamp(p + int2(1, 0), int2(0, 0), m), 0)).rgb;
    hf3 mn = min(ch, min(min(n, s), min(w, ea))), mx = max(ch, max(max(n, s), max(w, ea)));
    hf3 amp = sqrt(saturate(min(mn, (hf)1.0 - mx) / max(mx, (hf)1e-4)));
    hf3 wgt = k >= 0.0 ? -amp * (hf)k : (hf3)(hf)(-k);
    hf3 r = saturate((ch + wgt * (n + s + w + ea)) / ((hf)1.0 + (hf)4.0 * wgt));
    // (moving, within its neighbourhood's range: a pixel the reconstruction left a little brighter or darker than its
    // neighbours - the new frame's jittered samples, more of them above 2x - is not pushed further out, a sparkle that
    // moves with the image; detail between its neighbours still gains contrast. A quarter of the range beyond it up to
    // 2x - its moving sharpness as before - none at 3x: there, at sharpness 1, the shimmer of a pan 7.9 -> 3.3)
    hf3 mrg = (hf)(0.25 * (1.0 - saturate(ratio - 2.0))) * (mx - mn);
    r = lerp(r, clamp(r, mn - mrg, mx + mrg), (hf)mv);
    float3 o = dec((float3)r, e);
    float3 ic;
    if (indicator > 0.5 && indicatorAt(id, e, ic)) o = ic;
    Output[id] = float4(o, 1.0);
}
