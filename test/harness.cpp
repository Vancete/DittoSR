// Offline test of the DLL: drives <dir>\<dll> through the FidelityFX API as a game does, upscaling a synthetic HDR scene
// rendered jittered at render size with reversed-Z depth and motion vectors. What upscales follows <dir>\dittosr.ini
// (enabled=0 with AMD's DLL next to it as <dll>.orig.dll: AMD's FSR, for comparison).
//   harness.exe <dir> <png prefix> [key=value ...]
//     dll=amd_fidelityfx_dx12.dll (or amd_fidelityfx_loader_dx12.dll)
//     scene=tiles     a detailed background panning, a nearer square moving the other way (disocclusion, ghosting)
//     scene=glints    a dark ground with small, very bright HDR sparks panning (flashes: flick=1's sparkle)
//     scene=grass     thin grass blades over a cracked ground, the camera panning (one motion for every depth: a turn)
//     frames=64, ratio=1.5 (output / render; output 1440x810), vel=3 (grass: output pixels a frame), sharpen=0,
//                     dense=1 (grass: blades x dense, thinner by its square root), wobble=0 (grass: the speed vel - wobble, vel,
//                     vel + wobble in turn), blur=0 (grass: motion blur over
//                     blur x vel output pixels, in the render and the reference alike - the game's, before the upscale)
//     stop=0          grass: the pan stops after this frame (stop=24: how fast the image gets sharp again at rest)
//     sky=0           grass: 1 - dark blades against a bright HDR sky (6 - 25)
//     wind=0          grass: the blades sway in the wind, their tips this many pixels (period ~19 frames), with no
//                     motion vectors of their own - as games animate foliage and hair
//     spec=0          1: bright reflections sliding at half the pan's speed, no motion vectors of their own
//     light=0         1: a shadow sweeping across the screen with no motion vectors (a change only the clip and the
//                     reaction catch)
//     radiance=1      the colour handed over x this (a game in physical units: pair it with exposure_scale=1/K in the
//                     ini); the output / K is compared. A NaN or infinite output pixel counts as black
//     velocity=, reactive=  FSR 3.1's velocity factor and reactiveness scale through ffxConfigure (OptiScaler's sliders
//                     and presets; DittoSR: the frame weight)
//     maxres=0        1: the inputs' textures at the output size, the render in their top-left (magenta outside)
//     switch=N:R      the ratio R from frame N on, in the same textures (a resolution change, a ratio override)
//     autoexp=0       1: FSR's auto exposure flag (no exposure texture given - the upscaler measures it, as The Witcher 3 asks)
//     mvs=1           the motion vectors handed to the upscaler x this (0.8: 20 % short - a game's imprecise ones; the
//                     clip has to catch what they miss)
//     out=1440x810    the output size (the render: out / ratio); psnr=0: the GPU time alone (no reference, no PNGs)
//     api=dx12        or vk: the upscaler on Vulkan (dll=amd_fidelityfx_vk.dll) - a device with no extension and no
//                     optional feature, as the least a game may hand over; gpu= its physical device's index
//     outfmt=rgba16f  or rgba8: the output's format (the Vulkan passes are compiled per output format)
// Reported: the upscale's GPU time (timestamps either side of its dispatch, the median); PSNR against the scene
// supersampled 4x4 at output size (in a perceptual domain: sqrt of Reinhard), at the
// middle and last frames, PNGs of both; with an integer pan (grass), the shimmer - the mean difference between frame t
// and frame t - 1 shifted by the pan over the last 16 frames, 0 for a perfect upscaler (x 1000, perceptual); the flicker,
// any scene and speed, with flick=1: over the last 8 frames, on a central 640 x 360, the output's change between frames against the
// reference's (x 1000, perceptual).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <vector>

#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_upscale.h"
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "ffx_api/vk/ffx_api_vk.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 0x0000002u
struct ffxCreateBackendDX12Desc {
    ffxCreateContextDescHeader header;
    ID3D12Device *device;
};

static uint32_t OW = 1440, OH = 810;  // (out=)
static uint32_t RW = 960, RH = 540;
static bool g_grass, g_glints;
static float g_vel = 3.0f, g_dense = 1.0f, g_blur = 0.0f, g_mvs = 1.0f, g_radiance = 1.0f;
static int g_wobble = 0, g_light = 0, g_sky = 0, g_spec = 0;
static int g_stop = 0;  // (stop=: the pan stops after this frame - how fast the image gets sharp again)
static float g_wind = 0.0f;  // (wind=: the blades' tips sway this many output pixels, with no motion vectors of their own)
// the pan's speed at frame t (output pixels): vel, or vel - wobble, vel, vel + wobble in turn - a hand on the mouse
static float pan_vel(int t) {
    if (g_stop > 0 && t > g_stop) return 0.0f;
    return t <= 0 ? g_vel : g_vel + g_wobble * ((t * 7) % 3 - 1);
}
static float pan_pos(int t) {  // (the sum of pan_vel(1 .. t))
    if (g_stop > 0 && t > g_stop) t = g_stop;
    return g_vel * t + (t % 3 == 2 ? g_wobble : 0);
}
static const float BG_VEL[2] = {1.7f, 0.6f};  // (tiles: output pixels per frame)
static const float FG_VEL[2] = {-4.3f, 2.1f};

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("%s failed 0x%08x (line %d)\n", #x, (unsigned)hr_, __LINE__); exit(1); } } while (0)

// ---- the scenes, in output pixels at frame t
struct Sample { float rgb[3]; float depth; float mv[2]; };  // mv: output pixels, the previous position - this one

static float bg(float x, float y) {
    // fine diagonal lines, a zone plate, and hard-edged bright tiles (HDR highlights)
    float r2 = (x - 700) * (x - 700) + (y - 400) * (y - 400);
    float zp = 0.5f + 0.5f * cosf(r2 * 0.00035f);
    float lines = fmodf(fabsf(x * 0.7f + y * 0.3f), 9.0f) < 1.2f ? 1.0f : 0.0f;
    int tx = (int)floorf(x / 64.0f), ty = (int)floorf(y / 64.0f);
    float tile = ((tx + ty) & 1) ? 0.15f : 0.05f;
    float bright = ((tx * 7 + ty * 3) % 11 == 0) ? 8.0f : 0.0f;
    return tile + 0.4f * zp + 0.6f * lines + bright * (fmodf(x, 64.0f) > 32.0f ? 1.0f : 0.0f);
}
static Sample tiles(float x, float y, int t) {
    Sample s;
    float fx = 300 + FG_VEL[0] * t, fy = 200 + FG_VEL[1] * t;
    fx = fmodf(fmodf(fx, (float)OW) + OW, (float)OW), fy = fmodf(fmodf(fy, (float)OH) + OH, (float)OH);
    float lx = x - fx, ly = y - fy;
    if (lx >= 0 && lx < 220 && ly >= 0 && ly < 160) {
        float v = (fmodf(lx, 20.0f) < 3.0f || fmodf(ly, 20.0f) < 3.0f) ? 1.0f : 0.3f;
        s.rgb[0] = 1.5f * v, s.rgb[1] = 0.4f * v, s.rgb[2] = 0.2f * v;
        s.depth = 0.8f;  // (reversed-Z: nearer)
        s.mv[0] = -FG_VEL[0], s.mv[1] = -FG_VEL[1];  // (the square moves by +FG_VEL: it was FG_VEL back)
        return s;
    }
    float sx = x + BG_VEL[0] * t, sy = y + BG_VEL[1] * t;
    float v = bg(sx, sy);
    s.rgb[0] = v, s.rgb[1] = v * 0.9f + 0.05f, s.rgb[2] = v * 0.8f + 0.1f * bg(sx * 0.5f, sy * 0.5f);
    s.depth = 0.1f;
    s.mv[0] = BG_VEL[0], s.mv[1] = BG_VEL[1];
    return s;
}

static uint32_t hash(int32_t a, int32_t b) {
    uint32_t h = (uint32_t)a * 0x8da6b343u ^ (uint32_t)b * 0xd8163841u;
    h ^= h >> 13, h *= 0x5bd1e995u, h ^= h >> 15;
    return h;
}
static float hf(int32_t a, int32_t b, int k) { return (float)(hash(a * 7 + k, b * 13 - k) & 0xffff) / 65535.0f; }
// grass: blades (thin slanted segments, nearer) every few pixels in rows, over a sunlit cracked ground; the camera pans
// by vel output pixels a frame, every depth alike (a turn of the camera)
static Sample grass(float x, float y, int t) {
    Sample s;
    float sx = x + pan_pos(t), sy = y;
    s.mv[0] = pan_vel(t), s.mv[1] = 0;
    const float CW = 5.0f / g_dense, RHt = 70.0f;
    int cx0 = (int)floorf(sx / CW), ry0 = (int)floorf(sy / RHt);
    int reach = (int)ceilf(20.0f / CW);
    for (int ry = ry0; ry <= ry0 + 1; ry++)
        for (int cx = cx0 - reach; cx <= cx0 + reach; cx++) {
            if (hf(cx, ry, 0) < 0.35f) continue;
            float bx = (cx + hf(cx, ry, 1)) * CW, by = ry * RHt + 20.0f * hf(cx, ry, 2);  // (the base)
            float h = 25.0f + 45.0f * hf(cx, ry, 3), lean = (hf(cx, ry, 4) - 0.5f) * 0.9f;
            float w = (0.6f + 0.9f * hf(cx, ry, 5)) / sqrtf(g_dense);
            float u = (by - sy) / h;  // (0 at the base, 1 at the tip)
            if (u < 0 || u > 1) continue;
            float px = bx + lean * h * u * u;
            if (g_wind > 0.0f) px += g_wind * sinf(0.33f * t + 6.2832f * hf(cx, ry, 17)) * u * u;
            float hw = 0.5f * w * (1.0f - 0.7f * u);
            if (fabsf(sx - px) < hw) {
                float v = 0.35f + 0.9f * hf(cx, ry, 6) * (0.4f + u);
                if (g_sky) v *= 0.15f;  // (sky=1: dark blades against it)
                s.rgb[0] = 1.1f * v, s.rgb[1] = 0.9f * v, s.rgb[2] = 0.45f * v;
                s.depth = 0.4f + 0.1f * hf(cx, ry, 7);
                return s;
            }
        }
    if (g_sky) {  // sky=1: a bright HDR sky behind the blades, 6 - 25 (foliage against the sky, a neon wall)
        float k = 6.0f + 19.0f * (0.5f + 0.5f * sinf(sx * 0.004f + sy * 0.003f));
        s.rgb[0] = 0.8f * k, s.rgb[1] = 0.9f * k, s.rgb[2] = k;
        s.depth = 0.0f;
        return s;
    }
    // the ground: sunlit, cracks (a dark polygonal network) and grains
    float g = 1.4f + 0.25f * sinf(sx * 0.031f) * sinf(sy * 0.047f);
    float cxf = sx / 37.0f, cyf = sy / 29.0f;
    int ix = (int)floorf(cxf), iy = (int)floorf(cyf);
    float fx = cxf - ix - 0.5f + 0.3f * (hf(ix, iy, 8) - 0.5f), fy = cyf - iy - 0.5f + 0.3f * (hf(ix, iy, 9) - 0.5f);
    float edge = 0.5f - fmaxf(fabsf(fx), fabsf(fy));
    if (edge < 0.025f) g *= 0.35f;
    g *= 0.85f + 0.3f * hf((int)floorf(sx * 0.7f), (int)floorf(sy * 0.7f), 10);
    s.rgb[0] = g, s.rgb[1] = 0.8f * g, s.rgb[2] = 0.55f * g;
    s.depth = 0.2f;
    return s;
}
// light=1: a shadow - a disc of radius 160 with a 3-pixel edge, 70 % darker - sweeping across the screen by (5, 2.5)
// output pixels a frame, with no motion vectors of its own (the surfaces' stay): a change only the clip and the reaction
// catch, as a game's moving shadows, flashlights and animated lights
static float lightAt(float x, float y, int t) {
    float cx = fmodf(200.0f + 5.0f * t, (float)OW), cy = fmodf(150.0f + 2.5f * t, (float)OH);
    float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
    float in = fminf(fmaxf((160.0f - d) / 3.0f + 0.5f, 0.0f), 1.0f);
    return 1.0f - 0.7f * in;
}
static Sample sceneLit(float x, float y, int t);
static Sample scene(float x, float y, int t) {
    Sample s = sceneLit(x, y, t);
    if (g_spec) {  // spec=1: reflections - bright spots (15 - 50, 1.5 - 4 pixels, soft-edged) sliding at half the
                   // pan's speed across the surface, with the surface's motion vectors only (a reflection's parallax)
        float sx = x + 0.5f * pan_pos(t), sy = y;
        const float C = 61.0f;
        int cx0 = (int)floorf(sx / C), cy0 = (int)floorf(sy / C);
        float add = 0.0f;
        for (int cy = cy0 - 1; cy <= cy0 + 1; cy++)
            for (int cx = cx0 - 1; cx <= cx0 + 1; cx++) {
                if (hf(cx, cy, 20) > 0.6f) continue;
                float px = (cx + hf(cx, cy, 21)) * C, py = (cy + hf(cx, cy, 22)) * C, r = 1.5f + 2.5f * hf(cx, cy, 23);
                float d = sqrtf((sx - px) * (sx - px) + (sy - py) * (sy - py));
                add += (15.0f + 35.0f * hf(cx, cy, 24)) * fminf(fmaxf((r - d) / 1.0f + 0.5f, 0.0f), 1.0f);
            }
        for (int c = 0; c < 3; c++) s.rgb[c] += add;
    }
    if (g_light) {
        float l = lightAt(x, y, t);
        for (int c = 0; c < 3; c++) s.rgb[c] *= l;
    }
    return s;
}
// glints: a dark ground with small, very bright HDR sparks - discs of 0.25 - 0.9 pixels at 6 - 40, a few short
// streaks - panning by vel as the grass (a city's distant lights, sun on wet stones, specular sparkles): what a blend
// that overshoots shows as a flash
static Sample glints(float x, float y, int t) {
    Sample s;
    float sx = x + pan_pos(t), sy = y;
    s.mv[0] = pan_vel(t), s.mv[1] = 0;
    s.depth = 0.2f;
    float g = 0.08f + 0.06f * hf((int)floorf(sx * 0.5f), (int)floorf(sy * 0.5f), 11) + 0.04f * sinf(sx * 0.02f) * sinf(sy * 0.03f);
    const float CX = 23.0f, CY = 19.0f;
    int cx0 = (int)floorf(sx / CX), cy0 = (int)floorf(sy / CY);
    float v = 0.0f;
    for (int cy = cy0 - 1; cy <= cy0 + 1; cy++)
        for (int cx = cx0 - 1; cx <= cx0 + 1; cx++) {
            float p = hf(cx, cy, 12);
            if (p > 0.45f) continue;
            float px = (cx + hf(cx, cy, 13)) * CX, py = (cy + hf(cx, cy, 14)) * CY;
            float in = 6.0f + 34.0f * hf(cx, cy, 15);
            if (p < 0.08f) {  // (a streak: 1 x 7 pixels, slanted)
                float u = (sx - px) * 0.94f + (sy - py) * 0.34f, w = -(sx - px) * 0.34f + (sy - py) * 0.94f;
                if (fabsf(u) < 3.5f && fabsf(w) < 0.5f) v = fmaxf(v, in * 0.5f);
            } else {
                float r = 0.25f + 0.65f * hf(cx, cy, 16);
                if ((sx - px) * (sx - px) + (sy - py) * (sy - py) < r * r) v = fmaxf(v, in);
            }
        }
    float c = g + v;
    s.rgb[0] = c, s.rgb[1] = 0.95f * c, s.rgb[2] = 0.85f * c;
    return s;
}
static Sample sceneLit(float x, float y, int t) {
    if (g_glints) return glints(x, y, t);
    if (!g_grass) return tiles(x, y, t);
    if (g_blur <= 0.0f) return grass(x, y, t);
    // (the game's motion blur, before the upscale: the pan averaged over blur x vel output pixels)
    Sample s = grass(x, y, t), a;
    for (int c = 0; c < 3; c++) s.rgb[c] = 0;
    for (int k = 0; k < 8; k++) {
        a = grass(x + pan_vel(t) * g_blur * ((k + 0.5f) / 8 - 0.5f), y, t);
        for (int c = 0; c < 3; c++) s.rgb[c] += a.rgb[c] / 8;
    }
    return s;
}

static float perceptual(float c) {
    c = c > 0 ? c : 0;
    return sqrtf(c / (1 + c));
}

static uint16_t f2h(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000, e = (x >> 23) & 0xff, m = x & 0x7fffff;
    if (e == 0xff) return (uint16_t)(sign | 0x7c00 | (m ? 0x200 : 0));
    int ne = (int)e - 127 + 15;
    if (ne >= 31) return (uint16_t)(sign | 0x7c00);
    if (ne <= 0) {
        if (ne < -10) return (uint16_t)sign;
        m |= 0x800000;
        return (uint16_t)(sign | (m >> (14 - ne)));
    }
    return (uint16_t)(sign | (ne << 10) | (m >> 13));
}
static float h2f(uint16_t h) {
    uint32_t sign = (h & 0x8000) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff, x;
    if (e == 0) {
        if (!m) x = sign;
        else { e = 127 - 15 + 1; while (!(m & 0x400)) m <<= 1, e--; m &= 0x3ff; x = sign | (e << 23) | (m << 13); }
    } else if (e == 31) x = sign | 0x7f800000 | (m << 13);
    else x = sign | ((e - 15 + 127) << 23) | (m << 13);
    float f;
    memcpy(&f, &x, 4);
    return f;
}

static float halton(int i, int b) {
    float f = 1, r = 0;
    for (; i > 0; i /= b) f /= (float)b, r += f * (float)(i % b);
    return r;
}

struct Gpu {
    ID3D12Device *dev;
    ID3D12CommandQueue *q;
    ID3D12CommandAllocator *ca;
    ID3D12GraphicsCommandList *cl;
    ID3D12Fence *fence;
    UINT64 fv = 0;
    HANDLE ev;
    void wait() {
        q->Signal(fence, ++fv);
        if (fence->GetCompletedValue() < fv) fence->SetEventOnCompletion(fv, ev), WaitForSingleObject(ev, INFINITE);
    }
};

static ID3D12Resource *tex(Gpu &g, uint32_t w, uint32_t h, DXGI_FORMAT f, bool uav, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, rd.Width = w, rd.Height = h, rd.DepthOrArraySize = 1, rd.MipLevels = 1;
    rd.Format = f, rd.SampleDesc.Count = 1, rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource *r;
    CHECK(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r)));
    return r;
}
static ID3D12Resource *buffer(Gpu &g, UINT64 size, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES hp = {type};
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, rd.Width = size, rd.Height = 1, rd.DepthOrArraySize = 1, rd.MipLevels = 1;
    rd.SampleDesc.Count = 1, rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *r;
    CHECK(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                         type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
                                         nullptr, IID_PPV_ARGS(&r)));
    return r;
}
static void barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER br = {};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, br.Transition.StateBefore = a, br.Transition.StateAfter = b;
    cl->ResourceBarrier(1, &br);
}
// an upload of rows (bpp bytes a texel) into a texture (in COPY_DEST)
static void upload(Gpu &g, ID3D12Resource *up, UINT64 &off, ID3D12Resource *dst, const void *data, uint32_t w, uint32_t h, uint32_t bpp) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    D3D12_RESOURCE_DESC dd = dst->GetDesc();
    UINT64 total;
    g.dev->GetCopyableFootprints(&dd, 0, 1, off, &fp, nullptr, nullptr, &total);
    uint8_t *p;
    up->Map(0, nullptr, (void **)&p);
    for (uint32_t y = 0; y < h; y++) memcpy(p + fp.Offset + (UINT64)y * fp.Footprint.RowPitch, (const uint8_t *)data + (size_t)y * w * bpp, (size_t)w * bpp);
    up->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION s = {up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, d = {dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    s.PlacedFootprint = fp;
    g.cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    off = (fp.Offset + total + 511) & ~511ull;
}

static FfxApiResource res(ID3D12Resource *r, uint32_t state) {
    FfxApiResource a = {};
    a.resource = r, a.state = state;
    if (r) {
        D3D12_RESOURCE_DESC d = r->GetDesc();
        a.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D, a.description.width = (uint32_t)d.Width, a.description.height = d.Height;
        a.description.depth = 1, a.description.mipCount = 1;
    }
    return a;
}

// ---- the graphics API the upscaler runs on: D3D12 (api=dx12) or Vulkan (api=vk) ----
enum Kind { K_COLOR, K_DEPTH, K_MV, K_OUT };
static uint32_t ffx_format(Kind k, bool out8) {
    return k == K_COLOR ? FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT : k == K_DEPTH ? FFX_API_SURFACE_FORMAT_R32_FLOAT
           : k == K_MV ? FFX_API_SURFACE_FORMAT_R16G16_FLOAT : out8 ? FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM
                                                                     : FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
}
struct Tex {
    void *r = nullptr;
    Kind k;
    uint32_t w, h;
    bool out8 = false;
    int layout = 0;  // (Vulkan: 0 undefined, 1 its working layout)
};
struct Api {
    virtual ~Api() {}
    virtual bool init(int gpu) = 0;
    virtual const char *adapter() = 0;
    virtual ffxCreateContextDescHeader *backend() = 0;  // (the context's backend description)
    virtual void *device() = 0;
    virtual Tex texture(uint32_t w, uint32_t h, Kind k, bool out8) = 0;
    // the render's three inputs (rows of w texels: 8, 4 and 4 bytes) uploaded, submitted, waited for
    virtual void upload(Tex *const in[3], const void *const data[3], uint32_t w, uint32_t h) = 0;
    virtual void *begin() = 0;  // the dispatch's command list, its first timestamp written
    virtual void end(Tex &out, bool grab) = 0;  // the second timestamp, the output copied (grab), submitted, waited for
    virtual double gpu_ms() = 0;
    virtual const uint8_t *map(size_t *pitch) = 0;  // (the output end(grab) copied)
    virtual void unmap() = 0;
    virtual bool lost() = 0;
    double wallMs = 0.0;  // (the last end()'s submit to idle)
    FfxApiResource res(const Tex &t) {
        FfxApiResource a = {};
        a.resource = t.r;
        a.state = t.k == K_OUT ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
        a.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D, a.description.width = t.w, a.description.height = t.h;
        a.description.depth = 1, a.description.mipCount = 1, a.description.format = ffx_format(t.k, t.out8);
        if (t.k == K_OUT) a.description.usage = FFX_API_RESOURCE_USAGE_UAV;
        return a;
    }
};
static double qpc_ms() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t), QueryPerformanceFrequency(&f);
    return 1000.0 * t.QuadPart / f.QuadPart;
}

struct Dx12 : Api {
    Gpu g = {};
    ffxCreateBackendDX12Desc be = {};
    ID3D12Resource *up = nullptr, *rb = nullptr, *qrb = nullptr;
    ID3D12QueryHeap *qh = nullptr;
    UINT64 tsFreq = 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT ofp = {};
    char name[256] = "";
    const D3D12_RESOURCE_STATES SRV = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bool first = true;
    bool init(int gpu) override {
        // the GPU with the most memory (not the integrated one), or gpu=: DXGI's adapter index
        IDXGIFactory6 *fac;
        CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)));
        IDXGIAdapter1 *best = nullptr, *a;
        SIZE_T bestMem = 0;
        for (UINT i = 0; fac->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d;
            a->GetDesc1(&d);
            if (gpu >= 0 ? (int)i == gpu : !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory > bestMem)
                best = a, bestMem = d.DedicatedVideoMemory;
        }
        if (!best) return false;
        DXGI_ADAPTER_DESC1 ad;
        best->GetDesc1(&ad);
        snprintf(name, sizeof name, "%ls (D3D12)", ad.Description);
        CHECK(D3D12CreateDevice(best, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.dev)));
        D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
        CHECK(g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.q)));
        CHECK(g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.ca)));
        CHECK(g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.ca, nullptr, IID_PPV_ARGS(&g.cl)));
        CHECK(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)));
        g.ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        g.cl->Close();
        be = {{FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12, nullptr}, g.dev};
        up = buffer(g, 96ull << 20, D3D12_HEAP_TYPE_UPLOAD);
        D3D12_QUERY_HEAP_DESC qhd = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2};
        CHECK(g.dev->CreateQueryHeap(&qhd, IID_PPV_ARGS(&qh)));
        qrb = buffer(g, 16, D3D12_HEAP_TYPE_READBACK);
        g.q->GetTimestampFrequency(&tsFreq);
        return true;
    }
    const char *adapter() override { return name; }
    ffxCreateContextDescHeader *backend() override { return &be.header; }
    void *device() override { return g.dev; }
    Tex texture(uint32_t w, uint32_t h, Kind k, bool out8) override {
        Tex t;
        t.k = k, t.w = w, t.h = h, t.out8 = out8;
        DXGI_FORMAT f = k == K_COLOR ? DXGI_FORMAT_R16G16B16A16_FLOAT : k == K_DEPTH ? DXGI_FORMAT_R32_TYPELESS
                        : k == K_MV ? DXGI_FORMAT_R16G16_FLOAT : out8 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
        ID3D12Resource *r = tex(g, w, h, f, k == K_OUT, k == K_OUT ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COPY_DEST);
        t.r = r;
        if (k == K_OUT) {
            D3D12_RESOURCE_DESC od = r->GetDesc();
            UINT64 obytes;
            g.dev->GetCopyableFootprints(&od, 0, 1, 0, &ofp, nullptr, nullptr, &obytes);
            rb = buffer(g, obytes, D3D12_HEAP_TYPE_READBACK);
        }
        return t;
    }
    void upload(Tex *const in[3], const void *const data[3], uint32_t w, uint32_t h) override {
        g.ca->Reset();
        g.cl->Reset(g.ca, nullptr);
        if (!first)
            for (int i = 0; i < 3; i++) barrier(g.cl, (ID3D12Resource *)in[i]->r, SRV, D3D12_RESOURCE_STATE_COPY_DEST);
        UINT64 off = 0;
        const uint32_t bpp[3] = {8, 4, 4};
        for (int i = 0; i < 3; i++) ::upload(g, up, off, (ID3D12Resource *)in[i]->r, data[i], w, h, bpp[i]);
        for (int i = 0; i < 3; i++) barrier(g.cl, (ID3D12Resource *)in[i]->r, D3D12_RESOURCE_STATE_COPY_DEST, SRV);
        CHECK(g.cl->Close());
        ID3D12CommandList *lists[1] = {g.cl};
        g.q->ExecuteCommandLists(1, lists);
        g.wait();
        first = false;
    }
    void *begin() override {
        g.ca->Reset();
        g.cl->Reset(g.ca, nullptr);
        g.cl->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        return g.cl;
    }
    void end(Tex &out, bool grab) override {
        ID3D12Resource *o = (ID3D12Resource *)out.r;
        g.cl->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        g.cl->ResolveQueryData(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, qrb, 0);
        if (grab) {
            barrier(g.cl, o, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION s = {o, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, d = {rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            d.PlacedFootprint = ofp;
            g.cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            barrier(g.cl, o, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        CHECK(g.cl->Close());
        ID3D12CommandList *lists[1] = {g.cl};
        double t0 = qpc_ms();
        g.q->ExecuteCommandLists(1, lists);
        g.wait();
        wallMs = qpc_ms() - t0;
    }
    double gpu_ms() override {
        UINT64 *ts;
        D3D12_RANGE rr = {0, 16};
        qrb->Map(0, &rr, (void **)&ts);
        double ms = 1000.0 * (double)(ts[1] - ts[0]) / (double)tsFreq;
        qrb->Unmap(0, nullptr);
        return ms;
    }
    const uint8_t *map(size_t *pitch) override {
        uint8_t *mp;
        rb->Map(0, nullptr, (void **)&mp);
        *pitch = ofp.Footprint.RowPitch;
        return mp;
    }
    void unmap() override { rb->Unmap(0, nullptr); }
    bool lost() override {
        if (g.dev->GetDeviceRemovedReason() == S_OK) return false;
        printf("device removed 0x%08x\n", (unsigned)g.dev->GetDeviceRemovedReason());
        return true;
    }
};

#define VKCHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("%s failed %d (line %d)\n", #x, (int)r_, __LINE__); exit(1); } } while (0)
// (AMD's FSR on Vulkan - as <dll>.orig.dll, the comparison - needs device features this bare device lacks: DittoSR alone)
struct Vk : Api {
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
#define VKI(n) PFN_##n n = nullptr;
    VKI(vkCreateInstance) VKI(vkEnumeratePhysicalDevices) VKI(vkGetPhysicalDeviceProperties) VKI(vkGetPhysicalDeviceQueueFamilyProperties)
    VKI(vkGetPhysicalDeviceMemoryProperties) VKI(vkCreateDevice) VKI(vkEnumerateInstanceLayerProperties)
    VKI(vkGetDeviceQueue) VKI(vkCreateCommandPool) VKI(vkAllocateCommandBuffers) VKI(vkBeginCommandBuffer) VKI(vkEndCommandBuffer)
    VKI(vkQueueSubmit) VKI(vkQueueWaitIdle) VKI(vkResetCommandPool) VKI(vkCreateImage) VKI(vkGetImageMemoryRequirements)
    VKI(vkAllocateMemory) VKI(vkBindImageMemory) VKI(vkCreateBuffer) VKI(vkGetBufferMemoryRequirements) VKI(vkBindBufferMemory)
    VKI(vkMapMemory) VKI(vkCmdPipelineBarrier) VKI(vkCmdCopyBufferToImage) VKI(vkCmdCopyImageToBuffer) VKI(vkCreateQueryPool)
    VKI(vkCmdResetQueryPool) VKI(vkCmdWriteTimestamp) VKI(vkGetQueryPoolResults)
#undef VKI
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue q = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkQueryPool qp = VK_NULL_HANDLE;
    double tickMs = 0.0;
    VkPhysicalDeviceMemoryProperties mp = {};
    uint8_t *upMap = nullptr, *rbMap = nullptr;
    VkBuffer up = VK_NULL_HANDLE, rb = VK_NULL_HANDLE;
    size_t rbPitch = 0;
    ffxCreateBackendVKDesc be = {};
    char name[300] = "";

    uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want) {
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
        printf("no memory type 0x%x\n", (unsigned)want), exit(1);
    }
    VkDeviceMemory alloc(VkMemoryRequirements mr, VkMemoryPropertyFlags want) {
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size, ai.memoryTypeIndex = memtype(mr.memoryTypeBits, want);
        VkDeviceMemory m;
        VKCHECK(vkAllocateMemory(dev, &ai, nullptr, &m));
        return m;
    }
    uint8_t *hostBuffer(VkDeviceSize size, VkBufferUsageFlags use, VkBuffer *b) {
        VkBufferCreateInfo bc = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bc.size = size, bc.usage = use;
        VKCHECK(vkCreateBuffer(dev, &bc, nullptr, b));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(dev, *b, &mr);
        VkDeviceMemory m = alloc(mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VKCHECK(vkBindBufferMemory(dev, *b, m, 0));
        void *p;
        VKCHECK(vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, &p));
        return (uint8_t *)p;
    }
    bool init(int gpu) override {
        HMODULE lib = LoadLibraryA("vulkan-1.dll");
        if (!lib || !(gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(lib, "vkGetInstanceProcAddr"))) return printf("no vulkan-1.dll\n"), false;
#define G(n) n = (PFN_##n)gipa(inst, #n);
        G(vkCreateInstance) G(vkEnumerateInstanceLayerProperties)
        VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "DittoSR harness", app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ic = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ic.pApplicationInfo = &app;
        // (the validation layer where it is installed - the Vulkan SDK's)
        const char *val = "VK_LAYER_KHRONOS_validation";
        uint32_t nl = 0;
        vkEnumerateInstanceLayerProperties(&nl, nullptr);
        std::vector<VkLayerProperties> lp(nl);
        vkEnumerateInstanceLayerProperties(&nl, lp.data());
        for (auto &l : lp)
            if (!strcmp(l.layerName, val)) ic.enabledLayerCount = 1, ic.ppEnabledLayerNames = &val, printf("validation layer on\n");
        VKCHECK(vkCreateInstance(&ic, nullptr, &inst));
        G(vkEnumeratePhysicalDevices) G(vkGetPhysicalDeviceProperties) G(vkGetPhysicalDeviceQueueFamilyProperties)
        G(vkGetPhysicalDeviceMemoryProperties) G(vkCreateDevice)
#undef G
        gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(inst, &n, nullptr);
        std::vector<VkPhysicalDevice> pd(n);
        vkEnumeratePhysicalDevices(inst, &n, pd.data());
        // (the discrete GPU, or gpu=: the physical device's index)
        VkPhysicalDeviceProperties pp = {};
        for (uint32_t i = 0; i < n; i++) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(pd[i], &p);
            if (gpu >= 0 ? (int)i == gpu : (!phys || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) && pp.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                phys = pd[i], pp = p;
        }
        if (!phys) return printf("no Vulkan device\n"), false;
        snprintf(name, sizeof name, "%s (Vulkan %u.%u)", pp.deviceName, VK_VERSION_MAJOR(pp.apiVersion), VK_VERSION_MINOR(pp.apiVersion));
        tickMs = pp.limits.timestampPeriod * 1e-6;
        vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        uint32_t nq = 0, qf = UINT32_MAX;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qfp(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qfp.data());
        for (uint32_t i = 0; i < nq && qf == UINT32_MAX; i++)
            if ((qfp[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) qf = i;
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qc = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qc.queueFamilyIndex = qf, qc.queueCount = 1, qc.pQueuePriorities = &prio;
        VkDeviceCreateInfo dc = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dc.queueCreateInfoCount = 1, dc.pQueueCreateInfos = &qc;  // (no extension, no feature)
        VKCHECK(vkCreateDevice(phys, &dc, nullptr, &dev));
#define D(n) n = (PFN_##n)gdpa(dev, #n);
        D(vkGetDeviceQueue) D(vkCreateCommandPool) D(vkAllocateCommandBuffers) D(vkBeginCommandBuffer) D(vkEndCommandBuffer)
        D(vkQueueSubmit) D(vkQueueWaitIdle) D(vkResetCommandPool) D(vkCreateImage) D(vkGetImageMemoryRequirements)
        D(vkAllocateMemory) D(vkBindImageMemory) D(vkCreateBuffer) D(vkGetBufferMemoryRequirements) D(vkBindBufferMemory)
        D(vkMapMemory) D(vkCmdPipelineBarrier) D(vkCmdCopyBufferToImage) D(vkCmdCopyImageToBuffer) D(vkCreateQueryPool)
        D(vkCmdResetQueryPool) D(vkCmdWriteTimestamp) D(vkGetQueryPoolResults)
#undef D
        vkGetDeviceQueue(dev, qf, 0, &q);
        VkCommandPoolCreateInfo pc = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pc.queueFamilyIndex = qf;
        VKCHECK(vkCreateCommandPool(dev, &pc, nullptr, &pool));
        VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = pool, ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, ca.commandBufferCount = 1;
        VKCHECK(vkAllocateCommandBuffers(dev, &ca, &cb));
        VkQueryPoolCreateInfo qpc = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpc.queryType = VK_QUERY_TYPE_TIMESTAMP, qpc.queryCount = 2;
        VKCHECK(vkCreateQueryPool(dev, &qpc, nullptr, &qp));
        upMap = hostBuffer(96ull << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &up);
        be = {{FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK, nullptr}, dev, phys, gdpa};
        return true;
    }
    const char *adapter() override { return name; }
    ffxCreateContextDescHeader *backend() override { return &be.header; }
    void *device() override { return dev; }
    Tex texture(uint32_t w, uint32_t h, Kind k, bool out8) override {
        Tex t;
        t.k = k, t.w = w, t.h = h, t.out8 = out8;
        VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType = VK_IMAGE_TYPE_2D, ic.extent = {w, h, 1}, ic.mipLevels = 1, ic.arrayLayers = 1, ic.samples = VK_SAMPLE_COUNT_1_BIT;
        ic.format = k == K_COLOR ? VK_FORMAT_R16G16B16A16_SFLOAT : k == K_DEPTH ? VK_FORMAT_R32_SFLOAT : k == K_MV ? VK_FORMAT_R16G16_SFLOAT
                    : out8 ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R16G16B16A16_SFLOAT;
        ic.tiling = VK_IMAGE_TILING_OPTIMAL, ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ic.usage = VK_IMAGE_USAGE_SAMPLED_BIT | (k == K_OUT ? VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT : VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        VkImage im;
        VKCHECK(vkCreateImage(dev, &ic, nullptr, &im));
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(dev, im, &mr);
        VKCHECK(vkBindImageMemory(dev, im, alloc(mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), 0));
        t.r = (void *)im;
        if (k == K_OUT) rbPitch = (size_t)w * (out8 ? 4 : 8), rbMap = hostBuffer(rbPitch * h, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rb);
        return t;
    }
    void layout(Tex &t, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = from, b.newLayout = to, b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = (VkImage)t.r, b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    void record() {
        VKCHECK(vkResetCommandPool(dev, pool, 0));
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VKCHECK(vkBeginCommandBuffer(cb, &bi));
    }
    void submit() {
        VKCHECK(vkEndCommandBuffer(cb));
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1, si.pCommandBuffers = &cb;
        VKCHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
        VKCHECK(vkQueueWaitIdle(q));
    }
    void upload(Tex *const in[3], const void *const data[3], uint32_t w, uint32_t h) override {
        record();
        const uint32_t bpp[3] = {8, 4, 4};
        VkDeviceSize off = 0;
        for (int i = 0; i < 3; i++) {
            Tex &t = *in[i];
            memcpy(upMap + off, data[i], (size_t)w * h * bpp[i]);
            layout(t, t.layout ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy c = {};
            c.bufferOffset = off, c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, c.imageExtent = {w, h, 1};
            vkCmdCopyBufferToImage(cb, up, (VkImage)t.r, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            layout(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            t.layout = 1;
            off = (off + (VkDeviceSize)w * h * bpp[i] + 255) & ~255ull;
        }
        submit();
    }
    Tex *outTex = nullptr;
    void *begin() override {
        record();
        vkCmdResetQueryPool(cb, qp, 0, 2);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
        return (void *)cb;
    }
    void end(Tex &out, bool grab) override {
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
        if (grab) {
            layout(out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy c = {};
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, c.imageExtent = {out.w, out.h, 1};
            vkCmdCopyImageToBuffer(cb, (VkImage)out.r, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &c);
            layout(out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        }
        double t0 = qpc_ms();
        submit();
        wallMs = qpc_ms() - t0;
    }
    // (the output: GENERAL from the start, as the dispatch is told it is - UNORDERED_ACCESS)
    void prepare(Tex &out) {
        record();
        layout(out, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        submit();
    }
    double gpu_ms() override {
        uint64_t ts[2] = {};
        if (vkGetQueryPoolResults(dev, qp, 0, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) return 0.0;
        return (double)(ts[1] - ts[0]) * tickMs;
    }
    const uint8_t *map(size_t *pitch) override { return *pitch = rbPitch, rbMap; }
    void unmap() override {}
    bool lost() override { return false; }  // (a lost device fails the next submit: VKCHECK)
};

static const char *opt(int argc, char **argv, const char *key, const char *def) {
    size_t n = strlen(key);
    for (int i = 3; i < argc; i++)
        if (!strncmp(argv[i], key, n) && argv[i][n] == '=') return argv[i] + n + 1;
    return def;
}

int main(int argc, char **argv) {
    if (argc < 3) return printf("harness.exe <dir> <png prefix> [dll=] [scene=tiles|grass] [frames=] [out=WxH] [ratio=] [vel=] [dense=] [wobble=] [blur=] [mvs=] [light=] [sharpen=] [psnr=1] [flick=0] [gpu=index]\n"), 1;
    const char *dir = argv[1], *prefix = argv[2];
    bool vk = !strcmp(opt(argc, argv, "api", "dx12"), "vk");
    bool out8 = !strcmp(opt(argc, argv, "outfmt", "rgba16f"), "rgba8");
    const char *dll = opt(argc, argv, "dll", vk ? "amd_fidelityfx_vk.dll" : "amd_fidelityfx_dx12.dll");
    int frames = atoi(opt(argc, argv, "frames", "64"));
    sscanf(opt(argc, argv, "out", "1440x810"), "%ux%u", &OW, &OH);
    bool measure = atoi(opt(argc, argv, "psnr", "1")) != 0;
    bool flick = atoi(opt(argc, argv, "flick", "0")) != 0;
    bool sharpen = atoi(opt(argc, argv, "sharpen", "0")) != 0;
    g_grass = !strcmp(opt(argc, argv, "scene", "tiles"), "grass");
    g_glints = !strcmp(opt(argc, argv, "scene", "tiles"), "glints");
    g_vel = (float)atof(opt(argc, argv, "vel", "3"));
    g_wobble = atoi(opt(argc, argv, "wobble", "0"));
    g_stop = atoi(opt(argc, argv, "stop", "0"));
    g_dense = (float)atof(opt(argc, argv, "dense", "1"));
    g_blur = (float)atof(opt(argc, argv, "blur", "0"));
    g_mvs = (float)atof(opt(argc, argv, "mvs", "1"));
    g_radiance = (float)atof(opt(argc, argv, "radiance", "1"));
    g_light = atoi(opt(argc, argv, "light", "0"));
    g_sky = atoi(opt(argc, argv, "sky", "0"));
    g_spec = atoi(opt(argc, argv, "spec", "0"));
    g_wind = (float)atof(opt(argc, argv, "wind", "0"));
    float ratio = (float)atof(opt(argc, argv, "ratio", "1.5"));
    RW = (uint32_t)(OW / ratio), RH = (uint32_t)(OH / ratio);
    float r = (float)OW / RW;
    // maxres=1: the colour, depth and motion textures at the output size, the render in their top-left RW x RH - as a game
    // with dynamic resolution or an upscale ratio override renders; the rest magenta (a read outside shows). switch=N:R:
    // the ratio R from frame N on (with maxres), the textures as they were - a game's resolution change mid-play
    int swFrame = -1;
    float swRatio = 0;
    if (const char *sw = opt(argc, argv, "switch", nullptr)) sscanf(sw, "%d:%f", &swFrame, &swRatio);
    bool maxres = atoi(opt(argc, argv, "maxres", swFrame >= 0 ? "1" : "0")) != 0;
    const uint32_t TW = maxres ? OW : RW, TH = maxres ? OH : RH;
    // (a pan by a whole number of pixels every lag frames: frame t = frame t - lag shifted, for a perfect upscaler - lag 1
    // for an integer speed, 2 for x.5 (the history then resampled half a pixel off: the worst case), 4 for x.25)
    int lag = 0;
    for (int k : {1, 2, 4})
        if (!lag && (k == 1 || !g_wobble) && fabsf(g_vel * k - roundf(g_vel * k)) < 1e-4f) lag = k;
    bool shimmer = (g_grass || g_glints) && lag > 0 && !g_light && !g_spec && g_wind <= 0.0f;  // (the light breaks the pan's shift: no shimmer then)

    Api *api = vk ? (Api *)new Vk() : (Api *)new Dx12();
    if (!api->init(atoi(opt(argc, argv, "gpu", "-1")))) return printf("no GPU\n"), 1;
    printf("adapter: %s; %s, %s scene, %ux%u -> %ux%u%s\n", api->adapter(), dll, g_glints ? "glints" : g_grass ? "grass" : "tiles", RW, RH,
           OW, OH, out8 ? ", RGBA8 output" : "");

    char p[MAX_PATH];
    SetDllDirectoryA(dir);  // (AMD's loader, as <dll>.orig.dll, loads its upscaler DLL by name: the folder, as a game's)
    snprintf(p, sizeof p, "%s\\%s", dir, dll);
    HMODULE m = LoadLibraryA(p);
    if (!m) return printf("cannot load %s (%lu)\n", p, GetLastError()), 1;
    ffxContext ctx = nullptr;
    int32_t phases = 0;
    auto create = (PfnFfxCreateContext)GetProcAddress(m, "ffxCreateContext");
    auto destroy = (PfnFfxDestroyContext)GetProcAddress(m, "ffxDestroyContext");
    auto dispatch = (PfnFfxDispatch)GetProcAddress(m, "ffxDispatch");
    auto query = (PfnFfxQuery)GetProcAddress(m, "ffxQuery");
    if (!create || !destroy || !dispatch || !query) return printf("FidelityFX API exports missing\n"), 1;
    ffxCreateContextDescUpscale cd = {};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, cd.header.pNext = api->backend();
    cd.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
    if (atoi(opt(argc, argv, "autoexp", "0"))) cd.flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;  // (no exposure given: the upscaler's own)
    cd.maxRenderSize = {RW, RH}, cd.maxUpscaleSize = {OW, OH};
    if (create(&ctx, &cd.header, nullptr) != FFX_API_RETURN_OK) return printf("ffxCreateContext failed\n"), 1;
    // velocity=, reactive=: FSR 3.1's velocity factor and reactiveness scale configured, as OptiScaler's sliders and
    // presets do (DittoSR: the frame weight)
    for (int k = 0; k < 2; k++)
        if (const char *v = opt(argc, argv, k ? "reactive" : "velocity", nullptr)) {
            auto configure = (PfnFfxConfigure)GetProcAddress(m, "ffxConfigure");
            float vf = (float)atof(v);
            ffxConfigureDescUpscaleKeyValue kv = {};
            kv.header.type = FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE;
            kv.key = k ? FFX_API_CONFIGURE_UPSCALE_KEY_FREACTIVENESSSCALE : FFX_API_CONFIGURE_UPSCALE_KEY_FVELOCITYFACTOR, kv.ptr = &vf;
            printf("ffxConfigure %s %g: %u\n", k ? "reactiveness scale" : "velocity factor", vf, (unsigned)configure(&ctx, &kv.header));
        }
    ffxQueryDescUpscaleGetJitterPhaseCount pq = {{FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT}, RW, OW, &phases};
    query(&ctx, &pq.header);
    // (the upscaler's providers, as a game lists them before choosing one)
    uint64_t nv = 8, ids[8];
    const char *names[8];
    ffxQueryDescGetVersions gv = {{FFX_API_QUERY_DESC_TYPE_GET_VERSIONS}, FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, api->device(), &nv, ids, names};
    if (query(nullptr, &gv.header) == FFX_API_RETURN_OK) {
        printf("providers:");
        for (uint64_t i = 0; i < nv && i < 8; i++) printf(" %s (0x%llx)", names[i] ? names[i] : "?", (unsigned long long)ids[i]);
        printf("\n");
    }
    ffxQueryGetProviderVersion pv = {{FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION}};
    if (query(&ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName) printf("provider: %s\n", pv.versionName);

    Tex color = api->texture(TW, TH, K_COLOR, false), depth = api->texture(TW, TH, K_DEPTH, false), mv = api->texture(TW, TH, K_MV, false);
    Tex out = api->texture(OW, OH, K_OUT, out8);
    if (vk) ((Vk *)api)->prepare(out);
    std::vector<double> gpuMs;

    std::vector<uint16_t> cbuf(TW * TH * 4), mbuf(TW * TH * 2, 0);
    std::vector<float> dbuf(TW * TH, 0.0f), cur(OW * OH * 3);
    for (size_t i = 0; i < (size_t)TW * TH; i++)  // (outside the render: magenta, bright)
        cbuf[i * 4] = f2h(20.0f), cbuf[i * 4 + 1] = f2h(0.0f), cbuf[i * 4 + 2] = f2h(20.0f), cbuf[i * 4 + 3] = f2h(1.0f);
    std::vector<std::vector<float>> ring(lag > 0 ? lag : 1, std::vector<float>(OW * OH * 3));
    double psnrSum = 0, shimSum = 0, upMs = 0, flickSum = 0, sparkSum = 0, sparkPx = 0;
    int sparkN = 0;
    double sparkMax = 0;  // (the worst pixel: its excess over the reference's 3x3 brightest, x that brightest)
    int psnrN = 0, shimN = 0, flickN = 0;
    const uint32_t FLICK_W = OW < 640 ? OW : 640, FLICK_H = OH < 360 ? OH : 360;
    const int FLICK_N = 9;
    std::vector<float> fo(FLICK_W * FLICK_H * 3), fop(fo.size()), fg(fo.size()), fgp(fo.size()), lo(fo.size()), lg(fo.size());
    std::vector<float> curLin(OW * OH * 3);
    // (psnr=0, the GPU time alone: the first 16 frames rendered once and replayed - the CPU's render between upscales
    // left the GPU idle long enough to drop its clocks, and the time measured was its power saving)
    const int kCached = 16;
    std::vector<std::vector<uint16_t>> cc(measure ? 0 : kCached), mc(measure ? 0 : kCached);
    std::vector<std::vector<float>> dc(measure ? 0 : kCached);
    for (int t = 0; t < frames; t++) {
        if (t == swFrame && swRatio > 0) {  // (the new ratio: a new render size inside the same textures, a new jitter cycle)
            RW = (uint32_t)(OW / swRatio), RH = (uint32_t)(OH / swRatio), r = (float)OW / RW;
            ffxQueryDescUpscaleGetJitterPhaseCount pq2 = {{FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT}, RW, OW, &phases};
            query(&ctx, &pq2.header);
            printf("frame %d: render %ux%u\n", t, RW, RH);
        }
        int k = t % phases + 1;
        float jx = halton(k, 2) - 0.5f, jy = halton(k, 3) - 0.5f;
        bool replay = !measure && t >= kCached;
        if (replay) cbuf = cc[t % kCached], dbuf = dc[t % kCached], mbuf = mc[t % kCached];
        // the render: each pixel's one sample where the jitter put it (p + 0.5 - jitter), in output pixels
        for (uint32_t y = 0; y < RH && !replay; y++)
            for (uint32_t x = 0; x < RW; x++) {
                Sample s = scene((x + 0.5f - jx) * r, (y + 0.5f - jy) * r, t);
                size_t i = (size_t)y * TW + x;
                for (int c = 0; c < 3; c++) cbuf[i * 4 + c] = f2h(s.rgb[c] * g_radiance);
                cbuf[i * 4 + 3] = f2h(1.0f);
                dbuf[i] = s.depth;
                // (the previous position = this one + mv x scale, here in uv with scale = render size)
                mbuf[i * 2] = f2h(g_mvs * s.mv[0] / OW), mbuf[i * 2 + 1] = f2h(g_mvs * s.mv[1] / OH);
            }
        if (!measure && t < kCached) cc[t] = cbuf, dc[t] = dbuf, mc[t] = mbuf;
        {
            Tex *in[3] = {&color, &depth, &mv};
            const void *data[3] = {cbuf.data(), dbuf.data(), mbuf.data()};
            api->upload(in, data, TW, TH);
        }
        // the upscale alone in its own command list (its wall-clock time)
        {
            ffxDispatchDescUpscale dd = {};
            dd.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
            dd.commandList = api->begin();
            dd.color = api->res(color), dd.depth = api->res(depth), dd.motionVectors = api->res(mv), dd.output = api->res(out);
            dd.jitterOffset = {jx, jy};
            dd.motionVectorScale = {(float)RW, (float)RH};
            dd.renderSize = {RW, RH}, dd.upscaleSize = {OW, OH};
            dd.enableSharpening = sharpen, dd.sharpness = sharpen ? 0.5f : 0.0f;
            dd.frameTimeDelta = 16.6f, dd.preExposure = 1.0f, dd.reset = t == 0;
            dd.cameraNear = 0.1f, dd.cameraFar = 1e6f, dd.cameraFovAngleVertical = 1.0f, dd.viewSpaceToMetersFactor = 1.0f;
            ffxReturnCode_t rc = dispatch(&ctx, &dd.header);
            if (rc != FFX_API_RETURN_OK) return printf("ffxDispatch failed %u at frame %d\n", rc, t), 1;
        }
        // (with stop=: also the frames 1, 2, 4, 8 ... after the pan stopped)
        int ts = t - g_stop;
        bool psnrFrame = measure && (t == frames - 1 || t == frames / 2 || (g_stop > 0 && ts > 0 && (ts & (ts - 1)) == 0));
        bool shimFrame = shimmer && t >= frames - 16 - lag;
        bool flickFrame = measure && flick && t >= frames - FLICK_N;
        bool grab = psnrFrame || shimFrame || flickFrame;
        api->end(out, grab);
        if (t >= 8) upMs += api->wallMs, gpuMs.push_back(api->gpu_ms());
        if (api->lost()) return 1;
        if (!grab) continue;
        size_t pitch;
        const uint8_t *mp = api->map(&pitch);
        for (uint32_t y = 0; y < OH; y++) {
            const uint16_t *row = (const uint16_t *)(mp + (size_t)y * pitch);
            const uint8_t *row8 = mp + (size_t)y * pitch;
            for (uint32_t x = 0; x < OW; x++)
                for (int c = 0; c < 3; c++) {
                    float v = (out8 ? row8[x * 4 + c] / 255.0f : h2f(row[x * 4 + c])) / g_radiance;
                    if (!(v == v) || v > 1e30f) v = -1.0f;  // (NaN or infinite: counted as black, as a tonemapper shows it)
                    cur[((size_t)y * OW + x) * 3 + c] = perceptual(v), curLin[((size_t)y * OW + x) * 3 + c] = v;
                }
        }
        api->unmap();
        // the shimmer: this frame against the one lag frames back, shifted by the pan (away from the borders)
        if (shimFrame && t >= frames - 16) {
            const std::vector<float> &prev = ring[t % lag];
            int sh = (int)lroundf(pan_pos(t) - pan_pos(t - lag));
            double sum = 0;
            size_t n = 0;
            for (uint32_t y = 8; y < OH - 8; y++)
                for (uint32_t x = 8; x + sh < OW - 8; x++)
                    for (int c = 0; c < 3; c++, n++) sum += fabs(cur[((size_t)y * OW + x) * 3 + c] - prev[((size_t)y * OW + x + sh) * 3 + c]);
            shimSum += sum / n, shimN++;
        }
        // the flicker, any motion: on a central crop, the output's change from the previous frame against the
        // reference's - mean |(out_t - out_t-1) - (ref_t - ref_t-1)|, 0 for a perfect upscaler at any speed
        if (flickFrame) {
            uint32_t cx0 = (OW - FLICK_W) / 2, cy0 = (OH - FLICK_H) / 2;
            for (uint32_t y = 0; y < FLICK_H; y++)
                for (uint32_t x = 0; x < FLICK_W; x++) {
                    float ref[3] = {0, 0, 0};
                    for (int sy = 0; sy < 4; sy++)
                        for (int sx = 0; sx < 4; sx++) {
                            Sample s = scene(cx0 + x + (sx + 0.5f) / 4, cy0 + y + (sy + 0.5f) / 4, t);
                            for (int c = 0; c < 3; c++) ref[c] += s.rgb[c] / 16;
                        }
                    for (int c = 0; c < 3; c++) {
                        size_t i = ((size_t)y * FLICK_W + x) * 3 + c;
                        fo[i] = cur[((size_t)(cy0 + y) * OW + cx0 + x) * 3 + c], fg[i] = perceptual(ref[c]);
                        lo[i] = curLin[((size_t)(cy0 + y) * OW + cx0 + x) * 3 + c], lg[i] = ref[c];
                    }
                }
            if (t > frames - FLICK_N) {
                double sum = 0;
                for (size_t i = 0; i < fo.size(); i++) sum += fabs((fo[i] - fop[i]) - (fg[i] - fgp[i]));
                flickSum += sum / fo.size(), flickN++;
            }
            // the sparkle: light that is not there - in linear HDR, as the game's bloom after the upscale sees it: the
            // output's luma above the reference's brightest in its 3 x 3 (0 for a perfect upscaler; near white the
            // perceptual domain hides it - a small error there is a large linear one: a flash once bloomed)
            {
                auto Y = [&](const std::vector<float> &v, size_t i) { return 0.25f * v[i * 3] + 0.5f * v[i * 3 + 1] + 0.25f * v[i * 3 + 2]; };
                double ex = 0;
                float emax = 0;
                size_t nb = 0, np = 0;
                for (uint32_t y = 1; y + 1 < FLICK_H; y++)
                    for (uint32_t x = 1; x + 1 < FLICK_W; x++, np++) {
                        float rm = 0;
                        for (int j = -1; j <= 1; j++)
                            for (int i = -1; i <= 1; i++) rm = fmaxf(rm, Y(lg, (size_t)(y + j) * FLICK_W + x + i));
                        float e = Y(lo, (size_t)y * FLICK_W + x) - rm;
                        if (e > 0.0f) ex += e;
                        emax = fmaxf(emax, rm > 0 ? e / rm : 0);
                        if (e > 1.0f) nb++;  // (a pixel a whole unit of light too bright)
                    }
                sparkSum += ex / np, sparkPx += 100.0 * nb / np, sparkN++;
                sparkMax = fmax(sparkMax, emax);
            }
            std::swap(fo, fop), std::swap(fg, fgp);
        }
        if (shimFrame) ring[t % lag] = cur;
        if (!psnrFrame) continue;
        const std::vector<float> &o = cur;
        // the comparison: the output against the scene supersampled 4x4 at output size
        std::vector<uint8_t> img(OW * OH * 3), gt(OW * OH * 3);
        double se = 0;
        for (uint32_t y = 0; y < OH; y++)
            for (uint32_t x = 0; x < OW; x++) {
                float ref[3] = {0, 0, 0};
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++) {
                        Sample s = scene(x + (sx + 0.5f) / 4, y + (sy + 0.5f) / 4, t);
                        for (int c = 0; c < 3; c++) ref[c] += s.rgb[c] / 16;
                    }
                for (int c = 0; c < 3; c++) {
                    size_t i = ((size_t)y * OW + x) * 3 + c;
                    float e = perceptual(ref[c]);
                    se += (double)(o[i] - e) * (o[i] - e);
                    img[i] = (uint8_t)(fminf(o[i], 1.0f) * 255 + 0.5f);
                    gt[i] = (uint8_t)(fminf(e, 1.0f) * 255 + 0.5f);
                }
            }
        double mse = se / (OW * OH * 3.0), psnr = 10 * log10(1.0 / (mse > 1e-12 ? mse : 1e-12));
        psnrSum += psnr, psnrN++;
        printf("frame %d: PSNR %.2f dB\n", t, psnr);
        char f[MAX_PATH];
        snprintf(f, sizeof f, "%s_f%03d.png", prefix, t);
        stbi_write_png(f, OW, OH, 3, img.data(), OW * 3);
        snprintf(f, sizeof f, "%s_gt_f%03d.png", prefix, t);
        stbi_write_png(f, OW, OH, 3, gt.data(), OW * 3);
    }
    // (the GPU time: the median of the frames after the first 8 - the pipelines and caches warm)
    std::sort(gpuMs.begin(), gpuMs.end());
    double gmed = gpuMs.empty() ? 0.0 : gpuMs[gpuMs.size() / 2];
    if (psnrN) printf("mean PSNR %.2f dB, ", psnrSum / psnrN);
    if (shimN) printf("shimmer %.2f, ", 1000.0 * shimSum / shimN);
    if (flickN) printf("flicker %.2f, ", 1000.0 * flickSum / flickN);
    if (sparkN) printf("sparkle %.3f (%.3f %% px, worst x%.2f), ", 1000.0 * sparkSum / sparkN, sparkPx / sparkN, sparkMax);
    printf("%.3f ms GPU an upscale (median; %.3f ms wall clock)\n", gmed, upMs / (frames - 8));
    // menu=WxH: the in-game menu as the Vulkan window draws it (its frame times those of these upscales), over a grey
    // checkerboard - <prefix>_menu.png
    if (const char *mo = opt(argc, argv, "menu", nullptr)) {
        typedef int (*SnapFn)(int, int, int, uint32_t *);
        auto snap = (SnapFn)GetProcAddress(m, "DittoSRMenuSnapshot");
        int mw = 1920, mh = 1080, sel = atoi(opt(argc, argv, "menusel", "2"));
        sscanf(mo, "%dx%d", &mw, &mh);
        std::vector<uint32_t> px((size_t)mw * mh);
        double s0 = qpc_ms();
        bool got = snap && snap(mw, mh, sel, px.data());  // (the first: the atlas built too)
        double atlasMs = qpc_ms() - s0;
        s0 = qpc_ms();
        for (int i = 0; got && i < 4; i++) snap(mw, mh, sel, px.data());
        double snapMs = (qpc_ms() - s0) / 4;
        if (!got) printf("no menu snapshot\n");
        else {
            std::vector<uint8_t> img((size_t)mw * mh * 3);
            for (int y = 0; y < mh; y++)
                for (int x = 0; x < mw; x++) {
                    uint32_t p = px[(size_t)y * mw + x];
                    float a = (p >> 24) / 255.0f, bg = ((x / 16 + y / 16) & 1) ? 0.55f : 0.45f;
                    for (int c = 0; c < 3; c++)
                        img[((size_t)y * mw + x) * 3 + c] = (uint8_t)fminf(((p >> (16 - 8 * c)) & 255) + bg * 255.0f * (1.0f - a) + 0.5f, 255.0f);
                }
            char f[MAX_PATH];
            snprintf(f, sizeof f, "%s_menu.png", prefix);
            stbi_write_png(f, mw, mh, 3, img.data(), mw * 3);
            printf("menu: %s (%.1f ms a draw; the first, with the font atlas, %.1f ms)\n", f, snapMs, atlasMs);
        }
    }
    destroy(&ctx, nullptr);
    return 0;
}
