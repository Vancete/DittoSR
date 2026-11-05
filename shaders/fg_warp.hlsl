// DittoSR's frame generation: the in-between image at t, from the previous frame's image (t0) and this one's (t1),
// moved along the motion field (letsgo-recomp's flow_warp.frag). Output size. t2 = this frame's field, t3 = it
// dilated, t4 = the previous frame's field (hasPrev), u0 = the generated image.
//   Each pixel q: the point of this frame that is at q at t (q + (1 - t) v(p) = p, three steps from three starts: the
// dilated nearest vector, the farthest one, the pixel's own), each checked with its own vector (within 1.5 of the
// field's pixels: found); of those found, the nearest (what is in front at t). Found: this frame's image there, mixed
// by t with the previous frame's where the point was (p - v) - unless the previous field shows something nearer there
// than the point (it was covered: this frame's alone). Not found (uncovered between t and now): the previous image
// along the background's vector. Both images Catmull-Rom (bilinear softens a moved image).
#include "fg_common.hlsli"

Texture2D<float4> Prev : register(t0);
Texture2D<float4> Cur : register(t1);
Texture2D<float4> Field : register(t2);
Texture2D<float4> Dilated : register(t3);
Texture2D<float4> PrevField : register(t4);
RWTexture2D<float4> Output : register(u0);

float3 cr(Texture2D<float4> h, float2 uv) {
    float2 sz;
    h.GetDimensions(sz.x, sz.y);
    float2 p = uv * sz, c1 = floor(p - 0.5) + 0.5, f = p - c1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f), w12 = w1 + w2;
    float2 t0 = (c1 - 1.0) / sz, t3 = (c1 + 2.0) / sz, t12 = (c1 + w2 / w12) / sz;
    float3 r = h.SampleLevel(linearClamp, float2(t12.x, t0.y), 0.0).rgb * (w12.x * w0.y) +
               h.SampleLevel(linearClamp, float2(t0.x, t12.y), 0.0).rgb * (w0.x * w12.y) +
               h.SampleLevel(linearClamp, t12, 0.0).rgb * (w12.x * w12.y) +
               h.SampleLevel(linearClamp, float2(t3.x, t12.y), 0.0).rgb * (w3.x * w12.y) +
               h.SampleLevel(linearClamp, float2(t12.x, t3.y), 0.0).rgb * (w12.x * w3.y);
    float ws = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / ws, 0.0);
}

float4 at(Texture2D<float4> s, float2 uv) {
    float2 z;
    s.GetDimensions(z.x, z.y);
    return s.Load(int3(clamp(int2(uv * z), int2(0, 0), int2(z) - 1), 0));
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)size)) return;
    float2 q = (float2(id) + 0.5) * sizeRcp;
    float2 fs;
    Field.GetDimensions(fs.x, fs.y);
    float2 p = q;
    float4 f = 0.0;
    bool found = false;
    float4 d0 = at(Dilated, q);
    [unroll] for (int k = 0; k < 3; k++) {
        float2 c = q + (1.0 - t) * (k == 0 ? d0.xy : k == 1 ? d0.zw : at(Field, q).xy);
        [unroll] for (int i = 0; i < 2; i++) {
            float4 d = at(Dilated, c);
            c = q + (1.0 - t) * (k == 0 ? d.xy : k == 1 ? d.zw : at(Field, c).xy);
        }
        float4 g = at(Field, c);
        float2 e = (q + (1.0 - t) * g.xy - c) * fs;
        if (dot(e, e) < 2.25 && inside(c) && (!found || g.z < f.z)) { found = true; p = c; f = g; }
    }
    float3 col;
    if (found) {
        float2 r = p - f.xy;
        bool ok0 = inside(r);
        if (ok0 && hasPrev > 0.5 && at(PrevField, r).z < f.z * f.w * 0.94) ok0 = false;
        float3 n1 = cr(Cur, p);
        col = ok0 ? lerp(cr(Prev, r), n1, t) : n1;
    } else {
        float2 vb = at(Dilated, q + (1.0 - t) * d0.zw).zw, r = q - t * vb;
        col = inside(r) ? cr(Prev, r) : cr(Cur, q + (1.0 - t) * vb);
    }
    Output[id] = float4(col, 1.0);
}
