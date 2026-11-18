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
    float giStrength;     // 0 - 2: the bounce light added
    float radiusWorld;    // the occluders' reach, view units (ini ao_radius; 0: the screen's cap alone)
    float radiusScreen;   // ... at most this share of the screen's height (ini ao_screen)
    float aoPad0, aoPad1;
};

SamplerState linearClamp : register(s0);

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

// interleaved gradient noise (Jimenez 2014)
float ign(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }
