// Ditto Relight (experimental), the scene's analysis for the lighting extras (D3D12; one group, once a frame): from the
// mips (level 3's normal, depth and colour; level 4's), what the composition's extras read (ao_extra.hlsli):
//   [0] the sky's colour - the mean of level 4's texels without depth (a: how many);
//   [1] the sun's place - the brightest sky texel's uv, its luma, 1 when there is sky;
//   [2] the dominant light's direction (view space) - how the luma follows the normal over a 16 x 16 grid of level 3
//       (their covariance), a its confidence (that covariance's length over the mean luma);
//   [3] the dominant light's colour - the brightest emissive light's, else the sky's;
//   [16 - 31] the emissive lights - in each of a 4 x 4 of tiles of level 4, its brightest texel when it stands out (4 x its
//       tile's mean luma and over 0.5): its view position, a its radius (the texel's size there; 0: none);
//   [32 - 47] their colours, a their luma.
// t0 = level 3, t1 = level 4; u0 = 64 x 1.
#include "ao_common.hlsli"

Texture2D<uint4> M3 : register(t0);
Texture2D<uint4> M4 : register(t1);
AOFMT("rgba32f") RWTexture2D<float4> Scene : register(u0);

groupshared float4 gA[256], gB[256], gS[256], gT[256];

[numthreads(256, 1, 1)]
void main(uint li : SV_GroupIndex) {
    const float3 Y = float3(0.2126, 0.7152, 0.0722);
    uint2 d3 = ((uint2)rs + 7u) >> 3, d4 = ((uint2)rs + 15u) >> 4;
    // (the luma against the normal: one point of a 16 x 16 grid of level 3 each)
    {
        uint2 g = uint2(li & 15u, li >> 4);
        uint2 q = min((g * 2u + 1u) * d3 / 32u, d3 - 1u);
        float3 c;
        float4 m = mipDecode(M3.Load(int3(q, 0)), c);
        float v = m.w > 0.0 ? 1.0 : 0.0, L = dot(c, Y) * v;
        gA[li] = float4(m.xyz * L, L), gB[li] = float4(m.xyz * v, v);
    }
    // (the sky: level 4's texels without depth, a stride of them each - their colour's sum, and the brightest)
    {
        float3 ss = 0.0;
        float sn = 0.0, bl = -1.0;
        float2 buv = 0.0;
        for (uint k = li; k < d4.x * d4.y; k += 256u) {
            uint2 q = uint2(k % d4.x, k / d4.x);
            float3 c;
            float4 m = mipDecode(M4.Load(int3(q, 0)), c);
            if (m.w > 0.0) continue;
            ss += c, sn += 1.0;
            float l = dot(c, Y);
            if (l > bl) bl = l, buv = (float2(q) + 0.5) / float2(d4);
        }
        gS[li] = float4(ss, sn), gT[li] = float4(bl, buv, 0.0);
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 128u; s > 0u; s >>= 1) {
        if (li < s) {
            gA[li] += gA[li + s], gB[li] += gB[li + s], gS[li] += gS[li + s];
            if (gT[li + s].x > gT[li].x) gT[li] = gT[li + s];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    // (the emissive lights: a tile each of the first 16 threads)
    float4 lp = 0.0, lc = 0.0;
    if (li < 16u) {
        uint2 tl = uint2(li & 3u, li >> 2), t0 = tl * d4 / 4u, t1 = (tl + 1u) * d4 / 4u;
        float sum = 0.0, cnt = 0.0, bl = -1.0;
        uint2 bq = t0;
        float3 bc = 0.0;
        float bz = 0.0;
        for (uint y = t0.y; y < t1.y; y++)
            for (uint x = t0.x; x < t1.x; x++) {
                float3 c;
                float4 m = mipDecode(M4.Load(int3(x, y, 0)), c);
                if (m.w <= 0.0) continue;
                float l = dot(c, Y);
                sum += l, cnt += 1.0;
                if (l > bl) bl = l, bq = uint2(x, y), bc = c, bz = m.w;
            }
        if (cnt > 0.0 && bl > 0.5 && bl > 4.0 * sum / cnt) {
            float2 uv = (float2(bq) + 0.5) / float2(d4);
            lp = float4(viewPos(uv, bz), 16.0 * bz * 2.0 * tanY * rsRcp.y), lc = float4(bc, bl);
        }
        Scene[uint2(16u + li, 0u)] = lp, Scene[uint2(32u + li, 0u)] = lc;
    }
    if (li < 16u) gT[li + 16u] = float4(lc.w, lc.rgb);  // (each light's luma and colour, for the brightest below)
    GroupMemoryBarrierWithGroupSync();
    if (li == 0u) {
        float4 a = gA[0], b = gB[0], sk = gS[0], st = gT[0];
        float3 skyc = sk.w > 0.0 ? sk.rgb / sk.w : 0.0;
        Scene[uint2(0u, 0u)] = float4(skyc, sk.w);
        Scene[uint2(1u, 0u)] = float4(st.yz, st.x, sk.w > 0.0 ? 1.0 : 0.0);
        float meanL = b.w > 0.0 ? a.w / b.w : 0.0;
        float3 cov = b.w > 0.0 ? (a.xyz - meanL * b.xyz) / b.w : 0.0;
        float cl = length(cov);
        Scene[uint2(2u, 0u)] = float4(cl > 1e-6 ? cov / cl : float3(0.0, 1.0, 0.0), meanL > 1e-5 ? saturate(cl / meanL) : 0.0);
        float4 best = float4(-1.0, 0.0, 0.0, 0.0);
        for (uint k = 0u; k < 16u; k++) if (gT[16u + k].x > best.x) best = gT[16u + k];
        Scene[uint2(3u, 0u)] = float4(best.x > 0.0 ? best.yzw : skyc, best.x > 0.0 ? best.x : dot(skyc, Y));
    }
}
