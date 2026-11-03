// DittoSR's upscale (the stability level kept in the history's alpha). Output size.
// t0 = the previous history, t1 = the convert pass's (xy motion in NDC, z depth clip), t2 = the colour, t3 = the
// previous frame's convert pass, t4 = the change pass's 1 x 1, t6 = the game's reactive mask (optional).
// u0 = the history (RGBA16F: rgb in DittoSR's encoded domain, a = the stability level), u1 = the game's output (the
// colour decoded back to the game's domain).
//   The current frame: the 3x3 render samples around the pixel, each where its jitter put it, weighted by a Gaussian of
//   its distance in output pixels (narrow) and by a wider one in render pixels (smooth). The history: Catmull-Rom at the
//   previous position, clipped toward the samples' mean in YCoCg to mean +- gamma sigma. The blend: a share of the new
//   frame by the samples' nearness, more where something changed (another object there, the depth standing out, much
//   of the image changed, the history well outside the box); a stable pixel at rest takes less of each new frame.
#include "common.hlsli"

Texture2D<float4> PrevOutput : register(t0);
Texture2D<float4> MotionDepthClip : register(t1);
Texture2D<float4> InputColor : register(t2);
Texture2D<float4> PrevMda : register(t3);
Texture2D<float> Change : register(t4);
Texture2D<float> Reactive : register(t6);
VKFMT("rgba16f") RWTexture2D<float4> History : register(u0);
#if !DITTO_VK
RWTexture2D<float4> Output : register(u1);  // (Vulkan: the sharpening pass writes the output, sharpness 0 or not)
#endif

float3 hist(float2 uv, out float3 low) {  // (Catmull-Rom in 5 bilinear taps; low: their plain mean, ~3 pixels wide)
    float2 sz = os, p = uv * sz, c1 = floor(p - 0.5) + 0.5, f = p - c1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f), w12 = w1 + w2;
    float2 q0 = (c1 - 1.0) / sz, q3 = (c1 + 2.0) / sz, q12 = (c1 + w2 / w12) / sz;
    float3 a = PrevOutput.SampleLevel(linearClamp, float2(q12.x, q0.y), 0.0).rgb;
    float3 b = PrevOutput.SampleLevel(linearClamp, float2(q0.x, q12.y), 0.0).rgb;
    float3 c = PrevOutput.SampleLevel(linearClamp, q12, 0.0).rgb;
    float3 d = PrevOutput.SampleLevel(linearClamp, float2(q3.x, q12.y), 0.0).rgb;
    float3 g = PrevOutput.SampleLevel(linearClamp, float2(q12.x, q3.y), 0.0).rgb;
    low = 0.2 * (a + b + c + d + g);
    float3 r = a * w12.x * w0.y + b * w0.x * w12.y + c * w12.x * w12.y + d * w3.x * w12.y + g * w12.x * w3.y;
    float ws = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return saturate(r / ws);
}

void put(uint2 id, float3 g, float stab, float e) {
    g = saturate(g);
    History[id] = hstore(id, g, stab);
#if !DITTO_VK
    if (debugView < 0.5) return;  // (sharpen.hlsl writes the output from the history; a debug view: this pass)
    float3 o = dec(g, e);
    if (indicator > 0.5 && id.x < 12 && id.y < 12) o = float3(0.0, 1.0, 0.0) / e;
    Output[id] = float4(o, 1.0);
#endif
}

// The group's render samples: an 8 x 8 output tile reads at most 10 x 10 of them (8 / ratio and its 3x3's margin, at
// ratio 1 and above), each loaded and encoded once into groupshared memory - not by each of the 9 x ratio^2 output
// pixels around it (~20 at 1.5x, ~80 at 3x). One sample more on each side: the tile's bounds and a pixel's own 3x3
// round the same floor apart and can land a sample apart (seen at 3x, where q + jitter falls on whole samples). A
// pixel whose 3x3 still falls outside, or below ratio 1, its own loads.
#define TILE 12
#if DITTO_FP16
typedef float16_t4 hf4;
#else
typedef float4 hf4;
#endif
groupshared hf4 gsTile[TILE * TILE];  // rgb: the sample encoded; a: its luma

int2 sampleBase(uint2 p) {  // (the 3x3's centre sample, as main computes it)
    float2 uv = (float2(p) + 0.5) * osRcp, q = uv * rs;
    return int2(floor(q + jit));
}

hf4 sampleAt(int2 p, int2 cs, float e) {
    hf3 c = ench(InputColor.Load(int3(clamp(p, int2(0, 0), cs), 0)).rgb, e);
    return hf4(c, dot(c, hf3(0.25, 0.5, 0.25)));
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID, uint2 gid : SV_GroupID, uint li : SV_GroupIndex) {
    float e = encExposure();
    int2 cs = int2(rs) - 1;
    uint2 g0 = gid * 8;
    int2 t0 = sampleBase(g0) - 2, tw = sampleBase(min(g0 + 7, (uint2)os - 1)) + 3 - t0;
    bool tiled = all(tw <= TILE);
    if (tiled) {
        for (int i = (int)li; i < tw.x * tw.y; i += 64) {
            int2 o = int2(i % tw.x, i / tw.x);
            gsTile[o.y * TILE + o.x] = sampleAt(t0 + o, cs, e);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (any(id >= (uint2)os)) return;
    float2 uv = (float2(id) + 0.5) * osRcp, q = uv * rs;
    float4 mda4 = MotionDepthClip.SampleLevel(linearClamp, uv, 0.0);
    float mlen = length(0.5 * mda4.xy * os);
    // (moving, the history holds little and the frame's own samples carry the image: its colour through a wider Gaussian -
    // the narrow one, nearly the nearest sample, shows the render samples' grid as a stipple through the grass. The
    // samples' confidence - the new frame's share - stays the narrow one's)
    float mf = saturate((mlen - 0.5) / 3.5), wd = width > 0.0 ? width : 1.0;
    float ks = max(ratio, 1.0) / wd, km = max(ratio, 1.0) / (motionWidth > 0.0 ? motionWidth : wd);
    int2 b0 = sampleBase(id);
    bool inTile = tiled && all(b0 - t0 >= 1) && all(b0 - t0 <= tw - 2);
    // (the samples' colour work at hf precision - half in the FP16 variant; positions, weights and moments in float)
    hf3 cc[9]; hf l[9];
    [unroll] for (int i = 0; i < 9; i++) {
        int2 p = b0 + int2(i % 3 - 1, i / 3 - 1), o = p - t0;
        hf4 s4;
        [branch] if (inTile) s4 = gsTile[o.y * TILE + o.x];
        else s4 = sampleAt(p, cs, e);
        cc[i] = s4.rgb; l[i] = s4.a;
    }
    // the edge: the samples' luma gradient (Sobel); the Gaussian stretched along it, narrow across it
    hf gx = (l[2] + (hf)2.0 * l[5] + l[8]) - (l[0] + (hf)2.0 * l[3] + l[6]);
    hf gy = (l[6] + (hf)2.0 * l[7] + l[8]) - (l[0] + (hf)2.0 * l[1] + l[2]);
    hf lmn = l[0], lmx = l[0];
    [unroll] for (int i = 0; i < 9; i++) { lmn = min(lmn, l[i]); lmx = max(lmx, l[i]); }
    float g = length(float2(gx, gy)), es = saturate(g / (4.0 * (float)(lmx - lmn) + 1e-3));
    float2 nrm = g > 1e-5 ? float2(gx, gy) / g : float2(1.0, 0.0);
    float along = 1.0 - 0.75 * edge * es;
    // the thin line: the centre sample a local extreme of its 3x3 - a lash, a wire
    hf omn = 1.0, omx = 0.0;
    [unroll] for (int i = 0; i < 9; i++) if (i != 4) { omn = min(omn, l[i]); omx = max(omx, l[i]); }
    float ext = (float)max(omn - l[4], l[4] - omx) / ((float)(lmx - lmn) + 1e-3);
    float lockf = lockLines * saturate(ext * 4.0 + 0.5 * step(0.0, ext));
    hf3 sg = 0.0, sm = 0.0, ss = 0.0, ymn = 1e4, ymx = -1e4;
    float3 m1 = 0.0, m2 = 0.0;
    float wg = 0.0, wm = 0.0, wsum = 0.0;
    // (the Gaussians as exp2 with their constants folded once; the smooth one, round, separable: 3 + 3 exp2 for the 9;
    // the wide moving one only while moving - at rest its estimate is weighted 0)
    const float L2E = 1.44269504;
    float kn = -2.26 * L2E * ks * ks, kw = -2.26 * L2E * km * km;
    float2 d0 = float2(b0) + 0.5 - jit - q;
    float3 ex = exp2(-1.4 * L2E * (d0.x + float3(-1.0, 0.0, 1.0)) * (d0.x + float3(-1.0, 0.0, 1.0)));
    float3 ey = exp2(-1.4 * L2E * (d0.y + float3(-1.0, 0.0, 1.0)) * (d0.y + float3(-1.0, 0.0, 1.0)));
    float r2s[9];
    [unroll] for (int i = 0; i < 9; i++) {
        float2 d = d0 + float2(i % 3 - 1, i / 3 - 1);
        float dn = dot(d, nrm), dt = dot(d, float2(-nrm.y, nrm.x));
        float r2 = dn * dn + dt * dt * along;
        r2s[i] = r2;
        float w = exp2(r2 * kn), w2 = ex[i % 3] * ey[i / 3];
        sg += cc[i] * (hf)w; wg += w; ss += cc[i] * (hf)w2; wsum += w2;
        hf3 yc = ycch(cc[i]); ymn = min(ymn, yc); ymx = max(ymx, yc);
        float3 ycf = yc; m1 += ycf; m2 += ycf * ycf;
    }
    [branch] if (mf > 0.0) {
        [unroll] for (int i = 0; i < 9; i++) {
            float w3 = exp2(r2s[i] * kw);
            sm += cc[i] * (hf)w3; wm += w3;
        }
    }
    m1 /= 9.0; float3 sig = sqrt(max(m2 / 9.0 - m1 * m1, 0.0));
    float3 cg = lerp((float3)sg / max(wg, 1e-6), (float3)sm / max(wm, 1e-6), mf), cs2 = (float3)ss / max(wsum, 1e-6);
    float cf = saturate(wg);
    float3 cur = lerp(cs2, cg, smoothstep(0.05, conf > 0.0 ? conf : 0.6, cf));
    float3 mda = mda4.xyz;
    float2 puv = uv - 0.5 * mda.xy;
    // (GSR 2's depth clip only as far as the pixel moves: at rest a silhouette stands out every frame, alternating)
    if (depthMode > 0.5) mda.z *= saturate(mlen * 0.5);
    if (debugView > 0.5 && debugView < 1.5) { put(id, float3(saturate(abs(0.5 * mda.xy * os) * 64.0), 0.0), 0.0, 1.0); return; }
    bool off = any(puv < 0.0) || any(puv > 1.0);
    if (reset > 0.5 || off) { put(id, cs2, 0.0, e); return; }
    // (the previous frame's motion where this pixel was: changed by over half a pixel more than its neighbours' -
    // another object's - less history, a narrower box. Against the neighbours: the camera speeding up or slowing down,
    // a hand on the mouse, changes every pixel's motion alike and cut the whole history; another object's edge changes it
    // here and not a little way off. The neighbours as far as the change itself, so a revealed strip's lie outside it)
    float4 pm = PrevMda.SampleLevel(linearClamp, puv, 0.0);
    float2 dlt = 0.5 * (mda4.xy - pm.xy) * os;
    float rad = clamp(length(dlt), 2.0, 16.0), pdif = 0.0;
    [unroll] for (int n = 0; n < 4; n++) {
        float2 nuv = uv + float2(n == 0 ? rad : n == 1 ? -rad : 0.0, n == 2 ? rad : n == 3 ? -rad : 0.0) * osRcp;
        float2 nm = MotionDepthClip.SampleLevel(linearClamp, nuv, 0.0).xy;
        float2 npm = PrevMda.SampleLevel(linearClamp, nuv - 0.5 * nm, 0.0).xy;
        pdif = max(pdif, length(dlt - 0.5 * (nm - npm) * os));
    }
    float rj = smoothstep(0.5, 2.0, pdif);
    // (the disocclusion, depthMode 0: where the pixel was, the previous frame's surfaces - even the farthest of the four
    // around it - nearer than this one, and moving otherwise than it (their motion: the previous frame's convert pass
    // took the nearest's): it was hidden behind something that moved away. A turn of the camera, a still scene, or a
    // thin blade the jitter hits one frame and misses the next keep the history - GSR 2's depth clip cut it at every
    // depth edge as soon as anything moved, the depth alone at every sub-pixel blade: the grass's noise)
    if (depthMode < 0.5) {
        float4 pn = PrevMda.GatherAlpha(linearClamp, puv);
        float np = min(min(pn.x, pn.y), min(pn.z, pn.w)), nc = mda4.w;
        float nearer = np > 1e-7 ? smoothstep(0.03, 0.15, 1.0 - nc / np) : 0.0;
        mda.z = nearer * smoothstep(0.25, 1.0, pdif);
    }
    float3 hl, hy = ycc(hist(puv, hl));
    hl = ycc(hl);
    // (the stability level where the pixel was - the history's alpha)
    float sp = PrevOutput.SampleLevel(linearClamp, puv, 0.0).a;
    // (the share of the image that changed against its history - the change pass's one pixel)
    float gch = changeScale * smoothstep(0.2, 0.5, Change.Load(int3(0, 0, 0)));
    float rest = gammaRest > 0.0 ? gammaRest : 1.5;
    float gam = lerp(rest, motionGamma, saturate(mlen * 0.25)) * (1.0 - 0.5 * mda.z);
    gam = lerp(gam, 1.5 * rest, lockf);
    gam *= 1.0 - 0.6 * rj;
    // (stable and at rest, a wider box - the jittered samples' own spread no longer cuts the history)
    gam *= 1.0 + 0.35 * sp * (1.0 - saturate(mlen));
    // (moving: the box mean +- gamma sigma within the samples' own range - min / max, as far as the pixel moves)
    float3 blo = m1 - gam * sig, bhi = m1 + gam * sig;
    float mm = motionMinMax * saturate(mlen * 0.5);
    blo = lerp(blo, max(blo, (float3)ymn), mm); bhi = lerp(bhi, min(bhi, (float3)ymx), mm);
    float3 bc = 0.5 * (blo + bhi), ext3 = 0.5 * (bhi - blo) + 1.0 / 255.0;
    // (splitClip: the box against the history's low frequencies - the jittered samples' aliasing of fine detail no
    // longer counts as a change - the clip moving those alone; the detail above them kept, fading as the low ones fall
    // far out: a real change, the detail stale as well. Else the whole history clipped, in motion toward one aliased frame)
    float3 hc = splitClip > 0.5 ? hl : hy;
    float3 dir = hc - bc;
    float3 ad = abs(dir) / ext3;
    float t = max(ad.x, max(ad.y, ad.z));
    float3 hcc = t > 1.0 ? bc + dir / t : hc;
    hy = splitClip > 0.5 ? hcc + (hy - hl) * (1.0 - smoothstep(1.0, 2.5, t)) : hcc;
    // (above 2x - hr 0 to 1 at 3x - moving, more of the new frame: its jittered samples are what carries a moving
    // object's detail, the history resampled every frame softening it - at 3x a moving object matched FSR's at 2.3x,
    // and the grass still shimmered half as much as FSR's)
    float hr = saturate(ratio - 2.0);
    float base = lerp(lerp(0.2, alphaRest > 0.0 ? alphaRest : 0.035, sp), motionShare * (1.0 + 0.67 * hr), saturate(mlen * 0.125));
    // (moving, less contrast between the pixels a sample landed near and the rest: each frame's own pattern of them
    // shimmers - at rest it is what resolves the detail)
    float near = motionNear > 0.0 ? lerp(shareNear, motionNear, mf) : shareNear;
    float a = base * lerp(shareFar, near, smoothstep(0.05, 0.8, cf));
    // (frame_weight: the user's persistence - the reactions below still take over where something changed. Above 1, in
    // full moving and a quarter of its excess at rest: there the jittered samples it lets in showed as a grain - a
    // sharpening at rest with the sharpening off)
    float fwRest = frameWeight > 1.0 ? 1.0 + 0.25 * (frameWeight - 1.0) : frameWeight;
    a = saturate(a * lerp(fwRest, frameWeight, mf));
    a = lerp(a, 0.6, mda.z);
    // the reaction: the history outside the samples' range by t - something changed - the new frame by it
    // (with much of the image changed the reaction goes further: the new frame up to 0.95)
    a = lerp(a, lerp(0.5, 0.95, gch), saturate((t - 1.0) * react) * (1.0 - lockf));
    a = lerp(a, max(a, 0.5), gch);
    a = lerp(a, max(a, 0.6), rj);
    // (the game's reactive mask: particles, transparencies without motion of their own - more of the new frame)
    float rv = 0.0;
    if (hasReactive > 0.5) {
        rv = saturate(Reactive.SampleLevel(linearClamp, uv, 0.0) * reactiveScale);
        a = max(a, rv);
    }
    // (the stability for the next frame: 0 where the history was cut, else one step up. Above 2x at rest, a step only
    // when a sample landed near the pixel: at 3x one does every ~9 frames, and a step each frame had the pixel stable -
    // taking little of each new sample - before its first ones came; 30.8 to 31.9 dB on still grass at 3x, as a pixel
    // after a cut converges)
    bool cut = t > 1.25 || rj > 0.5 || mda.z > 0.5 || gch > 0.5 || rv > 0.5;
    float3 res = lerp(rgb(hy), cur, saturate(a));
    bool landed = hr <= 0.0 || cf > 0.15 * hr || mlen > 1.0;
    float stab = cut ? 0.0 : min(sp + (landed ? 0.34 : 0.0), 1.0);
    if (debugView > 1.5) {  // (the history as usual, the output a view of the decision)
        float3 v = debugView < 2.5 ? saturate(a).xxx : debugView < 3.5 ? sp.xxx : float3(saturate(t - 1.0), cut ? 1.0 : 0.0, cf);
#if DITTO_VK
        History[id] = float4(v, 0.0);  // (Vulkan: the view through the history and the sharpening pass - approximate)
#else
        History[id] = hstore(id, res, stab);
        Output[id] = float4(v, 1.0);
#endif
        return;
    }
    put(id, res, stab, e);
}
