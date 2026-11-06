// DittoSR's in-game menu (overlay.cpp): one instanced draw of quads over the game's back buffer. Each quad is a rounded
// box (its edge antialiased by its distance), or a glyph from the font atlas (GDI's, antialiased), with a vertical
// gradient from c0 at its top to c1 at its bottom. Colours are sRGB with straight alpha; the back buffer's encoding
// decides the output: as is (SDR), linear scRGB (FP16 back buffers) or PQ in BT.2020 (HDR10).

struct Quad {
    float4 rect;  // x0, y0, x1, y1 (back buffer pixels)
    float4 uv;    // the glyph's atlas rect (u0, v0, u1, v1)
    float4 c0, c1;
    float4 p;     // x: the corner radius (px), y: 0 a box, 1 a glyph, 2 a box's outline, z: the outline's width (px)
};
StructuredBuffer<Quad> Quads : register(t0);
Texture2D<float> Font : register(t1);
SamplerState linearClamp : register(s0);
cbuffer K : register(b0) {
    float2 screen;  // the back buffer's size
    float outMode;  // 0 SDR, 1 scRGB, 2 HDR10 (PQ)
    float nits;     // the menu's white in HDR
};

struct V {
    float4 pos : SV_Position;
    float2 uv : UV;
    float2 local : LOCAL;  // (from the quad's centre, px)
    nointerpolation uint id : ID;
};

V vs(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    Quad q = Quads[iid];
    float2 c = float2(vid == 1 || vid == 2 || vid == 4 ? 1.0 : 0.0, vid == 2 || vid == 4 || vid == 5 ? 1.0 : 0.0);
    float pad = q.p.y == 1.0 ? 0.0 : 1.0;  // (a box: a pixel more around, for its antialiased edge)
    float2 a = q.rect.xy - pad, b = q.rect.zw + pad;
    float2 xy = lerp(a, b, c);
    V o;
    o.pos = float4(xy / screen * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = lerp(q.uv.xy, q.uv.zw, c);
    o.local = xy - 0.5 * (q.rect.xy + q.rect.zw);
    o.id = iid;
    return o;
}

float3 toLinear(float3 c) { return select(c <= 0.04045, c / 12.92, pow((max(c, 0.0) + 0.055) / 1.055, 2.4)); }

float3 pq(float3 l) {  // (l in units of 10000 nits)
    float3 y = pow(saturate(l), 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
}

float4 ps(V i) : SV_Target {
    Quad q = Quads[i.id];
    float2 h = 0.5 * (q.rect.zw - q.rect.xy);
    float t = saturate((i.pos.y - q.rect.y) / max(q.rect.w - q.rect.y, 1.0));
    float4 c = lerp(q.c0, q.c1, t);
    float a;
    if (q.p.y == 1.0) {
        a = Font.SampleLevel(linearClamp, i.uv, 0.0);
    } else {
        float r = min(q.p.x, min(h.x, h.y));
        float2 d2 = abs(i.local) - h + r;
        float d = length(max(d2, 0.0)) + min(max(d2.x, d2.y), 0.0) - r;
        a = saturate(0.5 - d);
        if (q.p.y == 2.0) a *= saturate(0.5 + d + q.p.z);
    }
    c.a *= a;
    if (outMode == 1.0) c.rgb = toLinear(c.rgb) * (nits / 80.0);
    else if (outMode == 2.0) {
        float3 l = toLinear(c.rgb) * nits;
        l = mul(float3x3(0.6274, 0.3293, 0.0433, 0.0691, 0.9195, 0.0114, 0.0164, 0.0880, 0.8956), l);
        c.rgb = pq(l / 10000.0);
    }
    return c;
}
