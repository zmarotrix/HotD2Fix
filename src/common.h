// hod2fix - shared declarations (no DirectX headers here).
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Configuration (hod2fix.ini next to Hod2.exe)
// ---------------------------------------------------------------------------
struct Config {
    int   fullscreen;     // 1 = borderless fullscreen, 0 = window
    int   windowScale;    // window client size = 640x480 * windowScale (0 = largest that fits)
    int   renderScale;    // internal resolution multiplier (0 = auto from monitor height)
    int   msaa;           // 0/2/4/8
    int   forceLinear;    // 1 = bilinear filtering on 3D geometry (2D/HUD untouched)
    int   fixHalos;       // 1 = transparent effects don't cut holes in what's behind them
    int   edgeInset;      // 1 = keep smoothed sprites from sampling neighbouring texels
    int   haloToggleKey;  // hotkey that flips fixHalos for comparison (0 = none)
    int   smoothToggleKey; // hotkey that flips forceLinear for comparison (0 = none)
    int   anisotropy;     // 0/1 = off, else forced max anisotropy for linear-filtered textures
    int   vsync;          // 1 = present on vblank
    int   fpsLimit;       // frame limiter, 0 = off (game logic is 60 Hz)
    int   keepAspect;     // 1 = 4:3 with black bars, 0 = stretch
    int   linearScale;    // 1 = bilinear final upscale, 0 = nearest
    int   remapCursor;    // 1 = map mouse into the 640x480 game space
    int   clipCursor;     // 1 = keep cursor inside the window while focused
    int   pauseInBackground; // 0 = keep running when the window loses focus
    int   timerRes;       // 1 = timeBeginPeriod(1)
    int   logLevel;       // 0 off, 1 info, 2 verbose
    int   screenshotKey;  // virtual-key code (0 = off)
    int   screenshotNotify; // 1 = show a notice when a screenshot is saved
    int   audioEnabled;   // 1 = our WASAPI mixer replaces DirectSound
    int   audioLatency;   // target output period in ms
    int   audioVolume;    // master volume 0-100
    int   audioLimiter;   // 1 = smooth peak limiter instead of hard clipping
    char  audioDevice[128]; // part of the output device name (empty = Windows default)
    int   probeKey;       // debug: logs every draw under the cursor for one frame
    int   fastForwardKey; // held: the game runs fastForwardSpeed times faster (cutscenes, credits)
    int   fastForwardSpeed;
    int   skipIntro;      // 1 = skip the SEGA / Empire logo cards at startup
    int   floatDepth;     // 1 = 32-bit float reversed-Z depth (no z-fighting), supersampling instead of MSAA
    int   dumpTextures;   // debug: save every texture the game loads as PNG
    int   dumpEvery;      // debug: save every Nth presented frame as BMP (0 = off)
};
extern Config g_cfg;

void LoadConfig();
void Log(const char* fmt, ...);
void LogV(const char* fmt, ...);   // only when logLevel >= 2
void LogOnce(const char* what);    // logs a given message once per process
void Time_SetScale(int scale);     // speed of the clock the game sees (fast forward)

const char* GameDir();
extern LONG g_liveSurfaces;   // debug counter (ddraw.cpp)

// Game-visible resolution (what the game thinks the screen is).
extern int g_gameW, g_gameH;

// Window management (main.cpp)
void Win_Attach(HWND hwnd);        // called when the game hands us its window
HWND Win_Get();
bool Win_CursorGamePos(float* x, float* y);   // cursor in game pixels (640x480 space)

// Draw probe: 0 idle, 2 = capturing the current frame at (g_probeX, g_probeY)
extern volatile LONG g_probe;
extern float g_probeX, g_probeY;
extern LONG g_probeHits;
void Win_ApplyMode();
