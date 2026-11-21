// Ditto Relight (experimental), the lighting extras (flags2 16384; D3D12) - 9 sliders, each 0 off (fx(i): 0 - 1), applied
// by the composition to the relit colour before its curves. What they read: aoscene.hlsl's analysis (Scene, t6: the sky, the
// sun's place, the dominant light's direction and colour, the emissive lights), the mips (level 3 at t5: the area's luma;
// level 2 at t9: colour and depth for the rays), the game's depth (GDepth, t3). Included after those are declared.
//   0 emissive lights: each light's colour x r^2 / (d^2 + r^2) x the cosine at the pixel, x the albedo - a neon's spill;
//   1 local exposure: the colour x (a mid grey / its area's luma) ^ (0.5 x fx) - the dark areas lifted, the bright ones
//     brought down, the contrast within them kept (UE5's local exposure's way);
//   2 sky ambient: the sky's tint where the pixel is darker than its area (in shade) and faces up;
//   3 contact shadows: 8 steps toward the dominant light through the game's depth - something in the way: darker, on the
//     faces toward the light, by how sure the direction is;
//   4 aerial perspective: toward the sky's colour by e^(-depth / (5 x the AO's fade distance)) - distance haze;
//   5 god rays: 12 steps toward the sun's place, the sky's light along them added;
//   6 reflections: on faces pointing up, 10 steps along the reflected view through level 2's depth - its colour where it
//     hits, the sky's where it leaves upward - x Schlick's Fresnel;
//   7 foliage translucency: green pixels lit from behind (the dominant light behind them, toward the camera);
//   8 shadow tint: the shade toward the dominant light's complementary colour.
float fx(uint i) {
    uint w = i < 4u ? params1 >> (8u * i) : i < 8u ? params2 >> (8u * (i - 4u)) : flags2 >> 24;
    return float(w & 255u) / 255.0;
}

float3 lightingExtras(float3 o, float3 col, float3 albedo, float4 nd, int2 p) {
    const float3 Y = float3(0.2126, 0.7152, 0.0722);
    float2 uv = (float2(p) + 0.5) * rsRcp;
    float3 Pv = viewPos(uv, nd.w), n = nd.xyz, V = -normalize(Pv);
    float4 sky = Scene.Load(int3(0, 0, 0)), sun = Scene.Load(int3(1, 0, 0)), dom = Scene.Load(int3(2, 0, 0)), domc = Scene.Load(int3(3, 0, 0));
    float3 skyC = sky.a > 0.0 ? sky.rgb : 0.0;
    // (the area's luma: level 3's 3 x 3 around the pixel on its own surface - each texel x a Gaussian x how near its depth
    // is, its luma at most 4 x the pixel's: a light beside it no longer darkens the whole cell it falls in, as with the
    // better albedo)
    float lum = dot(o, Y), la = 0.0, wa = 0.0;
    {
        uint2 d3 = ((uint2)rs + 7u) >> 3;
        float2 t = uv * float2(d3) - 0.5;
        int2 tb = int2(floor(t + 0.5));
        float lp = dot(col, Y);
        [unroll] for (int k = 0; k < 9; k++) {
            int2 q = clamp(tb + int2(k % 3 - 1, k / 3 - 1), int2(0, 0), int2(d3) - 1);
            float3 c3;
            float4 m = mipDecode(Mip3c.Load(int3(q, 0)), c3);
            float2 dd = float2(q) - t;
            float w = exp(-dot(dd, dd) * 0.7) * (m.w > 0.0 ? saturate(1.0 - abs(m.w - nd.w) / (nd.w * 0.05 + 1e-3)) : 0.0);
            la += min(dot(c3, Y), 4.0 * lp + 1e-3) * w, wa += w;
        }
        la = max(wa > 1e-3 ? la / wa : lp, 1e-4);
    }
    float shade = saturate(1.0 - lum / (0.6 * la));
    bool hdr = flag2On(2048u);
    // 0 emissive lights
    float s = fx(0u);
    if (s > 0.0) {
        float3 add = 0.0;
        [loop] for (uint i = 0u; i < 16u; i++) {
            float4 lp = Scene.Load(int3(16 + i, 0, 0));
            if (lp.w <= 0.0) continue;
            float3 d = lp.xyz - Pv;
            float d2 = dot(d, d), r2 = lp.w * lp.w;
            if (d2 > 1600.0 * r2) continue;
            add += Scene.Load(int3(32 + i, 0, 0)).rgb * (r2 / (d2 + r2)) * saturate(dot(n, d * rsqrt(max(d2, 1e-8))));
        }
        o += albedo * add * 2.0 * s;
    }
    // 1 local exposure
    s = fx(1u);
    if (s > 0.0) o *= pow(clamp((hdr ? 0.18 : 0.5) / la, 0.25, 4.0), 0.5 * s);
    // 2 sky ambient
    s = fx(2u);
    if (s > 0.0 && sky.a > 0.0) {
        float3 tint = skyC / max(dot(skyC, Y), 1e-4);
        o += albedo * tint * la * shade * saturate(0.5 + 0.5 * n.y) * 0.5 * s;
    }
    // 3 contact shadows
    s = fx(3u);
    if (s > 0.0 && dom.w > 0.05) {
        float occ = 0.0, tt = 0.003 * nd.w;
        [loop] for (uint i = 0u; i < 8u; i++, tt *= 1.6) {
            float3 Q = Pv + dom.xyz * tt;
            if (Q.z <= 0.0) break;
            float2 q = projUv(Q);
            if (any(q <= 0.0) || any(q >= 1.0)) break;
            float sd = linDepth(GDepth.Load(int3(int2(q * rs), 0)));
            if (sd > 0.0 && sd < Q.z * 0.995 && Q.z - sd < 0.05 * nd.w) { occ = 1.0; break; }
        }
        o *= 1.0 - 0.6 * s * occ * saturate(dom.w * 4.0) * saturate(dot(n, dom.xyz) * 4.0);
    }
    // 4 aerial perspective
    s = fx(4u);
    if (s > 0.0 && fadeZ > 0.0 && sky.a > 0.0) o = lerp(o, skyC, (1.0 - exp(-nd.w / (5.0 * fadeZ))) * s);
    // 5 god rays
    s = fx(5u);
    if (s > 0.0 && sun.w > 0.0) {
        uint2 d2 = ((uint2)rs + 3u) >> 2;
        float3 acc = 0.0;
        [loop] for (uint i = 1u; i <= 12u; i++) {
            float2 q = lerp(uv, sun.xy, float(i) / 12.0);
            float3 c2;
            float4 m = mipDecode(Mip2x.Load(int3(min(int2(q * float2(d2)), int2(d2) - 1), 0)), c2);
            if (m.w <= 0.0) acc += c2 * (1.0 - float(i) / 13.0);
        }
        o += acc / 12.0 * 0.5 * s;
    }
    // 6 reflections
    s = fx(6u);
    if (s > 0.0 && n.y > 0.5) {
        float3 R = reflect(-V, n);
        uint2 d2 = ((uint2)rs + 3u) >> 2;
        float3 hit = 0.0;
        bool found = false;
        float tt = 0.01 * nd.w;
        [loop] for (uint i = 0u; i < 10u; i++, tt *= 1.5) {
            float3 Q = Pv + R * tt;
            if (Q.z <= 0.0) break;
            float2 q = projUv(Q);
            if (any(q <= 0.0) || any(q >= 1.0)) break;
            float3 c2;
            float4 m = mipDecode(Mip2x.Load(int3(min(int2(q * float2(d2)), int2(d2) - 1), 0)), c2);
            if (m.w > 0.0 && m.w < Q.z && Q.z - m.w < 0.1 * Q.z) { hit = c2, found = true; break; }
        }
        if (!found && R.y > 0.0) hit = skyC, found = sky.a > 0.0;
        float fres = 0.04 + 0.96 * pow(1.0 - saturate(dot(n, V)), 5.0);
        if (found) o += hit * fres * saturate((n.y - 0.5) * 2.0) * s;
    }
    // 7 foliage translucency
    s = fx(7u);
    if (s > 0.0 && dom.w > 0.05) {
        float green = saturate((col.g - max(col.r, col.b)) / max(col.g, 1e-4) * 2.0);
        o += col * green * saturate(dot(dom.xyz, -V)) * saturate(dom.w * 4.0) * 1.5 * s;
    }
    // 8 shadow tint
    s = fx(8u);
    if (s > 0.0) {
        float3 dc = domc.rgb / max(max(domc.r, max(domc.g, domc.b)), 1e-4), comp = saturate(1.0 - dc) + 0.2;
        comp /= max(dot(comp, Y), 1e-4);
        float l2 = dot(o, Y);
        o = lerp(o, comp * l2, shade * 0.35 * s);
    }
    return max(o, 0.0);
}
