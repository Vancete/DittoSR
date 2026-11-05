// DittoSR's frame generation: this frame's motion field (fg_common.hlsli), at the render size, from the game's depth
// and motion vectors - run with the FG prepare dispatch, on the game's command list. t0 = depth, t1 = motion vectors,
// u0 = the field (RGBA16F).
#include "fg_common.hlsli"

Texture2D<float> Depth : register(t0);
Texture2D<float2> Motion : register(t1);
RWTexture2D<float4> Field : register(u0);

// the distance from the depth buffer's value (any of the four conventions: only ratios of it are used)
float linearDepth(float d) {
    if (depthInv > 0.5) return depthInf > 0.5 ? nearZ / max(d, 1e-7) : nearZ * farZ / max(nearZ + d * (farZ - nearZ), 1e-7);
    return depthInf > 0.5 ? nearZ / max(1.0 - d, 1e-7) : nearZ * farZ / max(farZ - d * (farZ - nearZ), 1e-7);
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)size)) return;
    float2 uv = (float2(id) + 0.5) * sizeRcp;
    int2 ms = int2(mvRes);
    float2 mv = Motion.Load(int3(clamp(int2(uv * mvRes), int2(0, 0), ms - 1), 0));
    // (FSR's: the previous uv = uv + mv x scale; the field's: uv - the previous uv)
    float2 v = -(mv * mvToUv) + jitCancel;
    float l = linearDepth(Depth.Load(int3(id, 0)));
    Field[id] = float4(v, max(l, 1e-4), 1.0);
}
