// hod2fix - rendering backend (Direct3D 9Ex). The DirectX 7 front end talks to
// the GPU only through this interface, so it never needs the D3D9 headers.
//
// All coordinates passed in are in *game* pixels; the backend scales them to
// the internal render resolution.  State values are already translated to
// their D3D9 numeric equivalents by the caller.
#pragma once
#include "common.h"

namespace R {

struct Texture;   // ARGB8888 sampled texture (with mip levels)
struct Target;    // colour render target, internally upscaled
struct Depth;     // depth/stencil buffer, internally upscaled

bool Init(HWND hwnd);
void Shutdown();
bool Ready();

// --- resources -------------------------------------------------------------
Texture* CreateTexture(int w, int h, int levels);
void     DestroyTexture(Texture* t);
void     UploadTexture(Texture* t, int level, const uint32_t* argb, int w, int h);

Target*  CreateTarget(int w, int h);
void     DestroyTarget(Target* t);
Depth*   CreateDepth(int w, int h);
void     DestroyDepth(Depth* d);

// Read the target back at game resolution (ARGB8888, pitch = w*4).
bool     ReadTarget(Target* t, uint32_t* out);
// Draw an ARGB image (w x h) into rect dst (game pixels) of target.
void     WriteTarget(Target* t, const uint32_t* argb, int w, int h, const RECT& dst, bool keyed);
void     FillTarget(Target* t, const RECT* r, uint32_t argb);
void     CopyTarget(Target* dst, const RECT* dr, Target* src, const RECT* sr);
void     ClearDepth(Depth* d, float z);

// --- presentation ----------------------------------------------------------
void     Present(Target* t, const RECT* srcGame);
void     WaitVBlank();                          // paces like a 60 Hz vblank
enum { HOTKEY_SCREENSHOT = 1, HOTKEY_HALO = 2, HOTKEY_SMOOTH = 3, HOTKEY_PROBE = 4 };
void     Hotkey(int which);                     // queued, handled at the next Present
// Writes an ARGB image as PNG (synchronously); alpha keeps the alpha channel.
bool     SavePNG(const char* path, const uint32_t* argb, int w, int h, bool alpha);
// Where the game image lands inside the window client area (client pixels).
bool     GetViewMapping(RECT* dst, int* gw, int* gh);

// --- 3D device -------------------------------------------------------------
void SetRenderTarget(Target* t, Depth* d);
void BeginScene();
void EndScene();
void Clear(DWORD count, const RECT* rects, DWORD flags, DWORD color, float z, DWORD stencil);
void SetRS(DWORD state, DWORD value);
void SetTSS(DWORD stage, DWORD type, DWORD value);
void SetSampler(DWORD stage, DWORD type, DWORD value);
void SetTexture(DWORD stage, Texture* t);
void SetTextureTarget(DWORD stage, Target* t);
void SetTransform(DWORD type, const float* m16);
void SetViewport(DWORD x, DWORD y, DWORD w, DWORD h, float minZ, float maxZ);
void SetMaterial(const void* mat);              // D3DMATERIAL7 == D3DMATERIAL9
void SetLight(DWORD idx, const void* light);    // D3DLIGHT7 == D3DLIGHT9
void LightEnable(DWORD idx, BOOL on);
void SetClipPlane(DWORD idx, const float* plane);
void Draw(DWORD prim, DWORD fvf, const void* verts, DWORD vcount, const WORD* idx, DWORD icount);

UINT FVFStride(DWORD fvf);
float ScaleX();
float ScaleY();

} // namespace R
