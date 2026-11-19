// Ditto Relight (experimental), the occluder lookup table (lut on; CORTAO's occluder lookup table, built for DittoSR's own
// 32 rays - rayDir, ao_common.hlsli - once, on the GPU): for a circular occluder in tangent space, its direction (Lambert
// equal-area, 32 x 32 over the sphere) and its size (occR, sqrt-spaced: 32 steps up to 2), the bits of the
// 32 rays it blocks. The main pass then reads one texel an occluder in place of testing each ray - 32 rays for about
// the price of a few. Laid out 2D: x = direction x + 32 x size, y = direction y (1024 x 32, R32_UINT: 128 KB,
// cache-friendly).
// u0 = the table.
#include "ao_common.hlsli"

AOFMT("r32ui") RWTexture2D<uint> Lut : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (id.x >= 32u * 32u || id.y >= 32u) return;
    uint dx = id.x & 31u, ri = id.x >> 5u;
    float2 e = (float2(dx, id.y) + 0.5) / 16.0 - 1.0;  // (the cell's centre, [-1, 1])
    float s2 = dot(e, e);
    if (s2 > 1.0) { Lut[id] = 0u; return; }
    float3 hd = float3(e * 2.0 * sqrt(1.0 - s2), 1.0 - 2.0 * s2);
    float t = (float(ri) + 0.5) / 32.0, occR = 2.0 * t * t;
    float cosA = max(-1.0, 1.0 - occR * occR * 0.5);
    uint ob = 0u;
    [loop] for (uint j = 0u; j < 32u; j++) ob |= dot(rayDir(j, 32u), hd) > cosA ? (1u << j) : 0u;
    Lut[id] = ob;
}
