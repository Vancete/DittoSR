// Ditto Relight (experimental), the half-size result back at a render pixel (aoup.hlsl; aocompose.hlsl with flags 8,
// ao_up_compose - the upsample in the composition, as FastGI's): the 4 half-size texels around it - each the pixel
// halfOff computed in its 2 x 2 - x geoWeight (its depth and normal near this pixel's: no light across an edge) x 1 /
// (1 + its distance^2); none close: the nearest in depth. Included after Nd (t0, the prepass' normal and depth) is
// declared.
// (the pixel's own normal and depth: the prepass', or with flags 131072 its depth from the game's (GDepth) and the normal
// of the half texel it lies in)
float4 ndAt(Texture2D<float> GDepth, int2 p) {
    if (!flagOn(131072u)) return LOAD_ND(Nd, p);
    return float4(LOAD_ND(Nd, p >> 1).xyz, linDepth(GDepth.Load(int3(p, 0))));
}
float4 upHalf(Texture2D<float4> Half, int2 id, float4 nd) {
    uint hk = 0u;  // (the half size's pixel of each 2 x 2: 2x, 2y)
    int2 off = int2(hk & 1u, hk >> 1u), hm = int2(((uint2)rs + 1u) >> 1) - 1, m = int2(rs) - 1;
    int2 hb = int2(floor((float2(id) - float2(off)) * 0.5));
    float4 acc = 0.0, best = 0.0;
    float ws = 0.0, bd = 1e30;
    [unroll] for (int k = 0; k < 4; k++) {
        int2 h = clamp(hb + int2(k & 1, k >> 1), int2(0, 0), hm);
        int2 s = min(h * 2 + off, m);
        float4 sn = flagOn(131072u) ? LOAD_ND(Nd, h) : LOAD_ND(Nd, s);  // (131072: Nd the half size's)
        float4 v = Half.Load(int3(h, 0));
        float2 dd = float2(s - id);
        float w = geoWeight(nd.w, sn.w, nd.xyz, sn.xyz) / (1.0 + dot(dd, dd));
        acc += v * w;
        ws += w;
        float e = abs(sn.w - nd.w);
        if (sn.w > 0.0 && e < bd) bd = e, best = v;
    }
    return ws > 1e-4 ? acc / ws : best;
}
// ... the same, the GI also guided by the colour (the composition): each texel's GI x how near the game's colour at the
// pixel it was computed for is to this pixel's, in stops (e^(-stops^2 / 4.5), 1.5 stops wide) - its edges along the
// colour's, not blurred across a light's or a material's border; the AO by depth and normal alone
float4 upHalfGuided(Texture2D<float4> Half, Texture2D<float4> Col, int2 id, float4 nd) {
    uint hk = 0u;
    int2 off = int2(hk & 1u, hk >> 1u), hm = int2(((uint2)rs + 1u) >> 1) - 1, m = int2(rs) - 1;
    int2 hb = int2(floor((float2(id) - float2(off)) * 0.5));
    const float3 Y = float3(0.2126, 0.7152, 0.0722);
    float lc = max(dot(max(Col.Load(int3(id, 0)).rgb, 0.0), Y), 1e-4);
    float3 gi = 0.0;
    float ao = 0.0, ws = 0.0, wg = 0.0, bd = 1e30;
    float4 best = 0.0;
    [unroll] for (int k = 0; k < 4; k++) {
        int2 h = clamp(hb + int2(k & 1, k >> 1), int2(0, 0), hm);
        int2 s = min(h * 2 + off, m);
        float4 sn = flagOn(131072u) ? LOAD_ND(Nd, h) : LOAD_ND(Nd, s);
        float4 v = Half.Load(int3(h, 0));
        float2 dd = float2(s - id);
        float w = geoWeight(nd.w, sn.w, nd.xyz, sn.xyz) / (1.0 + dot(dd, dd));
        float st = log2(max(dot(max(Col.Load(int3(s, 0)).rgb, 0.0), Y), 1e-4) / lc);
        float w2 = w * exp(-st * st / 4.5);
        ao += v.a * w, ws += w;
        gi += v.rgb * w2, wg += w2;
        float e = abs(sn.w - nd.w);
        if (sn.w > 0.0 && e < bd) bd = e, best = v;
    }
    if (ws <= 1e-4) return best;
    return float4(wg > 1e-6 ? gi / wg : best.rgb, ao / ws);
}
