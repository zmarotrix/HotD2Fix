// hod2fix - DLL entry, exports, configuration, window management, IAT hooks.
#define INITGUID
#include "dx7.h"
#include <stdio.h>
#include <stdarg.h>
#include <set>
#include <string>
#include <mmsystem.h>
#include <math.h>

#pragma comment(lib, "winmm.lib")

Config g_cfg;
volatile LONG g_probe;
float g_probeX, g_probeY;
LONG g_probeHits;
int g_gameW = 640, g_gameH = 480;

static HMODULE s_self;
static char s_dir[MAX_PATH];
static CRITICAL_SECTION s_logLock;
static FILE* s_log;

// ---------------------------------------------------------------------------
// logging / config
// ---------------------------------------------------------------------------
static void LogWrite(const char* fmt, va_list ap)
{
    if (!s_log) return;
    EnterCriticalSection(&s_logLock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(s_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    vfprintf(s_log, fmt, ap);
    fputc('\n', s_log);
    fflush(s_log);
    LeaveCriticalSection(&s_logLock);
}

void Log(const char* fmt, ...)
{
    if (g_cfg.logLevel < 1) return;
    va_list ap; va_start(ap, fmt); LogWrite(fmt, ap); va_end(ap);
}

void LogV(const char* fmt, ...)
{
    if (g_cfg.logLevel < 2) return;
    va_list ap; va_start(ap, fmt); LogWrite(fmt, ap); va_end(ap);
}

void LogOnce(const char* what)
{
    static std::set<std::string> seen;
    EnterCriticalSection(&s_logLock);
    bool first = seen.insert(what).second;
    LeaveCriticalSection(&s_logLock);
    if (first) Log("%s", what);
}

const char* GameDir() { return s_dir; }

static int IniInt(const char* sec, const char* key, int def)
{
    char path[MAX_PATH];
    wsprintfA(path, "%s\\hod2fix.ini", s_dir);
    return (int)GetPrivateProfileIntA(sec, key, def, path);
}

// Accepts F1..F24, PrintScreen, ScrollLock, Pause, Insert, Home, End, a single
// letter/digit, a decimal or 0x-prefixed virtual-key code, or None.
static int ParseKey(const char* k)
{
    if (!*k || !lstrcmpiA(k, "None") || !lstrcmpiA(k, "0")) return 0;
    if ((k[0] == 'F' || k[0] == 'f') && k[1] >= '1' && k[1] <= '9') {
        int n = atoi(k + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (!lstrcmpiA(k, "PrintScreen") || !lstrcmpiA(k, "PrtSc")) return VK_SNAPSHOT;
    if (!lstrcmpiA(k, "ScrollLock")) return VK_SCROLL;
    if (!lstrcmpiA(k, "Pause")) return VK_PAUSE;
    if (!lstrcmpiA(k, "Insert")) return VK_INSERT;
    if (!lstrcmpiA(k, "Home")) return VK_HOME;
    if (!lstrcmpiA(k, "End")) return VK_END;
    if (!lstrcmpiA(k, "Tab")) return VK_TAB;
    if (!lstrcmpiA(k, "Backspace")) return VK_BACK;
    if (!k[1] && ((k[0] >= 'A' && k[0] <= 'Z') || (k[0] >= '0' && k[0] <= '9'))) return k[0];
    if (!k[1] && k[0] >= 'a' && k[0] <= 'z') return k[0] - 32;
    return (int)strtol(k, NULL, 0);
}

void LoadConfig()
{
    g_cfg.fullscreen  = IniInt("Display", "Fullscreen", 1);
    g_cfg.windowScale = IniInt("Display", "WindowScale", 0);
    g_cfg.renderScale = IniInt("Display", "RenderScale", 0);
    g_cfg.msaa        = IniInt("Display", "MSAA", 4);
    g_cfg.forceLinear = IniInt("Display", "SmoothTextures", 0);
    g_cfg.anisotropy  = IniInt("Display", "Anisotropy", 16);
    g_cfg.fixHalos    = IniInt("Display", "FixAlphaHalos", 1);
    g_cfg.edgeInset   = IniInt("Display", "SpriteEdgeFix", 1);
    g_cfg.vsync       = IniInt("Display", "VSync", 0);
    g_cfg.fpsLimit    = IniInt("Display", "FPSLimit", 60);
    g_cfg.keepAspect  = IniInt("Display", "KeepAspect", 1);
    g_cfg.linearScale = IniInt("Display", "LinearScale", 1);
    g_cfg.remapCursor = IniInt("Input", "RemapCursor", 1);
    g_cfg.clipCursor  = IniInt("Input", "ClipCursor", 0);
    g_cfg.pauseInBackground = IniInt("System", "PauseInBackground", 1);
    g_cfg.timerRes    = IniInt("System", "HighResTimer", 1);
    g_cfg.logLevel    = IniInt("System", "Log", 1);
    g_cfg.dumpEvery   = IniInt("Debug", "DumpEvery", 0);
    char path[MAX_PATH], key[32];
    wsprintfA(path, "%s\\hod2fix.ini", s_dir);
    GetPrivateProfileStringA("Screenshot", "Key", "F12", key, sizeof(key), path);
    g_cfg.screenshotKey = ParseKey(key);
    g_cfg.screenshotNotify = IniInt("Screenshot", "Notify", 1);
    GetPrivateProfileStringA("Display", "HaloToggleKey", "F11", key, sizeof(key), path);
    g_cfg.haloToggleKey = ParseKey(key);
    GetPrivateProfileStringA("Display", "SmoothToggleKey", "F9", key, sizeof(key), path);
    g_cfg.smoothToggleKey = ParseKey(key);
    g_cfg.dumpTextures = IniInt("Debug", "DumpTextures", 0);
    g_cfg.audioEnabled = IniInt("Audio", "Enabled", 1);
    g_cfg.audioLatency = IniInt("Audio", "LatencyMs", 10);
    if (g_cfg.audioLatency < 2) g_cfg.audioLatency = 2;
    if (g_cfg.audioLatency > 100) g_cfg.audioLatency = 100;
    g_cfg.audioVolume = IniInt("Audio", "Volume", 100);
    if (g_cfg.audioVolume < 0) g_cfg.audioVolume = 0;
    if (g_cfg.audioVolume > 100) g_cfg.audioVolume = 100;
    g_cfg.audioLimiter = IniInt("Audio", "Limiter", 1);
    GetPrivateProfileStringA("Audio", "Device", "", g_cfg.audioDevice, sizeof(g_cfg.audioDevice), path);
    GetPrivateProfileStringA("Debug", "ProbeKey", "F8", key, sizeof(key), path);
    g_cfg.probeKey = ParseKey(key);
    GetPrivateProfileStringA("System", "FastForwardKey", "F7", key, sizeof(key), path);
    g_cfg.fastForwardKey = ParseKey(key);
    g_cfg.fastForwardSpeed = IniInt("System", "FastForwardSpeed", 8);
    g_cfg.skipIntro = IniInt("System", "SkipIntroLogos", 1);
    GetPrivateProfileStringA("Display", "DepthBuffer", "float", key, sizeof(key), path);
    g_cfg.floatDepth = lstrcmpiA(key, "24") != 0;
    if (g_cfg.fastForwardSpeed < 2) g_cfg.fastForwardSpeed = 2;
    if (g_cfg.fastForwardSpeed > 32) g_cfg.fastForwardSpeed = 32;
}

// ---------------------------------------------------------------------------
// window management
// ---------------------------------------------------------------------------
static HWND    s_hwnd;
static WNDPROC s_origProc;
static bool    s_managed;     // we own the window's size and position
static bool    s_inApply;

HWND Win_Get() { return s_hwnd; }

static void ClipToWindow(bool on)
{
    if (!g_cfg.clipCursor) return;
    if (on && s_hwnd && GetForegroundWindow() == s_hwnd) {
        RECT rc;
        GetClientRect(s_hwnd, &rc);
        MapWindowPoints(s_hwnd, NULL, (POINT*)&rc, 2);
        ClipCursor(&rc);
    } else {
        ClipCursor(NULL);
    }
}

void Win_ApplyMode()
{
    if (!s_hwnd) return;
    s_inApply = true;
    HMONITOR mon = MonitorFromWindow(s_hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoA(mon, &mi);
    LONG style = GetWindowLongA(s_hwnd, GWL_STYLE);
    LONG ex = GetWindowLongA(s_hwnd, GWL_EXSTYLE);
    // never reveal hidden helper windows (D3DX creates one while probing devices)
    UINT show = (style & WS_VISIBLE) ? SWP_SHOWWINDOW : 0;
    if (g_cfg.fullscreen) {
        style = (style & ~(WS_OVERLAPPEDWINDOW | WS_DLGFRAME)) | WS_POPUP;
        ex &= ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_TOPMOST);
        SetWindowLongA(s_hwnd, GWL_STYLE, style);
        SetWindowLongA(s_hwnd, GWL_EXSTYLE, ex);
        const RECT& r = mi.rcMonitor;
        SetWindowPos(s_hwnd, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | show);
    } else {
        style = (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW;
        ex &= ~WS_EX_TOPMOST;
        SetWindowLongA(s_hwnd, GWL_STYLE, style);
        SetWindowLongA(s_hwnd, GWL_EXSTYLE, ex);
        const RECT& wa = mi.rcWork;
        int scale = g_cfg.windowScale;
        if (scale <= 0) {
            // largest integer multiple that fits the work area with borders
            scale = 1;
            for (int s = 2; s <= 8; s++) {
                RECT t = { 0, 0, g_gameW * s, g_gameH * s };
                AdjustWindowRectEx(&t, style, FALSE, ex);
                if (t.right - t.left <= wa.right - wa.left && t.bottom - t.top <= wa.bottom - wa.top) scale = s;
            }
        }
        RECT t = { 0, 0, g_gameW * scale, g_gameH * scale };
        AdjustWindowRectEx(&t, style, FALSE, ex);
        int w = t.right - t.left, h = t.bottom - t.top;
        int x = wa.left + ((wa.right - wa.left) - w) / 2;
        int y = wa.top + ((wa.bottom - wa.top) - h) / 2;
        if (y < wa.top) y = wa.top;
        SetWindowPos(s_hwnd, HWND_NOTOPMOST, x, y, w, h, SWP_FRAMECHANGED | show);
    }
    s_inApply = false;
    s_managed = true;
    ClipToWindow(true);
}

static LRESULT CALLBACK HookProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && (l & (1 << 29))) {   // Alt+Enter
            g_cfg.fullscreen = !g_cfg.fullscreen;
            Log("Alt+Enter -> %s", g_cfg.fullscreen ? "fullscreen" : "windowed");
            Win_ApplyMode();
            return 0;
        }
        break;
    case WM_SYSCHAR:
        if (w == VK_RETURN) return 0;   // no beep
        break;
    case WM_ACTIVATE:
        ClipToWindow(LOWORD(w) != WA_INACTIVE);
        break;
    case WM_ACTIVATEAPP:
        // the game pauses itself when deactivated; optionally keep it running
        if (!w && !g_cfg.pauseInBackground) return 0;
        break;
    case WM_SIZE: case WM_MOVE:
        if (LOWORD(w) != SIZE_MINIMIZED) ClipToWindow(true);
        break;
    case WM_WINDOWPOSCHANGING:
        // The game's own window procedure pins the window to 640x480, so it
        // never sees these while we manage the window.
        if (s_managed || s_inApply) {
            WINDOWPOS* wp = (WINDOWPOS*)l;
            if (!s_inApply && g_cfg.fullscreen) wp->flags |= SWP_NOMOVE | SWP_NOSIZE;
            return DefWindowProcA(h, m, w, l);
        }
        break;
    case WM_GETMINMAXINFO:
    case WM_SIZING:
        if (s_managed || s_inApply) return DefWindowProcA(h, m, w, l);
        break;
    case WM_DISPLAYCHANGE:
        if (g_cfg.fullscreen) Win_ApplyMode();
        break;
    }
    return CallWindowProcA(s_origProc, h, m, w, l);
}

void Win_Attach(HWND hwnd)
{
    if (!hwnd) return;
    s_inApply = false;
    // the game window is top level; ignore child windows handed to clippers
    while (GetParent(hwnd)) hwnd = GetParent(hwnd);
    if (hwnd == s_hwnd) { R::Init(hwnd); return; }
    s_hwnd = hwnd;
    s_managed = false;
    Log("attached to window %p", hwnd);
    s_origProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)HookProc);
    Win_ApplyMode();
    R::Init(hwnd);
}

// ---------------------------------------------------------------------------
// IAT hooks on Hod2.exe (only the game's own calls are affected)
// ---------------------------------------------------------------------------
static bool IsGameWindow(HWND h) { return s_hwnd && h == s_hwnd; }

static BOOL WINAPI H_GetClientRect(HWND h, LPRECT r)
{
    if (IsGameWindow(h) && r) {
        r->left = 0; r->top = 0; r->right = g_gameW; r->bottom = g_gameH;
        return TRUE;
    }
    return GetClientRect(h, r);
}

bool Win_CursorGamePos(float* x, float* y)
{
    POINT p;
    RECT dst; int gw, gh;
    if (!s_hwnd || !GetCursorPos(&p) || !R::GetViewMapping(&dst, &gw, &gh)) return false;
    POINT o = { 0, 0 };
    ClientToScreen(s_hwnd, &o);
    int dw = dst.right - dst.left, dh = dst.bottom - dst.top;
    if (dw <= 0 || dh <= 0) return false;
    *x = (float)((double)(p.x - o.x - dst.left) * gw / dw);
    *y = (float)((double)(p.y - o.y - dst.top) * gh / dh);
    return true;
}

static BOOL WINAPI H_GetCursorPos(LPPOINT p)
{
    BOOL ok = GetCursorPos(p);
    if (!ok || !g_cfg.remapCursor || !s_hwnd || !p) return ok;
    RECT dst; int gw, gh;
    if (!R::GetViewMapping(&dst, &gw, &gh)) return ok;
    POINT o = { 0, 0 };
    ClientToScreen(s_hwnd, &o);
    int dw = dst.right - dst.left, dh = dst.bottom - dst.top;
    if (dw <= 0 || dh <= 0) return ok;
    // map the real cursor into game pixels relative to the client origin
    double gx = (double)(p->x - o.x - dst.left) * gw / dw;
    double gy = (double)(p->y - o.y - dst.top) * gh / dh;
    p->x = o.x + (LONG)floor(gx);
    p->y = o.y + (LONG)floor(gy);
    return ok;
}

static BOOL WINAPI H_SetCursorPos(int x, int y)
{
    if (g_cfg.remapCursor && s_hwnd) {
        RECT dst; int gw, gh;
        if (R::GetViewMapping(&dst, &gw, &gh) && gw > 0 && gh > 0) {
            POINT o = { 0, 0 };
            ClientToScreen(s_hwnd, &o);
            x = o.x + dst.left + MulDiv(x - o.x, dst.right - dst.left, gw);
            y = o.y + dst.top + MulDiv(y - o.y, dst.bottom - dst.top, gh);
        }
    }
    return SetCursorPos(x, y);
}

static BOOL WINAPI H_MoveWindow(HWND h, int x, int y, int w, int hh, BOOL repaint)
{
    if (IsGameWindow(h) && s_managed) return TRUE;
    return MoveWindow(h, x, y, w, hh, repaint);
}

static BOOL WINAPI H_SetWindowPos(HWND h, HWND after, int x, int y, int w, int hh, UINT flags)
{
    if (IsGameWindow(h) && s_managed) flags |= SWP_NOMOVE | SWP_NOSIZE;
    return SetWindowPos(h, after, x, y, w, hh, flags);
}

// The game times its frames with QueryPerformanceCounter / GetTickCount.  It
// sees a clock that runs Time_SetScale times faster during fast forward, and
// stays continuous (never jumps back) when the speed changes.
static CRITICAL_SECTION s_clockLock;
static LONGLONG s_realBase, s_gameBase;
static int      s_timeScale = 1;

static LONGLONG GameClock()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    EnterCriticalSection(&s_clockLock);
    LONGLONG t = s_gameBase + (now.QuadPart - s_realBase) * s_timeScale;
    LeaveCriticalSection(&s_clockLock);
    return t;
}

void Time_SetScale(int scale)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    EnterCriticalSection(&s_clockLock);
    s_gameBase += (now.QuadPart - s_realBase) * s_timeScale;
    s_realBase = now.QuadPart;
    s_timeScale = scale < 1 ? 1 : scale;
    LeaveCriticalSection(&s_clockLock);
}

static BOOL WINAPI H_QueryPerformanceCounter(LARGE_INTEGER* c)
{
    if (!c) return FALSE;
    c->QuadPart = GameClock();
    return TRUE;
}

static DWORD WINAPI H_GetTickCount()
{
    static LONGLONG freq;
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    return (DWORD)(GameClock() / (freq / 1000));
}

static void PatchIAT(HMODULE mod, const char* dll, const char* func, void* hook)
{
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;
    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (lstrcmpiA((const char*)(base + imp->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA* addrs = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* n = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (lstrcmpA((const char*)n->Name, func) != 0) continue;
            DWORD old;
            VirtualProtect(&addrs->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            addrs->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&addrs->u1.Function, sizeof(void*), old, &old);
            LogV("hooked %s!%s", dll, func);
            return;
        }
    }
}

// The European release opens with SEGA and Empire logo cards, run by a small
// state machine: a dispatcher (switch over states 0-5) whose last state hands
// over to the title.  The dispatcher is found by its code, right after that
// last state's function:
//   end:   mov [mode], 3 / mov [state], 0 / jmp cleanup / nop padding
//   disp:  mov eax, [state] / cmp eax, 5 / ja ... / jmp [eax*4 + table]
// and made to jump straight to the end.
static void SkipIntroLogos(HMODULE exe)
{
    BYTE* base = (BYTE*)exe;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_CNT_CODE)) continue;
        BYTE* p = base + sec->VirtualAddress;
        BYTE* lim = p + sec->Misc.VirtualSize - 64;
        for (; p < lim; p++) {
            if (p[0] != 0xC7 || p[1] != 0x05 || *(DWORD*)(p + 6) != 3 || p[10] != 0xC7 || p[11] != 0x05 ||
                *(DWORD*)(p + 16) != 0 || p[20] != 0xE9) continue;
            DWORD state = *(DWORD*)(p + 12);
            BYTE* d = p + 25;
            while (*d == 0x90 && d < p + 48) d++;
            if (d[0] != 0xA1 || *(DWORD*)(d + 1) != state || d[5] != 0x83 || d[6] != 0xF8 || d[7] != 0x05 ||
                d[8] != 0x77 || d[10] != 0xFF || d[11] != 0x24 || d[12] != 0x85) continue;
            DWORD old;
            VirtualProtect(d, 5, PAGE_EXECUTE_READWRITE, &old);
            d[0] = 0xE9;
            *(LONG*)(d + 1) = (LONG)(p - (d + 5));
            VirtualProtect(d, 5, old, &old);
            FlushInstructionCache(GetCurrentProcess(), d, 5);
            Log("skipping the intro logo cards");
            return;
        }
    }
    LogV("no intro logo cards found (Japanese release has none)");
}

void Input_InstallHooks(void (*patchIAT)(HMODULE, const char*, const char*, void*));
void Audio_InstallHooks();

static void InstallHooks()
{
    HMODULE exe = GetModuleHandleA(NULL);
    InitializeCriticalSection(&s_clockLock);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    s_realBase = s_gameBase = now.QuadPart;
    PatchIAT(exe, "KERNEL32.dll", "QueryPerformanceCounter", (void*)H_QueryPerformanceCounter);
    PatchIAT(exe, "KERNEL32.dll", "GetTickCount", (void*)H_GetTickCount);
    PatchIAT(exe, "USER32.dll", "GetClientRect", (void*)H_GetClientRect);
    PatchIAT(exe, "USER32.dll", "GetCursorPos", (void*)H_GetCursorPos);
    PatchIAT(exe, "USER32.dll", "SetCursorPos", (void*)H_SetCursorPos);
    PatchIAT(exe, "USER32.dll", "MoveWindow", (void*)H_MoveWindow);
    PatchIAT(exe, "USER32.dll", "SetWindowPos", (void*)H_SetWindowPos);
    Input_InstallHooks(PatchIAT);
    Audio_InstallHooks();
    if (g_cfg.skipIntro) SkipIntroLogos(exe);
}

// ---------------------------------------------------------------------------
// exports
// ---------------------------------------------------------------------------
extern "C" HRESULT WINAPI hf_DirectDrawCreateEx(GUID* guid, LPVOID* out, REFIID iid, IUnknown* outer)
{
    Log("DirectDrawCreateEx");
    if (!out) return DDERR_INVALIDPARAMS;
    *out = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    if (iid != IID_IDirectDraw7) { Log("DirectDrawCreateEx: only IDirectDraw7 is supported"); return DDERR_INVALIDPARAMS; }
    (void)guid;
    *out = static_cast<IDirectDraw7*>(CreateDDraw7());
    return DD_OK;
}

extern "C" HRESULT WINAPI hf_DirectDrawCreate(GUID*, LPDIRECTDRAW* out, IUnknown*)
{
    Log("DirectDrawCreate (legacy interface) is not supported");
    if (out) *out = NULL;
    return DDERR_UNSUPPORTED;
}

extern "C" HRESULT WINAPI hf_DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA cb, LPVOID ctx, DWORD)
{
    if (!cb) return DDERR_INVALIDPARAMS;
    // Only the primary display: virtual display drivers on the system are never offered.
    cb(NULL, (LPSTR)"Primary Display Driver", (LPSTR)"display", ctx, NULL);
    return DD_OK;
}

extern "C" HRESULT WINAPI hf_DirectDrawEnumerateA(LPDDENUMCALLBACKA cb, LPVOID ctx)
{
    if (!cb) return DDERR_INVALIDPARAMS;
    cb(NULL, (LPSTR)"Primary Display Driver", (LPSTR)"display", ctx);
    return DD_OK;
}

extern "C" HRESULT WINAPI hf_DirectDrawCreateClipper(DWORD, LPDIRECTDRAWCLIPPER* out, IUnknown*)
{
    if (!out) return DDERR_INVALIDPARAMS;
    *out = new Clipper7();
    return DD_OK;
}

// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        s_self = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&s_logLock);
        GetModuleFileNameA(inst, s_dir, MAX_PATH);
        char* slash = strrchr(s_dir, '\\');
        if (slash) *slash = 0;
        LoadConfig();
        if (g_cfg.logLevel > 0) {
            char path[MAX_PATH];
            wsprintfA(path, "%s\\hod2fix.log", s_dir);
            s_log = fopen(path, "w");
        }
        Log("hod2fix loaded (fullscreen=%d renderScale=%d msaa=%d vsync=%d fps=%d)",
            g_cfg.fullscreen, g_cfg.renderScale, g_cfg.msaa, g_cfg.vsync, g_cfg.fpsLimit);

        // Real pixels everywhere: no DPI virtualisation of window or cursor coordinates.
        typedef BOOL (WINAPI *PFN_SetDpiCtx)(HANDLE);
        HMODULE u32 = GetModuleHandleA("user32.dll");
        PFN_SetDpiCtx setDpi = u32 ? (PFN_SetDpiCtx)GetProcAddress(u32, "SetProcessDpiAwarenessContext") : NULL;
        if (!setDpi || !setDpi((HANDLE)-4 /*PER_MONITOR_AWARE_V2*/)) SetProcessDPIAware();

        if (g_cfg.timerRes) timeBeginPeriod(1);
        InstallHooks();
    } else if (reason == DLL_PROCESS_DETACH) {
        Log("hod2fix unloading");
        if (g_cfg.timerRes) timeEndPeriod(1);
        if (s_log) { fclose(s_log); s_log = NULL; }
    }
    return TRUE;
}
