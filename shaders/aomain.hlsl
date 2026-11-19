// DittoSR's experimental AO / GI, the main pass (render size, or half: halfRes) - CORTAO's (MIT,
// licenses/LICENSE-cortao.txt) main pass, plus the GI:
//   `samples` (4 - 32) samples on a golden-angle spiral over a disc around the pixel, facing the camera (its radius: the
// occluders' reach in view space, capped to a share of the screen), rotated and dithered by a per-frame noise -
// DittoSR's accumulation averages it. Each sample within reach and above the pixel's surface is a circular occluder:
// its size by the sample's share of the disc and its slope to the camera (CORTAO's slope compensation), moved one
// radius into its surface when it faces the pixel (no self-occlusion), left as it is when it faces away (a backface;
// flags 1, backface boost: x its slope again). Rays over the hemisphere (cosine-distributed: a golden spiral on the unit disc,
// lifted) - a ray inside an occluder's angular size is blocked: 4 - 32 tested one by one, or 32 from CORTAO's occluder
// lookup table (lut: aolut.hlsl). The occlusion: 1 - the share of rays blocked.
//   mips: a sample's normal and depth from the depth mips (aomip.hlsl) by its distance (log2 of its pixels - 4, CORTAO's
// mip bias); cmips: its colour too, the mips' mean of its area (packed with them) - flags 4: the two levels around its
// distance blended (no steps where the level changes).
//   The GI: a ray blocked for the first time takes the colour of the occluder that blocks it (nearest first: the samples
// go outward), unless it is a backface (its lit side faces away). The rays being cosine-distributed, their plain mean
// is the irradiance / pi - the light bounced off what surrounds the pixel, as far as the screen shows it. flags 2: each
// occluder's light x (1 - (d / reach)^2)^2 - fading out toward the reach, not cut there (CORTAO has no falloff: right
// for its occlusion, but a thin bright light then lights everything within the reach alike).
//   The contact term: the rays blocked first by an occluder near the pixel (closer than half the reach, more the
// closer) darken more, x contact - a tyre on the road, a box against a wall: what a ray's yes / no leaves as faint as a
// distant wall's shade. The occlusion x (1 - contact x that share).
//   halfRes: one pixel of each 2 x 2 (halfOff, another each frame - the temporal smoothing and DittoSR's accumulation
// fill in the others), into a half-size target; aoup.hlsl brings it back to render size by depth and normal.
// t0 = the prepass' normal and depth, t1 = the game's colour, t2 - t5 the depth mips 1 - 4, t6 the lookup table;
// u0 = rgb the bounce light, a the occlusion.
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Color : register(t1);
Texture2D<uint4> Mip1 : register(t2);
Texture2D<uint4> Mip2 : register(t3);
Texture2D<uint4> Mip3 : register(t4);
Texture2D<uint4> Mip4 : register(t5);
Texture2D<uint> Lut : register(t6);
AOFMT("rgba16f") RWTexture2D<float4> Res : register(u0);

#ifndef AO_WG
#define AO_WG 8  // (the group's side: 16 the 16 x 16 variant, aomain16)
#endif

// the rays an occluder blocks, from the lookup table (32 x 32 directions x 32 sizes): hd its direction in tangent
// space, occR its size
uint lutBits(float3 hd, float occR) {
    if (hd.z <= -0.999) return 0u;
    float2 e = hd.xy * rsqrt(2.0 * (1.0 + hd.z));
    int2 c = clamp(int2((e * 0.5 + 0.5) * 32.0), int2(0, 0), int2(31, 31));
    int ri = clamp(int(sqrt(saturate(occR * 0.5)) * 32.0), 0, 31);
    return Lut.Load(int3(c.x + 32 * ri, c.y, 0));
}

// a sample's normal and depth at suv, from level L (0: the prepass'), the uv of the texel read, and - from a mip -
// its area's mean colour (hasCol)
float4 loadNd(int L, float2 suv, out float2 cuv, out float3 col, out bool hasCol) {
    uint2 dims = ((uint2)rs + (1u << L) - 1u) >> L;
    int2 t = min(int2(suv * float2(dims)), int2(dims) - 1);
    cuv = (float2(t) + 0.5) / float2(dims);
    col = 0.0, hasCol = false;
    if (L == 0) return Nd.Load(int3(t, 0));
    uint4 m = L == 1 ? Mip1.Load(int3(t, 0)) : L == 2 ? Mip2.Load(int3(t, 0)) : L == 3 ? Mip3.Load(int3(t, 0)) : Mip4.Load(int3(t, 0));
    hasCol = true;
    return mipDecode(m, col);
}

[numthreads(AO_WG, AO_WG, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    uint2 hdims = halfRes > 0.5 ? ((uint2)rs + 1u) >> 1 : (uint2)rs;
    if (any(id >= hdims)) return;
    uint hk = (uint)halfOff;
    uint2 pix = halfRes > 0.5 ? min(id * 2u + uint2(hk & 1u, hk >> 1u), (uint2)rs - 1u) : id;
    float4 nd = Nd.Load(int3(pix, 0));
    float z = nd.w;
    if (z <= 0.0) { Res[id] = float4(0.0, 0.0, 0.0, 1.0); return; }
    float3 n = nd.xyz;
    uint nr = lut > 0.5 ? 32u : clamp((uint)rays, 4u, 32u);
    uint ns = clamp((uint)samples, 4u, 32u);
    float2 uv = (float2(pix) + 0.5) * rsRcp;
    float3 P = viewPos(uv, z), V = -normalize(P);
    // the reach: the world radius, capped to radiusScreen of the screen's height at this depth (CORTAO's apparent
    // circle: the disc that covers the whole sphere seen from the camera)
    float ms = tanY * radiusScreen * 2.0, coef = ms * rsqrt(ms * ms + 1.0);
    float R = min(radiusWorld > 0.0 ? radiusWorld : 1e20, z * coef);
    float circle = R * rsqrt(max(z * z - R * R, 1e-12)) * z;
    // (the pixel pushed off its surface by about a pixel: no self-occlusion)
    P += n * (1.41421356 * tanX * rsRcp.x * z);
    // the noise: the spiral's rotation and radial dither, the rays' rotation - per pixel and per frame (a fixed pattern
    // streaked: neighbours' samples on the same texels of a bright strip, in blocks); the filters and DittoSR's accumulation
    // even it out
    float2 fp = float2(pix) + 5.588238 * frame;
    float rot = 6.2831853 * ign(fp), dith = ign(fp.yx + 17.0), rrot = 6.2831853 * ign(fp + float2(37.0, 11.0));
    float mir = ign(fp + 71.0);
    if (flagOn(2u)) {
        // (decorrelated noise: each dimension its own IGN, moved each frame along its own direction - its phase a
        // different step a frame; they shared one (the swapped coordinates, the same offset), so a pixel only ever saw
        // one curve through their combinations and its mean settled on a fixed pattern. Each still IGN moved in the
        // image, not one value stepped for every pixel at once: what changes from a frame to the next stays fine-grained,
        // for the filters - an R-sequence's step, the same everywhere, flickered in large patches)
        float2 q = float2(pix), f = (float2)frame;
        dith = ign(q + float2(17.0, 59.0) + f * float2(3.1, 9.7));
        rrot = 6.2831853 * ign(q + float2(37.0, 11.0) + f * float2(7.9, 1.3));
        mir = ign(q + 71.0 + f * float2(1.7, 13.3));
    }
    float2 sr = float2(cos(rot), sin(rot)) * circle;
    float3 right, up;
    basisOf(V, right, up);
    if (mir < 0.5) right = -right;
    float3 t0, b0;
    basisOf(n, t0, b0);
    float cr = cos(rrot), sn = sin(rrot);
    float3 tg = cr * t0 + sn * b0, bt = cr * b0 - sn * t0;
    uint full = 0xffffffffu >> (32u - nr);  // (no shift by 32: undefined in DXIL - it broke the loop early)
    uint bits = 0u;
    float3 gi = 0.0;
    float near = 0.0;
    [loop] for (uint i = 0u; i < ns; i++) {
        float a = float(i) * GOLDEN;
        float2 sp = float2(cos(a), sin(a));
        float inner = pow(float(i) / float(ns), 1.75), outer = pow(float(i + 1u) / float(ns), 1.75);
        float w = sqrt(outer * outer - inner * inner);
        float2 o = sp * lerp(inner, outer, dith);
        o = float2(o.x * sr.x - o.y * sr.y, o.x * sr.y + o.y * sr.x);
        float3 Q = P + right * o.x + up * o.y;
        if (Q.z <= 0.0) continue;
        float2 suv = projUv(Q);
        if (any(suv < 0.0) || any(suv > 1.0)) continue;
        float2 cuv;
        float3 mcol;
        bool hasCol;
        float lf = log2(max(length((suv - uv) * rs), 1.0)) - 4.0;  // (its level: CORTAO's mip bias)
        int L = mips > 0.5 ? clamp(int(floor(lf + 0.5)), 0, 4) : 0;
        float4 snd = loadNd(L, suv, cuv, mcol, hasCol);
        if (snd.w <= 0.0) continue;
        float3 d = viewPos(cuv, snd.w) - P;
        float l2 = dot(d, d);
        if (l2 >= R * R || dot(d, n) <= 0.0) continue;
        float il = rsqrt(l2);
        float3 dir = d * il, sn3 = snd.xyz;
        float slope = rsqrt(max(dot(sn3, V), 0.25));  // (CORTAO's slope compensation, at most x 2)
        float occR = R * w * slope * il;
        bool back = dot(sn3, dir) >= 0.5;
        if (back && flagOn(1u)) occR *= slope;  // (CORTAO's backface boost 1: the most contrast)
        if (!back) dir = normalize(sn3 * (-occR / max(1.0 - occR * occR * 0.5, 1e-6)) + dir);
        float3 hd = float3(dot(dir, tg), dot(dir, bt), dot(dir, n));
        float cosA = max(0.0, 1.0 - occR * occR * 0.5);
        uint ob = 0u;
        if (lut > 0.5) ob = lutBits(hd, occR);
        else {
            [loop] for (uint j = 0u; j < nr; j++) ob |= dot(rayDir(j, nr), hd) > cosA ? (1u << j) : 0u;
        }
        uint nb = ob & ~bits;
        bits |= ob;
        float prox = saturate(1.0 - 2.0 * sqrt(l2) / R);
        near += prox * prox * float(countbits(nb));
        if (giStrength > 0.0 && nb != 0u && !back)
            gi += (cmips > 0.5 && hasCol ? mcol : Color.SampleLevel(linearClamp, cuv, 0.0).rgb) * float(countbits(nb));
        if (bits == full) break;  // (every ray blocked: no later sample changes the occlusion or the GI)
    }
    float ao = (1.0 - float(countbits(bits)) / float(nr)) * saturate(1.0 - contact * near / float(nr));
    Res[id] = float4(gi / float(nr), ao);
}
