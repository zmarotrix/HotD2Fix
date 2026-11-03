// hod2fix - DirectDraw 7 / Direct3D 7 front-end objects.
#pragma once
#include "common.h"
#include <ddraw.h>
#include <d3d.h>
#include <vector>
#include "renderer.h"

struct DDraw7;
struct Surface7;
struct D3D7;
struct Device7;

// ---------------------------------------------------------------------------
// Pixel format conversion
// ---------------------------------------------------------------------------
struct PixConv {
    DWORD bpp;
    bool  pal8;
    DWORD mask[4];      // r g b a
    int   shift[4];
    int   bits[4];
    void  init(const DDPIXELFORMAT& pf);
    uint32_t toARGB(uint32_t v) const;
    uint32_t fromARGB(uint32_t c) const;
};

DDPIXELFORMAT MakePF(DWORD bpp, DWORD r, DWORD g, DWORD b, DWORD a);
DDPIXELFORMAT PF_X8R8G8B8();
DDPIXELFORMAT PF_R5G6B5();

// ---------------------------------------------------------------------------
struct Clipper7 : IDirectDrawClipper {
    LONG ref = 1;
    HWND hwnd = NULL;
    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv);
    STDMETHOD_(ULONG, AddRef)();
    STDMETHOD_(ULONG, Release)();
    STDMETHOD(GetClipList)(LPRECT, LPRGNDATA, LPDWORD);
    STDMETHOD(GetHWnd)(HWND*);
    STDMETHOD(Initialize)(LPDIRECTDRAW, DWORD);
    STDMETHOD(IsClipListChanged)(BOOL*);
    STDMETHOD(SetClipList)(LPRGNDATA, DWORD);
    STDMETHOD(SetHWnd)(DWORD, HWND);
};

struct Palette7 : IDirectDrawPalette {
    LONG ref = 1;
    DWORD caps = 0;
    PALETTEENTRY e[256] = {};
    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv);
    STDMETHOD_(ULONG, AddRef)();
    STDMETHOD_(ULONG, Release)();
    STDMETHOD(GetCaps)(LPDWORD);
    STDMETHOD(GetEntries)(DWORD, DWORD, DWORD, LPPALETTEENTRY);
    STDMETHOD(Initialize)(LPDIRECTDRAW, DWORD, LPPALETTEENTRY);
    STDMETHOD(SetEntries)(DWORD, DWORD, DWORD, LPPALETTEENTRY);
};

struct Gamma7 : IDirectDrawGammaControl {
    Surface7* owner;
    DDGAMMARAMP ramp;
    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv);
    STDMETHOD_(ULONG, AddRef)();
    STDMETHOD_(ULONG, Release)();
    STDMETHOD(GetGammaRamp)(DWORD, LPDDGAMMARAMP);
    STDMETHOD(SetGammaRamp)(DWORD, LPDDGAMMARAMP);
};

// ---------------------------------------------------------------------------
enum SurfKind { SK_PLAIN, SK_PRIMARY, SK_TARGET, SK_DEPTH, SK_TEXTURE };

struct Surface7 : IDirectDrawSurface7 {
    LONG       ref = 1;
    DDraw7*    dd = NULL;
    DDSURFACEDESC2 desc = {};
    SurfKind   kind = SK_PLAIN;
    bool       isTexture = false;      // can be bound with SetTexture
    PixConv    conv;

    std::vector<BYTE> mem;             // CPU copy (game pixel format)
    LONG       pitch = 0;

    R::Target*  rt = NULL;
    R::Depth*   ds = NULL;
    R::Texture* tex = NULL;            // only on the root of a texture chain
    R::Texture* texPadded = NULL;      // copy with transparent texels colour-padded (root only)
    uint32_t   hash = 0;               // FNV-1a of level 0 (ARGB), for diagnostics
    bool       softAlpha = false;      // level 0 has gradual transparency (smoke, fire), not just cut-outs

    Surface7*  root = this;            // owner of an implicit chain
    std::vector<Surface7*> chain;      // root: implicitly created surfaces
    Surface7*  next = NULL;            // next back buffer / next mip level
    int        level = 0;              // mip level
    std::vector<Surface7*> attached;   // explicitly attached (AddRef'd)

    bool       dirty = true;           // texture needs upload
    bool       hasCK = false;
    DDCOLORKEY ck = {};
    Clipper7*  clipper = NULL;
    Palette7*  palette = NULL;
    Gamma7     gamma;
    DWORD      uniq = 1;
    DWORD      priority = 0, lod = 0;

    HDC        dc = NULL;
    HBITMAP    dib = NULL, oldBmp = NULL;
    uint32_t*  dibBits = NULL;

    bool       locked = false;
    RECT       lockRect = {};
    bool       lockHasRect = false;

    Surface7();
    ~Surface7();

    Surface7* Root() { return root; }
    bool IsCPU() const { return kind == SK_PLAIN || kind == SK_TEXTURE; }
    void AllocMem();
    void ToARGB(const RECT& r, uint32_t* out, bool applyKey) const;  // region -> ARGB
    void FromARGB(const RECT& r, const uint32_t* in);                // ARGB -> region
    void MarkDirty() { root->dirty = true; uniq++; }
    void UploadIfDirty();
    void FullRect(RECT* r) const { r->left = 0; r->top = 0; r->right = desc.dwWidth; r->bottom = desc.dwHeight; }
    Surface7* ZBuffer();            // attached depth surface, if any
    void Destroy();

    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv);
    STDMETHOD_(ULONG, AddRef)();
    STDMETHOD_(ULONG, Release)();
    STDMETHOD(AddAttachedSurface)(LPDIRECTDRAWSURFACE7);
    STDMETHOD(AddOverlayDirtyRect)(LPRECT);
    STDMETHOD(Blt)(LPRECT, LPDIRECTDRAWSURFACE7, LPRECT, DWORD, LPDDBLTFX);
    STDMETHOD(BltBatch)(LPDDBLTBATCH, DWORD, DWORD);
    STDMETHOD(BltFast)(DWORD, DWORD, LPDIRECTDRAWSURFACE7, LPRECT, DWORD);
    STDMETHOD(DeleteAttachedSurface)(DWORD, LPDIRECTDRAWSURFACE7);
    STDMETHOD(EnumAttachedSurfaces)(LPVOID, LPDDENUMSURFACESCALLBACK7);
    STDMETHOD(EnumOverlayZOrders)(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK7);
    STDMETHOD(Flip)(LPDIRECTDRAWSURFACE7, DWORD);
    STDMETHOD(GetAttachedSurface)(LPDDSCAPS2, LPDIRECTDRAWSURFACE7*);
    STDMETHOD(GetBltStatus)(DWORD);
    STDMETHOD(GetCaps)(LPDDSCAPS2);
    STDMETHOD(GetClipper)(LPDIRECTDRAWCLIPPER*);
    STDMETHOD(GetColorKey)(DWORD, LPDDCOLORKEY);
    STDMETHOD(GetDC)(HDC*);
    STDMETHOD(GetFlipStatus)(DWORD);
    STDMETHOD(GetOverlayPosition)(LPLONG, LPLONG);
    STDMETHOD(GetPalette)(LPDIRECTDRAWPALETTE*);
    STDMETHOD(GetPixelFormat)(LPDDPIXELFORMAT);
    STDMETHOD(GetSurfaceDesc)(LPDDSURFACEDESC2);
    STDMETHOD(Initialize)(LPDIRECTDRAW, LPDDSURFACEDESC2);
    STDMETHOD(IsLost)();
    STDMETHOD(Lock)(LPRECT, LPDDSURFACEDESC2, DWORD, HANDLE);
    STDMETHOD(ReleaseDC)(HDC);
    STDMETHOD(Restore)();
    STDMETHOD(SetClipper)(LPDIRECTDRAWCLIPPER);
    STDMETHOD(SetColorKey)(DWORD, LPDDCOLORKEY);
    STDMETHOD(SetOverlayPosition)(LONG, LONG);
    STDMETHOD(SetPalette)(LPDIRECTDRAWPALETTE);
    STDMETHOD(Unlock)(LPRECT);
    STDMETHOD(UpdateOverlay)(LPRECT, LPDIRECTDRAWSURFACE7, LPRECT, DWORD, LPDDOVERLAYFX);
    STDMETHOD(UpdateOverlayDisplay)(DWORD);
    STDMETHOD(UpdateOverlayZOrder)(DWORD, LPDIRECTDRAWSURFACE7);
    STDMETHOD(GetDDInterface)(LPVOID*);
    STDMETHOD(PageLock)(DWORD);
    STDMETHOD(PageUnlock)(DWORD);
    STDMETHOD(SetSurfaceDesc)(LPDDSURFACEDESC2, DWORD);
    STDMETHOD(SetPrivateData)(REFGUID, LPVOID, DWORD, DWORD);
    STDMETHOD(GetPrivateData)(REFGUID, LPVOID, LPDWORD);
    STDMETHOD(FreePrivateData)(REFGUID);
    STDMETHOD(GetUniquenessValue)(LPDWORD);
    STDMETHOD(ChangeUniquenessValue)();
    STDMETHOD(SetPriority)(DWORD);
    STDMETHOD(GetPriority)(LPDWORD);
    STDMETHOD(SetLOD)(DWORD);
    STDMETHOD(GetLOD)(LPDWORD);
};

// ---------------------------------------------------------------------------
struct DDraw7 : IDirectDraw7 {
    LONG     ref = 1;
    HWND     hwnd = NULL;
    DWORD    coop = 0;
    bool     exclusive = false;
    DWORD    modeW = 640, modeH = 480, modeBpp = 16;
    bool     modeSet = false;
    Surface7* primary = NULL;
    D3D7*    d3d = NULL;

    DDraw7();
    void DisplayFormat(DWORD* w, DWORD* h, DDPIXELFORMAT* pf);

    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv);
    STDMETHOD_(ULONG, AddRef)();
    STDMETHOD_(ULONG, Release)();
    STDMETHOD(Compact)();
    STDMETHOD(CreateClipper)(DWORD, LPDIRECTDRAWCLIPPER*, IUnknown*);
    STDMETHOD(CreatePalette)(DWORD, LPPALETTEENTRY, LPDIRECTDRAWPALETTE*, IUnknown*);
    STDMETHOD(CreateSurface)(LPDDSURFACEDESC2, LPDIRECTDRAWSURFACE7*, IUnknown*);
    STDMETHOD(DuplicateSurface)(LPDIRECTDRAWSURFACE7, LPDIRECTDRAWSURFACE7*);
    STDMETHOD(EnumDisplayModes)(DWORD, LPDDSURFACEDESC2, LPVOID, LPDDENUMMODESCALLBACK2);
    STDMETHOD(EnumSurfaces)(DWORD, LPDDSURFACEDESC2, LPVOID, LPDDENUMSURFACESCALLBACK7);
    STDMETHOD(FlipToGDISurface)();
    STDMETHOD(GetCaps)(LPDDCAPS, LPDDCAPS);
    STDMETHOD(GetDisplayMode)(LPDDSURFACEDESC2);
    STDMETHOD(GetFourCCCodes)(LPDWORD, LPDWORD);
    STDMETHOD(GetGDISurface)(LPDIRECTDRAWSURFACE7*);
    STDMETHOD(GetMonitorFrequency)(LPDWORD);
    STDMETHOD(GetScanLine)(LPDWORD);
    STDMETHOD(GetVerticalBlankStatus)(LPBOOL);
    STDMETHOD(Initialize)(GUID*);
    STDMETHOD(RestoreDisplayMode)();
    STDMETHOD(SetCooperativeLevel)(HWND, DWORD);
    STDMETHOD(SetDisplayMode)(DWORD, DWORD, DWORD, DWORD, DWORD);
    STDMETHOD(WaitForVerticalBlank)(DWORD, HANDLE);
    STDMETHOD(GetAvailableVidMem)(LPDDSCAPS2, LPDWORD, LPDWORD);
    STDMETHOD(GetSurfaceFromDC)(HDC, LPDIRECTDRAWSURFACE7*);
    STDMETHOD(RestoreAllSurfaces)();
    STDMETHOD(TestCooperativeLevel)();
    STDMETHOD(GetDeviceIdentifier)(LPDDDEVICEIDENTIFIER2, DWORD);
    STDMETHOD(StartModeTest)(LPSIZE, DWORD, DWORD);
    STDMETHOD(EvaluateMode)(DWORD, DWORD*);
};

// ---------------------------------------------------------------------------
struct D3D7 : IDirect3D7 {
    DDraw7* dd;
    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv);
    STDMETHOD_(ULONG, AddRef)();
    STDMETHOD_(ULONG, Release)();
    STDMETHOD(EnumDevices)(LPD3DENUMDEVICESCALLBACK7, LPVOID);
    STDMETHOD(CreateDevice)(REFCLSID, LPDIRECTDRAWSURFACE7, LPDIRECT3DDEVICE7*);
    STDMETHOD(CreateVertexBuffer)(LPD3DVERTEXBUFFERDESC, LPDIRECT3DVERTEXBUFFER7*, DWORD);
    STDMETHOD(EnumZBufferFormats)(REFCLSID, LPD3DENUMPIXELFORMATSCALLBACK, LPVOID);
    STDMETHOD(EvictManagedTextures)();
};

struct VB7 : IDirect3DVertexBuffer7 {
    LONG ref = 1;
    D3DVERTEXBUFFERDESC desc = {};
    std::vector<BYTE> mem;
    UINT stride = 0;
    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv);
    STDMETHOD_(ULONG, AddRef)();
    STDMETHOD_(ULONG, Release)();
    STDMETHOD(Lock)(DWORD, LPVOID*, LPDWORD);
    STDMETHOD(Unlock)();
    STDMETHOD(ProcessVertices)(DWORD, DWORD, DWORD, LPDIRECT3DVERTEXBUFFER7, DWORD, LPDIRECT3DDEVICE7, DWORD);
    STDMETHOD(GetVertexBufferDesc)(LPD3DVERTEXBUFFERDESC);
    STDMETHOD(Optimize)(LPDIRECT3DDEVICE7, DWORD);
    STDMETHOD(ProcessVerticesStrided)(DWORD, DWORD, DWORD, LPD3DDRAWPRIMITIVESTRIDEDDATA, DWORD, LPDIRECT3DDEVICE7, DWORD);
};

struct Device7;
DDraw7* CreateDDraw7();
void    DeviceDescFill(D3DDEVICEDESC7* d, const GUID& guid, bool hw, bool tnl);
void    PresentSurface(Surface7* src, const RECT* srcRect);
void    D3D7_FlushTransparent();   // draws queued transparent geometry now (d3d7.cpp)
void    DumpTextureNow(Surface7* s);   // debug: save a texture to textures_dump (ddraw.cpp)
