// DittoSR's experimental AO / GI, the prepass (render size): each pixel's view-space normal, rebuilt from the depth
// (the game gives none - on each axis the neighbour nearer in depth, so an edge doesn't bend it), and its linear depth.
// t0 = the game's depth, u0 = xyz the normal (toward the camera), w the linear depth (0: the sky); u1 = packed (flags 4).
#include "ao_common.hlsli"

Texture2D<float> Depth : register(t0);
AOFMT("rgba32f") RWTexture2D<float4> Nd : register(u0);
AOFMT("rg32ui") RWTexture2D<uint2> NdPOut : register(u1);  // (packed: flags 4)

void store(uint2 id, float4 v) {
    if (flagOn(4u)) NdPOut[id] = ndPack(v);
    else Nd[id] = v;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    // (flags 131072: at half size - the pixel of each 2 x 2 the half-size passes take, into the half size's texture)
    bool hp = flagOn(131072u);
    if (any(id >= (hp ? ((uint2)rs + 1u) >> 1 : (uint2)rs))) return;
    int2 p = hp ? int2(min(id * 2u, (uint2)rs - 1u)) : int2(id), m = int2(rs) - 1;
    float z = linDepth(Depth.Load(int3(p, 0)));
    if (z <= 0.0) { store(id, 0.0); return; }
    float2 uv = (float2(p) + 0.5) * rsRcp;
    float3 P = viewPos(uv, z);
    float zl = linDepth(Depth.Load(int3(clamp(p + int2(-1, 0), int2(0, 0), m), 0)));
    float zr = linDepth(Depth.Load(int3(clamp(p + int2(1, 0), int2(0, 0), m), 0)));
    float zu = linDepth(Depth.Load(int3(clamp(p + int2(0, -1), int2(0, 0), m), 0)));
    float zd = linDepth(Depth.Load(int3(clamp(p + int2(0, 1), int2(0, 0), m), 0)));
    float3 n = -normalize(P);
    bool hx = zl > 0.0 || zr > 0.0, hy = zu > 0.0 || zd > 0.0;
    if (hx && hy) {
        bool useR = zr > 0.0 && (zl <= 0.0 || abs(zr - z) < abs(z - zl));
        bool useD = zd > 0.0 && (zu <= 0.0 || abs(zd - z) < abs(z - zu));
        if (flagOn(2048u)) {
            // (better normals, Turanszki's: on each axis the side whose 2 pixels extrapolate to this one's depth best -
            // the side on this pixel's surface, also next to an edge where the nearer neighbour is across it)
            float zl2 = linDepth(Depth.Load(int3(clamp(p + int2(-2, 0), int2(0, 0), m), 0)));
            float zr2 = linDepth(Depth.Load(int3(clamp(p + int2(2, 0), int2(0, 0), m), 0)));
            float zu2 = linDepth(Depth.Load(int3(clamp(p + int2(0, -2), int2(0, 0), m), 0)));
            float zd2 = linDepth(Depth.Load(int3(clamp(p + int2(0, 2), int2(0, 0), m), 0)));
            float el = zl > 0.0 && zl2 > 0.0 ? abs(2.0 * zl - zl2 - z) : 1e30, er = zr > 0.0 && zr2 > 0.0 ? abs(2.0 * zr - zr2 - z) : 1e30;
            float eu = zu > 0.0 && zu2 > 0.0 ? abs(2.0 * zu - zu2 - z) : 1e30, ed = zd > 0.0 && zd2 > 0.0 ? abs(2.0 * zd - zd2 - z) : 1e30;
            if (el < 1e29 || er < 1e29) useR = er < el;
            if (eu < 1e29 || ed < 1e29) useD = ed < eu;
        }
        float3 dx = useR ? viewPos(uv + float2(rsRcp.x, 0.0), zr) - P : P - viewPos(uv - float2(rsRcp.x, 0.0), zl);
        float3 dy = useD ? viewPos(uv + float2(0.0, rsRcp.y), zd) - P : P - viewPos(uv - float2(0.0, rsRcp.y), zu);
        float3 c = cross(dx, dy);
        if (dot(c, c) > 1e-20) {
            c = normalize(c);
            n = dot(c, -P) < 0.0 ? -c : c;
        }
    }
    store(id, float4(n, z));
}
