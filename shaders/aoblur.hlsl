// Ditto Relight (experimental), the blur's first half (render size): the main pass' occlusion and bounce light along x -
// 9 taps blurStep pixels apart (2 by default: 17 pixels wide), Gaussian (sigma 4 pixels) and weighted by depth and normal, so an edge keeps
// the two sides apart. aocompose.hlsl does the same along y: together a 17 x 17 blur for 18 taps, not 289 - the main
// pass' noise (per pixel and per frame) evened out within a frame; what is left of it, DittoSR's own accumulation takes.
// t0 = the prepass' normal and depth, t1 = the main pass'; u0 = the blurred along x.
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Res : register(t1);
RWTexture2D<float4> Out : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rs)) return;
    int2 p = int2(id);
    float4 nd = Nd.Load(int3(p, 0));
    if (nd.w <= 0.0) { Out[id] = float4(0.0, 0.0, 0.0, 1.0); return; }
    float4 r = aoBlur(Res, Nd, p, int2((int)blurStep, 0), nd);
    Out[id] = r;
}
