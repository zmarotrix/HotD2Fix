// hod2fix - IDirect3D7 / IDirect3DDevice7 / IDirect3DVertexBuffer7.
#include "dx7.h"
#include <map>
#include <math.h>

// D3D9 numeric values the translation needs (d3d9.h cannot be included here).
enum {
    D9_TS_WORLD = 256,
    D9_RS_DEPTHBIAS = 195,
    D9_RS_SLOPESCALEDEPTHBIAS = 175,
    D9_RS_COLORWRITEENABLE = 168,
    D9_SAMP_ADDRESSU = 1, D9_SAMP_ADDRESSV = 2, D9_SAMP_BORDERCOLOR = 4,
    D9_SAMP_MAGFILTER = 5, D9_SAMP_MINFILTER = 6, D9_SAMP_MIPFILTER = 7,
    D9_SAMP_MIPMAPLODBIAS = 8, D9_SAMP_MAXMIPLEVEL = 9, D9_SAMP_MAXANISOTROPY = 10,
    D9_TEXF_NONE = 0, D9_TEXF_POINT = 1, D9_TEXF_LINEAR = 2, D9_TEXF_ANISOTROPIC = 3,
};

static const int kMaxStages = 8;
static const int kNumRS = 256;
static const int kNumTSS = 32;
static const int kNumXF = 24;   // D3D7 transform state ids 1..23

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------
struct StateBlock {
    // which states are part of the block
    std::vector<DWORD> rs;
    std::vector<std::pair<DWORD, DWORD>> tss;   // (stage, type)
    std::vector<DWORD> xf;
    std::vector<DWORD> lights, lightEn, clip;
    std::vector<DWORD> tex;
    bool viewport = false, material = false;
    // captured values
    DWORD rsV[kNumRS];
    DWORD tssV[kMaxStages][kNumTSS];
    D3DMATRIX xfV[kNumXF];
    D3DVIEWPORT7 vp;
    D3DMATERIAL7 mat;
    std::map<DWORD, D3DLIGHT7> lightV;
    std::map<DWORD, BOOL> lightEnV;
    float clipV[32][4];
    Surface7* texV[kMaxStages];
};

struct Device7 : IDirect3DDevice7 {
    LONG      ref = 1;
    D3D7*     d3d;
    GUID      guid;
    Surface7* target = NULL;

    DWORD rs[kNumRS];
    DWORD tss[kMaxStages][kNumTSS];
    D3DMATRIX xf[kNumXF];
    D3DVIEWPORT7 vp;
    D3DMATERIAL7 mat;
    std::map<DWORD, D3DLIGHT7> lights;
    std::map<DWORD, BOOL> lightOn;
    float clip[32][4];
    Surface7* tex[kMaxStages];
    D3DCLIPSTATUS clipStatus;

    std::vector<StateBlock*> blocks;
    StateBlock* recording = NULL;
    std::vector<BYTE> strided;
    std::vector<BYTE> edged;   // vertex copy with edge-inset texture coordinates
    bool filtersForced = false;
    DWORD addrU[kMaxStages], addrV[kMaxStages];   // address modes the game asked for
    bool clampForced = false;
    bool paddedBound = false;                      // stage 0 uses the colour-padded copy

    Device7(D3D7* d, REFCLSID g, Surface7* rtSurf);
    ~Device7();

    void Defaults();
    void BindTarget();
    void ApplyRS(DWORD s, DWORD v);
    void ApplyTSS(DWORD stage, DWORD t, DWORD v);
    void ApplyTexture(DWORD stage, Surface7* s);
    void ApplyAlphaTest();
    void PrepareFilters(DWORD fvf);
    const void* PrepareDraw(DWORD fvf, const void* v, DWORD n);
    const void* PrepareEdges(DWORD fvf, const void* v, DWORD n);
    void Submit(DWORD prim, DWORD fvf, const void* v, DWORD n, const WORD* idx, DWORD ni);
    void ProbeDraw(DWORD prim, DWORD fvf, const void* v, DWORD n, const WORD* idx, DWORD ni);
    void Capture(StateBlock* b);
    void Apply(StateBlock* b);
    bool Rec() const { return recording != NULL; }

    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv);
    STDMETHOD_(ULONG, AddRef)();
    STDMETHOD_(ULONG, Release)();
    STDMETHOD(GetCaps)(LPD3DDEVICEDESC7);
    STDMETHOD(EnumTextureFormats)(LPD3DENUMPIXELFORMATSCALLBACK, LPVOID);
    STDMETHOD(BeginScene)();
    STDMETHOD(EndScene)();
    STDMETHOD(GetDirect3D)(LPDIRECT3D7*);
    STDMETHOD(SetRenderTarget)(LPDIRECTDRAWSURFACE7, DWORD);
    STDMETHOD(GetRenderTarget)(LPDIRECTDRAWSURFACE7*);
    STDMETHOD(Clear)(DWORD, LPD3DRECT, DWORD, D3DCOLOR, D3DVALUE, DWORD);
    STDMETHOD(SetTransform)(D3DTRANSFORMSTATETYPE, LPD3DMATRIX);
    STDMETHOD(GetTransform)(D3DTRANSFORMSTATETYPE, LPD3DMATRIX);
    STDMETHOD(SetViewport)(LPD3DVIEWPORT7);
    STDMETHOD(MultiplyTransform)(D3DTRANSFORMSTATETYPE, LPD3DMATRIX);
    STDMETHOD(GetViewport)(LPD3DVIEWPORT7);
    STDMETHOD(SetMaterial)(LPD3DMATERIAL7);
    STDMETHOD(GetMaterial)(LPD3DMATERIAL7);
    STDMETHOD(SetLight)(DWORD, LPD3DLIGHT7);
    STDMETHOD(GetLight)(DWORD, LPD3DLIGHT7);
    STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE, DWORD);
    STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE, LPDWORD);
    STDMETHOD(BeginStateBlock)();
    STDMETHOD(EndStateBlock)(LPDWORD);
    STDMETHOD(PreLoad)(LPDIRECTDRAWSURFACE7);
    STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE, DWORD, LPVOID, DWORD, DWORD);
    STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE, DWORD, LPVOID, DWORD, LPWORD, DWORD, DWORD);
    STDMETHOD(SetClipStatus)(LPD3DCLIPSTATUS);
    STDMETHOD(GetClipStatus)(LPD3DCLIPSTATUS);
    STDMETHOD(DrawPrimitiveStrided)(D3DPRIMITIVETYPE, DWORD, LPD3DDRAWPRIMITIVESTRIDEDDATA, DWORD, DWORD);
    STDMETHOD(DrawIndexedPrimitiveStrided)(D3DPRIMITIVETYPE, DWORD, LPD3DDRAWPRIMITIVESTRIDEDDATA, DWORD, LPWORD, DWORD, DWORD);
    STDMETHOD(DrawPrimitiveVB)(D3DPRIMITIVETYPE, LPDIRECT3DVERTEXBUFFER7, DWORD, DWORD, DWORD);
    STDMETHOD(DrawIndexedPrimitiveVB)(D3DPRIMITIVETYPE, LPDIRECT3DVERTEXBUFFER7, DWORD, DWORD, LPWORD, DWORD, DWORD);
    STDMETHOD(ComputeSphereVisibility)(LPD3DVECTOR, LPD3DVALUE, DWORD, DWORD, LPDWORD);
    STDMETHOD(GetTexture)(DWORD, LPDIRECTDRAWSURFACE7*);
    STDMETHOD(SetTexture)(DWORD, LPDIRECTDRAWSURFACE7);
    STDMETHOD(GetTextureStageState)(DWORD, D3DTEXTURESTAGESTATETYPE, LPDWORD);
    STDMETHOD(SetTextureStageState)(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD);
    STDMETHOD(ValidateDevice)(LPDWORD);
    STDMETHOD(ApplyStateBlock)(DWORD);
    STDMETHOD(CaptureStateBlock)(DWORD);
    STDMETHOD(DeleteStateBlock)(DWORD);
    STDMETHOD(CreateStateBlock)(D3DSTATEBLOCKTYPE, LPDWORD);
    STDMETHOD(Load)(LPDIRECTDRAWSURFACE7, LPPOINT, LPDIRECTDRAWSURFACE7, LPRECT, DWORD);
    STDMETHOD(LightEnable)(DWORD, BOOL);
    STDMETHOD(GetLightEnable)(DWORD, BOOL*);
    STDMETHOD(SetClipPlane)(DWORD, D3DVALUE*);
    STDMETHOD(GetClipPlane)(DWORD, D3DVALUE*);
    STDMETHOD(GetInfo)(DWORD, LPVOID, DWORD);
};

static inline DWORD F2DW(float f) { DWORD d; memcpy(&d, &f, 4); return d; }
static inline float DW2F(DWORD d) { float f; memcpy(&f, &d, 4); return f; }

static void Identity(D3DMATRIX& m)
{
    ZeroMemory(&m, sizeof(m));
    m._11 = m._22 = m._33 = m._44 = 1.0f;
}

// r = a * b (row-major 4x4)
static void MatMul(const D3DMATRIX& a, const D3DMATRIX& b, D3DMATRIX& r)
{
    const float* A = (const float*)&a;
    const float* B = (const float*)&b;
    float R[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += A[i * 4 + k] * B[k * 4 + j];
            R[i * 4 + j] = s;
        }
    memcpy(&r, R, sizeof(R));
}

static DWORD XFTo9(DWORD t)
{
    if (t == D3DTRANSFORMSTATE_WORLD) return D9_TS_WORLD;
    if (t >= 4 && t <= 6) return D9_TS_WORLD + (t - 3);   // WORLD1..3
    return t;                                            // VIEW, PROJECTION, TEXTUREn match
}

Device7::Device7(D3D7* d, REFCLSID g, Surface7* rtSurf) : d3d(d), guid(g), target(rtSurf)
{
    if (target) target->AddRef();
    memset(tex, 0, sizeof(tex));
    memset(&clipStatus, 0, sizeof(clipStatus));
    Defaults();
    BindTarget();
}

Device7::~Device7()
{
    for (int i = 0; i < kMaxStages; i++) if (tex[i]) tex[i]->Release();
    for (StateBlock* b : blocks) delete b;
    if (target) target->Release();
}

void Device7::Defaults()
{
    memset(rs, 0, sizeof(rs));
    rs[D3DRENDERSTATE_ZENABLE] = D3DZB_TRUE;
    rs[D3DRENDERSTATE_FILLMODE] = D3DFILL_SOLID;
    rs[D3DRENDERSTATE_SHADEMODE] = D3DSHADE_GOURAUD;
    rs[D3DRENDERSTATE_ZWRITEENABLE] = TRUE;
    rs[D3DRENDERSTATE_LASTPIXEL] = TRUE;
    rs[D3DRENDERSTATE_SRCBLEND] = D3DBLEND_ONE;
    rs[D3DRENDERSTATE_DESTBLEND] = D3DBLEND_ZERO;
    rs[D3DRENDERSTATE_CULLMODE] = D3DCULL_CCW;
    rs[D3DRENDERSTATE_ZFUNC] = D3DCMP_LESSEQUAL;
    rs[D3DRENDERSTATE_ALPHAFUNC] = D3DCMP_ALWAYS;
    rs[D3DRENDERSTATE_FOGDENSITY] = F2DW(1.0f);
    rs[D3DRENDERSTATE_FOGEND] = F2DW(1.0f);
    rs[D3DRENDERSTATE_TEXTUREPERSPECTIVE] = TRUE;
    rs[D3DRENDERSTATE_STENCILFUNC] = D3DCMP_ALWAYS;
    rs[D3DRENDERSTATE_STENCILFAIL] = D3DSTENCILOP_KEEP;
    rs[D3DRENDERSTATE_STENCILZFAIL] = D3DSTENCILOP_KEEP;
    rs[D3DRENDERSTATE_STENCILPASS] = D3DSTENCILOP_KEEP;
    rs[D3DRENDERSTATE_STENCILMASK] = 0xFFFFFFFF;
    rs[D3DRENDERSTATE_STENCILWRITEMASK] = 0xFFFFFFFF;
    rs[D3DRENDERSTATE_TEXTUREFACTOR] = 0xFFFFFFFF;
    rs[D3DRENDERSTATE_CLIPPING] = TRUE;
    rs[D3DRENDERSTATE_LIGHTING] = TRUE;
    rs[D3DRENDERSTATE_COLORVERTEX] = TRUE;
    rs[D3DRENDERSTATE_LOCALVIEWER] = TRUE;
    rs[D3DRENDERSTATE_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
    rs[D3DRENDERSTATE_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
    rs[D3DRENDERSTATE_AMBIENTMATERIALSOURCE] = D3DMCS_COLOR2;
    rs[D3DRENDERSTATE_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
    for (DWORD s = 0; s < kNumRS; s++) ApplyRS(s, rs[s]);

    memset(tss, 0, sizeof(tss));
    for (DWORD i = 0; i < kMaxStages; i++) {
        tss[i][D3DTSS_COLOROP] = i == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        tss[i][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        tss[i][D3DTSS_COLORARG2] = D3DTA_CURRENT;
        tss[i][D3DTSS_ALPHAOP] = i == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
        tss[i][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        tss[i][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        tss[i][D3DTSS_TEXCOORDINDEX] = i;
        tss[i][D3DTSS_ADDRESS] = D3DTADDRESS_WRAP;
        tss[i][D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
        tss[i][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
        tss[i][D3DTSS_MAGFILTER] = D3DTFG_POINT;
        tss[i][D3DTSS_MINFILTER] = D3DTFN_POINT;
        tss[i][D3DTSS_MIPFILTER] = D3DTFP_NONE;
        tss[i][D3DTSS_MAXANISOTROPY] = 1;
        for (DWORD t = 1; t < kNumTSS; t++) ApplyTSS(i, t, tss[i][t]);
    }

    for (int i = 0; i < kNumXF; i++) Identity(xf[i]);
    memset(&mat, 0, sizeof(mat));
    memset(clip, 0, sizeof(clip));
}

void Device7::BindTarget()
{
    if (!target) return;
    Surface7* z = target->ZBuffer();
    R::SetRenderTarget(target->rt, z ? z->ds : NULL);
    vp.dwX = 0;
    vp.dwY = 0;
    vp.dwWidth = target->desc.dwWidth;
    vp.dwHeight = target->desc.dwHeight;
    vp.dvMinZ = 0.0f;
    vp.dvMaxZ = 1.0f;
    R::SetViewport(vp.dwX, vp.dwY, vp.dwWidth, vp.dwHeight, vp.dvMinZ, vp.dvMaxZ);
}

// Render states: almost all D3D7 values match D3D9 numerically.
void Device7::ApplyRS(DWORD s, DWORD v)
{
    switch (s) {
    case D3DRENDERSTATE_ZENABLE: case D3DRENDERSTATE_FILLMODE: case D3DRENDERSTATE_SHADEMODE:
    case D3DRENDERSTATE_ZWRITEENABLE: case D3DRENDERSTATE_LASTPIXEL:
    case D3DRENDERSTATE_SRCBLEND: case D3DRENDERSTATE_DESTBLEND: case D3DRENDERSTATE_CULLMODE:
    case D3DRENDERSTATE_ZFUNC: case D3DRENDERSTATE_DITHERENABLE: case D3DRENDERSTATE_ALPHABLENDENABLE:
    case D3DRENDERSTATE_FOGENABLE: case D3DRENDERSTATE_SPECULARENABLE: case D3DRENDERSTATE_FOGCOLOR:
    case D3DRENDERSTATE_FOGTABLEMODE: case D3DRENDERSTATE_FOGSTART: case D3DRENDERSTATE_FOGEND:
    case D3DRENDERSTATE_FOGDENSITY: case D3DRENDERSTATE_RANGEFOGENABLE:
    case D3DRENDERSTATE_STENCILENABLE: case D3DRENDERSTATE_STENCILFAIL: case D3DRENDERSTATE_STENCILZFAIL:
    case D3DRENDERSTATE_STENCILPASS: case D3DRENDERSTATE_STENCILFUNC: case D3DRENDERSTATE_STENCILREF:
    case D3DRENDERSTATE_STENCILMASK: case D3DRENDERSTATE_STENCILWRITEMASK: case D3DRENDERSTATE_TEXTUREFACTOR:
    case D3DRENDERSTATE_CLIPPING: case D3DRENDERSTATE_LIGHTING: case D3DRENDERSTATE_AMBIENT:
    case D3DRENDERSTATE_FOGVERTEXMODE: case D3DRENDERSTATE_COLORVERTEX: case D3DRENDERSTATE_LOCALVIEWER:
    case D3DRENDERSTATE_NORMALIZENORMALS: case D3DRENDERSTATE_DIFFUSEMATERIALSOURCE:
    case D3DRENDERSTATE_SPECULARMATERIALSOURCE: case D3DRENDERSTATE_AMBIENTMATERIALSOURCE:
    case D3DRENDERSTATE_EMISSIVEMATERIALSOURCE: case D3DRENDERSTATE_VERTEXBLEND:
    case D3DRENDERSTATE_CLIPPLANEENABLE:
        if (s == D3DRENDERSTATE_ZENABLE && v > 1) LogOnce("game asks for W-buffering");
        R::SetRS(s, v);
        break;
    case D3DRENDERSTATE_ALPHATESTENABLE: case D3DRENDERSTATE_ALPHAREF: case D3DRENDERSTATE_ALPHAFUNC:
    case D3DRENDERSTATE_COLORKEYENABLE:
        ApplyAlphaTest();
        break;
    default:
        if (s >= D3DRENDERSTATE_WRAP0 && s <= D3DRENDERSTATE_WRAP7) { R::SetRS(s, v); break; }
        if (s == D3DRENDERSTATE_ZBIAS) {
            char b[40];
            wsprintfA(b, "game uses ZBIAS %u", v);
            LogOnce(b);
            // D3D7 integer bias (0..16) -> D3D9 float depth bias
            R::SetRS(D9_RS_DEPTHBIAS, F2DW(-(float)v * 0.000004f));
            R::SetRS(D9_RS_SLOPESCALEDEPTHBIAS, F2DW(v ? -(float)v * 0.25f : 0.0f));
            break;
        }
        if (s == 44 || s == 45) {   // legacy TEXTUREADDRESSU / V
            R::SetSampler(0, s == 44 ? D9_SAMP_ADDRESSU : D9_SAMP_ADDRESSV, v);
            break;
        }
        // TEXTUREPERSPECTIVE, ANTIALIAS, LINEPATTERN, ZVISIBLE, STIPPLEDALPHA,
        // EDGEANTIALIAS, EXTENTS, COLORKEYBLENDENABLE: no D3D9 equivalent, safe to ignore.
        break;
    }
}

// D3D9 has no colour keying.  Keyed texels are uploaded with alpha 0, and
// alpha testing is forced on while COLORKEYENABLE is set and a keyed texture
// is bound.
void Device7::ApplyAlphaTest()
{
    Surface7* t = tex[0];
    bool ck = rs[D3DRENDERSTATE_COLORKEYENABLE] && t && t->root->hasCK;
    if (ck && !rs[D3DRENDERSTATE_ALPHATESTENABLE]) {
        R::SetRS(D3DRENDERSTATE_ALPHATESTENABLE, TRUE);
        R::SetRS(D3DRENDERSTATE_ALPHAFUNC, D3DCMP_GREATER);
        R::SetRS(D3DRENDERSTATE_ALPHAREF, 0);
    } else {
        R::SetRS(D3DRENDERSTATE_ALPHATESTENABLE, rs[D3DRENDERSTATE_ALPHATESTENABLE]);
        R::SetRS(D3DRENDERSTATE_ALPHAFUNC, rs[D3DRENDERSTATE_ALPHAFUNC] ? rs[D3DRENDERSTATE_ALPHAFUNC] : D3DCMP_ALWAYS);
        R::SetRS(D3DRENDERSTATE_ALPHAREF, rs[D3DRENDERSTATE_ALPHAREF] & 0xFF);
    }
}

static DWORD MagTo9(DWORD v)
{
    switch (v) {
    case D3DTFG_POINT: return D9_TEXF_POINT;
    case D3DTFG_ANISOTROPIC: return D9_TEXF_ANISOTROPIC;
    case D3DTFG_LINEAR: case D3DTFG_FLATCUBIC: case D3DTFG_GAUSSIANCUBIC: return D9_TEXF_LINEAR;
    }
    return D9_TEXF_POINT;
}
static DWORD MinTo9(DWORD v)
{
    switch (v) {
    case D3DTFN_POINT: return D9_TEXF_POINT;
    case D3DTFN_LINEAR: return D9_TEXF_LINEAR;
    case D3DTFN_ANISOTROPIC: return D9_TEXF_ANISOTROPIC;
    }
    return D9_TEXF_POINT;
}
static DWORD MipTo9(DWORD v)
{
    switch (v) {
    case D3DTFP_POINT: return D9_TEXF_POINT;
    case D3DTFP_LINEAR: return D9_TEXF_LINEAR;
    }
    return D9_TEXF_NONE;
}

void Device7::ApplyTSS(DWORD st, DWORD t, DWORD v)
{
    switch (t) {
    case D3DTSS_ADDRESS:
        addrU[st] = addrV[st] = v;
        if (st == 0 && clampForced) break;
        R::SetSampler(st, D9_SAMP_ADDRESSU, v);
        R::SetSampler(st, D9_SAMP_ADDRESSV, v);
        break;
    case D3DTSS_ADDRESSU:
        addrU[st] = v;
        if (!(st == 0 && clampForced)) R::SetSampler(st, D9_SAMP_ADDRESSU, v);
        break;
    case D3DTSS_ADDRESSV:
        addrV[st] = v;
        if (!(st == 0 && clampForced)) R::SetSampler(st, D9_SAMP_ADDRESSV, v);
        break;
    case D3DTSS_BORDERCOLOR:   R::SetSampler(st, D9_SAMP_BORDERCOLOR, v); break;
    case D3DTSS_MAGFILTER:     if (!filtersForced) R::SetSampler(st, D9_SAMP_MAGFILTER, MagTo9(v)); break;
    case D3DTSS_MINFILTER:     if (!filtersForced) R::SetSampler(st, D9_SAMP_MINFILTER, MinTo9(v)); break;
    case D3DTSS_MIPFILTER:     R::SetSampler(st, D9_SAMP_MIPFILTER, MipTo9(v)); break;
    case D3DTSS_MIPMAPLODBIAS: R::SetSampler(st, D9_SAMP_MIPMAPLODBIAS, v); break;
    case D3DTSS_MAXMIPLEVEL:   R::SetSampler(st, D9_SAMP_MAXMIPLEVEL, v); break;
    case D3DTSS_MAXANISOTROPY: R::SetSampler(st, D9_SAMP_MAXANISOTROPY, v ? v : 1); break;
    default:
        if (t >= 1 && t <= 24) R::SetTSS(st, t, v);   // ops, args, bumpenv, texcoordindex, ttff
        break;
    }
}

// The PC port asks for point sampling on most 3D geometry.  Optionally use
// bilinear for untransformed (3D) draws while pre-transformed 2D/HUD draws
// keep the filtering the game requested.
void Device7::PrepareFilters(DWORD fvf)
{
    bool want = g_cfg.forceLinear && (fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW;
    if (want == filtersForced) return;
    filtersForced = want;
    for (DWORD st = 0; st < 2; st++) {
        if (want) {
            R::SetSampler(st, D9_SAMP_MAGFILTER, D9_TEXF_LINEAR);
            R::SetSampler(st, D9_SAMP_MINFILTER, D9_TEXF_LINEAR);
        } else {
            R::SetSampler(st, D9_SAMP_MAGFILTER, MagTo9(tss[st][D3DTSS_MAGFILTER]));
            R::SetSampler(st, D9_SAMP_MINFILTER, MinTo9(tss[st][D3DTSS_MINFILTER]));
        }
    }
}

// Sprites whose image reaches the texture edge (e.g. the domed tower behind
// the first boss) are drawn with wrapping on.  At a coordinate of exactly 1.0
// the outermost sliver of the sprite wraps to the opposite edge of the image;
// at 640x480 that sliver rarely covers a pixel, at higher resolutions it shows
// as a thin dashed line.  Bilinear filtering also reads half a texel past the
// region a draw uses, pulling in neighbouring images.  For draws whose
// coordinates stay within one copy of the texture (so it isn't tiling):
//  * stage 0 is clamped instead of wrapped (all draws), and
//  * with smoothing, coordinates are pulled in by half a texel at the edges of
//    the draw's own texture region, so sampling never crosses into a neighbour.
const void* Device7::PrepareEdges(DWORD fvf, const void* v, DWORD n)
{
    bool inRange = false;
    UINT off = 0, stride = 0;
    float umin = 2, umax = -2, vmin = 2, vmax = -2;
    if (g_cfg.edgeInset && v && n && tex[0] && !tss[0][D3DTSS_TEXTURETRANSFORMFLAGS]) {
        DWORD pos = fvf & D3DFVF_POSITION_MASK;
        off = pos == D3DFVF_XYZRHW ? 16 : pos == D3DFVF_XYZ ? 12 : 12 + 4 * ((pos - D3DFVF_XYZB1) / 2 + 1);
        if (fvf & D3DFVF_NORMAL) off += 12;
        if (fvf & 0x20) off += 4;
        if (fvf & D3DFVF_DIFFUSE) off += 4;
        if (fvf & D3DFVF_SPECULAR) off += 4;
        UINT ntex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
        UINT set = tss[0][D3DTSS_TEXCOORDINDEX] & 0xFFFF;
        if (set < ntex && !(tss[0][D3DTSS_TEXCOORDINDEX] & 0xFFFF0000)) {
            for (UINT i = 0; i < set; i++) {
                UINT fmt = (fvf >> (16 + i * 2)) & 3;
                off += fmt == 0 ? 8 : fmt == 1 ? 12 : fmt == 2 ? 16 : 4;
            }
            UINT fmt = (fvf >> (16 + set * 2)) & 3;
            if (fmt != 3) {   // at least two coordinates
                stride = R::FVFStride(fvf);
                const BYTE* p = (const BYTE*)v + off;
                for (DWORD i = 0; i < n; i++, p += stride) {
                    const float* uv = (const float*)p;
                    if (uv[0] < umin) umin = uv[0];
                    if (uv[0] > umax) umax = uv[0];
                    if (uv[1] < vmin) vmin = uv[1];
                    if (uv[1] > vmax) vmax = uv[1];
                }
                inRange = true;
            }
        }
    }

    // "One copy" may sit at any integer offset: the dome tower uses v in
    // [-1, 0].  ku/kv is that offset; with wrapping, shifting by it is a no-op.
    const float eps = 0.03f;
    float ku = floorf(umin + eps), kv = floorf(vmin + eps);
    if (inRange && (umax > ku + 1 + eps || vmax > kv + 1 + eps)) inRange = false;   // tiling

    bool clamp = inRange && addrU[0] == D3DTADDRESS_WRAP && addrV[0] == D3DTADDRESS_WRAP;
    if (clamp != clampForced) {
        clampForced = clamp;
        R::SetSampler(0, D9_SAMP_ADDRESSU, clamp ? D3DTADDRESS_CLAMP : addrU[0]);
        R::SetSampler(0, D9_SAMP_ADDRESSV, clamp ? D3DTADDRESS_CLAMP : addrV[0]);
    }
    if (!inRange) return v;
    if (!clamp) ku = kv = 0;   // other address modes give offsets a meaning of their own

    bool inset = filtersForced;
    Surface7* t = tex[0]->root;
    float hu = 0.5f / (t->desc.dwWidth ? t->desc.dwWidth : 1);
    float hv = 0.5f / (t->desc.dwHeight ? t->desc.dwHeight : 1);
    bool doU = inset && umax - umin > 4 * hu, doV = inset && vmax - vmin > 4 * hv;   // leave 1-2 texel strips alone
    if (!doU && !doV && ku == 0 && kv == 0) return v;
    float u0 = umin - ku + hu, u1 = umax - ku - hu, v0 = vmin - kv + hv, v1 = vmax - kv - hv;
    edged.resize((size_t)stride * n);
    memcpy(edged.data(), v, edged.size());
    BYTE* p = edged.data() + off;
    for (DWORD i = 0; i < n; i++, p += stride) {
        float* uv = (float*)p;
        uv[0] -= ku;
        uv[1] -= kv;
        if (doU) uv[0] = uv[0] < u0 ? u0 : uv[0] > u1 ? u1 : uv[0];
        if (doV) uv[1] = uv[1] < v0 ? v0 : uv[1] > v1 ? v1 : uv[1];
    }
    return edged.data();
}

// Per-draw fixups applied on top of the game's state.
const void* Device7::PrepareDraw(DWORD fvf, const void* v, DWORD n)
{
    PrepareFilters(fvf);
    v = PrepareEdges(fvf, v, n);

    // Smoothed draws that rely on transparency sample the colour-padded copy
    // so edges don't pick up the black stored in transparent texels.
    Surface7* t = tex[0] && !tex[0]->rt ? tex[0]->root : NULL;
    bool usesAlpha = rs[D3DRENDERSTATE_ALPHATESTENABLE] || rs[D3DRENDERSTATE_ALPHABLENDENABLE] ||
                     rs[D3DRENDERSTATE_COLORKEYENABLE];
    bool pad = filtersForced && t && t->texPadded && usesAlpha;
    if (pad != paddedBound) {
        paddedBound = pad;
        R::SetTexture(0, pad ? t->texPadded : (t ? t->tex : NULL));
    }
    if (g_cfg.logLevel >= 2 && rs[D3DRENDERSTATE_ALPHABLENDENABLE]) {
        char b[160];
        wsprintfA(b, "blend draw fvf=%X src=%u dst=%u zen=%u zwrite=%u atest=%u afunc=%u aref=%u ck=%u tex=%ubpp a=%X",
                  fvf, rs[D3DRENDERSTATE_SRCBLEND], rs[D3DRENDERSTATE_DESTBLEND], rs[D3DRENDERSTATE_ZENABLE],
                  rs[D3DRENDERSTATE_ZWRITEENABLE], rs[D3DRENDERSTATE_ALPHATESTENABLE], rs[D3DRENDERSTATE_ALPHAFUNC],
                  rs[D3DRENDERSTATE_ALPHAREF], rs[D3DRENDERSTATE_COLORKEYENABLE],
                  tex[0] ? tex[0]->conv.bpp : 0, tex[0] ? tex[0]->conv.mask[3] : 0);
        LogOnce(b);
    }
    return v;
}

// Transparent effects (fire, smoke, sparks) are drawn with depth writes on, so
// their nearly invisible edge texels still occlude anything drawn behind them
// later: the "halo" around effects.  With FixAlphaHalos, and only for draws
// that really are soft effects (models and translucent characters are drawn
// blended too, and need their own depth writes to hide their back parts):
//  * additive effects don't write depth at all (their result is order independent);
//  * effects with a softly transparent texture are drawn in colour without
//    depth writes, then again depth-only for texels that are at least
//    HaloAlphaRef opaque, so solid parts still occlude correctly.
void Device7::Submit(DWORD prim, DWORD fvf, const void* v, DWORD n, const WORD* idx, DWORD ni)
{
    bool blended = rs[D3DRENDERSTATE_ALPHABLENDENABLE] && rs[D3DRENDERSTATE_DESTBLEND] != D3DBLEND_ZERO;
    bool additive = rs[D3DRENDERSTATE_DESTBLEND] == D3DBLEND_ONE;
    Surface7* t = tex[0] && !tex[0]->rt ? tex[0]->root : NULL;
    bool effect = additive || (t && t->softAlpha);
    if (!g_cfg.fixHalos || !blended || !effect || !rs[D3DRENDERSTATE_ZENABLE] || !rs[D3DRENDERSTATE_ZWRITEENABLE]) {
        R::Draw(prim, fvf, v, n, idx, ni);
        return;
    }
    R::SetRS(D3DRENDERSTATE_ZWRITEENABLE, FALSE);
    R::Draw(prim, fvf, v, n, idx, ni);

    if (!additive && g_cfg.haloAlphaRef <= 255) {
        DWORD ref = rs[D3DRENDERSTATE_ALPHATESTENABLE] ? (rs[D3DRENDERSTATE_ALPHAREF] & 0xFF) : 0;
        if (ref < (DWORD)g_cfg.haloAlphaRef) ref = g_cfg.haloAlphaRef;
        R::SetRS(D9_RS_COLORWRITEENABLE, 0);
        R::SetRS(D3DRENDERSTATE_ZWRITEENABLE, TRUE);
        R::SetRS(D3DRENDERSTATE_ALPHATESTENABLE, TRUE);
        R::SetRS(D3DRENDERSTATE_ALPHAFUNC, D3DCMP_GREATEREQUAL);
        R::SetRS(D3DRENDERSTATE_ALPHAREF, ref);
        R::Draw(prim, fvf, v, n, idx, ni);
        R::SetRS(D9_RS_COLORWRITEENABLE, 0xF);
        ApplyAlphaTest();
    }
    R::SetRS(D3DRENDERSTATE_ZWRITEENABLE, TRUE);
}

// Debug (F8): logs every draw in the current frame that covers the probe
// point, with the texture coordinates sampled there and the relevant state.
void Device7::ProbeDraw(DWORD prim, DWORD fvf, const void* v, DWORD n, const WORD* idx, DWORD ni)
{
    static LONG drawNo;
    drawNo++;
    if (!v || !n || (prim != D3DPT_TRIANGLELIST && prim != D3DPT_TRIANGLESTRIP && prim != D3DPT_TRIANGLEFAN)) return;
    UINT stride = R::FVFStride(fvf);
    DWORD pos = fvf & D3DFVF_POSITION_MASK;
    bool rhw = pos == D3DFVF_XYZRHW;

    D3DMATRIX wv, wvp;
    MatMul(xf[D3DTRANSFORMSTATE_WORLD], xf[D3DTRANSFORMSTATE_VIEW], wv);
    MatMul(wv, xf[D3DTRANSFORMSTATE_PROJECTION], wvp);
    std::vector<float> sx(n), sy(n), sw(n);
    for (DWORD i = 0; i < n; i++) {
        const float* p = (const float*)((const BYTE*)v + (size_t)i * stride);
        if (rhw) { sx[i] = p[0]; sy[i] = p[1]; sw[i] = p[3] > 0 ? p[3] : 1.0f; continue; }
        float cx = p[0] * wvp._11 + p[1] * wvp._21 + p[2] * wvp._31 + wvp._41;
        float cy = p[0] * wvp._12 + p[1] * wvp._22 + p[2] * wvp._32 + wvp._42;
        float cw = p[0] * wvp._14 + p[1] * wvp._24 + p[2] * wvp._34 + wvp._44;
        if (cw <= 1e-6f) { sw[i] = -1; continue; }
        float r = 1.0f / cw;
        sx[i] = vp.dwX + (1.0f + cx * r) * 0.5f * vp.dwWidth;
        sy[i] = vp.dwY + (1.0f - cy * r) * 0.5f * vp.dwHeight;
        sw[i] = r;
    }

    // texture coordinates of stage 0
    int uvOff = -1;
    {
        UINT off = rhw ? 16 : pos == D3DFVF_XYZ ? 12 : 12 + 4 * ((pos - D3DFVF_XYZB1) / 2 + 1);
        if (fvf & D3DFVF_NORMAL) off += 12;
        if (fvf & 0x20) off += 4;
        if (fvf & D3DFVF_DIFFUSE) off += 4;
        if (fvf & D3DFVF_SPECULAR) off += 4;
        UINT ntex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
        UINT set = tss[0][D3DTSS_TEXCOORDINDEX] & 0xFFFF;
        if (set < ntex) {
            for (UINT i = 0; i < set; i++) {
                UINT fmt = (fvf >> (16 + i * 2)) & 3;
                off += fmt == 0 ? 8 : fmt == 1 ? 12 : fmt == 2 ? 16 : 4;
            }
            uvOff = (int)off;
        }
    }
    auto UV = [&](DWORD i, int c) -> float {
        return uvOff < 0 ? 0.0f : ((const float*)((const BYTE*)v + (size_t)i * stride + uvOff))[c];
    };

    float px = g_probeX, py = g_probeY;
    bool hit = false;
    float hu = 0, hv = 0;
    DWORD hitTri[3] = {};
    auto tri = [&](DWORD a, DWORD b, DWORD c) {
        if (hit || a >= n || b >= n || c >= n || sw[a] <= 0 || sw[b] <= 0 || sw[c] <= 0) return;
        float d = (sy[b] - sy[c]) * (sx[a] - sx[c]) + (sx[c] - sx[b]) * (sy[a] - sy[c]);
        if (fabsf(d) < 1e-9f) return;
        float l0 = ((sy[b] - sy[c]) * (px - sx[c]) + (sx[c] - sx[b]) * (py - sy[c])) / d;
        float l1 = ((sy[c] - sy[a]) * (px - sx[c]) + (sx[a] - sx[c]) * (py - sy[c])) / d;
        float l2 = 1.0f - l0 - l1;
        if (l0 < 0 || l1 < 0 || l2 < 0) return;
        // perspective-correct interpolation
        float w0 = l0 * sw[a], w1 = l1 * sw[b], w2 = l2 * sw[c], ws = w0 + w1 + w2;
        if (ws <= 0) return;
        hu = (w0 * UV(a, 0) + w1 * UV(b, 0) + w2 * UV(c, 0)) / ws;
        hv = (w0 * UV(a, 1) + w1 * UV(b, 1) + w2 * UV(c, 1)) / ws;
        hitTri[0] = a; hitTri[1] = b; hitTri[2] = c;
        hit = true;
    };
    DWORD cnt = idx ? ni : n;
    auto I = [&](DWORD k) -> DWORD { return idx ? idx[k] : k; };
    if (prim == D3DPT_TRIANGLELIST) for (DWORD k = 0; k + 2 < cnt; k += 3) tri(I(k), I(k + 1), I(k + 2));
    else if (prim == D3DPT_TRIANGLESTRIP) for (DWORD k = 0; k + 2 < cnt; k++) tri(I(k), I(k + 1), I(k + 2));
    else for (DWORD k = 1; k + 1 < cnt; k++) tri(I(0), I(k), I(k + 1));
    if (!hit) return;

    InterlockedIncrement(&g_probeHits);
    Surface7* t = tex[0] ? tex[0]->root : NULL;
    float umin = 9, umax = -9, vmin = 9, vmax = -9;
    for (DWORD i = 0; i < n && uvOff >= 0; i++) {
        float u = UV(i, 0), w = UV(i, 1);
        if (u < umin) umin = u;
        if (u > umax) umax = u;
        if (w < vmin) vmin = w;
        if (w > vmax) vmax = w;
    }
    DWORD tw = t ? t->desc.dwWidth : 0, th = t ? t->desc.dwHeight : 0;
    Log("probe #%ld fvf=%X prim=%u verts=%u%s tex=%ux%u_%08X%s uv=(%.5f,%.5f) texel=(%.2f,%.2f) "
        "uvrange u[%.4f..%.4f] v[%.4f..%.4f] addr=%u/%u%s filt=%u/%u%s pad=%d",
        drawNo, fvf, prim, n, idx ? " indexed" : "", tw, th, t ? t->hash : 0,
        (t && t->hasCK) ? (t->softAlpha ? " ck soft" : " ck") : (t && t->softAlpha) ? " soft" : "",
        hu, hv, hu * tw, hv * th, umin, umax, vmin, vmax, addrU[0], addrV[0], clampForced ? " (clamped)" : "",
        tss[0][D3DTSS_MAGFILTER], tss[0][D3DTSS_MINFILTER], filtersForced ? " (smoothed)" : "", (int)paddedBound);
    Log("        blend=%u src=%u dst=%u atest=%u afunc=%u aref=%u ck=%u zen=%u zwrite=%u zfunc=%u cull=%u fog=%u "
        "colorop=%u/%u,%u alphaop=%u/%u,%u tfactor=%08X tri=%u,%u,%u",
        rs[D3DRENDERSTATE_ALPHABLENDENABLE], rs[D3DRENDERSTATE_SRCBLEND], rs[D3DRENDERSTATE_DESTBLEND],
        rs[D3DRENDERSTATE_ALPHATESTENABLE], rs[D3DRENDERSTATE_ALPHAFUNC], rs[D3DRENDERSTATE_ALPHAREF],
        rs[D3DRENDERSTATE_COLORKEYENABLE], rs[D3DRENDERSTATE_ZENABLE], rs[D3DRENDERSTATE_ZWRITEENABLE],
        rs[D3DRENDERSTATE_ZFUNC], rs[D3DRENDERSTATE_CULLMODE], rs[D3DRENDERSTATE_FOGENABLE],
        tss[0][D3DTSS_COLOROP], tss[0][D3DTSS_COLORARG1], tss[0][D3DTSS_COLORARG2],
        tss[0][D3DTSS_ALPHAOP], tss[0][D3DTSS_ALPHAARG1], tss[0][D3DTSS_ALPHAARG2],
        rs[D3DRENDERSTATE_TEXTUREFACTOR], hitTri[0], hitTri[1], hitTri[2]);
    for (int k = 0; k < 3; k++) {
        DWORD i = hitTri[k];
        const float* p = (const float*)((const BYTE*)v + (size_t)i * stride);
        Log("        v%u pos=(%.3f,%.3f,%.3f%s%.4f) screen=(%.2f,%.2f) uv=(%.5f,%.5f)", i, p[0], p[1], p[2],
            rhw ? ", rhw " : ", w ", rhw ? p[3] : (sw[i] > 0 ? 1.0f / sw[i] : 0.0f), sx[i], sy[i], UV(i, 0), UV(i, 1));
    }
}

void Device7::ApplyTexture(DWORD stage, Surface7* s)
{
    if (stage == 0) paddedBound = false;
    if (!s) { R::SetTexture(stage, NULL); return; }
    if (s->rt) { R::SetTextureTarget(stage, s->rt); return; }
    s->UploadIfDirty();
    R::SetTexture(stage, s->root->tex);
    if (stage == 0) paddedBound = false;
}

// --- IUnknown --------------------------------------------------------------
HRESULT Device7::QueryInterface(REFIID riid, LPVOID* ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3DDevice7) { *ppv = this; AddRef(); return S_OK; }
    *ppv = NULL;
    return E_NOINTERFACE;
}
ULONG Device7::AddRef() { return InterlockedIncrement(&ref); }
ULONG Device7::Release() { LONG r = InterlockedDecrement(&ref); if (r <= 0) { delete this; return 0; } return r; }

HRESULT Device7::GetCaps(LPD3DDEVICEDESC7 d)
{
    if (!d) return DDERR_INVALIDPARAMS;
    DeviceDescFill(d, guid, guid != IID_IDirect3DRGBDevice, guid == IID_IDirect3DTnLHalDevice);
    return D3D_OK;
}

static const DDPIXELFORMAT* TextureFormats(int* n)
{
    static DDPIXELFORMAT f[6];
    static bool init = false;
    if (!init) {
        f[0] = MakePF(16, 0xF800, 0x07E0, 0x001F, 0);          // R5G6B5
        f[1] = MakePF(16, 0x7C00, 0x03E0, 0x001F, 0x8000);     // A1R5G5B5
        f[2] = MakePF(16, 0x0F00, 0x00F0, 0x000F, 0xF000);     // A4R4G4B4
        f[3] = MakePF(16, 0x7C00, 0x03E0, 0x001F, 0);          // X1R5G5B5
        f[4] = MakePF(32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000); // A8R8G8B8
        f[5] = MakePF(32, 0xFF0000, 0xFF00, 0xFF, 0);          // X8R8G8B8
        init = true;
    }
    *n = 6;
    return f;
}

HRESULT Device7::EnumTextureFormats(LPD3DENUMPIXELFORMATSCALLBACK cb, LPVOID ctx)
{
    if (!cb) return DDERR_INVALIDPARAMS;
    int n;
    const DDPIXELFORMAT* f = TextureFormats(&n);
    for (int i = 0; i < n; i++) {
        DDPIXELFORMAT pf = f[i];
        if (cb(&pf, ctx) == D3DENUMRET_CANCEL) break;
    }
    return D3D_OK;
}

HRESULT Device7::BeginScene() { R::BeginScene(); return D3D_OK; }
HRESULT Device7::EndScene() { R::EndScene(); return D3D_OK; }

HRESULT Device7::GetDirect3D(LPDIRECT3D7* out)
{
    if (!out) return DDERR_INVALIDPARAMS;
    d3d->AddRef();
    *out = d3d;
    return D3D_OK;
}

HRESULT Device7::SetRenderTarget(LPDIRECTDRAWSURFACE7 s, DWORD)
{
    Surface7* t = (Surface7*)s;
    if (!t || !t->rt) return DDERR_INVALIDPARAMS;
    t->AddRef();
    if (target) target->Release();
    target = t;
    BindTarget();
    return D3D_OK;
}

HRESULT Device7::GetRenderTarget(LPDIRECTDRAWSURFACE7* out)
{
    if (!out) return DDERR_INVALIDPARAMS;
    *out = target;
    if (target) target->AddRef();
    return D3D_OK;
}

HRESULT Device7::Clear(DWORD count, LPD3DRECT rects, DWORD flags, D3DCOLOR color, D3DVALUE z, DWORD stencil)
{
    R::Clear(count, (const RECT*)rects, flags, color, z, stencil);   // D3DRECT and RECT share a layout
    return D3D_OK;
}

HRESULT Device7::SetTransform(D3DTRANSFORMSTATETYPE t, LPD3DMATRIX m)
{
    if (!m || (DWORD)t >= kNumXF) return DDERR_INVALIDPARAMS;
    if (Rec()) { recording->xf.push_back(t); recording->xfV[t] = *m; return D3D_OK; }
    xf[t] = *m;
    R::SetTransform(XFTo9(t), (const float*)m);
    if (t == D3DTRANSFORMSTATE_PROJECTION && m->_34 != 0 && m->_33 != 0 && m->_33 != 1) {
        // depth range, for diagnosing precision problems
        char b[96];
        int n = (int)(-m->_43 / m->_33 * 1000), f = (int)(-m->_43 / (m->_33 - 1));
        wsprintfA(b, "projection near=%d.%03d far=%d", n / 1000, n % 1000, f);
        LogOnce(b);
    }
    return D3D_OK;
}

HRESULT Device7::GetTransform(D3DTRANSFORMSTATETYPE t, LPD3DMATRIX m)
{
    if (!m || (DWORD)t >= kNumXF) return DDERR_INVALIDPARAMS;
    *m = xf[t];
    return D3D_OK;
}

HRESULT Device7::MultiplyTransform(D3DTRANSFORMSTATETYPE t, LPD3DMATRIX m)
{
    if (!m || (DWORD)t >= kNumXF) return DDERR_INVALIDPARAMS;
    D3DMATRIX r;
    MatMul(*m, xf[t], r);
    return SetTransform(t, &r);
}

HRESULT Device7::SetViewport(LPD3DVIEWPORT7 v)
{
    if (!v) return DDERR_INVALIDPARAMS;
    if (Rec()) { recording->viewport = true; recording->vp = *v; return D3D_OK; }
    vp = *v;
    R::SetViewport(v->dwX, v->dwY, v->dwWidth, v->dwHeight, v->dvMinZ, v->dvMaxZ);
    return D3D_OK;
}

HRESULT Device7::GetViewport(LPD3DVIEWPORT7 v) { if (!v) return DDERR_INVALIDPARAMS; *v = vp; return D3D_OK; }

HRESULT Device7::SetMaterial(LPD3DMATERIAL7 m)
{
    if (!m) return DDERR_INVALIDPARAMS;
    if (Rec()) { recording->material = true; recording->mat = *m; return D3D_OK; }
    mat = *m;
    R::SetMaterial(m);
    return D3D_OK;
}

HRESULT Device7::GetMaterial(LPD3DMATERIAL7 m) { if (!m) return DDERR_INVALIDPARAMS; *m = mat; return D3D_OK; }

HRESULT Device7::SetLight(DWORD i, LPD3DLIGHT7 l)
{
    if (!l) return DDERR_INVALIDPARAMS;
    if (Rec()) { recording->lights.push_back(i); recording->lightV[i] = *l; return D3D_OK; }
    lights[i] = *l;
    R::SetLight(i, l);
    return D3D_OK;
}

HRESULT Device7::GetLight(DWORD i, LPD3DLIGHT7 l)
{
    if (!l) return DDERR_INVALIDPARAMS;
    auto it = lights.find(i);
    if (it == lights.end()) return DDERR_INVALIDPARAMS;
    *l = it->second;
    return D3D_OK;
}

HRESULT Device7::SetRenderState(D3DRENDERSTATETYPE s, DWORD v)
{
    if ((DWORD)s >= kNumRS) return D3D_OK;
    if (Rec()) { recording->rs.push_back(s); recording->rsV[s] = v; return D3D_OK; }
    rs[s] = v;
    if (g_cfg.logLevel >= 2 && s != D3DRENDERSTATE_FOGCOLOR && s != D3DRENDERSTATE_TEXTUREFACTOR &&
        s != D3DRENDERSTATE_AMBIENT && s != D3DRENDERSTATE_FOGSTART && s != D3DRENDERSTATE_FOGEND) {
        char b[64];
        wsprintfA(b, "RS %u = %u", (DWORD)s, v);
        LogOnce(b);
    }
    ApplyRS(s, v);
    return D3D_OK;
}

HRESULT Device7::GetRenderState(D3DRENDERSTATETYPE s, LPDWORD v)
{
    if (!v) return DDERR_INVALIDPARAMS;
    *v = (DWORD)s < kNumRS ? rs[s] : 0;
    return D3D_OK;
}

// --- state blocks ------------------------------------------------------------
HRESULT Device7::BeginStateBlock()
{
    if (recording) return D3DERR_INBEGINSTATEBLOCK;
    recording = new StateBlock();
    return D3D_OK;
}

HRESULT Device7::EndStateBlock(LPDWORD h)
{
    if (!recording) return D3DERR_NOTINBEGINSTATEBLOCK;
    if (!h) return DDERR_INVALIDPARAMS;
    blocks.push_back(recording);
    *h = (DWORD)blocks.size();
    recording = NULL;
    return D3D_OK;
}

void Device7::Capture(StateBlock* b)
{
    for (DWORD s : b->rs) b->rsV[s] = rs[s];
    for (auto& p : b->tss) b->tssV[p.first][p.second] = tss[p.first][p.second];
    for (DWORD t : b->xf) b->xfV[t] = xf[t];
    for (DWORD i : b->lights) { auto it = lights.find(i); if (it != lights.end()) b->lightV[i] = it->second; }
    for (DWORD i : b->lightEn) b->lightEnV[i] = lightOn.count(i) ? lightOn[i] : FALSE;
    for (DWORD i : b->clip) memcpy(b->clipV[i], clip[i], sizeof(clip[i]));
    for (DWORD i : b->tex) b->texV[i] = tex[i];
    if (b->viewport) b->vp = vp;
    if (b->material) b->mat = mat;
}

void Device7::Apply(StateBlock* b)
{
    for (DWORD s : b->rs) SetRenderState((D3DRENDERSTATETYPE)s, b->rsV[s]);
    for (auto& p : b->tss) SetTextureStageState(p.first, (D3DTEXTURESTAGESTATETYPE)p.second, b->tssV[p.first][p.second]);
    for (DWORD t : b->xf) SetTransform((D3DTRANSFORMSTATETYPE)t, &b->xfV[t]);
    for (DWORD i : b->lights) { auto it = b->lightV.find(i); if (it != b->lightV.end()) SetLight(i, &it->second); }
    for (DWORD i : b->lightEn) LightEnable(i, b->lightEnV[i]);
    for (DWORD i : b->clip) SetClipPlane(i, b->clipV[i]);
    for (DWORD i : b->tex) SetTexture(i, b->texV[i]);
    if (b->viewport) SetViewport(&b->vp);
    if (b->material) SetMaterial(&b->mat);
}

HRESULT Device7::ApplyStateBlock(DWORD h)
{
    if (!h || h > blocks.size() || !blocks[h - 1]) return D3DERR_INVALIDSTATEBLOCK;
    if (recording) return D3DERR_INBEGINSTATEBLOCK;
    Apply(blocks[h - 1]);
    return D3D_OK;
}

HRESULT Device7::CaptureStateBlock(DWORD h)
{
    if (!h || h > blocks.size() || !blocks[h - 1]) return D3DERR_INVALIDSTATEBLOCK;
    Capture(blocks[h - 1]);
    return D3D_OK;
}

HRESULT Device7::DeleteStateBlock(DWORD h)
{
    if (!h || h > blocks.size() || !blocks[h - 1]) return D3DERR_INVALIDSTATEBLOCK;
    delete blocks[h - 1];
    blocks[h - 1] = NULL;
    return D3D_OK;
}

HRESULT Device7::CreateStateBlock(D3DSTATEBLOCKTYPE type, LPDWORD h)
{
    if (!h) return DDERR_INVALIDPARAMS;
    StateBlock* b = new StateBlock();
    // Capture everything for all block types: replaying a superset of the
    // requested category restores the same values the game saw.
    for (DWORD s = 1; s < kNumRS; s++) b->rs.push_back(s);
    for (DWORD st = 0; st < kMaxStages; st++)
        for (DWORD t = 1; t < kNumTSS; t++) b->tss.push_back(std::make_pair(st, t));
    for (DWORD t = 1; t < kNumXF; t++) b->xf.push_back(t);
    for (auto& l : lights) b->lights.push_back(l.first);
    for (auto& l : lightOn) b->lightEn.push_back(l.first);
    for (DWORD i = 0; i < 32; i++) b->clip.push_back(i);
    if (type == D3DSBT_ALL || type == D3DSBT_PIXELSTATE)
        for (DWORD i = 0; i < kMaxStages; i++) b->tex.push_back(i);
    b->viewport = true;
    b->material = true;
    Capture(b);
    blocks.push_back(b);
    *h = (DWORD)blocks.size();
    return D3D_OK;
}

HRESULT Device7::PreLoad(LPDIRECTDRAWSURFACE7 s)
{
    if (s) ((Surface7*)s)->UploadIfDirty();
    return D3D_OK;
}

// --- drawing ---------------------------------------------------------------
HRESULT Device7::DrawPrimitive(D3DPRIMITIVETYPE p, DWORD fvf, LPVOID v, DWORD n, DWORD)
{
    for (int i = 0; i < kMaxStages; i++) if (tex[i] && !tex[i]->rt && tex[i]->root->dirty) ApplyTexture(i, tex[i]);
    const void* dv = PrepareDraw(fvf, v, n);
    if (g_probe == 2) ProbeDraw(p, fvf, dv, n, NULL, 0);
    Submit(p, fvf, dv, n, NULL, 0);
    return D3D_OK;
}

HRESULT Device7::DrawIndexedPrimitive(D3DPRIMITIVETYPE p, DWORD fvf, LPVOID v, DWORD n, LPWORD idx, DWORD ni, DWORD)
{
    for (int i = 0; i < kMaxStages; i++) if (tex[i] && !tex[i]->rt && tex[i]->root->dirty) ApplyTexture(i, tex[i]);
    const void* dv = PrepareDraw(fvf, v, n);
    if (g_probe == 2) ProbeDraw(p, fvf, dv, n, idx, ni);
    Submit(p, fvf, dv, n, idx, ni);
    return D3D_OK;
}

HRESULT Device7::SetClipStatus(LPD3DCLIPSTATUS c) { if (c) clipStatus = *c; return D3D_OK; }
HRESULT Device7::GetClipStatus(LPD3DCLIPSTATUS c) { if (!c) return DDERR_INVALIDPARAMS; *c = clipStatus; return D3D_OK; }

// Interleave strided data into a temporary FVF vertex buffer.
static void Interleave(DWORD fvf, LPD3DDRAWPRIMITIVESTRIDEDDATA d, DWORD n, std::vector<BYTE>& out)
{
    UINT stride = R::FVFStride(fvf);
    out.resize((size_t)stride * n);
    DWORD pos = fvf & D3DFVF_POSITION_MASK;
    UINT posSize = pos == D3DFVF_XYZRHW ? 16 : pos == D3DFVF_XYZ ? 12 : 12 + 4 * ((pos - D3DFVF_XYZB1) / 2 + 1);
    UINT ntex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (DWORD i = 0; i < n; i++) {
        BYTE* o = out.data() + (size_t)i * stride;
        auto copy = [&](const D3DDP_PTRSTRIDE& s, UINT bytes) {
            if (s.lpvData) memcpy(o, (const BYTE*)s.lpvData + (size_t)i * s.dwStride, bytes);
            else memset(o, 0, bytes);
            o += bytes;
        };
        copy(d->position, posSize);
        if (fvf & D3DFVF_NORMAL) copy(d->normal, 12);
        if (fvf & 0x20) { memset(o, 0, 4); o += 4; }
        if (fvf & D3DFVF_DIFFUSE) copy(d->diffuse, 4);
        if (fvf & D3DFVF_SPECULAR) copy(d->specular, 4);
        for (UINT t = 0; t < ntex; t++) {
            UINT fmt = (fvf >> (16 + t * 2)) & 3;
            UINT bytes = fmt == 0 ? 8 : fmt == 1 ? 12 : fmt == 2 ? 16 : 4;
            copy(d->textureCoords[t], bytes);
        }
    }
}

HRESULT Device7::DrawPrimitiveStrided(D3DPRIMITIVETYPE p, DWORD fvf, LPD3DDRAWPRIMITIVESTRIDEDDATA d, DWORD n, DWORD f)
{
    if (!d) return DDERR_INVALIDPARAMS;
    Interleave(fvf, d, n, strided);
    return DrawPrimitive(p, fvf, strided.data(), n, f);
}

HRESULT Device7::DrawIndexedPrimitiveStrided(D3DPRIMITIVETYPE p, DWORD fvf, LPD3DDRAWPRIMITIVESTRIDEDDATA d, DWORD n, LPWORD idx, DWORD ni, DWORD f)
{
    if (!d) return DDERR_INVALIDPARAMS;
    Interleave(fvf, d, n, strided);
    return DrawIndexedPrimitive(p, fvf, strided.data(), n, idx, ni, f);
}

HRESULT Device7::DrawPrimitiveVB(D3DPRIMITIVETYPE p, LPDIRECT3DVERTEXBUFFER7 vbI, DWORD start, DWORD n, DWORD f)
{
    VB7* vb = (VB7*)vbI;
    if (!vb || start + n > vb->desc.dwNumVertices) return DDERR_INVALIDPARAMS;
    return DrawPrimitive(p, vb->desc.dwFVF, vb->mem.data() + (size_t)start * vb->stride, n, f);
}

HRESULT Device7::DrawIndexedPrimitiveVB(D3DPRIMITIVETYPE p, LPDIRECT3DVERTEXBUFFER7 vbI, DWORD start, DWORD n, LPWORD idx, DWORD ni, DWORD f)
{
    VB7* vb = (VB7*)vbI;
    if (!vb || start + n > vb->desc.dwNumVertices) return DDERR_INVALIDPARAMS;
    return DrawIndexedPrimitive(p, vb->desc.dwFVF, vb->mem.data() + (size_t)start * vb->stride, n, idx, ni, f);
}

HRESULT Device7::ComputeSphereVisibility(LPD3DVECTOR, LPD3DVALUE, DWORD n, DWORD, LPDWORD out)
{
    if (out) memset(out, 0, n * sizeof(DWORD));   // report everything as visible
    return D3D_OK;
}

HRESULT Device7::GetTexture(DWORD st, LPDIRECTDRAWSURFACE7* out)
{
    if (!out || st >= kMaxStages) return DDERR_INVALIDPARAMS;
    *out = tex[st];
    if (tex[st]) tex[st]->AddRef();
    return D3D_OK;
}

HRESULT Device7::SetTexture(DWORD st, LPDIRECTDRAWSURFACE7 s)
{
    if (st >= kMaxStages) return DDERR_INVALIDPARAMS;
    Surface7* t = (Surface7*)s;
    if (Rec()) { recording->tex.push_back(st); recording->texV[st] = t; return D3D_OK; }
    if (t) t->AddRef();
    if (tex[st]) tex[st]->Release();
    tex[st] = t;
    ApplyTexture(st, t);
    if (st == 0) ApplyAlphaTest();
    return D3D_OK;
}

HRESULT Device7::GetTextureStageState(DWORD st, D3DTEXTURESTAGESTATETYPE t, LPDWORD v)
{
    if (!v || st >= kMaxStages || (DWORD)t >= kNumTSS) return DDERR_INVALIDPARAMS;
    *v = tss[st][t];
    return D3D_OK;
}

HRESULT Device7::SetTextureStageState(DWORD st, D3DTEXTURESTAGESTATETYPE t, DWORD v)
{
    if (st >= kMaxStages || (DWORD)t >= kNumTSS) return DDERR_INVALIDPARAMS;
    if (Rec()) { recording->tss.push_back(std::make_pair(st, (DWORD)t)); recording->tssV[st][t] = v; return D3D_OK; }
    tss[st][t] = v;
    if (g_cfg.logLevel >= 2) {
        char b[64];
        wsprintfA(b, "TSS stage%u type%u = %u", st, (DWORD)t, v);
        LogOnce(b);
    }
    ApplyTSS(st, t, v);
    return D3D_OK;
}

HRESULT Device7::ValidateDevice(LPDWORD passes) { if (passes) *passes = 1; return D3D_OK; }

HRESULT Device7::Load(LPDIRECTDRAWSURFACE7 dstI, LPPOINT pt, LPDIRECTDRAWSURFACE7 srcI, LPRECT sr, DWORD)
{
    Surface7* dst = (Surface7*)dstI;
    Surface7* src = (Surface7*)srcI;
    if (!dst || !src) return DDERR_INVALIDPARAMS;
    // copy each level that exists in both chains
    Surface7* d = dst; Surface7* s = src;
    int lvl = 0;
    while (d && s) {
        RECT r;
        if (sr && lvl == 0) r = *sr; else s->FullRect(&r);
        RECT dr = r;
        if (pt && lvl == 0) OffsetRect(&dr, pt->x - r.left, pt->y - r.top);
        d->Blt(&dr, s, &r, 0, NULL);
        if (s->hasCK) { d->hasCK = true; d->ck = s->ck; }
        d = d->next && d->next != d->root ? d->next : NULL;
        s = s->next && s->next != s->root ? s->next : NULL;
        lvl++;
    }
    if (src->palette) dst->SetPalette(src->palette);
    dst->MarkDirty();
    return D3D_OK;
}

HRESULT Device7::LightEnable(DWORD i, BOOL on)
{
    if (Rec()) { recording->lightEn.push_back(i); recording->lightEnV[i] = on; return D3D_OK; }
    if (!lights.count(i)) {
        // D3D7 creates a default directional light when enabling an unset index
        D3DLIGHT7 l = {};
        l.dltType = D3DLIGHT_DIRECTIONAL;
        l.dcvDiffuse.r = l.dcvDiffuse.g = l.dcvDiffuse.b = 1.0f;
        l.dvDirection.z = 1.0f;
        lights[i] = l;
        R::SetLight(i, &l);
    }
    lightOn[i] = on;
    R::LightEnable(i, on);
    return D3D_OK;
}

HRESULT Device7::GetLightEnable(DWORD i, BOOL* on)
{
    if (!on) return DDERR_INVALIDPARAMS;
    *on = lightOn.count(i) ? lightOn[i] : FALSE;
    return D3D_OK;
}

HRESULT Device7::SetClipPlane(DWORD i, D3DVALUE* p)
{
    if (!p || i >= 32) return DDERR_INVALIDPARAMS;
    if (Rec()) { recording->clip.push_back(i); memcpy(recording->clipV[i], p, 16); return D3D_OK; }
    memcpy(clip[i], p, 16);
    R::SetClipPlane(i, p);
    return D3D_OK;
}

HRESULT Device7::GetClipPlane(DWORD i, D3DVALUE* p)
{
    if (!p || i >= 32) return DDERR_INVALIDPARAMS;
    memcpy(p, clip[i], 16);
    return D3D_OK;
}

HRESULT Device7::GetInfo(DWORD, LPVOID, DWORD) { return S_FALSE; }

// ---------------------------------------------------------------------------
// Device description
// ---------------------------------------------------------------------------
void DeviceDescFill(D3DDEVICEDESC7* d, const GUID& guid, bool hw, bool tnl)
{
    ZeroMemory(d, sizeof(*d));
    d->dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
                   D3DDEVCAPS_TEXTURESYSTEMMEMORY | D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP |
                   D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_CANBLTSYSTONONLOCAL;
    if (hw) d->dwDevCaps |= D3DDEVCAPS_HWRASTERIZATION | D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_TEXTURENONLOCALVIDMEM |
                            D3DDEVCAPS_EXECUTEVIDEOMEMORY | D3DDEVCAPS_TLVERTEXVIDEOMEMORY;
    if (tnl) d->dwDevCaps |= D3DDEVCAPS_HWTRANSFORMANDLIGHT;

    D3DPRIMCAPS pc = {};
    pc.dwSize = sizeof(pc);
    pc.dwMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_LINEPATTERNREP | D3DPMISCCAPS_CULLNONE |
                    D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW;
    pc.dwRasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE |
                      D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_ZBIAS | D3DPRASTERCAPS_FOGRANGE |
                      D3DPRASTERCAPS_ANISOTROPY | D3DPRASTERCAPS_WFOG | D3DPRASTERCAPS_ZFOG | D3DPRASTERCAPS_SUBPIXEL;
    pc.dwZCmpCaps = 0xFF;   // all D3DPCMPCAPS_*
    pc.dwSrcBlendCaps = 0x1FFF;
    pc.dwDestBlendCaps = 0x1FFF;
    pc.dwAlphaCmpCaps = 0xFF;
    pc.dwShadeCaps = D3DPSHADECAPS_COLORFLATRGB | D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARFLATRGB |
                     D3DPSHADECAPS_SPECULARGOURAUDRGB | D3DPSHADECAPS_ALPHAFLATBLEND | D3DPSHADECAPS_ALPHAGOURAUDBLEND |
                     D3DPSHADECAPS_FOGFLAT | D3DPSHADECAPS_FOGGOURAUD;
    pc.dwTextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_TRANSPARENCY |
                       D3DPTEXTURECAPS_BORDER | D3DPTEXTURECAPS_ALPHAPALETTE | D3DPTEXTURECAPS_PROJECTED |
                       D3DPTEXTURECAPS_CUBEMAP | D3DPTEXTURECAPS_COLORKEYBLEND;
    pc.dwTextureFilterCaps = D3DPTFILTERCAPS_NEAREST | D3DPTFILTERCAPS_LINEAR | D3DPTFILTERCAPS_MIPNEAREST |
                             D3DPTFILTERCAPS_MIPLINEAR | D3DPTFILTERCAPS_LINEARMIPNEAREST | D3DPTFILTERCAPS_LINEARMIPLINEAR |
                             D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MINFANISOTROPIC |
                             D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR |
                             D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR | D3DPTFILTERCAPS_MAGFANISOTROPIC;
    pc.dwTextureBlendCaps = D3DPTBLENDCAPS_DECAL | D3DPTBLENDCAPS_MODULATE | D3DPTBLENDCAPS_DECALALPHA |
                            D3DPTBLENDCAPS_MODULATEALPHA | D3DPTBLENDCAPS_DECALMASK | D3DPTBLENDCAPS_MODULATEMASK |
                            D3DPTBLENDCAPS_COPY | D3DPTBLENDCAPS_ADD;
    pc.dwTextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP |
                              D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV;
    d->dpcLineCaps = pc;
    d->dpcTriCaps = pc;

    d->dwDeviceRenderBitDepth = DDBD_16 | DDBD_32;
    d->dwDeviceZBufferBitDepth = DDBD_16 | DDBD_24 | DDBD_32;
    d->dwMinTextureWidth = 1;
    d->dwMinTextureHeight = 1;
    d->dwMaxTextureWidth = 4096;
    d->dwMaxTextureHeight = 4096;
    d->dwMaxTextureRepeat = 8192;
    d->dwMaxTextureAspectRatio = 4096;
    d->dwMaxAnisotropy = 16;
    d->dvGuardBandLeft = -8192.0f;
    d->dvGuardBandTop = -8192.0f;
    d->dvGuardBandRight = 8191.0f;
    d->dvGuardBandBottom = 8191.0f;
    d->dvExtentsAdjust = 0.0f;
    d->dwStencilCaps = 0xFF;
    d->dwFVFCaps = 8;   // texture coordinate sets
    d->dwTextureOpCaps = 0x00FFFFFF;
    d->wMaxTextureBlendStages = 8;
    d->wMaxSimultaneousTextures = 8;
    d->dwMaxActiveLights = 8;
    d->dvMaxVertexW = 1.0e10f;
    d->deviceGUID = guid;
    d->wMaxUserClipPlanes = 6;
    d->wMaxVertexBlendMatrices = 4;
    d->dwVertexProcessingCaps = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_MATERIALSOURCE7 | D3DVTXPCAPS_VERTEXFOG |
                                D3DVTXPCAPS_DIRECTIONALLIGHTS | D3DVTXPCAPS_POSITIONALLIGHTS | D3DVTXPCAPS_LOCALVIEWER;
}

// ---------------------------------------------------------------------------
// IDirect3D7
// ---------------------------------------------------------------------------
HRESULT D3D7::QueryInterface(REFIID riid, LPVOID* ppv) { return dd->QueryInterface(riid, ppv); }
ULONG D3D7::AddRef() { return dd->AddRef(); }
ULONG D3D7::Release() { return dd->Release(); }

HRESULT D3D7::EnumDevices(LPD3DENUMDEVICESCALLBACK7 cb, LPVOID ctx)
{
    if (!cb) return DDERR_INVALIDPARAMS;
    struct { const char* desc; const char* name; const GUID* g; bool hw, tnl; } devs[] = {
        { "Microsoft Direct3D RGB Software Emulation", "RGB Emulation", &IID_IDirect3DRGBDevice, false, false },
        { "Microsoft Direct3D Hardware acceleration through Direct3D HAL", "Direct3D HAL", &IID_IDirect3DHALDevice, true, false },
        { "Microsoft Direct3D Hardware Transform and Lighting acceleration capable device", "Direct3D T&L HAL", &IID_IDirect3DTnLHalDevice, true, true },
    };
    for (auto& d : devs) {
        D3DDEVICEDESC7 desc;
        DeviceDescFill(&desc, *d.g, d.hw, d.tnl);
        if (cb((LPSTR)d.desc, (LPSTR)d.name, &desc, ctx) == D3DENUMRET_CANCEL) break;
    }
    return D3D_OK;
}

HRESULT D3D7::CreateDevice(REFCLSID g, LPDIRECTDRAWSURFACE7 s, LPDIRECT3DDEVICE7* out)
{
    if (!out) return DDERR_INVALIDPARAMS;
    Surface7* rt = (Surface7*)s;
    if (!rt || !rt->rt) { Log("CreateDevice: surface is not a render target"); return DDERR_INVALIDPARAMS; }
    Log("CreateDevice on %ux%u target%s", rt->desc.dwWidth, rt->desc.dwHeight, rt->ZBuffer() ? " (+Z)" : "");
    AddRef();
    *out = new Device7(this, g, rt);
    return D3D_OK;
}

HRESULT D3D7::CreateVertexBuffer(LPD3DVERTEXBUFFERDESC d, LPDIRECT3DVERTEXBUFFER7* out, DWORD)
{
    if (!d || !out) return DDERR_INVALIDPARAMS;
    VB7* vb = new VB7();
    vb->desc = *d;
    vb->desc.dwSize = sizeof(D3DVERTEXBUFFERDESC);
    vb->stride = R::FVFStride(d->dwFVF);
    vb->mem.assign((size_t)vb->stride * d->dwNumVertices, 0);
    *out = vb;
    return D3D_OK;
}

HRESULT D3D7::EnumZBufferFormats(REFCLSID, LPD3DENUMPIXELFORMATSCALLBACK cb, LPVOID ctx)
{
    if (!cb) return DDERR_INVALIDPARAMS;
    struct { DWORD bits, stencil, zmask, smask; } f[] = {
        { 16, 0, 0xFFFF, 0 },
        { 24, 0, 0xFFFFFF, 0 },
        { 32, 8, 0xFFFFFF, 0xFF000000 },
        { 32, 0, 0xFFFFFFFF, 0 },
    };
    for (auto& z : f) {
        DDPIXELFORMAT pf = {};
        pf.dwSize = sizeof(pf);
        pf.dwFlags = DDPF_ZBUFFER | (z.stencil ? DDPF_STENCILBUFFER : 0);
        pf.dwZBufferBitDepth = z.bits;
        pf.dwStencilBitDepth = z.stencil;
        pf.dwZBitMask = z.zmask;
        pf.dwStencilBitMask = z.smask;
        if (cb(&pf, ctx) == D3DENUMRET_CANCEL) break;
    }
    return D3D_OK;
}

HRESULT D3D7::EvictManagedTextures() { return D3D_OK; }

// ---------------------------------------------------------------------------
// Vertex buffer (system memory)
// ---------------------------------------------------------------------------
HRESULT VB7::QueryInterface(REFIID riid, LPVOID* ppv)
{
    if (riid == IID_IUnknown || riid == IID_IDirect3DVertexBuffer7) { *ppv = this; AddRef(); return S_OK; }
    *ppv = NULL;
    return E_NOINTERFACE;
}
ULONG VB7::AddRef() { return InterlockedIncrement(&ref); }
ULONG VB7::Release() { LONG r = InterlockedDecrement(&ref); if (r <= 0) { delete this; return 0; } return r; }

HRESULT VB7::Lock(DWORD, LPVOID* p, LPDWORD size)
{
    if (!p) return DDERR_INVALIDPARAMS;
    *p = mem.data();
    if (size) *size = (DWORD)mem.size();
    return D3D_OK;
}

HRESULT VB7::Unlock() { return D3D_OK; }

// Software transform of positions into XYZRHW using the device matrices.
HRESULT VB7::ProcessVertices(DWORD, DWORD dstIndex, DWORD count, LPDIRECT3DVERTEXBUFFER7 srcI, DWORD srcIndex,
                             LPDIRECT3DDEVICE7 devI, DWORD)
{
    VB7* src = (VB7*)srcI;
    Device7* dev = (Device7*)devI;
    if (!src || !dev) return DDERR_INVALIDPARAMS;
    if (dstIndex + count > desc.dwNumVertices || srcIndex + count > src->desc.dwNumVertices) return DDERR_INVALIDPARAMS;
    LogOnce("ProcessVertices: software transform (no lighting)");

    D3DMATRIX wv, wvp;
    const D3DMATRIX& w = dev->xf[D3DTRANSFORMSTATE_WORLD];
    const D3DMATRIX& v = dev->xf[D3DTRANSFORMSTATE_VIEW];
    const D3DMATRIX& p = dev->xf[D3DTRANSFORMSTATE_PROJECTION];
    MatMul(w, v, wv);
    MatMul(wv, p, wvp);
    const D3DVIEWPORT7& vp = dev->vp;
    DWORD sf = src->desc.dwFVF, df = desc.dwFVF;
    for (DWORD i = 0; i < count; i++) {
        const BYTE* s = src->mem.data() + (size_t)(srcIndex + i) * src->stride;
        BYTE* d = mem.data() + (size_t)(dstIndex + i) * stride;
        const float* in = (const float*)s;
        float x = in[0], y = in[1], z = in[2];
        float cx = x * wvp._11 + y * wvp._21 + z * wvp._31 + wvp._41;
        float cy = x * wvp._12 + y * wvp._22 + z * wvp._32 + wvp._42;
        float cz = x * wvp._13 + y * wvp._23 + z * wvp._33 + wvp._43;
        float cw = x * wvp._14 + y * wvp._24 + z * wvp._34 + wvp._44;
        float rhw = cw != 0.0f ? 1.0f / cw : 1.0f;
        float* o = (float*)d;
        o[0] = vp.dwX + (1.0f + cx * rhw) * 0.5f * vp.dwWidth;
        o[1] = vp.dwY + (1.0f - cy * rhw) * 0.5f * vp.dwHeight;
        o[2] = vp.dvMinZ + cz * rhw * (vp.dvMaxZ - vp.dvMinZ);
        o[3] = rhw;
        // copy diffuse/specular/texcoords when both formats carry them
        const BYTE* sp = s + ((sf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW ? 16 : 12) + ((sf & D3DFVF_NORMAL) ? 12 : 0) + ((sf & 0x20) ? 4 : 0);
        BYTE* dp = d + 16 + ((df & 0x20) ? 4 : 0);
        DWORD diffuse = 0xFFFFFFFF, specular = 0;
        if (sf & D3DFVF_DIFFUSE) { memcpy(&diffuse, sp, 4); sp += 4; }
        if (sf & D3DFVF_SPECULAR) { memcpy(&specular, sp, 4); sp += 4; }
        if (df & D3DFVF_DIFFUSE) { memcpy(dp, &diffuse, 4); dp += 4; }
        if (df & D3DFVF_SPECULAR) { memcpy(dp, &specular, 4); dp += 4; }
        size_t texBytes = (size_t)(s + src->stride - sp);
        size_t room = (size_t)(d + stride - dp);
        memcpy(dp, sp, texBytes < room ? texBytes : room);
    }
    return D3D_OK;
}

HRESULT VB7::GetVertexBufferDesc(LPD3DVERTEXBUFFERDESC d) { if (!d) return DDERR_INVALIDPARAMS; *d = desc; return D3D_OK; }
HRESULT VB7::Optimize(LPDIRECT3DDEVICE7, DWORD) { return D3D_OK; }
HRESULT VB7::ProcessVerticesStrided(DWORD, DWORD, DWORD, LPD3DDRAWPRIMITIVESTRIDEDDATA, DWORD, LPDIRECT3DDEVICE7, DWORD)
{
    LogOnce("ProcessVerticesStrided unsupported");
    return DDERR_UNSUPPORTED;
}
