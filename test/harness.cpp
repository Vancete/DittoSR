// Offline test of the DLL: drives <dir>\<dll> through the FidelityFX API as a game does, upscaling a synthetic HDR scene
// rendered jittered at render size with reversed-Z depth and motion vectors. What upscales follows <dir>\dittosr.ini
// (enabled=0 with AMD's DLL next to it as <dll>.orig.dll: AMD's FSR, for comparison).
//   harness.exe <dir> <png prefix> [key=value ...]
//     dll=amd_fidelityfx_dx12.dll (or amd_fidelityfx_loader_dx12.dll)
//     scene=tiles     a detailed background panning, a nearer square moving the other way (disocclusion, ghosting)
//     scene=grass     thin grass blades over a cracked ground, the camera panning (one motion for every depth: a turn)
//     frames=64, ratio=1.5 (output / render; output 1440x810), vel=3 (grass: output pixels a frame), sharpen=0,
//                     dense=1 (grass: blades x dense, thinner by its square root), wobble=0 (grass: the speed vel - wobble, vel,
//                     vel + wobble in turn), blur=0 (grass: motion blur over
//                     blur x vel output pixels, in the render and the reference alike - the game's, before the upscale)
//     out=1440x810    the output size (the render: out / ratio); psnr=0: the GPU time alone (no reference, no PNGs)
// Reported: the upscale's GPU time (timestamps either side of its dispatch, the median); PSNR against the scene
// supersampled 4x4 at output size (in a perceptual domain: sqrt of Reinhard), at the
// middle and last frames, PNGs of both; with an integer pan (grass), the shimmer - the mean difference between frame t
// and frame t - 1 shifted by the pan over the last 16 frames, 0 for a perfect upscaler (x 1000, perceptual).
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

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 0x0000002u
struct ffxCreateBackendDX12Desc {
    ffxCreateContextDescHeader header;
    ID3D12Device *device;
};

static uint32_t OW = 1440, OH = 810;  // (out=)
static uint32_t RW = 960, RH = 540;
static bool g_grass;
static float g_vel = 3.0f, g_dense = 1.0f, g_blur = 0.0f;
static int g_wobble = 0;
// the pan's speed at frame t (output pixels): vel, or vel - wobble, vel, vel + wobble in turn - a hand on the mouse
static float pan_vel(int t) { return t <= 0 ? g_vel : g_vel + g_wobble * ((t * 7) % 3 - 1); }
static float pan_pos(int t) { return g_vel * t + (t % 3 == 2 ? g_wobble : 0); }  // (the sum of pan_vel(1 .. t))
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
            float hw = 0.5f * w * (1.0f - 0.7f * u);
            if (fabsf(sx - px) < hw) {
                float v = 0.35f + 0.9f * hf(cx, ry, 6) * (0.4f + u);
                s.rgb[0] = 1.1f * v, s.rgb[1] = 0.9f * v, s.rgb[2] = 0.45f * v;
                s.depth = 0.4f + 0.1f * hf(cx, ry, 7);
                return s;
            }
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
static Sample scene(float x, float y, int t) {
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

static const char *opt(int argc, char **argv, const char *key, const char *def) {
    size_t n = strlen(key);
    for (int i = 3; i < argc; i++)
        if (!strncmp(argv[i], key, n) && argv[i][n] == '=') return argv[i] + n + 1;
    return def;
}

int main(int argc, char **argv) {
    if (argc < 3) return printf("harness.exe <dir> <png prefix> [dll=] [scene=tiles|grass] [frames=] [out=WxH] [ratio=] [vel=] [dense=] [wobble=] [blur=] [sharpen=] [psnr=1]\n"), 1;
    const char *dir = argv[1], *prefix = argv[2];
    const char *dll = opt(argc, argv, "dll", "amd_fidelityfx_dx12.dll");
    int frames = atoi(opt(argc, argv, "frames", "64"));
    sscanf(opt(argc, argv, "out", "1440x810"), "%ux%u", &OW, &OH);
    bool measure = atoi(opt(argc, argv, "psnr", "1")) != 0;
    bool sharpen = atoi(opt(argc, argv, "sharpen", "0")) != 0;
    g_grass = !strcmp(opt(argc, argv, "scene", "tiles"), "grass");
    g_vel = (float)atof(opt(argc, argv, "vel", "3"));
    g_wobble = atoi(opt(argc, argv, "wobble", "0"));
    g_dense = (float)atof(opt(argc, argv, "dense", "1"));
    g_blur = (float)atof(opt(argc, argv, "blur", "0"));
    float ratio = (float)atof(opt(argc, argv, "ratio", "1.5"));
    RW = (uint32_t)(OW / ratio), RH = (uint32_t)(OH / ratio);
    float r = (float)OW / RW;
    int ivel = (int)g_vel;
    bool shimmer = g_grass && (float)ivel == g_vel;  // (an integer pan: frame t = frame t - 1 shifted, for a perfect upscaler)

    Gpu g = {};
    // the GPU with the most memory (not the integrated one)
    IDXGIFactory6 *fac;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)));
    IDXGIAdapter1 *best = nullptr, *a;
    SIZE_T bestMem = 0;
    for (UINT i = 0; fac->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory > bestMem) best = a, bestMem = d.DedicatedVideoMemory;
    }
    DXGI_ADAPTER_DESC1 ad;
    best->GetDesc1(&ad);
    printf("adapter: %ls; %s, %s scene, %ux%u -> %ux%u\n", ad.Description, dll, g_grass ? "grass" : "tiles", RW, RH, OW, OH);
    CHECK(D3D12CreateDevice(best, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.dev)));
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    CHECK(g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.q)));
    CHECK(g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.ca)));
    CHECK(g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.ca, nullptr, IID_PPV_ARGS(&g.cl)));
    CHECK(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)));
    g.ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g.cl->Close();

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
    ffxCreateBackendDX12Desc be = {{FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12, nullptr}, g.dev};
    ffxCreateContextDescUpscale cd = {};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, cd.header.pNext = &be.header;
    cd.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
    cd.maxRenderSize = {RW, RH}, cd.maxUpscaleSize = {OW, OH};
    if (create(&ctx, &cd.header, nullptr) != FFX_API_RETURN_OK) return printf("ffxCreateContext failed\n"), 1;
    ffxQueryDescUpscaleGetJitterPhaseCount pq = {{FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT}, RW, OW, &phases};
    query(&ctx, &pq.header);
    // (the upscaler's providers, as a game lists them before choosing one)
    uint64_t nv = 8, ids[8];
    const char *names[8];
    ffxQueryDescGetVersions gv = {{FFX_API_QUERY_DESC_TYPE_GET_VERSIONS}, FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, g.dev, &nv, ids, names};
    if (query(nullptr, &gv.header) == FFX_API_RETURN_OK) {
        printf("providers:");
        for (uint64_t i = 0; i < nv && i < 8; i++) printf(" %s (0x%llx)", names[i] ? names[i] : "?", (unsigned long long)ids[i]);
        printf("\n");
    }
    ffxQueryGetProviderVersion pv = {{FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION}};
    if (query(&ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName) printf("provider: %s\n", pv.versionName);

    ID3D12Resource *color = tex(g, RW, RH, DXGI_FORMAT_R16G16B16A16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *depth = tex(g, RW, RH, DXGI_FORMAT_R32_TYPELESS, false, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *mv = tex(g, RW, RH, DXGI_FORMAT_R16G16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *out = tex(g, OW, OH, DXGI_FORMAT_R16G16B16A16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource *up = buffer(g, 96ull << 20, D3D12_HEAP_TYPE_UPLOAD);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT ofp;
    D3D12_RESOURCE_DESC od = out->GetDesc();
    UINT64 obytes;
    g.dev->GetCopyableFootprints(&od, 0, 1, 0, &ofp, nullptr, nullptr, &obytes);
    ID3D12Resource *rb = buffer(g, obytes, D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_STATES SRV = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // the upscale's GPU time: timestamps either side of its dispatch
    ID3D12QueryHeap *qh;
    D3D12_QUERY_HEAP_DESC qhd = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2};
    CHECK(g.dev->CreateQueryHeap(&qhd, IID_PPV_ARGS(&qh)));
    ID3D12Resource *qrb = buffer(g, 16, D3D12_HEAP_TYPE_READBACK);
    UINT64 tsFreq = 1;
    g.q->GetTimestampFrequency(&tsFreq);
    std::vector<double> gpuMs;

    std::vector<uint16_t> cbuf(RW * RH * 4), mbuf(RW * RH * 2);
    std::vector<float> dbuf(RW * RH), cur(OW * OH * 3), prev(OW * OH * 3);
    double psnrSum = 0, shimSum = 0, upMs = 0;
    int psnrN = 0, shimN = 0;
    // (psnr=0, the GPU time alone: the first 16 frames rendered once and replayed - the CPU's render between upscales
    // left the GPU idle long enough to drop its clocks, and the time measured was its power saving)
    const int kCached = 16;
    std::vector<std::vector<uint16_t>> cc(measure ? 0 : kCached), mc(measure ? 0 : kCached);
    std::vector<std::vector<float>> dc(measure ? 0 : kCached);
    for (int t = 0; t < frames; t++) {
        int k = t % phases + 1;
        float jx = halton(k, 2) - 0.5f, jy = halton(k, 3) - 0.5f;
        bool replay = !measure && t >= kCached;
        if (replay) cbuf = cc[t % kCached], dbuf = dc[t % kCached], mbuf = mc[t % kCached];
        // the render: each pixel's one sample where the jitter put it (p + 0.5 - jitter), in output pixels
        for (uint32_t y = 0; y < RH && !replay; y++)
            for (uint32_t x = 0; x < RW; x++) {
                Sample s = scene((x + 0.5f - jx) * r, (y + 0.5f - jy) * r, t);
                size_t i = (size_t)y * RW + x;
                for (int c = 0; c < 3; c++) cbuf[i * 4 + c] = f2h(s.rgb[c]);
                cbuf[i * 4 + 3] = f2h(1.0f);
                dbuf[i] = s.depth;
                // (the previous position = this one + mv x scale, here in uv with scale = render size)
                mbuf[i * 2] = f2h(s.mv[0] / OW), mbuf[i * 2 + 1] = f2h(s.mv[1] / OH);
            }
        if (!measure && t < kCached) cc[t] = cbuf, dc[t] = dbuf, mc[t] = mbuf;
        g.ca->Reset();
        g.cl->Reset(g.ca, nullptr);
        if (t) {
            barrier(g.cl, color, SRV, D3D12_RESOURCE_STATE_COPY_DEST);
            barrier(g.cl, depth, SRV, D3D12_RESOURCE_STATE_COPY_DEST);
            barrier(g.cl, mv, SRV, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        UINT64 off = 0;
        upload(g, up, off, color, cbuf.data(), RW, RH, 8);
        upload(g, up, off, depth, dbuf.data(), RW, RH, 4);
        upload(g, up, off, mv, mbuf.data(), RW, RH, 4);
        barrier(g.cl, color, D3D12_RESOURCE_STATE_COPY_DEST, SRV);
        barrier(g.cl, depth, D3D12_RESOURCE_STATE_COPY_DEST, SRV);
        barrier(g.cl, mv, D3D12_RESOURCE_STATE_COPY_DEST, SRV);
        CHECK(g.cl->Close());
        ID3D12CommandList *lists[1] = {g.cl};
        g.q->ExecuteCommandLists(1, lists);
        g.wait();
        // the upscale alone in its own command list (its wall-clock time)
        g.ca->Reset();
        g.cl->Reset(g.ca, nullptr);
        {
            ffxDispatchDescUpscale dd = {};
            dd.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
            dd.commandList = g.cl;
            dd.color = res(color, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
            dd.depth = res(depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
            dd.motionVectors = res(mv, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
            dd.output = res(out, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
            dd.color.description.format = FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
            dd.depth.description.format = FFX_API_SURFACE_FORMAT_R32_FLOAT;
            dd.motionVectors.description.format = FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
            dd.output.description.format = FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
            dd.output.description.usage = FFX_API_RESOURCE_USAGE_UAV;
            dd.jitterOffset = {jx, jy};
            dd.motionVectorScale = {(float)RW, (float)RH};
            dd.renderSize = {RW, RH}, dd.upscaleSize = {OW, OH};
            dd.enableSharpening = sharpen, dd.sharpness = sharpen ? 0.5f : 0.0f;
            dd.frameTimeDelta = 16.6f, dd.preExposure = 1.0f, dd.reset = t == 0;
            dd.cameraNear = 0.1f, dd.cameraFar = 1e6f, dd.cameraFovAngleVertical = 1.0f, dd.viewSpaceToMetersFactor = 1.0f;
            g.cl->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
            ffxReturnCode_t rc = dispatch(&ctx, &dd.header);
            if (rc != FFX_API_RETURN_OK) return printf("ffxDispatch failed %u at frame %d\n", rc, t), 1;
            g.cl->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 1);
            g.cl->ResolveQueryData(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, qrb, 0);
        }
        bool psnrFrame = measure && (t == frames - 1 || t == frames / 2);
        bool shimFrame = shimmer && t >= frames - 17;
        bool grab = psnrFrame || shimFrame;
        if (grab) {
            barrier(g.cl, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION s = {out, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, d = {rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            d.PlacedFootprint = ofp;
            g.cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            barrier(g.cl, out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        CHECK(g.cl->Close());
        LARGE_INTEGER t0, t1, fq;
        QueryPerformanceCounter(&t0);
        g.q->ExecuteCommandLists(1, lists);
        g.wait();
        QueryPerformanceCounter(&t1), QueryPerformanceFrequency(&fq);
        if (t >= 8) {
            upMs += 1000.0 * (t1.QuadPart - t0.QuadPart) / fq.QuadPart;
            UINT64 *ts;
            D3D12_RANGE rr = {0, 16};
            qrb->Map(0, &rr, (void **)&ts);
            gpuMs.push_back(1000.0 * (double)(ts[1] - ts[0]) / (double)tsFreq);
            qrb->Unmap(0, nullptr);
        }
        if (g.dev->GetDeviceRemovedReason() != S_OK) return printf("device removed 0x%08x\n", (unsigned)g.dev->GetDeviceRemovedReason()), 1;
        if (!grab) continue;
        uint8_t *mp;
        rb->Map(0, nullptr, (void **)&mp);
        for (uint32_t y = 0; y < OH; y++) {
            const uint16_t *row = (const uint16_t *)(mp + (size_t)y * ofp.Footprint.RowPitch);
            for (uint32_t x = 0; x < OW; x++)
                for (int c = 0; c < 3; c++) cur[((size_t)y * OW + x) * 3 + c] = perceptual(h2f(row[x * 4 + c]));
        }
        rb->Unmap(0, nullptr);
        // the shimmer: this frame against the previous one shifted by the pan (away from the borders)
        if (shimFrame && t >= frames - 16) {
            double sum = 0;
            size_t n = 0;
            for (uint32_t y = 8; y < OH - 8; y++)
                for (uint32_t x = 8; x + ivel + g_wobble < OW - 8; x++)
                    for (int c = 0; c < 3; c++, n++) sum += fabs(cur[((size_t)y * OW + x) * 3 + c] - prev[((size_t)y * OW + x + (int)pan_vel(t)) * 3 + c]);
            shimSum += sum / n, shimN++;
        }
        if (shimFrame) std::swap(cur, prev);
        if (!psnrFrame) continue;
        const std::vector<float> &o = shimFrame ? prev : cur;
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
    printf("%.3f ms GPU an upscale (median; %.3f ms wall clock)\n", gmed, upMs / (frames - 8));
    destroy(&ctx, nullptr);
    return 0;
}
