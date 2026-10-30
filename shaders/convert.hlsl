// DittoSR's convert pass: Snapdragon GSR 2's (BSD 3-Clause, licenses/LICENSE-snapdragon-gsr.txt) depth
// clip and the motion of the nearest of the 3x3. Render size. u0 (RGBA16F): xy = the motion in NDC (2 x (uv - the
// previous uv)), z = depth clip (how far this pixel's depth stands out from its 4x4 neighbourhood's), w = the nearest
// of the 3x3's nearness (1 - depth: reversed-Z's own value, ~ 1 / distance, precise in half floats where it is small) -
// the next frame's disocclusion test reads it where the pixel went.
// t0 = depth (1: far once un-inverted), t1 = the game's motion vectors.
#include "common.hlsli"

Texture2D<float> InputDepth : register(t0);
Texture2D<float2> InputVelocity : register(t1);
VKFMT("rgba16f") RWTexture2D<float4> Out : register(u0);

float depthAt(int2 p, int2 ds) {
    float d = InputDepth.Load(int3(clamp(p, int2(0, 0), ds), 0));
    return depthInverted > 0.5 ? 1.0 - d : d;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rs)) return;
    float2 dsz;
    InputDepth.GetDimensions(dsz.x, dsz.y);
    float2 fc = float2(id) + 0.5;
    float2 texCoord = fc * rsRcp;
    // (the gather between texels id - 1 and id, in the depth texture's own uv: it may be larger than the render size)
    float2 gatherCoord = float2(id) / dsz, step2 = 2.0 / dsz;
    float4 btmLeft = InputDepth.GatherRed(linearClamp, gatherCoord);
    float4 btmRight = InputDepth.GatherRed(linearClamp, gatherCoord + float2(step2.x, 0.0));
    float4 topLeft = InputDepth.GatherRed(linearClamp, gatherCoord + float2(0.0, step2.y));
    float4 topRight = InputDepth.GatherRed(linearClamp, gatherCoord + step2);
    if (depthInverted > 0.5) btmLeft = 1.0 - btmLeft, btmRight = 1.0 - btmRight, topLeft = 1.0 - topLeft, topRight = 1.0 - topRight;
    float maxC = min(min(min(btmLeft.z, btmRight.w), topLeft.y), topRight.x);
    float btmLeft4 = min(min(min(btmLeft.y, btmLeft.x), btmLeft.z), btmLeft.w);
    float depthclip = 0.0;
    if (maxC < 1.0 - 1.0e-05) {
        float btmRight4 = min(min(min(btmRight.y, btmRight.x), btmRight.z), btmRight.w);
        float topLeft4 = min(min(min(topLeft.y, topLeft.x), topLeft.z), topLeft.w);
        float topRight4 = min(min(min(topRight.y, topRight.x), topRight.z), topRight.w);
        float Ksep_Kfov_diagonal = 1.37e-05 * tanHalfFovH * length(rs);
        float Depthsep = Ksep_Kfov_diagonal * (1.0 - maxC);
        float Wdepth = saturate(Depthsep / (abs(maxC - btmLeft4) + 1.19e-07))
                     + saturate(Depthsep / (abs(maxC - btmRight4) + 1.19e-07))
                     + saturate(Depthsep / (abs(maxC - topLeft4) + 1.19e-07))
                     + saturate(Depthsep / (abs(maxC - topRight4) + 1.19e-07));
        depthclip = saturate(1.0 - Wdepth * 0.25);
    }
    // (the nearest of the 3x3: a moving silhouette's own motion over the background around it)
    int2 ms = int2(mvSize);
    int2 mp = clamp(int2(texCoord * mvSize), int2(0, 0), ms - 1);
    int2 dp = int2(id), ds = int2(rs) - 1, best = int2(0, 0);
    float dmin = depthAt(dp, ds);  // (the centre unless a neighbour is strictly nearer)
    for (int i = 0; i < 9; i++) {
        int2 q = int2(i % 3 - 1, i / 3 - 1);
        float dd = depthAt(dp + q, ds);
        if (dd < dmin) { dmin = dd; best = q; }
    }
    // (FSR: the previous uv = uv + mv x scale / size; ours: uv - the previous uv)
    // (the offset in the vectors' own texels: they may be at the output resolution)
    int2 mb = int2(round(float2(best) * mvSize * rsRcp));
    float2 mv = InputVelocity.Load(int3(clamp(mp + mb, int2(0, 0), ms - 1), 0));
    float2 v = -(mv * mvScale - jitCancel);
    Out[id] = float4(2.0 * v, depthclip, 1.0 - dmin);
}
