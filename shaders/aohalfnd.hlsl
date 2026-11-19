// Ditto Relight (experimental), the prepass' normal and depth at half size (half resolution, FastGI's): each texel the
// render pixel the half-size main pass computed for it (2x, 2y: a fixed grid, the same every frame) - the filter and the
// temporal smoothing then run at half size on it, as FastGI's.
// t0 = the prepass' normal and depth (render size); u0 = half size.
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
AOFMT("rgba32f") RWTexture2D<float4> Out : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    uint2 hd = ((uint2)rs + 1u) >> 1;
    if (any(id >= hd)) return;
    Out[id] = Nd.Load(int3(min(id * 2u, (uint2)rs - 1u), 0));
}
