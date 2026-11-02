// hod2fix - DirectSound replacement: a software mixer that outputs over WASAPI.
//
// The game's DirectSoundCreate import is redirected here.  Every sound buffer
// is mixed in float by our own thread and written to a shared-mode WASAPI
// stream (low-latency period through IAudioClient3 where available), which
// follows the Windows default output device.
//
// Implemented: IDirectSound, IDirectSoundBuffer (primary + secondary,
// static and streaming), DuplicateSoundBuffer, IDirectSoundNotify,
// IDirectSound3DListener and IDirectSound3DBuffer (distance attenuation with
// min/max distance and rolloff, MUTE3DATMAXDISTANCE, stereo panning from the
// listener orientation, sound cones, Doppler).
#define DIRECTSOUND_VERSION 0x0700
#include "common.h"
#include <mmreg.h>
#include <dsound.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <vector>
#include <memory>
#include <math.h>

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ole32.lib")

// GUIDs used here (defined locally so no extra import library is needed)
static const GUID kIID_IDirectSound           = { 0x279AFA83, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const GUID kIID_IDirectSoundBuffer     = { 0x279AFA85, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const GUID kIID_IDirectSound3DListener = { 0x279AFA84, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const GUID kIID_IDirectSound3DBuffer   = { 0x279AFA86, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const GUID kIID_IDirectSoundNotify     = { 0xB0210783, 0x89CD, 0x11D0, { 0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16 } };
static const GUID kCLSID_MMDeviceEnumerator   = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID kIID_IMMDeviceEnumerator    = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID kIID_IMMNotificationClient  = { 0x7991EEC9, 0x7E89, 0x4D85, { 0x83, 0x90, 0x6C, 0x70, 0x3C, 0xEC, 0x60, 0xC0 } };
static const GUID kIID_IAudioClient           = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID kIID_IAudioClient3          = { 0x7ED4EE07, 0x8E67, 0x4CD4, { 0x8C, 0x1A, 0x2B, 0x7A, 0x59, 0x87, 0xAD, 0x42 } };
static const GUID kIID_IAudioRenderClient     = { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };
static const GUID kKSDATAFORMAT_FLOAT         = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const GUID kKSDATAFORMAT_PCM           = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

// kPKEY_FriendlyName
static const PROPERTYKEY kPKEY_FriendlyName = { { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 14 };

template <class T> static void SafeRel(T*& p) { if (p) { p->Release(); p = NULL; } }

// millibels (hundredths of a dB) -> linear gain
static inline float MbToGain(LONG mb)
{
    if (mb <= DSBVOLUME_MIN) return 0.0f;
    return powf(10.0f, (float)mb / 2000.0f);
}

struct V3 { float x, y, z; };
static inline V3 Sub(const V3& a, const V3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static inline float Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 Cross(const V3& a, const V3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static inline float Len(const V3& a) { return sqrtf(Dot(a, a)); }
static inline V3 Norm(const V3& a) { float l = Len(a); return l > 1e-6f ? V3{ a.x / l, a.y / l, a.z / l } : V3{ 0, 0, 1 }; }
static inline V3 FromD3D(const D3DVECTOR& v) { return { v.x, v.y, v.z }; }

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------
struct DSBuf;
struct DSound;

struct SoundData {
    std::vector<BYTE> bytes;
    WAVEFORMATEX fmt;
};

static CRITICAL_SECTION   s_lock;           // guards voices and all playback state
static std::vector<DSBuf*> s_voices;        // every secondary buffer
static DS3DLISTENER       s_listener;
static float              s_primaryGain = 1.0f;
static volatile LONG      s_outRate = 48000;
static HANDLE             s_thread;
static volatile bool      s_quit;
static volatile LONG      s_reinit;          // default device changed
static bool               s_started;

// ---------------------------------------------------------------------------
// Secondary / primary buffer
// ---------------------------------------------------------------------------
struct DSBuf : IDirectSoundBuffer, IDirectSound3DBuffer, IDirectSoundNotify {
    LONG ref = 1;
    DSound* ds = NULL;
    DWORD flags = 0;
    bool primary = false;
    std::shared_ptr<SoundData> data;

    // playback state (under s_lock)
    bool playing = false, looping = false;
    double pos = 0;                    // in frames
    LONG vol = 0, pan = 0;
    DWORD freq = 0;                    // 0 = original
    DS3DBUFFER p3d;
    std::vector<DSBPOSITIONNOTIFY> notes;
    float gainL = 0, gainR = 0;        // last applied, for click-free ramps
    bool started = false;

    DSBuf()
    {
        ZeroMemory(&p3d, sizeof(p3d));
        p3d.dwSize = sizeof(p3d);
        p3d.flMinDistance = DS3D_DEFAULTMINDISTANCE;
        p3d.flMaxDistance = DS3D_DEFAULTMAXDISTANCE;
        p3d.dwInsideConeAngle = DS3D_DEFAULTCONEANGLE;
        p3d.dwOutsideConeAngle = DS3D_DEFAULTCONEANGLE;
        p3d.vConeOrientation.z = 1.0f;
        p3d.lConeOutsideVolume = DS3D_DEFAULTCONEOUTSIDEVOLUME;
        p3d.dwMode = DS3DMODE_NORMAL;
    }

    DWORD Bytes() const { return data ? (DWORD)data->bytes.size() : 0; }
    DWORD Align() const { return data && data->fmt.nBlockAlign ? data->fmt.nBlockAlign : 1; }
    DWORD Frames() const { return Bytes() / Align(); }

    void FireNotes(DWORD from, DWORD to, bool wrapped)
    {
        for (auto& n : notes) {
            if (n.dwOffset == DSBPN_OFFSETSTOP || !n.hEventNotify) continue;
            bool hit = wrapped ? (n.dwOffset >= from || n.dwOffset < to) : (n.dwOffset >= from && n.dwOffset < to);
            if (hit) SetEvent(n.hEventNotify);
        }
    }
    void FireStop()
    {
        for (auto& n : notes)
            if (n.dwOffset == DSBPN_OFFSETSTOP && n.hEventNotify) SetEvent(n.hEventNotify);
    }

    // IUnknown (shared by all three interfaces)
    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv);
    STDMETHOD_(ULONG, AddRef)() { return InterlockedIncrement(&ref); }
    STDMETHOD_(ULONG, Release)();

    // IDirectSoundBuffer
    STDMETHOD(GetCaps)(LPDSBCAPS caps);
    STDMETHOD(GetCurrentPosition)(LPDWORD play, LPDWORD write);
    STDMETHOD(GetFormat)(LPWAVEFORMATEX fmt, DWORD size, LPDWORD written);
    STDMETHOD(GetVolume)(LPLONG v) { if (!v) return DSERR_INVALIDPARAM; *v = vol; return DS_OK; }
    STDMETHOD(GetPan)(LPLONG p) { if (!p) return DSERR_INVALIDPARAM; *p = pan; return DS_OK; }
    STDMETHOD(GetFrequency)(LPDWORD f);
    STDMETHOD(GetStatus)(LPDWORD st);
    STDMETHOD(Initialize)(LPDIRECTSOUND, LPCDSBUFFERDESC) { return DSERR_ALREADYINITIALIZED; }
    STDMETHOD(Lock)(DWORD off, DWORD bytes, LPVOID* p1, LPDWORD b1, LPVOID* p2, LPDWORD b2, DWORD flags);
    STDMETHOD(Play)(DWORD, DWORD, DWORD flags);
    STDMETHOD(SetCurrentPosition)(DWORD p);
    STDMETHOD(SetFormat)(LPCWAVEFORMATEX fmt);
    STDMETHOD(SetVolume)(LONG v);
    STDMETHOD(SetPan)(LONG p);
    STDMETHOD(SetFrequency)(DWORD f);
    STDMETHOD(Stop)();
    STDMETHOD(Unlock)(LPVOID, DWORD, LPVOID, DWORD) { return DS_OK; }
    STDMETHOD(Restore)() { return DS_OK; }

    // IDirectSound3DBuffer
    STDMETHOD(GetAllParameters)(LPDS3DBUFFER p) { if (!p) return DSERR_INVALIDPARAM; *p = p3d; return DS_OK; }
    STDMETHOD(GetConeAngles)(LPDWORD in, LPDWORD out) { if (in) *in = p3d.dwInsideConeAngle; if (out) *out = p3d.dwOutsideConeAngle; return DS_OK; }
    STDMETHOD(GetConeOrientation)(D3DVECTOR* v) { if (!v) return DSERR_INVALIDPARAM; *v = p3d.vConeOrientation; return DS_OK; }
    STDMETHOD(GetConeOutsideVolume)(LPLONG v) { if (!v) return DSERR_INVALIDPARAM; *v = p3d.lConeOutsideVolume; return DS_OK; }
    STDMETHOD(GetMaxDistance)(D3DVALUE* d) { if (!d) return DSERR_INVALIDPARAM; *d = p3d.flMaxDistance; return DS_OK; }
    STDMETHOD(GetMinDistance)(D3DVALUE* d) { if (!d) return DSERR_INVALIDPARAM; *d = p3d.flMinDistance; return DS_OK; }
    STDMETHOD(GetMode)(LPDWORD m) { if (!m) return DSERR_INVALIDPARAM; *m = p3d.dwMode; return DS_OK; }
    STDMETHOD(GetPosition)(D3DVECTOR* v) { if (!v) return DSERR_INVALIDPARAM; *v = p3d.vPosition; return DS_OK; }
    STDMETHOD(GetVelocity)(D3DVECTOR* v) { if (!v) return DSERR_INVALIDPARAM; *v = p3d.vVelocity; return DS_OK; }
    STDMETHOD(SetAllParameters)(LPCDS3DBUFFER p, DWORD);
    STDMETHOD(SetConeAngles)(DWORD in, DWORD out, DWORD);
    STDMETHOD(SetConeOrientation)(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD);
    STDMETHOD(SetConeOutsideVolume)(LONG v, DWORD);
    STDMETHOD(SetMaxDistance)(D3DVALUE d, DWORD);
    STDMETHOD(SetMinDistance)(D3DVALUE d, DWORD);
    STDMETHOD(SetMode)(DWORD m, DWORD);
    STDMETHOD(SetPosition)(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD);
    STDMETHOD(SetVelocity)(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD);

    // IDirectSoundNotify
    STDMETHOD(SetNotificationPositions)(DWORD n, LPCDSBPOSITIONNOTIFY p);
};

// ---------------------------------------------------------------------------
// Listener (lives inside the DirectSound object, reached through the primary)
// ---------------------------------------------------------------------------
struct Listener : IDirectSound3DListener {
    DSound* ds = NULL;
    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv);
    STDMETHOD_(ULONG, AddRef)();
    STDMETHOD_(ULONG, Release)();
    STDMETHOD(GetAllParameters)(LPDS3DLISTENER p) { if (!p) return DSERR_INVALIDPARAM; *p = s_listener; return DS_OK; }
    STDMETHOD(GetDistanceFactor)(D3DVALUE* f) { if (!f) return DSERR_INVALIDPARAM; *f = s_listener.flDistanceFactor; return DS_OK; }
    STDMETHOD(GetDopplerFactor)(D3DVALUE* f) { if (!f) return DSERR_INVALIDPARAM; *f = s_listener.flDopplerFactor; return DS_OK; }
    STDMETHOD(GetOrientation)(D3DVECTOR* f, D3DVECTOR* t) { if (f) *f = s_listener.vOrientFront; if (t) *t = s_listener.vOrientTop; return DS_OK; }
    STDMETHOD(GetPosition)(D3DVECTOR* v) { if (!v) return DSERR_INVALIDPARAM; *v = s_listener.vPosition; return DS_OK; }
    STDMETHOD(GetRolloffFactor)(D3DVALUE* f) { if (!f) return DSERR_INVALIDPARAM; *f = s_listener.flRolloffFactor; return DS_OK; }
    STDMETHOD(GetVelocity)(D3DVECTOR* v) { if (!v) return DSERR_INVALIDPARAM; *v = s_listener.vVelocity; return DS_OK; }
    STDMETHOD(SetAllParameters)(LPCDS3DLISTENER p, DWORD)
    {
        if (!p) return DSERR_INVALIDPARAM;
        EnterCriticalSection(&s_lock); s_listener = *p; s_listener.dwSize = sizeof(s_listener); LeaveCriticalSection(&s_lock);
        return DS_OK;
    }
    STDMETHOD(SetDistanceFactor)(D3DVALUE f, DWORD) { s_listener.flDistanceFactor = f > 0 ? f : DS3D_DEFAULTDISTANCEFACTOR; return DS_OK; }
    STDMETHOD(SetDopplerFactor)(D3DVALUE f, DWORD) { s_listener.flDopplerFactor = f; return DS_OK; }
    STDMETHOD(SetOrientation)(D3DVALUE xf, D3DVALUE yf, D3DVALUE zf, D3DVALUE xt, D3DVALUE yt, D3DVALUE zt, DWORD)
    {
        EnterCriticalSection(&s_lock);
        s_listener.vOrientFront.x = xf; s_listener.vOrientFront.y = yf; s_listener.vOrientFront.z = zf;
        s_listener.vOrientTop.x = xt; s_listener.vOrientTop.y = yt; s_listener.vOrientTop.z = zt;
        LeaveCriticalSection(&s_lock);
        return DS_OK;
    }
    STDMETHOD(SetPosition)(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD)
    {
        EnterCriticalSection(&s_lock);
        s_listener.vPosition.x = x; s_listener.vPosition.y = y; s_listener.vPosition.z = z;
        LeaveCriticalSection(&s_lock);
        return DS_OK;
    }
    STDMETHOD(SetRolloffFactor)(D3DVALUE f, DWORD) { s_listener.flRolloffFactor = f; return DS_OK; }
    STDMETHOD(SetVelocity)(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD)
    {
        EnterCriticalSection(&s_lock);
        s_listener.vVelocity.x = x; s_listener.vVelocity.y = y; s_listener.vVelocity.z = z;
        LeaveCriticalSection(&s_lock);
        return DS_OK;
    }
    // Deferred settings are applied immediately; the next mix period picks them up.
    STDMETHOD(CommitDeferredSettings)() { return DS_OK; }
};

// ---------------------------------------------------------------------------
// DirectSound object
// ---------------------------------------------------------------------------
struct DSound : IDirectSound {
    LONG ref = 1;
    Listener listener;
    DWORD speakers = DSSPEAKER_COMBINED(DSSPEAKER_STEREO, DSSPEAKER_GEOMETRY_WIDE);
    DSound() { listener.ds = this; }

    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv)
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == kIID_IDirectSound) { *ppv = this; AddRef(); return S_OK; }
        *ppv = NULL;
        LogOnce("DirectSound: unsupported interface requested");
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() { return InterlockedIncrement(&ref); }
    STDMETHOD_(ULONG, Release)()
    {
        LONG r = InterlockedDecrement(&ref);
        if (r <= 0) { delete this; return 0; }
        return r;
    }
    STDMETHOD(CreateSoundBuffer)(LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER* out, LPUNKNOWN);
    STDMETHOD(GetCaps)(LPDSCAPS caps);
    STDMETHOD(DuplicateSoundBuffer)(LPDIRECTSOUNDBUFFER orig, LPDIRECTSOUNDBUFFER* out);
    STDMETHOD(SetCooperativeLevel)(HWND, DWORD) { return DS_OK; }
    STDMETHOD(Compact)() { return DS_OK; }
    STDMETHOD(GetSpeakerConfig)(LPDWORD c) { if (!c) return DSERR_INVALIDPARAM; *c = speakers; return DS_OK; }
    STDMETHOD(SetSpeakerConfig)(DWORD c) { speakers = c; return DS_OK; }
    STDMETHOD(Initialize)(LPCGUID) { return DSERR_ALREADYINITIALIZED; }
};

HRESULT Listener::QueryInterface(REFIID riid, LPVOID* ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == kIID_IDirectSound3DListener) { *ppv = this; AddRef(); return S_OK; }
    *ppv = NULL;
    return E_NOINTERFACE;
}
ULONG Listener::AddRef() { return ds->AddRef(); }
ULONG Listener::Release() { return ds->Release(); }

// ---------------------------------------------------------------------------
// DSBuf methods
// ---------------------------------------------------------------------------
HRESULT DSBuf::QueryInterface(REFIID riid, LPVOID* ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == kIID_IDirectSoundBuffer) { *ppv = static_cast<IDirectSoundBuffer*>(this); AddRef(); return S_OK; }
    if (riid == kIID_IDirectSound3DBuffer && (flags & DSBCAPS_CTRL3D) && !primary) {
        *ppv = static_cast<IDirectSound3DBuffer*>(this); AddRef(); return S_OK;
    }
    if (riid == kIID_IDirectSoundNotify && !primary) { *ppv = static_cast<IDirectSoundNotify*>(this); AddRef(); return S_OK; }
    if (riid == kIID_IDirectSound3DListener && primary) {
        ds->listener.AddRef();
        *ppv = &ds->listener;
        return S_OK;
    }
    *ppv = NULL;
    LogOnce("DirectSoundBuffer: unsupported interface requested");
    return E_NOINTERFACE;
}

ULONG DSBuf::Release()
{
    LONG r = InterlockedDecrement(&ref);
    if (r <= 0) {
        EnterCriticalSection(&s_lock);
        for (size_t i = 0; i < s_voices.size(); i++)
            if (s_voices[i] == this) { s_voices.erase(s_voices.begin() + i); break; }
        LeaveCriticalSection(&s_lock);
        DSound* owner = ds;
        delete this;
        if (owner) owner->Release();
        return 0;
    }
    return r;
}

HRESULT DSBuf::GetCaps(LPDSBCAPS caps)
{
    if (!caps || caps->dwSize < sizeof(DSBCAPS)) return DSERR_INVALIDPARAM;
    caps->dwFlags = flags | DSBCAPS_LOCSOFTWARE;
    caps->dwBufferBytes = primary ? 0 : Bytes();
    caps->dwUnlockTransferRate = 0;
    caps->dwPlayCpuOverhead = 0;
    return DS_OK;
}

HRESULT DSBuf::GetCurrentPosition(LPDWORD play, LPDWORD write)
{
    if (primary) { if (play) *play = 0; if (write) *write = 0; return DS_OK; }
    EnterCriticalSection(&s_lock);
    DWORD size = Bytes(), align = Align();
    DWORD p = size ? ((DWORD)pos * align) % size : 0;
    // the write cursor sits a little ahead, like hardware buffers report
    DWORD ahead = data ? (data->fmt.nAvgBytesPerSec / 66) / align * align : 0;   // ~15 ms
    DWORD w = size ? (playing ? (p + ahead) % size : p) : 0;
    LeaveCriticalSection(&s_lock);
    if (play) *play = p;
    if (write) *write = w;
    return DS_OK;
}

HRESULT DSBuf::GetFormat(LPWAVEFORMATEX fmt, DWORD size, LPDWORD written)
{
    DWORD need = sizeof(WAVEFORMATEX);
    if (written) *written = need;
    if (!fmt) return written ? DS_OK : DSERR_INVALIDPARAM;
    if (size < need) return DSERR_INVALIDPARAM;
    *fmt = data->fmt;
    fmt->cbSize = 0;
    return DS_OK;
}

HRESULT DSBuf::GetFrequency(LPDWORD f)
{
    if (!f) return DSERR_INVALIDPARAM;
    *f = freq ? freq : data->fmt.nSamplesPerSec;
    return DS_OK;
}

HRESULT DSBuf::GetStatus(LPDWORD st)
{
    if (!st) return DSERR_INVALIDPARAM;
    EnterCriticalSection(&s_lock);
    *st = (playing ? DSBSTATUS_PLAYING : 0) | (playing && looping ? DSBSTATUS_LOOPING : 0) |
          (playing ? DSBSTATUS_LOCSOFTWARE : 0);
    LeaveCriticalSection(&s_lock);
    return DS_OK;
}

HRESULT DSBuf::Lock(DWORD off, DWORD bytes, LPVOID* p1, LPDWORD b1, LPVOID* p2, LPDWORD b2, DWORD lflags)
{
    if (primary) return DSERR_PRIOLEVELNEEDED;
    DWORD size = Bytes();
    if (!size || !p1 || !b1) return DSERR_INVALIDPARAM;
    if (lflags & DSBLOCK_FROMWRITECURSOR) GetCurrentPosition(NULL, &off);
    if (lflags & DSBLOCK_ENTIREBUFFER) { off = 0; bytes = size; }
    if (off >= size || bytes > size) return DSERR_INVALIDPARAM;
    BYTE* base = data->bytes.data();
    if (off + bytes <= size) {
        *p1 = base + off; *b1 = bytes;
        if (p2) *p2 = NULL;
        if (b2) *b2 = 0;
    } else {
        *p1 = base + off; *b1 = size - off;
        if (p2) *p2 = base;
        if (b2) *b2 = bytes - (size - off);
    }
    return DS_OK;
}

HRESULT DSBuf::Play(DWORD, DWORD, DWORD pflags)
{
    if (primary) return DS_OK;
    EnterCriticalSection(&s_lock);
    looping = (pflags & DSBPLAY_LOOPING) != 0;
    if (!playing) { playing = true; started = false; }
    LeaveCriticalSection(&s_lock);
    return DS_OK;
}

HRESULT DSBuf::SetCurrentPosition(DWORD p)
{
    if (primary) return DSERR_INVALIDCALL;
    EnterCriticalSection(&s_lock);
    DWORD f = Frames();
    pos = f ? (double)((p / Align()) % f) : 0;
    LeaveCriticalSection(&s_lock);
    return DS_OK;
}

HRESULT DSBuf::SetFormat(LPCWAVEFORMATEX fmt)
{
    if (!primary) return DSERR_INVALIDCALL;
    if (fmt) data->fmt = *fmt;
    return DS_OK;
}

HRESULT DSBuf::SetVolume(LONG v)
{
    if (v < DSBVOLUME_MIN || v > DSBVOLUME_MAX) return DSERR_INVALIDPARAM;
    vol = v;
    if (primary) s_primaryGain = MbToGain(v);
    return DS_OK;
}

HRESULT DSBuf::SetPan(LONG p)
{
    if (p < DSBPAN_LEFT || p > DSBPAN_RIGHT) return DSERR_INVALIDPARAM;
    pan = p;
    return DS_OK;
}

HRESULT DSBuf::SetFrequency(DWORD f)
{
    if (f && (f < DSBFREQUENCY_MIN || f > DSBFREQUENCY_MAX)) return DSERR_INVALIDPARAM;
    freq = f;
    return DS_OK;
}

HRESULT DSBuf::Stop()
{
    if (primary) return DS_OK;
    EnterCriticalSection(&s_lock);
    bool was = playing;
    playing = false;
    if (was) FireStop();
    LeaveCriticalSection(&s_lock);
    return DS_OK;
}

HRESULT DSBuf::SetAllParameters(LPCDS3DBUFFER p, DWORD)
{
    if (!p) return DSERR_INVALIDPARAM;
    EnterCriticalSection(&s_lock);
    p3d = *p;
    p3d.dwSize = sizeof(p3d);
    LeaveCriticalSection(&s_lock);
    return DS_OK;
}
HRESULT DSBuf::SetConeAngles(DWORD in, DWORD out, DWORD) { p3d.dwInsideConeAngle = in; p3d.dwOutsideConeAngle = out; return DS_OK; }
HRESULT DSBuf::SetConeOrientation(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD)
{
    EnterCriticalSection(&s_lock);
    p3d.vConeOrientation.x = x; p3d.vConeOrientation.y = y; p3d.vConeOrientation.z = z;
    LeaveCriticalSection(&s_lock);
    return DS_OK;
}
HRESULT DSBuf::SetConeOutsideVolume(LONG v, DWORD) { p3d.lConeOutsideVolume = v; return DS_OK; }
HRESULT DSBuf::SetMaxDistance(D3DVALUE d, DWORD) { p3d.flMaxDistance = d; return DS_OK; }
HRESULT DSBuf::SetMinDistance(D3DVALUE d, DWORD) { p3d.flMinDistance = d; return DS_OK; }
HRESULT DSBuf::SetMode(DWORD m, DWORD) { p3d.dwMode = m; return DS_OK; }
HRESULT DSBuf::SetPosition(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD)
{
    EnterCriticalSection(&s_lock);
    p3d.vPosition.x = x; p3d.vPosition.y = y; p3d.vPosition.z = z;
    LeaveCriticalSection(&s_lock);
    return DS_OK;
}
HRESULT DSBuf::SetVelocity(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD)
{
    EnterCriticalSection(&s_lock);
    p3d.vVelocity.x = x; p3d.vVelocity.y = y; p3d.vVelocity.z = z;
    LeaveCriticalSection(&s_lock);
    return DS_OK;
}

HRESULT DSBuf::SetNotificationPositions(DWORD n, LPCDSBPOSITIONNOTIFY p)
{
    if (n && !p) return DSERR_INVALIDPARAM;
    EnterCriticalSection(&s_lock);
    if (playing) { LeaveCriticalSection(&s_lock); return DSERR_INVALIDCALL; }
    notes.assign(p, p + n);
    LeaveCriticalSection(&s_lock);
    return DS_OK;
}

// ---------------------------------------------------------------------------
// DSound methods
// ---------------------------------------------------------------------------
HRESULT DSound::CreateSoundBuffer(LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER* out, LPUNKNOWN)
{
    if (!desc || !out) return DSERR_INVALIDPARAM;
    *out = NULL;
    DSBuf* b = new DSBuf();
    b->ds = this;
    b->flags = desc->dwFlags;
    b->data = std::make_shared<SoundData>();
    if (desc->dwFlags & DSBCAPS_PRIMARYBUFFER) {
        b->primary = true;
        WAVEFORMATEX& f = b->data->fmt;
        ZeroMemory(&f, sizeof(f));
        f.wFormatTag = WAVE_FORMAT_PCM;
        f.nChannels = 2;
        f.nSamplesPerSec = 22050;
        f.wBitsPerSample = 16;
        f.nBlockAlign = 4;
        f.nAvgBytesPerSec = 22050 * 4;
    } else {
        if (!desc->lpwfxFormat || !desc->dwBufferBytes) { delete b; return DSERR_INVALIDPARAM; }
        const WAVEFORMATEX* f = desc->lpwfxFormat;
        if (f->wFormatTag != WAVE_FORMAT_PCM || (f->wBitsPerSample != 8 && f->wBitsPerSample != 16) ||
            f->nChannels < 1 || f->nChannels > 2) {
            Log("DirectSound: unsupported buffer format (tag %u, %u bit, %u ch)", f->wFormatTag, f->wBitsPerSample, f->nChannels);
            delete b;
            return DSERR_BADFORMAT;
        }
        b->data->fmt = *f;
        b->data->fmt.cbSize = 0;
        b->data->fmt.nBlockAlign = (WORD)(f->nChannels * f->wBitsPerSample / 8);
        b->data->fmt.nAvgBytesPerSec = f->nSamplesPerSec * b->data->fmt.nBlockAlign;
        DWORD size = desc->dwBufferBytes / b->data->fmt.nBlockAlign * b->data->fmt.nBlockAlign;
        b->data->bytes.assign(size, f->wBitsPerSample == 8 ? 0x80 : 0);
        EnterCriticalSection(&s_lock);
        s_voices.push_back(b);
        LeaveCriticalSection(&s_lock);
    }
    AddRef();   // buffers keep their DirectSound object alive
    LogV("DirectSound: CreateSoundBuffer flags=%08X bytes=%u %uHz %u-bit %uch", desc->dwFlags, desc->dwBufferBytes,
         b->data->fmt.nSamplesPerSec, b->data->fmt.wBitsPerSample, b->data->fmt.nChannels);
    *out = b;
    return DS_OK;
}

HRESULT DSound::GetCaps(LPDSCAPS caps)
{
    if (!caps || caps->dwSize < sizeof(DSCAPS)) return DSERR_INVALIDPARAM;
    DWORD size = caps->dwSize;
    ZeroMemory(caps, size);
    caps->dwSize = size;
    caps->dwFlags = DSCAPS_PRIMARYMONO | DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY8BIT | DSCAPS_PRIMARY16BIT |
                    DSCAPS_CONTINUOUSRATE | DSCAPS_SECONDARYMONO | DSCAPS_SECONDARYSTEREO |
                    DSCAPS_SECONDARY8BIT | DSCAPS_SECONDARY16BIT;
    caps->dwMinSecondarySampleRate = DSBFREQUENCY_MIN;
    caps->dwMaxSecondarySampleRate = DSBFREQUENCY_MAX;
    caps->dwPrimaryBuffers = 1;
    return DS_OK;
}

HRESULT DSound::DuplicateSoundBuffer(LPDIRECTSOUNDBUFFER origI, LPDIRECTSOUNDBUFFER* out)
{
    DSBuf* o = static_cast<DSBuf*>(origI);
    if (!o || !out || o->primary) return DSERR_INVALIDPARAM;
    DSBuf* b = new DSBuf();
    b->ds = this;
    b->flags = o->flags;
    b->data = o->data;   // sample memory is shared, like DirectSound
    EnterCriticalSection(&s_lock);
    b->vol = o->vol; b->pan = o->pan; b->freq = o->freq; b->p3d = o->p3d;
    s_voices.push_back(b);
    LeaveCriticalSection(&s_lock);
    AddRef();
    *out = b;
    return DS_OK;
}

// ---------------------------------------------------------------------------
// Mixing
// ---------------------------------------------------------------------------
static inline void ReadFrame(const SoundData& d, DWORD frame, float& l, float& r)
{
    const BYTE* p = d.bytes.data() + (size_t)frame * d.fmt.nBlockAlign;
    if (d.fmt.wBitsPerSample == 16) {
        const short* s = (const short*)p;
        l = s[0] * (1.0f / 32768.0f);
        r = d.fmt.nChannels == 2 ? s[1] * (1.0f / 32768.0f) : l;
    } else {
        l = ((int)p[0] - 128) * (1.0f / 128.0f);
        r = d.fmt.nChannels == 2 ? ((int)p[1] - 128) * (1.0f / 128.0f) : l;
    }
}

// Target channel gains and a pitch factor (Doppler) for a voice.
static void VoiceParams(const DSBuf* b, float& gl, float& gr, float& pitch)
{
    float g = MbToGain(b->vol);
    pitch = 1.0f;
    bool spatial = (b->flags & DSBCAPS_CTRL3D) && b->p3d.dwMode != DS3DMODE_DISABLE;
    if (!spatial) {
        gl = g * (b->pan > 0 ? MbToGain(-b->pan) : 1.0f);
        gr = g * (b->pan < 0 ? MbToGain(b->pan) : 1.0f);
        return;
    }

    const DS3DLISTENER& L = s_listener;
    V3 front = Norm(FromD3D(L.vOrientFront));
    V3 top = Norm(FromD3D(L.vOrientTop));
    V3 right = Norm(Cross(top, front));   // left-handed: x right, y up, z forward
    V3 up = Cross(front, right);
    V3 rel = FromD3D(b->p3d.vPosition);
    V3 local;
    if (b->p3d.dwMode == DS3DMODE_HEADRELATIVE) {
        local = rel;
    } else {
        V3 d = Sub(rel, FromD3D(L.vPosition));
        local = { Dot(d, right), Dot(d, up), Dot(d, front) };
    }
    float dist = Len(local);

    // distance attenuation (inverse distance law with rolloff)
    float minD = b->p3d.flMinDistance > 0 ? b->p3d.flMinDistance : 1.0f;
    float maxD = b->p3d.flMaxDistance > minD ? b->p3d.flMaxDistance : minD;
    if (dist > maxD) {
        if (b->flags & DSBCAPS_MUTE3DATMAXDISTANCE) { gl = gr = 0; return; }
        dist = maxD;
    }
    float atten = dist <= minD ? 1.0f : powf(minD / dist, L.flRolloffFactor > 0 ? L.flRolloffFactor : 0.0f);

    // sound cone
    if (b->p3d.dwInsideConeAngle < 360 && b->p3d.dwMode != DS3DMODE_HEADRELATIVE) {
        V3 toListener = Norm(Sub(FromD3D(L.vPosition), rel));
        float c = Dot(Norm(FromD3D(b->p3d.vConeOrientation)), toListener);
        float ang = acosf(c < -1 ? -1 : c > 1 ? 1 : c) * 2.0f * 57.29578f;   // full angle in degrees
        float in = (float)b->p3d.dwInsideConeAngle, out = (float)b->p3d.dwOutsideConeAngle;
        float og = MbToGain(b->p3d.lConeOutsideVolume);
        if (ang > in) atten *= ang >= out || out <= in ? og : 1.0f + (og - 1.0f) * (ang - in) / (out - in);
    }

    // constant-power pan from the horizontal direction, full level at centre
    float px = dist > 1e-4f ? local.x / dist : 0.0f;
    float a = (px + 1.0f) * 0.785398f;   // 0..pi/2
    float l = cosf(a) * 1.41421f, r = sinf(a) * 1.41421f;
    gl = g * atten * (l > 1 ? 1 : l);
    gr = g * atten * (r > 1 ? 1 : r);

    // Doppler shift
    if (L.flDopplerFactor > 0 && b->p3d.dwMode != DS3DMODE_HEADRELATIVE) {
        float df = L.flDistanceFactor > 0 ? L.flDistanceFactor : 1.0f;
        float c = 343.3f / df;   // speed of sound in game units per second
        V3 u = Norm(Sub(rel, FromD3D(L.vPosition)));
        float vl = Dot(FromD3D(L.vVelocity), u) * L.flDopplerFactor;
        float vs = Dot(FromD3D(b->p3d.vVelocity), u) * L.flDopplerFactor;
        if (c + vs > 1e-3f) {
            pitch = (c + vl) / (c + vs);
            if (pitch < 0.5f) pitch = 0.5f;
            if (pitch > 2.0f) pitch = 2.0f;
        }
    }
}

static void MixVoice(DSBuf* b, float* out, int frames, int rate)
{
    const SoundData& d = *b->data;
    DWORD total = b->Frames();
    if (!b->playing || !total) return;

    float tl, tr, pitch;
    VoiceParams(b, tl, tr, pitch);
    float master = s_primaryGain * g_cfg.audioVolume / 100.0f;
    tl *= master;
    tr *= master;
    if (!b->started) { b->gainL = tl; b->gainR = tr; b->started = true; }   // no ramp-in from silence on start

    double step = (double)(b->freq ? b->freq : d.fmt.nSamplesPerSec) * pitch / rate;
    float gl = b->gainL, gr = b->gainR;
    float dl = (tl - gl) / frames, dr = (tr - gr) / frames;
    DWORD align = d.fmt.nBlockAlign, size = b->Bytes();
    DWORD startByte = ((DWORD)b->pos * align) % size;
    bool wrapped = false;

    for (int i = 0; i < frames; i++) {
        DWORD i0 = (DWORD)b->pos;
        float frac = (float)(b->pos - i0);
        DWORD i1 = i0 + 1;
        if (i1 >= total) i1 = b->looping ? 0 : i0;
        float l0, r0, l1, r1;
        ReadFrame(d, i0, l0, r0);
        ReadFrame(d, i1, l1, r1);
        float l = l0 + (l1 - l0) * frac, r = r0 + (r1 - r0) * frac;
        if ((b->flags & DSBCAPS_CTRL3D) && b->p3d.dwMode != DS3DMODE_DISABLE) l = r = (l + r) * 0.5f;
        gl += dl;
        gr += dr;
        out[i * 2] += l * gl;
        out[i * 2 + 1] += r * gr;
        b->pos += step;
        if (b->pos >= total) {
            if (b->looping) {
                b->pos = fmod(b->pos, (double)total);
                wrapped = true;
            } else {
                b->FireNotes(startByte, size, false);
                b->playing = false;
                b->pos = 0;
                b->FireStop();
                b->gainL = gl; b->gainR = gr;
                return;
            }
        }
    }
    b->gainL = tl;
    b->gainR = tr;
    DWORD endByte = ((DWORD)b->pos * align) % size;
    if (wrapped || endByte != startByte) b->FireNotes(startByte, endByte, wrapped || endByte < startByte);
}

static void MixAll(float* out, int frames, int rate)
{
    memset(out, 0, sizeof(float) * frames * 2);
    EnterCriticalSection(&s_lock);
    int active = 0;
    for (DSBuf* b : s_voices) {
        if (b->playing) active++;
        MixVoice(b, out, frames, rate);
    }
    size_t count = s_voices.size();
    LeaveCriticalSection(&s_lock);

    // verbose log: activity summary every ~2 seconds
    if (g_cfg.logLevel >= 2) {
        static float peak;
        static int maxActive, acc;
        for (int i = 0; i < frames * 2; i++) { float a = fabsf(out[i]); if (a > peak) peak = a; }
        if (active > maxActive) maxActive = active;
        acc += frames;
        if (acc >= rate * 2) {
            Log("Audio: %u buffers, up to %d playing, peak %.3f", (unsigned)count, maxActive, peak);
            peak = 0; maxActive = 0; acc = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// WASAPI output
// ---------------------------------------------------------------------------
struct DeviceWatch : IMMNotificationClient {
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv)
    {
        if (riid == IID_IUnknown || riid == kIID_IMMNotificationClient) { *ppv = this; return S_OK; }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() { return 1; }
    STDMETHOD_(ULONG, Release)() { return 1; }
    STDMETHOD(OnDeviceStateChanged)(LPCWSTR, DWORD) { return S_OK; }
    STDMETHOD(OnDeviceAdded)(LPCWSTR) { return S_OK; }
    STDMETHOD(OnDeviceRemoved)(LPCWSTR) { return S_OK; }
    STDMETHOD(OnDefaultDeviceChanged)(EDataFlow flow, ERole role, LPCWSTR)
    {
        if (flow == eRender && role == eConsole && !g_cfg.audioDevice[0]) InterlockedExchange(&s_reinit, 1);
        return S_OK;
    }
    STDMETHOD(OnPropertyValueChanged)(LPCWSTR, const PROPERTYKEY) { return S_OK; }
};
static DeviceWatch s_watch;

struct Output {
    IMMDevice* dev = NULL;
    IAudioClient* client = NULL;
    IAudioRenderClient* render = NULL;
    HANDLE event = NULL;
    UINT32 bufferFrames = 0;
    int channels = 2, rate = 48000;
    bool isFloat = true;
    int bits = 32;
};

static void CloseOutput(Output& o)
{
    if (o.client) o.client->Stop();
    SafeRel(o.render);
    SafeRel(o.client);
    SafeRel(o.dev);
}

// Finds an output whose friendly name contains `want` (case-insensitive).
static IMMDevice* FindDevice(IMMDeviceEnumerator* en, const char* want)
{
    IMMDeviceCollection* list = NULL;
    IMMDevice* found = NULL;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &list))) return NULL;
    UINT n = 0;
    list->GetCount(&n);
    WCHAR wantW[128];
    MultiByteToWideChar(CP_ACP, 0, want, -1, wantW, 128);
    _wcslwr_s(wantW);
    for (UINT i = 0; i < n && !found; i++) {
        IMMDevice* d = NULL;
        IPropertyStore* ps = NULL;
        if (FAILED(list->Item(i, &d))) continue;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(kPKEY_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
                WCHAR name[256];
                wcsncpy_s(name, v.pwszVal, _TRUNCATE);
                _wcslwr_s(name);
                if (wcsstr(name, wantW)) { found = d; d = NULL; }
            }
            PropVariantClear(&v);
            ps->Release();
        }
        SafeRel(d);
    }
    list->Release();
    return found;
}

static bool OpenOutput(Output& o, IMMDeviceEnumerator* en)
{
    if (g_cfg.audioDevice[0]) {
        o.dev = FindDevice(en, g_cfg.audioDevice);
        if (!o.dev) LogOnce("Audio: configured device not found, using the default output");
    }
    if (!o.dev && FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &o.dev))) return false;

    WAVEFORMATEX* mix = NULL;
    IAudioClient3* c3 = NULL;
    bool lowLatency = false;
    if (SUCCEEDED(o.dev->Activate(kIID_IAudioClient3, CLSCTX_ALL, NULL, (void**)&c3))) {
        o.client = c3;
    } else if (FAILED(o.dev->Activate(kIID_IAudioClient, CLSCTX_ALL, NULL, (void**)&o.client))) {
        return false;
    }
    if (FAILED(o.client->GetMixFormat(&mix))) return false;
    o.channels = mix->nChannels;
    o.rate = mix->nSamplesPerSec;
    o.bits = mix->wBitsPerSample;
    o.isFloat = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((WAVEFORMATEXTENSIBLE*)mix)->SubFormat == kKSDATAFORMAT_FLOAT);

    HRESULT hr = E_FAIL;
    if (c3) {
        UINT32 def = 0, fund = 0, minP = 0, maxP = 0;
        if (SUCCEEDED(c3->GetSharedModeEnginePeriod(mix, &def, &fund, &minP, &maxP)) && fund) {
            UINT32 want = (UINT32)((UINT64)o.rate * g_cfg.audioLatency / 1000);
            UINT32 period = want / fund * fund;
            if (period < minP) period = minP;
            if (period > maxP) period = maxP;
            hr = c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, mix, NULL);
            if (SUCCEEDED(hr)) {
                lowLatency = true;
                Log("Audio: low-latency period %u frames (%.1f ms)", period, period * 1000.0 / o.rate);
            }
        }
    }
    if (FAILED(hr)) {
        if (c3) {   // a failed IAudioClient3 init leaves the client unusable
            SafeRel(o.client);
            if (FAILED(o.dev->Activate(kIID_IAudioClient, CLSCTX_ALL, NULL, (void**)&o.client))) { CoTaskMemFree(mix); return false; }
        }
        REFERENCE_TIME dur = (REFERENCE_TIME)g_cfg.audioLatency * 2 * 10000;
        hr = o.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, 0, mix, NULL);
    }
    CoTaskMemFree(mix);
    if (FAILED(hr)) { Log("Audio: IAudioClient init failed 0x%08X", hr); return false; }

    if (!o.event) o.event = CreateEventW(NULL, FALSE, FALSE, NULL);
    o.client->SetEventHandle(o.event);
    o.client->GetBufferSize(&o.bufferFrames);
    if (FAILED(o.client->GetService(kIID_IAudioRenderClient, (void**)&o.render))) return false;
    InterlockedExchange(&s_outRate, o.rate);

    IPropertyStore* ps = NULL;
    if (SUCCEEDED(o.dev->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(kPKEY_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
            char name[256];
            WideCharToMultiByte(CP_ACP, 0, v.pwszVal, -1, name, sizeof(name), NULL, NULL);
            Log("Audio: output \"%s\" %d Hz, %d ch, %s %d-bit, buffer %u frames%s", name, o.rate, o.channels,
                o.isFloat ? "float" : "PCM", o.bits, o.bufferFrames, lowLatency ? "" : " (standard latency)");
        }
        PropVariantClear(&v);
        ps->Release();
    }
    return SUCCEEDED(o.client->Start());
}

// Stereo-linked peak limiter: when several loud sounds overlap the sum can
// reach 2-3x full scale, which plain DirectSound clips harshly.  The gain
// drops instantly to keep peaks under the ceiling and recovers over ~250 ms.
static void Limit(float* mix, UINT32 frames, int rate)
{
    static float gain = 1.0f;
    const float ceiling = 0.97f;
    const float release = 1.0f - expf(-1.0f / (0.25f * rate));
    for (UINT32 i = 0; i < frames; i++) {
        float l = mix[i * 2], r = mix[i * 2 + 1];
        float peak = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
        float need = peak * gain > ceiling ? ceiling / peak : 1.0f;
        if (need < gain) gain = need;                  // attack: immediate
        else gain += (1.0f - gain) * release;          // release: smooth
        if (peak * gain > ceiling) gain = ceiling / peak;
        mix[i * 2] = l * gain;
        mix[i * 2 + 1] = r * gain;
    }
}

// Converts the stereo float mix into the device format.
static void WriteOut(const Output& o, BYTE* dst, const float* mix, UINT32 frames)
{
    for (UINT32 i = 0; i < frames; i++) {
        float l = mix[i * 2], r = mix[i * 2 + 1];
        if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
        if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
        for (int c = 0; c < o.channels; c++) {
            float v = o.channels == 1 ? (l + r) * 0.5f : c == 0 ? l : c == 1 ? r : 0.0f;
            if (o.isFloat) {
                ((float*)dst)[i * o.channels + c] = v;
            } else if (o.bits == 16) {
                ((short*)dst)[i * o.channels + c] = (short)(v * 32767.0f);
            } else if (o.bits == 24) {
                int s = (int)(v * 8388607.0f);
                BYTE* p = dst + ((size_t)i * o.channels + c) * 3;
                p[0] = (BYTE)s; p[1] = (BYTE)(s >> 8); p[2] = (BYTE)(s >> 16);
            } else {
                ((int*)dst)[i * o.channels + c] = (int)(v * 2147483647.0f);
            }
        }
    }
}

static DWORD WINAPI MixerThread(LPVOID)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);

    IMMDeviceEnumerator* en = NULL;
    CoCreateInstance(kCLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void**)&en);
    if (en) en->RegisterEndpointNotificationCallback(&s_watch);

    Output o;
    std::vector<float> mix;
    bool open = false;
    LARGE_INTEGER qpf, last;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&last);

    while (!s_quit) {
        if (!open || InterlockedExchange(&s_reinit, 0)) {
            CloseOutput(o);
            open = en && OpenOutput(o, en);
            if (!open) CloseOutput(o);
            QueryPerformanceCounter(&last);
        }

        if (!open) {
            // No usable device: keep voices advancing in real time so the
            // game's streaming (which waits on notifications) never stalls.
            Sleep(10);
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            int frames = (int)((now.QuadPart - last.QuadPart) * s_outRate / qpf.QuadPart);
            if (frames > 0) {
                if (frames > s_outRate / 10) frames = s_outRate / 10;
                mix.resize((size_t)frames * 2);
                MixAll(mix.data(), frames, s_outRate);
                last = now;
            }
            static DWORD retry;
            if (GetTickCount() - retry > 2000) { retry = GetTickCount(); InterlockedExchange(&s_reinit, 1); }
            continue;
        }

        DWORD w = WaitForSingleObject(o.event, 200);
        if (w != WAIT_OBJECT_0) continue;
        UINT32 padding = 0;
        HRESULT hr = o.client->GetCurrentPadding(&padding);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { Log("Audio: device lost, reopening"); open = false; continue; }
        if (FAILED(hr)) continue;
        UINT32 frames = o.bufferFrames - padding;
        if (!frames) continue;
        BYTE* dst = NULL;
        hr = o.render->GetBuffer(frames, &dst);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { Log("Audio: device lost, reopening"); open = false; continue; }
        if (FAILED(hr)) continue;
        mix.resize((size_t)frames * 2);
        MixAll(mix.data(), (int)frames, o.rate);
        if (g_cfg.audioLimiter) Limit(mix.data(), frames, o.rate);
        WriteOut(o, dst, mix.data(), frames);
        o.render->ReleaseBuffer(frames, 0);
    }

    CloseOutput(o);
    if (o.event) CloseHandle(o.event);
    if (en) { en->UnregisterEndpointNotificationCallback(&s_watch); en->Release(); }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
    return 0;
}

static void StartMixer()
{
    if (s_started) return;
    s_started = true;
    ZeroMemory(&s_listener, sizeof(s_listener));
    s_listener.dwSize = sizeof(s_listener);
    s_listener.vOrientFront.z = 1.0f;
    s_listener.vOrientTop.y = 1.0f;
    s_listener.flDistanceFactor = DS3D_DEFAULTDISTANCEFACTOR;
    s_listener.flRolloffFactor = DS3D_DEFAULTROLLOFFFACTOR;
    s_listener.flDopplerFactor = DS3D_DEFAULTDOPPLERFACTOR;
    s_thread = CreateThread(NULL, 0, MixerThread, NULL, 0, NULL);
}

// ---------------------------------------------------------------------------
// Entry point replacing DSOUND.dll!DirectSoundCreate (ordinal 1)
// ---------------------------------------------------------------------------
static HRESULT WINAPI H_DirectSoundCreate(LPCGUID, LPDIRECTSOUND* out, LPUNKNOWN outer)
{
    if (!out) return DSERR_INVALIDPARAM;
    if (outer) return DSERR_NOAGGREGATION;
    StartMixer();
    *out = new DSound();
    Log("DirectSound: using hod2fix mixer (WASAPI)");
    return DS_OK;
}

static bool PatchOrdinal(HMODULE mod, const char* dll, WORD ordinal, void* hook)
{
    BYTE* base = (BYTE*)mod;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (lstrcmpiA((const char*)(base + imp->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA* addrs = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++) {
            if (!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal) || IMAGE_ORDINAL(names->u1.Ordinal) != ordinal) continue;
            DWORD old;
            VirtualProtect(&addrs->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            addrs->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&addrs->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

void Audio_InstallHooks()
{
    InitializeCriticalSection(&s_lock);
    if (!g_cfg.audioEnabled) { Log("Audio: using Windows DirectSound (hod2fix audio disabled)"); return; }
    if (PatchOrdinal(GetModuleHandleA(NULL), "DSOUND.dll", 1, (void*)H_DirectSoundCreate))
        LogV("hooked DSOUND.dll!DirectSoundCreate");
}

void Audio_Shutdown()
{
    if (!s_thread) return;
    s_quit = true;
    WaitForSingleObject(s_thread, 500);
    CloseHandle(s_thread);
    s_thread = NULL;
}
