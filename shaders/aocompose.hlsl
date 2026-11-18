// DittoSR's experimental AO / GI, the composition (render size): the main pass' occlusion and bounce light through a
// sparse 3 x 3 (2 pixels apart) weighted by depth and normal - CORTAO's prefilter idea: not to clean the noise, only to
// make it something the temporal accumulation that follows can average - then onto the game's colour, which every
// later pass reads in its place: colour x lerp(1, occlusion, aoStrength) + giStrength x albedo x bounce light. The
// albedo, unknown (the colour is albedo x light): the colour's hue at 0.6 of its brightest channel, grey 0.5 where dark.
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
    float3 col = max(c.rgb, 0.0);
    float mx = max(col.r, max(col.g, col.b));
    float3 albedo = lerp((float3)0.5, 0.6 * col / max(mx, 1e-4), saturate(mx * 8.0));
    Out[id] = float4(col * lerp(1.0, r.a, aoStrength) + giStrength * albedo * r.rgb, c.a);
}
