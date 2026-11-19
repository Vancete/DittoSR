// DittoSR's experimental AO / GI, the prepass (render size): each pixel's view-space normal, rebuilt from the depth
// (the game gives none - on each axis the neighbour nearer in depth, so an edge doesn't bend it), and its linear depth.
// t0 = the game's depth, u0 = xyz the normal (toward the camera), w the linear depth (0: the sky).
#include "ao_common.hlsli"

Texture2D<float> Depth : register(t0);
AOFMT("rgba32f") RWTexture2D<float4> Nd : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rs)) return;
    int2 p = int2(id), m = int2(rs) - 1;
    float z = linDepth(Depth.Load(int3(p, 0)));
    if (z <= 0.0) { Nd[id] = 0.0; return; }
    float2 uv = (float2(id) + 0.5) * rsRcp;
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
        float3 dx = useR ? viewPos(uv + float2(rsRcp.x, 0.0), zr) - P : P - viewPos(uv - float2(rsRcp.x, 0.0), zl);
        float3 dy = useD ? viewPos(uv + float2(0.0, rsRcp.y), zd) - P : P - viewPos(uv - float2(0.0, rsRcp.y), zu);
        float3 c = cross(dx, dy);
        if (dot(c, c) > 1e-20) {
            c = normalize(c);
            n = dot(c, -P) < 0.0 ? -c : c;
        }
    }
    Nd[id] = float4(n, z);
}
