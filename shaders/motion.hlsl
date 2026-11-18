// DittoSR's motion pass: the motion checks and the derived reactive mask. Render size - once per render pixel, not per output pixel
// (the upscale did them per output pixel: 8 reads of the motion at each, ratio^2 times over the same render pixels).
// u0 (RGBA8): x = the motion's change against its neighbours' (rj), y = the disocclusion, z = the
// derived reactive mask, w = this pixel's encoded luma - the next frame's reactive test reads it where the pixel went.
// t0 = the previous frame's motion pass, t1 = the convert pass's (this frame), t2 = the colour, t3 = the previous convert pass's.
#include "common.hlsli"

Texture2D<float4> PrevMotion : register(t0);
Texture2D<float4> Mda : register(t1);
Texture2D<float4> Color : register(t2);
Texture2D<float4> PrevMda : register(t3);
VKFMT("rgba8") RWTexture2D<float4> Out : register(u0);
Texture2D<float2> PrevEnv : register(t4);   // (the luma envelope where the pixel was: x its min, y its max)
VKFMT("rg8") RWTexture2D<float2> EnvOut : register(u1);

#define MC_MDA Mda
#define MC_PREVMDA PrevMda
#define MC_PREVMOTION PrevMotion
#define MC_PREVENV PrevEnv
#include "motion_core.hlsli"

groupshared float gsLuma[100];  // (the group's 8 x 8 and a sample around: each encoded once, not by its 9 neighbours)

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID, uint2 gid : SV_GroupID, uint li : SV_GroupIndex) {
    float e = encExposure();
    int2 p = int2(id), cs = int2(rs) - 1;
    // the colour's luma here and the 3x3's spread (the reactive test and the envelope - only when one is on)
    bool lumas = autoReactive > 0.0 || envGain > 0.0;
    if (lumas) {
        for (int i = (int)li; i < 100; i += 64) {
            int2 q = clamp(int2(gid * 8) - 1 + int2(i % 10, i / 10), int2(0, 0), cs);
            gsLuma[i] = dot(enc(Color.Load(int3(q, 0)).rgb, e), float3(0.25, 0.5, 0.25));
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (any(id >= (uint2)rs)) return;
    float lc = 0.0, lmn = 1.0, lmx = 0.0;
    if (lumas) {
        int2 o = p - int2(gid * 8);
        [unroll] for (int i = 0; i < 9; i++) {
            float l = gsLuma[(o.y + i / 3) * 10 + o.x + i % 3];
            lmn = min(lmn, l), lmx = max(lmx, l);
            if (i == 4) lc = l;
        }
    }
    float4 ax;
    float2 env;
    motionCore((float2(id) + 0.5) * rsRcp, Mda.Load(int3(p, 0)), lc, lmn, lmx, ax, env);
    EnvOut[id] = env;
    Out[id] = ax;
}
