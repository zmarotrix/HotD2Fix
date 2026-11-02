// hod2fix - Direct3D 9Ex backend.
//
// Design notes
//  * The device's implicit swap chain lives on a hidden 1x1 window and is never
//    presented.  The game window gets an *additional* swap chain, which can be
//    recreated on resize / Alt+Enter without resetting the device, so no game
//    state is ever lost.
//  * Every DirectDraw render target is a render-target texture at
//    (game size * scale).  With MSAA a multisampled surface is rendered to and
//    resolved into the texture on demand.
//  * Pre-transformed (XYZRHW) vertices are rescaled on the CPU so 2D/HUD
//    geometry lands where the game expects it at the higher resolution.
#include "renderer.h"
#include <d3d9.h>
#include <vector>
#include <math.h>
#include <psapi.h>
#include <wincodec.h>
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

#pragma comment(lib, "winmm.lib")

namespace R {

struct Texture {
    IDirect3DTexture9* tex;
    int w, h, levels;
};

struct Target {
    int gw, gh;                 // game size
    int sw, sh;                 // scaled size
    IDirect3DTexture9* tex;     // resolved colour (sampled / presented)
    IDirect3DSurface9* texSurf; // level 0 of tex
    IDirect3DSurface9* msaa;    // multisampled render surface (or null)
    bool dirty;                 // msaa has content newer than tex
    IDirect3DSurface9* native;  // gw x gh RT for downscaled readback
    IDirect3DSurface9* sysmem;  // gw x gh lockable copy
};

struct Depth {
    int gw, gh, sw, sh;
    IDirect3DSurface9* surf;
};

typedef HRESULT (WINAPI *PFN_Direct3DCreate9Ex)(UINT, IDirect3D9Ex**);

static HMODULE              s_d3d9;
static IDirect3D9Ex*        s_d3d;
static IDirect3DDevice9Ex*  s_dev;
static IDirect3DSwapChain9* s_swap;
static D3DPRESENT_PARAMETERS s_swapPP;
static HWND                 s_hwnd, s_dummy;
static int                  s_scale = 1;
static D3DMULTISAMPLE_TYPE  s_msType = D3DMULTISAMPLE_NONE;
static DWORD                s_msQuality = 0;
static Target*              s_curRT;
static Depth*               s_curDS;
static bool                 s_inScene;
static IDirect3DTexture9*   s_scratch;
static int                  s_scratchW, s_scratchH;
static IDirect3DStateBlock9* s_saveSB;
static std::vector<BYTE>    s_vtmp;
static RECT                 s_lastDst;
static int                  s_lastGW, s_lastGH;
static bool                 s_haveMapping;
static int                  s_clientW, s_clientH;
static DWORD                s_maxAniso = 16;

// ---------------------------------------------------------------------------
// Frame pacing
// ---------------------------------------------------------------------------
static LARGE_INTEGER s_qpf, s_next;
static HANDLE        s_timer;
static bool          s_waited;   // WaitForVerticalBlank already paced this frame

static void PreciseSleepUntil(LONGLONG target)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONGLONG remain = target - now.QuadPart;
    if (remain <= 0) return;
    // Sleep on a high resolution timer until ~1 ms before the deadline, then spin.
    LONGLONG spinTicks = s_qpf.QuadPart / 1000;
    if (remain > spinTicks && s_timer) {
        LARGE_INTEGER due;
        due.QuadPart = -((remain - spinTicks) * 10000000LL / s_qpf.QuadPart);
        if (SetWaitableTimer(s_timer, &due, 0, NULL, NULL, FALSE))
            WaitForSingleObject(s_timer, 100);
    }
    for (;;) {
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= target) break;
        YieldProcessor();
    }
}

static void Pace()
{
    if (g_cfg.fpsLimit <= 0) return;
    LONGLONG period = s_qpf.QuadPart / g_cfg.fpsLimit;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (s_next.QuadPart == 0 || now.QuadPart > s_next.QuadPart + period * 3) {
        // first frame or we fell far behind (loading): resynchronise
        s_next.QuadPart = now.QuadPart + period;
        return;
    }
    PreciseSleepUntil(s_next.QuadPart);
    s_next.QuadPart += period;
}

void WaitVBlank()
{
    Pace();
    s_waited = true;
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = NULL; } }

static IDirect3DSurface9* RTSurf(Target* t) { return t->msaa ? t->msaa : t->texSurf; }

static void Resolve(Target* t)
{
    if (t && t->msaa && t->dirty) {
        s_dev->StretchRect(t->msaa, NULL, t->texSurf, NULL, D3DTEXF_NONE);
        t->dirty = false;
    }
}

static RECT ScaleRect(Target* t, const RECT& r)
{
    RECT o;
    o.left   = MulDiv(r.left,   t->sw, t->gw);
    o.right  = MulDiv(r.right,  t->sw, t->gw);
    o.top    = MulDiv(r.top,    t->sh, t->gh);
    o.bottom = MulDiv(r.bottom, t->sh, t->gh);
    if (o.left < 0) o.left = 0;
    if (o.top < 0) o.top = 0;
    if (o.right > t->sw) o.right = t->sw;
    if (o.bottom > t->sh) o.bottom = t->sh;
    return o;
}

static bool CreateSwapChain()
{
    SafeRelease(s_swap);
    RECT rc;
    GetClientRect(s_hwnd, &rc);
    s_clientW = rc.right - rc.left;
    s_clientH = rc.bottom - rc.top;
    if (s_clientW <= 0 || s_clientH <= 0) return false;

    ZeroMemory(&s_swapPP, sizeof(s_swapPP));
    s_swapPP.BackBufferWidth  = s_clientW;
    s_swapPP.BackBufferHeight = s_clientH;
    s_swapPP.BackBufferFormat = D3DFMT_X8R8G8B8;
    s_swapPP.BackBufferCount  = 2;
    s_swapPP.SwapEffect       = D3DSWAPEFFECT_FLIPEX;
    s_swapPP.hDeviceWindow    = s_hwnd;
    s_swapPP.Windowed         = TRUE;
    s_swapPP.PresentationInterval = g_cfg.vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;

    HRESULT hr = s_dev->CreateAdditionalSwapChain(&s_swapPP, &s_swap);
    if (FAILED(hr)) {
        Log("FLIPEX swap chain failed (0x%08X), falling back to DISCARD", hr);
        s_swapPP.SwapEffect = D3DSWAPEFFECT_DISCARD;
        s_swapPP.BackBufferCount = 1;
        hr = s_dev->CreateAdditionalSwapChain(&s_swapPP, &s_swap);
    }
    if (FAILED(hr)) {
        Log("CreateAdditionalSwapChain failed: 0x%08X", hr);
        s_swap = NULL;
        return false;
    }
    LogV("swap chain %dx%d effect %d", s_clientW, s_clientH, s_swapPP.SwapEffect);
    return true;
}

static LRESULT CALLBACK DummyProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

bool Ready() { return s_dev != NULL; }
float ScaleX() { return (float)s_scale; }
float ScaleY() { return (float)s_scale; }

bool Init(HWND hwnd)
{
    if (s_dev) {
        if (hwnd && hwnd != s_hwnd) { s_hwnd = hwnd; CreateSwapChain(); }
        return true;
    }
    s_hwnd = hwnd;
    QueryPerformanceFrequency(&s_qpf);
    s_timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    if (!s_timer) s_timer = CreateWaitableTimerW(NULL, FALSE, NULL);

    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    lstrcatA(path, "\\d3d9.dll");
    s_d3d9 = LoadLibraryA(path);
    if (!s_d3d9) { Log("cannot load %s", path); return false; }
    PFN_Direct3DCreate9Ex create = (PFN_Direct3DCreate9Ex)GetProcAddress(s_d3d9, "Direct3DCreate9Ex");
    if (!create || FAILED(create(D3D_SDK_VERSION, &s_d3d))) { Log("Direct3DCreate9Ex failed"); return false; }

    // Choose the adapter that drives the monitor the game window is on.
    UINT adapter = D3DADAPTER_DEFAULT;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    for (UINT i = 0; i < s_d3d->GetAdapterCount(); i++)
        if (s_d3d->GetAdapterMonitor(i) == mon) { adapter = i; break; }
    D3DADAPTER_IDENTIFIER9 id;
    if (SUCCEEDED(s_d3d->GetAdapterIdentifier(adapter, 0, &id)))
        Log("adapter %u: %s", adapter, id.Description);

    // Internal resolution: integer multiple of 480 that fits the monitor.
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoA(mon, &mi);
    int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
    s_scale = g_cfg.renderScale > 0 ? g_cfg.renderScale : monH / (g_gameH > 0 ? g_gameH : 480);
    if (s_scale < 1) s_scale = 1;
    if (s_scale > 8) s_scale = 8;
    Log("render scale %dx (%dx%d internal)", s_scale, g_gameW * s_scale, g_gameH * s_scale);

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DummyProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "hod2fix_dummy";
    RegisterClassA(&wc);
    s_dummy = CreateWindowA("hod2fix_dummy", "", WS_POPUP, 0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);

    D3DPRESENT_PARAMETERS pp = {};
    pp.BackBufferWidth = 1;
    pp.BackBufferHeight = 1;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = s_dummy;
    pp.Windowed = TRUE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    DWORD flags = D3DCREATE_MULTITHREADED | D3DCREATE_NOWINDOWCHANGES;
    HRESULT hr = s_d3d->CreateDeviceEx(adapter, D3DDEVTYPE_HAL, s_dummy,
        flags | D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, NULL, &s_dev);
    if (FAILED(hr))
        hr = s_d3d->CreateDeviceEx(adapter, D3DDEVTYPE_HAL, s_dummy,
            flags | D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, NULL, &s_dev);
    if (FAILED(hr)) { Log("CreateDeviceEx failed: 0x%08X", hr); s_dev = NULL; return false; }

    s_dev->SetMaximumFrameLatency(1);

    D3DCAPS9 caps;
    s_dev->GetDeviceCaps(&caps);
    s_maxAniso = caps.MaxAnisotropy ? caps.MaxAnisotropy : 1;

    if (g_cfg.msaa >= 2) {
        for (int m = g_cfg.msaa; m >= 2; m--) {
            DWORD q1 = 0, q2 = 0;
            if (SUCCEEDED(s_d3d->CheckDeviceMultiSampleType(adapter, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, TRUE, (D3DMULTISAMPLE_TYPE)m, &q1)) &&
                SUCCEEDED(s_d3d->CheckDeviceMultiSampleType(adapter, D3DDEVTYPE_HAL, D3DFMT_D24S8, TRUE, (D3DMULTISAMPLE_TYPE)m, &q2))) {
                s_msType = (D3DMULTISAMPLE_TYPE)m;
                s_msQuality = 0;
                break;
            }
        }
        Log("MSAA %dx", (int)s_msType);
    }

    s_dev->CreateStateBlock(D3DSBT_ALL, &s_saveSB);
    CreateSwapChain();
    Log("Direct3D 9Ex device ready");
    return true;
}

void Shutdown()
{
    SafeRelease(s_scratch);
    SafeRelease(s_saveSB);
    SafeRelease(s_swap);
    SafeRelease(s_dev);
    SafeRelease(s_d3d);
    if (s_dummy) { DestroyWindow(s_dummy); s_dummy = NULL; }
}

// ---------------------------------------------------------------------------
// resources
// ---------------------------------------------------------------------------
Texture* CreateTexture(int w, int h, int levels)
{
    if (!s_dev) return NULL;
    Texture* t = new Texture();
    t->w = w; t->h = h; t->levels = levels;
    HRESULT hr = s_dev->CreateTexture(w, h, levels, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t->tex, NULL);
    if (FAILED(hr)) {
        Log("CreateTexture %dx%d/%d failed 0x%08X", w, h, levels, hr);
        delete t;
        return NULL;
    }
    return t;
}

void DestroyTexture(Texture* t)
{
    if (!t) return;
    SafeRelease(t->tex);
    delete t;
}

void UploadTexture(Texture* t, int level, const uint32_t* argb, int w, int h)
{
    if (!t || !t->tex || level >= t->levels) return;
    D3DLOCKED_RECT lr;
    if (FAILED(t->tex->LockRect(level, &lr, NULL, D3DLOCK_DISCARD))) return;
    D3DSURFACE_DESC sd;
    t->tex->GetLevelDesc(level, &sd);
    int cw = (int)sd.Width < w ? (int)sd.Width : w;
    int ch = (int)sd.Height < h ? (int)sd.Height : h;
    for (int y = 0; y < ch; y++)
        memcpy((BYTE*)lr.pBits + y * lr.Pitch, argb + y * w, cw * 4);
    t->tex->UnlockRect(level);
}

Target* CreateTarget(int w, int h)
{
    if (!s_dev || w <= 0 || h <= 0) return NULL;
    Target* t = new Target();
    t->gw = w; t->gh = h;
    t->sw = w * s_scale; t->sh = h * s_scale;
    HRESULT hr = s_dev->CreateTexture(t->sw, t->sh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &t->tex, NULL);
    if (FAILED(hr)) { Log("CreateTarget %dx%d failed 0x%08X", t->sw, t->sh, hr); delete t; return NULL; }
    t->tex->GetSurfaceLevel(0, &t->texSurf);
    if (s_msType != D3DMULTISAMPLE_NONE) {
        if (FAILED(s_dev->CreateRenderTarget(t->sw, t->sh, D3DFMT_X8R8G8B8, s_msType, s_msQuality, FALSE, &t->msaa, NULL)))
            t->msaa = NULL;
    }
    s_dev->ColorFill(RTSurf(t), NULL, D3DCOLOR_XRGB(0, 0, 0));
    if (t->msaa) s_dev->ColorFill(t->texSurf, NULL, D3DCOLOR_XRGB(0, 0, 0));
    LogV("target %dx%d -> %dx%d%s", w, h, t->sw, t->sh, t->msaa ? " msaa" : "");
    return t;
}

void DestroyTarget(Target* t)
{
    if (!t) return;
    if (s_curRT == t) s_curRT = NULL;
    SafeRelease(t->sysmem);
    SafeRelease(t->native);
    SafeRelease(t->msaa);
    SafeRelease(t->texSurf);
    SafeRelease(t->tex);
    delete t;
}

Depth* CreateDepth(int w, int h)
{
    if (!s_dev || w <= 0 || h <= 0) return NULL;
    Depth* d = new Depth();
    d->gw = w; d->gh = h;
    d->sw = w * s_scale; d->sh = h * s_scale;
    HRESULT hr = s_dev->CreateDepthStencilSurface(d->sw, d->sh, D3DFMT_D24S8, s_msType, s_msQuality, FALSE, &d->surf, NULL);
    if (FAILED(hr)) { Log("CreateDepth failed 0x%08X", hr); delete d; return NULL; }
    return d;
}

void DestroyDepth(Depth* d)
{
    if (!d) return;
    if (s_curDS == d) { s_curDS = NULL; if (s_dev) s_dev->SetDepthStencilSurface(NULL); }
    SafeRelease(d->surf);
    delete d;
}

bool ReadTarget(Target* t, uint32_t* out)
{
    if (!t || !s_dev) return false;
    Resolve(t);
    if (!t->sysmem &&
        FAILED(s_dev->CreateOffscreenPlainSurface(t->gw, t->gh, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &t->sysmem, NULL)))
        return false;
    IDirect3DSurface9* src = t->texSurf;
    if (t->sw != t->gw || t->sh != t->gh) {
        if (!t->native &&
            FAILED(s_dev->CreateRenderTarget(t->gw, t->gh, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &t->native, NULL)))
            return false;
        s_dev->StretchRect(t->texSurf, NULL, t->native, NULL, D3DTEXF_LINEAR);
        src = t->native;
    }
    if (FAILED(s_dev->GetRenderTargetData(src, t->sysmem))) return false;
    D3DLOCKED_RECT lr;
    if (FAILED(t->sysmem->LockRect(&lr, NULL, D3DLOCK_READONLY))) return false;
    for (int y = 0; y < t->gh; y++) {
        const uint32_t* s = (const uint32_t*)((BYTE*)lr.pBits + y * lr.Pitch);
        uint32_t* d = out + y * t->gw;
        for (int x = 0; x < t->gw; x++) d[x] = s[x] | 0xFF000000;
    }
    t->sysmem->UnlockRect();
    return true;
}

// Draws a screen-space textured quad (pixel coordinates) onto a surface using a
// private state set; the game's state is restored afterwards.  alpha < 255
// blends the quad using its texture alpha scaled by alpha.
static void DrawQuadOn(IDirect3DSurface9* rt, IDirect3DBaseTexture9* tex, float x0, float y0, float x1, float y1,
                       float u1, float v1, bool keyed, bool linear, bool blend, BYTE alpha, bool needScene)
{
    IDirect3DSurface9* oldRT = NULL; IDirect3DSurface9* oldDS = NULL;
    s_dev->GetRenderTarget(0, &oldRT);
    s_dev->GetDepthStencilSurface(&oldDS);
    if (s_saveSB) s_saveSB->Capture();

    s_dev->SetRenderTarget(0, rt);
    s_dev->SetDepthStencilSurface(NULL);
    s_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    s_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, blend);
    s_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    s_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    s_dev->SetRenderState(D3DRS_TEXTUREFACTOR, (DWORD)alpha << 24);
    s_dev->SetRenderState(D3DRS_ALPHATESTENABLE, keyed);
    s_dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
    s_dev->SetRenderState(D3DRS_ALPHAREF, 0);
    s_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    s_dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    s_dev->SetTexture(0, tex);
    s_dev->SetTexture(1, NULL);
    s_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    s_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    s_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, blend ? D3DTOP_MODULATE : D3DTOP_SELECTARG1);
    s_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    s_dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
    s_dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    s_dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    s_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    s_dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    s_dev->SetSamplerState(0, D3DSAMP_MINFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    s_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    s_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    s_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    s_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    s_dev->SetVertexShader(NULL);
    s_dev->SetPixelShader(NULL);
    s_dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);

    x0 -= 0.5f; y0 -= 0.5f; x1 -= 0.5f; y1 -= 0.5f;
    struct V { float x, y, z, w, u, v; } q[4] = {
        { x0, y0, 0, 1, 0,  0  }, { x1, y0, 0, 1, u1, 0  },
        { x0, y1, 0, 1, 0,  v1 }, { x1, y1, 0, 1, u1, v1 },
    };
    if (needScene) s_dev->BeginScene();
    s_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    if (needScene) s_dev->EndScene();

    s_dev->SetRenderTarget(0, oldRT);
    s_dev->SetDepthStencilSurface(oldDS);
    if (s_saveSB) s_saveSB->Apply();   // also restores the game's viewport
    SafeRelease(oldRT);
    SafeRelease(oldDS);
}

static void DrawQuadInto(Target* t, IDirect3DBaseTexture9* tex, float u1, float v1, const RECT& dst, bool keyed, bool linear)
{
    DrawQuadOn(RTSurf(t), tex,
               dst.left * (float)t->sw / t->gw, dst.top * (float)t->sh / t->gh,
               dst.right * (float)t->sw / t->gw, dst.bottom * (float)t->sh / t->gh,
               u1, v1, keyed, linear, false, 255, !s_inScene);
    if (t->msaa) t->dirty = true;
}

void WriteTarget(Target* t, const uint32_t* argb, int w, int h, const RECT& dst, bool keyed)
{
    if (!t || !s_dev || w <= 0 || h <= 0) return;
    if (!s_scratch || s_scratchW < w || s_scratchH < h) {
        SafeRelease(s_scratch);
        s_scratchW = w > s_scratchW ? w : s_scratchW;
        s_scratchH = h > s_scratchH ? h : s_scratchH;
        if (FAILED(s_dev->CreateTexture(s_scratchW, s_scratchH, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s_scratch, NULL))) {
            s_scratch = NULL;
            return;
        }
    }
    D3DLOCKED_RECT lr;
    if (FAILED(s_scratch->LockRect(0, &lr, NULL, D3DLOCK_DISCARD))) return;
    for (int y = 0; y < h; y++) memcpy((BYTE*)lr.pBits + y * lr.Pitch, argb + y * w, w * 4);
    s_scratch->UnlockRect(0);

    // 1:1 copies use point sampling so movie frames / 2D stay crisp per pixel
    bool linear = (dst.right - dst.left) != w || (dst.bottom - dst.top) != h;
    DrawQuadInto(t, s_scratch, (float)w / s_scratchW, (float)h / s_scratchH, dst, keyed, linear);
}

void FillTarget(Target* t, const RECT* r, uint32_t argb)
{
    if (!t || !s_dev) return;
    RECT sr;
    if (r) sr = ScaleRect(t, *r);
    s_dev->ColorFill(RTSurf(t), r ? &sr : NULL, argb);
    if (t->msaa) t->dirty = true;
}

void CopyTarget(Target* dst, const RECT* dr, Target* src, const RECT* sr)
{
    if (!dst || !src || !s_dev) return;
    Resolve(src);
    RECT d, s;
    if (dr) d = ScaleRect(dst, *dr);
    if (sr) s = ScaleRect(src, *sr);
    if (dst == src) {
        LogOnce("Blt within the same render target is not supported");
        return;
    }
    s_dev->StretchRect(src->texSurf, sr ? &s : NULL, RTSurf(dst), dr ? &d : NULL, D3DTEXF_LINEAR);
    if (dst->msaa) dst->dirty = true;
}

void ClearDepth(Depth* d, float z)
{
    if (!d || !s_dev) return;
    IDirect3DSurface9* oldDS = NULL;
    s_dev->GetDepthStencilSurface(&oldDS);
    D3DVIEWPORT9 vp;
    s_dev->GetViewport(&vp);
    s_dev->SetDepthStencilSurface(d->surf);
    D3DVIEWPORT9 full = { 0, 0, (DWORD)d->sw, (DWORD)d->sh, 0.0f, 1.0f };
    if (s_curRT) { full.Width = s_curRT->sw < d->sw ? s_curRT->sw : d->sw; full.Height = s_curRT->sh < d->sh ? s_curRT->sh : d->sh; }
    s_dev->SetViewport(&full);
    s_dev->Clear(0, NULL, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, z, 0);
    s_dev->SetDepthStencilSurface(oldDS);
    s_dev->SetViewport(&vp);
    SafeRelease(oldDS);
}

// ---------------------------------------------------------------------------
// presentation
// ---------------------------------------------------------------------------
bool GetViewMapping(RECT* dst, int* gw, int* gh)
{
    if (!s_haveMapping) return false;
    *dst = s_lastDst;
    *gw = s_lastGW;
    *gh = s_lastGH;
    return true;
}

static void DumpFrame(Target* t)
{
    static int frame = 0, saved = 0;
    if (g_cfg.dumpEvery <= 0 || (++frame % g_cfg.dumpEvery) != 0 || saved >= 40) return;
    // full internal resolution, so upscaling quality can be inspected
    Resolve(t);
    IDirect3DSurface9* sys = NULL;
    if (FAILED(s_dev->CreateOffscreenPlainSurface(t->sw, t->sh, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL))) return;
    std::vector<uint32_t> px((size_t)t->sw * t->sh);
    D3DLOCKED_RECT lr;
    if (FAILED(s_dev->GetRenderTargetData(t->texSurf, sys)) || FAILED(sys->LockRect(&lr, NULL, D3DLOCK_READONLY))) {
        sys->Release();
        return;
    }
    for (int y = 0; y < t->sh; y++) memcpy(&px[(size_t)y * t->sw], (BYTE*)lr.pBits + y * lr.Pitch, t->sw * 4);
    sys->UnlockRect();
    sys->Release();
    char path[MAX_PATH];
    wsprintfA(path, "%s\\hod2fix_frame%03d.bmp", GameDir(), saved++);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    ih.biSize = sizeof(ih); ih.biWidth = t->sw; ih.biHeight = -t->sh; ih.biPlanes = 1; ih.biBitCount = 32;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + (DWORD)px.size() * 4;
    DWORD w;
    WriteFile(f, &fh, sizeof(fh), &w, NULL);
    WriteFile(f, &ih, sizeof(ih), &w, NULL);
    WriteFile(f, px.data(), (DWORD)px.size() * 4, &w, NULL);
    CloseHandle(f);
    Log("dumped frame %d to %s", frame, path);
}



// ---------------------------------------------------------------------------
// Screenshots (PNG via WIC on a worker thread) and the on-screen notice
// ---------------------------------------------------------------------------
struct Shot {
    std::vector<BYTE> bgr;   // 24-bit rows, top-down
    int w, h;
    WCHAR path[MAX_PATH];
};

static DWORD WINAPI SaveShotThread(LPVOID param)
{
    Shot* shot = (Shot*)param;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool comInit = SUCCEEDED(hr);
    IWICImagingFactory* f = NULL;
    IWICStream* st = NULL;
    IWICBitmapEncoder* enc = NULL;
    IWICBitmapFrameEncode* fr = NULL;
    IPropertyBag2* props = NULL;
    hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (SUCCEEDED(hr)) hr = f->CreateStream(&st);
    if (SUCCEEDED(hr)) hr = st->InitializeFromFilename(shot->path, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = f->CreateEncoder(GUID_ContainerFormatPng, NULL, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&fr, &props);
    if (SUCCEEDED(hr)) hr = fr->Initialize(props);
    if (SUCCEEDED(hr)) hr = fr->SetSize(shot->w, shot->h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = fr->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr) && fmt != GUID_WICPixelFormat24bppBGR) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = fr->WritePixels(shot->h, shot->w * 3, (UINT)shot->bgr.size(), shot->bgr.data());
    if (SUCCEEDED(hr)) hr = fr->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    if (FAILED(hr)) Log("screenshot save failed: 0x%08X", hr);
    SafeRelease(props);
    SafeRelease(fr);
    SafeRelease(enc);
    SafeRelease(st);
    SafeRelease(f);
    if (comInit) CoUninitialize();
    delete shot;
    return 0;
}

bool SavePNG(const char* path, const uint32_t* argb, int w, int h, bool alpha)
{
    HRESULT init = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    WCHAR wpath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
    IWICImagingFactory* f = NULL;
    IWICStream* st = NULL;
    IWICBitmapEncoder* enc = NULL;
    IWICBitmapFrameEncode* fr = NULL;
    IPropertyBag2* props = NULL;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (SUCCEEDED(hr)) hr = f->CreateStream(&st);
    if (SUCCEEDED(hr)) hr = st->InitializeFromFilename(wpath, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = f->CreateEncoder(GUID_ContainerFormatPng, NULL, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&fr, &props);
    if (SUCCEEDED(hr)) hr = fr->Initialize(props);
    if (SUCCEEDED(hr)) hr = fr->SetSize(w, h);
    WICPixelFormatGUID want = alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat32bppBGR;
    WICPixelFormatGUID fmt = want;
    if (SUCCEEDED(hr)) hr = fr->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr) && fmt != want) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = fr->WritePixels(h, w * 4, (UINT)(w * h * 4), (BYTE*)argb);
    if (SUCCEEDED(hr)) hr = fr->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    SafeRelease(props);
    SafeRelease(fr);
    SafeRelease(enc);
    SafeRelease(st);
    SafeRelease(f);
    if (SUCCEEDED(init)) CoUninitialize();
    return SUCCEEDED(hr);
}

static IDirect3DTexture9* s_toastTex;
static int      s_toastW, s_toastH;
static LONGLONG s_toastStart;
static const double kToastSeconds = 2.5;

// Renders the notice text with GDI into a texture.
static void ShowToast(const char* text)
{
    SafeRelease(s_toastTex);
    int fontH = s_clientH / 36;
    if (fontH < 16) fontH = 16;
    HDC dc = CreateCompatibleDC(NULL);
    HFONT font = CreateFontA(-fontH, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Segoe UI");
    HFONT oldFont = (HFONT)SelectObject(dc, font);
    RECT tr = { 0, 0, 0, 0 };
    DrawTextA(dc, text, -1, &tr, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    int pad = fontH / 2;
    int w = tr.right + pad * 2, h = tr.bottom + pad * 2;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    uint32_t* bits = NULL;
    HBITMAP bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
    if (bmp && bits) {
        HBITMAP oldBmp = (HBITMAP)SelectObject(dc, bmp);
        for (int i = 0; i < w * h; i++) bits[i] = 0x00181818;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT r = { pad, pad, w - pad, h - pad };
        DrawTextA(dc, text, -1, &r, DT_SINGLELINE | DT_NOPREFIX);
        GdiFlush();
        for (int i = 0; i < w * h; i++) bits[i] = (bits[i] & 0xFFFFFF) | 0xE0000000;
        if (SUCCEEDED(s_dev->CreateTexture(w, h, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s_toastTex, NULL))) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(s_toastTex->LockRect(0, &lr, NULL, D3DLOCK_DISCARD))) {
                for (int y = 0; y < h; y++) memcpy((BYTE*)lr.pBits + y * lr.Pitch, bits + y * w, w * 4);
                s_toastTex->UnlockRect(0);
            }
        }
        SelectObject(dc, oldBmp);
    }
    if (bmp) DeleteObject(bmp);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteDC(dc);
    s_toastW = w;
    s_toastH = h;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    s_toastStart = now.QuadPart;
}

static void DrawToast(IDirect3DSurface9* bb)
{
    if (!s_toastTex) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double t = (double)(now.QuadPart - s_toastStart) / s_qpf.QuadPart;
    if (t >= kToastSeconds) { SafeRelease(s_toastTex); return; }
    double fade = kToastSeconds - t < 0.4 ? (kToastSeconds - t) / 0.4 : 1.0;
    float m = (float)(s_clientH / 40);
    DrawQuadOn(bb, s_toastTex, m, m, m + s_toastW, m + s_toastH, 1.0f, 1.0f,
               false, false, true, (BYTE)(255 * fade), true);
}

static void TakeScreenshot(Target* t)
{
    Resolve(t);
    IDirect3DSurface9* sys = NULL;
    if (FAILED(s_dev->CreateOffscreenPlainSurface(t->sw, t->sh, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL))) return;
    D3DLOCKED_RECT lr;
    if (FAILED(s_dev->GetRenderTargetData(t->texSurf, sys)) || FAILED(sys->LockRect(&lr, NULL, D3DLOCK_READONLY))) {
        sys->Release();
        ShowToast("Screenshot failed");
        return;
    }
    Shot* shot = new Shot();
    shot->w = t->sw;
    shot->h = t->sh;
    shot->bgr.resize((size_t)t->sw * t->sh * 3);
    for (int y = 0; y < t->sh; y++) {
        const BYTE* src = (const BYTE*)lr.pBits + y * lr.Pitch;
        BYTE* dst = shot->bgr.data() + (size_t)y * t->sw * 3;
        for (int x = 0; x < t->sw; x++) {
            dst[x * 3] = src[x * 4];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 2];
        }
    }
    sys->UnlockRect();
    sys->Release();

    char dir[MAX_PATH], name[64], path[MAX_PATH];
    wsprintfA(dir, "%s\\screenshots", GameDir());
    CreateDirectoryA(dir, NULL);
    SYSTEMTIME tm;
    GetLocalTime(&tm);
    wsprintfA(name, "hod2_%04d%02d%02d_%02d%02d%02d_%03d.png",
              tm.wYear, tm.wMonth, tm.wDay, tm.wHour, tm.wMinute, tm.wSecond, tm.wMilliseconds);
    wsprintfA(path, "%s\\%s", dir, name);
    MultiByteToWideChar(CP_ACP, 0, path, -1, shot->path, MAX_PATH);
    HANDLE th = CreateThread(NULL, 0, SaveShotThread, shot, 0, NULL);
    if (th) CloseHandle(th); else delete shot;
    Log("screenshot %dx%d -> %s", t->sw, t->sh, path);

    if (g_cfg.screenshotNotify) {
        char msg[128];
        wsprintfA(msg, "Screenshot saved:  screenshots\\%s", name);
        ShowToast(msg);
    }
}

// Hotkeys arrive as window messages (see main.cpp) and are handled at the
// next present, on the render thread.
static volatile LONG s_wantShot, s_wantHaloToggle, s_wantSmoothToggle, s_wantProbe;
static DWORD s_lastHotkey[5];

// Both the DirectInput watcher and the key-state poll below may report the
// same press, so each hotkey fires at most once per 300 ms.
void Hotkey(int which)
{
    if (which < 1 || which > 4) return;
    DWORD now = GetTickCount();
    if (now - s_lastHotkey[which] < 300) return;
    s_lastHotkey[which] = now;
    if (which == HOTKEY_SCREENSHOT) InterlockedExchange(&s_wantShot, 1);
    if (which == HOTKEY_HALO) InterlockedExchange(&s_wantHaloToggle, 1);
    if (which == HOTKEY_SMOOTH) InterlockedExchange(&s_wantSmoothToggle, 1);
    if (which == HOTKEY_PROBE) InterlockedExchange(&s_wantProbe, 1);
}

static void PollKey(int vk, int which, bool& wasDown)
{
    if (!vk) return;
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    if (down && !wasDown && GetForegroundWindow() == s_hwnd) Hotkey(which);
    wasDown = down;
}

static void PollHotkeys(Target* t)
{
    static bool shotDown, haloDown, smoothDown;
    PollKey(g_cfg.screenshotKey, HOTKEY_SCREENSHOT, shotDown);
    PollKey(g_cfg.haloToggleKey, HOTKEY_HALO, haloDown);
    PollKey(g_cfg.smoothToggleKey, HOTKEY_SMOOTH, smoothDown);
    static bool probeDown;
    PollKey(g_cfg.probeKey, HOTKEY_PROBE, probeDown);
    // a probe covers exactly the frame between two presents
    if (g_probe == 2) {
        g_probe = 0;
        Log("probe end: %ld draws cover (%.1f, %.1f)", g_probeHits, g_probeX, g_probeY);
        char msg[96];
        wsprintfA(msg, "Probe: %ld draws under the cursor (see hod2fix.log)", g_probeHits);
        ShowToast(msg);
    }
    if (InterlockedExchange(&s_wantProbe, 0) && Win_CursorGamePos(&g_probeX, &g_probeY)) {
        g_probeHits = 0;
        Log("probe start at game pixel (%.1f, %.1f)", g_probeX, g_probeY);
        g_probe = 2;
    }
#ifdef HOD2FIX_HOTKEY_TEST
    static int frames = 0;
    frames++;
    if (frames == 400 || frames == 520) Hotkey(HOTKEY_HALO);
    if (frames == 640) Hotkey(HOTKEY_SCREENSHOT);
    if (frames == 450) Hotkey(HOTKEY_PROBE);
#endif
    if (InterlockedExchange(&s_wantShot, 0)) TakeScreenshot(t);
    if (InterlockedExchange(&s_wantSmoothToggle, 0)) {
        g_cfg.forceLinear = !g_cfg.forceLinear;
        Log("smooth textures %s", g_cfg.forceLinear ? "on" : "off");
        ShowToast(g_cfg.forceLinear ? "Smooth textures: ON" : "Smooth textures: OFF (original)");
    }
    if (InterlockedExchange(&s_wantHaloToggle, 0)) {
        g_cfg.fixHalos = !g_cfg.fixHalos;
        Log("alpha halo fix %s", g_cfg.fixHalos ? "on" : "off");
        ShowToast(g_cfg.fixHalos ? "Alpha halo fix: ON" : "Alpha halo fix: OFF (original)");
    }
}

void Present(Target* t, const RECT* srcGame)
{
    if (!s_dev || !t) return;
    DumpFrame(t);
    static int presents = 0;
    if (g_cfg.logLevel >= 2 && (++presents % 1800) == 0) {
        PROCESS_MEMORY_COUNTERS_EX pm = { sizeof(pm) };
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm));
        Log("frame %d: live surfaces %ld, private %u MB", presents, g_liveSurfaces, (unsigned)(pm.PrivateUsage >> 20));
    }
    if (s_inScene) { s_dev->EndScene(); }

    RECT rc;
    GetClientRect(s_hwnd, &rc);
    if (rc.right != s_clientW || rc.bottom != s_clientH || !s_swap)
        CreateSwapChain();

    if (s_swap) {
        Resolve(t);
        RECT src = { 0, 0, t->gw, t->gh };
        if (srcGame) {
            src = *srcGame;
            if (src.right > t->gw) src.right = t->gw;
            if (src.bottom > t->gh) src.bottom = t->gh;
        }
        RECT ss = ScaleRect(t, src);
        int srcW = src.right - src.left, srcH = src.bottom - src.top;
        if (srcW <= 0 || srcH <= 0) { srcW = t->gw; srcH = t->gh; ss.left = ss.top = 0; ss.right = t->sw; ss.bottom = t->sh; }

        RECT dst = { 0, 0, s_clientW, s_clientH };
        if (g_cfg.keepAspect) {
            // fit preserving aspect ratio
            if ((LONGLONG)s_clientW * srcH > (LONGLONG)s_clientH * srcW) {
                int w = (int)((LONGLONG)s_clientH * srcW / srcH);
                dst.left = (s_clientW - w) / 2; dst.right = dst.left + w;
            } else {
                int h = (int)((LONGLONG)s_clientW * srcH / srcW);
                dst.top = (s_clientH - h) / 2; dst.bottom = dst.top + h;
            }
        }
        s_lastDst = dst; s_lastGW = srcW; s_lastGH = srcH; s_haveMapping = true;

        PollHotkeys(t);
        IDirect3DSurface9* bb = NULL;
        if (SUCCEEDED(s_swap->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
            s_dev->ColorFill(bb, NULL, D3DCOLOR_XRGB(0, 0, 0));
            s_dev->StretchRect(t->texSurf, &ss, bb, &dst, g_cfg.linearScale ? D3DTEXF_LINEAR : D3DTEXF_POINT);
            DrawToast(bb);
            bb->Release();
        }
    }

    if (!s_waited) Pace();
    s_waited = false;

    if (s_swap) {
        HRESULT hr = s_swap->Present(NULL, NULL, NULL, NULL, 0);
        if (hr == D3DERR_DEVICEREMOVED || hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICEHUNG) {
            LogOnce("device lost/removed during Present");
        }
    }
    if (s_inScene) s_dev->BeginScene();
}

// ---------------------------------------------------------------------------
// 3D device
// ---------------------------------------------------------------------------
void SetRenderTarget(Target* t, Depth* d)
{
    if (!s_dev) return;
    s_curRT = t;
    s_curDS = d;
    if (t) s_dev->SetRenderTarget(0, RTSurf(t));
    s_dev->SetDepthStencilSurface(d ? d->surf : NULL);
}

void BeginScene()
{
    if (!s_dev || s_inScene) return;
    s_dev->BeginScene();
    s_inScene = true;
}

void EndScene()
{
    if (!s_dev || !s_inScene) return;
    s_dev->EndScene();
    s_inScene = false;
}

void Clear(DWORD count, const RECT* rects, DWORD flags, DWORD color, float z, DWORD stencil)
{
    if (!s_dev || !s_curRT) return;
    if (!s_curDS) flags &= ~(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
    if (!flags) return;
    if (count && rects) {
        std::vector<D3DRECT> sr(count);
        for (DWORD i = 0; i < count; i++) {
            RECT r = ScaleRect(s_curRT, rects[i]);
            sr[i].x1 = r.left; sr[i].y1 = r.top; sr[i].x2 = r.right; sr[i].y2 = r.bottom;
        }
        s_dev->Clear(count, sr.data(), flags, color, z, stencil);
    } else {
        s_dev->Clear(0, NULL, flags, color, z, stencil);
    }
    if (s_curRT->msaa) s_curRT->dirty = true;
}

void SetRS(DWORD state, DWORD value) { if (s_dev) s_dev->SetRenderState((D3DRENDERSTATETYPE)state, value); }
void SetTSS(DWORD stage, DWORD type, DWORD value) { if (s_dev) s_dev->SetTextureStageState(stage, (D3DTEXTURESTAGESTATETYPE)type, value); }

void SetSampler(DWORD stage, DWORD type, DWORD value)
{
    if (!s_dev) return;
    if (g_cfg.anisotropy > 1 && type == D3DSAMP_MINFILTER && value == D3DTEXF_LINEAR) {
        DWORD a = (DWORD)g_cfg.anisotropy < s_maxAniso ? (DWORD)g_cfg.anisotropy : s_maxAniso;
        s_dev->SetSamplerState(stage, D3DSAMP_MAXANISOTROPY, a);
        value = D3DTEXF_ANISOTROPIC;
    }
    s_dev->SetSamplerState(stage, (D3DSAMPLERSTATETYPE)type, value);
}

void SetTexture(DWORD stage, Texture* t) { if (s_dev) s_dev->SetTexture(stage, t ? t->tex : NULL); }

void SetTextureTarget(DWORD stage, Target* t)
{
    if (!s_dev) return;
    if (t == s_curRT) { s_dev->SetTexture(stage, NULL); return; }
    Resolve(t);
    s_dev->SetTexture(stage, t ? t->tex : NULL);
}

void SetTransform(DWORD type, const float* m16) { if (s_dev) s_dev->SetTransform((D3DTRANSFORMSTATETYPE)type, (const D3DMATRIX*)m16); }

void SetViewport(DWORD x, DWORD y, DWORD w, DWORD h, float minZ, float maxZ)
{
    if (!s_dev || !s_curRT) return;
    D3DVIEWPORT9 vp;
    vp.X = x * s_curRT->sw / s_curRT->gw;
    vp.Y = y * s_curRT->sh / s_curRT->gh;
    vp.Width = w * s_curRT->sw / s_curRT->gw;
    vp.Height = h * s_curRT->sh / s_curRT->gh;
    if (vp.X > (DWORD)s_curRT->sw) vp.X = s_curRT->sw;
    if (vp.Y > (DWORD)s_curRT->sh) vp.Y = s_curRT->sh;
    if (vp.X + vp.Width > (DWORD)s_curRT->sw) vp.Width = s_curRT->sw - vp.X;
    if (vp.Y + vp.Height > (DWORD)s_curRT->sh) vp.Height = s_curRT->sh - vp.Y;
    vp.MinZ = minZ;
    vp.MaxZ = maxZ;
    s_dev->SetViewport(&vp);
}

void SetMaterial(const void* mat) { if (s_dev) s_dev->SetMaterial((const D3DMATERIAL9*)mat); }
void SetLight(DWORD idx, const void* light) { if (s_dev) s_dev->SetLight(idx, (const D3DLIGHT9*)light); }
void LightEnable(DWORD idx, BOOL on) { if (s_dev) s_dev->LightEnable(idx, on); }
void SetClipPlane(DWORD idx, const float* plane) { if (s_dev) s_dev->SetClipPlane(idx, plane); }

UINT FVFStride(DWORD fvf)
{
    UINT s = 0;
    switch (fvf & D3DFVF_POSITION_MASK) {
    case D3DFVF_XYZ:    s = 12; break;
    case D3DFVF_XYZRHW: s = 16; break;
    case D3DFVF_XYZB1:  s = 16; break;
    case D3DFVF_XYZB2:  s = 20; break;
    case D3DFVF_XYZB3:  s = 24; break;
    case D3DFVF_XYZB4:  s = 28; break;
    case D3DFVF_XYZB5:  s = 32; break;
    }
    if (fvf & D3DFVF_NORMAL)   s += 12;
    if (fvf & 0x20)            s += 4;   // D3DFVF_RESERVED1 (D3D7) / PSIZE (D3D9)
    if (fvf & D3DFVF_DIFFUSE)  s += 4;
    if (fvf & D3DFVF_SPECULAR) s += 4;
    UINT ntex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (UINT i = 0; i < ntex; i++) {
        switch ((fvf >> (16 + i * 2)) & 3) {
        case 0: s += 8; break;
        case 1: s += 12; break;
        case 2: s += 16; break;
        case 3: s += 4; break;
        }
    }
    return s;
}

static UINT PrimCount(DWORD prim, DWORD n)
{
    switch (prim) {
    case D3DPT_POINTLIST:     return n;
    case D3DPT_LINELIST:      return n / 2;
    case D3DPT_LINESTRIP:     return n > 1 ? n - 1 : 0;
    case D3DPT_TRIANGLELIST:  return n / 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:   return n > 2 ? n - 2 : 0;
    }
    return 0;
}

void Draw(DWORD prim, DWORD fvf, const void* verts, DWORD vcount, const WORD* idx, DWORD icount)
{
    if (!s_dev || !s_curRT || !verts || !vcount) return;
    UINT pc = PrimCount(prim, idx ? icount : vcount);
    if (!pc) return;
    UINT stride = FVFStride(fvf);
    if (!stride) return;
    if (g_cfg.logLevel >= 2) {
        char b[64];
        wsprintfA(b, "Draw FVF %08X prim %u%s", fvf, prim, idx ? " indexed" : "");
        LogOnce(b);
    }

    const void* vp = verts;
    if ((fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW && (s_curRT->sw != s_curRT->gw || s_curRT->sh != s_curRT->gh)) {
        // Scale pre-transformed positions, keeping pixel centres aligned.
        size_t bytes = (size_t)stride * vcount;
        if (s_vtmp.size() < bytes) s_vtmp.resize(bytes);
        memcpy(s_vtmp.data(), verts, bytes);
        float sx = (float)s_curRT->sw / s_curRT->gw;
        float sy = (float)s_curRT->sh / s_curRT->gh;
        BYTE* p = s_vtmp.data();
        for (DWORD i = 0; i < vcount; i++, p += stride) {
            float* f = (float*)p;
            f[0] = (f[0] + 0.5f) * sx - 0.5f;
            f[1] = (f[1] + 0.5f) * sy - 0.5f;
        }
        vp = s_vtmp.data();
    }

    bool began = !s_inScene;
    if (began) s_dev->BeginScene();
    s_dev->SetFVF(fvf);
    if (idx)
        s_dev->DrawIndexedPrimitiveUP((D3DPRIMITIVETYPE)prim, 0, vcount, pc, idx, D3DFMT_INDEX16, vp, stride);
    else
        s_dev->DrawPrimitiveUP((D3DPRIMITIVETYPE)prim, pc, vp, stride);
    if (began) s_dev->EndScene();
    if (s_curRT->msaa) s_curRT->dirty = true;
}

} // namespace R
