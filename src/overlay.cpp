// DittoSR's in-game menu: F7 (menu_key) opens a side panel on the left of the game - the upscale's settings changed live,
// the frame time and DittoSR's own GPU time as a graph.
//   D3D12: drawn into the game's frame, as below. Vulkan: a Vulkan game's present cannot be hooked once its device exists
// (this DLL loads with the FidelityFX context, long after) - the same panel is drawn on the CPU into a layered window of
// its own, topmost over the game's client area while the game is in front (the window section at the end).
//   DXGI's Present is hooked in the swap chain's vtable (shared by every swap chain of its class: a dummy one on the
// game's device finds it), once per process, and ID3D12CommandQueue::ExecuteCommandLists likewise: a D3D12 swap chain
// does not tell its queue, so the menu draws on the direct queue the presenting thread last executed on - the game's
// frame, just before its Present. One instanced draw of quads (shaders/overlay.hlsl): rounded boxes and glyphs from a
// font atlas GDI draws (Segoe UI) at the screen's scale.
//   The keys are read with GetAsyncKeyState (the game's window in front); while the menu shows, the arrows and Enter
// are kept from the game (its window procedure subclassed: messages and raw input), and the mouse too where its cursor
// is over the panel.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "dittosr.h"
#include "ov_vs.h"
#include "ov_ps.h"

namespace {

// ---- the draw list ----

struct Quad {
    float rect[4], uv[4], c0[4], c1[4], p[4];
};
struct C4 {
    float r, g, b, a;
};
C4 rgba(uint32_t hex, float a = 1.0f) {
    return {((hex >> 16) & 255) / 255.0f, ((hex >> 8) & 255) / 255.0f, (hex & 255) / 255.0f, a};
}
C4 with_a(C4 c, float a) { return {c.r, c.g, c.b, c.a * a}; }

// the theme
const C4 kText = rgba(0xECEEF2), kText2 = rgba(0x9AA1AD), kText3 = rgba(0x666C78);
const C4 kAccent = rgba(0x2DD4BF), kAccentHot = rgba(0x5EEAD4), kAccentInk = rgba(0x062320), kAccent2 = rgba(0xA78BFA), kWarn = rgba(0xF59E0B);
const C4 kPanelTop = rgba(0x181A20, 0.97f), kPanelBottom = rgba(0x0E1013, 0.97f);
const C4 kCard = rgba(0xFFFFFF, 0.045f), kLine = rgba(0xFFFFFF, 0.07f), kSel = rgba(0xFFFFFF, 0.07f);

// ---- the font ----

enum Font { F_BODY, F_VALUE, F_SMALL, F_CAPS, F_TITLE, F_BIG, F_COUNT };
const wchar_t kExtra[] = {0x00D7, 0x00B7, 0x2190, 0x2191, 0x2192, 0x2193, 0x2039, 0x203A, 0x2014, 0x2022, 0x2026};
constexpr int kAscii = 95, kGlyphs = kAscii + (int)(sizeof kExtra / sizeof kExtra[0]);
struct Glyph {
    float u0, v0, u1, v1, w, h, adv;
};
struct FontData {
    Glyph g[kGlyphs];
    float height;
};

int glyph_index(uint32_t cp) {
    if (cp >= 32 && cp < 127) return (int)cp - 32;
    for (int i = 0; i < kGlyphs - kAscii; i++)
        if (kExtra[i] == cp) return kAscii + i;
    return '?' - 32;
}

uint32_t next_cp(const char *&s) {  // (UTF-8)
    uint8_t c = (uint8_t)*s++;
    if (c < 0x80) return c;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    uint32_t cp = c & (0x3F >> n);
    for (int i = 0; i < n && *s; i++) cp = (cp << 6) | ((uint8_t)*s++ & 0x3F);
    return cp;
}

// The atlas: every font's glyphs drawn by GDI (grayscale antialiasing) into one R8 image
bool build_atlas(float scale, FontData *fonts, std::vector<uint8_t> &pix, int &aw, int &ah) {
    struct Spec { float px; int weight; const wchar_t *face; } spec[F_COUNT] = {
        {15.0f, FW_NORMAL, L"Segoe UI"}, {15.0f, FW_SEMIBOLD, L"Segoe UI"}, {12.5f, FW_NORMAL, L"Segoe UI"},
        {11.5f, FW_BOLD, L"Segoe UI"},   {23.0f, FW_BOLD, L"Segoe UI"},     {25.0f, FW_SEMIBOLD, L"Segoe UI"}};
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    HFONT hf[F_COUNT];
    for (int f = 0; f < F_COUNT; f++)
        hf[f] = CreateFontW(-(int)lroundf(spec[f].px * scale), 0, 0, 0, spec[f].weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, spec[f].face);
    // the layout first: rows of cells, 2 px of room on each side
    aw = 1024;
    struct Cell { int x, y, w, h; } cells[F_COUNT][kGlyphs];
    int x = 0, y = 0, rowH = 0;
    for (int f = 0; f < F_COUNT; f++) {
        SelectObject(dc, hf[f]);
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        fonts[f].height = (float)tm.tmHeight;
        for (int i = 0; i < kGlyphs; i++) {
            wchar_t wc = i < kAscii ? (wchar_t)(32 + i) : kExtra[i - kAscii];
            SIZE sz;
            GetTextExtentPoint32W(dc, &wc, 1, &sz);
            int w = sz.cx + 4, h = tm.tmHeight + 2;
            if (x + w > aw) x = 0, y += rowH, rowH = 0;
            cells[f][i] = {x, y, w, h};
            fonts[f].g[i].adv = (float)sz.cx, fonts[f].g[i].w = (float)w, fonts[f].g[i].h = (float)h;
            x += w, rowH = h > rowH ? h : rowH;
        }
    }
    ah = 64;
    while (ah < y + rowH) ah *= 2;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader, bi.bmiHeader.biWidth = aw, bi.bmiHeader.biHeight = -ah;
    bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) {
        for (HFONT h : hf) DeleteObject(h);
        DeleteDC(dc);
        return false;
    }
    HGDIOBJ old = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)aw * ah * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    for (int f = 0; f < F_COUNT; f++) {
        SelectObject(dc, hf[f]);
        for (int i = 0; i < kGlyphs; i++) {
            wchar_t wc = i < kAscii ? (wchar_t)(32 + i) : kExtra[i - kAscii];
            Cell c = cells[f][i];
            TextOutW(dc, c.x + 2, c.y + 1, &wc, 1);
            Glyph &g = fonts[f].g[i];
            g.u0 = (float)c.x / aw, g.v0 = (float)c.y / ah, g.u1 = (float)(c.x + c.w) / aw, g.v1 = (float)(c.y + c.h) / ah;
        }
    }
    GdiFlush();
    pix.resize((size_t)aw * ah);
    const uint8_t *b = (const uint8_t *)bits;
    for (size_t i = 0; i < pix.size(); i++) pix[i] = b[i * 4 + 1];
    SelectObject(dc, old);
    DeleteObject(bmp);
    for (HFONT h : hf) DeleteObject(h);
    DeleteDC(dc);
    return true;
}

// the atlas the menu measures and draws with (D3D12: copied into a texture; the window: sampled on the CPU)
struct FontAtlas {
    FontData f[F_COUNT];
    std::vector<uint8_t> pix;
    int w = 0, h = 0;
    float scale = 0.0f;
};
FontAtlas g_font;

// ---- the renderer ----

constexpr int kSlots = 3, kMaxQuads = 8192;

struct Gfx {
    ID3D12Device *dev = nullptr;
    ID3D12RootSignature *rs = nullptr;
    struct { DXGI_FORMAT f; ID3D12PipelineState *p; } pso[6] = {};
    ID3D12DescriptorHeap *rtv = nullptr, *srv = nullptr;
    UINT rtvInc = 0;
    ID3D12Resource *font = nullptr, *fontUp = nullptr;
    bool fontPending = false;
    UINT64 fontUpFence = 0;
    float fontScale = 0.0f;
    struct Slot { ID3D12CommandAllocator *alloc = nullptr; ID3D12Resource *buf = nullptr; Quad *map = nullptr; UINT64 fence = 0; } slot[kSlots];
    ID3D12GraphicsCommandList *cl = nullptr;
    ID3D12Fence *fence = nullptr;
    UINT64 fenceVal = 0;
    HANDLE ev = nullptr;
    int cur = 0;
    bool ok = false;
};
Gfx g_gfx;
std::mutex g_mu;

void wait_fence(UINT64 v) {
    if (!g_gfx.fence || g_gfx.fence->GetCompletedValue() >= v) return;
    g_gfx.fence->SetEventOnCompletion(v, g_gfx.ev);
    WaitForSingleObject(g_gfx.ev, 2000);
}

template <class T> void rel(T *&p) {
    if (p) p->Release(), p = nullptr;
}

void gfx_release() {
    wait_fence(g_gfx.fenceVal);
    for (auto &s : g_gfx.slot) rel(s.alloc), rel(s.buf), s.map = nullptr;
    for (auto &p : g_gfx.pso) rel(p.p);
    rel(g_gfx.cl), rel(g_gfx.fence), rel(g_gfx.rs), rel(g_gfx.rtv), rel(g_gfx.srv), rel(g_gfx.font), rel(g_gfx.fontUp), rel(g_gfx.dev);
    if (g_gfx.ev) CloseHandle(g_gfx.ev), g_gfx.ev = nullptr;
    g_gfx.ok = false, g_gfx.fontScale = 0.0f, g_gfx.fenceVal = 0;
}

ID3D12Resource *buffer(ID3D12Device *d, UINT64 size, D3D12_HEAP_TYPE heap) {
    D3D12_HEAP_PROPERTIES hp = {heap};
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, bd.Width = size, bd.Height = 1, bd.DepthOrArraySize = 1, bd.MipLevels = 1;
    bd.SampleDesc.Count = 1, bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *r = nullptr;
    if (FAILED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}

bool gfx_init(ID3D12Device *d) {
    g_gfx.dev = d, d->AddRef();
    D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0};
    D3D12_ROOT_PARAMETER rp[3] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, rp[0].Constants.Num32BitValues = 4;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV, rp[1].Descriptor.ShaderRegister = 0;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[2].DescriptorTable.NumDescriptorRanges = 1, rp[2].DescriptorTable.pDescriptorRanges = &range;
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rd = {3, rp, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob *blob = nullptr, *err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
        if (err) err->Release();
        return false;
    }
    HRESULT hr = d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_gfx.rs));
    blob->Release();
    if (FAILED(hr)) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kSlots, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_gfx.rtv)))) return false;
    g_gfx.rtvInc = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_gfx.srv)))) return false;
    for (auto &s : g_gfx.slot) {
        if (FAILED(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.alloc)))) return false;
        s.buf = buffer(d, sizeof(Quad) * kMaxQuads, D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RANGE none = {0, 0};
        if (!s.buf || FAILED(s.buf->Map(0, &none, (void **)&s.map))) return false;
    }
    if (FAILED(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_gfx.slot[0].alloc, nullptr, IID_PPV_ARGS(&g_gfx.cl)))) return false;
    g_gfx.cl->Close();
    if (FAILED(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_gfx.fence)))) return false;
    g_gfx.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return g_gfx.ev != nullptr;
}

ID3D12PipelineState *pso_for(DXGI_FORMAT f) {
    for (auto &p : g_gfx.pso)
        if (p.p && p.f == f) return p.p;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g_gfx.rs;
    pd.VS = {g_ov_vs, sizeof g_ov_vs}, pd.PS = {g_ov_ps, sizeof g_ov_ps};
    D3D12_RENDER_TARGET_BLEND_DESC &b = pd.BlendState.RenderTarget[0];
    b.BlendEnable = TRUE, b.SrcBlend = D3D12_BLEND_SRC_ALPHA, b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA, b.BlendOp = D3D12_BLEND_OP_ADD;
    b.SrcBlendAlpha = D3D12_BLEND_ZERO, b.DestBlendAlpha = D3D12_BLEND_ONE, b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID, pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1, pd.RTVFormats[0] = f, pd.SampleDesc.Count = 1;
    ID3D12PipelineState *p = nullptr;
    if (FAILED(g_gfx.dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&p)))) return nullptr;
    for (auto &e : g_gfx.pso)
        if (!e.p) {
            e.f = f, e.p = p;
            return p;
        }
    p->Release();
    return nullptr;
}

// the atlas at this scale: drawn by GDI, its texture made (copied on the next command list)
bool ensure_font(float scale) {
    if (g_gfx.font && fabsf(g_gfx.fontScale - scale) < 0.01f) return true;
    wait_fence(g_gfx.fenceVal);
    rel(g_gfx.font), rel(g_gfx.fontUp);
    if (fabsf(g_font.scale - scale) >= 0.01f || g_font.pix.empty()) {
        if (!build_atlas(scale, g_font.f, g_font.pix, g_font.w, g_font.h)) return false;
        g_font.scale = scale;
    }
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, td.Width = g_font.w, td.Height = g_font.h, td.DepthOrArraySize = 1;
    td.MipLevels = 1, td.Format = DXGI_FORMAT_R8_UNORM, td.SampleDesc.Count = 1;
    if (FAILED(g_gfx.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_gfx.font))))
        return false;
    g_gfx.fontUp = buffer(g_gfx.dev, (UINT64)g_font.w * g_font.h, D3D12_HEAP_TYPE_UPLOAD);  // (1024 wide: rows 256-aligned)
    void *m = nullptr;
    if (!g_gfx.fontUp || FAILED(g_gfx.fontUp->Map(0, nullptr, &m))) return false;
    memcpy(m, g_font.pix.data(), g_font.pix.size());
    g_gfx.fontUp->Unmap(0, nullptr);
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R8_UNORM, sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, sd.Texture2D.MipLevels = 1;
    g_gfx.dev->CreateShaderResourceView(g_gfx.font, &sd, g_gfx.srv->GetCPUDescriptorHandleForHeapStart());
    g_gfx.fontPending = true, g_gfx.fontScale = scale;
    return true;
}

// ---- the UI: an immediate-mode draw list ----

struct Ui {
    std::vector<Quad> q;
    float alpha = 1.0f;  // (the panel's fade)
    float dx = 0.0f, dy = 0.0f;  // (the slide, the scroll)
    float s = 1.0f;  // (pixels per unit: the screen's height / 1080)

    void box(float x0, float y0, float x1, float y1, C4 c0, C4 c1, float radius, int mode = 0, float width = 0.0f) {
        if (q.size() >= kMaxQuads) return;
        Quad o = {{x0 + dx, y0 + dy, x1 + dx, y1 + dy}, {0, 0, 0, 0}, {c0.r, c0.g, c0.b, c0.a * alpha}, {c1.r, c1.g, c1.b, c1.a * alpha},
                  {radius, (float)mode, width, 0}};
        q.push_back(o);
    }
    void box(float x0, float y0, float x1, float y1, C4 c, float radius) { box(x0, y0, x1, y1, c, c, radius); }
    float measure(int f, const char *t, float track = 0.0f) const {
        float w = 0.0f;
        while (*t) w += g_font.f[f].g[glyph_index(next_cp(t))].adv + track;
        return w;
    }
    // text with its top at y; returns the pen's end
    float text(int f, float x, float y, const char *t, C4 c, float track = 0.0f) {
        const FontData &fd = g_font.f[f];
        x = floorf(x + dx + 0.5f) - dx, y = floorf(y + dy + 0.5f) - dy;
        while (*t) {
            const Glyph &g = fd.g[glyph_index(next_cp(t))];
            if (q.size() < kMaxQuads && g.adv > 0.0f) {
                Quad o = {{x - 2 + dx, y - 1 + dy, x - 2 + g.w + dx, y - 1 + g.h + dy}, {g.u0, g.v0, g.u1, g.v1}, {c.r, c.g, c.b, c.a * alpha},
                          {c.r, c.g, c.b, c.a * alpha}, {0, 1, 0, 0}};
                q.push_back(o);
            }
            x += g.adv + track;
        }
        return x;
    }
    float h(int f) const { return g_font.f[f].height; }
};

// ---- the menu's state ----

enum Item { I_UPSCALER, I_COMPARE, I_SHARP, I_WEIGHT, I_SHIMMER, I_HSHARP, I_DEBUG, I_INDICATOR, I_BUTTONS, I_COUNT };

struct Key {
    int vk;
    bool down = false;
    double next = 0.0;
};

// the frame times (ms) and DittoSR's GPU time, a ring; their averages over half a second
struct Perf {
    static constexpr int kRing = 256;
    float frameMs[kRing] = {}, upMs[kRing] = {};
    int head = 0, count = 0;
    double fpsAcc = 0.0;
    int fpsN = 0;
    float fps = 0.0f, avgFrame = 0.0f, avgUp = 0.0f;
    double fpsT = 0.0;
};

struct Menu : Perf {  // (its Perf: a copy of g_perf, taken before each draw)
    bool open = false;
    float anim = 0.0f;  // (0 hidden - 1 shown)
    int sel = I_SHARP, btn = 0;
    float scroll = 0.0f;
    double toastUntil = 0.0;
    char toast[64] = "";
    bool toastOk = true;
    Key kMenu{0}, kUp{VK_UP}, kDown{VK_DOWN}, kLeft{VK_LEFT}, kRight{VK_RIGHT}, kEnter{VK_RETURN}, kBack{VK_BACK};
    bool lmb = false;
    int drag = -1;  // (the slider the mouse holds)
    float rowY[I_COUNT] = {}, rowH[I_COUNT] = {};
    float btnX[3][2] = {};
    float trackX0 = 0, trackX1 = 0;
    float panel[4] = {};  // (on screen)
    HWND hwnd = nullptr;
    IDXGISwapChain *main = nullptr;  // (the swap chain drawn on: the largest seen - compared, never used)
    UINT mainArea = 0, bbW = 0, bbH = 0;
    double mainT = 0.0;  // (its last Present)
    int colorSpaceHdr10 = -1;  // (SetColorSpace1's: -1 unknown)
};
Menu g_m;
double g_qpcMs = 0.0;
volatile bool g_menuShown = false;  // (the window procedure's: keep the arrows from the game)
Perf g_perf;
std::mutex g_perfMu;  // (g_perf's alone: the frame records it without waiting on a draw)

double now_ms() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * g_qpcMs;
}

// One frame: its time since the last, DittoSR's GPU time
void record_frame() {
    std::lock_guard<std::mutex> lk(g_perfMu);
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    static int64_t last;
    if (last) {
        Perf &p = g_perf;
        double t = q.QuadPart * g_qpcMs;
        float ms = (float)((q.QuadPart - last) * g_qpcMs);
        double age = (q.QuadPart - g_stats.lastQpc) * g_qpcMs;
        p.frameMs[p.head] = ms, p.upMs[p.head] = age < 500.0 && g_stats.active ? g_stats.upscaleMs : 0.0f;
        p.head = (p.head + 1) % Perf::kRing, p.count = p.count < Perf::kRing ? p.count + 1 : p.count;
        p.fpsAcc += ms, p.fpsN++;
        if (t - p.fpsT > 500.0 && p.fpsN) {
            p.avgFrame = (float)(p.fpsAcc / p.fpsN), p.fps = 1000.0f / p.avgFrame;
            float su = 0.0f;
            int nu = 0;
            for (int i = 0; i < p.fpsN && i < Perf::kRing; i++) {
                float v = p.upMs[(p.head - 1 - i + Perf::kRing) % Perf::kRing];
                if (v > 0.0f) su += v, nu++;
            }
            p.avgUp = nu ? su / nu : 0.0f;
            p.fpsAcc = 0.0, p.fpsN = 0, p.fpsT = t;
        }
    }
    last = q.QuadPart;
}

void take_perf() {
    std::lock_guard<std::mutex> lk(g_perfMu);
    static_cast<Perf &>(g_m) = g_perf;
}

bool key_pressed(Key &k, double t, bool repeat) {
    bool d = (GetAsyncKeyState(k.vk) & 0x8000) != 0;
    bool fire = false;
    if (d && !k.down) fire = true, k.next = t + 380.0;
    else if (d && repeat && t >= k.next) fire = true, k.next = t + 55.0;
    k.down = d;
    return fire;
}

bool visible(int i) { return (i != I_UPSCALER && i != I_COMPARE) || g_stats.hasReal; }

// the sliders' and choices' ranges
struct Range { float lo, hi, step; };
Range range_of(int i) {
    switch (i) {
    case I_SHARP: return {0.0f, 1.0f, 0.05f};
    case I_WEIGHT: return {0.25f, 4.0f, 0.25f};
    case I_HSHARP: return {0.0f, 2.0f, 0.25f};
    default: return {0, 1, 1};
    }
}
float *slider_of(DittoSettings &s, int i) {
    switch (i) {
    case I_SHARP: return &s.sharpness;
    case I_WEIGHT: return &s.frameWeight;
    case I_HSHARP: return &s.historySharpen;
    default: return nullptr;
    }
}
const float kShimmer[] = {-1.0f, 0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
const char *kDebugNames[] = {"Off", "Motion", "New frame's share", "Stability", "Clip / cut / confidence"};

int shimmer_index(float v) {
    int best = 0;
    for (int i = 0; i < 8; i++)
        if (fabsf(kShimmer[i] - v) < fabsf(kShimmer[best] - v)) best = i;
    return best;
}

// one step of item i (dir -1, +1; 0: Enter); true: the settings changed
bool adjust(DittoSettings &s, int i, int dir) {
    DittoSettings d;
    switch (i) {
    case I_UPSCALER: InterlockedExchange(&g_reqToggle, 1); return false;
    case I_COMPARE: InterlockedExchange(&g_reqCompare, 1); return false;
    case I_SHARP: case I_WEIGHT: case I_HSHARP: {
        float *v = slider_of(s, i);
        Range r = range_of(i);
        if (dir == 0) *v = *slider_of(d, i);  // (Enter: back to the default)
        else *v = fminf(fmaxf(roundf((*v + dir * r.step) / r.step) * r.step, r.lo), r.hi);
        return true;
    }
    case I_SHIMMER: s.antiShimmer = kShimmer[(shimmer_index(s.antiShimmer) + (dir < 0 ? 7 : 1)) % 8]; return true;
    case I_DEBUG: {  // (Off, 2, 3, 4: the motion view, 1, from the ini only)
        const int order[4] = {0, 2, 3, 4};
        int k = 0;
        for (int j = 0; j < 4; j++)
            if (order[j] == s.debugView) k = j;
        s.debugView = order[(k + (dir < 0 ? 3 : 1)) % 4];
        return true;
    }
    case I_INDICATOR: s.indicator = !s.indicator; return true;
    }
    return false;
}

void show_toast(const char *t, bool ok) {
    snprintf(g_m.toast, sizeof g_m.toast, "%s", t);
    g_m.toastOk = ok, g_m.toastUntil = now_ms() + 2500.0;
}

void press_button(DittoSettings &s, int b, bool &changed) {
    if (b == 0) {
        bool ok = ditto_save_settings();
        show_toast(ok ? "Saved to dittosr.ini" : "Could not write dittosr.ini", ok);
    } else if (b == 1) {
        s = DittoSettings(), changed = true;
        show_toast("Defaults restored (not saved yet)", true);
    } else {
        ditto_request_reload();
        show_toast("dittosr.ini read again", true);
    }
}

float ease(float t) { return 1.0f - powf(1.0f - t, 3.0f); }

void subclass(HWND h);

const char *api_name() { return g_stats.api == 2 ? "XeSS" : "FSR"; }

// ---- the panel ----

void draw_toggle(Ui &u, float x1, float yc, bool on) {
    float w = 34 * u.s, h = 18 * u.s, x0 = x1 - w;
    u.box(x0, yc - h / 2, x1, yc + h / 2, on ? kAccent : rgba(0xFFFFFF, 0.14f), h / 2);
    float k = 14 * u.s, kx = on ? x1 - 2 * u.s - k : x0 + 2 * u.s;
    u.box(kx, yc - k / 2, kx + k, yc + k / 2, rgba(on ? 0x0B1F1C : 0xE6E8EC), k / 2);
}

void draw_choice(Ui &u, float x1, float y, const char *v, bool sel) {
    float w = u.measure(F_VALUE, v);
    float ax = x1 - 10 * u.s;
    u.text(F_VALUE, ax, y, "\xE2\x80\xBA", sel ? kAccent : kText3);
    float vx = ax - 8 * u.s - w;
    u.text(F_VALUE, vx, y, v, kText);
    u.text(F_VALUE, vx - 16 * u.s, y, "\xE2\x80\xB9", sel ? kAccent : kText3);
}

// The panel into u; returns the content's height (for the scroll)
float build(Ui &u, const DittoSettings &s, float W, float H) {
    float S = u.s;
    float pw = 372 * S, m = 16 * S, pad = 20 * S;
    float px0 = m, py0 = m, px1 = m + pw, py1 = H - m;  // (on the left)
    g_m.panel[0] = px0 + u.dx, g_m.panel[1] = py0, g_m.panel[2] = px1 + u.dx, g_m.panel[3] = py1;
    // the panel: a soft shadow, the card, its edge
    float sdy = u.dy;
    u.dy = 0.0f;
    for (int i = 3; i >= 1; i--) u.box(px0 - i * 4 * S, py0 - i * 2 * S, px1 + i * 4 * S, py1 + i * 6 * S, rgba(0, 0.10f), 20 * S + i * 4 * S);
    u.box(px0, py0, px1, py1, kPanelTop, kPanelBottom, 18 * S);
    u.box(px0, py0, px1, py1, rgba(0xFFFFFF, 0.08f), rgba(0xFFFFFF, 0.03f), 18 * S, 2, 1.0f);
    u.dy = sdy;
    float x0 = px0 + pad, x1 = px1 - pad, y = py0 + pad;

    // the header: the name, the version, the state
    float tx = u.text(F_TITLE, x0, y, "DittoSR", kText);
    const char *ver = DITTOSR_VERSION;
    float vw = u.measure(F_SMALL, ver) + 12 * S, vy = y + (u.h(F_TITLE) - u.h(F_SMALL)) / 2 + 1 * S;
    u.box(tx + 10 * S, vy - 1 * S, tx + 10 * S + vw, vy + u.h(F_SMALL) + 1 * S, rgba(0xFFFFFF, 0.08f), 6 * S);
    u.text(F_SMALL, tx + 16 * S, vy, ver, kText2);
    LARGE_INTEGER qn;
    QueryPerformanceCounter(&qn);
    double age = (double)(qn.QuadPart - g_stats.lastQpc) * g_qpcMs;
    bool live = g_stats.lastQpc && age < 1000.0;
    const char *st = !live ? "IDLE" : g_stats.active ? "ACTIVE" : g_stats.api == 2 ? "XESS" : "FSR";
    C4 sc = !live ? kText3 : g_stats.active ? kAccent : kWarn;
    float sw = u.measure(F_CAPS, st, 1.0f * S) + 22 * S;
    float sy = y + (u.h(F_TITLE) - u.h(F_CAPS)) / 2;
    u.box(x1 - sw, sy - 3 * S, x1, sy + u.h(F_CAPS) + 3 * S, with_a(sc, 0.14f), 8 * S);
    u.box(x1 - sw + 8 * S, sy + u.h(F_CAPS) / 2 - 3 * S, x1 - sw + 14 * S, sy + u.h(F_CAPS) / 2 + 3 * S, sc, 3 * S);
    u.text(F_CAPS, x1 - sw + 17 * S, sy, st, sc, 1.0f * S);
    y += u.h(F_TITLE) + 4 * S;
    char line[128];
    if (live)
        snprintf(line, sizeof line, "%s input \xC2\xB7 %u\xC3\x97%u \xE2\x86\x92 %u\xC3\x97%u \xC2\xB7 %.2f\xC3\x97", api_name(), g_stats.rw, g_stats.rh,
                 g_stats.ow, g_stats.oh, g_stats.rw ? (float)g_stats.ow / g_stats.rw : 0.0f);
    else
        snprintf(line, sizeof line, "Waiting for the game's upscaler\xE2\x80\xA6");
    u.text(F_SMALL, x0, y, line, kText2);
    y += u.h(F_SMALL) + 16 * S;

    // the performance card: three numbers, the graph
    auto section = [&](const char *t) {
        u.text(F_CAPS, x0, y, t, kText3, 1.2f * S);
        y += u.h(F_CAPS) + 8 * S;
    };
    section("PERFORMANCE");
    float cy0 = y, ch = 64 * S + 96 * S;
    u.box(x0, cy0, x1, cy0 + ch, kCard, 12 * S);
    float cw = (x1 - x0) / 3;
    char num[32];
    struct { const char *label; float v; const char *fmt; C4 c; } nums[3] = {
        {"FPS", g_m.fps, "%.0f", kText}, {"FRAME MS", g_m.avgFrame, "%.2f", kAccent}, {"DITTOSR MS", g_m.avgUp, "%.2f", kAccent2}};
    for (int i = 0; i < 3; i++) {
        float cx = x0 + cw * i + 14 * S;
        if (i == 2 && g_m.avgUp <= 0.0f) snprintf(num, sizeof num, "\xE2\x80\x94");
        else snprintf(num, sizeof num, nums[i].fmt, nums[i].v);
        u.text(F_BIG, cx, cy0 + 10 * S, num, nums[i].c);
        u.text(F_CAPS, cx, cy0 + 12 * S + u.h(F_BIG), nums[i].label, kText3, 0.8f * S);
        if (i) u.box(x0 + cw * i, cy0 + 14 * S, x0 + cw * i + 1, cy0 + 54 * S, kLine, 0);
    }
    // the graph: the frame time (an area) and DittoSR's GPU time in front, on one scale
    float gx0 = x0 + 12 * S, gx1 = x1 - 12 * S, gy0 = cy0 + 70 * S, gy1 = cy0 + ch - 12 * S;
    int n = (int)((gx1 - gx0) / (3 * S));
    if (n > Menu::kRing) n = Menu::kRing;
    if (n > g_m.count) n = g_m.count;
    float mx = 1.0f;
    for (int i = 0; i < n; i++) mx = fmaxf(mx, g_m.frameMs[(g_m.head - 1 - i + Menu::kRing) % Menu::kRing]);
    const float nice[] = {4.0f, 8.33f, 16.7f, 25.0f, 33.3f, 50.0f, 66.7f, 100.0f, 200.0f, 500.0f};
    float scale = 1000.0f;
    for (float v : nice)
        if (mx <= v) {
            scale = v;
            break;
        }
    u.box(gx0, gy1, gx1, gy1 + 1, kLine, 0);
    u.box(gx0, (gy0 + gy1) / 2, gx1, (gy0 + gy1) / 2 + 1, rgba(0xFFFFFF, 0.04f), 0);
    snprintf(num, sizeof num, "%.1f ms", scale);
    u.text(F_SMALL, gx1 - u.measure(F_SMALL, num), gy0 - 2 * S, num, kText3);
    float bw = 3 * S;
    for (int i = 0; i < n; i++) {
        int k = (g_m.head - 1 - i + Menu::kRing) % Menu::kRing;
        float bx1 = gx1 - i * bw, bx0 = bx1 - bw + 0.6f * S;
        float fh = fminf(g_m.frameMs[k] / scale, 1.0f) * (gy1 - gy0);
        float uh = fminf(g_m.upMs[k] / scale, 1.0f) * (gy1 - gy0);
        if (fh > 0.5f) {
            u.box(bx0, gy1 - fh, bx1, gy1, with_a(kAccent, 0.45f), with_a(kAccent, 0.08f), 0);
            u.box(bx0, gy1 - fh, bx1, gy1 - fh + 1.5f * S, kAccent, 0);
        }
        if (uh > 0.5f) u.box(bx0, gy1 - uh, bx1, gy1, with_a(kAccent2, 0.95f), with_a(kAccent2, 0.7f), 0);
    }
    if (live && g_m.avgFrame > 0.0f && g_m.avgUp > 0.0f) {
        snprintf(num, sizeof num, "DittoSR %.0f%% of the frame", 100.0f * g_m.avgUp / g_m.avgFrame);
        u.text(F_SMALL, gx0, gy0 - 2 * S, num, kText3);
    }
    y = cy0 + ch + 18 * S;

    // the rows
    float rx0 = x0 - 8 * S, rx1 = x1 + 8 * S;
    g_m.trackX0 = x0, g_m.trackX1 = x1;
    auto row_bg = [&](int i, float rh) {
        g_m.rowY[i] = y, g_m.rowH[i] = rh;
        if (g_m.sel == i) {
            u.box(rx0, y, rx1, y + rh, kSel, 10 * S);
            u.box(rx0, y, rx1, y + rh, with_a(kAccent, 0.9f), with_a(kAccent, 0.9f), 10 * S, 2, 1.5f * S);
        }
    };
    float lh = u.h(F_BODY);
    auto toggle_row = [&](int i, const char *label, bool on) {
        float rh = 38 * S;
        row_bg(i, rh);
        u.text(F_BODY, x0, y + (rh - lh) / 2, label, kText);
        draw_toggle(u, x1, y + rh / 2, on);
        y += rh + 2 * S;
    };
    auto choice_row = [&](int i, const char *label, const char *v) {
        float rh = 38 * S;
        row_bg(i, rh);
        u.text(F_BODY, x0, y + (rh - lh) / 2, label, kText);
        draw_choice(u, x1, y + (rh - u.h(F_VALUE)) / 2, v, g_m.sel == i);
        y += rh + 2 * S;
    };
    auto slider_row = [&](int i, const char *label, const char *hint) {
        float rh = 54 * S;
        row_bg(i, rh);
        DittoSettings d;
        float v = *slider_of(const_cast<DittoSettings &>(s), i), dv = *slider_of(d, i);
        Range r = range_of(i);
        float ty = y + 8 * S;
        u.text(F_BODY, x0, ty, label, kText);
        char b[32];
        snprintf(b, sizeof b, "%.2f", v);
        u.text(F_VALUE, x1 - u.measure(F_VALUE, b), ty, b, g_m.sel == i ? kAccent : kText);
        if (hint) u.text(F_SMALL, x0 + u.measure(F_BODY, label) + 8 * S, ty + (lh - u.h(F_SMALL)) / 2 + 1 * S, hint, kText3);
        float ky = y + rh - 14 * S, th = 4 * S;
        float t = (v - r.lo) / (r.hi - r.lo), td = (dv - r.lo) / (r.hi - r.lo);
        float kx = x0 + t * (x1 - x0);
        u.box(x0, ky - th / 2, x1, ky + th / 2, rgba(0xFFFFFF, 0.12f), th / 2);
        u.box(x0, ky - th / 2, kx, ky + th / 2, with_a(kAccent, 0.85f), kAccent, th / 2);
        float dx = x0 + td * (x1 - x0);
        u.box(dx - 1 * S, ky - 6 * S, dx + 1 * S, ky + 6 * S, rgba(0xFFFFFF, 0.25f), 1 * S);
        float k = (g_m.sel == i ? 14 : 12) * S;
        u.box(kx - k / 2, ky - k / 2, kx + k / 2, ky + k / 2, rgba(0xFFFFFF), k / 2);
        y += rh + 2 * S;
    };

    if (g_stats.hasReal) {
        section("UPSCALER");
        char b[48];
        snprintf(b, sizeof b, "%s", g_stats.active ? "DittoSR" : g_stats.api == 2 ? "Game's XeSS" : "Game's FSR");
        choice_row(I_UPSCALER, "Upscaler", b);
        toggle_row(I_COMPARE, "Side-by-side comparison", g_stats.compare);
        y += 10 * S;
    }
    section("IMAGE");
    slider_row(I_SHARP, "Sharpness", nullptr);
    slider_row(I_WEIGHT, "Frame weight", "steadier \xE2\x86\x90 \xE2\x86\x92 crisper");
    {
        char b[16];
        if (s.antiShimmer < 0.0f) snprintf(b, sizeof b, "Auto");
        else if (s.antiShimmer == 0.0f) snprintf(b, sizeof b, "Off");
        else snprintf(b, sizeof b, "%.1f", s.antiShimmer);
        choice_row(I_SHIMMER, "Anti-shimmer", b);
    }
    slider_row(I_HSHARP, "Motion detail", nullptr);
    y += 10 * S;
    section("ADVANCED");
    choice_row(I_DEBUG, "Debug view", kDebugNames[s.debugView < 0 || s.debugView > 4 ? 0 : s.debugView]);
    toggle_row(I_INDICATOR, "Indicator", s.indicator);
    y += 12 * S;

    // the buttons
    {
        float bh = 36 * S, gap = 8 * S, bw3 = (x1 - x0 - 2 * gap) / 3;
        g_m.rowY[I_BUTTONS] = y, g_m.rowH[I_BUTTONS] = bh;
        const char *names[3] = {"Save", "Defaults", "Revert"};
        for (int b = 0; b < 3; b++) {
            float bx0 = x0 + b * (bw3 + gap), bx1 = bx0 + bw3;
            g_m.btnX[b][0] = bx0, g_m.btnX[b][1] = bx1;
            bool hot = g_m.sel == I_BUTTONS && g_m.btn == b;
            if (b == 0) u.box(bx0, y, bx1, y + bh, hot ? kAccentHot : kAccent, 10 * S);
            else u.box(bx0, y, bx1, y + bh, rgba(0xFFFFFF, hot ? 0.16f : 0.07f), 10 * S);
            if (hot && b) u.box(bx0, y, bx1, y + bh, with_a(kAccent, 0.9f), with_a(kAccent, 0.9f), 10 * S, 2, 1.5f * S);
            float w = u.measure(F_VALUE, names[b]);
            u.text(F_VALUE, (bx0 + bx1 - w) / 2, y + (bh - u.h(F_VALUE)) / 2, names[b], b == 0 ? kAccentInk : kText);
        }
        y += bh + 12 * S;
    }
    float content = y - (py0 + pad);
    // the footer: the keys, or the last action's note (it stays at the panel's bottom)
    u.dy = 0.0f;
    float fy = py1 - pad - u.h(F_SMALL);
    u.box(px0 + 1, fy - 14 * S, px1 - 1, py1 - 1, rgba(0x0E1013, 0.0f), rgba(0x0E1013, 0.96f), 17 * S);
    u.box(x0, fy - 10 * S, x1, fy - 10 * S + 1, kLine, 0);
    if (now_ms() < g_m.toastUntil) {
        u.box(x0, fy + u.h(F_SMALL) / 2 - 3 * S, x0 + 6 * S, fy + u.h(F_SMALL) / 2 + 3 * S, g_m.toastOk ? kAccent : kWarn, 3 * S);
        u.text(F_SMALL, x0 + 12 * S, fy, g_m.toast, kText);
    } else {
        u.text(F_SMALL, x0, fy, "\xE2\x86\x91\xE2\x86\x93 Select   \xE2\x86\x90\xE2\x86\x92 Adjust   Enter Reset/Toggle   F7 Close", kText3);
    }
    u.dy = sdy;
    return content + 2 * pad + u.h(F_SMALL) + 24 * S;
}

// ---- input ----

int step_sel(int from, int dir) {
    int i = from;
    for (int n = 0; n < I_COUNT; n++) {
        i = (i + dir + I_COUNT) % I_COUNT;
        if (visible(i)) return i;
    }
    return from;
}

bool cursor_on_panel(POINT *bb) {
    CURSORINFO ci = {sizeof ci};
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !g_m.hwnd) return false;
    POINT p = ci.ptScreenPos;
    ScreenToClient(g_m.hwnd, &p);
    RECT rc;
    GetClientRect(g_m.hwnd, &rc);
    if (!g_m.bbW || rc.right <= 0 || rc.bottom <= 0) return false;
    bb->x = p.x * (LONG)g_m.bbW / rc.right, bb->y = p.y * (LONG)g_m.bbH / rc.bottom;
    return bb->x >= g_m.panel[0] && bb->x < g_m.panel[2] && bb->y >= g_m.panel[1] && bb->y < g_m.panel[3];
}

void input(DittoSettings &s, bool &changed, double t) {
    if (key_pressed(g_m.kUp, t, true)) g_m.sel = step_sel(g_m.sel, -1);
    if (key_pressed(g_m.kDown, t, true)) g_m.sel = step_sel(g_m.sel, 1);
    bool l = key_pressed(g_m.kLeft, t, true), r = key_pressed(g_m.kRight, t, true), e = key_pressed(g_m.kEnter, t, false);
    if (g_m.sel == I_BUTTONS) {
        if (l) g_m.btn = (g_m.btn + 2) % 3;
        if (r) g_m.btn = (g_m.btn + 1) % 3;
        if (e) press_button(s, g_m.btn, changed);
    } else {
        if (l) changed |= adjust(s, g_m.sel, -1);
        if (r) changed |= adjust(s, g_m.sel, 1);
        if (e) changed |= adjust(s, g_m.sel, 0);
    }
    if (key_pressed(g_m.kBack, t, false)) g_m.open = false;
    // the mouse, where its cursor shows over the panel (rows hit in the panel's coordinates, the scroll included)
    POINT p;
    bool on = cursor_on_panel(&p);
    bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool click = down && !g_m.lmb;
    g_m.lmb = down;
    if (!down) g_m.drag = -1;
    if (!on && g_m.drag < 0) return;
    float my = (float)p.y - g_m.scroll, mx = (float)p.x;
    int hit = -1;
    for (int i = 0; i < I_COUNT; i++)
        if (visible(i) && g_m.rowH[i] > 0 && my >= g_m.rowY[i] && my < g_m.rowY[i] + g_m.rowH[i]) hit = i;
    if (g_m.drag >= 0) hit = g_m.drag;
    if (hit < 0) return;
    if (g_m.drag < 0) g_m.sel = hit;
    if (hit == I_BUTTONS) {
        for (int b = 0; b < 3; b++)
            if (mx >= g_m.btnX[b][0] && mx < g_m.btnX[b][1]) {
                g_m.btn = b;
                if (click) press_button(s, b, changed);
            }
        return;
    }
    if (float *v = slider_of(s, hit)) {
        if (click) g_m.drag = hit;
        if (g_m.drag == hit) {
            Range r = range_of(hit);
            float t01 = fminf(fmaxf((mx - g_m.trackX0) / (g_m.trackX1 - g_m.trackX0), 0.0f), 1.0f);
            float nv = roundf((r.lo + t01 * (r.hi - r.lo)) / r.step) * r.step;
            if (nv != *v) *v = nv, changed = true;
        }
    } else if (click) {
        changed |= adjust(s, hit, mx < (g_m.trackX0 + g_m.trackX1) / 2 && hit != I_INDICATOR ? -1 : 1);
    }
}

// The menu's frame: its input (the game in front), the scroll, the panel into u (scale: pixels per unit)
void make_panel(Ui &u, float W, float H, float scale, double t, bool focus) {
    take_perf();
    DittoSettings s = ditto_settings();
    bool changed = false;
    if (g_m.open && focus) input(s, changed, t);
    if (changed) ditto_set_settings(s);
    u.s = scale, u.alpha = ease(g_m.anim), u.dx = -(1.0f - ease(g_m.anim)) * 420.0f * scale;
    u.q.reserve(2048);
    // (the scroll: the selected row kept in view)
    float viewTop = 16 * scale, viewBottom = H - 16 * scale - 48 * scale;
    if (g_m.rowH[g_m.sel] > 0) {
        float ry0 = g_m.rowY[g_m.sel] + g_m.scroll, ry1 = ry0 + g_m.rowH[g_m.sel];
        if (ry1 > viewBottom) g_m.scroll -= ry1 - viewBottom;
        if (ry0 < viewTop + 40 * scale) g_m.scroll += viewTop + 40 * scale - ry0;
        if (g_m.sel == step_sel(I_COUNT - 1, 1)) g_m.scroll = 0.0f;  // (the first row: the header in view)
        g_m.scroll = fminf(g_m.scroll, 0.0f);
    }
    u.dy = g_m.scroll;
    float content = build(u, s, W, H);
    if (content <= H - 32 * scale) g_m.scroll = 0.0f;
}

// The menu key (the game in front) and the panel's slide; false: nothing to draw
bool menu_state(double t, bool focus, HWND gameWnd) {
    g_m.kMenu.vk = g_cfg.menuKey;
    if (g_cfg.menuKey && focus && key_pressed(g_m.kMenu, t, false)) {
        g_m.open = !g_m.open;
        ditto_log("menu: %s\n", g_m.open ? "open" : "closed");
        if (g_m.open) subclass(gameWnd);
    }
    if (!focus && g_m.open) g_m.kMenu.down = false;
    static double lastT = t;
    float dt = (float)(t - lastT);
    lastT = t;
    g_m.anim = fminf(fmaxf(g_m.anim + (g_m.open ? 1.0f : -1.0f) * dt / 160.0f, 0.0f), 1.0f);
    g_menuShown = g_m.open;
    return g_m.anim > 0.0f;
}

// ---- the window procedure: the menu's keys kept from the game while it shows ----

WNDPROC g_origProc = nullptr;
HWND g_procHwnd = nullptr;

bool menu_vk(UINT vk) { return vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT || vk == VK_RETURN || vk == VK_BACK; }

LRESULT CALLBACK menu_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (g_menuShown) {
        switch (msg) {
        case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
            if (menu_vk((UINT)w) || (msg == WM_CHAR && w == '\r')) return 0;
            break;
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MOUSEWHEEL: {
            POINT p;
            if (cursor_on_panel(&p)) return 0;
            break;
        }
        case WM_INPUT: {
            RAWINPUT ri;
            UINT size = sizeof ri;
            if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
                POINT p;
                if ((ri.header.dwType == RIM_TYPEKEYBOARD && menu_vk(ri.data.keyboard.VKey)) ||
                    (ri.header.dwType == RIM_TYPEMOUSE && cursor_on_panel(&p)))
                    return DefWindowProcW(h, msg, w, l);
            }
            break;
        }
        }
    }
    return CallWindowProcW(g_origProc, h, msg, w, l);
}

void subclass(HWND h) {
    if (!h || h == g_procHwnd) return;
    if (g_procHwnd && g_origProc) SetWindowLongPtrW(g_procHwnd, GWLP_WNDPROC, (LONG_PTR)g_origProc);
    g_origProc = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)menu_proc);
    g_procHwnd = g_origProc ? h : nullptr;
}

// ---- the queues: ExecuteCommandLists hooked to find the one the game presents with ----

thread_local ID3D12CommandQueue *t_direct;  // (the last direct queue this thread executed on)
ID3D12CommandQueue *volatile g_direct;      // (any thread's)
typedef void(STDMETHODCALLTYPE *EclFn)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
EclFn o_ecl;

void STDMETHODCALLTYPE hk_ecl(ID3D12CommandQueue *q, UINT n, ID3D12CommandList *const *l) {
    if (q != t_direct && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) t_direct = q, g_direct = q;
    o_ecl(q, n, l);
}

// ---- each Present ----

// (why the menu could not draw: logged once per reason)
bool skip(const char *why) {
    static const char *last;
    if (why != last) ditto_log("menu: not drawn - %s\n", why), last = why;
    return false;
}

int out_mode(IDXGISwapChain *sc, DXGI_FORMAT f) {
    if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) return 1;
    if (f != DXGI_FORMAT_R10G10B10A2_UNORM) return 0;
    if (g_m.colorSpaceHdr10 >= 0) return g_m.colorSpaceHdr10 ? 2 : 0;
    // (SetColorSpace1 not seen - called before the hook: the display's mode decides)
    IDXGIOutput *o = nullptr;
    int m = 0;
    if (SUCCEEDED(sc->GetContainingOutput(&o))) {
        IDXGIOutput6 *o6 = nullptr;
        DXGI_OUTPUT_DESC1 d;
        if (SUCCEEDED(o->QueryInterface(IID_PPV_ARGS(&o6)))) {
            if (SUCCEEDED(o6->GetDesc1(&d)) && d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) m = 2;
            o6->Release();
        }
        o->Release();
    }
    g_m.colorSpaceHdr10 = m == 2;
    return m;
}

void on_present(IDXGISwapChain *sc) {
    std::lock_guard<std::mutex> lk(g_mu);
    DXGI_SWAP_CHAIN_DESC d;
    if (FAILED(sc->GetDesc(&d))) return;
    // (the largest swap chain is the game's: a launcher's or a tool's small one left alone)
    // (or the one presenting once the last stopped: a new swap chain, a smaller resolution)
    double t = now_ms();
    UINT area = d.BufferDesc.Width * d.BufferDesc.Height;
    if (sc != g_m.main) {
        if (area <= g_m.mainArea && t - g_m.mainT < 1000.0) return;
        g_m.main = sc, g_m.colorSpaceHdr10 = -1;
    }
    g_m.mainArea = area, g_m.mainT = t, g_m.bbW = d.BufferDesc.Width, g_m.bbH = d.BufferDesc.Height;
    g_m.hwnd = d.OutputWindow;
    record_frame();
    // the keys (the game's window in front)
    HWND fg = GetForegroundWindow();
    bool focus = fg && (fg == d.OutputWindow || GetAncestor(d.OutputWindow, GA_ROOT) == fg);
    if (!menu_state(t, focus, d.OutputWindow)) return;

    // the queue: the direct queue this thread last executed on (the swap chain's own is not to be asked for), or the
    // last any thread did
    ID3D12CommandQueue *queue = t_direct ? t_direct : g_direct;
    if (!queue) return (void)skip("no direct queue seen");
    queue->AddRef();
    struct QRel { ID3D12CommandQueue *q; ~QRel() { q->Release(); } } qr{queue};
    ID3D12Device *dev = nullptr;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&dev)))) return (void)skip("the queue's device");
    if (dev != g_gfx.dev) {
        if (g_gfx.dev) gfx_release();
        if (!gfx_init(dev)) {
            static bool logged;
            if (!logged) ditto_log("menu: could not set up its drawing\n"), logged = true;
            gfx_release(), dev->Release();
            return;
        }
    }
    dev->Release();
    IDXGISwapChain3 *sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) return (void)skip("no IDXGISwapChain3");
    UINT bi = sc3->GetCurrentBackBufferIndex();
    sc3->Release();
    ID3D12Resource *bb = nullptr;
    if (FAILED(sc->GetBuffer(bi, IID_PPV_ARGS(&bb)))) return (void)skip("GetBuffer (not a D3D12 swap chain?)");
    struct BRel { ID3D12Resource *r; ~BRel() { r->Release(); } } br{bb};
    {
        ID3D12Device *bdev = nullptr;
        bb->GetDevice(IID_PPV_ARGS(&bdev));
        if (bdev) bdev->Release();
        if (bdev != g_gfx.dev) return (void)skip("the back buffer is on another device than the queue");
    }
    D3D12_RESOURCE_DESC bd = bb->GetDesc();
    DXGI_FORMAT fmt = bd.Format;
    if (fmt == DXGI_FORMAT_R8G8B8A8_TYPELESS) fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (fmt == DXGI_FORMAT_B8G8R8A8_TYPELESS) fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (fmt == DXGI_FORMAT_R10G10B10A2_TYPELESS) fmt = DXGI_FORMAT_R10G10B10A2_UNORM;
    if (fmt == DXGI_FORMAT_R16G16B16A16_TYPELESS) fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
    float W = (float)bd.Width, H = (float)bd.Height;
    float scale = fminf(fmaxf(H / 1080.0f, 0.75f), 3.0f);
    ID3D12PipelineState *pso = pso_for(fmt);
    if (!pso) return (void)skip("no pipeline for the back buffer's format");
    if (!ensure_font(scale)) return (void)skip("the font");
    {
        static UINT64 logged;
        UINT64 key = ((UINT64)bd.Width << 32) | ((UINT64)bd.Height << 8) | (UINT64)fmt;
        if (key != logged) ditto_log("menu: drawing on %llux%u, format %d\n", (unsigned long long)bd.Width, bd.Height, (int)fmt), logged = key;
    }

    // the menu: input, then the panel
    Ui u;
    make_panel(u, W, H, scale, t, focus);
    if (u.q.empty()) return;

    // the draw, on the swap chain's queue before the game's Present
    Gfx::Slot &sl = g_gfx.slot[g_gfx.cur];
    wait_fence(sl.fence);
    if (g_gfx.fontUp && !g_gfx.fontPending && g_gfx.fence->GetCompletedValue() >= g_gfx.fontUpFence) rel(g_gfx.fontUp);
    memcpy(sl.map, u.q.data(), u.q.size() * sizeof(Quad));
    sl.alloc->Reset();
    ID3D12GraphicsCommandList *cl = g_gfx.cl;
    cl->Reset(sl.alloc, pso);
    D3D12_RESOURCE_BARRIER b[2] = {};
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, b[0].Transition.pResource = bb;
    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT, b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    int nb = 1;
    if (g_gfx.fontPending) {
        D3D12_TEXTURE_COPY_LOCATION dst = {g_gfx.font, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, src = {g_gfx.fontUp, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.SubresourceIndex = 0;
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8_UNORM, (UINT)g_font.w, (UINT)g_font.h, 1, (UINT)g_font.w};
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, b[1].Transition.pResource = g_gfx.font;
        b[1].Transition.Subresource = 0;
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST, b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        nb = 2;
    }
    cl->ResourceBarrier(nb, b);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_gfx.rtv->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)g_gfx.cur * g_gfx.rtvInc;
    D3D12_RENDER_TARGET_VIEW_DESC rv = {};
    rv.Format = fmt, rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_gfx.dev->CreateRenderTargetView(bb, &rv, rtv);
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = {0, 0, W, H, 0, 1};
    // (everything within the panel: its rows scroll under the header's and footer's edges)
    D3D12_RECT sr = {0, (LONG)g_m.panel[1], (LONG)fminf(fmaxf(g_m.panel[2] + 32 * scale, 0.0f), W), (LONG)fminf(g_m.panel[3] + 24 * scale, H)};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sr);
    cl->SetGraphicsRootSignature(g_gfx.rs);
    ID3D12DescriptorHeap *heaps[1] = {g_gfx.srv};
    cl->SetDescriptorHeaps(1, heaps);
    float k[4] = {W, H, (float)out_mode(sc, fmt), 203.0f};
    cl->SetGraphicsRoot32BitConstants(0, 4, k, 0);
    cl->SetGraphicsRootShaderResourceView(1, sl.buf->GetGPUVirtualAddress());
    cl->SetGraphicsRootDescriptorTable(2, g_gfx.srv->GetGPUDescriptorHandleForHeapStart());
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(6, (UINT)u.q.size(), 0, 0);
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET, b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cl->ResourceBarrier(1, b);
    if (FAILED(cl->Close())) return;
    ID3D12CommandList *lists[1] = {cl};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(g_gfx.fence, ++g_gfx.fenceVal);
    sl.fence = g_gfx.fenceVal;
    if (g_gfx.fontPending) g_gfx.fontPending = false, g_gfx.fontUpFence = g_gfx.fenceVal;
    g_gfx.cur = (g_gfx.cur + 1) % kSlots;
}

// ---- the window (Vulkan): the panel drawn on the CPU into a layered window over the game ----

struct Win {
    HWND wnd = nullptr;   // (ours: layered, click-through, topmost, never activated)
    HWND game = nullptr;  // (the game's: this process's largest visible top-level window)
    double findT = -1e9, drawT = -1e9, topT = -1e9;
    HDC mem = nullptr;
    HBITMAP bmp = nullptr;
    uint32_t *bits = nullptr;
    int bw = 0, bh = 0;
    POINT org = {};
    std::vector<float> acc;  // (premultiplied RGBA, bw x bh)
    std::vector<Quad> last;  // (the last panel drawn: an unchanged one is not drawn again)
    bool shown = false;
};
Win g_win;
volatile bool g_winMode = false;  // (the window draws the menu: the upscale records the frame times)

BOOL CALLBACK find_proc(HWND h, LPARAM l) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || h == g_win.wnd || !IsWindowVisible(h) || IsIconic(h)) return TRUE;
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    RECT rc;
    if (!GetClientRect(h, &rc)) return TRUE;
    auto *best = (std::pair<HWND, LONG> *)l;
    LONG area = rc.right * rc.bottom;
    if (area > best->second) *best = {h, area};
    return TRUE;
}

HWND find_game_window() {
    std::pair<HWND, LONG> best = {nullptr, 64 * 64};
    EnumWindows(find_proc, (LPARAM)&best);
    return best.first;
}

// the atlas, bilinear (D3D12's sampler: texel centres at + 0.5, the edges clamped)
float font_at(float u, float v) {
    float x = u * g_font.w - 0.5f, y = v * g_font.h - 0.5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - x0, fy = y - y0;
    auto at = [](int x, int y) {
        x = x < 0 ? 0 : x >= g_font.w ? g_font.w - 1 : x;
        y = y < 0 ? 0 : y >= g_font.h ? g_font.h - 1 : y;
        return g_font.pix[(size_t)y * g_font.w + x] * (1.0f / 255.0f);
    };
    float a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * fx, b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * fx;
    return a + (b - a) * fy;
}

// The draw list into acc (premultiplied RGBA, bw wide), rows y0c - y1c: overlay.hlsl's quads, SDR, on the CPU. A box's
// distance is worked out only near its edge - inside, a fill covers fully and an outline not at all; a glyph on whole
// pixels reads its texels as they are (the atlas is drawn at the screen's scale), a spaced one (the capitals) bilinear
void raster(const std::vector<Quad> &qs, float *acc, int bw, int y0c, int y1c) {
    auto blend = [](float *o, const float *c, float a) {
        float sa = c[3] * a, ia = 1.0f - sa;
        o[0] = o[0] * ia + c[0] * sa, o[1] = o[1] * ia + c[1] * sa, o[2] = o[2] * ia + c[2] * sa, o[3] = o[3] * ia + sa;
    };
    for (const Quad &q : qs) {
        bool glyph = q.p[1] == 1.0f, outline = q.p[1] == 2.0f;
        float pad = glyph ? 0.0f : 1.0f;  // (a box: a pixel more around, for its antialiased edge)
        float ax = q.rect[0] - pad, ay = q.rect[1] - pad, bx = q.rect[2] + pad, by = q.rect[3] + pad;
        // (the pixels whose centres the quad covers)
        int x0 = (int)ceilf(ax - 0.5f), x1 = (int)ceilf(bx - 0.5f), y0 = (int)ceilf(ay - 0.5f), y1 = (int)ceilf(by - 0.5f);
        x0 = x0 < 0 ? 0 : x0, x1 = x1 > bw ? bw : x1, y0 = y0 < y0c ? y0c : y0, y1 = y1 > y1c ? y1c : y1;
        if (x0 >= x1 || y0 >= y1) continue;
        float cx = 0.5f * (q.rect[0] + q.rect[2]), cy = 0.5f * (q.rect[1] + q.rect[3]);
        float hx = 0.5f * (q.rect[2] - q.rect[0]), hy = 0.5f * (q.rect[3] - q.rect[1]);
        float r = fminf(q.p[0], fminf(hx, hy)), gh = fmaxf(q.rect[3] - q.rect[1], 1.0f);
        // (an outline: nothing deeper inside than its width and a pixel)
        float m = outline ? q.p[2] + 1.0f : 0.5f;
        for (int y = y0; y < y1; y++) {
            float py = y + 0.5f, t = fminf(fmaxf((py - q.rect[1]) / gh, 0.0f), 1.0f);
            float c[4];
            for (int k = 0; k < 4; k++) c[k] = q.c0[k] + (q.c1[k] - q.c0[k]) * t;
            if (c[3] <= 0.0f) continue;
            float *row = acc + (size_t)y * bw * 4;
            if (glyph && (fabsf(ax - roundf(ax)) > 1e-3f || fabsf(ay - roundf(ay)) > 1e-3f)) {
                float v = q.uv[1] + (q.uv[3] - q.uv[1]) * (py - ay) / (by - ay);
                for (int x = x0; x < x1; x++) {
                    float a = font_at(q.uv[0] + (q.uv[2] - q.uv[0]) * (x + 0.5f - ax) / (bx - ax), v);
                    if (a > 0.0f) blend(row + x * 4, c, a);
                }
                continue;
            }
            if (glyph) {
                int tx0 = (int)(q.uv[0] * g_font.w + (x0 + 0.5f - ax)), ty = (int)(q.uv[1] * g_font.h + (py - ay));
                if (ty < 0 || ty >= g_font.h) continue;
                const uint8_t *src = g_font.pix.data() + (size_t)ty * g_font.w;
                for (int x = x0; x < x1; x++) {
                    int tx = tx0 + (x - x0);
                    uint8_t a8 = tx >= 0 && tx < g_font.w ? src[tx] : 0;
                    if (a8) blend(row + x * 4, c, a8 * (1.0f / 255.0f));
                }
                continue;
            }
            float ady = fabsf(py - cy), dy2 = ady - hy + r;
            // the core: |px - cx| <= half, the coverage certain - a fill 1, an outline 0
            float half = ady <= hy - r - m ? hx - m : ady <= hy - m ? hx - r - m : -1.0f;
            int xa = x1, xb = x1;
            if (half > 0.0f) {
                xa = (int)ceilf(cx - half - 0.5f), xb = (int)floorf(cx + half - 0.5f) + 1;
                xa = xa < x0 ? x0 : xa > x1 ? x1 : xa, xb = xb < xa ? xa : xb > x1 ? x1 : xb;
            }
            auto edge = [&](int x) {  // (the rounded box's distance: a square root only in its corners)
                float dx2 = fabsf(x + 0.5f - cx) - hx + r;
                float d = (dx2 > 0.0f && dy2 > 0.0f) ? sqrtf(dx2 * dx2 + dy2 * dy2) - r : fmaxf(dx2, dy2) - r;
                float a = fminf(fmaxf(0.5f - d, 0.0f), 1.0f);
                if (outline) a *= fminf(fmaxf(0.5f + d + q.p[2], 0.0f), 1.0f);
                if (a > 0.0f) blend(row + x * 4, c, a);
            };
            for (int x = x0; x < xa; x++) edge(x);
            if (!outline)
                for (int x = xa; x < xb; x++) blend(row + x * 4, c, 1.0f);
            for (int x = xb; x < x1; x++) edge(x);
        }
    }
}

// The panel into bits (premultiplied BGRA, pitch pixels a row; bw x bh of it, the rest left as it is), through acc
// (bw x bh premultiplied RGBA): in bands of rows across the CPU's cores. Only the panel's rows are touched - as D3D12's
// scissor, its rows scrolling under the header's and footer's edges; those outside stay as made: transparent
void paint(const std::vector<Quad> &q, float *acc, uint32_t *bits, int pitch, int bw, int bh, float scale) {
    int y0 = (int)fmaxf(g_m.panel[1], 0.0f), y1 = (int)fminf(g_m.panel[3] + 24 * scale, (float)bh);
    if (y0 >= y1) return;
    auto band = [&](int b0, int b1) {
        memset(acc + (size_t)b0 * bw * 4, 0, (size_t)(b1 - b0) * bw * 4 * sizeof(float));
        raster(q, acc, bw, b0, b1);
        for (int y = b0; y < b1; y++) {
            const float *a = acc + (size_t)y * bw * 4;
            uint32_t *o = bits + (size_t)y * pitch;
            for (int x = 0; x < bw; x++, a += 4) {
                auto b8 = [](float v) { return (uint32_t)(fminf(fmaxf(v, 0.0f), 1.0f) * 255.0f + 0.5f); };
                o[x] = b8(a[3]) << 24 | b8(a[0]) << 16 | b8(a[1]) << 8 | b8(a[2]);
            }
        }
    };
    // (half the cores, 2 - 8: the game keeps its own)
    int n = (int)std::thread::hardware_concurrency() / 2;
    n = n < 2 ? 2 : n > 8 ? 8 : n;
    std::vector<std::thread> th;
    int step = (y1 - y0 + n - 1) / n;
    for (int b = y0 + step; b < y1; b += step) th.emplace_back(band, b, b + step < y1 ? b + step : y1);
    band(y0, y0 + step < y1 ? y0 + step : y1);
    for (auto &t : th) t.join();
}

void win_hide() {
    if (g_win.shown) ShowWindow(g_win.wnd, SW_HIDE), g_win.shown = false, g_win.last.clear();
}

// One turn of the window's thread: the key, the panel, drawn when it changed (30 a second at most, but sliding)
void win_frame() {
    double t = now_ms();
    if (!g_win.game || !IsWindow(g_win.game) || t - g_win.findT > 1000.0) g_win.game = find_game_window(), g_win.findT = t;
    HWND game = g_win.game, fg = GetForegroundWindow();
    bool focus = game && fg && (fg == game || GetAncestor(game, GA_ROOT) == fg);
    std::lock_guard<std::mutex> lk(g_mu);
    // (out of the game - another window in front -, the menu hides with it)
    if (!menu_state(t, focus, game) || !focus || IsIconic(game)) return win_hide();
    RECT rc;
    POINT org = {0, 0};
    if (!GetClientRect(game, &rc) || !ClientToScreen(game, &org) || rc.right < 64 || rc.bottom < 64) return win_hide();
    float W = (float)rc.right, H = (float)rc.bottom;
    float scale = fminf(fmaxf(H / 1080.0f, 0.75f), 3.0f);
    if (fabsf(g_font.scale - scale) >= 0.01f || g_font.pix.empty()) {
        if (!build_atlas(scale, g_font.f, g_font.pix, g_font.w, g_font.h)) return (void)skip("the font"), win_hide();
        g_font.scale = scale;
    }
    g_m.hwnd = game, g_m.bbW = (UINT)rc.right, g_m.bbH = (UINT)rc.bottom;
    Ui u;
    make_panel(u, W, H, scale, t, focus);
    int bw = (int)fminf(ceilf(420.0f * scale), W), bh = (int)H;
    bool same = g_win.shown && bw == g_win.bw && bh == g_win.bh && org.x == g_win.org.x && org.y == g_win.org.y &&
                u.q.size() == g_win.last.size() && !memcmp(u.q.data(), g_win.last.data(), u.q.size() * sizeof(Quad));
    if (same || (g_win.shown && g_m.anim >= 1.0f && t - g_win.drawT < 33.0)) return;
    // the bitmap (a top-down 32-bit DIB, premultiplied BGRA), at the panel's size
    if (bw != g_win.bw || bh != g_win.bh || !g_win.bmp) {
        if (g_win.bmp) DeleteObject(g_win.bmp), g_win.bmp = nullptr;
        if (!g_win.mem) g_win.mem = CreateCompatibleDC(nullptr);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader, bi.bmiHeader.biWidth = bw, bi.bmiHeader.biHeight = -bh;
        bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
        void *bits = nullptr;
        g_win.bmp = g_win.mem ? CreateDIBSection(g_win.mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        if (!g_win.bmp) return (void)skip("its bitmap"), win_hide();
        SelectObject(g_win.mem, g_win.bmp);
        g_win.bits = (uint32_t *)bits, g_win.bw = bw, g_win.bh = bh;
        memset(g_win.bits, 0, (size_t)bw * bh * 4);  // (the rows the panel never reaches: transparent)
        g_win.acc.assign((size_t)bw * bh * 4, 0.0f);
        ditto_log("menu: drawn on a window over the game, %dx%d\n", rc.right, rc.bottom);
    }
    paint(u.q, g_win.acc.data(), g_win.bits, bw, bw, bh, scale);
    POINT src = {0, 0};
    SIZE sz = {bw, bh};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    HDC screen = GetDC(nullptr);
    BOOL ok = UpdateLayeredWindow(g_win.wnd, screen, &org, &sz, g_win.mem, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
    if (!ok) return (void)skip("UpdateLayeredWindow failed"), win_hide();
    if (!g_win.shown) ShowWindow(g_win.wnd, SW_SHOWNOACTIVATE), g_win.shown = true, g_win.topT = -1e9;
    // (kept above the game - a fullscreen window may be topmost itself)
    if (t - g_win.topT > 1000.0)
        SetWindowPos(g_win.wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER), g_win.topT = t;
    g_win.org = org, g_win.drawT = t;
    g_win.last.swap(u.q);
}

DWORD WINAPI win_thread(void *) {
    // (per-monitor DPI aware: the game's client area in physical pixels)
    typedef void *(WINAPI * SetDpiCtx)(void *);
    if (auto f = (SetDpiCtx)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext")) f((void *)-4);
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = DefWindowProcW, wc.hInstance = GetModuleHandleW(nullptr), wc.lpszClassName = L"DittoSRMenu";
    RegisterClassExW(&wc);
    g_win.wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                wc.lpszClassName, L"DittoSR", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_win.wnd) {
        ditto_log("menu: could not make its window\n");
        g_winMode = false;
        return 0;
    }
    ditto_log("menu: ready (F7, menu_key) - drawn on a window of its own over the game (Vulkan)\n");
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) TranslateMessage(&msg), DispatchMessageW(&msg);
        win_frame();
        Sleep(g_m.anim > 0.0f ? 8 : 30);
    }
}

// ---- the hooks ----

typedef HRESULT(STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *Present1Fn)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
typedef HRESULT(STDMETHODCALLTYPE *ResizeFn)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *Resize1Fn)(IDXGISwapChain3 *, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT *, IUnknown *const *);
typedef HRESULT(STDMETHODCALLTYPE *ColorSpaceFn)(IDXGISwapChain3 *, DXGI_COLOR_SPACE_TYPE);
PresentFn o_present;
Present1Fn o_present1;
ResizeFn o_resize;
Resize1Fn o_resize1;
ColorSpaceFn o_colorSpace;

HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) on_present(sc);
    return o_present(sc, sync, flags);
}
HRESULT STDMETHODCALLTYPE hk_present1(IDXGISwapChain1 *sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS *p) {
    if (!(flags & DXGI_PRESENT_TEST)) on_present(sc);
    return o_present1(sc, sync, flags, p);
}
// (a resize: the menu's draws done with the old buffers first)
HRESULT STDMETHODCALLTYPE hk_resize(IDXGISwapChain *sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        wait_fence(g_gfx.fenceVal);
    }
    return o_resize(sc, n, w, h, f, fl);
}
HRESULT STDMETHODCALLTYPE hk_resize1(IDXGISwapChain3 *sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl, const UINT *m, IUnknown *const *q) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        wait_fence(g_gfx.fenceVal);
    }
    return o_resize1(sc, n, w, h, f, fl, m, q);
}
HRESULT STDMETHODCALLTYPE hk_colorSpace(IDXGISwapChain3 *sc, DXGI_COLOR_SPACE_TYPE c) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (sc == g_m.main || !g_m.main) g_m.colorSpaceHdr10 = c == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    }
    return o_colorSpace(sc, c);
}

template <class T> bool patch(void **vt, int i, void *hook, T *orig) {
    if (vt[i] == hook) return true;
    DWORD old;
    if (!VirtualProtect(&vt[i], sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) return false;
    *orig = (T)vt[i];
    vt[i] = hook;
    VirtualProtect(&vt[i], sizeof(void *), old, &old);
    return true;
}

}  // namespace

// The menu, once per process: the FidelityFX DLL and the XeSS proxy may both be loaded, a game may have a D3D12 and a
// Vulkan context (false: another has it, or there is no menu key)
static bool claim_menu() {
    static int claimed = -1;
    if (claimed >= 0) return false;
    claimed = 0;
    if (!g_cfg.menuKey) return false;
    char name[64];
    snprintf(name, sizeof name, "Local\\DittoSR_menu_%lu", GetCurrentProcessId());
    HANDLE mx = CreateMutexA(nullptr, FALSE, name);
    if (!mx || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mx) CloseHandle(mx);
        ditto_log("menu: another DittoSR in this process has it\n");
        return false;
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcMs = 1000.0 / (double)f.QuadPart;
    claimed = 1;
    return true;
}

void ditto_overlay_install(ID3D12Device *device) {
    if (!device || !claim_menu()) return;
    // a dummy swap chain on the game's device: its vtable is the game's swap chain's
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = DefWindowProcW, wc.hInstance = GetModuleHandleW(nullptr), wc.lpszClassName = L"DittoSRMenuProbe";
    RegisterClassExW(&wc);
    HWND hw = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    IDXGIFactory2 *fac = nullptr;
    ID3D12CommandQueue *q = nullptr;
    IDXGISwapChain1 *sc = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = 64, sd.Height = 64, sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM, sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT, sd.BufferCount = 2, sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    bool ok = hw && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) && SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) &&
              SUCCEEDED(fac->CreateSwapChainForHwnd(q, hw, &sd, nullptr, nullptr, &sc));
    if (ok) {
        // (pinned: the hooks point into this DLL - it must outlive every swap chain)
        HMODULE self;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)&ditto_overlay_install, &self);
        void **vt = *(void ***)sc;
        ok = patch(vt, 8, (void *)hk_present, &o_present) && patch(vt, 13, (void *)hk_resize, &o_resize) &&
             patch(vt, 22, (void *)hk_present1, &o_present1) && patch(vt, 38, (void *)hk_colorSpace, &o_colorSpace) &&
             patch(vt, 39, (void *)hk_resize1, &o_resize1) && patch(*(void ***)q, 10, (void *)hk_ecl, &o_ecl);
    }
    ditto_log("menu: %s\n", ok ? "ready (F7, menu_key)" : "could not hook the swap chain");
    if (sc) sc->Release();
    if (q) q->Release();
    if (fac) fac->Release();
    if (hw) DestroyWindow(hw);
}

void ditto_overlay_install_window() {
    if (!claim_menu()) return;
    // (pinned: the window's thread runs in this DLL)
    HMODULE self;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)&ditto_overlay_install_window, &self);
    g_winMode = true;
    HANDLE th = CreateThread(nullptr, 0, win_thread, nullptr, 0, nullptr);
    if (th) CloseHandle(th);
    else g_winMode = false, ditto_log("menu: could not start its thread\n");
}

void ditto_overlay_tick() {
    if (g_winMode) record_frame();
}

// The harness' look at the window's menu (menu=1): the panel open at W x H, the row sel selected, drawn as the window
// draws it - premultiplied BGRA into bgra (W x H). 0: no font
extern "C" __declspec(dllexport) int DittoSRMenuSnapshot(int W, int H, int sel, uint32_t *bgra) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_qpcMs <= 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcMs = 1000.0 / (double)f.QuadPart;
    }
    float scale = fminf(fmaxf(H / 1080.0f, 0.75f), 3.0f);
    if (fabsf(g_font.scale - scale) >= 0.01f || g_font.pix.empty()) {
        if (!build_atlas(scale, g_font.f, g_font.pix, g_font.w, g_font.h)) return 0;
        g_font.scale = scale;
    }
    Menu keep = g_m;
    g_m.open = true, g_m.anim = 1.0f, g_m.sel = sel;
    Ui u;
    make_panel(u, (float)W, (float)H, scale, now_ms(), false);
    // (as the window: its own strip, its buffer kept)
    int bw = (int)fminf(ceilf(420.0f * scale), (float)W);
    static std::vector<float> acc;
    acc.resize((size_t)bw * H * 4);
    paint(u.q, acc.data(), bgra, W, bw, H, scale);
    g_m = keep;
    return 1;
}
