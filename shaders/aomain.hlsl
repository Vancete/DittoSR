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
// mip bias); cmips: its colour too, the mips' mean of its area (packed with them), the level around its
// distance.
//   The GI: a ray blocked for the first time takes the colour of the occluder that blocks it (nearest first: the samples
// go outward), unless it is a backface (its lit side faces away). The rays being cosine-distributed, their plain mean
// is the irradiance / pi - the light bounced off what surrounds the pixel, as far as the screen shows it.
//   The contact term: the rays blocked first by an occluder near the pixel (closer than half the reach, more the
// closer) darken more, x contact - a tyre on the road, a box against a wall: what a ray's yes / no leaves as faint as a
// distant wall's shade. The occlusion x (1 - contact x that share).
//   halfRes: one pixel of each 2 x 2 (halfOff: the same every frame, FastGI's fixed grid), into a half-size target; with
// flags 16 (ao_fuse) its normal and depth written here too (u1, packed u2), in place of aohalfnd.hlsl.
// t0 = the prepass' normal and depth, t1 = the game's colour, t2 - t5 the depth mips 1 - 4, t6 the lookup table;
// u0 = rgb the bounce light, a the occlusion.
// (u1, u2: fused - above)
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Color : register(t1);
Texture2D<uint4> Mip1 : register(t2);
Texture2D<uint4> Mip2 : register(t3);
Texture2D<uint4> Mip3 : register(t4);
Texture2D<uint4> Mip4 : register(t5);
Texture2D<uint> Lut : register(t6);
AOFMT("rgba16f") RWTexture2D<float4> Res : register(u0);
AOFMT("rgba32f") RWTexture2D<float4> NdHalf : register(u1);
AOFMT("rg32ui") RWTexture2D<uint2> NdHalfP : register(u2);
AOFMT("rgba16f") RWTexture2D<float4> Probe : register(u3);  // (screen probes, flags 4194304: the GI at each probe)
#if !DITTO_VK
Texture2D<float> Blue : register(t8);  // (blue noise, flags 8388608: 64 x 64, R8)
Texture2D<float4> PrevH : register(t9);  // (interleaved GI, flags2 512: the temporal smoothing's last result)
RWTexture2D<float4> ProbeDir : register(u4);  // (directional probes, flags2 64: the light's mean direction, a the depth)
#endif

// (FP16, flags2 256 - the AO_HALF variant: the directions' and sizes' math in 16 bits)
#if AO_HALF
typedef float16_t hf;
typedef vector<float16_t, 3> hf3;
#else
typedef float hf;
typedef float3 hf3;
#endif

#ifndef AO_WG
#define AO_WG 8  // (the group's side: 16 the 16 x 16 variant, aomain16)
#endif

// (mips in groupshared memory, flags2 1024: level 4's 32 x 32 texels around the group's centre, loaded once a group)
groupshared uint4 gM4[32][32];
groupshared int2 gM4o;
static bool gLds = false;  // (this dispatch reads it)

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
    if (L == 0) return LOAD_ND(Nd, t);
    uint4 m;
    int2 lt = t - gM4o;
    if (L == 4 && gLds && all(lt >= 0) && all(lt < 32)) m = gM4[lt.y][lt.x];
    else m = L == 1 ? Mip1.Load(int3(t, 0)) : L == 2 ? Mip2.Load(int3(t, 0)) : L == 3 ? Mip3.Load(int3(t, 0)) : Mip4.Load(int3(t, 0));
    hasCol = true;
    return mipDecode(m, col);
}

// one sample - its view position Q, its share w of the disc: its occluder's rays (bits; giOnly: gbits, the light alone -
// the guided samples', the occlusion left to the spiral's), its light to the newly blocked rays
// (returns 1 for a sample off the screen - the edge fill's count)
int occluderAt(float3 Q, float w, float3 P, float3 n, float3 V, float R, float2 uv, float3 tg, float3 bt, uint nr, bool giOn,
               bool giOnly, inout uint bits, inout uint gbits, inout float3 gi, inout float near, inout float3 gdir) {
    if (Q.z <= 0.0) return 1;
    float2 suv = projUv(Q);
    if (any(suv < 0.0) || any(suv > 1.0)) return 1;
    float2 cuv;
    float3 mcol;
    bool hasCol;
    float lf = log2(max(length((suv - uv) * rs), 1.0)) - 4.0;  // (its level: CORTAO's mip bias)
    int L = mips > 0.5 ? clamp(int(floor(lf + 0.5)), flagOn(131072u) ? 1 : 0, 4) : 0;  // (131072: level 1 the nearest)
    float4 snd = loadNd(L, suv, cuv, mcol, hasCol);
    if (snd.w <= 0.0) return 0;
    float3 d = viewPos(cuv, snd.w) - P;
    float l2 = dot(d, d);
    if (l2 >= R * R || dot(d, n) <= 0.0) return 0;
    float il = rsqrt(l2);
    float3 dir = d * il, sn3 = snd.xyz, dir0 = dir;
    float slope = rsqrt(max(dot(sn3, V), 0.25));  // (CORTAO's slope compensation, at most x 2)
    float occR = R * w * slope * il;
    bool back = dot(sn3, dir) >= 0.5;
    if (back && flagOn(1u)) occR *= slope;  // (CORTAO's backface boost 1: the most contrast)
    if (!back) dir = normalize(sn3 * (-occR / max(1.0 - occR * occR * 0.5, 1e-6)) + dir);
    hf3 hd = hf3(dot(dir, tg), dot(dir, bt), dot(dir, n));
    hf cosA = (hf)max(0.0, 1.0 - occR * occR * 0.5);
    uint ob = 0u;
    if (flag2On(1u)) ob = lutBits((float3)hd, occR);
    else {
        [loop] for (uint j = 0u; j < nr; j++) ob |= dot((hf3)rayDir(j, nr), hd) > cosA ? (1u << j) : 0u;
    }
    uint nb = ob & ~(bits | gbits);
    if (giOnly) gbits |= ob;
    else {
        bits |= ob;
        float prox = saturate(1.0 - 2.0 * sqrt(l2) / R);
        near += prox * prox * float(countbits(nb));
    }
    if (giOn && giStrength > 0.0 && nb != 0u && !back) {
        // (flags 512, the emitter's cosine at half strength: its light x (1 + how squarely its surface faces the
        // pixel) / 2 - one seen edge-on lights it less, by half at most)
        float3 gcol = cmips > 0.5 && hasCol ? mcol : Color.SampleLevel(linearClamp, cuv, 0.0).rgb;
        if (flag2On(8u) && !(cmips > 0.5 && hasCol) && mips > 0.5) {
            // (the source clamp, flags2 8: a full-resolution sample's colour at most 3 x its 4 x 4 area's mean - a
            // highlight or a reflection no light source)
            uint2 d2 = ((uint2)rs + 3u) >> 2;
            float3 c2;
            mipDecode(Mip2.Load(int3(min(int2(suv * float2(d2)), int2(d2) - 1), 0)), c2);
            gcol = min(gcol, 3.0 * c2 + 0.02);
        }
        float3 add = gcol * float(countbits(nb)) *
                     (flagOn(512u) ? 0.5 + 0.5 * saturate(dot(sn3, -dir0)) : 1.0) *
                     (flagOn(1048576u) ? saturate(1.0 - l2 / (R * R)) : 1.0);  // (1048576: FastGI's falloff, 1 - (d / reach)^2)
        gi += add;
        gdir += dir0 * dot(add, float3(0.2126, 0.7152, 0.0722));  // (directional probes: the light's direction, weighted)
    }
    return 0;
}

[numthreads(AO_WG, AO_WG, 1)]
void main(uint2 tid : SV_DispatchThreadID, uint2 gid : SV_GroupID, uint li : SV_GroupIndex) {
    uint2 hdims = halfRes > 0.5 ? ((uint2)rs + 1u) >> 1 : (uint2)rs;
    // (screen probes, flags 4194304: this dispatch the pixels' AO alone with half the samples, or - axis 5 - the probes',
    // one thread a probe at its pixel within its 4 x 4 this frame, the GI with twice the samples)
    bool probes = flagOn(4194304u), probeRun = probes && axis > 4.5;
    gLds = flag2On(1024u) && mips > 0.5 && !probeRun;
    if (gLds) {  // (mips in groupshared memory: the group's level-4 tile, all threads)
        uint2 d4 = ((uint2)rs + 15u) >> 4;
        float2 cuv = (float2(gid * AO_WG + AO_WG / 2) + 0.5) / float2(hdims);
        int2 org = int2(cuv * float2(d4)) - 16;
        for (uint k = li; k < 1024u; k += AO_WG * AO_WG) {
            int2 t = int2(k & 31u, k >> 5);
            gM4[t.y][t.x] = Mip4.Load(int3(clamp(org + t, int2(0, 0), int2(d4) - 1), 0));
        }
        if (li == 0u) gM4o = org;
        GroupMemoryBarrierWithGroupSync();
    }
    uint2 id = tid;
    if (probeRun) {
        if (any(tid >= (hdims + 3u) >> 2)) return;
        id = min(tid * 4u + probeJitter(), hdims - 1u);
    } else if (any(id >= hdims)) return;
    uint hk = 0u;  // (the half size's pixel of each 2 x 2: 2x, 2y - FastGI's fixed grid)
    uint2 pix = halfRes > 0.5 ? min(id * 2u + uint2(hk & 1u, hk >> 1u), (uint2)rs - 1u) : id;
    float4 nd = flagOn(131072u) ? LOAD_ND(Nd, id) : LOAD_ND(Nd, pix);  // (131072: Nd the half size's)
    if (flagOn(16u) && halfRes > 0.5 && !probeRun) {  // (fused: the half size's normal and depth, as aohalfnd.hlsl's)
        if (flagOn(4u)) NdHalfP[id] = ndPack(nd);
        else NdHalf[id] = nd;
    }
    float z = nd.w;
    if (z <= 0.0) {
        if (probeRun) Probe[tid] = 0.0;
        else Res[id] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    float3 n = nd.xyz;
    uint nr = flag2On(1u) ? 32u : clamp((uint)rays, 4u, 32u);
    uint ns = clamp((uint)samples, 4u, 32u);
    bool giOn = !probes || probeRun;  // (aoprobe.hlsl brings the probes' GI to every pixel)
    if (probes) ns = probeRun ? min(ns * 2u, 64u) : max(ns / 2u, 4u);
    // (interleaved GI, flags2 512: on half the pixels a frame - a checkerboard, the other half the next - the GI from the
    // temporal smoothing's last result, the AO alone with half the samples)
    bool reuse = flag2On(512u) && !probes && (((id.x + id.y + (uint)frame) & 1u) != 0u);
    if (reuse) giOn = false, ns = max(ns / 2u, 4u);
    // (cheaper far pixels, flags2 4096: past twice the AO's fade distance - the AO faded out - half the samples)
    if (flag2On(4096u) && fadeZ > 0.0 && z > 2.0 * fadeZ) ns = max(ns / 2u, 4u);
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
    // (flags 524288, FastGI's 8-frame cycle: the noise the same every 8 frames - the temporal smoothing's mean a fixed one)
    float frm = flagOn(524288u) ? float((uint)frame & 7u) : frame;
    float2 fp = float2(pix) + 5.588238 * frm;
    float rot = 6.2831853 * ign(fp), dith = ign(fp.yx + 17.0), rrot = 6.2831853 * ign(fp + float2(37.0, 11.0));
    float mir = ign(fp + 71.0);
    if (flagOn(2u)) {
        // (decorrelated noise: each dimension its own IGN, moved each frame along its own direction - its phase a
        // different step a frame; they shared one (the swapped coordinates, the same offset), so a pixel only ever saw
        // one curve through their combinations and its mean settled on a fixed pattern. Each still IGN moved in the
        // image, not one value stepped for every pixel at once: what changes from a frame to the next stays fine-grained,
        // for the filters - an R-sequence's step, the same everywhere, flickered in large patches)
        float2 q = float2(pix), f = (float2)frm;
        dith = ign(q + float2(17.0, 59.0) + f * float2(3.1, 9.7));
        rrot = 6.2831853 * ign(q + float2(37.0, 11.0) + f * float2(7.9, 1.3));
        mir = ign(q + 71.0 + f * float2(1.7, 13.3));
    }
#if !DITTO_VK
    if (flagOn(8388608u)) {
        // (blue noise, flags 8388608: each dimension the 64 x 64 mask at its own offset, stepped by the golden ratio a frame
        // - spatially even, each pixel's values over the frames spread evenly too)
        int2 q = int2(pix);
        float g = 0.61803398875 * frm;
        rot = 6.2831853 * frac(Blue.Load(int3(q & 63, 0)) + g);
        dith = frac(Blue.Load(int3((q + int2(17, 41)) & 63, 0)) + 0.5 * g);
        rrot = 6.2831853 * frac(Blue.Load(int3((q + int2(29, 7)) & 63, 0)) + 0.75 * g);
        mir = frac(Blue.Load(int3((q + int2(53, 23)) & 63, 0)) + 0.25 * g);
    }
#endif
    float2 sr = float2(cos(rot), sin(rot)) * circle;
    float3 right, up;
    basisOf(V, right, up);
    if (mir < 0.5) right = -right;
    float3 t0, b0;
    basisOf(n, t0, b0);
    float cr = cos(rrot), sn = sin(rrot);
    float3 tg = cr * t0 + sn * b0, bt = cr * b0 - sn * t0;
    uint full = 0xffffffffu >> (32u - nr);  // (no shift by 32: undefined in DXIL - it broke the loop early)
    uint bits = 0u, gbits = 0u;
    float3 gi = 0.0, gdir = 0.0;
    float near = 0.0;
    uint off = 0u;
    [loop] for (uint i = 0u; i < ns; i++) {
        float a = float(i) * GOLDEN;
        float2 sp = float2((hf)cos(a), (hf)sin(a));
        hf inner = (hf)pow(float(i) / float(ns), 1.75), outer = (hf)pow(float(i + 1u) / float(ns), 1.75);
        float w = (float)sqrt(outer * outer - inner * inner);
        float2 o = sp * lerp(inner, outer, dith);
        o = float2(o.x * sr.x - o.y * sr.y, o.x * sr.y + o.y * sr.x);
        off += (uint)occluderAt(P + right * o.x + up * o.y, w, P, n, V, R, uv, tg, bt, nr, giOn, false, bits, gbits, gi, near, gdir);
        if (bits == full) break;  // (every ray blocked: no later sample changes the occlusion or the GI)
    }
    if (flagOn(2097152u) && giOn && mips > 0.5 && bits != full) {
        // (samples guided toward bright areas, flags 2097152: 16 points on the disc - 8 directions at half and all of its
        // radius, turned by the noise - their colour from the mips' level 3; the 2 brightest each one more sample, for the
        // light alone: a small bright light the spiral missed still lights the rays nothing else blocked)
        float bl0 = -1.0, bl1 = -1.0;
        float3 q0 = 0.0, q1 = 0.0;
        uint2 d3 = ((uint2)rs + 7u) >> 3;
        [unroll] for (uint k = 0u; k < 16u; k++) {
            float a = rot + 0.78539816 * float(k & 7u) + (k >= 8u ? 0.39269908 : 0.0);
            float2 o = float2(cos(a), sin(a)) * circle * (k >= 8u ? 0.9 : 0.45);
            float3 Q = P + right * o.x + up * o.y;
            if (Q.z <= 0.0) continue;
            float2 suv = projUv(Q);
            if (any(suv < 0.0) || any(suv > 1.0)) continue;
            float3 c;
            float4 m = mipDecode(Mip3.Load(int3(min(int2(suv * float2(d3)), int2(d3) - 1), 0)), c);
            float l = m.w > 0.0 ? dot(c, float3(0.2126, 0.7152, 0.0722)) : -1.0;
            if (l > bl0) bl1 = bl0, q1 = q0, bl0 = l, q0 = Q;
            else if (l > bl1) bl1 = l, q1 = Q;
        }
        float wg = rsqrt(float(ns));
        if (bl0 > 0.0) occluderAt(q0, wg, P, n, V, R, uv, tg, bt, nr, true, true, bits, gbits, gi, near, gdir);
        if (bl1 > 0.0) occluderAt(q1, wg, P, n, V, R, uv, tg, bt, nr, true, true, bits, gbits, gi, near, gdir);
    }
    if (giOn && flag2On(4u) && mips > 0.5) {
        // (sky light, flags2 4: the rays nothing blocked x how far up they point (view space) x the sky's colour - the
        // mean of level 4's sky texels (no depth) along the screen's top)
        uint2 d4 = ((uint2)rs + 15u) >> 4;
        float3 sky = 0.0;
        float ns4 = 0.0;
        [unroll] for (uint k = 0u; k < 4u; k++) {
            float3 c;
            float4 m = mipDecode(Mip4.Load(int3(min(uint2((0.125 + 0.25 * k) * d4.x, 0u), d4 - 1u), 0)), c);
            if (m.w <= 0.0) sky += c, ns4 += 1.0;
        }
        uint freeBits = full & ~(bits | gbits);
        if (ns4 > 0.0 && freeBits != 0u) {
            sky /= ns4;
            float up2 = 0.0;
            for (uint fb = freeBits; fb != 0u; fb &= fb - 1u) {
                float3 r = rayDir(firstbitlow(fb), nr);
                up2 += saturate((tg * r.x + bt * r.y + n * r.z).y);
            }
            gi += sky * up2;
        }
    }
    // (edge fill, flags2 32: the light the samples off the screen would have brought - the GI x the share of the samples on
    // it, up to x 2)
    if (flag2On(32u) && off > 0u && off < ns) gi *= min(float(ns) / float(ns - off), 2.0);
    float ao = (1.0 - float(countbits(bits)) / float(nr)) * saturate(1.0 - contact * near / float(nr));
    if (probeRun) {
        Probe[tid] = float4(gi / float(nr), z);  // (a: its depth - the probes' history and filter)
#if !DITTO_VK
        if (flag2On(64u)) ProbeDir[tid] = float4(dot(gdir, gdir) > 1e-12 ? normalize(gdir) : (float3)0.0, z);
#endif
        return;
    }
#if !DITTO_VK
    if (reuse) { Res[id] = float4(PrevH.Load(int3(id, 0)).rgb, ao); return; }
#endif
    Res[id] = float4(gi / float(nr), ao);
}
