#include "me2_menu.h"

#include "calcview_hook.h"
#include "convo_fp.h"
#include "d3d_capture.h"
#include "engine_probe.h"
#include "logger.h"
#include "hud_probe.h"
#include "me2_xr.h"

#include <cstdint>

#include <Windows.h>
#include <Xinput.h>
#include <MinHook.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"

namespace ME2VR::Menu
{
namespace
{
// [PROFILES] four settings snapshots, hotkeys 1-4 (ME2 parity). A slot is just a copy of the ini,
// so a slot that was never saved has no file and the mod keeps the current values rather than wiping them.
constexpr int kProfileCount = 4;
bool g_profileHotkeys = true;
int  g_profileKey[kProfileCount] = { 0x31, 0x32, 0x33, 0x34 };   // 1 2 3 4
int  g_activeProfile = 0;
int  g_rebindingProfile = -1;
void LoadProfile(int slot) noexcept;   // defined below, used by the hotkey poll above it
const char* ProfileName(int slot) noexcept
{
    switch (slot) { case 1: return "Profile 2"; case 2: return "Profile 3"; case 3: return "Profile 4"; default: return "Default"; }
}
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11Texture2D* g_tex = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
int g_w = 900;
int g_h = 720;
bool g_ready = false;
bool g_open = false;
bool g_disableDof = false;  // applied to GamerSettings.ini on next launch by d3d_capture
// Rebindable hotkeys (ME2 parity). ME3 shipped with these hard-wired and no way to change them.
int  g_recenterKey = 'R';        // VK code
int  g_fpToggleKey = 'K';
bool g_rebindingRecenter = false;
bool g_rebindingFpToggle = false;
int  g_menuKey = VK_INSERT;
bool g_rebindingMenuKey = false;

const char* KeyName(int vk) noexcept
{
    static char buf[16];
    if (vk >= 'A' && vk <= 'Z') { buf[0] = static_cast<char>(vk); buf[1] = '\0'; return buf; }
    if (vk >= '0' && vk <= '9') { buf[0] = static_cast<char>(vk); buf[1] = '\0'; return buf; }
    if (vk >= VK_F1 && vk <= VK_F12) { sprintf_s(buf, "F%d", vk - VK_F1 + 1); return buf; }
    switch (vk)
    {
        case VK_SPACE: return "Space";  case VK_TAB: return "Tab";   case VK_RETURN: return "Enter";
        case VK_LSHIFT: case VK_SHIFT: return "Shift";  case VK_LCONTROL: case VK_CONTROL: return "Ctrl";
        case VK_OEM_3: return "`";  case VK_HOME: return "Home";  case VK_END: return "End";
        case VK_INSERT: return "Insert";  case VK_DELETE: return "Delete";
        case VK_PRIOR: return "Page Up";  case VK_NEXT: return "Page Down";
        case VK_LEFT: return "Left";  case VK_RIGHT: return "Right";
        case VK_UP: return "Up";  case VK_DOWN: return "Down";
        case VK_BACK: return "Backspace";
    }
    sprintf_s(buf, "0x%02X", vk);
    return buf;
}

// Shared rebind widget: ESC cancels; mouse buttons and the current menu key are reserved.
// Returns true only when a new key was captured, so the caller can persist it immediately.
bool RebindWidget(const char* label, int* key, bool* arming) noexcept
{
    ImGui::Text("%s: %s", label, KeyName(*key));
    ImGui::SameLine();
    ImGui::PushID(label);
    if (ImGui::SmallButton(*arming ? "press a key..." : "Rebind")) *arming = true;
    ImGui::PopID();
    if (!*arming) return false;
    for (int vk = 0x08; vk <= 0xFE; ++vk)
    {
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == g_menuKey) continue;
        if ((GetAsyncKeyState(vk) & 0x8000) == 0) continue;
        if (vk == VK_ESCAPE) { *arming = false; return false; }
        *key = vk;
        *arming = false;
        return true;
    }
    return false;
}

bool AnyRebinding() noexcept
{
    return g_rebindingMenuKey || g_rebindingRecenter || g_rebindingFpToggle || g_rebindingProfile >= 0;
}

// ===================== [MOVEFIX] XInput hooks (ported from ME2, 2026-07-28) =====================
// ME3 had NO XInput hooks at all, which cost two things at once: no live left-stick reading for the
// sprint latch (head aim stayed on while storming, so looking left and right went the wrong way), and
// "run where you look" was simply absent. Both come off this one hook.
// HeadLookYawUU is the EXACT applied offset and is 0 whenever look is off (menus, cine, head-aim), so
// the rotation is an exact no-op there. Also zeroes the pad while the Insert menu is open.
using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetState_t g_origXInput = nullptr;
XInputGetState_t g_origXInputEx = nullptr;   // XInputGetStateEx (ordinal 100) trampoline
std::atomic<int> g_leftStickMag{0};
std::atomic_bool g_padSprint{false};   // [FPSTORM] A held = storm/roadie-run in LE3
std::atomic_bool g_padLeftShoulder{false}; // [WHEELVR] L1/LB held = native weapon-wheel ownership
std::atomic<unsigned long long> g_padSampleMs{0}; // successful pad sample time; prevents stale wheel ownership
std::atomic_bool g_moveFollowsHead{true};    // "Run where you look" (ini MoveFollowsHead)
// [DECOUPLE] zero a RIGHT-stick axis so only the head steers it. Pitch is the useful one in VR
// (stick pitch fights the headset); yaw suits players who want to turn purely by looking.
std::atomic_bool g_decoupledPitch{false};
std::atomic_bool g_decoupledYaw{false};
// [R3RECENTER] double-tap BACK to recenter. Players use a gamepad in the headset, where reaching for a
// keyboard key is exactly the thing you cannot do. BACK because it is unbound during gameplay, so no
// native action collides with it; a DOUBLE tap because this hook sees the pad before the game does,
// and a single press on a bound button would fire constantly during normal play.
constexpr DWORD kPadRecenterDoubleMs = 400;

// [DECOUPLEMENU 2026-08-22] Decoupled pitch/yaw zero a look axis before the game ever reads it,
// which is right for gameplay and wrong for menus: while a full-screen GUI or the galaxy map is up,
// that axis is how you move the cursor, so entries simply cannot be reached. Reported against ME1:
// "some commands like universe map are out of reach unless DP is off". The option is about gameplay
// look, so it stands down whenever a menu owns the screen and comes back by itself afterwards.
// Cached briefly because the input hooks are polled far faster than this state can change.
bool MenuOwnsLook() noexcept
{
    static unsigned long long s_lastMs = 0;
    static bool s_cached = false;
    const unsigned long long now = GetTickCount64();
    if (now - s_lastMs >= 50)
    {
        s_lastMs = now;
        s_cached = (ME2VR::D3DCapture::GetMenuMode() || ME2VR::EngineProbe::IsGalaxyMapOpen() ||
                       ME2VR::EngineProbe::IsMenuInputOwned());
    }
    return s_cached;
}

inline void ApplyStickDecouple(XINPUT_GAMEPAD* pad) noexcept
{
    if (MenuOwnsLook()) return;   // [DECOUPLEMENU] menus need the stick axis back
    if (g_decoupledYaw.load(std::memory_order_relaxed))   pad->sThumbRX = 0;
    if (g_decoupledPitch.load(std::memory_order_relaxed)) pad->sThumbRY = 0;
}

inline void CheckPadRecenter(const XINPUT_STATE* state, DWORD result, DWORD idx) noexcept
{
    if (result != ERROR_SUCCESS || idx != 0 || state == nullptr || g_open) return;
    static bool s_prev = false;
    static DWORD s_lastClickMs = 0;
    const bool down = (state->Gamepad.wButtons & XINPUT_GAMEPAD_BACK) != 0;
    if (down && !s_prev)
    {
        const DWORD now = GetTickCount();
        if (s_lastClickMs != 0 && (now - s_lastClickMs) <= kPadRecenterDoubleMs)
        {
            ME2VR::Me2Xr::Recenter();
            s_lastClickMs = 0;            // consume, so a triple tap is not two recenters
        }
        else s_lastClickMs = now;
    }
    s_prev = down;
}

inline void CaptureLeftStick(const XINPUT_STATE* state, DWORD result, DWORD idx) noexcept
{
    if (result == ERROR_SUCCESS && idx == 0 && state != nullptr)
    {
        const int lx = state->Gamepad.sThumbLX;
        const int ly = state->Gamepad.sThumbLY;
        g_leftStickMag.store((std::max)(std::abs(lx), std::abs(ly)), std::memory_order_relaxed);
        g_padSprint.store((state->Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0, std::memory_order_relaxed);
        g_padLeftShoulder.store((state->Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0,
                                std::memory_order_relaxed);
        g_padSampleMs.store(GetTickCount64(), std::memory_order_release);
    }
}

inline void RotateMoveStickByHeadLook(XINPUT_GAMEPAD* pad) noexcept
{
    const int yawUU = ME2VR::CalcViewHook::HeadLookYawUU();
    if (yawUU == 0) return;
    if (pad->sThumbLX == 0 && pad->sThumbLY == 0) return;
    // Positive yawUU = view turned LEFT -> rotate the stick vector left by the same angle. Magnitude is
    // preserved so the game's dead zones behave exactly as before; clamped to int16.
    const float a = static_cast<float>(yawUU) * (6.2831853f / 65536.0f);
    const float c = std::cos(a), sn = std::sin(a);
    const float lx = static_cast<float>(pad->sThumbLX);
    const float ly = static_cast<float>(pad->sThumbLY);
    float rx = lx * c - ly * sn;   // x=right, y=forward
    float ry = lx * sn + ly * c;
    if (rx > 32767.0f) rx = 32767.0f; else if (rx < -32768.0f) rx = -32768.0f;
    if (ry > 32767.0f) ry = 32767.0f; else if (ry < -32768.0f) ry = -32768.0f;
    pad->sThumbLX = static_cast<SHORT>(rx);
    pad->sThumbLY = static_cast<SHORT>(ry);
}

inline void PostProcessPad(XINPUT_STATE* state) noexcept
{
    if (state == nullptr) return;
    if (g_open) { ZeroMemory(&state->Gamepad, sizeof(XINPUT_GAMEPAD)); return; }
    if (g_moveFollowsHead.load(std::memory_order_relaxed)) RotateMoveStickByHeadLook(&state->Gamepad);
    ApplyStickDecouple(&state->Gamepad);
}

DWORD WINAPI HookedXInputGetState(DWORD idx, XINPUT_STATE* state) noexcept
{
    const DWORD r = g_origXInput ? g_origXInput(idx, state) : ERROR_DEVICE_NOT_CONNECTED;
    CaptureLeftStick(state, r, idx);
    CheckPadRecenter(state, r, idx);
    PostProcessPad(state);
    return r;
}

DWORD WINAPI HookedXInputGetStateEx(DWORD idx, XINPUT_STATE* state) noexcept
{
    const DWORD r = g_origXInputEx ? g_origXInputEx(idx, state)
                                   : (g_origXInput ? g_origXInput(idx, state) : ERROR_DEVICE_NOT_CONNECTED);
    CaptureLeftStick(state, r, idx);
    CheckPadRecenter(state, r, idx);
    PostProcessPad(state);
    return r;
}

// ===================== [DECOUPLE] mouse half =====================
// ApplyStickDecouple only touches XInput, so decoupled pitch and yaw did nothing at all for anyone
// playing on mouse and keyboard. UE3 reads mouse look through RAW INPUT (which is why the menu's
// WndProc swallows WM_INPUT), so the place to cut an axis is the raw-input read itself: let the call
// through, then zero the axis the head owns before the engine ever sees the packet. Absolute cursor
// movement is left alone, so menus and the pointer behave normally - only the relative look delta
// is touched. Ported from ME2.
using GetRawInputData_t = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using GetRawInputBuffer_t = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
GetRawInputData_t g_origGetRawInputData = nullptr;
GetRawInputBuffer_t g_origGetRawInputBuffer = nullptr;

inline void DecoupleRawMouse(RAWINPUT* ri) noexcept
{
    if (ri == nullptr || ri->header.dwType != RIM_TYPEMOUSE) return;
    if ((ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) return;   // absolute device: leave alone
    if (MenuOwnsLook()) return;   // [DECOUPLEMENU] menus need the mouse axis back
    if (g_decoupledYaw.load(std::memory_order_relaxed))   ri->data.mouse.lLastX = 0;
    if (g_decoupledPitch.load(std::memory_order_relaxed)) ri->data.mouse.lLastY = 0;
}

UINT WINAPI HookedGetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr) noexcept
{
    const UINT r = g_origGetRawInputData ? g_origGetRawInputData(h, cmd, data, size, hdr)
                                         : static_cast<UINT>(-1);
    if (!g_open && cmd == RID_INPUT && data != nullptr && r != static_cast<UINT>(-1))
        DecoupleRawMouse(reinterpret_cast<RAWINPUT*>(data));
    return r;
}

// Batched variant: the buffer is a packed array walked with NEXTRAWINPUTBLOCK, not a flat stride.
UINT WINAPI HookedGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT hdr) noexcept
{
    const UINT r = g_origGetRawInputBuffer ? g_origGetRawInputBuffer(data, size, hdr)
                                           : static_cast<UINT>(-1);
    if (!g_open && data != nullptr && r != static_cast<UINT>(-1) && r > 0)
    {
        // Advance by hand: NEXTRAWINPUTBLOCK's alignment macro needs a type this TU does not pull in.
        // Each block is dwSize bytes, 8-byte aligned on x64.
        RAWINPUT* cur = data;
        for (UINT i = 0; i < r && cur != nullptr; ++i)
        {
            DecoupleRawMouse(cur);
            const ULONG_PTR next = (reinterpret_cast<ULONG_PTR>(cur) + cur->header.dwSize + 7u)
                                   & ~static_cast<ULONG_PTR>(7u);
            cur = reinterpret_cast<RAWINPUT*>(next);
        }
    }
    return r;
}

void InstallMouseHooks() noexcept
{
    static bool s_done = false;
    if (s_done) return;
    const MH_STATUS mhInit = MH_Initialize();   // tolerate already-initialized (d3d_capture inits it too)
    if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED) return;
    HMODULE u32 = GetModuleHandleA("user32.dll");
    if (u32 == nullptr) return;
    if (FARPROC p = GetProcAddress(u32, "GetRawInputData"))
    {
        void* orig = nullptr;
        if (MH_CreateHook(reinterpret_cast<void*>(p), reinterpret_cast<void*>(&HookedGetRawInputData), &orig) == MH_OK &&
            MH_EnableHook(reinterpret_cast<void*>(p)) == MH_OK)
        {
            g_origGetRawInputData = reinterpret_cast<GetRawInputData_t>(orig);
            ME2VR::Log::Line("[DECOUPLE] GetRawInputData hook ready (mouse decouple live)");
        }
    }
    if (FARPROC p = GetProcAddress(u32, "GetRawInputBuffer"))
    {
        void* orig = nullptr;
        if (MH_CreateHook(reinterpret_cast<void*>(p), reinterpret_cast<void*>(&HookedGetRawInputBuffer), &orig) == MH_OK &&
            MH_EnableHook(reinterpret_cast<void*>(p)) == MH_OK)
        {
            g_origGetRawInputBuffer = reinterpret_cast<GetRawInputBuffer_t>(orig);
            ME2VR::Log::Line("[DECOUPLE] GetRawInputBuffer hook ready");
        }
    }
    s_done = (g_origGetRawInputData != nullptr || g_origGetRawInputBuffer != nullptr);
}

void InstallPadHooks() noexcept
{
    static bool s_done = false;
    static unsigned s_calls = 0;
    if (s_done) return;
    if ((s_calls++ % 300) != 0) return;   // retry ~every 5s until the game has loaded an XInput dll
    const MH_STATUS mhInit = MH_Initialize();   // tolerate already-initialized (d3d_capture inits it too)
    if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED) return;
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", "xinput1_2.dll", "xinput1_1.dll" };
    for (const char* d : dlls)
    {
        HMODULE m = GetModuleHandleA(d);
        if (m == nullptr) continue;   // only hook XInput dlls the GAME already loaded
        FARPROC p = GetProcAddress(m, "XInputGetState");
        if (p != nullptr)
        {
            void* orig = nullptr;
            if (MH_CreateHook(reinterpret_cast<void*>(p), reinterpret_cast<void*>(&HookedXInputGetState), &orig) == MH_OK &&
                MH_EnableHook(reinterpret_cast<void*>(p)) == MH_OK)
            {
                if (g_origXInput == nullptr) g_origXInput = reinterpret_cast<XInputGetState_t>(orig);
                ME2VR::Log::Line(std::string("[MOVEFIX] XInputGetState hook ready: ") + d);
            }
        }
        // XInputGetStateEx - ordinal 100, no named export (xinput1_3/1_4; absent from 9_1_0). UE3 can
        // poll the pad through THIS entry, bypassing the named hook.
        FARPROC pex = GetProcAddress(m, reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(100)));
        if (pex != nullptr && pex != p)
        {
            void* origEx = nullptr;
            if (MH_CreateHook(reinterpret_cast<void*>(pex), reinterpret_cast<void*>(&HookedXInputGetStateEx), &origEx) == MH_OK &&
                MH_EnableHook(reinterpret_cast<void*>(pex)) == MH_OK)
            {
                if (g_origXInputEx == nullptr) g_origXInputEx = reinterpret_cast<XInputGetState_t>(origEx);
                ME2VR::Log::Line(std::string("[MOVEFIX] XInputGetStateEx (ord 100) hook ready: ") + d);
            }
        }
    }
    if (g_origXInput != nullptr || g_origXInputEx != nullptr) s_done = true;   // latch only on success
}
// ================================================================================================

bool g_insertWasDown = false;
HWND g_hwnd = nullptr;
WNDPROC g_originalWndProc = nullptr;
bool g_mouseAnchorValid = false;
POINT g_mouseAnchorScreen = {};
float g_virtualMouseX = 450.0f;
float g_virtualMouseY = 360.0f;
// [MENUWHEEL] Scroll-wheel deltas, accumulated in the WndProc (window thread) and drained in
// FeedMouse (render thread). Everything else about this menu's input is POLLED - position from
// GetCursorPos, buttons from GetAsyncKeyState - but the wheel has no poll-able state at all, so it
// has to be caught as a message. WM_MOUSEWHEEL was already being swallowed while the menu is open
// (correctly - the game must not see it), it just was never forwarded to ImGui, which is why the
// menu could not be scrolled with the wheel. Raw WHEEL_DELTA units; converted on drain.
std::atomic<int> g_wheelAccumV{0};
std::atomic<int> g_wheelAccumH{0};
bool g_savedFlash = false;
int g_savedFrames = 0;
bool g_valuesLoaded = false;

// Engine-level game-input freeze while the menu is open (so mouse/look don't drive the camera).
// P1 ULocalPlayer -> Actor (PlayerController) @0x68 -> bIgnoreMoveInput / bIgnoreLookInput (LE3 offsets).
constexpr std::uintptr_t kUPlayerActor = 0x68;
constexpr std::uintptr_t kPcIgnoreMove = 0x781;   // LE3 APlayerController.bIgnoreMoveInput
constexpr std::uintptr_t kPcIgnoreLook = 0x782;   // LE3 APlayerController.bIgnoreLookInput
bool g_frozen = false;
unsigned char g_savedMove = 0, g_savedLook = 0;

void EnforceFreeze(bool wantFrozen) noexcept
{
    __try
    {
        const std::uintptr_t lp = ME2VR::EngineProbe::GetPrimaryLocalPlayer();
        if (lp == 0) return;
        const std::uintptr_t pc = *reinterpret_cast<std::uintptr_t*>(lp + kUPlayerActor);
        if (pc < 0x10000) return;
        auto* im = reinterpret_cast<unsigned char*>(pc + kPcIgnoreMove);
        auto* il = reinterpret_cast<unsigned char*>(pc + kPcIgnoreLook);
        if (wantFrozen)
        {
            if (!g_frozen) { g_savedMove = *im; g_savedLook = *il; g_frozen = true; }
            *im = 1; *il = 1;   // re-assert every frame (the game can reset these)
        }
        else if (g_frozen)
        {
            *im = g_savedMove; *il = g_savedLook; g_frozen = false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

template <typename T> void SafeRelease(T*& p) noexcept { if (p) { p->Release(); p = nullptr; } }
float ClampF(float v, float lo, float hi, float fb) noexcept { return std::isfinite(v) ? (std::max)(lo, (std::min)(hi, v)) : fb; }
int ClampI(int v, int lo, int hi, int fb) noexcept { (void)fb; return (std::max)(lo, (std::min)(hi, v)); }

std::wstring IniPath()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p(exe);
    const size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p.resize(slash + 1);
    return p + L"MELE3VR.ini";
}

float IniGetF(const std::wstring& path, const wchar_t* key, float def) noexcept
{
    wchar_t d[32] = {}, buf[64] = {};
    swprintf_s(d, L"%.3f", def);
    GetPrivateProfileStringW(L"FirstPerson", key, d, buf, 64, path.c_str());
    return static_cast<float>(_wtof(buf));
}
void LoadFpStates(const std::wstring& path) noexcept
{
    for (int i = 0; i < ME2VR::EngineProbe::FpStateCount(); ++i)
    {
        ME2VR::EngineProbe::FpStateCfg* s = ME2VR::EngineProbe::GetFpStateCfg(i);
        if (s == nullptr) continue;
        const ME2VR::EngineProbe::FpStateCfg d = ME2VR::EngineProbe::FpStateDefault(i);
        wchar_t k[32] = {};
        swprintf_s(k, L"S%d_on", i);       s->on       = GetPrivateProfileIntW(L"FirstPerson", k, d.on ? 1 : 0, path.c_str()) != 0;
        swprintf_s(k, L"S%d_hideHead", i); s->hideHead = GetPrivateProfileIntW(L"FirstPerson", k, d.hideHead ? 1 : 0, path.c_str()) != 0;
        swprintf_s(k, L"S%d_hideBody", i); s->hideBody = GetPrivateProfileIntW(L"FirstPerson", k, d.hideBody ? 1 : 0, path.c_str()) != 0;
        swprintf_s(k, L"S%d_x", i);        s->x = ClampF(IniGetF(path, k, d.x), -200.0f, 400.0f, d.x);
        swprintf_s(k, L"S%d_y", i);        s->y = ClampF(IniGetF(path, k, d.y), -100.0f, 100.0f, d.y);
        swprintf_s(k, L"S%d_z", i);        s->z = ClampF(IniGetF(path, k, d.z), -100.0f, 200.0f, d.z);
    }
}
void SaveFpStates(const std::wstring& path) noexcept
{
    for (int i = 0; i < ME2VR::EngineProbe::FpStateCount(); ++i)
    {
        const ME2VR::EngineProbe::FpStateCfg* s = ME2VR::EngineProbe::GetFpStateCfg(i);
        if (s == nullptr) continue;
        wchar_t k[32] = {}, v[32] = {};
        swprintf_s(k, L"S%d_on", i);       WritePrivateProfileStringW(L"FirstPerson", k, s->on ? L"1" : L"0", path.c_str());
        swprintf_s(k, L"S%d_hideHead", i); WritePrivateProfileStringW(L"FirstPerson", k, s->hideHead ? L"1" : L"0", path.c_str());
        swprintf_s(k, L"S%d_hideBody", i); WritePrivateProfileStringW(L"FirstPerson", k, s->hideBody ? L"1" : L"0", path.c_str());
        swprintf_s(k, L"S%d_x", i); swprintf_s(v, L"%.1f", s->x); WritePrivateProfileStringW(L"FirstPerson", k, v, path.c_str());
        swprintf_s(k, L"S%d_y", i); swprintf_s(v, L"%.1f", s->y); WritePrivateProfileStringW(L"FirstPerson", k, v, path.c_str());
        swprintf_s(k, L"S%d_z", i); swprintf_s(v, L"%.1f", s->z); WritePrivateProfileStringW(L"FirstPerson", k, v, path.c_str());
    }
}

void LoadValues() noexcept
{
    const std::wstring path = IniPath();
    wchar_t buf[64] = {};
    ME2VR::CalcViewHook::SetVrEnabled(GetPrivateProfileIntW(L"VR", L"Enabled", 1, path.c_str()) != 0);   // VR on by default
    GetPrivateProfileStringW(L"VR", L"HalfEyeUU", L"1.6", buf, 64, path.c_str());
    ME2VR::CalcViewHook::SetHalfEyeUU(ClampF(static_cast<float>(_wtof(buf)), 0.0f, 16.0f, 1.6f));
    ME2VR::CalcViewHook::SetSwapEyes(GetPrivateProfileIntW(L"VR", L"SwapEyes", 0, path.c_str()) != 0);
    // [MOVEFIX] default ON: run-where-you-look is what makes head tracking feel like it steers you,
    // and a stale 0 from an ini written before this option existed should not leave anyone without it.
    SetMoveFollowsHead(GetPrivateProfileIntW(L"VR", L"MoveFollowsHead", 1, path.c_str()) != 0);
    SetDecoupledPitch(GetPrivateProfileIntW(L"VR", L"DecoupledPitch", 0, path.c_str()) != 0);
    SetDecoupledYaw(GetPrivateProfileIntW(L"VR", L"DecoupledYaw", 0, path.c_str()) != 0);
    ME2VR::D3DCapture::SetMonoMenus(GetPrivateProfileIntW(L"VR", L"MonoMenus", 1, path.c_str()) != 0);
    ME2VR::D3DCapture::SetMirrorPresentEvery(GetPrivateProfileIntW(L"VR", L"MirrorPresentEvery", 0, path.c_str()));   // 0 = keep the default 8
    g_recenterKey = ClampI(GetPrivateProfileIntW(L"VR", L"RecenterKey", 'R', path.c_str()), 0x08, 0xFE, 'R');
    g_fpToggleKey = ClampI(GetPrivateProfileIntW(L"VR", L"FpToggleKey", 'K', path.c_str()), 0x08, 0xFE, 'K');
    g_menuKey = ClampI(GetPrivateProfileIntW(L"VR", L"MenuKey", VK_INSERT, path.c_str()), 0x08, 0xFE, VK_INSERT);
    ME2VR::EngineProbe::SetCoverThirdPerson(GetPrivateProfileIntW(L"VR", L"CoverThirdPerson", 1, path.c_str()) != 0);
    ME2VR::CalcViewHook::SetCineVrConvo(GetPrivateProfileIntW(L"VR", L"CineVrConvo", 0, path.c_str()) != 0);        // [VRCINE] opt-in, ME2 defaults
    ME2VR::CalcViewHook::SetCineVrCutscene(GetPrivateProfileIntW(L"VR", L"CineVrCutscene", 0, path.c_str()) != 0);
    ME2VR::CalcViewHook::SetCineVrHeadTracking(GetPrivateProfileIntW(L"VR", L"CineVrHeadTracking", 1, path.c_str()) != 0);
    GetPrivateProfileStringW(L"VR", L"CineScreenZoom", L"1.0", buf, 64, path.c_str());
    ME2VR::Me2Xr::SetCineScreenZoom(static_cast<float>(_wtof(buf)));
    ME2VR::CalcViewHook::SetStereoFramePacing(GetPrivateProfileIntW(L"VR", L"StereoFramePacing", 1, path.c_str()) != 0);
    ME2VR::CalcViewHook::SetFullRefreshPacing(GetPrivateProfileIntW(L"VR", L"FullRefreshPacing", 1, path.c_str()) != 0);
    ME2VR::Log::SetDiagnostics(GetPrivateProfileIntW(L"VR", L"Diagnostics", 0, path.c_str()) != 0);
    ME2VR::EngineProbe::SetHeadAimEnabled(GetPrivateProfileIntW(L"VR", L"HeadAim", 1, path.c_str()) != 0);
    ME2VR::EngineProbe::SetHeadAimInvertYaw(GetPrivateProfileIntW(L"VR", L"HeadAimInvYaw", 0, path.c_str()) != 0);
    ME2VR::EngineProbe::SetHeadAimInvertPitch(GetPrivateProfileIntW(L"VR", L"HeadAimInvPitch", 0, path.c_str()) != 0);
    ME2VR::EngineProbe::SetHeadAimWeaponOnly(GetPrivateProfileIntW(L"VR", L"HeadAimWeaponOnly", 1, path.c_str()) != 0);
    ME2VR::EngineProbe::SetHeadAimCoverOff(GetPrivateProfileIntW(L"VR", L"HeadAimCoverOff", 1, path.c_str()) != 0);
    ME2VR::CalcViewHook::SetHeadPosEnabled(GetPrivateProfileIntW(L"VR", L"PosEnabled", 1, path.c_str()) != 0);
    ME2VR::CalcViewHook::SetLeanInvertFwd(GetPrivateProfileIntW(L"VR", L"LeanInvertFwd", 0, path.c_str()) != 0);
    ME2VR::Me2Xr::SetQuestFovMatch(GetPrivateProfileIntW(L"VR", L"QuestFovMatch", 1, path.c_str()) != 0);
    ME2VR::Me2Xr::SetVrFovFill(GetPrivateProfileIntW(L"VR", L"VrFovFill", 1, path.c_str()) != 0);
    g_profileHotkeys = GetPrivateProfileIntW(L"VR", L"ProfileHotkeys", 1, path.c_str()) != 0;
    for (int i = 0; i < kProfileCount; ++i)
    {
        wchar_t kn[32] = {}; swprintf_s(kn, L"ProfileKey%d", i);
        g_profileKey[i] = ClampI(GetPrivateProfileIntW(L"VR", kn, 0x31 + i, path.c_str()), 0x08, 0xFE, 0x31 + i);
    }
    { wchar_t cb[64] = {};
      GetPrivateProfileStringW(L"VR", L"MenuScreenDist", L"2.20", cb, 64, path.c_str());
      ME2VR::Me2Xr::SetMenuScreenDist(static_cast<float>(_wtof(cb)));
      GetPrivateProfileStringW(L"VR", L"MenuScreenSize", L"3.20", cb, 64, path.c_str());
      ME2VR::Me2Xr::SetMenuScreenSize(static_cast<float>(_wtof(cb)));
      GetPrivateProfileStringW(L"VR", L"MenuPanelDist", L"1.50", cb, 64, path.c_str());
      ME2VR::Me2Xr::SetMenuPanelDist(static_cast<float>(_wtof(cb)));
      GetPrivateProfileStringW(L"VR", L"MenuPanelSize", L"1.40", cb, 64, path.c_str());
      ME2VR::Me2Xr::SetMenuPanelSize(static_cast<float>(_wtof(cb)));
      GetPrivateProfileStringW(L"VR", L"MenuPanelOffX", L"0.00", cb, 64, path.c_str());
      ME2VR::Me2Xr::SetMenuPanelOffX(static_cast<float>(_wtof(cb)));
      GetPrivateProfileStringW(L"VR", L"MenuPanelOffY", L"0.00", cb, 64, path.c_str());
      ME2VR::Me2Xr::SetMenuPanelOffY(static_cast<float>(_wtof(cb))); }
    { wchar_t fb[64] = {};
      GetPrivateProfileStringW(L"VR", L"VrFillH", L"1.000", fb, 64, path.c_str());
      ME2VR::Me2Xr::SetVrFillH(static_cast<float>(_wtof(fb)));
      GetPrivateProfileStringW(L"VR", L"VrFillV", L"1.000", fb, 64, path.c_str());
      ME2VR::Me2Xr::SetVrFillV(static_cast<float>(_wtof(fb))); }
    ME2VR::CalcViewHook::SetStereoFramePacingHz(ClampI(GetPrivateProfileIntW(L"VR", L"StereoFramePacingHz", 0, path.c_str()), 0, 240, 0));
    ME2VR::D3DCapture::SetUiDupEnabled(GetPrivateProfileIntW(L"VR", L"UiDupEnabled", 1, path.c_str()) != 0);
    // [INVMAT] stale-inverse fix (LE1 dark-panel root cause) - visual only, no gameplay effect, no
    // downside found. Forced ON unconditionally (ME2 parity) rather than exposed as a toggle, so a
    // stale ini from before this existed can't leave anyone with the dark fog band on look up/down.
    ME2VR::CalcViewHook::SetInvMatrixFix(true);
    // VR mode + AER + DIBR (ported from ME2 2026-07-12).
    // [SFR] default is now 4 (same-frame stereo). An existing ini holding the old SBS value 1 is
    // migrated once, so nobody silently keeps rendering the mode with the per-eye effect bugs.
    {
        int m = ClampI(GetPrivateProfileIntW(L"VR", L"Mode", 4, path.c_str()), 0, 4, 4);
        // The legacy SBS split (mode 1) was removed from the picker 2026-07-28, so this migration is
        // now UNCONDITIONAL - it used to run once behind SfrMigrated, which was correct while the mode
        // was still selectable. An ini holding 1 today would select a mode the UI cannot show or leave.
        if (m == 1)
        {
            m = 4;
            WritePrivateProfileStringW(L"VR", L"SfrMigrated", L"1", path.c_str());
            WritePrivateProfileStringW(L"VR", L"Mode", L"4", path.c_str());
        }
        ME2VR::CalcViewHook::SetVrMode(m);
    }
    // [SFR] convergence + HUD trims (the [UIRATIO] match itself is automatic and needs no setting).
    GetPrivateProfileStringW(L"VR", L"SfrConvergence", L"0.03", buf, 64, path.c_str());
    ME2VR::CalcViewHook::SetSfrConvergence(static_cast<float>(_wtof(buf)));
    GetPrivateProfileStringW(L"VR", L"SfrUiScaleX", L"1.0", buf, 64, path.c_str());
    ME2VR::CalcViewHook::SetSfrUiScaleX(static_cast<float>(_wtof(buf)));
    GetPrivateProfileStringW(L"VR", L"SfrUiScaleY", L"1.0", buf, 64, path.c_str());
    ME2VR::CalcViewHook::SetSfrUiScaleY(static_cast<float>(_wtof(buf)));
    GetPrivateProfileStringW(L"VR", L"SfrUiOffX", L"0.0", buf, 64, path.c_str());
    ME2VR::CalcViewHook::SetSfrUiOffX(static_cast<float>(_wtof(buf)));
    GetPrivateProfileStringW(L"VR", L"SfrUiOffY", L"0.0", buf, 64, path.c_str());
    ME2VR::CalcViewHook::SetSfrUiOffY(static_cast<float>(_wtof(buf)));
    ME2VR::CalcViewHook::SetHeadLookUserEnabled(GetPrivateProfileIntW(L"VR", L"HeadLook", 1, path.c_str()) != 0);
    GetPrivateProfileStringW(L"VR", L"HeadLookSmoothing", L"0.4", buf, 64, path.c_str());
    ME2VR::CalcViewHook::SetHeadLookSmoothing(static_cast<float>(_wtof(buf)));
    GetPrivateProfileStringW(L"VR", L"LookSensitivity", L"1.0", buf, 64, path.c_str());
    ME2VR::CalcViewHook::SetLookSensitivity(ClampF(static_cast<float>(_wtof(buf)), 0.1f, 3.0f, 1.0f));
    ME2VR::CalcViewHook::SetInvertLookYaw(GetPrivateProfileIntW(L"VR", L"InvertLookYaw", 0, path.c_str()) != 0);
    ME2VR::CalcViewHook::SetInvertLookPitch(GetPrivateProfileIntW(L"VR", L"InvertLookPitch", 0, path.c_str()) != 0);
    g_disableDof = GetPrivateProfileIntW(L"VR", L"DisableDof", 0, path.c_str()) != 0;
    GetPrivateProfileStringW(L"VR", L"PosScale", L"2.0", buf, 64, path.c_str());   // ME2's baked default
    ME2VR::CalcViewHook::SetHeadPosScale(ClampF(static_cast<float>(_wtof(buf)), 0.0f, 12.0f, 2.0f));
    GetPrivateProfileStringW(L"VR", L"PoseTagDelay", L"2.0", buf, 64, path.c_str());
    ME2VR::CalcViewHook::SetPoseTagDelayFrames(static_cast<float>(_wtof(buf)));
    GetPrivateProfileStringW(L"VR", L"AerHalfEyeUU", L"3.4", buf, 64, path.c_str());   // baked 2026-08-21
    ME2VR::CalcViewHook::SetAerHalfEyeUU(ClampF(static_cast<float>(_wtof(buf)), 0.0f, 8.0f, 3.4f));
    ME2VR::CalcViewHook::SetAerSwapEyes(GetPrivateProfileIntW(L"VR", L"AerSwapEyes", 0, path.c_str()) != 0);   // [AERSHAKE bake] 0: FIFO stamp killed the pipeline swap this compensated
    ME2VR::CalcViewHook::SetAerFramePacing(GetPrivateProfileIntW(L"VR", L"AerFramePacing", 1, path.c_str()) != 0);
    ME2VR::CalcViewHook::SetAerFramePacingHz(ClampI(GetPrivateProfileIntW(L"VR", L"AerFramePacingHz", 0, path.c_str()), 0, 240, 0));
    GetPrivateProfileStringW(L"VR", L"DepthWarpGain", L"1.2", buf, 64, path.c_str());
    ME2VR::CalcViewHook::SetDepthWarpGain(ClampF(static_cast<float>(_wtof(buf)), 0.0f, 10.0f, 1.2f));
    GetPrivateProfileStringW(L"VR", L"DepthWarpConv", L"0.985", buf, 64, path.c_str());
    ME2VR::CalcViewHook::SetDepthWarpConv(ClampF(static_cast<float>(_wtof(buf)), 0.90f, 1.005f, 0.985f));
    ME2VR::CalcViewHook::SetDepthWarpFlip(GetPrivateProfileIntW(L"VR", L"DepthWarpFlip", 1, path.c_str()) != 0);
    ME2VR::CalcViewHook::SetDibrAutoConverge(GetPrivateProfileIntW(L"VR", L"DibrAutoConverge", 1, path.c_str()) != 0);
    ME2VR::ConvoFp::SetEnabled(GetPrivateProfileIntW(L"FirstPerson", L"ConvoFirstPerson", 0, path.c_str()) != 0);
    ME2VR::CalcViewHook::SetConvoFpInvertFacing(GetPrivateProfileIntW(L"FirstPerson", L"ConvoFpInvertFacing", 0, path.c_str()) != 0);
    GetPrivateProfileStringW(L"FirstPerson", L"ConvoFpTurnRate", L"90", buf, 64, path.c_str()); ME2VR::ConvoFp::SetTurnRate(static_cast<float>(_wtof(buf)));
    GetPrivateProfileStringW(L"FirstPerson", L"ConvoFpAnimFollow", L"0.65", buf, 64, path.c_str()); ME2VR::ConvoFp::SetAnimFollow(static_cast<float>(_wtof(buf)));
    GetPrivateProfileStringW(L"FirstPerson", L"ConvoFpEyeUpUU", L"0", buf, 64, path.c_str()); ME2VR::ConvoFp::SetEyeUpUU(static_cast<float>(_wtof(buf)));
    GetPrivateProfileStringW(L"FirstPerson", L"ConvoFpZoom", L"1.0", buf, 64, path.c_str()); ME2VR::ConvoFp::SetZoom(static_cast<float>(_wtof(buf)));
    ME2VR::ConvoFp::SetHideHead(GetPrivateProfileIntW(L"FirstPerson", L"ConvoFpHideHead", 1, path.c_str()) != 0);
    ME2VR::ConvoFp::SetKillDof(GetPrivateProfileIntW(L"FirstPerson", L"ConvoFpDisableDof", 1, path.c_str()) != 0);
    if (ME2VR::ConvoFp::GetEnabled() && ME2VR::CalcViewHook::GetCineVrConvo())
        ME2VR::CalcViewHook::SetCineVrConvo(false);
    ME2VR::HudProbe::SetEditorEnabled(GetPrivateProfileIntW(L"HUD", L"PerElementEnabled", 1, path.c_str()) != 0);
    for (int i = 0; i < ME2VR::HudProbe::GroupCount(); ++i)
    {
        auto cfg = ME2VR::HudProbe::DefaultGroupConfig(i);
        wchar_t key[32] = {}, def[32] = {};
        swprintf_s(key, L"G%d_X", i); swprintf_s(def, L"%.3f", cfg.offsetX); GetPrivateProfileStringW(L"HUD", key, def, buf, 64, path.c_str()); cfg.offsetX = ClampF(static_cast<float>(_wtof(buf)), -600.0f, 600.0f, cfg.offsetX);
        swprintf_s(key, L"G%d_Y", i); swprintf_s(def, L"%.3f", cfg.offsetY); GetPrivateProfileStringW(L"HUD", key, def, buf, 64, path.c_str()); cfg.offsetY = ClampF(static_cast<float>(_wtof(buf)), -500.0f, 500.0f, cfg.offsetY);
        swprintf_s(key, L"G%d_SX", i); swprintf_s(def, L"%.3f", cfg.scaleX); GetPrivateProfileStringW(L"HUD", key, def, buf, 64, path.c_str()); cfg.scaleX = ClampF(static_cast<float>(_wtof(buf)), 0.3f, 3.0f, cfg.scaleX);
        swprintf_s(key, L"G%d_SY", i); swprintf_s(def, L"%.3f", cfg.scaleY); GetPrivateProfileStringW(L"HUD", key, def, buf, 64, path.c_str()); cfg.scaleY = ClampF(static_cast<float>(_wtof(buf)), 0.3f, 3.0f, cfg.scaleY);
        ME2VR::HudProbe::SetGroupConfig(i, cfg);
    }
    LoadFpStates(path);
    g_valuesLoaded = true;
}

void SaveValues() noexcept
{
    const std::wstring path = IniPath();
    wchar_t buf[64] = {};
    WritePrivateProfileStringW(L"VR", L"ProfileHotkeys", g_profileHotkeys ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"MoveFollowsHead", GetMoveFollowsHead() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"DecoupledPitch",  GetDecoupledPitch()  ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"DecoupledYaw",    GetDecoupledYaw()    ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"MonoMenus", ME2VR::D3DCapture::GetMonoMenus() ? L"1" : L"0", path.c_str());
    swprintf_s(buf, L"%d", g_recenterKey); WritePrivateProfileStringW(L"VR", L"RecenterKey", buf, path.c_str());
    swprintf_s(buf, L"%d", g_fpToggleKey); WritePrivateProfileStringW(L"VR", L"FpToggleKey", buf, path.c_str());
    swprintf_s(buf, L"%d", g_menuKey); WritePrivateProfileStringW(L"VR", L"MenuKey", buf, path.c_str());
    WritePrivateProfileStringW(L"VR", L"CoverThirdPerson", ME2VR::EngineProbe::GetCoverThirdPerson() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"CineVrConvo", ME2VR::CalcViewHook::GetCineVrConvo() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"FirstPerson", L"ConvoFirstPerson", ME2VR::ConvoFp::GetEnabled() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"FirstPerson", L"ConvoFpInvertFacing", ME2VR::CalcViewHook::GetConvoFpInvertFacing() ? L"1" : L"0", path.c_str());
    swprintf_s(buf, L"%.2f", ME2VR::ConvoFp::GetTurnRate()); WritePrivateProfileStringW(L"FirstPerson", L"ConvoFpTurnRate", buf, path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::ConvoFp::GetAnimFollow()); WritePrivateProfileStringW(L"FirstPerson", L"ConvoFpAnimFollow", buf, path.c_str());
    swprintf_s(buf, L"%.2f", ME2VR::ConvoFp::GetEyeUpUU()); WritePrivateProfileStringW(L"FirstPerson", L"ConvoFpEyeUpUU", buf, path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::ConvoFp::GetZoom()); WritePrivateProfileStringW(L"FirstPerson", L"ConvoFpZoom", buf, path.c_str());
    WritePrivateProfileStringW(L"FirstPerson", L"ConvoFpHideHead", ME2VR::ConvoFp::GetHideHead() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"FirstPerson", L"ConvoFpDisableDof", ME2VR::ConvoFp::GetKillDof() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"CineVrCutscene", ME2VR::CalcViewHook::GetCineVrCutscene() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"CineVrHeadTracking", ME2VR::CalcViewHook::GetCineVrHeadTracking() ? L"1" : L"0", path.c_str());
    swprintf_s(buf, L"%.2f", ME2VR::Me2Xr::GetCineScreenZoom()); WritePrivateProfileStringW(L"VR", L"CineScreenZoom", buf, path.c_str());
    for (int i = 0; i < kProfileCount; ++i)
    {
        wchar_t kn[32] = {}; swprintf_s(kn, L"ProfileKey%d", i);
        swprintf_s(buf, L"%d", g_profileKey[i]); WritePrivateProfileStringW(L"VR", kn, buf, path.c_str());
    }
    WritePrivateProfileStringW(L"VR", L"VrFovFill", ME2VR::Me2Xr::GetVrFovFill() ? L"1" : L"0", path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::Me2Xr::GetVrFillH()); WritePrivateProfileStringW(L"VR", L"VrFillH", buf, path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::Me2Xr::GetVrFillV()); WritePrivateProfileStringW(L"VR", L"VrFillV", buf, path.c_str());
    WritePrivateProfileStringW(L"VR", L"QuestFovMatch", ME2VR::Me2Xr::GetQuestFovMatch() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"FullRefreshPacing", ME2VR::CalcViewHook::GetFullRefreshPacing() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"Diagnostics", ME2VR::Log::DiagnosticsOn() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"HeadAim", ME2VR::EngineProbe::GetHeadAimEnabled() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"HeadAimInvYaw", ME2VR::EngineProbe::GetHeadAimInvertYaw() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"HeadAimInvPitch", ME2VR::EngineProbe::GetHeadAimInvertPitch() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"HeadAimWeaponOnly", ME2VR::EngineProbe::GetHeadAimWeaponOnly() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"HeadAimCoverOff", ME2VR::EngineProbe::GetHeadAimCoverOff() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"PosEnabled", ME2VR::CalcViewHook::GetHeadPosEnabled() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"LeanInvertFwd", ME2VR::CalcViewHook::GetLeanInvertFwd() ? L"1" : L"0", path.c_str());
    swprintf_s(buf, L"%.2f", ME2VR::CalcViewHook::GetHeadPosScale());
    WritePrivateProfileStringW(L"VR", L"PosScale", buf, path.c_str());
    swprintf_s(buf, L"%.2f", ME2VR::Me2Xr::GetMenuScreenDist()); WritePrivateProfileStringW(L"VR", L"MenuScreenDist", buf, path.c_str());
    swprintf_s(buf, L"%.2f", ME2VR::Me2Xr::GetMenuScreenSize()); WritePrivateProfileStringW(L"VR", L"MenuScreenSize", buf, path.c_str());
    swprintf_s(buf, L"%.2f", ME2VR::Me2Xr::GetMenuPanelDist()); WritePrivateProfileStringW(L"VR", L"MenuPanelDist", buf, path.c_str());
    swprintf_s(buf, L"%.2f", ME2VR::Me2Xr::GetMenuPanelSize()); WritePrivateProfileStringW(L"VR", L"MenuPanelSize", buf, path.c_str());
    swprintf_s(buf, L"%.2f", ME2VR::Me2Xr::GetMenuPanelOffX()); WritePrivateProfileStringW(L"VR", L"MenuPanelOffX", buf, path.c_str());
    swprintf_s(buf, L"%.2f", ME2VR::Me2Xr::GetMenuPanelOffY()); WritePrivateProfileStringW(L"VR", L"MenuPanelOffY", buf, path.c_str());
    WritePrivateProfileStringW(L"VR", L"Enabled", ME2VR::CalcViewHook::GetVrEnabled() ? L"1" : L"0", path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::CalcViewHook::GetHalfEyeUU());
    WritePrivateProfileStringW(L"VR", L"HalfEyeUU", buf, path.c_str());
    WritePrivateProfileStringW(L"VR", L"SwapEyes", ME2VR::CalcViewHook::GetSwapEyes() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"StereoFramePacing", ME2VR::CalcViewHook::GetStereoFramePacing() ? L"1" : L"0", path.c_str());
    swprintf_s(buf, L"%d", ME2VR::CalcViewHook::GetStereoFramePacingHz());
    WritePrivateProfileStringW(L"VR", L"StereoFramePacingHz", buf, path.c_str());
    WritePrivateProfileStringW(L"VR", L"UiDupEnabled", ME2VR::D3DCapture::GetUiDupEnabled() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"InvMatrixFix", ME2VR::CalcViewHook::GetInvMatrixFix() ? L"1" : L"0", path.c_str());
    swprintf_s(buf, L"%d", ME2VR::CalcViewHook::GetVrMode());
    WritePrivateProfileStringW(L"VR", L"Mode", buf, path.c_str());
    swprintf_s(buf, L"%.4f", ME2VR::CalcViewHook::GetSfrConvergence());
    WritePrivateProfileStringW(L"VR", L"SfrConvergence", buf, path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::CalcViewHook::GetSfrUiScaleX());
    WritePrivateProfileStringW(L"VR", L"SfrUiScaleX", buf, path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::CalcViewHook::GetSfrUiScaleY());
    WritePrivateProfileStringW(L"VR", L"SfrUiScaleY", buf, path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::CalcViewHook::GetSfrUiOffX());
    WritePrivateProfileStringW(L"VR", L"SfrUiOffX", buf, path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::CalcViewHook::GetSfrUiOffY());
    WritePrivateProfileStringW(L"VR", L"SfrUiOffY", buf, path.c_str());
    WritePrivateProfileStringW(L"VR", L"HeadLook", ME2VR::CalcViewHook::GetHeadLookUserEnabled() ? L"1" : L"0", path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::CalcViewHook::GetHeadLookSmoothing());
    WritePrivateProfileStringW(L"VR", L"HeadLookSmoothing", buf, path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::CalcViewHook::GetLookSensitivity());
    WritePrivateProfileStringW(L"VR", L"LookSensitivity", buf, path.c_str());
    WritePrivateProfileStringW(L"VR", L"InvertLookYaw", ME2VR::CalcViewHook::GetInvertLookYaw() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"InvertLookPitch", ME2VR::CalcViewHook::GetInvertLookPitch() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"DisableDof", g_disableDof ? L"1" : L"0", path.c_str());
    swprintf_s(buf, L"%d", ME2VR::D3DCapture::GetMirrorPresentEvery());   // [MIRRORREC] was load-only
    WritePrivateProfileStringW(L"VR", L"MirrorPresentEvery", buf, path.c_str());
    swprintf_s(buf, L"%.2f", ME2VR::CalcViewHook::GetPoseTagDelayFrames());
    WritePrivateProfileStringW(L"VR", L"PoseTagDelay", buf, path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::CalcViewHook::GetAerHalfEyeUU());
    WritePrivateProfileStringW(L"VR", L"AerHalfEyeUU", buf, path.c_str());
    WritePrivateProfileStringW(L"VR", L"AerSwapEyes", ME2VR::CalcViewHook::GetAerSwapEyes() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"AerFramePacing", ME2VR::CalcViewHook::GetAerFramePacing() ? L"1" : L"0", path.c_str());
    swprintf_s(buf, L"%d", ME2VR::CalcViewHook::GetAerFramePacingHz());
    WritePrivateProfileStringW(L"VR", L"AerFramePacingHz", buf, path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::CalcViewHook::GetDepthWarpGain());
    WritePrivateProfileStringW(L"VR", L"DepthWarpGain", buf, path.c_str());
    swprintf_s(buf, L"%.4f", ME2VR::CalcViewHook::GetDepthWarpConv());
    WritePrivateProfileStringW(L"VR", L"DepthWarpConv", buf, path.c_str());
    WritePrivateProfileStringW(L"VR", L"DepthWarpFlip", ME2VR::CalcViewHook::GetDepthWarpFlip() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"DibrAutoConverge", ME2VR::CalcViewHook::GetDibrAutoConverge() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"HUD", L"PerElementEnabled", ME2VR::HudProbe::GetEditorEnabled() ? L"1" : L"0", path.c_str());
    for (int i = 0; i < ME2VR::HudProbe::GroupCount(); ++i)
    {
        const auto cfg = ME2VR::HudProbe::GetGroupConfig(i);
        wchar_t key[32] = {};
        swprintf_s(key, L"G%d_X", i); swprintf_s(buf, L"%.3f", cfg.offsetX); WritePrivateProfileStringW(L"HUD", key, buf, path.c_str());
        swprintf_s(key, L"G%d_Y", i); swprintf_s(buf, L"%.3f", cfg.offsetY); WritePrivateProfileStringW(L"HUD", key, buf, path.c_str());
        swprintf_s(key, L"G%d_SX", i); swprintf_s(buf, L"%.3f", cfg.scaleX); WritePrivateProfileStringW(L"HUD", key, buf, path.c_str());
        swprintf_s(key, L"G%d_SY", i); swprintf_s(buf, L"%.3f", cfg.scaleY); WritePrivateProfileStringW(L"HUD", key, buf, path.c_str());
    }
    SaveFpStates(path);
    g_savedFlash = true;
    g_savedFrames = 120;
    ME2VR::Log::Line("[ME3MENU] settings saved to MELE3VR.ini");
}

void SeedVirtualMouse() noexcept
{
    g_mouseAnchorValid = false;
    POINT pt = {};
    if (!GetCursorPos(&pt)) return;
    HWND hwnd = g_hwnd ? g_hwnd : GetForegroundWindow();
    RECT rc = {}; POINT client = pt;
    if (hwnd && ScreenToClient(hwnd, &client) && GetClientRect(hwnd, &rc) && rc.right > 0 && rc.bottom > 0)
    {
        g_virtualMouseX = ClampF(static_cast<float>(client.x) / rc.right * g_w, 0.0f, static_cast<float>(g_w - 1), g_w * 0.5f);
        g_virtualMouseY = ClampF(static_cast<float>(client.y) / rc.bottom * g_h, 0.0f, static_cast<float>(g_h - 1), g_h * 0.5f);
        // [MENUNAV2] The anchor must (a) not sit on a screen edge - the OS cannot move the cursor
        // past one, so deltas in that direction never exist ("can move up but not down") - and
        // (b) actually BE where the cursor is. The first fix anchored at the window centre and
        // violated (b): the render resolution runs above the display, the window is taller than the
        // desktop, and its centre lies BELOW the screen - SetCursorPos silently clamped while the
        // anchor kept the unclamped value, so a permanent vertical delta pegged the virtual mouse.
        // Metrics APIs cannot be trusted for the correction either (the mod spoofs them for the
        // high-res window). Ground truth is the cursor itself: place it, READ BACK where the OS
        // actually put it, then step toward the interior so every direction has room.
        POINT centre = { rc.right / 2, rc.bottom / 2 };
        if (ClientToScreen(hwnd, &centre))
        {
            SetCursorPos(centre.x, centre.y);
            POINT landed = {};
            if (GetCursorPos(&landed))
            {
                const int insetY = (landed.y > 400) ? landed.y - 200 : landed.y + 200;
                const int insetX = (landed.x > 400) ? landed.x - 200 : landed.x + 200;
                SetCursorPos(insetX, insetY);
                if (GetCursorPos(&landed)) pt = landed;   // the anchor IS the cursor, wherever that truly is
            }
        }
    }
    else { g_virtualMouseX = g_w * 0.5f; g_virtualMouseY = g_h * 0.5f; }
    g_mouseAnchorScreen = pt;
    g_mouseAnchorValid = true;
    ClipCursor(nullptr);
    ShowCursor(TRUE);
}

LRESULT CALLBACK MenuWndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (g_open)
    {
        // [MENUWHEEL] Capture the wheel BEFORE the swallow below so the menu can scroll. The game
        // still never sees it. Taken before the g_menuKey comparison too: for wheel messages wparam
        // is the packed delta, not a key code, so that check is meaningless for them.
        if (msg == WM_MOUSEWHEEL)  g_wheelAccumV.fetch_add(GET_WHEEL_DELTA_WPARAM(w), std::memory_order_relaxed);
        if (msg == WM_MOUSEHWHEEL) g_wheelAccumH.fetch_add(GET_WHEEL_DELTA_WPARAM(w), std::memory_order_relaxed);
        switch (msg)
        {
        case WM_INPUT: case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR:
            if (w != static_cast<WPARAM>(g_menuKey)) return 0;
            break;
        default: break;
        }
    }
    return CallWindowProcW(g_originalWndProc, h, msg, w, l);
}

void SetGameWindow(HWND hwnd) noexcept
{
    if (hwnd == nullptr || hwnd == g_hwnd) return;
    if (g_hwnd && g_originalWndProc) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
    g_hwnd = hwnd;
    g_originalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MenuWndProc)));
}

void PollInsert() noexcept
{
    // [PROFILES] Hotkeys are edge-triggered. Always refresh their edge state, but suppress actions
    // while any key-capture widget is armed so the captured key cannot also load a profile.
    {
        static bool s_profDown[kProfileCount] = {};
        const bool canSwitch = g_profileHotkeys && g_valuesLoaded && !AnyRebinding();
        for (int i = 0; i < kProfileCount; ++i)
        {
            const bool d = (GetAsyncKeyState(g_profileKey[i]) & 0x8000) != 0;
            if (canSwitch && d && !s_profDown[i] && g_activeProfile != i) LoadProfile(i);
            s_profDown[i] = d;
        }
    }
    // [MARK] F9 = census/VR-status marker. Presentation is decided from engine signals the game
    // reuses for unrelated things, so the only reliable way to attribute a state to a SCREEN is for
    // the player to say "this one, now". One keypress stamps a numbered snapshot of every signal the
    // decision uses, which is what turns a [GUICEN] object delta into a named menu instead of a guess.
    // Reads state and writes a log line; it changes no behaviour and is safe to press at any time.
    {
        static bool s_markDown = false;
        static int s_markSeq = 0;
        const bool md = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (md && !s_markDown)
        {
            ++s_markSeq;
            unsigned char mv = 0, lk = 0;
            const bool owned = ME2VR::EngineProbe::IsMenuInputOwned(&mv, &lk);
            const bool mono = ME2VR::D3DCapture::GetMenuMode();
            const bool flatCine = ME2VR::CalcViewHook::GetCinematic();
            const bool vrCine = ME2VR::CalcViewHook::GetVrCineActive();
            char b[320] = {};
            sprintf_s(b, "[MARK] #%d  >>> %s <<<  (gm %d, gameplayCam %d, convoCam %d, namedGui %d, "
                         "move %u, look %u, owned %d, vrCine %d, flatCine %d, fov %.1f)",
                      s_markSeq,
                      mono ? "MONO - flat panel" : (flatCine ? "MONO - flat cinematic" :
                            (vrCine ? "VR - cinematic" : "VR - stereo")),
                      ME2VR::D3DCapture::GetAutoGameMode(),
                      ME2VR::EngineProbe::IsGameplayCamera() ? 1 : 0,
                      ME2VR::EngineProbe::IsConvoCamera() ? 1 : 0,
                      ME2VR::EngineProbe::GetNamedGuiActive() ? 1 : 0,
                      static_cast<unsigned>(mv), static_cast<unsigned>(lk), owned ? 1 : 0,
                      vrCine ? 1 : 0, flatCine ? 1 : 0,
                      static_cast<double>(ME2VR::CalcViewHook::GetGameRawFovH() * 2.0f * 57.2957795f));
            ME2VR::Log::Line(b);
        }
        s_markDown = md;
    }
    const bool down = (GetAsyncKeyState(g_menuKey) & 0x8000) != 0;
    if (!AnyRebinding() && down && !g_insertWasDown)
    {
        g_open = !g_open;
        if (g_open) { if (!g_valuesLoaded) LoadValues(); SeedVirtualMouse(); }
        else
        {
            g_mouseAnchorValid = false;
            // Auto-save on close. Settings previously persisted ONLY via the explicit Save buttons,
            // so a toggled checkbox (the VR cutscene opt-in, the whole 2026-07-29 session) silently
            // reverted on next boot and the feature "randomly" stopped working. Closing the menu is
            // the user saying "keep this".
            if (g_valuesLoaded) SaveValues();
        }
        ME2VR::Log::Line(std::string("[ME3MENU] settings menu ") + (g_open ? "opened" : "closed"));
    }
    g_insertWasDown = down;
}

void FeedMouse() noexcept
{
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = true;
    ClipCursor(nullptr);
    POINT pt = {};
    if (!g_mouseAnchorValid) SeedVirtualMouse();
    if (GetCursorPos(&pt) && g_mouseAnchorValid)
    {
        g_virtualMouseX = ClampF(g_virtualMouseX + (pt.x - g_mouseAnchorScreen.x), 0.0f, static_cast<float>(g_w - 1), g_w * 0.5f);
        g_virtualMouseY = ClampF(g_virtualMouseY + (pt.y - g_mouseAnchorScreen.y), 0.0f, static_cast<float>(g_h - 1), g_h * 0.5f);
        SetCursorPos(g_mouseAnchorScreen.x, g_mouseAnchorScreen.y);   // pin the real cursor
    }
    io.AddMousePosEvent(g_virtualMouseX, g_virtualMouseY);
    io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
    // [MENUWHEEL] Drain whatever the WndProc collected since the last frame. exchange(0) so a frame
    // can never replay or lose a notch, however the two threads interleave.
    const int wv = g_wheelAccumV.exchange(0, std::memory_order_relaxed);
    const int wh = g_wheelAccumH.exchange(0, std::memory_order_relaxed);
    if (wv != 0 || wh != 0)
        io.AddMouseWheelEvent(static_cast<float>(wh) / static_cast<float>(WHEEL_DELTA),
                              static_cast<float>(wv) / static_cast<float>(WHEEL_DELTA));
}

// [PROFILES] Every slot is an independent full-INI snapshot. The old implementation used the live
// INI as slot 0; loading any other slot overwrote it, so slot 0 silently became the loaded profile and
// could never restore its own renderer mode. Sidecars p0-p3 keep all four slots genuinely independent.
std::wstring ProfilePath(int slot) noexcept
{
    std::wstring path = IniPath();
    const size_t dot = path.rfind(L".ini");
    if (dot == std::wstring::npos) return path;
    wchar_t suffix[16] = {};
    swprintf_s(suffix, L".p%d.ini", slot);
    return path.substr(0, dot) + suffix;
}

void SaveProfile(int slot) noexcept
{
    if (slot < 0 || slot >= kProfileCount) return;
    SaveValues();                                   // flush the complete live state, including VR Mode
    const std::wstring dst = ProfilePath(slot);
    if (!CopyFileW(IniPath().c_str(), dst.c_str(), FALSE))
    {
        ME2VR::Log::Line(std::string("Failed to save settings profile: ") + ProfileName(slot) +
                         " (error " + std::to_string(GetLastError()) + ")");
        return;
    }
    g_activeProfile = slot;
    ME2VR::Log::Line(std::string("Saved settings profile: ") + ProfileName(slot) +
                     " (VR mode " + std::to_string(ME2VR::CalcViewHook::GetVrMode()) + ")");
}

// A slot that was never saved has no file, so keep the current settings rather than wiping them.
void LoadProfile(int slot) noexcept
{
    if (slot < 0 || slot >= kProfileCount) return;
    const std::wstring src = ProfilePath(slot);
    if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        g_activeProfile = slot;
        ME2VR::Log::Line(std::string("Settings profile ") + ProfileName(slot) + " is empty - keeping current settings.");
        return;
    }
    if (!CopyFileW(src.c_str(), IniPath().c_str(), FALSE))
    {
        ME2VR::Log::Line(std::string("Failed to load settings profile: ") + ProfileName(slot) +
                         " (error " + std::to_string(GetLastError()) + ")");
        return;
    }
    g_activeProfile = slot;
    LoadValues();
    ME2VR::Log::Line(std::string("Loaded settings profile: ") + ProfileName(slot) +
                     " (VR mode " + std::to_string(ME2VR::CalcViewHook::GetVrMode()) + ")");
}

void BuildUI() noexcept
{
    // Draggable + resizable window (title bar = drag handle); initial size/pos only on first use.
    ImGui::SetNextWindowSize(ImVec2(540, 430), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(g_w * 0.5f - 270.0f, g_h * 0.5f - 215.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("MASS EFFECT 3 - VR", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    // [VRSTATUS] Live presentation readout. Presentation is inferred from engine signals the game
    // reuses for unrelated purposes, so when VR "goes away" the useful question is always WHICH
    // owner flattened it - the menu controller or the cinematic one. This answers that at a glance
    // instead of from a log. NOTE: opening this menu asserts the same input locks a native menu
    // does, so the row shows what the state WAS before Insert took input (Insert's own locks are
    // excluded from the menu decision by design), and it never reads MENU while only Insert is up.
    {
        const bool mono = ME2VR::D3DCapture::GetMenuMode();
        const bool flatCine = ME2VR::CalcViewHook::GetCinematic();
        const bool vrCine = ME2VR::CalcViewHook::GetVrCineActive();
        const ImVec4 green(0.35f, 0.85f, 0.40f, 1.0f), amber(0.95f, 0.70f, 0.25f, 1.0f);
        ImGui::TextUnformatted("Presentation:");
        ImGui::SameLine();
        if (mono)          ImGui::TextColored(amber, "MONO  (menu / flat panel)");
        else if (flatCine) ImGui::TextColored(amber, "MONO  (flat cinematic)");
        else if (vrCine)   ImGui::TextColored(green, "VR  (cinematic in stereo)");
        else               ImGui::TextColored(green, "VR  (gameplay stereo)");
        // Raw state rows are debug tooling, not player UI - shown only while Diagnostics=1.
        if (ME2VR::Log::DiagnosticsOn())
        {
            unsigned char mv = 0, lk = 0;
            const bool owned = ME2VR::EngineProbe::IsMenuInputOwned(&mv, &lk);
            ImGui::TextDisabled("gm %d | gameplayCam %d | convoCam %d | namedGui %d | inputLocks %u/%u (owned %d)",
                                ME2VR::D3DCapture::GetAutoGameMode(),
                                ME2VR::EngineProbe::IsGameplayCamera() ? 1 : 0,
                                ME2VR::EngineProbe::IsConvoCamera() ? 1 : 0,
                                ME2VR::EngineProbe::GetNamedGuiActive() ? 1 : 0,
                                static_cast<unsigned>(mv), static_cast<unsigned>(lk), owned ? 1 : 0);
            ImGui::TextDisabled("F9 stamps a numbered marker + full state into the log (for the menu census).");
        }
    }
    ImGui::Separator();

    // Tab order and contents mirror ME2 exactly (2026-07-28). Previously everything lived in one
    // "Stereo" tab -- mode picker, resolution, DoF, stereo, AER, DIBR and the HUD trims, thirty
    // controls in a single list. Each control below now sits in the tab it occupies in ME2.
    if (ImGui::BeginTabBar("tabs"))
    {
        // ==== TAB: Tracking ====
        if (ImGui::BeginTabItem("Tracking"))
        {
            if (ImGui::Button("Recenter view")) ME2VR::Me2Xr::Recenter();
            ImGui::SameLine(); ImGui::TextDisabled("re-aligns forward to where you're looking");
            if (RebindWidget("Recenter hotkey", &g_recenterKey, &g_rebindingRecenter)) SaveValues();
            if (RebindWidget("First-person toggle", &g_fpToggleKey, &g_rebindingFpToggle)) SaveValues();
            ImGui::TextDisabled("Gamepad: double-tap Back to recenter, no keyboard needed.");

            ImGui::Separator();
            ImGui::TextUnformatted("Head look (exploration / render-side)");
            bool headLook = ME2VR::CalcViewHook::GetHeadLookUserEnabled();
            if (ImGui::Checkbox("Head tracking enabled (view look-around)", &headLook)) ME2VR::CalcViewHook::SetHeadLookUserEnabled(headLook);
            float sensitivity = ME2VR::CalcViewHook::GetLookSensitivity();
            ImGui::SetNextItemWidth(420.0f);
            if (ImGui::SliderFloat("Look sensitivity", &sensitivity, 0.25f, 2.0f, "%.2f")) ME2VR::CalcViewHook::SetLookSensitivity(sensitivity);
            bool invertYaw = ME2VR::CalcViewHook::GetInvertLookYaw();
            if (ImGui::Checkbox("Invert look yaw", &invertYaw)) ME2VR::CalcViewHook::SetInvertLookYaw(invertYaw);
            ImGui::SameLine();
            bool invertPitch = ME2VR::CalcViewHook::GetInvertLookPitch();
            if (ImGui::Checkbox("Invert look pitch", &invertPitch)) ME2VR::CalcViewHook::SetInvertLookPitch(invertPitch);
            ImGui::Separator();
            ImGui::TextUnformatted("Head tracking smoothness");
            float sm = ME2VR::CalcViewHook::GetHeadLookSmoothing();
            ImGui::SetNextItemWidth(420.0f);
            if (ImGui::SliderFloat("Head-look smoothing", &sm, 0.0f, 0.9f, "%.2f")) ME2VR::CalcViewHook::SetHeadLookSmoothing(sm);
            ImGui::TextDisabled("Low-pass when the head is nearly still; fast turns always pass through 1:1.");
            float ptd = ME2VR::CalcViewHook::GetPoseTagDelayFrames();
            ImGui::SetNextItemWidth(420.0f);
            if (ImGui::SliderFloat("Tracking smoothness (pose-tag delay)", &ptd, 0.0f, 3.0f, "%.2f"))
                ME2VR::CalcViewHook::SetPoseTagDelayFrames(ptd);
            ImGui::TextDisabled("RAISE until the world stops dragging WITH your head; LOWER if it leads ahead.");
            ImGui::TextDisabled("Depends on your PC - set it while wearing the headset. Start around 2.0.");
            ImGui::Separator();
            bool ha = ME2VR::EngineProbe::GetHeadAimEnabled();
            if (ImGui::Checkbox("Head drives aim (combat)", &ha)) ME2VR::EngineProbe::SetHeadAimEnabled(ha);
            ImGui::TextDisabled("Where you look is where you shoot. The stick still works on top.");
            ImGui::TextDisabled("Also turns your CHARACTER, which is how the game decides what you are facing.");
            if (ha)
            {
                bool iy = ME2VR::EngineProbe::GetHeadAimInvertYaw();
                if (ImGui::Checkbox("Invert aim yaw", &iy)) ME2VR::EngineProbe::SetHeadAimInvertYaw(iy);
                ImGui::SameLine();
                bool ip = ME2VR::EngineProbe::GetHeadAimInvertPitch();
                if (ImGui::Checkbox("Invert aim pitch", &ip)) ME2VR::EngineProbe::SetHeadAimInvertPitch(ip);
                bool wo = ME2VR::EngineProbe::GetHeadAimWeaponOnly();
                if (ImGui::Checkbox("Only while weapon is drawn", &wo)) ME2VR::EngineProbe::SetHeadAimWeaponOnly(wo);
                bool co = ME2VR::EngineProbe::GetHeadAimCoverOff();
                if (ImGui::Checkbox("Turn off head aim in cover", &co))
                {
                    ME2VR::EngineProbe::SetHeadAimCoverOff(co);
                    // Stamp the flip: an A/B test is only worth anything if the log says which side
                    // of it each firefight was played on.
                    ME2VR::Log::Line(std::string("[AIMCOVER] head aim in cover: ") +
                                     (co ? "OFF (fix active)" : "ON (pre-fix behaviour)"));
                }
                ImGui::TextDisabled("The game aims for you in cover. Off, both fight and the view swings away.");
            }

            ImGui::Separator();
            bool pe = ME2VR::CalcViewHook::GetHeadPosEnabled();
            if (ImGui::Checkbox("Lean", &pe)) ME2VR::CalcViewHook::SetHeadPosEnabled(pe);
            float ps = ME2VR::CalcViewHook::GetHeadPosScale();
            ImGui::SetNextItemWidth(420.0f);
            if (ImGui::SliderFloat("Lean strength", &ps, 0.0f, 6.0f, "%.2f"))
                ME2VR::CalcViewHook::SetHeadPosScale(ClampF(ps, 0.0f, 12.0f, 1.0f));
            bool linv = ME2VR::CalcViewHook::GetLeanInvertFwd();
            if (ImGui::Checkbox("Invert lean forward/back", &linv)) ME2VR::CalcViewHook::SetLeanInvertFwd(linv);

            ImGui::Separator();
            bool mfh = ME2VR::Menu::GetMoveFollowsHead();
            if (ImGui::Checkbox("Run where you look", &mfh)) ME2VR::Menu::SetMoveFollowsHead(mfh);
            bool dp = ME2VR::Menu::GetDecoupledPitch();
            if (ImGui::Checkbox("Decoupled pitch (look up/down with head only)", &dp)) ME2VR::Menu::SetDecoupledPitch(dp);
            ImGui::SameLine();
            bool dy = ME2VR::Menu::GetDecoupledYaw();
            if (ImGui::Checkbox("Decoupled yaw (turn left/right with head only)", &dy)) ME2VR::Menu::SetDecoupledYaw(dy);

            ImGui::Separator();
            bool mm = ME2VR::D3DCapture::GetMonoMenus();
            if (ImGui::Checkbox("Flat menus and galaxy map", &mm)) ME2VR::D3DCapture::SetMonoMenus(mm);

            // [MIRRORREC 2026-08-22] Desktop-mirror rate, for screen recording. In VR the flat window is
            // only a mirror - the headset image is already submitted before the mirror present - so it is
            // presented 1 frame in N. That throttle is not cosmetic: presenting every frame creates DWM
            // back-pressure that stalls the render thread, which is what capped ME2 at 60fps (87 -> 119.7
            // when throttled) and cost ME1 5.8ms -> 14.2ms per frame on identical work. It changes NOTHING
            // about resolution, image quality or the headset view - only the monitor picture and the frame
            // rate - so it is opt-in and the label says what it costs.
            {
                const int cur = ME2VR::D3DCapture::GetMirrorPresentEvery();
                int sel = (cur <= 1) ? 2 : ((cur <= 2) ? 1 : 0);
                static const char* const kMirrorItems[] = { "Normal (best frame rate)",
                                                            "Smooth - every 2nd frame",
                                                            "Every frame (smoothest, costs the most)" };
                ImGui::SetNextItemWidth(360.0f);
                if (ImGui::Combo("Desktop mirror (screen recording)", &sel, kMirrorItems, 3))
                {
                    const int v = (sel == 2) ? 1 : ((sel == 1) ? 2 : 8);
                    ME2VR::D3DCapture::SetMirrorPresentEvery(v); SaveValues();
                }
                ImGui::TextDisabled("Only the picture on your monitor. Does not change resolution, quality or");
                ImGui::TextDisabled("the headset image. Raise it to record, put it back to Normal to play.");
            }

            ImGui::Separator();
            ImGui::TextUnformatted("Flat panel (menus and anything shown flat)");
            float v = ME2VR::Me2Xr::GetMenuScreenDist();
            ImGui::SetNextItemWidth(420.0f);
            if (ImGui::SliderFloat("Flat panel distance", &v, 0.4f, 6.0f, "%.2f m")) ME2VR::Me2Xr::SetMenuScreenDist(v);
            v = ME2VR::Me2Xr::GetMenuScreenSize();
            ImGui::SetNextItemWidth(420.0f);
            if (ImGui::SliderFloat("Flat panel size", &v, 0.8f, 6.0f, "%.2f m")) ME2VR::Me2Xr::SetMenuScreenSize(v);

            // ---- [VRCINE] Conversations & Cutscenes - ME2's design, ported as-is ----
            if (ImGui::CollapsingHeader("Conversations & Cutscenes (experimental)", ImGuiTreeNodeFlags_DefaultOpen))
            {
                bool cvc = ME2VR::CalcViewHook::GetCineVrConvo();
                if (ImGui::Checkbox("VR conversations", &cvc))
                {
                    ME2VR::CalcViewHook::SetCineVrConvo(cvc);
                    if (cvc) ME2VR::ConvoFp::SetEnabled(false);
                }
                bool cvs = ME2VR::CalcViewHook::GetCineVrCutscene();
                if (ImGui::Checkbox("VR cutscenes", &cvs))
                {
                    ME2VR::CalcViewHook::SetCineVrCutscene(cvs);

                }
                bool cht = ME2VR::CalcViewHook::GetCineVrHeadTracking();
                if (ImGui::Checkbox("Head tracking during cinematics", &cht)) ME2VR::CalcViewHook::SetCineVrHeadTracking(cht);
                float cz = ME2VR::Me2Xr::GetCineScreenZoom();
                ImGui::SetNextItemWidth(420.0f);
                if (ImGui::SliderFloat("Cutscene zoom", &cz, 0.5f, 3.0f, "%.2f")) ME2VR::Me2Xr::SetCineScreenZoom(cz);
                ImGui::TextDisabled("Size of the flat cutscene screen when a scene is NOT rendered in VR.");

                ImGui::Separator();
                bool cfp = ME2VR::ConvoFp::GetEnabled();
                if (ImGui::Checkbox("First person conversations [EXPERIMENTAL]", &cfp))
                {
                    ME2VR::ConvoFp::SetEnabled(cfp);
                    if (cfp) ME2VR::CalcViewHook::SetCineVrConvo(false);
                }
                float fz = ME2VR::ConvoFp::GetZoom();
                ImGui::SetNextItemWidth(420.0f);
                if (ImGui::SliderFloat("First person conversation zoom", &fz, 1.0f, 2.5f, "%.2fx")) ME2VR::ConvoFp::SetZoom(fz);
                if (cfp)
                {
                    float turn=ME2VR::ConvoFp::GetTurnRate(); ImGui::SetNextItemWidth(360.0f); if(ImGui::SliderFloat("Turn to speaker (deg/sec)",&turn,0,360,"%.0f"))ME2VR::ConvoFp::SetTurnRate(turn);
                    float follow=ME2VR::ConvoFp::GetAnimFollow(); ImGui::SetNextItemWidth(360.0f); if(ImGui::SliderFloat("Animation follow",&follow,0,1,"%.2f"))ME2VR::ConvoFp::SetAnimFollow(follow);
                    float up=ME2VR::ConvoFp::GetEyeUpUU(); ImGui::SetNextItemWidth(360.0f); if(ImGui::SliderFloat("Eye height trim (uu)",&up,-40,40,"%+.0f"))ME2VR::ConvoFp::SetEyeUpUU(up);
                    bool hh=ME2VR::ConvoFp::GetHideHead(); if(ImGui::Checkbox("Hide Shepard's head",&hh))ME2VR::ConvoFp::SetHideHead(hh);
                    bool kd=ME2VR::ConvoFp::GetKillDof(); if(ImGui::Checkbox("Disable depth of field in conversations",&kd))ME2VR::ConvoFp::SetKillDof(kd);
                    bool inv=ME2VR::CalcViewHook::GetConvoFpInvertFacing(); if(ImGui::Checkbox("Invert facing",&inv))ME2VR::CalcViewHook::SetConvoFpInvertFacing(inv);
                    if(ME2VR::ConvoFp::IsArmed()){ME2VR::ConvoFp::Diag d{};ME2VR::ConvoFp::GetDiag(&d);ImGui::Text("ACTIVE  source=%d  speakers=%d  yaw=%.0f",d.source,d.speakerCount,d.baseYawDeg);}
                }
            }

            ImGui::Separator();
            bool flat = ME2VR::D3DCapture::GetManualFlat();
            if (ImGui::Checkbox("Flat screen mode (manual override)", &flat)) ME2VR::D3DCapture::SetManualFlat(flat);
            ImGui::TextDisabled("Hotkey: F2");

            ImGui::Separator();
            bool ovl = ME2VR::D3DCapture::GetUiOverlayMode();
            if (ImGui::Checkbox("HUD: world-locked layer (stops head movement)", &ovl)) ME2VR::D3DCapture::SetUiOverlayMode(ovl);
            ImGui::TextDisabled("ON = HUD pinned in space; turn your head and it stays put. Recenter re-centers it.");

            ImGui::EndTabItem();
        }

        // ==== TAB: First Person (per-state eye offsets) ====
        if (ImGui::BeginTabItem("First Person"))
        {
            bool fpOn = ME2VR::EngineProbe::GetFirstPerson();
            if (ImGui::Checkbox("First person enabled", &fpOn)) ME2VR::EngineProbe::SetFirstPerson(fpOn);
            ImGui::SameLine(); ImGui::Text("Hotkey: %s", KeyName(g_fpToggleKey));
            bool ctp = ME2VR::EngineProbe::GetCoverThirdPerson();
            if (ImGui::Checkbox("Third person while in cover", &ctp)) ME2VR::EngineProbe::SetCoverThirdPerson(ctp);
            ImGui::TextWrapped("ME3 is a third-person game, so first person is fitted on top of it - and each camera "
                               "state needs its own eye position. Pick a state below, nudge Forward / Up / Right until it sits right, then Save.");
            ImGui::Separator();

            auto fpRow = [&](int id)
            {
                ME2VR::EngineProbe::FpStateCfg* s = ME2VR::EngineProbe::GetFpStateCfg(id);
                if (s == nullptr) return;
                ImGui::PushID(id);
                if (ImGui::CollapsingHeader(ME2VR::EngineProbe::FpStateLabel(id)))
                {
                    if (ImGui::SmallButton("Reset to default")) *s = ME2VR::EngineProbe::FpStateDefault(id);
                    ImGui::Checkbox("First person", &s->on);
                    ImGui::SameLine(); ImGui::Checkbox("Hide head", &s->hideHead);
                    ImGui::SameLine(); ImGui::Checkbox("Hide body", &s->hideBody);
                    ImGui::SetNextItemWidth(360.0f); ImGui::SliderFloat("Forward", &s->x, -100.0f, 200.0f, "%.0f");
                    ImGui::SetNextItemWidth(360.0f); ImGui::SliderFloat("Up",      &s->z, -100.0f, 200.0f, "%.0f");
                    ImGui::SetNextItemWidth(360.0f); ImGui::SliderFloat("Right",   &s->y, -100.0f, 100.0f, "%.0f");
                }
                ImGui::PopID();
            };
            for (int i = 0; i < ME2VR::EngineProbe::FpStateCount(); ++i) fpRow(i);

            ImGui::Separator();
            if (ImGui::Button("Restore ALL to defaults"))
                for (int i = 0; i < ME2VR::EngineProbe::FpStateCount(); ++i)
                    if (auto* s = ME2VR::EngineProbe::GetFpStateCfg(i)) *s = ME2VR::EngineProbe::FpStateDefault(i);
            ImGui::SameLine();
            if (ImGui::Button("Save")) SaveValues();
            ImGui::EndTabItem();
        }

        // ==== TAB: View (how much of the headset the game fills) ====
        if (ImGui::BeginTabItem("View"))
        {
            bool ff = ME2VR::Me2Xr::GetVrFovFill();
            if (ImGui::Checkbox("Fill the headset FOV", &ff)) ME2VR::Me2Xr::SetVrFovFill(ff);
            if (ff)
            {
                float fh = ME2VR::Me2Xr::GetVrFillH();
                ImGui::SetNextItemWidth(420.0f);
                if (ImGui::SliderFloat("Fill H", &fh, 0.5f, 1.25f, "%.3f")) ME2VR::Me2Xr::SetVrFillH(fh);
                float fv = ME2VR::Me2Xr::GetVrFillV();
                ImGui::SetNextItemWidth(420.0f);
                if (ImGui::SliderFloat("Fill V", &fv, 0.5f, 1.25f, "%.3f")) ME2VR::Me2Xr::SetVrFillV(fv);
            }
            ImGui::EndTabItem();
        }

        // ==== TAB: VR (mode, resolution, per-mode controls) ====
        if (ImGui::BeginTabItem("VR"))
        {
            bool vr = ME2VR::CalcViewHook::GetVrEnabled();
            if (ImGui::Checkbox("Enable VR (OpenXR headset output)", &vr)) ME2VR::CalcViewHook::SetVrEnabled(vr);
            ImGui::TextDisabled("Hotkey: F4");
            ImGui::Separator();

            // Mode picker. The legacy SBS split (mode 1) was REMOVED 2026-07-28 to match ME2, which
            // dropped it once same-frame stereo superseded it. LoadValues migrates any ini still
            // holding mode 1 up to 4, so an old config cannot select a mode the picker no longer shows.
            int mode = ME2VR::CalcViewHook::GetVrMode();
            ImGui::TextUnformatted("Render mode");
            if (ImGui::RadioButton("Mono (flat panel)", mode == 0)) { ME2VR::CalcViewHook::SetVrMode(0); mode = 0; }
            ImGui::SameLine();
            if (ImGui::RadioButton("Stereo", mode == 4)) { ME2VR::CalcViewHook::SetVrMode(4); mode = 4; }
            ImGui::SameLine();
            if (ImGui::RadioButton("AER", mode == 2)) { ME2VR::CalcViewHook::SetVrMode(2); mode = 2; }
            ImGui::SameLine();
            if (ImGui::RadioButton("DIBR", mode == 3)) { ME2VR::CalcViewHook::SetVrMode(3); mode = 3; }
            ImGui::Separator();

            // Render resolution. 16:9, matching ME1/ME2. ME3 previously offered SQUARE sizes only, on the
            // theory that a near-square eye buffer wastes width -- but the engine silently refuses a square
            // override (an ini of 4096x4096 rendered 4096x2160), so the option was never real, and ME2
            // recorded the same result. Stereo splits this backbuffer side-by-side, so PER-EYE is half the
            // width: 6144x3456 -> 3072x3456, the first preset at/above a typical 3072x3264 recommendation.
            // Below that the mod submits less than native and the compositor upscales - that is the sharpness gap.
            {
                ImGui::TextUnformatted("Render resolution");
                ImGui::TextDisabled("Takes effect next time you launch.");
                const std::wstring ip = IniPath();
                const int curW = GetPrivateProfileIntW(L"VR", L"RenderW", 0, ip.c_str());
                const int curH = GetPrivateProfileIntW(L"VR", L"RenderH", 0, ip.c_str());
                static const char* const kResLabels[] = {
                    "3K  -  3072 x 1728",
                    "4K  -  4096 x 2304",
                    "5K  -  5120 x 2880",
                    "6K  -  6144 x 3456",
                    "8K  -  8192 x 4608",
                    "10K - 10240 x 5760",
                };
                static const int kResW[] = { 3072, 4096, 5120, 6144, 8192, 10240 };
                static const int kResH[] = { 1728, 2304, 2880, 3456, 4608,  5760 };
                constexpr int kResCount = static_cast<int>(sizeof(kResW) / sizeof(kResW[0]));
                int idx = 3;   // 6K if the ini holds something the mod doesn't recognise
                for (int k = 0; k < kResCount; ++k) if (kResW[k] == curW && kResH[k] == curH) { idx = k; break; }
                ImGui::SetNextItemWidth(420.0f);
                if (ImGui::Combo("##vrres", &idx, kResLabels, kResCount))
                {
                    wchar_t val[16];
                    swprintf_s(val, L"%d", kResW[idx]); WritePrivateProfileStringW(L"VR", L"RenderW", val, ip.c_str());
                    swprintf_s(val, L"%d", kResH[idx]); WritePrivateProfileStringW(L"VR", L"RenderH", val, ip.c_str());
                }
                if (curW != 0 && curW == curH)
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                       "Current %dx%d is square - the engine refuses those. Pick one above.", curW, curH);
            }
            ImGui::Separator();

            if (mode == 4)          // ---- Stereo (same-frame) ----
            {
                float he = ME2VR::CalcViewHook::GetHalfEyeUU();
                ImGui::SetNextItemWidth(420.0f);
                if (ImGui::SliderFloat("Eye separation##sfr", &he, 0.0f, 10.0f, "%.2f uu"))
                    ME2VR::CalcViewHook::SetHalfEyeUU(ClampF(he, 0.0f, 16.0f, 6.0f));
                ImGui::TextDisabled("1.6 is life-size. Higher shrinks the world and makes it feel like a model.");

                float cv = ME2VR::CalcViewHook::GetSfrConvergence();
                ImGui::SetNextItemWidth(420.0f);
                if (ImGui::SliderFloat("Convergence##sfr", &cv, -0.12f, 0.12f, "%.4f"))
                    ME2VR::CalcViewHook::SetSfrConvergence(cv);

                bool sw = ME2VR::CalcViewHook::GetSwapEyes();
                if (ImGui::Checkbox("Swap eyes (invert depth)##sfr", &sw)) ME2VR::CalcViewHook::SetSwapEyes(sw);

                bool sp = ME2VR::CalcViewHook::GetStereoFramePacing();
                if (ImGui::Checkbox("Display Locked Pacing##sfr", &sp)) ME2VR::CalcViewHook::SetStereoFramePacing(sp);
                bool fr = ME2VR::CalcViewHook::GetFullRefreshPacing();
                if (ImGui::Checkbox("Run at full frame rate##sfr", &fr)) ME2VR::CalcViewHook::SetFullRefreshPacing(fr);
            }
            else if (mode == 2)     // ---- AER ----
            {
                float ahe = ME2VR::CalcViewHook::GetAerHalfEyeUU();
                ImGui::SetNextItemWidth(420.0f);
                if (ImGui::SliderFloat("AER eye separation", &ahe, 0.0f, 8.0f, "%.2f uu"))
                    ME2VR::CalcViewHook::SetAerHalfEyeUU(ClampF(ahe, 0.0f, 8.0f, 1.6f));
                bool asw = ME2VR::CalcViewHook::GetAerSwapEyes();
                if (ImGui::Checkbox("Swap eyes (invert depth)##aer", &asw)) ME2VR::CalcViewHook::SetAerSwapEyes(asw);
                bool ap = ME2VR::CalcViewHook::GetAerFramePacing();
                if (ImGui::Checkbox("Display-locked pacing (the flicker fix)", &ap)) ME2VR::CalcViewHook::SetAerFramePacing(ap);
                bool afr = ME2VR::CalcViewHook::GetFullRefreshPacing();
                if (ImGui::Checkbox("Run at full frame rate##aer", &afr)) ME2VR::CalcViewHook::SetFullRefreshPacing(afr);
                int hz = ME2VR::CalcViewHook::GetAerFramePacingHz();
                ImGui::SetNextItemWidth(420.0f);
                if (ImGui::SliderInt("Pacing Hz (0 = auto-learn)", &hz, 0, 144))
                    ME2VR::CalcViewHook::SetAerFramePacingHz(ClampI(hz, 0, 240, 0));
            }
            else if (mode == 3)     // ---- DIBR ----
            {
                float gain = ME2VR::CalcViewHook::GetDepthWarpGain();
                ImGui::SetNextItemWidth(420.0f);
                if (ImGui::SliderFloat("Depth strength", &gain, 0.0f, 5.0f, "%.2f"))
                    ME2VR::CalcViewHook::SetDepthWarpGain(ClampF(gain, 0.0f, 10.0f, 1.2f));
                bool autoConv = ME2VR::CalcViewHook::GetDibrAutoConverge();
                if (ImGui::Checkbox("Auto-Convergence", &autoConv))
                    ME2VR::CalcViewHook::SetDibrAutoConverge(autoConv);
                if (!autoConv)
                {
                    float conv = ME2VR::CalcViewHook::GetDepthWarpConv();
                    ImGui::SetNextItemWidth(420.0f);
                    if (ImGui::SliderFloat("Convergence plane", &conv, 0.90f, 1.00f, "%.4f"))
                        ME2VR::CalcViewHook::SetDepthWarpConv(ClampF(conv, 0.90f, 1.005f, 0.985f));
                }
                bool flip = ME2VR::CalcViewHook::GetDepthWarpFlip();
                if (ImGui::Checkbox("Flip depth", &flip)) ME2VR::CalcViewHook::SetDepthWarpFlip(flip);
            }
            ImGui::Separator();
            bool qf = ME2VR::Me2Xr::GetQuestFovMatch();
            if (ImGui::Checkbox("Quest Link image fix", &qf)) ME2VR::Me2Xr::SetQuestFovMatch(qf);

            if (ImGui::Checkbox("Disable depth of field", &g_disableDof)) SaveValues();
            ImGui::TextDisabled("Applied on the next game launch.");

            bool ud = ME2VR::D3DCapture::GetUiDupEnabled();
            if (ImGui::Checkbox("HUD in both eyes", &ud)) ME2VR::D3DCapture::SetUiDupEnabled(ud);
            ImGui::TextDisabled("ON = HUD/text into both eyes.");
            ImGui::EndTabItem();
        }

        // ==== TAB: Comfort (where this menu panel sits) ====
        if (ImGui::BeginTabItem("Comfort"))
        {
            float v = ME2VR::Me2Xr::GetMenuPanelDist();
            ImGui::SetNextItemWidth(420.0f);
            if (ImGui::SliderFloat("Menu distance (m)", &v, 0.8f, 4.0f, "%.2f")) ME2VR::Me2Xr::SetMenuPanelDist(v);
            v = ME2VR::Me2Xr::GetMenuPanelSize();
            ImGui::SetNextItemWidth(420.0f);
            if (ImGui::SliderFloat("Menu size (m wide)", &v, 0.5f, 3.5f, "%.2f")) ME2VR::Me2Xr::SetMenuPanelSize(v);
            v = ME2VR::Me2Xr::GetMenuPanelOffX();
            ImGui::SetNextItemWidth(420.0f);
            if (ImGui::SliderFloat("Menu left/right (m)", &v, -1.5f, 1.5f, "%+.2f")) ME2VR::Me2Xr::SetMenuPanelOffX(v);
            v = ME2VR::Me2Xr::GetMenuPanelOffY();
            ImGui::SetNextItemWidth(420.0f);
            if (ImGui::SliderFloat("Menu up/down (m)", &v, -1.0f, 1.0f, "%+.2f")) ME2VR::Me2Xr::SetMenuPanelOffY(v);
            if (ImGui::Button("Center menu")) { ME2VR::Me2Xr::SetMenuPanelOffX(0.0f); ME2VR::Me2Xr::SetMenuPanelOffY(0.0f); }
            ImGui::SameLine();
            if (ImGui::Button("Reset panel"))
            {
                ME2VR::Me2Xr::SetMenuPanelDist(1.5f); ME2VR::Me2Xr::SetMenuPanelSize(1.4f);
                ME2VR::Me2Xr::SetMenuPanelOffX(0.0f); ME2VR::Me2Xr::SetMenuPanelOffY(0.0f);
            }
            ImGui::Separator();
            ImGui::TextUnformatted("Menu open key");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", KeyName(g_menuKey));
            ImGui::SameLine();
            if (ImGui::SmallButton(g_rebindingMenuKey ? "press a key..." : "Rebind##menukey"))
                g_rebindingMenuKey = !g_rebindingMenuKey;
            if (g_rebindingMenuKey)
            {
                for (int vk = 0x08; vk <= 0xFE; ++vk)
                {
                    if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
                    if ((GetAsyncKeyState(vk) & 0x8000) == 0) continue;
                    if (vk == VK_ESCAPE) { g_rebindingMenuKey = false; break; }
                    g_menuKey = vk;
                    g_insertWasDown = true;  // consume the key that completed capture
                    g_rebindingMenuKey = false;
                    SaveValues();
                    break;
                }
            }
            ImGui::EndTabItem();
        }

        // ==== TAB: HUD ====
        // [HUDOPEN 2026-08-21] Every player has a different
        // headset, IPD and comfort zone, so nudging HUD pieces into view is ordinary setup, not
        // an expert feature.
        if (ImGui::BeginTabItem("HUD"))
        {
            ImGui::TextDisabled("Trims on top of the auto un-stretch. Keep near 1.0 / 0.");
            // [HUDNOTE] The element transforms are applied from the game's own HUD event, which
            // does not tick while this menu owns input, so edits land only once it is closed.
            ImGui::TextDisabled("Close this menu to see HUD size changes.");
            float sx = ME2VR::CalcViewHook::GetSfrUiScaleX();
            ImGui::SetNextItemWidth(420.0f);
            if (ImGui::SliderFloat("Scale X (width)", &sx, 0.2f, 1.5f, "%.3f")) ME2VR::CalcViewHook::SetSfrUiScaleX(sx);
            float sy = ME2VR::CalcViewHook::GetSfrUiScaleY();
            ImGui::SetNextItemWidth(420.0f);
            if (ImGui::SliderFloat("Scale Y (height)", &sy, 0.2f, 1.5f, "%.3f")) ME2VR::CalcViewHook::SetSfrUiScaleY(sy);
            float ox = ME2VR::CalcViewHook::GetSfrUiOffX();
            ImGui::SetNextItemWidth(420.0f);
            if (ImGui::SliderFloat("Offset X", &ox, -0.5f, 0.5f, "%.3f")) ME2VR::CalcViewHook::SetSfrUiOffX(ox);
            float oy = ME2VR::CalcViewHook::GetSfrUiOffY();
            ImGui::SetNextItemWidth(420.0f);
            if (ImGui::SliderFloat("Offset Y", &oy, -0.5f, 0.5f, "%.3f")) ME2VR::CalcViewHook::SetSfrUiOffY(oy);
            if (ImGui::Button("Reset HUD"))
            {
                ME2VR::CalcViewHook::SetSfrUiScaleX(1.0f); ME2VR::CalcViewHook::SetSfrUiScaleY(1.0f);
                ME2VR::CalcViewHook::SetSfrUiOffX(0.0f);   ME2VR::CalcViewHook::SetSfrUiOffY(0.0f);
            }
            ImGui::SameLine();
            if (ImGui::Button("Save##hud")) SaveValues();
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Individual HUD pieces", ImGuiTreeNodeFlags_DefaultOpen))
            {
                bool enabled = ME2VR::HudProbe::GetEditorEnabled();
                if (ImGui::Checkbox("Enable per-element HUD control", &enabled)) ME2VR::HudProbe::SetEditorEnabled(enabled);
                ImGui::TextDisabled("Reset restores the shipped layout.");
                if (ImGui::Button("Reset all pieces")) ME2VR::HudProbe::ResetAll();
                for (int i = 0; i < ME2VR::HudProbe::GroupCount(); ++i)
                {
                    ImGui::PushID(i);
                    auto cfg = ME2VR::HudProbe::GetGroupConfig(i);
                    if (ImGui::TreeNode(ME2VR::HudProbe::GroupLabel(i)))
                    {
                        bool changed = false;
                        // [HUDCONSIST 2026-08-22] Same labels and same control order as ME1/ME2.
                        ImGui::SetNextItemWidth(360.0f); changed |= ImGui::SliderFloat("Left / right", &cfg.offsetX, -600.0f, 600.0f, "%.1f");
                        ImGui::SetNextItemWidth(360.0f); changed |= ImGui::SliderFloat("Up / down", &cfg.offsetY, -500.0f, 500.0f, "%.1f");
                        ImGui::SetNextItemWidth(360.0f); changed |= ImGui::SliderFloat("Size X (width)", &cfg.scaleX, 0.30f, 3.00f, "%.3f");
                        ImGui::SetNextItemWidth(360.0f); changed |= ImGui::SliderFloat("Size Y (height)", &cfg.scaleY, 0.30f, 3.00f, "%.3f");
                        if (ImGui::SmallButton("Link Y to X")) { cfg.scaleY = cfg.scaleX; changed = true; }
                        if (changed) ME2VR::HudProbe::SetGroupConfig(i, cfg);
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Reset")) ME2VR::HudProbe::ResetGroup(i);
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTabItem();
        }

        // ==== TAB: Profiles (standard, matching ME2) ====
        if (ImGui::BeginTabItem("Profiles"))
        {
            if (ImGui::CollapsingHeader("Profiles", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::TextDisabled("Each slot is a full snapshot of your settings.");
                ImGui::Separator();
                ImGui::Text("Active: %s", ProfileName(g_activeProfile));
                for (int i = 0; i < kProfileCount; ++i)
                {
                    ImGui::PushID(1000 + i);
                    if (ImGui::RadioButton(ProfileName(i), g_activeProfile == i)) LoadProfile(i);
                    ImGui::SameLine(220.0f);
                    if (ImGui::SmallButton("Save to this slot")) SaveProfile(i);
                    ImGui::PopID();
                }
            }

            if (ImGui::CollapsingHeader("Profile hotkeys", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (ImGui::Checkbox("Enable profile hotkeys", &g_profileHotkeys)) SaveValues();
                ImGui::Separator();
                for (int i = 0; i < kProfileCount; ++i)
                {
                    ImGui::PushID(2000 + i);
                    ImGui::Text("%s", ProfileName(i));
                    ImGui::SameLine(160.0f);
                    ImGui::TextDisabled("%s", KeyName(g_profileKey[i]));
                    ImGui::SameLine();
                    if (ImGui::SmallButton(g_rebindingProfile == i ? "press..." : "Rebind"))
                        g_rebindingProfile = (g_rebindingProfile == i) ? -1 : i;
                    ImGui::PopID();
                }
                if (g_rebindingProfile >= 0 && g_rebindingProfile < kProfileCount)
                {
                    for (int vk = 0x08; vk <= 0xFE; ++vk)
                    {
                        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == g_menuKey) continue;
                        if ((GetAsyncKeyState(vk) & 0x8000) == 0) continue;
                        if (vk == VK_ESCAPE) { g_rebindingProfile = -1; break; }
                        g_profileKey[g_rebindingProfile] = vk;
                        g_rebindingProfile = -1;
                        SaveValues();
                        break;
                    }
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    if (ImGui::Button("Save")) SaveValues();
    if (g_savedFlash && g_savedFrames > 0) { --g_savedFrames; ImGui::SameLine(); ImGui::TextDisabled("saved"); }
    ImGui::SameLine(); ImGui::TextDisabled("%s: close   (settings -> MELE3VR.ini)", KeyName(g_menuKey));
    ImGui::End();
}
}  // namespace

void Init(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height) noexcept
{
    if (g_ready || device == nullptr || context == nullptr || width <= 0 || height <= 0) return;
    g_device = device; g_context = context; g_device->AddRef(); g_context->AddRef();
    g_w = width; g_h = height;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = g_w; td.Height = g_h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_tex)) || !g_tex) { SafeRelease(g_context); SafeRelease(g_device); return; }
    if (FAILED(g_device->CreateRenderTargetView(g_tex, nullptr, &g_rtv)) || !g_rtv) { SafeRelease(g_tex); SafeRelease(g_context); SafeRelease(g_device); return; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(1.3f);
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = 0.85f;
    if (!ImGui_ImplDX11_Init(g_device, g_context)) { ImGui::DestroyContext(); SafeRelease(g_rtv); SafeRelease(g_tex); SafeRelease(g_context); SafeRelease(g_device); return; }

    LoadValues();
    g_ready = true;
    ME2VR::Log::Line(std::string("[ME3MENU] settings menu ready (") + KeyName(g_menuKey) + " to open).");
}

void OnPresent(IDXGISwapChain* swapChain) noexcept
{
    if (swapChain != nullptr && g_hwnd == nullptr)
    {
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (SUCCEEDED(swapChain->GetDesc(&desc))) SetGameWindow(desc.OutputWindow);
    }
    PollInsert();
    InstallPadHooks();   // [MOVEFIX] one-shot; no-op until the game loads an XInput dll
    InstallMouseHooks(); // [DECOUPLE] raw-input mouse half (user32 is always loaded)
    EnforceFreeze(g_open);   // freeze game look/move input while the menu is open
}

ID3D11Texture2D* RenderFrame() noexcept
{
    if (!g_ready || !g_open || g_context == nullptr || g_rtv == nullptr) return nullptr;
    FeedMouse();
    ImGui_ImplDX11_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(g_w), static_cast<float>(g_h));
    if (io.DeltaTime <= 0.0f) io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    BuildUI();
    ImGui::Render();
    const float clear[4] = {0, 0, 0, 0};
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_context->ClearRenderTargetView(g_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    return g_tex;
}

// Flat mode (VR off): draw ImGui straight onto the swapchain backbuffer with the REAL mouse, since the
// VR submit path that normally composites the menu isn't running. Mutually exclusive with RenderFrame().
void RenderToBackbuffer(IDXGISwapChain* swapChain) noexcept
{
    if (!g_ready || !g_open || g_context == nullptr || g_device == nullptr || swapChain == nullptr) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || bb == nullptr) return;
    D3D11_TEXTURE2D_DESC bd = {};
    bb->GetDesc(&bd);
    ID3D11RenderTargetView* rtv = nullptr;
    if (SUCCEEDED(g_device->CreateRenderTargetView(bb, nullptr, &rtv)) && rtv != nullptr)
    {
        ImGui_ImplDX11_NewFrame();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(bd.Width), static_cast<float>(bd.Height));
        if (io.DeltaTime <= 0.0f) io.DeltaTime = 1.0f / 60.0f;
        io.MouseDrawCursor = true;
        ClipCursor(nullptr);
        POINT pt = {};
        if (GetCursorPos(&pt) && g_hwnd != nullptr && ScreenToClient(g_hwnd, &pt))
        {
            RECT rc = {};
            GetClientRect(g_hwnd, &rc);
            const float sx = (rc.right  > 0) ? static_cast<float>(bd.Width)  / rc.right  : 1.0f;
            const float sy = (rc.bottom > 0) ? static_cast<float>(bd.Height) / rc.bottom : 1.0f;
            io.AddMousePosEvent(static_cast<float>(pt.x) * sx, static_cast<float>(pt.y) * sy);
        }
        io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
        io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
        ImGui::NewFrame();
        BuildUI();
        ImGui::Render();
        g_context->OMSetRenderTargets(1, &rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        rtv->Release();
    }
    bb->Release();
}

int  LeftStickMagnitude() noexcept { return g_leftStickMag.load(std::memory_order_relaxed); }
bool PadSprintHeld() noexcept { return g_padSprint.load(std::memory_order_relaxed); }
bool PadLeftShoulderHeld() noexcept
{
    // [WHEELFRESH 2026-08-17] Mission loading can stop XInput polling while leaving the last LB
    // sample latched for seconds. Only a live sample may exempt the weapon wheel from menu mono.
    const unsigned long long sampled = g_padSampleMs.load(std::memory_order_acquire);
    return g_padLeftShoulder.load(std::memory_order_relaxed) && sampled != 0 &&
           GetTickCount64() - sampled <= 250ull;
}
int  GetRecenterKey() noexcept { return g_recenterKey; }
int  GetFpToggleKey() noexcept { return g_fpToggleKey; }
bool IsRebindingFpToggle() noexcept { return g_rebindingFpToggle; }
bool GetMoveFollowsHead() noexcept { return g_moveFollowsHead.load(std::memory_order_relaxed); }
void SetMoveFollowsHead(bool on) noexcept { g_moveFollowsHead.store(on, std::memory_order_relaxed); }
bool GetDecoupledPitch() noexcept { return g_decoupledPitch.load(std::memory_order_relaxed); }
void SetDecoupledPitch(bool on) noexcept { g_decoupledPitch.store(on, std::memory_order_relaxed); }
bool GetDecoupledYaw() noexcept { return g_decoupledYaw.load(std::memory_order_relaxed); }
void SetDecoupledYaw(bool on) noexcept { g_decoupledYaw.store(on, std::memory_order_relaxed); }
bool IsOpen() noexcept { return g_open; }
bool IsSoleInputLockOwner() noexcept
{
    return g_open && g_frozen && g_savedMove == 0 && g_savedLook == 0;
}
}  // namespace ME2VR::Menu
