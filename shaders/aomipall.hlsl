// Ditto Relight (experimental), the depth mips 1 - 4 in one pass (ao_fuse; aomip.hlsl's, one dispatch in place
// of four, as FidelityFX SPD): each 16 x 16 group makes 16 x 16 texels of level 1 from the prepass, then 8 x 8 of level
// 2, 4 x 4 of level 3 and 2 x 2 of level 4 from them in groupshared memory. Each texel as aomip.hlsl's: one of the 4
// below it by its per-texel, per-frame hash (the sky only when all 4 are), the colour the 4's mean. A thread past a
// level's edge computes the edge texel (as aomip.hlsl's reads clamp there), and writes nothing.
// t0 = the prepass' normal and depth, t1 = the game's colour; u0 - u3 = levels 1 - 4.
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Color : register(t1);
AOFMT("rgba32ui") RWTexture2D<uint4> M1 : register(u0);
AOFMT("rgba32ui") RWTexture2D<uint4> M2 : register(u1);
AOFMT("rgba32ui") RWTexture2D<uint4> M3 : register(u2);
AOFMT("rgba32ui") RWTexture2D<uint4> M4 : register(u3);

groupshared float4 gN[16][16];
groupshared float3 gC[16][16];
groupshared float4 gN2[8][8];
groupshared float3 gC2[8][8];

uint2 dimsOf(uint L) { return ((uint2)rs + (1u << L) - 1u) >> L; }

[numthreads(16, 16, 1)]
void main(uint2 gid : SV_GroupID, uint2 lt : SV_GroupThreadID) {
    // level 1
    uint2 d1 = dimsOf(1u), t1 = gid * 16u + lt, c1 = min(t1, d1 - 1u);
    {
        uint h = c1.x * 7u + c1.y * 13u + (uint)frame * 5u + 1u;
        float4 pick = 0.0;
        [unroll] for (uint k = 0u; k < 4u; k++) {
            uint j = (h + k) & 3u;
            int2 q = min(int2(c1 * 2u + uint2(j & 1u, j >> 1u)), int2(rs) - 1);
            float4 v = flagOn(131072u) ? LOAD_ND(Nd, c1) : LOAD_ND(Nd, q);  // (131072: Nd the half size's - level 1 itself)
            if (pick.w <= 0.0 && v.w > 0.0) pick = v;
        }
        float3 col = max(Color.SampleLevel(linearClamp, float2(c1 * 2u + 1u) * rsRcp, 0.0).rgb, 0.0);
        if (all(t1 < d1)) M1[t1] = mipEncode(pick, col);
        gN[lt.y][lt.x] = pick, gC[lt.y][lt.x] = col;
    }
    GroupMemoryBarrierWithGroupSync();
    // level 2 (8 x 8 of the group's threads)
    if (all(lt < 8u)) {
        uint2 d2 = dimsOf(2u), t2 = gid * 8u + lt, c2 = min(t2, d2 - 1u), b = (c2 - gid * 8u) * 2u;
        uint h = c2.x * 7u + c2.y * 13u + (uint)frame * 5u + 2u;
        float4 pick = 0.0;
        float3 acc = 0.0;
        [unroll] for (uint k = 0u; k < 4u; k++) {
            uint j = (h + k) & 3u;
            uint2 q = b + uint2(j & 1u, j >> 1u);
            float4 v = gN[q.y][q.x];
            acc += gC[q.y][q.x];
            if (pick.w <= 0.0 && v.w > 0.0) pick = v;
        }
        if (all(t2 < d2)) M2[t2] = mipEncode(pick, acc * 0.25);
        gN2[lt.y][lt.x] = pick, gC2[lt.y][lt.x] = acc * 0.25;
    }
    GroupMemoryBarrierWithGroupSync();
    // level 3 (4 x 4), from level 2 - into gN (free now)
    if (all(lt < 4u)) {
        uint2 d3 = dimsOf(3u), t3 = gid * 4u + lt, c3 = min(t3, d3 - 1u), b = (c3 - gid * 4u) * 2u;
        uint h = c3.x * 7u + c3.y * 13u + (uint)frame * 5u + 3u;
        float4 pick = 0.0;
        float3 acc = 0.0;
        [unroll] for (uint k = 0u; k < 4u; k++) {
            uint j = (h + k) & 3u;
            uint2 q = b + uint2(j & 1u, j >> 1u);
            float4 v = gN2[q.y][q.x];
            acc += gC2[q.y][q.x];
            if (pick.w <= 0.0 && v.w > 0.0) pick = v;
        }
        if (all(t3 < d3)) M3[t3] = mipEncode(pick, acc * 0.25);
        gN[lt.y][lt.x] = pick, gC[lt.y][lt.x] = acc * 0.25;
    }
    GroupMemoryBarrierWithGroupSync();
    // level 4 (2 x 2), from level 3
    if (all(lt < 2u)) {
        uint2 d4 = dimsOf(4u), t4 = gid * 2u + lt, c4 = min(t4, d4 - 1u), b = (c4 - gid * 2u) * 2u;
        uint h = c4.x * 7u + c4.y * 13u + (uint)frame * 5u + 4u;
        float4 pick = 0.0;
        float3 acc = 0.0;
        [unroll] for (uint k = 0u; k < 4u; k++) {
            uint j = (h + k) & 3u;
            uint2 q = b + uint2(j & 1u, j >> 1u);
            float4 v = gN[q.y][q.x];
            acc += gC[q.y][q.x];
            if (pick.w <= 0.0 && v.w > 0.0) pick = v;
        }
        if (all(t4 < d4)) M4[t4] = mipEncode(pick, acc * 0.25);
    }
}
