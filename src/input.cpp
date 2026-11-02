// hod2fix - hotkeys read from the game's own DirectInput keyboard.
//
// Once the game acquires its DirectInput keyboard, Windows stops turning
// keystrokes into window messages for it, so hooks and WM_KEYDOWN never see
// them.  Instead we watch the key data the game itself reads: DirectInputCreateEx
// is intercepted, keyboard devices are recorded at creation, and their
// GetDeviceState/GetDeviceData calls are inspected (results are not modified).
#define DIRECTINPUT_VERSION 0x0700
#include "common.h"
#include "renderer.h"
#include <dinput.h>

typedef HRESULT (WINAPI *PFN_DICreateEx)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDeviceEx)(IDirectInput7A*, REFGUID, REFIID, LPVOID*, LPUNKNOWN);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetDeviceState)(IDirectInputDevice7A*, DWORD, LPVOID);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetDeviceData)(IDirectInputDevice7A*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

static PFN_DICreateEx      s_realCreateEx;
static PFN_CreateDeviceEx  s_realCreateDeviceEx;
static PFN_GetDeviceState  s_realGetState;
static PFN_GetDeviceData   s_realGetData;
static void*               s_keyboards[4];
static BYTE                s_prevDown[256];

// DirectInput key codes are scan codes; extended keys get bit 7 set.
static int VkToDik(int vk)
{
    if (!vk) return -1;
    if (vk == VK_SNAPSHOT) return DIK_SYSRQ;
    UINT sc = MapVirtualKeyA(vk, 4 /*MAPVK_VK_TO_VSC_EX*/);
    if (!sc) return -1;
    if ((sc & 0xFF00) == 0xE000) return (sc & 0x7F) | 0x80;
    return sc & 0xFF;
}

static bool IsKeyboard(void* dev)
{
    for (void* k : s_keyboards) if (k && k == dev) return true;
    return false;
}

static void KeyEvent(int dik, bool down)
{
    if (dik < 0 || dik > 255) return;
    bool was = s_prevDown[dik] != 0;
    s_prevDown[dik] = down;
    if (!down || was) return;
    if (dik == VkToDik(g_cfg.screenshotKey)) R::Hotkey(R::HOTKEY_SCREENSHOT);
    if (dik == VkToDik(g_cfg.haloToggleKey)) R::Hotkey(R::HOTKEY_HALO);
    if (dik == VkToDik(g_cfg.smoothToggleKey)) R::Hotkey(R::HOTKEY_SMOOTH);
    if (dik == VkToDik(g_cfg.probeKey)) R::Hotkey(R::HOTKEY_PROBE);
}

static HRESULT STDMETHODCALLTYPE H_GetDeviceState(IDirectInputDevice7A* self, DWORD size, LPVOID data)
{
    HRESULT hr = s_realGetState(self, size, data);
    if (SUCCEEDED(hr) && data && size >= 256 && IsKeyboard(self)) {
        const BYTE* keys = (const BYTE*)data;
        int watch[4] = { VkToDik(g_cfg.screenshotKey), VkToDik(g_cfg.haloToggleKey), VkToDik(g_cfg.smoothToggleKey),
                         VkToDik(g_cfg.probeKey) };
        for (int k : watch)
            if (k >= 0) KeyEvent(k, (keys[k] & 0x80) != 0);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_GetDeviceData(IDirectInputDevice7A* self, DWORD cb, LPDIDEVICEOBJECTDATA data,
                                                 LPDWORD count, DWORD flags)
{
    HRESULT hr = s_realGetData(self, cb, data, count, flags);
    if (SUCCEEDED(hr) && data && count && !(flags & DIGDD_PEEK) && cb >= sizeof(DIDEVICEOBJECTDATA_DX3) &&
        IsKeyboard(self)) {
        for (DWORD i = 0; i < *count; i++) {
            const DIDEVICEOBJECTDATA* d = (const DIDEVICEOBJECTDATA*)((const BYTE*)data + i * cb);
            KeyEvent((int)d->dwOfs, (d->dwData & 0x80) != 0);
        }
    }
    return hr;
}

static void PatchSlot(void** vtbl, int index, void* hook, void** orig)
{
    if (vtbl[index] == hook) return;
    DWORD old;
    VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    if (orig && !*orig) *orig = vtbl[index];
    vtbl[index] = hook;
    VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
}

static HRESULT STDMETHODCALLTYPE H_CreateDeviceEx(IDirectInput7A* self, REFGUID guid, REFIID iid, LPVOID* out, LPUNKNOWN outer)
{
    HRESULT hr = s_realCreateDeviceEx(self, guid, iid, out, outer);
    if (SUCCEEDED(hr) && out && *out && guid == GUID_SysKeyboard) {
        for (void*& k : s_keyboards) if (!k) { k = *out; break; }
        // all DirectInput devices share one vtable; the hooks filter by device
        void** vtbl = *(void***)*out;
        PatchSlot(vtbl, 9, (void*)H_GetDeviceState, (void**)&s_realGetState);
        PatchSlot(vtbl, 10, (void*)H_GetDeviceData, (void**)&s_realGetData);
        Log("watching DirectInput keyboard for hotkeys");
    }
    return hr;
}

static HRESULT WINAPI H_DirectInputCreateEx(HINSTANCE inst, DWORD ver, REFIID iid, LPVOID* out, LPUNKNOWN outer)
{
    HRESULT hr = s_realCreateEx(inst, ver, iid, out, outer);
    if (SUCCEEDED(hr) && out && *out && iid == IID_IDirectInput7A) {
        void** vtbl = *(void***)*out;
        PatchSlot(vtbl, 9, (void*)H_CreateDeviceEx, (void**)&s_realCreateDeviceEx);   // CreateDeviceEx
    }
    return hr;
}

void Input_InstallHooks(void (*patchIAT)(HMODULE, const char*, const char*, void*))
{
    HMODULE di = LoadLibraryA("dinput.dll");
    s_realCreateEx = di ? (PFN_DICreateEx)GetProcAddress(di, "DirectInputCreateEx") : NULL;
    if (s_realCreateEx)
        patchIAT(GetModuleHandleA(NULL), "DINPUT.dll", "DirectInputCreateEx", (void*)H_DirectInputCreateEx);
}
