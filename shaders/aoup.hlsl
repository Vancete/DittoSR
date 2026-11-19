// Ditto Relight (experimental), the half-resolution main pass back to render size (halfRes on; ao_up.hlsli's upHalf):
// each pixel from the 4 half-size texels around it, by depth and normal. With ao_up_compose (flags 8) the composition
// does it in place of this pass.
// t0 = the prepass' normal and depth, t1 = the half-size result; u0 = render size.
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Half : register(t1);
Texture2D<float> GDepth : register(t2);  // (the game's depth: flags 131072)
AOFMT("rgba16f") RWTexture2D<float4> Out : register(u0);

#include "ao_up.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rs)) return;
    float4 nd = ndAt(GDepth, int2(id));
    Out[id] = nd.w <= 0.0 ? float4(0.0, 0.0, 0.0, 1.0) : upHalf(Half, int2(id), nd);
}
