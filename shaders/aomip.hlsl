// Ditto Relight (experimental), the depth mips (mips on; CORTAO's prepass mips): level axis (1 - 4) of the prepass'
// normal and depth, each texel one of the 4 below it - not an average (an average depth is a surface that isn't
// there): which one by a per-texel, per-frame hash (the next frame another), the sky taken only when all 4 are. With
// them, packed (mipEncode), the game's colour's mean over the texel's area (the 4 below it averaged) - the colour mips:
// the GI's far occluders take their area's colour (cmips), read with their depth. The main pass reads its far samples
// from them (fewer cache misses, a steadier pick).
// t0 = the prepass' normal and depth (level 1), t1 = the game's colour (level 1), t2 = the level below (2 - 4);
// u0 = this level.
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Color : register(t1);
Texture2D<uint4> Below : register(t2);
AOFMT("rgba32ui") RWTexture2D<uint4> Dst : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    uint L = (uint)axis;
    uint2 dims = ((uint2)rs + (1u << L) - 1u) >> L, sdims = ((uint2)rs + (1u << (L - 1u)) - 1u) >> (L - 1u);
    if (any(id >= dims)) return;
    uint h = id.x * 7u + id.y * 13u + (uint)frame * 5u + L;
    float4 pick = 0.0;
    float3 acc = 0.0;
    [unroll] for (uint k = 0u; k < 4u; k++) {
        uint j = (h + k) & 3u;
        int2 q = min(int2(id * 2u + uint2(j & 1u, j >> 1u)), int2(sdims) - 1);
        float4 v;
        if (L == 1u) {
            v = flagOn(131072u) ? LOAD_ND(Nd, id) : LOAD_ND(Nd, q);  // (131072: Nd the half size's - level 1 itself)
        } else {
            float3 c;
            v = mipDecode(Below.Load(int3(q, 0)), c);
            acc += c;
        }
        if (pick.w <= 0.0 && v.w > 0.0) pick = v;
    }
    // (level 1: the 2 x 2's mean in one bilinear tap between its pixels)
    float3 col = L == 1u ? Color.SampleLevel(linearClamp, float2(id * 2u + 1u) * rsRcp, 0.0).rgb : acc * 0.25;
    Dst[id] = mipEncode(pick, max(col, 0.0));
}
