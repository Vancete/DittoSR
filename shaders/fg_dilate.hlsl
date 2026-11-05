// DittoSR's frame generation: the field dilated - xy = the vector of the nearest of each pixel's 3 x 3 (a moving
// object keeps its edges), zw = the farthest's (the background's: what an uncovered pixel moves with).
// t0 = the field, u0 = the dilated field (RGBA16F). Render size.
#include "fg_common.hlsli"

Texture2D<float4> Field : register(t0);
RWTexture2D<float4> Dilated : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)size)) return;
    int2 p = int2(id), fs = int2(size) - 1;
    float ln = 1e30, lf = -1.0;
    float2 fg = 0.0, bg = 0.0;
    [unroll] for (int y = -1; y <= 1; y++)
        [unroll] for (int x = -1; x <= 1; x++) {
            float4 f = Field.Load(int3(clamp(p + int2(x, y), int2(0, 0), fs), 0));
            if (f.z < ln) { ln = f.z; fg = f.xy; }
            if (f.z > lf) { lf = f.z; bg = f.xy; }
        }
    Dilated[id] = float4(fg, bg);
}
