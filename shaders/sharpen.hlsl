// DittoSR's sharpening (output size, when sharpness > 0): a contrast-adaptive sharpen of the history just written - in
// its perceptual domain - into the game's output. Each channel: the centre against its 4 neighbours, by as much as the
// neighbourhood leaves room before black or white (the room to the clip over the peak, square-rooted, as AMD's CAS):
// flat areas and strong edges are left almost alone, fine detail gains contrast without halos. Only where the image
// holds still: the strength fades out as the pixel moves (none from 2 output pixels a frame - the residual noise of
// the jittered samples it would amplify shimmers; the game's motion blur is there anyway) and with a low stability level.
// t0 = the history (u0 of the upscale; a = the stability level), t1 = the convert pass's (motion), u1 = the game's output.
#include "common.hlsli"

Texture2D<float4> History : register(t0);
Texture2D<float4> MotionDepthClip : register(t1);
VKFMT(OUTFMT) RWTexture2D<float4> Output : register(u1);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)os)) return;
    float e = encExposure();
    int2 p = int2(id), m = int2(os) - 1;
    float4 c4 = History.Load(int3(p, 0));
    float3 c = c4.rgb;
    float2 uv = (float2(id) + 0.5) * osRcp;
    float mlen = length(0.5 * MotionDepthClip.SampleLevel(linearClamp, uv, 0.0).xy * os);
    float k = 0.2 * saturate(sharpness) * (1.0 - saturate(mlen * 0.5)) * (0.5 + 0.5 * c4.a);  // (1: CAS's strongest)
    // (the filter at hf precision - half in the FP16 variant)
    hf3 ch = (hf3)c;
    hf3 n = (hf3)History.Load(int3(clamp(p + int2(0, -1), int2(0, 0), m), 0)).rgb;
    hf3 s = (hf3)History.Load(int3(clamp(p + int2(0, 1), int2(0, 0), m), 0)).rgb;
    hf3 w = (hf3)History.Load(int3(clamp(p + int2(-1, 0), int2(0, 0), m), 0)).rgb;
    hf3 ea = (hf3)History.Load(int3(clamp(p + int2(1, 0), int2(0, 0), m), 0)).rgb;
    hf3 mn = min(ch, min(min(n, s), min(w, ea))), mx = max(ch, max(max(n, s), max(w, ea)));
    hf3 amp = sqrt(saturate(min(mn, (hf)1.0 - mx) / max(mx, (hf)1e-4)));
    hf3 wgt = -amp * (hf)k;
    hf3 r = saturate((ch + wgt * (n + s + w + ea)) / ((hf)1.0 + (hf)4.0 * wgt));
    float3 o = dec((float3)r, e);
    if (indicator > 0.5 && id.x < 12 && id.y < 12) o = float3(0.0, 1.0, 0.0) / e;
    Output[id] = float4(o, 1.0);
}
