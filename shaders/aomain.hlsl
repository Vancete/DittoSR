// DittoSR's experimental AO / GI, the main pass (render size) - CORTAO's (MIT, licenses/LICENSE-cortao.txt) main pass
// without its lookup tables (each ray tested against each occluder directly), plus the GI:
//   12 samples on a golden-angle spiral over a disc around the pixel, facing the camera (its radius: the occluders'
// reach in view space, capped to a share of the screen), rotated and dithered by a fixed 4 x 4 pattern - the
// accumulation averages it. Each sample within reach and above the pixel's surface is a circular occluder: its size
// by the sample's share of the disc and its slope to the camera (CORTAO's slope compensation), moved one radius into
// its surface when it faces the pixel (no self-occlusion), left as it is when it faces away (a backface). 4 - 32 rays over
// the hemisphere (cosine-distributed: a golden spiral on the unit disc, lifted) - a ray inside an occluder's angular
// size is blocked. The occlusion: 1 - the share of rays blocked.
//   The GI: a ray blocked for the first time takes the colour of the occluder that blocks it (nearest first: the samples
// go outward), unless it is a backface (its lit side faces away). The rays being cosine-distributed, their plain mean
// is the irradiance / pi - the light bounced off what surrounds the pixel, as far as the screen shows it.
//   The contact term: the rays blocked first by an occluder near the pixel (closer than half the reach, more the
// closer) darken more, x contact - a tyre on the road, a box against a wall: what a ray's yes / no leaves as faint as a
// distant wall's shade. The occlusion x (1 - contact x that share).
// t0 = the prepass' normal and depth, t1 = the game's colour; u0 = rgb the bounce light, a the occlusion.
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Color : register(t1);
RWTexture2D<float4> Res : register(u0);

#define SAMPLES 12
static const float GOLDEN = 2.39996323;

float3 rayDir(uint j, uint nr) {
    float a = float(j) * GOLDEN, r = sqrt((float(j) + 0.5) / float(nr));
    return float3(cos(a) * r, sin(a) * r, sqrt(1.0 - r * r));
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rs)) return;
    float4 nd = Nd.Load(int3(id, 0));
    float z = nd.w;
    if (z <= 0.0) { Res[id] = float4(0.0, 0.0, 0.0, 1.0); return; }
    float3 n = nd.xyz;
    uint nr = clamp((uint)rays, 4u, 32u);
    float2 uv = (float2(id) + 0.5) * rsRcp;
    float3 P = viewPos(uv, z), V = -normalize(P);
    // the reach: the world radius, capped to radiusScreen of the screen's height at this depth (CORTAO's apparent
    // circle: the disc that covers the whole sphere seen from the camera)
    float ms = tanY * radiusScreen * 2.0, coef = ms * rsqrt(ms * ms + 1.0);
    float R = min(radiusWorld > 0.0 ? radiusWorld : 1e20, z * coef);
    float circle = R * rsqrt(max(z * z - R * R, 1e-12)) * z;
    // (the pixel pushed off its surface by about a pixel: no self-occlusion)
    P += n * (1.41421356 * tanX * rsRcp.x * z);
    // the noise: the spiral's rotation and radial dither, the rays' rotation - a fixed 4 x 4 pattern (Bayer's order: the
    // 16 rotations spread over the tile, the dither and the rays' turn on golden-ratio steps of it) that the
    // composition's 4 x 4 filter averages out
    static const uint BAYER[16] = {0u, 8u, 2u, 10u, 12u, 4u, 14u, 6u, 3u, 11u, 1u, 9u, 15u, 7u, 13u, 5u};
    float b = float(BAYER[(id.x & 3u) + 4u * (id.y & 3u)]);
    float rot = 6.2831853 * (b + 0.5) / 16.0, dith = frac(b * 0.618034 + 0.31), rrot = 6.2831853 * frac(b * 0.381966 + 0.11);
    float2 sr = float2(cos(rot), sin(rot)) * circle;
    float3 right, up;
    basisOf(V, right, up);
    if (((uint)b & 1u) != 0u) right = -right;
    float3 t0, b0;
    basisOf(n, t0, b0);
    float cr = cos(rrot), sn = sin(rrot);
    float3 tg = cr * t0 + sn * b0, bt = cr * b0 - sn * t0;
    uint bits = 0u;
    float3 gi = 0.0;
    float near = 0.0;
    [unroll] for (uint i = 0u; i < SAMPLES; i++) {
        float a = float(i) * GOLDEN;
        float2 sp = float2(cos(a), sin(a));
        float inner = pow(float(i) / SAMPLES, 1.75), outer = pow(float(i + 1u) / SAMPLES, 1.75);
        float w = sqrt(outer * outer - inner * inner);
        float2 o = sp * lerp(inner, outer, dith);
        o = float2(o.x * sr.x - o.y * sr.y, o.x * sr.y + o.y * sr.x);
        float3 Q = P + right * o.x + up * o.y;
        if (Q.z <= 0.0) continue;
        float2 suv = projUv(Q);
        if (any(suv < 0.0) || any(suv > 1.0)) continue;
        int2 sp2 = int2(suv * rs);
        float4 snd = Nd.Load(int3(sp2, 0));
        if (snd.w <= 0.0) continue;
        float2 cuv = (float2(sp2) + 0.5) * rsRcp;
        float3 d = viewPos(cuv, snd.w) - P;
        float l2 = dot(d, d);
        if (l2 >= R * R || dot(d, n) <= 0.0) continue;
        float il = rsqrt(l2);
        float3 dir = d * il, sn3 = snd.xyz;
        float slope = rsqrt(max(dot(sn3, V), 0.25));  // (CORTAO's slope compensation, at most x 2)
        float occR = R * w * slope * il;
        bool back = dot(sn3, dir) >= 0.5;
        if (!back) dir = normalize(sn3 * (-occR / max(1.0 - occR * occR * 0.5, 1e-6)) + dir);
        float3 hd = float3(dot(dir, tg), dot(dir, bt), dot(dir, n));
        float cosA = max(0.0, 1.0 - occR * occR * 0.5);
        uint ob = 0u;
        [loop] for (uint j = 0u; j < nr; j++) ob |= dot(rayDir(j, nr), hd) > cosA ? (1u << j) : 0u;
        uint nb = ob & ~bits;
        bits |= ob;
        float prox = saturate(1.0 - 2.0 * sqrt(l2) / R);
        near += prox * prox * float(countbits(nb));
        if (giStrength > 0.0 && nb != 0u && !back) gi += Color.SampleLevel(linearClamp, cuv, 0.0).rgb * float(countbits(nb));
    }
    float ao = (1.0 - float(countbits(bits)) / float(nr)) * saturate(1.0 - contact * near / float(nr));
    Res[id] = float4(gi / float(nr), ao);
}
