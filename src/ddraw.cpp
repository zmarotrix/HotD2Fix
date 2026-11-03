// hod2fix - IDirectDraw7 / IDirectDrawSurface7 / clipper / palette.
#include "dx7.h"
#include <intrin.h>
#include <set>

// ---------------------------------------------------------------------------
// pixel formats
// ---------------------------------------------------------------------------
DDPIXELFORMAT MakePF(DWORD bpp, DWORD r, DWORD g, DWORD b, DWORD a)
{
    DDPIXELFORMAT pf = {};
    pf.dwSize = sizeof(pf);
    pf.dwFlags = DDPF_RGB | (a ? DDPF_ALPHAPIXELS : 0);
    pf.dwRGBBitCount = bpp;
    pf.dwRBitMask = r;
    pf.dwGBitMask = g;
    pf.dwBBitMask = b;
    pf.dwRGBAlphaBitMask = a;
    return pf;
}
DDPIXELFORMAT PF_X8R8G8B8() { return MakePF(32, 0xFF0000, 0xFF00, 0xFF, 0); }
DDPIXELFORMAT PF_R5G6B5()   { return MakePF(16, 0xF800, 0x07E0, 0x001F, 0); }

void PixConv::init(const DDPIXELFORMAT& pf)
{
    bpp = pf.dwRGBBitCount;
    pal8 = (pf.dwFlags & DDPF_PALETTEINDEXED8) != 0;
    if (pal8) bpp = 8;
    if (bpp == 0) bpp = 16;
    mask[0] = pf.dwRBitMask;
    mask[1] = pf.dwGBitMask;
    mask[2] = pf.dwBBitMask;
    mask[3] = (pf.dwFlags & DDPF_ALPHAPIXELS) ? pf.dwRGBAlphaBitMask : 0;
    if (!(pf.dwFlags & DDPF_RGB)) mask[0] = mask[1] = mask[2] = mask[3] = 0;
    for (int i = 0; i < 4; i++) {
        unsigned long s = 0;
        if (mask[i] && _BitScanForward(&s, mask[i])) {
            shift[i] = (int)s;
            bits[i] = (int)__popcnt(mask[i]);
        } else {
            shift[i] = 0;
            bits[i] = 0;
        }
    }
}

static inline uint32_t Expand(uint32_t x, int bits)
{
    if (bits >= 8) return x >> (bits - 8);
    uint32_t max = (1u << bits) - 1;
    return (x * 255 + (max >> 1)) / max;
}

static inline uint32_t Reduce(uint32_t x8, int bits)
{
    if (bits >= 8) return x8 << (bits - 8);
    uint32_t max = (1u << bits) - 1;
    return (x8 * max + 127) / 255;
}

uint32_t PixConv::toARGB(uint32_t v) const
{
    if (bpp == 32 && mask[0] == 0xFF0000 && mask[1] == 0xFF00 && mask[2] == 0xFF)
        return mask[3] ? v : (v | 0xFF000000);
    if (bpp == 16 && mask[0] == 0xF800 && mask[1] == 0x07E0 && mask[2] == 0x001F) {
        uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        return 0xFF000000 | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
    }
    uint32_t c[4];
    for (int i = 0; i < 4; i++)
        c[i] = bits[i] ? Expand((v & mask[i]) >> shift[i], bits[i]) : (i == 3 ? 255u : 0u);
    return (c[3] << 24) | (c[0] << 16) | (c[1] << 8) | c[2];
}

uint32_t PixConv::fromARGB(uint32_t c) const
{
    if (bpp == 32 && mask[0] == 0xFF0000 && mask[1] == 0xFF00 && mask[2] == 0xFF)
        return mask[3] ? c : (c & 0xFFFFFF);
    uint32_t ch[4] = { (c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24 };
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        if (bits[i]) v |= (Reduce(ch[i], bits[i]) << shift[i]) & mask[i];
    return v;
}

static inline uint32_t ReadPix(const BYTE* p, DWORD bpp)
{
    switch (bpp) {
    case 8:  return *p;
    case 16: return *(const WORD*)p;
    case 24: return p[0] | (p[1] << 8) | (p[2] << 16);
    default: return *(const DWORD*)p;
    }
}

static inline void WritePix(BYTE* p, DWORD bpp, uint32_t v)
{
    switch (bpp) {
    case 8:  *p = (BYTE)v; break;
    case 16: *(WORD*)p = (WORD)v; break;
    case 24: p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); break;
    default: *(DWORD*)p = v; break;
    }
}

// ---------------------------------------------------------------------------
// Clipper
// ---------------------------------------------------------------------------
HRESULT Clipper7::QueryInterface(REFIID riid, LPVOID* ppv)
{
    if (riid == IID_IUnknown || riid == IID_IDirectDrawClipper) { *ppv = this; AddRef(); return S_OK; }
    *ppv = NULL;
    return E_NOINTERFACE;
}
ULONG Clipper7::AddRef() { return InterlockedIncrement(&ref); }
ULONG Clipper7::Release() { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
HRESULT Clipper7::GetClipList(LPRECT, LPRGNDATA, LPDWORD size) { if (size) *size = 0; return DDERR_NOCLIPLIST; }
HRESULT Clipper7::GetHWnd(HWND* h) { if (!h) return DDERR_INVALIDPARAMS; *h = hwnd; return DD_OK; }
HRESULT Clipper7::Initialize(LPDIRECTDRAW, DWORD) { return DDERR_ALREADYINITIALIZED; }
HRESULT Clipper7::IsClipListChanged(BOOL* b) { if (b) *b = FALSE; return DD_OK; }
HRESULT Clipper7::SetClipList(LPRGNDATA, DWORD) { return DD_OK; }
HRESULT Clipper7::SetHWnd(DWORD, HWND h) { hwnd = h; if (h) Win_Attach(h); return DD_OK; }

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
HRESULT Palette7::QueryInterface(REFIID riid, LPVOID* ppv)
{
    if (riid == IID_IUnknown || riid == IID_IDirectDrawPalette) { *ppv = this; AddRef(); return S_OK; }
    *ppv = NULL;
    return E_NOINTERFACE;
}
ULONG Palette7::AddRef() { return InterlockedIncrement(&ref); }
ULONG Palette7::Release() { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
HRESULT Palette7::GetCaps(LPDWORD c) { if (c) *c = caps; return DD_OK; }
HRESULT Palette7::GetEntries(DWORD, DWORD start, DWORD count, LPPALETTEENTRY out)
{
    if (!out || start + count > 256) return DDERR_INVALIDPARAMS;
    memcpy(out, e + start, count * sizeof(PALETTEENTRY));
    return DD_OK;
}
HRESULT Palette7::Initialize(LPDIRECTDRAW, DWORD, LPPALETTEENTRY) { return DDERR_ALREADYINITIALIZED; }
HRESULT Palette7::SetEntries(DWORD, DWORD start, DWORD count, LPPALETTEENTRY in)
{
    if (!in || start + count > 256) return DDERR_INVALIDPARAMS;
    memcpy(e + start, in, count * sizeof(PALETTEENTRY));
    return DD_OK;
}

// ---------------------------------------------------------------------------
// Gamma control (accepted, not applied)
// ---------------------------------------------------------------------------
HRESULT Gamma7::QueryInterface(REFIID riid, LPVOID* ppv) { return owner->QueryInterface(riid, ppv); }
ULONG Gamma7::AddRef() { return owner->AddRef(); }
ULONG Gamma7::Release() { return owner->Release(); }
HRESULT Gamma7::GetGammaRamp(DWORD, LPDDGAMMARAMP r) { if (!r) return DDERR_INVALIDPARAMS; *r = ramp; return DD_OK; }
HRESULT Gamma7::SetGammaRamp(DWORD, LPDDGAMMARAMP r) { if (!r) return DDERR_INVALIDPARAMS; ramp = *r; LogOnce("SetGammaRamp ignored"); return DD_OK; }

// ---------------------------------------------------------------------------
// Surface
// ---------------------------------------------------------------------------
LONG g_liveSurfaces;

Surface7::Surface7()
{
    InterlockedIncrement(&g_liveSurfaces);
    gamma.owner = this;
    for (int i = 0; i < 256; i++)
        gamma.ramp.red[i] = gamma.ramp.green[i] = gamma.ramp.blue[i] = (WORD)(i * 257);
}

Surface7::~Surface7() { InterlockedDecrement(&g_liveSurfaces); }

void Surface7::AllocMem()
{
    DWORD bpp = conv.bpp;
    pitch = (LONG)(((desc.dwWidth * bpp / 8) + 3) & ~3u);
    mem.assign((size_t)pitch * desc.dwHeight, 0);
    desc.lPitch = pitch;
    desc.dwFlags |= DDSD_PITCH;
}

Surface7* Surface7::ZBuffer()
{
    for (Surface7* a : attached)
        if (a->kind == SK_DEPTH) return a;
    return NULL;
}

static void ClipRect(RECT& r, const Surface7* s)
{
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > (LONG)s->desc.dwWidth) r.right = s->desc.dwWidth;
    if (r.bottom > (LONG)s->desc.dwHeight) r.bottom = s->desc.dwHeight;
}

// Extracts a region as ARGB8888. With applyKey, colour-keyed pixels get alpha 0.
void Surface7::ToARGB(const RECT& r, uint32_t* out, bool applyKey) const
{
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;

    if (rt) {
        std::vector<uint32_t> all((size_t)desc.dwWidth * desc.dwHeight);
        if (!R::ReadTarget(rt, all.data())) { memset(out, 0, (size_t)w * h * 4); return; }
        for (int y = 0; y < h; y++)
            memcpy(out + y * w, all.data() + (r.top + y) * desc.dwWidth + r.left, w * 4);
        return;
    }
    if (mem.empty()) { memset(out, 0, (size_t)w * h * 4); return; }

    DWORD bpp = conv.bpp, bytes = bpp / 8;
    for (int y = 0; y < h; y++) {
        const BYTE* row = mem.data() + (r.top + y) * pitch + r.left * bytes;
        uint32_t* o = out + y * w;
        for (int x = 0; x < w; x++, row += bytes) {
            uint32_t v = ReadPix(row, bpp);
            uint32_t c;
            if (conv.pal8) {
                const PALETTEENTRY& pe = palette ? palette->e[v & 255] : PALETTEENTRY{ (BYTE)v, (BYTE)v, (BYTE)v, 0 };
                c = 0xFF000000 | (pe.peRed << 16) | (pe.peGreen << 8) | pe.peBlue;
            } else {
                c = conv.toARGB(v);
            }
            if (applyKey && hasCK && v >= ck.dwColorSpaceLowValue && v <= ck.dwColorSpaceHighValue)
                c &= 0x00FFFFFF;
            o[x] = c;
        }
    }
}

void Surface7::FromARGB(const RECT& r, const uint32_t* in)
{
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0 || mem.empty()) return;
    DWORD bpp = conv.bpp, bytes = bpp / 8;
    for (int y = 0; y < h; y++) {
        BYTE* row = mem.data() + (r.top + y) * pitch + r.left * bytes;
        const uint32_t* s = in + y * w;
        for (int x = 0; x < w; x++, row += bytes)
            WritePix(row, bpp, conv.fromARGB(s[x]));
    }
}

// Fully transparent texels are mostly stored black.  Bilinear filtering blends
// that black into visible edges (dark fringes, and dark lines along straight
// transparent edges such as backdrop strips).  This fills transparent texels
// with the average colour of their visible neighbours, growing outwards a few
// texels, while leaving alpha untouched.  Returns false if nothing is
// transparent.
static bool PadTransparent(uint32_t* px, int w, int h)
{
    std::vector<BYTE> known((size_t)w * h);
    bool any = false;
    for (size_t i = 0; i < known.size(); i++) {
        known[i] = (px[i] >> 24) != 0;
        if (!known[i]) any = true;
    }
    if (!any) return false;
    std::vector<int> grown;
    for (int pass = 0; pass < 4; pass++) {
        grown.clear();
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                size_t i = (size_t)y * w + x;
                if (known[i]) continue;
                unsigned r = 0, g = 0, b = 0, n = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    int yy = y + dy;
                    if (yy < 0 || yy >= h) continue;
                    for (int dx = -1; dx <= 1; dx++) {
                        int xx = x + dx;
                        if (xx < 0 || xx >= w || (!dx && !dy)) continue;
                        size_t j = (size_t)yy * w + xx;
                        if (!known[j]) continue;
                        r += (px[j] >> 16) & 255; g += (px[j] >> 8) & 255; b += px[j] & 255; n++;
                    }
                }
                if (!n) continue;
                px[i] = (px[i] & 0xFF000000) | ((r / n) << 16) | ((g / n) << 8) | (b / n);
                grown.push_back((int)i);
            }
        }
        if (grown.empty()) break;
        for (int i : grown) known[i] = 1;
    }
    return true;
}

// Debug: every distinct texture image is written once to the textures_dump
// folder, named WxH_<hash>.png (with alpha).
static void DumpTexture(Surface7* s, const uint32_t* argb)
{
    static std::set<uint32_t> seen;
    int w = s->desc.dwWidth, h = s->desc.dwHeight;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < (size_t)w * h; i++) { hash ^= argb[i]; hash *= 16777619u; }
    if (!seen.insert(hash).second) return;
    char dir[MAX_PATH], path[MAX_PATH];
    wsprintfA(dir, "%s\\textures_dump", GameDir());
    CreateDirectoryA(dir, NULL);
    wsprintfA(path, "%s\\%dx%d_%08X.png", dir, w, h, hash);
    R::SavePNG(path, argb, w, h, true);
}

void Surface7::UploadIfDirty()
{
    Surface7* r = root;
    if (!r->dirty || r->kind != SK_TEXTURE) return;
    if (!r->tex) {
        int levels = 1 + (int)r->chain.size();
        r->tex = R::CreateTexture(r->desc.dwWidth, r->desc.dwHeight, levels);
        if (!r->tex) return;
    }
    std::vector<uint32_t> buf;
    bool padded = false;
    for (Surface7* s = r; s; s = s->next) {
        RECT full;
        s->FullRect(&full);
        buf.resize((size_t)s->desc.dwWidth * s->desc.dwHeight);
        s->ToARGB(full, buf.data(), r->hasCK);
        if (s == r) {
            uint32_t hsh = 2166136261u;
            for (uint32_t c : buf) { hsh ^= c; hsh *= 16777619u; }
            r->hash = hsh;
            // Soft effects have plenty of partially transparent texels; cut-out
            // sprites and models only have a stray few (or none).
            size_t partial = 0;
            for (uint32_t c : buf) { uint32_t a = c >> 24; if (a >= 16 && a < 240) partial++; }
            r->softAlpha = partial > 8 && partial * 50 > buf.size();
        }
        if (g_cfg.dumpTextures && s == r) DumpTexture(r, buf.data());
        R::UploadTexture(r->tex, s->level, buf.data(), s->desc.dwWidth, s->desc.dwHeight);
        if (g_cfg.forceLinear || g_cfg.smoothToggleKey) {
            if (s == r) padded = PadTransparent(buf.data(), s->desc.dwWidth, s->desc.dwHeight);
            else if (padded) PadTransparent(buf.data(), s->desc.dwWidth, s->desc.dwHeight);
            if (padded) {
                if (!r->texPadded) r->texPadded = R::CreateTexture(r->desc.dwWidth, r->desc.dwHeight, 1 + (int)r->chain.size());
                if (r->texPadded) R::UploadTexture(r->texPadded, s->level, buf.data(), s->desc.dwWidth, s->desc.dwHeight);
            }
        }
        if (s->next == r) break;
    }
    if (!padded && r->texPadded) { R::DestroyTexture(r->texPadded); r->texPadded = NULL; }
    r->dirty = false;
}

void Surface7::Destroy()
{
    for (Surface7* s : chain) { s->Destroy(); delete s; }
    chain.clear();
    for (Surface7* a : attached) a->Release();
    attached.clear();
    if (dc) { SelectObject(dc, oldBmp); DeleteDC(dc); dc = NULL; }
    if (dib) { DeleteObject(dib); dib = NULL; }
    if (clipper) { clipper->Release(); clipper = NULL; }
    if (palette) { palette->Release(); palette = NULL; }
    R::DestroyTexture(tex); tex = NULL;
    R::DestroyTexture(texPadded); texPadded = NULL;
    R::DestroyTarget(rt); rt = NULL;
    R::DestroyDepth(ds); ds = NULL;
    if (dd && dd->primary == this) dd->primary = NULL;
}

HRESULT Surface7::QueryInterface(REFIID riid, LPVOID* ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirectDrawSurface7) { *ppv = this; AddRef(); return S_OK; }
    if (riid == IID_IDirectDrawGammaControl) { *ppv = &gamma; AddRef(); return S_OK; }
    LogOnce("Surface7::QueryInterface: unsupported interface");
    *ppv = NULL;
    return E_NOINTERFACE;
}

ULONG Surface7::AddRef()
{
    return InterlockedIncrement(&ref);
}

ULONG Surface7::Release()
{
    LONG r = InterlockedDecrement(&ref);
    if (r <= 0) {
        if (root == this) {
            Destroy();
            delete this;
        } else {
            ref = 0;  // implicit chain members die with their root
        }
        return 0;
    }
    return r;
}

HRESULT Surface7::AddAttachedSurface(LPDIRECTDRAWSURFACE7 s)
{
    Surface7* a = (Surface7*)s;
    if (!a) return DDERR_INVALIDPARAMS;
    for (Surface7* x : attached) if (x == a) return DDERR_SURFACEALREADYATTACHED;
    a->AddRef();
    attached.push_back(a);
    return DD_OK;
}

HRESULT Surface7::AddOverlayDirtyRect(LPRECT) { return DDERR_UNSUPPORTED; }

// Blit implementation shared by Blt and BltFast.
static HRESULT DoBlt(Surface7* dst, const RECT* dr, Surface7* src, const RECT* sr, bool keySrc)
{
    RECT d, s;
    if (dr) d = *dr; else dst->FullRect(&d);
    if (sr) s = *sr; else src->FullRect(&s);
    ClipRect(s, src);
    if (d.right <= d.left || d.bottom <= d.top || s.right <= s.left || s.bottom <= s.top) return DD_OK;
    keySrc = keySrc && src->hasCK;

    // Anything blitted to the primary is shown on screen.
    if (dst->kind == SK_PRIMARY && dst->dd && !dst->dd->exclusive) {
        PresentSurface(src, &s);
        return DD_OK;
    }

    if (dst->rt && src->rt && !keySrc) {
        RECT dc = d; ClipRect(dc, dst);
        R::CopyTarget(dst->rt, &dc, src->rt, &s);
        dst->uniq++;
        return DD_OK;
    }

    int sw = s.right - s.left, sh = s.bottom - s.top;
    std::vector<uint32_t> px((size_t)sw * sh);
    src->ToARGB(s, px.data(), keySrc);

    if (dst->rt) {
        R::WriteTarget(dst->rt, px.data(), sw, sh, d, keySrc);
        dst->uniq++;
        return DD_OK;
    }
    if (dst->mem.empty()) return DDERR_GENERIC;

    // CPU destination: nearest-neighbour stretch with clipping.
    int dw = d.right - d.left, dh = d.bottom - d.top;
    DWORD bpp = dst->conv.bpp, bytes = bpp / 8;
    bool sameFmt = !keySrc && dw == sw && dh == sh && !src->rt && src->conv.bpp == bpp &&
                   !src->conv.pal8 && !dst->conv.pal8 &&
                   memcmp(src->conv.mask, dst->conv.mask, sizeof(dst->conv.mask)) == 0;
    for (int y = 0; y < dh; y++) {
        int ty = d.top + y;
        if (ty < 0 || ty >= (int)dst->desc.dwHeight) continue;
        int syy = y * sh / dh;
        if (sameFmt && d.left >= 0 && d.right <= (LONG)dst->desc.dwWidth) {
            memcpy(dst->mem.data() + ty * dst->pitch + d.left * bytes,
                   src->mem.data() + (s.top + syy) * src->pitch + s.left * bytes, dw * bytes);
            continue;
        }
        BYTE* row = dst->mem.data() + ty * dst->pitch;
        for (int x = 0; x < dw; x++) {
            int tx = d.left + x;
            if (tx < 0 || tx >= (int)dst->desc.dwWidth) continue;
            uint32_t c = px[syy * sw + x * sw / dw];
            if (keySrc && !(c >> 24)) continue;
            WritePix(row + tx * bytes, bpp, dst->conv.fromARGB(c));
        }
    }
    dst->MarkDirty();
    return DD_OK;
}

static void FillCPU(Surface7* s, const RECT& r, DWORD color)
{
    if (s->mem.empty()) return;
    DWORD bpp = s->conv.bpp, bytes = bpp / 8;
    for (LONG y = r.top; y < r.bottom; y++) {
        BYTE* row = s->mem.data() + y * s->pitch + r.left * bytes;
        for (LONG x = r.left; x < r.right; x++, row += bytes) WritePix(row, bpp, color);
    }
    s->MarkDirty();
}

HRESULT Surface7::Blt(LPRECT dr, LPDIRECTDRAWSURFACE7 srcI, LPRECT sr, DWORD flags, LPDDBLTFX fx)
{
    if (flags & DDBLT_COLORFILL) {
        if (!fx) return DDERR_INVALIDPARAMS;
        RECT r; if (dr) r = *dr; else FullRect(&r);
        ClipRect(r, this);
        if (rt) R::FillTarget(rt, &r, conv.toARGB(fx->dwFillColor));
        else FillCPU(this, r, fx->dwFillColor);
        return DD_OK;
    }
    if (flags & DDBLT_DEPTHFILL) {
        if (!fx) return DDERR_INVALIDPARAMS;
        DWORD maxv = conv.bpp >= 32 ? 0xFFFFFFFF : ((1u << conv.bpp) - 1);
        if (ds) R::ClearDepth(ds, (float)((double)fx->dwFillDepth / (double)maxv));
        return DD_OK;
    }
    Surface7* src = (Surface7*)srcI;
    if (!src) return DDERR_INVALIDPARAMS;
    bool key = (flags & DDBLT_KEYSRC) != 0;
    if ((flags & DDBLT_KEYSRCOVERRIDE) && fx) {
        LogOnce("Blt: DDBLT_KEYSRCOVERRIDE treated as source key");
        key = true;
    }
    return DoBlt(this, dr, src, sr, key);
}

HRESULT Surface7::BltBatch(LPDDBLTBATCH, DWORD, DWORD) { return DDERR_UNSUPPORTED; }

HRESULT Surface7::BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE7 srcI, LPRECT sr, DWORD flags)
{
    Surface7* src = (Surface7*)srcI;
    if (!src) return DDERR_INVALIDPARAMS;
    RECT s; if (sr) s = *sr; else src->FullRect(&s);
    RECT d = { (LONG)x, (LONG)y, (LONG)x + (s.right - s.left), (LONG)y + (s.bottom - s.top) };
    return DoBlt(this, &d, src, &s, (flags & DDBLTFAST_SRCCOLORKEY) != 0);
}

HRESULT Surface7::DeleteAttachedSurface(DWORD, LPDIRECTDRAWSURFACE7 s)
{
    for (size_t i = 0; i < attached.size(); i++) {
        if (!s || attached[i] == (Surface7*)s) {
            attached[i]->Release();
            attached.erase(attached.begin() + i);
            return DD_OK;
        }
    }
    return DDERR_SURFACENOTATTACHED;
}

HRESULT Surface7::EnumAttachedSurfaces(LPVOID ctx, LPDDENUMSURFACESCALLBACK7 cb)
{
    if (!cb) return DDERR_INVALIDPARAMS;
    if (next && next != root) {
        next->AddRef();
        DDSURFACEDESC2 d = next->desc;
        if (cb(next, &d, ctx) == DDENUMRET_CANCEL) return DD_OK;
    }
    for (Surface7* a : attached) {
        a->AddRef();
        DDSURFACEDESC2 d = a->desc;
        if (cb(a, &d, ctx) == DDENUMRET_CANCEL) return DD_OK;
    }
    return DD_OK;
}

HRESULT Surface7::EnumOverlayZOrders(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK7) { return DDERR_UNSUPPORTED; }

HRESULT Surface7::Flip(LPDIRECTDRAWSURFACE7 target, DWORD)
{
    Surface7* back = target ? (Surface7*)target : root->next;
    if (!back) back = this;
    PresentSurface(back, NULL);
    return DD_OK;
}

HRESULT Surface7::GetAttachedSurface(LPDDSCAPS2 caps, LPDIRECTDRAWSURFACE7* out)
{
    if (!caps || !out) return DDERR_INVALIDPARAMS;
    DWORD want = caps->dwCaps;
    *out = NULL;
    if (next && next != this) {
        DWORD have = next->desc.ddsCaps.dwCaps;
        if ((have & want) == want) { next->AddRef(); *out = next; return DD_OK; }
    }
    for (Surface7* a : attached) {
        if ((a->desc.ddsCaps.dwCaps & want) == want) { a->AddRef(); *out = a; return DD_OK; }
    }
    return DDERR_NOTFOUND;
}

HRESULT Surface7::GetBltStatus(DWORD) { return DD_OK; }
HRESULT Surface7::GetCaps(LPDDSCAPS2 c) { if (!c) return DDERR_INVALIDPARAMS; *c = desc.ddsCaps; return DD_OK; }

HRESULT Surface7::GetClipper(LPDIRECTDRAWCLIPPER* c)
{
    if (!c) return DDERR_INVALIDPARAMS;
    *c = clipper;
    if (!clipper) return DDERR_NOCLIPPERATTACHED;
    clipper->AddRef();
    return DD_OK;
}

HRESULT Surface7::GetColorKey(DWORD flags, LPDDCOLORKEY k)
{
    if (!k) return DDERR_INVALIDPARAMS;
    if (!(flags & DDCKEY_SRCBLT) || !hasCK) return DDERR_NOCOLORKEY;
    *k = ck;
    return DD_OK;
}

HRESULT Surface7::GetDC(HDC* out)
{
    if (!out) return DDERR_INVALIDPARAMS;
    if (dc) return DDERR_DCALREADYCREATED;
    int w = desc.dwWidth, h = desc.dwHeight;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    if (!dib) {
        dib = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void**)&dibBits, NULL, 0);
        if (!dib) return DDERR_GENERIC;
    }
    RECT full; FullRect(&full);
    ToARGB(full, dibBits, false);
    dc = CreateCompatibleDC(NULL);
    oldBmp = (HBITMAP)SelectObject(dc, dib);
    *out = dc;
    return DD_OK;
}

HRESULT Surface7::ReleaseDC(HDC h)
{
    if (!dc || h != dc) return DDERR_NODC;
    GdiFlush();
    RECT full; FullRect(&full);
    size_t n = (size_t)desc.dwWidth * desc.dwHeight;
    for (size_t i = 0; i < n; i++) dibBits[i] |= 0xFF000000;   // GDI leaves alpha at 0
    if (rt) R::WriteTarget(rt, dibBits, desc.dwWidth, desc.dwHeight, full, false);
    else { FromARGB(full, dibBits); MarkDirty(); }
    SelectObject(dc, oldBmp);
    DeleteDC(dc);
    dc = NULL;
    return DD_OK;
}

HRESULT Surface7::GetFlipStatus(DWORD) { return DD_OK; }
HRESULT Surface7::GetOverlayPosition(LPLONG, LPLONG) { return DDERR_NOTAOVERLAYSURFACE; }

HRESULT Surface7::GetPalette(LPDIRECTDRAWPALETTE* p)
{
    if (!p) return DDERR_INVALIDPARAMS;
    *p = palette;
    if (!palette) return DDERR_NOPALETTEATTACHED;
    palette->AddRef();
    return DD_OK;
}

HRESULT Surface7::GetPixelFormat(LPDDPIXELFORMAT pf)
{
    if (!pf) return DDERR_INVALIDPARAMS;
    *pf = desc.ddpfPixelFormat;
    return DD_OK;
}

HRESULT Surface7::GetSurfaceDesc(LPDDSURFACEDESC2 d)
{
    if (!d) return DDERR_INVALIDPARAMS;
    DWORD size = d->dwSize;
    if (size != sizeof(DDSURFACEDESC2) && size != sizeof(DDSURFACEDESC)) return DDERR_INVALIDPARAMS;
    DDSURFACEDESC2 tmp = desc;
    tmp.dwSize = size;
    tmp.lpSurface = NULL;
    memcpy(d, &tmp, size);
    return DD_OK;
}

HRESULT Surface7::Initialize(LPDIRECTDRAW, LPDDSURFACEDESC2) { return DDERR_ALREADYINITIALIZED; }
HRESULT Surface7::IsLost() { return DD_OK; }

HRESULT Surface7::Lock(LPRECT r, LPDDSURFACEDESC2 d, DWORD flags, HANDLE)
{
    if (!d) return DDERR_INVALIDPARAMS;
    if (kind == SK_DEPTH) { LogOnce("Lock on a Z buffer is not supported"); return DDERR_GENERIC; }
    if (mem.empty()) AllocMem();
    RECT full; FullRect(&full);
    lockHasRect = r != NULL;
    lockRect = r ? *r : full;
    ClipRect(lockRect, this);

    if (rt) {
        // pull the current GPU image so partial writes keep the rest intact
        std::vector<uint32_t> px((size_t)desc.dwWidth * desc.dwHeight);
        if (R::ReadTarget(rt, px.data())) FromARGB(full, px.data());
    }

    DWORD size = d->dwSize ? d->dwSize : sizeof(DDSURFACEDESC2);
    DDSURFACEDESC2 tmp = desc;
    tmp.dwSize = size;
    tmp.dwFlags |= DDSD_PITCH | DDSD_LPSURFACE;
    tmp.lPitch = pitch;
    tmp.lpSurface = mem.data() + lockRect.top * pitch + lockRect.left * (conv.bpp / 8);
    memcpy(d, &tmp, size < sizeof(tmp) ? size : sizeof(tmp));
    locked = true;
    (void)flags;
    return DD_OK;
}

HRESULT Surface7::Unlock(LPRECT)
{
    if (!locked) return DDERR_NOTLOCKED;
    locked = false;
    if (rt) {
        int w = lockRect.right - lockRect.left, h = lockRect.bottom - lockRect.top;
        if (w > 0 && h > 0) {
            std::vector<uint32_t> px((size_t)w * h);
            // read from CPU memory (not the GPU) - temporarily drop rt for ToARGB
            R::Target* t = rt; rt = NULL;
            ToARGB(lockRect, px.data(), false);
            rt = t;
            R::WriteTarget(rt, px.data(), w, h, lockRect, false);
        }
        uniq++;
    } else {
        MarkDirty();
    }
    return DD_OK;
}

HRESULT Surface7::Restore() { return DD_OK; }

HRESULT Surface7::SetClipper(LPDIRECTDRAWCLIPPER c)
{
    if (clipper) clipper->Release();
    clipper = (Clipper7*)c;
    if (clipper) clipper->AddRef();
    return DD_OK;
}

HRESULT Surface7::SetColorKey(DWORD flags, LPDDCOLORKEY k)
{
    if (!(flags & DDCKEY_SRCBLT)) return DD_OK;   // only source keys matter here
    if (k) {
        ck = *k;
        if (!(flags & DDCKEY_COLORSPACE)) ck.dwColorSpaceHighValue = ck.dwColorSpaceLowValue;
        hasCK = true;
        desc.dwFlags |= DDSD_CKSRCBLT;
        desc.ddckCKSrcBlt = ck;
    } else {
        hasCK = false;
        desc.dwFlags &= ~DDSD_CKSRCBLT;
    }
    // every level of a mip chain shares the key
    if (root == this)
        for (Surface7* s : chain) { s->hasCK = hasCK; s->ck = ck; }
    MarkDirty();
    return DD_OK;
}

HRESULT Surface7::SetOverlayPosition(LONG, LONG) { return DDERR_NOTAOVERLAYSURFACE; }

HRESULT Surface7::SetPalette(LPDIRECTDRAWPALETTE p)
{
    if (palette) palette->Release();
    palette = (Palette7*)p;
    if (palette) palette->AddRef();
    MarkDirty();
    return DD_OK;
}

HRESULT Surface7::UpdateOverlay(LPRECT, LPDIRECTDRAWSURFACE7, LPRECT, DWORD, LPDDOVERLAYFX) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT Surface7::UpdateOverlayDisplay(DWORD) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT Surface7::UpdateOverlayZOrder(DWORD, LPDIRECTDRAWSURFACE7) { return DDERR_NOTAOVERLAYSURFACE; }

HRESULT Surface7::GetDDInterface(LPVOID* out)
{
    if (!out) return DDERR_INVALIDPARAMS;
    dd->AddRef();
    *out = static_cast<IDirectDraw7*>(dd);
    return DD_OK;
}

HRESULT Surface7::PageLock(DWORD) { return DD_OK; }
HRESULT Surface7::PageUnlock(DWORD) { return DD_OK; }
HRESULT Surface7::SetSurfaceDesc(LPDDSURFACEDESC2, DWORD) { LogOnce("SetSurfaceDesc unsupported"); return DDERR_UNSUPPORTED; }
HRESULT Surface7::SetPrivateData(REFGUID, LPVOID, DWORD, DWORD) { return DD_OK; }
HRESULT Surface7::GetPrivateData(REFGUID, LPVOID, LPDWORD) { return DDERR_NOTFOUND; }
HRESULT Surface7::FreePrivateData(REFGUID) { return DD_OK; }
HRESULT Surface7::GetUniquenessValue(LPDWORD v) { if (!v) return DDERR_INVALIDPARAMS; *v = uniq; return DD_OK; }
HRESULT Surface7::ChangeUniquenessValue() { uniq++; return DD_OK; }
HRESULT Surface7::SetPriority(DWORD p) { priority = p; return DD_OK; }
HRESULT Surface7::GetPriority(LPDWORD p) { if (!p) return DDERR_INVALIDPARAMS; *p = priority; return DD_OK; }
HRESULT Surface7::SetLOD(DWORD l) { lod = l; return DD_OK; }
HRESULT Surface7::GetLOD(LPDWORD l) { if (!l) return DDERR_INVALIDPARAMS; *l = lod; return DD_OK; }

// Shows a surface on screen.
void PresentSurface(Surface7* src, const RECT* srcRect)
{
    if (!src) return;
    if (src->rt) {
        R::Present(src->rt, srcRect);
        return;
    }
    // CPU surface: draw it into the primary's target first
    Surface7* prim = src->dd ? src->dd->primary : NULL;
    if (!prim || !prim->rt) return;
    RECT s; if (srcRect) s = *srcRect; else src->FullRect(&s);
    int w = s.right - s.left, h = s.bottom - s.top;
    if (w <= 0 || h <= 0) return;
    std::vector<uint32_t> px((size_t)w * h);
    src->ToARGB(s, px.data(), false);
    RECT d = { 0, 0, (LONG)prim->desc.dwWidth, (LONG)prim->desc.dwHeight };
    R::WriteTarget(prim->rt, px.data(), w, h, d, false);
    R::Present(prim->rt, NULL);
}

// ---------------------------------------------------------------------------
// DirectDraw
// ---------------------------------------------------------------------------
DDraw7::DDraw7() {}

DDraw7* CreateDDraw7() { return new DDraw7(); }

void DDraw7::DisplayFormat(DWORD* w, DWORD* h, DDPIXELFORMAT* pf)
{
    if (modeSet) {
        *w = modeW; *h = modeH;
        *pf = modeBpp == 16 ? PF_R5G6B5() : PF_X8R8G8B8();
    } else {
        *w = GetSystemMetrics(SM_CXSCREEN);
        *h = GetSystemMetrics(SM_CYSCREEN);
        *pf = PF_X8R8G8B8();
    }
}

HRESULT DDraw7::QueryInterface(REFIID riid, LPVOID* ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirectDraw7) { *ppv = this; AddRef(); return S_OK; }
    if (riid == IID_IDirect3D7) {
        if (!d3d) { d3d = new D3D7(); d3d->dd = this; }
        AddRef();
        *ppv = d3d;
        return S_OK;
    }
    LogOnce("DDraw7::QueryInterface: unsupported interface requested");
    *ppv = NULL;
    return E_NOINTERFACE;
}

ULONG DDraw7::AddRef() { return InterlockedIncrement(&ref); }
ULONG DDraw7::Release()
{
    LONG r = InterlockedDecrement(&ref);
    if (r <= 0) {
        delete d3d;
        d3d = NULL;
        delete this;
        return 0;
    }
    return r;
}

HRESULT DDraw7::Compact() { return DD_OK; }

HRESULT DDraw7::CreateClipper(DWORD, LPDIRECTDRAWCLIPPER* out, IUnknown*)
{
    if (!out) return DDERR_INVALIDPARAMS;
    *out = new Clipper7();
    return DD_OK;
}

HRESULT DDraw7::CreatePalette(DWORD flags, LPPALETTEENTRY e, LPDIRECTDRAWPALETTE* out, IUnknown*)
{
    if (!out) return DDERR_INVALIDPARAMS;
    Palette7* p = new Palette7();
    p->caps = flags;
    if (e) memcpy(p->e, e, ((flags & DDPCAPS_8BIT) ? 256 : 16) * sizeof(PALETTEENTRY));
    *out = p;
    return DD_OK;
}

static Surface7* NewSurface(DDraw7* dd, const DDSURFACEDESC2& d, SurfKind kind)
{
    Surface7* s = new Surface7();
    s->dd = dd;
    s->desc = d;
    s->desc.dwSize = sizeof(DDSURFACEDESC2);
    s->kind = kind;
    s->conv.init(s->desc.ddpfPixelFormat);
    if (d.dwFlags & DDSD_CKSRCBLT) { s->hasCK = true; s->ck = d.ddckCKSrcBlt; }
    return s;
}

HRESULT DDraw7::CreateSurface(LPDDSURFACEDESC2 sd, LPDIRECTDRAWSURFACE7* out, IUnknown*)
{
    if (!sd || !out) return DDERR_INVALIDPARAMS;
    *out = NULL;
    DDSURFACEDESC2 d = {};
    memcpy(&d, sd, sd->dwSize < sizeof(d) ? sd->dwSize : sizeof(d));
    d.dwSize = sizeof(d);
    DWORD caps = (d.dwFlags & DDSD_CAPS) ? d.ddsCaps.dwCaps : 0;

    DWORD dispW, dispH;
    DDPIXELFORMAT dispPF;
    DisplayFormat(&dispW, &dispH, &dispPF);

    SurfKind kind;
    if (caps & DDSCAPS_PRIMARYSURFACE) kind = SK_PRIMARY;
    else if (caps & DDSCAPS_ZBUFFER)   kind = SK_DEPTH;
    else if (caps & DDSCAPS_3DDEVICE)  kind = SK_TARGET;
    else if (caps & DDSCAPS_TEXTURE)   kind = SK_TEXTURE;
    else                               kind = SK_PLAIN;

    if (kind == SK_PRIMARY) {
        // The primary always looks like the game resolution in exclusive mode,
        // or like the desktop otherwise.
        d.dwWidth = exclusive ? modeW : dispW;
        d.dwHeight = exclusive ? modeH : dispH;
        d.dwFlags |= DDSD_WIDTH | DDSD_HEIGHT;
    }
    if (!(d.dwFlags & DDSD_WIDTH) || !(d.dwFlags & DDSD_HEIGHT) || !d.dwWidth || !d.dwHeight) {
        if (kind == SK_PRIMARY) return DDERR_INVALIDPARAMS;
        d.dwWidth = dispW; d.dwHeight = dispH;   // DX allows this for some legacy cases
        d.dwFlags |= DDSD_WIDTH | DDSD_HEIGHT;
    }
    if (kind == SK_DEPTH) {
        if (!(d.dwFlags & DDSD_PIXELFORMAT)) {
            DWORD bits = (d.dwFlags & DDSD_ZBUFFERBITDEPTH) ? d.dwMipMapCount /*union*/ : 16;
            ZeroMemory(&d.ddpfPixelFormat, sizeof(d.ddpfPixelFormat));
            d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
            d.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
            d.ddpfPixelFormat.dwZBufferBitDepth = bits ? bits : 16;
            d.ddpfPixelFormat.dwZBitMask = bits >= 32 ? 0xFFFFFFFF : (1u << (bits ? bits : 16)) - 1;
            d.dwFlags |= DDSD_PIXELFORMAT;
        }
    } else if (!(d.dwFlags & DDSD_PIXELFORMAT) || kind == SK_PRIMARY) {
        d.ddpfPixelFormat = dispPF;
        d.dwFlags |= DDSD_PIXELFORMAT;
    }
    if (!(caps & DDSCAPS_SYSTEMMEMORY)) caps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
    if (kind == SK_PRIMARY) caps |= DDSCAPS_VISIBLE | DDSCAPS_FRONTBUFFER * ((caps & DDSCAPS_FLIP) ? 1 : 0);
    d.ddsCaps.dwCaps = caps;
    d.dwFlags |= DDSD_CAPS;

    Surface7* s = NewSurface(this, d, kind);
    LogV("CreateSurface kind=%d %ux%u bpp=%u caps=%08X caps2=%08X", kind, d.dwWidth, d.dwHeight,
         s->conv.bpp, caps, d.ddsCaps.dwCaps2);

    switch (kind) {
    case SK_PRIMARY:
    case SK_TARGET:
        s->rt = R::CreateTarget(d.dwWidth, d.dwHeight);
        s->isTexture = (caps & DDSCAPS_TEXTURE) != 0;
        break;
    case SK_DEPTH:
        s->ds = R::CreateDepth(d.dwWidth, d.dwHeight);
        break;
    case SK_TEXTURE:
        s->isTexture = true;
        s->AllocMem();
        break;
    case SK_PLAIN:
        s->AllocMem();
        break;
    }
    if ((kind == SK_PRIMARY || kind == SK_TARGET) && !s->rt) {
        Log("CreateSurface: render target allocation failed (device not ready?)");
        s->Destroy();
        delete s;
        return DDERR_OUTOFVIDEOMEMORY;
    }

    // flipping chain
    if (kind == SK_PRIMARY && (caps & DDSCAPS_FLIP) && (d.dwFlags & DDSD_BACKBUFFERCOUNT) && d.dwBackBufferCount) {
        Surface7* prev = s;
        for (DWORD i = 0; i < d.dwBackBufferCount; i++) {
            DDSURFACEDESC2 bd = d;
            bd.dwFlags &= ~DDSD_BACKBUFFERCOUNT;
            bd.dwBackBufferCount = 0;
            bd.ddsCaps.dwCaps = (caps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE)) |
                                DDSCAPS_3DDEVICE | (i == 0 ? DDSCAPS_BACKBUFFER : 0);
            Surface7* b = NewSurface(this, bd, SK_TARGET);
            b->rt = R::CreateTarget(bd.dwWidth, bd.dwHeight);
            b->root = s;
            s->chain.push_back(b);
            prev->next = b;
            prev = b;
        }
        prev->next = s;   // ring
    }

    // mip chain
    if (kind == SK_TEXTURE && (caps & DDSCAPS_MIPMAP)) {
        DWORD count = (d.dwFlags & DDSD_MIPMAPCOUNT) ? d.dwMipMapCount : 0;
        if (!count) {
            DWORD m = d.dwWidth > d.dwHeight ? d.dwWidth : d.dwHeight;
            while (m) { count++; m >>= 1; }
        }
        s->desc.dwMipMapCount = count;
        s->desc.dwFlags |= DDSD_MIPMAPCOUNT;
        Surface7* prev = s;
        for (DWORD i = 1; i < count; i++) {
            DDSURFACEDESC2 md = d;
            md.dwWidth = d.dwWidth >> i ? d.dwWidth >> i : 1;
            md.dwHeight = d.dwHeight >> i ? d.dwHeight >> i : 1;
            md.dwMipMapCount = count - i;
            md.ddsCaps.dwCaps2 |= DDSCAPS2_MIPMAPSUBLEVEL;
            Surface7* m = NewSurface(this, md, SK_TEXTURE);
            m->isTexture = true;
            m->level = (int)i;
            m->root = s;
            m->AllocMem();
            s->chain.push_back(m);
            prev->next = m;
            prev = m;
        }
    }

    if (kind == SK_PRIMARY) primary = s;
    *out = s;
    return DD_OK;
}

HRESULT DDraw7::DuplicateSurface(LPDIRECTDRAWSURFACE7, LPDIRECTDRAWSURFACE7*) { LogOnce("DuplicateSurface unsupported"); return DDERR_UNSUPPORTED; }

HRESULT DDraw7::EnumDisplayModes(DWORD, LPDDSURFACEDESC2 filter, LPVOID ctx, LPDDENUMMODESCALLBACK2 cb)
{
    if (!cb) return DDERR_INVALIDPARAMS;
    static const DWORD modes[][2] = {
        { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 960 }, { 1280, 1024 }, { 1600, 1200 },
    };
    static const DWORD bpps[] = { 16, 32 };
    for (DWORD b : bpps) {
        for (auto& m : modes) {
            DDSURFACEDESC2 d = {};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
            d.dwWidth = m[0];
            d.dwHeight = m[1];
            d.lPitch = m[0] * b / 8;
            d.dwRefreshRate = 60;
            d.ddpfPixelFormat = b == 16 ? PF_R5G6B5() : PF_X8R8G8B8();
            if (filter) {
                if ((filter->dwFlags & DDSD_WIDTH) && filter->dwWidth != d.dwWidth) continue;
                if ((filter->dwFlags & DDSD_HEIGHT) && filter->dwHeight != d.dwHeight) continue;
                if ((filter->dwFlags & DDSD_PIXELFORMAT) && filter->ddpfPixelFormat.dwRGBBitCount != b) continue;
            }
            if (cb(&d, ctx) == DDENUMRET_CANCEL) return DD_OK;
        }
    }
    return DD_OK;
}

HRESULT DDraw7::EnumSurfaces(DWORD, LPDDSURFACEDESC2, LPVOID, LPDDENUMSURFACESCALLBACK7) { LogOnce("EnumSurfaces unsupported"); return DDERR_UNSUPPORTED; }
HRESULT DDraw7::FlipToGDISurface() { return DD_OK; }

static void FillDDCaps(DDCAPS* c)
{
    DWORD size = c->dwSize;
    if (size < sizeof(DDCAPS_DX5)) size = sizeof(DDCAPS_DX5);
    if (size > sizeof(DDCAPS)) size = sizeof(DDCAPS);
    ZeroMemory(c, size);
    c->dwSize = size;
    c->dwCaps = DDCAPS_3D | DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY |
                DDCAPS_CANBLTSYSMEM | DDCAPS_BLTDEPTHFILL | DDCAPS_GDI | DDCAPS_PALETTE;
    c->dwCaps2 = DDCAPS2_WIDESURFACES | DDCAPS2_CANRENDERWINDOWED | DDCAPS2_NOPAGELOCKREQUIRED |
                 DDCAPS2_FLIPNOVSYNC | DDCAPS2_PRIMARYGAMMA | DDCAPS2_NONLOCALVIDMEM;
    c->dwCKeyCaps = DDCKEYCAPS_SRCBLT;
    c->dwFXCaps = DDFXCAPS_BLTSTRETCHX | DDFXCAPS_BLTSTRETCHY | DDFXCAPS_BLTSHRINKX | DDFXCAPS_BLTSHRINKY;
    c->dwPalCaps = DDPCAPS_8BIT;
    c->dwZBufferBitDepths = DDBD_16 | DDBD_24 | DDBD_32;
    c->dwVidMemTotal = 256 * 1024 * 1024;
    c->dwVidMemFree = 224 * 1024 * 1024;
    c->dwNumFourCCCodes = 0;
    c->ddsOldCaps.dwCaps = DDSCAPS_3DDEVICE | DDSCAPS_BACKBUFFER | DDSCAPS_COMPLEX | DDSCAPS_FLIP |
                           DDSCAPS_FRONTBUFFER | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_PRIMARYSURFACE |
                           DDSCAPS_SYSTEMMEMORY | DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY | DDSCAPS_ZBUFFER |
                           DDSCAPS_MIPMAP | DDSCAPS_LOCALVIDMEM | DDSCAPS_NONLOCALVIDMEM | DDSCAPS_PALETTE;
    if (size >= sizeof(DDCAPS_DX6)) {
        DDCAPS_DX7* c7 = (DDCAPS_DX7*)c;
        c7->ddsCaps.dwCaps = c->ddsOldCaps.dwCaps;
        c7->ddsCaps.dwCaps2 = DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE;
    }
}

HRESULT DDraw7::GetCaps(LPDDCAPS hal, LPDDCAPS hel)
{
    if (hal) FillDDCaps(hal);
    if (hel) FillDDCaps(hel);
    return DD_OK;
}

HRESULT DDraw7::GetDisplayMode(LPDDSURFACEDESC2 d)
{
    if (!d) return DDERR_INVALIDPARAMS;
    DWORD w, h;
    DDPIXELFORMAT pf;
    DisplayFormat(&w, &h, &pf);
    DWORD size = d->dwSize ? d->dwSize : sizeof(DDSURFACEDESC2);
    DDSURFACEDESC2 t = {};
    t.dwSize = size;
    t.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    t.dwWidth = w;
    t.dwHeight = h;
    t.lPitch = w * pf.dwRGBBitCount / 8;
    t.dwRefreshRate = 60;
    t.ddpfPixelFormat = pf;
    memcpy(d, &t, size < sizeof(t) ? size : sizeof(t));
    return DD_OK;
}

HRESULT DDraw7::GetFourCCCodes(LPDWORD n, LPDWORD) { if (n) *n = 0; return DD_OK; }

HRESULT DDraw7::GetGDISurface(LPDIRECTDRAWSURFACE7* s)
{
    if (!s) return DDERR_INVALIDPARAMS;
    *s = primary;
    if (!primary) return DDERR_NOTFOUND;
    primary->AddRef();
    return DD_OK;
}

HRESULT DDraw7::GetMonitorFrequency(LPDWORD f) { if (!f) return DDERR_INVALIDPARAMS; *f = 60; return DD_OK; }

static DWORD FakeScanLine()
{
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    LONGLONG frame = f.QuadPart / 60;
    return (DWORD)((c.QuadPart % frame) * 525 / frame);
}

HRESULT DDraw7::GetScanLine(LPDWORD l)
{
    if (!l) return DDERR_INVALIDPARAMS;
    *l = FakeScanLine();
    if (*l >= 480) return DDERR_VERTICALBLANKINPROGRESS;
    return DD_OK;
}

HRESULT DDraw7::GetVerticalBlankStatus(LPBOOL b) { if (!b) return DDERR_INVALIDPARAMS; *b = FakeScanLine() >= 480; return DD_OK; }
HRESULT DDraw7::Initialize(GUID*) { return DDERR_ALREADYINITIALIZED; }
HRESULT DDraw7::RestoreDisplayMode() { modeSet = false; return DD_OK; }

HRESULT DDraw7::SetCooperativeLevel(HWND h, DWORD flags)
{
    Log("SetCooperativeLevel hwnd=%p flags=%08X", h, flags);
    coop = flags;
    exclusive = (flags & DDSCL_EXCLUSIVE) != 0;
    if (h) {
        hwnd = h;
        Win_Attach(h);
    }
    return DD_OK;
}

HRESULT DDraw7::SetDisplayMode(DWORD w, DWORD h, DWORD bpp, DWORD, DWORD)
{
    Log("SetDisplayMode %ux%ux%u (emulated, desktop mode unchanged)", w, h, bpp);
    modeW = w; modeH = h; modeBpp = bpp ? bpp : 16;
    modeSet = true;
    g_gameW = w;
    g_gameH = h;
    Win_ApplyMode();
    return DD_OK;
}

HRESULT DDraw7::WaitForVerticalBlank(DWORD flags, HANDLE)
{
    if (flags & DDWAITVB_BLOCKBEGINEVENT) return DDERR_UNSUPPORTED;
    R::WaitVBlank();
    return DD_OK;
}

HRESULT DDraw7::GetAvailableVidMem(LPDDSCAPS2, LPDWORD total, LPDWORD freeMem)
{
    if (total) *total = 256 * 1024 * 1024;
    if (freeMem) *freeMem = 224 * 1024 * 1024;
    return DD_OK;
}

HRESULT DDraw7::GetSurfaceFromDC(HDC, LPDIRECTDRAWSURFACE7*) { return DDERR_NOTFOUND; }
HRESULT DDraw7::RestoreAllSurfaces() { return DD_OK; }
HRESULT DDraw7::TestCooperativeLevel() { return DD_OK; }

HRESULT DDraw7::GetDeviceIdentifier(LPDDDEVICEIDENTIFIER2 id, DWORD)
{
    if (!id) return DDERR_INVALIDPARAMS;
    ZeroMemory(id, sizeof(*id));
    lstrcpyA(id->szDriver, "hod2fix.dll");
    lstrcpyA(id->szDescription, "hod2fix Direct3D 9Ex renderer");
    id->liDriverVersion.QuadPart = 0x0006000E00010000LL;
    // {6E5A8F1B-2C3D-4E5F-8A9B-0C1D2E3F4A5B}
    static const GUID g = { 0x6e5a8f1b, 0x2c3d, 0x4e5f, { 0x8a, 0x9b, 0x0c, 0x1d, 0x2e, 0x3f, 0x4a, 0x5b } };
    id->guidDeviceIdentifier = g;
    id->dwWHQLLevel = 1;
    return DD_OK;
}

HRESULT DDraw7::StartModeTest(LPSIZE, DWORD, DWORD) { return DDERR_UNSUPPORTED; }
HRESULT DDraw7::EvaluateMode(DWORD, DWORD*) { return DDERR_UNSUPPORTED; }
