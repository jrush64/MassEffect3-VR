#pragma once

#include <cstdint>

#include <Windows.h>

struct ID3D11Device;
struct IDXGISwapChain;
struct ID3D11Texture2D;

namespace ME2VR::D3DCapture
{
void InstallBreakpointProbes() noexcept;
void TryInstallFactoryHooks(void* factory, const IID& riid) noexcept;

// Render-above-display-resolution spoof.
// ArmResolutionSpoof() MUST run synchronously in DllMain, before any thread; InstallDisplayQueryHooks()
// runs from the worker thread, before the game's D3D init.
void ArmResolutionSpoof() noexcept;
void InstallDisplayQueryHooks() noexcept;

// Per-eye UI duplication master toggle (heuristic can false-positive on full-screen effect passes ->
// translucent half-size ghost; toggle to isolate/disable).
bool GetUiDupEnabled() noexcept;
void SetUiDupEnabled(bool on) noexcept;

// Retained references to the game's D3D11 device + swapchain (captured at first present).
// Null until the swapchain is created. Used by the OpenXR bridge (me2_xr). Not AddRef'd for caller.
ID3D11Device* GetGameDevice() noexcept;
IDXGISwapChain* GetGameSwapChain() noexcept;
unsigned GetBackbufferWidth() noexcept;
unsigned GetBackbufferHeight() noexcept;

// True when a full-screen menu/UI is up (detected by UI-draw volume). In this mode the frame should
// be presented MONO (no stereo split, UI shown full-width to both eyes) - 2D menus aren't stereo.
bool GetMenuMode() noexcept;
void UpdatePresentationState() noexcept; // once per Present: sole owner of MENU_MONO entry/exit
bool GetMenuModeManual() noexcept;    // [AUTOMENU] the manual toggle alone, without the auto modes
// [XSTATE] record one VR-cine (VrCineActive) edge for the per-window transition counter in
// [FRAMETIME]. Caller takes the QueryPerformanceCounter pair around the edge itself.
void NoteCineTransition(const LARGE_INTEGER& t0, const LARGE_INTEGER& t1) noexcept;
void SetAutoGameMode(int mode) noexcept;   int  GetAutoGameMode() noexcept;
bool GetMonoMenus() noexcept;              void SetMonoMenus(bool on) noexcept;
// [MIRRORTHROTTLE] present the flat mirror 1 frame in N (1 = every frame, for recording the window).
int  GetMirrorPresentEvery() noexcept;     void SetMirrorPresentEvery(int n) noexcept;
bool GetManualFlat() noexcept;
void SetManualFlat(bool on) noexcept;

// The UI overlay texture (HUD + menus, transparent bg) to composite as a flat layer over the
// always-stereo world. GetUiOverlayActive() is true on frames that drew UI.
ID3D11Texture2D* GetUiOverlayTexture() noexcept;
bool GetUiOverlayActive() noexcept;

// UI mode: true = head-locked overlay layer (UI doesn't swim with head); false = baked per-eye into
// the world (old behavior). Toggle from the Insert menu.
bool GetUiOverlayMode() noexcept;
void SetUiOverlayMode(bool on) noexcept;

// The captured GFx UI texture (complete UI), to be submitted as a quad layer. Null until seen.
ID3D11Texture2D* GetUiTexture() noexcept;
unsigned GetUiTexWidth() noexcept;
unsigned GetUiTexHeight() noexcept;

// --- DIBR (depth-image-based stereo): the 4th VR mode. Left = real frame, right = depth-warped synth. ---
void SetDepthMapEnabled(bool enabled) noexcept;   // master: run the depth capture + warp (true only in mode 3)
bool GetDepthMapEnabled() noexcept;
bool IsDibrStereoReady() noexcept;                // depthEnabled && depthReady && SRV valid
void SetDibrWarp(float gain, float convergence, bool flip) noexcept;   // push live warp tunables
void GetDepthProbe(float* center, float* tl, float* br, float* tr) noexcept;   // depth taps (auto-converge feeds off center)
ID3D11Texture2D* GetDibrRightEye(ID3D11Texture2D* backBuffer) noexcept;        // synth eye; null -> submit backbuffer
ID3D11Texture2D* GetSfrPass0Texture() noexcept;
uint64_t SfrCaptureAgePresents() noexcept;   // [SFRFRESH] presents since the last left-eye capture                               // [SFR] held pass-0 image = LEFT eye
// [FREEZETAG] monotonic completed-capture count; unchanged since last present => same pixels.
uint64_t GetSfrPass0CaptureSeq() noexcept;
}
