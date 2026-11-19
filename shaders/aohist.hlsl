// Ditto Relight (experimental), the luma histogram's clear (64 x 1) before the composition counts into it (aocompose.hlsl)
// u1 = the histogram.
#include "ao_common.hlsli"

AOFMT("r32ui") RWTexture2D<uint> Hist : register(u1);

[numthreads(64, 1, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (id.x < 64u) Hist[uint2(id.x, 0u)] = 0u;
}
