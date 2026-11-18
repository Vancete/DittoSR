// Ditto Relight (experimental), FastGI's GI (giMode 1), its Trace at its low resolution (fgDiv): per ray (cosine-
// distributed around the normal, rotated by the noise) up to fgSteps depth checks along it, quadratically spaced out to
// fgRadius; at the first behind the depth buffer, refined by two bisections (fgRefine), and within fgThick the hit's
// colour counts - x its facing the ray, its confidence (fgReject: off the ray, on the receiver's plane, facing away or
// seen edge-on) and a fade toward the screen's edges and the ray's end; behind it by more, the ray stops there (no light
// through walls). Its colour point-sampled, its luminance clamped to fgClamp. The receivers past fgMaxDist none, fading
// over its last 20 %. As FastGI, but the normals the prepass' (rebuilt from the depth: the game gives none) and the
// noise moving every frame (DittoSR's accumulation averages it - FastGI moves it under a temporal resolve).
// t0 = the prepass' normal and depth, t1 = the game's colour, t2 = the game's depth; u0 (low) = rgb the radiance, a the
// depth (0: the sky).
#include "ao_common.hlsli"

Texture2D<float4> Nd : register(t0);
Texture2D<float4> Color : register(t1);
Texture2D<float> Depth : register(t2);  // (the march's depth checks: 4 bytes a read, not the prepass' 16)
RWTexture2D<float4> Out : register(u0);

float zAt(float2 uv) { return linDepth(Depth.Load(int3(clamp(int2(uv * rs), int2(0, 0), int2(rs) - 1), 0))); }

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    uint2 ls = fgLow();
    if (any(id >= ls)) return;
    int2 p = fgFull(int2(id)), m = int2(rs) - 1;
    float4 nd = Nd.Load(int3(p, 0));
    float depth = nd.w;
    if (depth <= 0.0) { Out[id] = 0.0; return; }
    if (depth >= fgMaxDist) { Out[id] = float4(0.0, 0.0, 0.0, min(depth, 60000.0)); return; }
    float3 normal = nd.xyz;
    float2 uv = (float2(p) + 0.5) * rsRcp;
    float3 origin = viewPos(uv, depth) + normal * fgBias;
    float3 tangent, bitangent;
    basisOf(normal, tangent, bitangent);
    float noise = frac(ign(float2(id)) + fgPhase);
    uint rays = clamp((uint)fgRays, 1u, 8u), steps = clamp((uint)fgSteps, 4u, 32u);
    float zn = min(zNear, zFar);
    float3 radiance = 0.0;
    [loop] for (uint ray = 0u; ray < rays; ray++) {
        float u = (float(ray) + 0.5) / float(rays);
        float angle = 6.2831853 * frac(noise + float(ray) * 0.618033989);
        float3 direction = tangent * (sqrt(u) * cos(angle)) + bitangent * (sqrt(u) * sin(angle)) + normal * sqrt(1.0 - u);
        float previous = 0.0;
        [loop] for (uint i = 0u; i < steps; i++) {
            float t = (float(i) + 0.75 + noise * 0.25) / float(steps);
            float along = fgRadius * t * t;
            float3 position = origin + direction * along;
            if (position.z <= zn) break;
            float2 hitUV = projUv(position);
            if (any(hitUV <= 0.0) || any(hitUV >= 1.0)) break;
            float sceneZ = zAt(hitUV);
            if (sceneZ <= 0.0) { previous = along; continue; }  // (the sky)
            float delta = position.z - sceneZ;
            if (delta < 0.0) { previous = along; continue; }
            if (fgRefine > 0.5) {  // (only the first crossing refined, not every step)
                float lo = previous, hi = along;
                [unroll] for (int k = 0; k < 2; k++) {
                    float mid = 0.5 * (lo + hi);
                    float3 mp = origin + direction * mid;
                    float2 muv = projUv(mp);
                    if (any(muv <= 0.0) || any(muv >= 1.0)) break;
                    float mz = zAt(muv);
                    if (mz > 0.0 && mp.z - mz >= 0.0) { hi = mid; hitUV = muv; sceneZ = mz; delta = mp.z - mz; }
                    else lo = mid;
                }
                along = hi;
                t = sqrt(hi / fgRadius);
            }
            // (the first occluder stops the ray even beyond the thickness: no light through walls)
            if (delta <= fgThick && along > fgBias * 2.0) {
                int2 hp = clamp(int2(hitUV * rs), int2(0, 0), m);
                float3 hitNormal = Nd.Load(int3(hp, 0)).xyz;
                float facing = saturate(dot(hitNormal, -direction) * 4.0);
                float confidence = 1.0;
                if (fgReject > 0.0) {
                    float3 hitPos = viewPos(hitUV, sceneZ), toHit = hitPos - origin;
                    float onRay = dot(toHit, direction), offRay = length(toHit - direction * onRay);
                    float footprint = 2.0 * sceneZ * rsRcp.y * tanY;
                    float tolerance = max(0.01, min(fgThick * 0.25, 0.1) + footprint * 1.5);
                    float geometry = saturate(1.0 - offRay / tolerance);
                    // (a depth-slab crossing is not enough: the surface must face the receiver)
                    float3 actual = toHit * rsqrt(max(dot(toHit, toHit), 1e-6));
                    geometry *= smoothstep(0.0, 0.15, dot(normal, actual)) * smoothstep(0.0, 0.15, dot(hitNormal, -actual));
                    geometry *= smoothstep(0.05, 0.25, dot(hitNormal, -normalize(hitPos)));
                    geometry *= step(fgBias, onRay) * step(onRay, fgRadius);
                    confidence = lerp(1.0, geometry, fgReject);
                }
                float2 edge = min(hitUV, 1.0 - hitUV);
                float fade = saturate(min(edge.x, edge.y) * 20.0) * (1.0 - t * t);
                float3 color = max(Color.Load(int3(hp, 0)).rgb, 0.0);
                color *= min(1.0, fgClamp / max(dot(color, float3(0.2126, 0.7152, 0.0722)), 1e-4));  // (no hue shift)
                radiance += color * facing * fade * confidence;
            }
            break;
        }
    }
    float distanceFade = saturate((fgMaxDist - depth) / max(1.0, fgMaxDist * 0.2));
    Out[id] = float4(radiance * (distanceFade / float(rays)), min(depth, 60000.0));
}
