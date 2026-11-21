// Ditto Relight (experimental), the area's luma for the better albedo and the local exposure (D3D12; the mips' level 3 -
// one texel each, 8 x 8 pixels): each texel's luma blurred over its 3 x 3 neighbours on its own surface - a Gaussian by
// distance x how near each one's depth is to its own, each neighbour's luma at most 4 x its own (a light bar beside a
// wall no longer brightens the wall's area) -, and its depth. The composition reads it bilinearly, each of the 4 x how
// near its depth is to the pixel's (aocompose.hlsl): smooth across the texels' borders - no blocks in motion.
// t0 = level 3; u0 = float2(the area's luma, depth; 0: sky), level 3's size.
#include "ao_common.hlsli"

Texture2D<uint4> M3 : register(t0);
AOFMT("rg32f") RWTexture2D<float2> Local : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    uint2 d3 = ((uint2)rs + 7u) >> 3;
    if (any(id >= d3)) return;
    const float3 Y = float3(0.2126, 0.7152, 0.0722);
    float3 c;
    float4 m0 = mipDecode(M3.Load(int3(id, 0)), c);
    float l0 = dot(c, Y), z = m0.w;
    if (z <= 0.0) { Local[id] = float2(l0, 0.0); return; }
    float la = 0.0, wa = 0.0;
    [unroll] for (int k = 0; k < 9; k++) {
        int2 o = int2(k % 3 - 1, k / 3 - 1);
        int2 q = clamp(int2(id) + o, int2(0, 0), int2(d3) - 1);
        float3 cq;
        float4 m = mipDecode(M3.Load(int3(q, 0)), cq);
        // (x e^(-(stops from its own luma)^2 / 2): a light bar's texels left out of the wall's beside them)
        float lq = dot(cq, Y), st = log2(max(lq, 1e-5) / max(l0, 1e-5));
        float w = exp(-dot(float2(o), float2(o)) * 0.7 - 0.5 * st * st) * (m.w > 0.0 ? saturate(1.0 - abs(m.w - z) / (z * 0.05 + 1e-3)) : 0.0);
        la += min(lq, 4.0 * l0 + 1e-3) * w, wa += w;
    }
    Local[id] = float2(wa > 1e-3 ? la / wa : l0, z);
}
