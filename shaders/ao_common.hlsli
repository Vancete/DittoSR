// Ditto Relight (experimental): ambient occlusion and global illumination (aoprep, aomain, aocompose - D3D12, render size),
// after CORTAO (Circular Occluder Ray-Traced Ambient Occlusion, Daniel Isheden - MIT, licenses/LICENSE-cortao.txt):
// the depth around each pixel sampled into circular occluders, a fixed set of rays across the hemisphere traced against
// them - each ray blocked or not, a bit. The occlusion: the share of rays blocked. The GI (DittoSR's extension): each
// ray takes the colour of the occluder that blocked it first - one bounce of what the screen shows, the occluders' own
// light toward the pixel. Their own root constants (dittosr.cpp AoConstants), not common.hlsli's.

cbuffer A : register(b0) {
    float2 rs;            // render size
    float2 rsRcp;
    float tanX, tanY;     // tan(fov / 2) across and up
    float zNear, zFar;    // the camera's planes (either order)
    float inverted;       // 1: reversed-Z (1 near)
    float frame;          // the frame, for the noise (0 - 1023)
    float aoStrength;     // 0 - 1: the occlusion's darkening
    float giStrength;     // 0 - 4: the bounce light added
    float radiusWorld;    // the occluders' reach, view units (ini ao_radius; 0: the screen's cap alone)
    float radiusScreen;   // ... at most this share of the screen's height (ini ao_screen)
    float rays;           // the rays a pixel without the occluder table (4 - 32: one bit each)
    float contact;        // how much more the rays the nearest occluders block darken (0 - 4)
    float giDark;         // the bounce light x this where the pixel is dark (0 - 4; 1: the same everywhere)
    float spare0;         // (free)
    uint params1;         // bits 0 - 7: the local exposure's strength (0 - 255; flags2 16384)
    float temporal;       // the history's weight (temporal smoothing, 0 - 0.95; 0: off, or no valid history)
    float spatial;        // the spatial filter's taps' spacing (1 - 2: x 2 pixels)
    float axis;           // ... its axis this pass (0 x, 1 y)
    float2 mvScale;       // the game's motion vectors -> uv (the previous uv = uv + mv x this; temporal smoothing)
    float2 mvSize;        // ... their resolution
    float giCeiling;      // the bounce light added at most this x the pixel's colour, saturating (0 - 4; 0: no ceiling)
    float spare1;         // (free)
    float mips;           // 1: the main pass' far samples from the depth mips (aomip.hlsl)
    uint params2;         // (free)
    float flags;          // bits: 1 CORTAO's backface boost (a backface occluder x its slope compensation), 2 decorrelated noise,
                          // 4 packed normal and depth, 8 the upsample in the composition, 16 the main pass writes the half
                          // size's normal and depth, 32 the history's weight by its length (D3D12); 256 AO on the ambient light
                          // alone, 512 the emitter's cosine, 2048 better normals; 16384 a wider spatial filter for the GI,
                          // 32768 a longer history for it; 131072 the prepass at half size; 262144 the curve from its table;
                          // 524288 FastGI's 8-frame noise cycle, 1048576 its GI falloff; 8388608 blue noise
                          // (D3D12)
    uint flags2;          // bits: 1 the occluder lookup table (aolut.hlsl - 32 rays); (D3D12) 2 the unified filter, 8 ReSTIR GI, 32 edge
                          // fill, 128 a-trous, 64 stochastic reflections, 256 FP16, 512 interleaved GI, 4096 cheaper far pixels;
                          //  2048 HDR, 8192 the AO as a power, 16384 the local
                          // exposure
    float cmips;          // 1: the GI's far occluders' colour from the colour mips (aocmip.hlsl)
    float samples;        // the main pass' samples a pixel (4 - 32)
    float halfRes;        // 1: the main pass at half size (the pixel at 2x, 2y of each 2 x 2) - aoup.hlsl back
    float fadeZ;          // the AO faded out from this depth to twice it (view units; 0: none)
    // the curve (aocompose.hlsl): its points (x, y, up to 8, by x) and the natural cubic spline's second derivatives;
    // curveN its points (0: none - the line)
    float4 curveX4[2];
    float4 curveY4[2];
    float4 curveD4[2];
    float curveN;
};

SamplerState linearClamp : register(s0);

// Vulkan (DITTO_VK, SPIR-V): the storage images' formats spelled out (as common.hlsli's VKFMT)
#if DITTO_VK
#define AOFMT(f) [[vk::image_format(f)]]
#else
#define AOFMT(f)
#endif

// the linear depth (view z) of a depth buffer value; 0: the sky (the far plane)
float linDepth(float d) {
    float n = min(zNear, zFar), f = max(zNear, zFar);
    if (inverted > 0.5) return d <= 0.0 ? 0.0 : n * f / (n + d * (f - n));
    return d >= 1.0 ? 0.0 : n * f / (f - d * (f - n));
}

// view space: x right, y up, z forward (the camera at 0)
float3 viewPos(float2 uv, float z) { return float3((uv.x * 2.0 - 1.0) * tanX * z, (1.0 - uv.y * 2.0) * tanY * z, z); }
float2 projUv(float3 p) { return float2(0.5 + 0.5 * p.x / (p.z * tanX), 0.5 - 0.5 * p.y / (p.z * tanY)); }

// an orthonormal tangent and bitangent around n
void basisOf(float3 n, out float3 t, out float3 b) {
    float3 h = abs(n.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    t = normalize(cross(h, n));
    b = cross(n, t);
}

// CORTAO's filter weight: the tap's depth dq against the centre's plane (c its view position, nc its normal) at the
// tap's uv - its error relative to the centre's depth x 512 (CORTAO's depth sensitivity) - and its normal's (|dn|^2 x 4:
// ~ CORTAO's 16 / 32 on its spheremap), exp2(-error)
float planeWeight(float3 c, float3 nc, float2 uvq, float dq, float3 nq) {
    float3 r = viewPos(uvq, 1.0);
    float dn = dot(nc, r), ze = abs(dn) > 1e-3 ? dot(nc, c) / dn : dq;
    float3 d = nc - nq;
    return exp2(-(512.0 * abs(dq - ze) / c.z + 4.0 * dot(d, d)));
}

// the filter's taps' weight (FastGI's GeometryWeight): a tap's depth within max(0.02, 1.5 %) of the centre's, its normal's closeness (cos^4); 0 the sky
float geoWeight(float dc, float dt, float3 nc, float3 nt) {
    float wd = saturate(1.0 - abs(dc - dt) / max(0.02, dc * 0.015));
    float wn = saturate(dot(nc, nt));
    wn *= wn;
    return dt > 0.00001 ? wd * wn * wn : 0.0;
}

// the rays: a golden spiral on the unit disc, lifted onto the hemisphere (cosine-distributed), ray j of nr
static const float GOLDEN = 2.39996323;
float3 rayDir(uint j, uint nr) {
    float a = float(j) * GOLDEN, r = sqrt((float(j) + 0.5) / float(nr));
    return float3(cos(a) * r, sin(a) * r, sqrt(1.0 - r * r));
}

// the depth mips' texels (aomip.hlsl; RGBA32_UINT): x the normal's x, y (halves), y the depth, z / w the colour's mean
// (halves) and, w's bit 16, the normal's z positive
uint4 mipEncode(float4 nd, float3 c) {
    return uint4(f32tof16(nd.x) | (f32tof16(nd.y) << 16), asuint(nd.w), f32tof16(c.r) | (f32tof16(c.g) << 16), f32tof16(c.b) | (nd.z > 0.0 ? 0x10000u : 0u));
}
float4 mipDecode(uint4 m, out float3 c) {
    float2 n = float2(f16tof32(m.x & 0xffffu), f16tof32(m.x >> 16));
    c = float3(f16tof32(m.z & 0xffffu), f16tof32(m.z >> 16), f16tof32(m.w & 0xffffu));
    return float4(n, ((m.w & 0x10000u) ? 1.0 : -1.0) * sqrt(saturate(1.0 - dot(n, n))), asfloat(m.y));
}

// the curve at t (the spline over [x0, xn-1]; flat before and after its ends, as an editor's) and past 1 (linear HDR
// above the editor's range) the curve's end + (t - 1)
#define CRV_X(i) curveX4[(i) >> 2][(i) & 3]
#define CRV_Y(i) curveY4[(i) >> 2][(i) & 3]
#define CRV_D(i) curveD4[(i) >> 2][(i) & 3]
float curveIn(float t) {
    uint n = (uint)curveN;
    if (t <= CRV_X(0)) return CRV_Y(0);
    if (t >= CRV_X(n - 1u)) return CRV_Y(n - 1u);
    uint k = 0u;
    [loop] for (uint i = 1u; i + 1u < n; i++) if (t >= CRV_X(i)) k = i;
    float x0 = CRV_X(k), x1 = CRV_X(k + 1u), h = max(x1 - x0, 1e-5);
    float a = (x1 - t) / h, b = (t - x0) / h;
    float y = a * CRV_Y(k) + b * CRV_Y(k + 1u) + ((a * a * a - a) * CRV_D(k) + (b * b * b - b) * CRV_D(k + 1u)) * h * h / 6.0;
    return saturate(y);
}
float curveAt(float t) { return t > 1.0 ? curveIn(1.0) + (t - 1.0) : curveIn(t); }

bool flagOn(uint b) { return ((uint)flags & b) != 0u; }
bool flag2On(uint b) { return (flags2 & b) != 0u; }
// (flags2 2048: the colour linear HDR - the curve's pivot 30 % of the brightness: 0.071 (0.3 ^ 2.2), else 0.3; 8192: the
// occlusion applied as pow(occlusion, aoStrength), not lerp(1, occlusion, aoStrength))
#define pivot (flag2On(2048u) ? 0.071 : 0.3)

// the packed normal and depth (flags 4, ao_pack_nd): RG32_UINT - x the normal, octahedral, 2 x 16-bit snorm; y the linear
// depth's bits - 8 bytes a texel in place of RGBA32F's 16, read at t7 by every pass that reads the normal and depth
Texture2D<uint2> NdP : register(t7);
uint2 ndPack(float4 nd) {
    float s = abs(nd.x) + abs(nd.y) + abs(nd.z);
    float3 n = s > 0.0 ? nd.xyz / s : float3(0.0, 0.0, 1.0);
    float2 e = n.xy;
    if (n.z < 0.0) e = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    int2 q = int2(round(clamp(e, -1.0, 1.0) * 32767.0));
    return uint2(((uint)q.x & 0xffffu) | ((uint)q.y << 16), asuint(nd.w));
}
float4 ndUnpack(uint2 v) {
    float2 e = max(float2(int2((int)(v.x << 16), (int)v.x) >> 16) / 32767.0, -1.0);
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.xy += float2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return float4(normalize(n), asfloat(v.y));
}
// (a pass' normal and depth at p: tex - its RGBA32F one -, or the packed one at t7)
#define LOAD_ND(tex, p) (flagOn(4u) ? ndUnpack(NdP.Load(int3((p), 0))) : tex.Load(int3((p), 0)))


// the temporal smoothing's depth with its history's length in the 5 low bits (aotemporal.hlsl; 0 - 31)
float hdPack(float d, uint c) { return asfloat((asuint(d) & ~31u) | min(c, 31u)); }
uint hdCount(float v) { return asuint(v) & 31u; }

// interleaved gradient noise (Jimenez 2014)
// (a PCG step: the next of a stream of uniform [0, 1) numbers)
float rndNext(inout uint s) {
    s = s * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return float((w >> 22u) ^ w) * (1.0 / 4294967296.0);
}
float ign(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }
