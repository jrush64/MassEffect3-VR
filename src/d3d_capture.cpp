#include "d3d_capture.h"

#include "allocviewstate_finder.h"
#include "calcview_finder.h"
#include "calcview_hook.h"
#include "convo_fp.h"
#include "engine_probe.h"
#include "hud_probe.h"
#include "logger.h"
#include "me2_menu.h"
#include "me2_xr.h"

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <pdh.h>   // [FRAMEOWNER] types only; pdh.dll is loaded dynamically
#include <pdhmsg.h>

#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <string>

#pragma intrinsic(_ReturnAddress)

namespace
{
constexpr UINT kPresentSlot = 8;
constexpr UINT kCreateSwapChainSlot = 10;
constexpr UINT kCreateSwapChainForHwndSlot = 15;
constexpr UINT kCreateSwapChainForCoreWindowSlot = 16;
constexpr UINT kCreateSwapChainForCompositionSlot = 24;
constexpr UINT kCreatePixelShaderSlot = 15;
constexpr UINT kPSSetShaderResourcesSlot = 8;
constexpr UINT kDrawIndexedSlot = 12;
constexpr UINT kDrawSlot = 13;
constexpr UINT kDrawIndexedInstancedSlot = 20;
constexpr UINT kDrawInstancedSlot = 21;
constexpr UINT kOMSetRenderTargetsSlot = 33;
constexpr UINT kDrawAutoSlot = 38;
constexpr UINT kDrawIndexedInstancedIndirectSlot = 39;
constexpr UINT kDrawInstancedIndirectSlot = 40;
constexpr UINT kDispatchSlot = 41;
constexpr UINT kDispatchIndirectSlot = 42;
constexpr UINT kRSSetViewportsSlot = 44;
constexpr UINT kRSSetScissorRectsSlot = 45;
constexpr UINT kCopySubresourceRegionSlot = 46;
constexpr UINT kCopyResourceSlot = 47;
constexpr UINT kResolveSubresourceSlot = 57;
constexpr UINT kExecuteCommandListSlot = 58;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using CreateSwapChainForCoreWindowFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using CreateSwapChainForCompositionFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using PSSetShaderResourcesFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using DrawAutoFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using DrawIndexedInstancedIndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using DrawInstancedIndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using DispatchIndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using RSSetViewportsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using RSSetScissorRectsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_RECT*);
using CopySubresourceRegionFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
using CopyResourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using ResolveSubresourceFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);
using ExecuteCommandListFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);

std::mutex g_hookMutex;
PresentFn g_originalPresent = nullptr;
void** g_presentSlot = nullptr;
CreateSwapChainFn g_originalCreateSwapChain = nullptr;
void** g_createSwapChainSlot = nullptr;
CreateSwapChainForHwndFn g_originalCreateSwapChainForHwnd = nullptr;
void** g_createSwapChainForHwndSlot = nullptr;
CreateSwapChainForCoreWindowFn g_originalCreateSwapChainForCoreWindow = nullptr;
void** g_createSwapChainForCoreWindowSlot = nullptr;
CreateSwapChainForCompositionFn g_originalCreateSwapChainForComposition = nullptr;
void** g_createSwapChainForCompositionSlot = nullptr;
PSSetShaderResourcesFn g_originalPSSetShaderResources = nullptr;
void** g_psSetShaderResourcesSlot = nullptr;
DrawIndexedFn g_originalDrawIndexed = nullptr;
void** g_drawIndexedSlot = nullptr;
DrawFn g_originalDraw = nullptr;
void** g_drawSlot = nullptr;
DrawIndexedInstancedFn g_originalDrawIndexedInstanced = nullptr;
void** g_drawIndexedInstancedSlot = nullptr;
DrawInstancedFn g_originalDrawInstanced = nullptr;
void** g_drawInstancedSlot = nullptr;
OMSetRenderTargetsFn g_originalOMSetRenderTargets = nullptr;
void** g_omSetRenderTargetsSlot = nullptr;
DrawAutoFn g_originalDrawAuto = nullptr;
void** g_drawAutoSlot = nullptr;
DrawIndexedInstancedIndirectFn g_originalDrawIndexedInstancedIndirect = nullptr;
void** g_drawIndexedInstancedIndirectSlot = nullptr;
DrawInstancedIndirectFn g_originalDrawInstancedIndirect = nullptr;
void** g_drawInstancedIndirectSlot = nullptr;
DispatchFn g_originalDispatch = nullptr;
void** g_dispatchSlot = nullptr;
DispatchIndirectFn g_originalDispatchIndirect = nullptr;
void** g_dispatchIndirectSlot = nullptr;
RSSetViewportsFn g_originalRSSetViewports = nullptr;
void** g_rsSetViewportsSlot = nullptr;
RSSetScissorRectsFn g_originalRSSetScissorRects = nullptr;
void** g_rsSetScissorRectsSlot = nullptr;
CopySubresourceRegionFn g_originalCopySubresourceRegion = nullptr;
void** g_copySubresourceRegionSlot = nullptr;
CopyResourceFn g_originalCopyResource = nullptr;
void** g_copyResourceSlot = nullptr;
ResolveSubresourceFn g_originalResolveSubresource = nullptr;
void** g_resolveSubresourceSlot = nullptr;
ExecuteCommandListFn g_originalExecuteCommandList = nullptr;
void** g_executeCommandListSlot = nullptr;

std::atomic_bool g_captured{false};
std::atomic<unsigned long long> g_presentCount{0};
std::atomic<unsigned long long> g_psSetShaderResourcesCount{0};
std::atomic<unsigned long long> g_psSetShaderResourcesLogCount{0};
std::atomic<unsigned long long> g_drawCompositeLogCount{0};
std::atomic<unsigned long long> g_stereoBoundDrawLogCount{0};
std::atomic<unsigned long long> g_stateTraceLogCount{0};
std::atomic<unsigned long long> g_executeCommandListCount{0};
std::atomic<unsigned long long> g_executeCommandListLogCount{0};
std::atomic<unsigned long long> g_copyPathLogCount{0};
std::atomic<unsigned long long> g_omSetRenderTargetsCount{0};
std::atomic<unsigned long long> g_omSetRenderTargetsLogCount{0};
std::atomic<unsigned long long> g_viewportSetCount{0};
std::atomic<unsigned long long> g_viewportLogCount{0};
std::atomic<unsigned long long> g_scissorSetCount{0};
std::atomic<unsigned long long> g_scissorLogCount{0};
std::atomic<unsigned long long> g_stackLogCount{0};
std::atomic<unsigned long long> g_uiSeamStackCount{0};
// MinHook on the real draw FUNCTIONS (catches cached-pointer callers that bypass the vtable).
DrawIndexedFn g_mhRealDrawIndexed = nullptr;
DrawFn g_mhRealDraw = nullptr;
DrawIndexedInstancedFn g_mhRealDrawIndexedInstanced = nullptr;
std::atomic_bool g_mhDrawInstalled{false};
std::atomic<uint64_t> g_ftDraws{0};   // [FRAMETIME] draws seen by the Mh hooks; read per 5s window

// [HOOKCOST] How much of the render thread's 100% is OURS. Every draw and every bind passes through
// these hooks; the Citadel hub runs 7000 draws/present at 3x the per-draw cost of a combat map, and
// the per-draw classifier (IsUiDrawNow) makes D3D state queries on some of them. TSC cycles spent in
// hook bodies (excluding the real D3D call) are accumulated per thread and flushed to two atomics in
// batches, so the probe itself costs ~nothing: draw = the three Mh draw hooks + the vtable draw hooks,
// bind = OMSetRenderTargets / PSSetShaderResources / RSSetViewports / ClearDSV hooks. uiq = how many
// draws paid for the D3D state queries. Converted to ms per present in [FRAMEOWNER] with a TSC
// frequency calibrated against QPC over the same window.
std::atomic<uint64_t> g_hcDrawCyc{0}, g_hcBindCyc{0}, g_hcUiQueries{0};
thread_local uint64_t t_hcDraw = 0, t_hcBind = 0; thread_local unsigned t_hcN = 0;
inline void HookCostFlushMaybe() noexcept
{
    if (++t_hcN < 64) return;
    t_hcN = 0;
    if (t_hcDraw) { g_hcDrawCyc.fetch_add(t_hcDraw, std::memory_order_relaxed); t_hcDraw = 0; }
    if (t_hcBind) { g_hcBindCyc.fetch_add(t_hcBind, std::memory_order_relaxed); t_hcBind = 0; }
}
inline void HookCostDraw(uint64_t cyc) noexcept { t_hcDraw += cyc; HookCostFlushMaybe(); }
inline void HookCostBind(uint64_t cyc) noexcept { t_hcBind += cyc; HookCostFlushMaybe(); }

// The only glass-smear fix: replace ME3's exact distortion composite and transform its raw-FOV
// vector field into the wide headset projection. No camera lifetime or buffer-history mutation.
CreatePixelShaderFn g_mhRealCreatePixelShader = nullptr;
std::atomic_bool g_mhCreatePixelShaderInstalled{false};
constexpr UINT kDistortionCompositeHash = 0x50414EE1u;
std::atomic<ID3D11PixelShader*> g_distortionReplacementPs{nullptr};
ID3D11Buffer* g_distortionScaleCb = nullptr;
std::mutex g_distortionFixMutex;
std::atomic<unsigned long long> g_distortionCorrectedDraws{0};

std::atomic_bool g_uiDupEnabled{true};      // master enable for UI handling
// UI mode: TRUE = redirect UI to a private RT shown as a quad layer. FALSE = per-eye dup baked into
// the world (the OG good behavior - UI moves with head but clean, no menu leftovers). DEFAULT FALSE:
// the overlay left stale menu content on screen during gameplay. Galaxy-map handling = auto mono instead.
std::atomic_bool g_uiOverlayMode{false};
// UI overlay: redirect full-screen UI draws to a private RT, presented as ONE flat layer over the
// (always-stereo) world. No per-eye split of the UI, no world mode switch -> no snap, menus readable.
ID3D11Texture2D* g_uiOverlayTex = nullptr;
ID3D11RenderTargetView* g_uiOverlayRTV = nullptr;
ID3D11ShaderResourceView* g_uiOverlaySRV = nullptr;
std::atomic<unsigned long long> g_overlayClearedFrame{0xFFFFFFFFFFFFFFFFull};
std::atomic_bool g_overlayHasContent{false};
std::atomic_bool g_menuPresentation{false}; // authoritative MENU_MONO state
std::atomic_int g_menuPresentationReason{0};
// EGameModes is retained as cinematic telemetry only. It does not own menu presentation.
std::atomic<int>  g_autoGameMode{-1};
std::atomic_bool  g_monoMenus{true};        // true = menus/map flat mono (default), false = VR menus
std::atomic_bool g_manualFlat{false};       // manual F2 / menu override
float g_menuEma = 0.0f;                      // smoothed UI-draw rate (menu detector, noise-robust)
std::atomic_bool g_traceActive{false};        // ordered single-frame op trace
std::atomic<int> g_traceSeq{0};
std::atomic_bool g_uiPerEye{false};  // wrong layer (R8 mask, not UI) -> disabled while diagnosing
std::atomic<unsigned long long> g_uiPerEyeCount{0};
ID3D11Texture2D* g_uiSrcTexture = nullptr;   // the GFx UI render target (blit source); submitted as a quad
UINT g_uiTexW = 0, g_uiTexH = 0;
DXGI_FORMAT g_uiTexFmt = DXGI_FORMAT_UNKNOWN;
std::atomic_bool g_uiTexLogged{false};
std::uintptr_t g_exeBaseForUi = 0;
constexpr std::uintptr_t kUiCompositeCallerRva = 0x4850A2;   // distinctive caller of the UI blit
D3D11_VIEWPORT g_lastLoggedViewport = {};
bool g_haveLastLoggedViewport = false;

struct TrackedResource
{
    std::uintptr_t ptr;
    UINT width;
    UINT height;
    DXGI_FORMAT format;
};

thread_local D3D11_VIEWPORT g_currentViewport = {};
thread_local bool g_haveCurrentViewport = false;
std::atomic<unsigned> g_pendingStereoCompositeDraws{0};
std::atomic<unsigned> g_traceAfterFullSizeStereoBind{0};
UINT g_pendingStereoSlot = 0;
TrackedResource g_pendingStereoResource = {};
TrackedResource g_boundPsTrackedResources[32] = {};
std::uintptr_t g_backbufferResourcePtr = 0;
UINT g_backbufferWidth = 0;
UINT g_backbufferHeight = 0;
ID3D11Device* g_gameDevice = nullptr;       // retained (AddRef'd) for the OpenXR bridge
IDXGISwapChain* g_gameSwapChain = nullptr;  // retained (AddRef'd)
void** g_immediateVtable = nullptr;         // the immediate context's vtable (for comparison)
using CreateDeferredContextFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);
CreateDeferredContextFn g_originalCreateDeferredContext = nullptr;
void** g_createDeferredContextSlot = nullptr;
std::atomic_bool g_deferredLogged{false};
std::uintptr_t g_lastLoggedRtvResourcePtr = 0;
UINT g_lastLoggedRtvWidth = 0;
UINT g_lastLoggedRtvHeight = 0;
DXGI_FORMAT g_lastLoggedRtvFormat = DXGI_FORMAT_UNKNOWN;
UINT g_lastLoggedRtvCount = 0;
std::uintptr_t g_currentRtvResourcePtr = 0;
UINT g_currentRtvWidth = 0;
UINT g_currentRtvHeight = 0;
DXGI_FORMAT g_currentRtvFormat = DXGI_FORMAT_UNKNOWN;
TrackedResource g_trackedStereoResources[16] = {};
UINT g_trackedStereoResourceCount = 0;

// [CTXRTV 2026-08-21] Per-CONTEXT bound-render-target tracking. The single g_currentRtv* globals
// above are written by whichever thread binds last, and the game records rendering on several
// threads - so at a given draw, the global usually describes some OTHER thread's target. On the
// one machine the interleaving happened to be benign; on two other systems it was not:
// [BBARM] measured the backbuffer prefilter rejecting >1,000,000 consecutive draws (prefilterPass=0)
// during gameplay, which meant the pass-0 capture never armed and stereo rendered FLAT for them.
// A D3D11 context is single-threaded BY CONTRACT, so "what did THIS context bind last" is race-free
// by definition - that is the question every per-draw decision must ask. Fixed array + linear scan:
// a game holds a handful of contexts, and the read path is a few compares.
struct CtxRtvEntry
{
    ID3D11DeviceContext* ctx;
    std::uintptr_t resPtr;
    UINT w, h;
    DXGI_FORMAT fmt;
};
constexpr int kCtxRtvN = 32;
CtxRtvEntry g_ctxRtv[kCtxRtvN] = {};
std::atomic<int> g_ctxRtvCount{0};

void CtxNoteRtvBind(ID3D11DeviceContext* ctx, std::uintptr_t resPtr, UINT w, UINT h, DXGI_FORMAT fmt) noexcept
{
    if (ctx == nullptr) return;
    const int n = g_ctxRtvCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
    {
        if (g_ctxRtv[i].ctx == ctx)
        {
            // Only this context's own thread writes this slot (D3D11 context contract), so plain
            // stores are safe; the fields are read racily by design and validated downstream.
            g_ctxRtv[i].resPtr = resPtr; g_ctxRtv[i].w = w; g_ctxRtv[i].h = h; g_ctxRtv[i].fmt = fmt;
            return;
        }
    }
    int slot = g_ctxRtvCount.fetch_add(1, std::memory_order_acq_rel);
    if (slot >= kCtxRtvN) { g_ctxRtvCount.store(kCtxRtvN, std::memory_order_release); return; }
    g_ctxRtv[slot].resPtr = resPtr; g_ctxRtv[slot].w = w; g_ctxRtv[slot].h = h; g_ctxRtv[slot].fmt = fmt;
    g_ctxRtv[slot].ctx = ctx;   // ctx last: a concurrent reader matching on ctx sees populated fields
}

const CtxRtvEntry* CtxBoundRtv(ID3D11DeviceContext* ctx) noexcept
{
    const int n = g_ctxRtvCount.load(std::memory_order_acquire);
    for (int i = 0; i < n && i < kCtxRtvN; ++i)
        if (g_ctxRtv[i].ctx == ctx) return &g_ctxRtv[i];
    return nullptr;
}

// [RTTARGETS] What the draws render INTO, per window: a histogram keyed by the bound RTV's
// (width, height, format) at draw time. The Citadel hub's 7000 draws are 2.5x the per-draw cost of a
// combat map at the same count, and 40% of them land in a backbuffer-sized RGBA8 target; naming the
// targets (scene colour vs light attenuation vs shadow depth vs GUI) says which engine pass multiplies
// in a crowd. Lock-free: keys claimed by CAS, counts relaxed; a slot miss is dropped (counted).
constexpr int kRtHistN = 16;
std::atomic<uint64_t> g_rtHistKey[kRtHistN] = {};
std::atomic<uint32_t> g_rtHistCount[kRtHistN] = {};
std::atomic<uint32_t> g_rtHistDropped{0};
inline void RtHistNote() noexcept
{
    const uint64_t key = (1ull << 63) |
                         (static_cast<uint64_t>(g_currentRtvWidth & 0xFFFFF) << 40) |
                         (static_cast<uint64_t>(g_currentRtvHeight & 0xFFFFFF) << 16) |
                         static_cast<uint64_t>(static_cast<unsigned>(g_currentRtvFormat) & 0xFFFF);
    for (int i = 0; i < kRtHistN; ++i)
    {
        uint64_t k = g_rtHistKey[i].load(std::memory_order_relaxed);
        if (k == key) { g_rtHistCount[i].fetch_add(1, std::memory_order_relaxed); return; }
        if (k == 0 && g_rtHistKey[i].compare_exchange_strong(k, key, std::memory_order_relaxed))
        { g_rtHistCount[i].fetch_add(1, std::memory_order_relaxed); return; }
        if (k == key) { g_rtHistCount[i].fetch_add(1, std::memory_order_relaxed); return; }
    }
    g_rtHistDropped.fetch_add(1, std::memory_order_relaxed);
}
D3D11_RECT g_lastLoggedScissor = {};
bool g_haveLastLoggedScissor = false;
void* g_vectoredHandler = nullptr;
std::atomic_bool g_breakpointsInstalled{false};
std::atomic<unsigned long long> g_breakpointTotalHits{0};
thread_local std::uintptr_t g_rearmBreakpointAddress = 0;

struct BreakpointProbe
{
    std::uintptr_t rva;
    const char* name;
    BYTE originalByte;
    BYTE* address;
    std::atomic<unsigned long long> hits;
    bool armed;
};

BreakpointProbe g_breakpointProbes[] = {
    {0x34C616, "vp-main-A-ret0", 0, nullptr, 0, false},
    {0x34A28E, "vp-main-A-ret1", 0, nullptr, 0, false},
    {0x34C269, "vp-main-A-ret2", 0, nullptr, 0, false},
    {0x1F862E, "vp-main-B-ret0", 0, nullptr, 0, false},
    {0x3AAFC5, "vp-main-B-ret1", 0, nullptr, 0, false},
    {0x34B296, "vp-main-B-ret2", 0, nullptr, 0, false},
    {0x34C2C2, "vp-main-B-ret3", 0, nullptr, 0, false},
    {0x3546AE, "vp-small-quarter-ret", 0, nullptr, 0, false},
};

// ======================= DIBR (depth-image-based stereo) - globals =======================
// Ported from ME1 d3d_capture.cpp (shipped single-tap guard-gather warp). Pure D3D11:
// capture scene depth at ClearDSV, warp the finished color frame into a synthesized right eye. No engine
// offsets. The whole pipeline is gated on g_depthMapEnabled, set true ONLY in VR mode 3 (DIBR).
constexpr UINT kClearDepthStencilViewSlot = 53;   // ID3D11DeviceContext::ClearDepthStencilView (TODO-2: verify)
using ClearDSVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
ClearDSVFn g_mhRealClearDSV = nullptr;   // MinHook trampoline to the real ClearDepthStencilView
bool g_mhClearDSVInstalled = false;

bool g_mhOMSetRTInstalled = false;   // [RTVDURABLE] inline hook replaces the vtable patch

// [EXITKILL] hard-terminate at ExitProcess entry - see the install site for why.
using ExitProcessFn = void(WINAPI*)(UINT);
ExitProcessFn g_origExitProcess = nullptr;
bool g_exitKillInstalled = false;
// [VRRES] the render size the player asked for (MELE3VR.ini RenderW/RenderH), captured at attach.
// The attach-time write into GamerSettings.ini lands AFTER the engine has already read its config
// (dxgi.dll is loaded late), and the engine writes its in-memory ResX/ResY back on exit - measured
// 2026-08-15: ini said 3072x1728, VRRES logged "wrote", GamerSettings ended every session at
// 5760x3240 and the game rendered 4K x2. So the override is ALSO written at ExitProcess entry, after
// the engine's own flush, which is the value the next launch reads.
UINT g_vrresW = 0, g_vrresH = 0;
bool DispqWriteRes(UINT w, UINT h) noexcept;
void WINAPI ExitProcessHook(UINT uExitCode) noexcept
{
    if (g_vrresW >= 640 && g_vrresH >= 360)
    {
        const bool wrote = DispqWriteRes(g_vrresW, g_vrresH);
        char l[160];
        std::snprintf(l, sizeof(l), "[VRRES] exit: ResX/ResY %ux%u %s", g_vrresW, g_vrresH,
                      wrote ? "written to GamerSettings.ini after the game's own flush" : "already correct / write FAILED");
        ME2VR::Log::Line(l);
    }
    TerminateProcess(GetCurrentProcess(), uExitCode);   // never returns; skips the hanging detach chain
}
ID3D11DeviceContext* g_gameContext = nullptr;      // immediate context for the warp pass (grabbed lazily)

std::atomic<bool> g_depthMapEnabled{false};        // master: capture + warp run only when true (mode 3)
void* g_boundDepthRes = nullptr;                   // resource behind the bound DSV (identity only, never deref'd)
struct DepthDrawEntry { void* res; uint32_t draws; };
DepthDrawEntry g_depthDraws[16] = {};
constexpr uint32_t kSceneDepthDrawThreshold = 30;  // >= this many draws since last clear = a real geometry pass

ID3D11Texture2D* g_depthCopy = nullptr;            // capture scratch (may be a flat pooled pass; content-gated below)
ID3D11Texture2D* g_depthPublished = nullptr;       // last VERIFIED-real depth - the SRV views THIS, never the raw grab
ID3D11ShaderResourceView* g_depthSrv = nullptr;
UINT g_depthCopyW = 0, g_depthCopyH = 0;
std::atomic<bool> g_depthReady{false};
std::atomic<int> g_depthCapLogs{0};
std::atomic<int> g_depthMissLogs{0};
std::atomic<UINT> g_sceneRenderW{0};               // LIVE render res (== backbuffer); the scene-size gate matches THIS
std::atomic<UINT> g_sceneRenderH{0};
ID3D11Texture2D* g_depthStaging[2] = {nullptr, nullptr};   // non-blocking double-buffered probe readback
bool g_depthStagingInFlight[2] = {false, false};
int  g_depthStagingWrite = 0;
ID3D11Texture2D* g_depthGpuSnap[2] = {nullptr, nullptr};   // GPU snapshot ring (promote source, GPU->GPU ~free)
std::atomic<int> g_depthProbeLogs{0};
DXGI_FORMAT g_depthCopyFmt = DXGI_FORMAT_UNKNOWN;
int g_depthDecodeKind = 0;                          // 0=24-bit uint, 1=float32(4B), 2=float32(8B texel)
std::atomic<uint32_t> g_depthCopyCount{0};
std::atomic<float> g_probeCenter{0.0f}, g_probeTL{0.0f}, g_probeBR{0.0f}, g_probeTR{0.0f}, g_probeLC{0.0f}, g_probeRC{0.0f};

// warp shader + pipeline state
ID3D11VertexShader* g_depthVizVs = nullptr;
ID3D11PixelShader* g_depthVizPs = nullptr;
ID3D11PixelShader* g_depthMapPs = nullptr;
ID3D11SamplerState* g_depthVizSampler = nullptr;
ID3D11RasterizerState* g_depthVizRaster = nullptr;
ID3D11DepthStencilState* g_depthVizDepthState = nullptr;
ID3D11BlendState* g_depthVizBlend = nullptr;
ID3D11Buffer* g_dibrParamsCb = nullptr;
ID3D11Buffer* g_depthMapCb = nullptr;
bool g_depthVizReady = false;
bool g_depthVizTried = false;
// warp I/O
ID3D11Texture2D* g_dibrColorCopy = nullptr;
ID3D11ShaderResourceView* g_dibrColorSrv = nullptr;
UINT g_dibrColorW = 0, g_dibrColorH = 0;
ID3D11Texture2D* g_dibrWarpedTex = nullptr;
ID3D11RenderTargetView* g_dibrWarpedRtv = nullptr;
ID3D11ShaderResourceView* g_dibrWarpedSrv = nullptr;
UINT g_dibrWarpedW = 0, g_dibrWarpedH = 0;
// live warp tunables (overwritten each frame by SetDibrWarp; defaults match the guide)
std::atomic<float> g_dibrGain{2.00f};
std::atomic<float> g_dibrConvergence{0.985f};
std::atomic<float> g_dibrSign{1.0f};
std::atomic<float> g_dibrNearCut{0.965f};
std::atomic<float> g_dibrNearScale{85.0f};
std::atomic<float> g_dibrEdgeScale{120.0f};
std::atomic<float> g_dibrCrossScale{140.0f};
std::atomic<float> g_dibrLeakScale{400.0f};
std::atomic<float> g_dibrSilhouetteScale{180.0f};
std::atomic<float> g_dibrSourceScale{1.0f};

// Find (or claim a free slot for) the per-depth-RESOURCE draw counter. Render-thread only, so no lock.
uint32_t* DepthDrawCounter(void* res) noexcept
{
    if (res == nullptr) return nullptr;
    for (auto& e : g_depthDraws) if (e.res == res) return &e.draws;
    for (auto& e : g_depthDraws) if (e.res == nullptr) { e.res = res; e.draws = 0; return &e.draws; }
    return nullptr;
}
// ===================== end DIBR globals =====================

HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) noexcept;
HRESULT STDMETHODCALLTYPE CreateSwapChainHook(IDXGIFactory* factory,
                                              IUnknown* device,
                                              DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** swapChain) noexcept;
HRESULT STDMETHODCALLTYPE CreateSwapChainForHwndHook(IDXGIFactory2* factory,
                                                     IUnknown* device,
                                                     HWND hwnd,
                                                     const DXGI_SWAP_CHAIN_DESC1* desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                                     IDXGIOutput* output,
                                                     IDXGISwapChain1** swapChain) noexcept;
HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindowHook(IDXGIFactory2* factory,
                                                           IUnknown* device,
                                                           IUnknown* window,
                                                           const DXGI_SWAP_CHAIN_DESC1* desc,
                                                           IDXGIOutput* output,
                                                           IDXGISwapChain1** swapChain) noexcept;
HRESULT STDMETHODCALLTYPE CreateSwapChainForCompositionHook(IDXGIFactory2* factory,
                                                           IUnknown* device,
                                                           const DXGI_SWAP_CHAIN_DESC1* desc,
                                                           IDXGIOutput* output,
                                                           IDXGISwapChain1** swapChain) noexcept;
void STDMETHODCALLTYPE PSSetShaderResourcesHook(ID3D11DeviceContext* context,
                                                UINT startSlot,
                                                UINT numViews,
                                                ID3D11ShaderResourceView* const* shaderResourceViews) noexcept;
void STDMETHODCALLTYPE DrawIndexedHook(ID3D11DeviceContext* context,
                                       UINT indexCount,
                                       UINT startIndexLocation,
                                       INT baseVertexLocation) noexcept;
void STDMETHODCALLTYPE DrawHook(ID3D11DeviceContext* context,
                                UINT vertexCount,
                                UINT startVertexLocation) noexcept;
void STDMETHODCALLTYPE DrawIndexedInstancedHook(ID3D11DeviceContext* context,
                                                UINT indexCountPerInstance,
                                                UINT instanceCount,
                                                UINT startIndexLocation,
                                                INT baseVertexLocation,
                                                UINT startInstanceLocation) noexcept;
void STDMETHODCALLTYPE DrawInstancedHook(ID3D11DeviceContext* context,
                                         UINT vertexCountPerInstance,
                                         UINT instanceCount,
                                         UINT startVertexLocation,
                                         UINT startInstanceLocation) noexcept;
void STDMETHODCALLTYPE DrawAutoHook(ID3D11DeviceContext* context) noexcept;
void STDMETHODCALLTYPE DrawIndexedInstancedIndirectHook(ID3D11DeviceContext* context,
                                                        ID3D11Buffer* bufferForArgs,
                                                        UINT alignedByteOffsetForArgs) noexcept;
void STDMETHODCALLTYPE DrawInstancedIndirectHook(ID3D11DeviceContext* context,
                                                 ID3D11Buffer* bufferForArgs,
                                                 UINT alignedByteOffsetForArgs) noexcept;
void STDMETHODCALLTYPE DispatchHook(ID3D11DeviceContext* context,
                                    UINT threadGroupCountX,
                                    UINT threadGroupCountY,
                                    UINT threadGroupCountZ) noexcept;
void STDMETHODCALLTYPE DispatchIndirectHook(ID3D11DeviceContext* context,
                                            ID3D11Buffer* bufferForArgs,
                                            UINT alignedByteOffsetForArgs) noexcept;
void STDMETHODCALLTYPE ExecuteCommandListHook(ID3D11DeviceContext* context,
                                              ID3D11CommandList* commandList,
                                              BOOL restoreContextState) noexcept;
void STDMETHODCALLTYPE CopySubresourceRegionHook(ID3D11DeviceContext* context,
                                                 ID3D11Resource* dstResource,
                                                 UINT dstSubresource,
                                                 UINT dstX,
                                                 UINT dstY,
                                                 UINT dstZ,
                                                 ID3D11Resource* srcResource,
                                                 UINT srcSubresource,
                                                 const D3D11_BOX* srcBox) noexcept;
void STDMETHODCALLTYPE CopyResourceHook(ID3D11DeviceContext* context,
                                        ID3D11Resource* dstResource,
                                        ID3D11Resource* srcResource) noexcept;
void STDMETHODCALLTYPE ResolveSubresourceHook(ID3D11DeviceContext* context,
                                              ID3D11Resource* dstResource,
                                              UINT dstSubresource,
                                              ID3D11Resource* srcResource,
                                              UINT srcSubresource,
                                              DXGI_FORMAT format) noexcept;
void STDMETHODCALLTYPE OMSetRenderTargetsHook(ID3D11DeviceContext* context,
                                              UINT numViews,
                                              ID3D11RenderTargetView* const* renderTargetViews,
                                              ID3D11DepthStencilView* depthStencilView) noexcept;
void STDMETHODCALLTYPE RSSetViewportsHook(ID3D11DeviceContext* context,
                                          UINT numViewports,
                                          const D3D11_VIEWPORT* viewports) noexcept;
void STDMETHODCALLTYPE RSSetScissorRectsHook(ID3D11DeviceContext* context,
                                             UINT numRects,
                                             const D3D11_RECT* rects) noexcept;
const char* FormatName(DXGI_FORMAT format) noexcept;

bool WriteByte(BYTE* address, BYTE value) noexcept
{
    if (address == nullptr) return false;

    DWORD oldProtect = 0;
    if (!VirtualProtect(address, sizeof(BYTE), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        return false;
    }

    *address = value;

    DWORD ignored = 0;
    VirtualProtect(address, sizeof(BYTE), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), address, sizeof(BYTE));
    return true;
}

BreakpointProbe* FindBreakpointProbe(std::uintptr_t address) noexcept
{
    for (BreakpointProbe& probe : g_breakpointProbes)
    {
        if (probe.address != nullptr && reinterpret_cast<std::uintptr_t>(probe.address) == address)
        {
            return &probe;
        }
    }
    return nullptr;
}

LONG WINAPI BreakpointVectoredHandler(EXCEPTION_POINTERS* info) noexcept
{
    if (info == nullptr || info->ExceptionRecord == nullptr || info->ContextRecord == nullptr)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_BREAKPOINT)
    {
        auto* probe = FindBreakpointProbe(reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress));
        if (probe == nullptr) return EXCEPTION_CONTINUE_SEARCH;

        WriteByte(probe->address, probe->originalByte);
        g_rearmBreakpointAddress = reinterpret_cast<std::uintptr_t>(probe->address);

#if defined(_M_X64)
        info->ContextRecord->Rip = g_rearmBreakpointAddress;
#else
        info->ContextRecord->Eip = static_cast<DWORD>(g_rearmBreakpointAddress);
#endif
        info->ContextRecord->EFlags |= 0x100;

        const auto hit = probe->hits.fetch_add(1, std::memory_order_relaxed) + 1;
        const auto total = g_breakpointTotalHits.fetch_add(1, std::memory_order_relaxed) + 1;
        if (total <= 160 || (hit % 300) == 0)
        {
            char buffer[256] = {};
            sprintf_s(buffer,
                      "[ME3DISC] BREAKPOINT %s rva=0x%llX hit=%llu total=%llu thread=%lu",
                      probe->name,
                      static_cast<unsigned long long>(probe->rva),
                      static_cast<unsigned long long>(hit),
                      static_cast<unsigned long long>(total),
                      GetCurrentThreadId());
            ME2VR::Log::Line(buffer);
        }

        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code == EXCEPTION_SINGLE_STEP && g_rearmBreakpointAddress != 0)
    {
        auto* probe = FindBreakpointProbe(g_rearmBreakpointAddress);
        if (probe != nullptr)
        {
            WriteByte(probe->address, 0xCC);
        }
        g_rearmBreakpointAddress = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

template <typename T>
void SafeRelease(T*& ptr) noexcept
{
    if (ptr != nullptr)
    {
        ptr->Release();
        ptr = nullptr;
    }
}

std::string HexPointer(const void* ptr)
{
    char buffer[32] = {};
    sprintf_s(buffer, "0x%p", ptr);
    return buffer;
}

std::string HexHRESULT(HRESULT hr)
{
    char buffer[32] = {};
    sprintf_s(buffer, "0x%08X", static_cast<unsigned int>(hr));
    return buffer;
}

std::string CallerTag(void* address)
{
    if (address == nullptr) return "unknown";

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0 || mbi.AllocationBase == nullptr)
    {
        return HexPointer(address);
    }

    const auto module = static_cast<HMODULE>(mbi.AllocationBase);
    char path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameA(module, path, static_cast<DWORD>(sizeof(path)));
    const char* name = path;
    if (len > 0)
    {
        const char* slash = std::strrchr(path, '\\');
        const char* fwd = std::strrchr(path, '/');
        const char* sep = slash > fwd ? slash : fwd;
        if (sep != nullptr && sep[1] != '\0') name = sep + 1;
    }

    const auto rva = reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module);
    char buffer[256] = {};
    sprintf_s(buffer, "%s+0x%llX", name, static_cast<unsigned long long>(rva));
    return buffer;
}

std::string StackTag()
{
    void* frames[16] = {};
    const USHORT count = CaptureStackBackTrace(0, static_cast<DWORD>((std::min)(static_cast<size_t>(_countof(frames)), size_t{12})), frames, nullptr);
    std::string result;
    for (USHORT i = 0; i < count; ++i)
    {
        if (!result.empty()) result += " <- ";
        result += CallerTag(frames[i]);
    }
    return result;
}

// Ordered single-frame trace: current bound RT (size/fmt/backbuffer) + the PS input texture slot 0.
void FirstFire(const char* method, int id) noexcept
{
    static std::atomic<unsigned> mask{0};
    const unsigned bit = 1u << (id & 31);
    const unsigned prev = mask.fetch_or(bit, std::memory_order_relaxed);
    if ((prev & bit) == 0)
        ME2VR::Log::Line(std::string("[ME3DISC] *** DRAW HOOK FIRST FIRE: ") + method + " ***");
}

void TraceOp(const char* op, ID3D11DeviceContext* ctx, UINT count) noexcept
{
    return;   // disabled: per-op trace served its purpose; removing overhead
    if (!g_traceActive.load(std::memory_order_acquire)) return;
    const int seq = g_traceSeq.fetch_add(1, std::memory_order_relaxed);
    if (seq > 5000) return;
    unsigned inW = 0, inH = 0, inF = 0;
    if (ctx != nullptr && count != 0xFFFFFFFFu)   // draws: peek PS SRV slot 0
    {
        ID3D11ShaderResourceView* srv = nullptr;
        ctx->PSGetShaderResources(0, 1, &srv);
        if (srv) { ID3D11Resource* r = nullptr; srv->GetResource(&r);
            if (r) { ID3D11Texture2D* t = nullptr;
                if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
                    D3D11_TEXTURE2D_DESC d = {}; t->GetDesc(&d); inW = d.Width; inH = d.Height; inF = d.Format; t->Release(); }
                r->Release(); }
            srv->Release(); }
    }
    const bool isBB = (g_currentRtvResourcePtr == g_backbufferResourcePtr && g_backbufferResourcePtr != 0);
    char buf[320] = {};
    sprintf_s(buf, "[ME3DISC] TRACE %03d %-10s rt=%ux%u fmt=%u bb=%d  cnt=%u  psIn=%ux%u/f%u",
              seq, op, g_currentRtvWidth, g_currentRtvHeight, static_cast<unsigned>(g_currentRtvFormat),
              isBB ? 1 : 0, count, inW, inH, inF);
    ME2VR::Log::Line(buf);
}

void TraceCopy(const char* op, const TrackedResource& dst, const TrackedResource& src) noexcept
{
    if (!g_traceActive.load(std::memory_order_acquire)) return;
    const int seq = g_traceSeq.fetch_add(1, std::memory_order_relaxed);
    if (seq > 5000) return;
    const bool dstBB = (dst.ptr == g_backbufferResourcePtr && g_backbufferResourcePtr != 0);
    char b[256] = {};
    sprintf_s(b, "[ME3DISC] TRACE %03d %-12s dst=%ux%u/f%u bb=%d <- src=%ux%u/f%u",
              seq, op, dst.width, dst.height, static_cast<unsigned>(dst.format), dstBB ? 1 : 0,
              src.width, src.height, static_cast<unsigned>(src.format));
    ME2VR::Log::Line(b);
}

// UI-seam discovery: when a draw targets the backbuffer (where the UI/GFx pass lands), capture the
// call stack - the engine's UI render function sits above the D3D wrapper. Capped; log-only.
void LogUiSeamStack(const char* kind, UINT count) noexcept
{
    return;   // disabled: discovery done; removing per-draw overhead/flicker
    // The UI/GFx renders to an LDR target (R8G8B8A8 / B8G8R8A8, optionally sRGB); the world renders
    // to HDR intermediates. Capture LDR-target draws of ALL draw types -> the engine UI render fn
    // sits in their call stacks. Log the RT size+fmt so the mod can tell its own RT from the backbuffer.
    const UINT f = static_cast<UINT>(g_currentRtvFormat);
    const bool ldr = (f == 28 || f == 29 || f == 87 || f == 91);
    if (!ldr || g_currentRtvWidth == 0) return;
    const auto n = g_uiSeamStackCount.fetch_add(1, std::memory_order_relaxed);
    if (n >= 200) return;
    char hdr[176] = {};
    sprintf_s(hdr, "[ME3DISC] UISEAM #%llu %s cnt=%u rt=%ux%u fmt=%u -> ",
              static_cast<unsigned long long>(n), kind, count, g_currentRtvWidth, g_currentRtvHeight, f);
    ME2VR::Log::Line(std::string(hdr) + StackTag());
}

// The whole UI is rendered by GFx to its own texture, then blitted to the backbuffer full-width by a
// single full-screen quad (<=6 verts). That blit samples by screen position, so splitting its viewport
// only samples half. Instead: capture the blit's SOURCE texture (the complete UI) and SKIP the blit;
// me2_xr then submits that texture as a zero-disparity quad layer over the stereo world.
// Gated by the blit's distinctive caller (+0x4850A2) so the world's own passes are untouched.
bool CaptureUiCompositeAndSkip(ID3D11DeviceContext* context, UINT count) noexcept
{
    if (!g_uiPerEye.load(std::memory_order_acquire) || context == nullptr) return false;
    if (count > 6) return false;   // full-screen quad/tri only (cheap pre-filter)

    // Identify the UI blit purely by its caller signature (+0x4850A2) -- no dependence on per-frame
    // render-target/viewport state tracking (which was unreliable under the flip-model swapchain).
    if (g_exeBaseForUi == 0) g_exeBaseForUi = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    void* frames[24] = {};
    const USHORT nf = CaptureStackBackTrace(0, 24, frames, nullptr);
    bool isUi = false;
    for (USHORT i = 0; i < nf; ++i)
        if (reinterpret_cast<std::uintptr_t>(frames[i]) == g_exeBaseForUi + kUiCompositeCallerRva) { isUi = true; break; }
    if (!isUi) return false;

    // Scan the bound PS textures and pick the UI COLOR target (R8G8B8A8 family) - slot 0 is often a
    // single-channel mask (R8), not the color. Only treat this as the UI blit if a color texture exists.
    ID3D11ShaderResourceView* srvs[8] = {};
    context->PSGetShaderResources(0, 8, srvs);
    ID3D11Texture2D* colorTex = nullptr;
    D3D11_TEXTURE2D_DESC colorDesc = {};
    for (UINT s = 0; s < 8; ++s)
    {
        if (srvs[s] == nullptr) continue;
        ID3D11Resource* res = nullptr;
        srvs[s]->GetResource(&res);
        if (res != nullptr)
        {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex)
            {
                D3D11_TEXTURE2D_DESC d = {};
                tex->GetDesc(&d);
                const UINT f = static_cast<UINT>(d.Format);
                const bool isColor = (f == 28 || f == 29 || f == 87 || f == 91);
                if (!g_uiTexLogged.load(std::memory_order_acquire))
                    ME2VR::Log::Line("[ME3DISC] UI blit slot " + std::to_string(s) + " = " +
                                     std::to_string(d.Width) + "x" + std::to_string(d.Height) + " fmt=" + std::to_string(f) +
                                     (isColor ? " [COLOR]" : ""));
                if (isColor && colorTex == nullptr && d.Width >= 256 && d.Height >= 256)
                {
                    colorTex = tex; colorDesc = d;   // keep this ref
                }
                else { tex->Release(); }
            }
            res->Release();
        }
        srvs[s]->Release();
    }
    g_uiTexLogged.store(true, std::memory_order_release);

    if (colorTex == nullptr) return false;   // not the UI color composite -> leave it alone

    if (g_uiSrcTexture != colorTex)
    {
        if (g_uiSrcTexture) g_uiSrcTexture->Release();
        g_uiSrcTexture = colorTex;
    }
    else { colorTex->Release(); }
    g_uiTexW = colorDesc.Width; g_uiTexH = colorDesc.Height; g_uiTexFmt = colorDesc.Format;
    return true;   // skip the full-width blit; the UI goes out as a quad layer instead
}

bool IsInterestingViewport(UINT numViewports, const D3D11_VIEWPORT* viewports) noexcept
{
    if (numViewports == 0 || viewports == nullptr) return false;
    const D3D11_VIEWPORT& vp = viewports[0];
    return (std::fabs(vp.Width - 1863.0f) <= 1.0f && std::fabs(vp.Height - 1048.0f) <= 1.0f) ||
           (std::fabs(vp.Width - 933.0f) <= 1.0f && std::fabs(vp.Height - 526.0f) <= 1.0f);
}

bool IsInterestingScissor(UINT numRects, const D3D11_RECT* rects) noexcept
{
    if (numRects == 0 || rects == nullptr) return false;
    const D3D11_RECT& rect = rects[0];
    const long width = rect.right - rect.left;
    const long height = rect.bottom - rect.top;
    if (rect.left == 0 && rect.top == 0 && width >= 3600 && height >= 2000) return false;
    return (width >= 1300 && height >= 1800) || (width >= 3600 && height <= 1100);
}

void TrackStereoResource(std::uintptr_t resourcePtr, const D3D11_TEXTURE2D_DESC& desc) noexcept
{
    if (resourcePtr == 0) return;
    const bool interesting =
        (desc.Width == 1863 && desc.Height == 1048) ||
        (desc.Width == 933 && desc.Height == 526) ||
        (desc.Width == 931 && desc.Height == 524);
    if (!interesting) return;

    for (UINT i = 0; i < g_trackedStereoResourceCount; ++i)
    {
        if (g_trackedStereoResources[i].ptr == resourcePtr) return;
    }

    if (g_trackedStereoResourceCount >= _countof(g_trackedStereoResources)) return;
    TrackedResource& tracked = g_trackedStereoResources[g_trackedStereoResourceCount++];
    tracked.ptr = resourcePtr;
    tracked.width = desc.Width;
    tracked.height = desc.Height;
    tracked.format = desc.Format;

    char buffer[256] = {};
    sprintf_s(buffer,
              "[ME3DISC] TRACK stereo texture tex=%p %ux%u fmt=%u %s",
              reinterpret_cast<void*>(resourcePtr),
              desc.Width,
              desc.Height,
              static_cast<unsigned int>(desc.Format),
              FormatName(desc.Format));
    ME2VR::Log::Line(buffer);
}

const TrackedResource* FindTrackedStereoResource(std::uintptr_t resourcePtr) noexcept
{
    if (resourcePtr == 0) return nullptr;
    for (UINT i = 0; i < g_trackedStereoResourceCount; ++i)
    {
        if (g_trackedStereoResources[i].ptr == resourcePtr) return &g_trackedStereoResources[i];
    }
    return nullptr;
}

const TrackedResource* FindBoundPsStereoResource(UINT* outSlot) noexcept
{
    for (UINT i = 0; i < _countof(g_boundPsTrackedResources); ++i)
    {
        if (g_boundPsTrackedResources[i].ptr != 0)
        {
            if (outSlot != nullptr) *outSlot = i;
            return &g_boundPsTrackedResources[i];
        }
    }
    return nullptr;
}

void FormatBoundPsSummary(char* buffer, size_t bufferSize) noexcept
{
    if (buffer == nullptr || bufferSize == 0) return;
    buffer[0] = '\0';

    size_t used = 0;
    for (UINT i = 0; i < _countof(g_boundPsTrackedResources); ++i)
    {
        const TrackedResource& bound = g_boundPsTrackedResources[i];
        if (bound.ptr == 0) continue;

        const int written = sprintf_s(buffer + used,
                                      bufferSize - used,
                                      "%ss%u=%p/%ux%u/f%u",
                                      used == 0 ? "" : ",",
                                      i,
                                      reinterpret_cast<void*>(bound.ptr),
                                      bound.width,
                                      bound.height,
                                      static_cast<unsigned int>(bound.format));
        if (written <= 0) break;
        used += static_cast<size_t>(written);
        if (used + 1 >= bufferSize) break;
    }

    if (used == 0)
    {
        sprintf_s(buffer, bufferSize, "none");
    }
}

bool ConsumeStateTraceBudget() noexcept
{
    unsigned expected = g_traceAfterFullSizeStereoBind.load(std::memory_order_acquire);
    while (expected != 0 &&
           !g_traceAfterFullSizeStereoBind.compare_exchange_weak(expected,
                                                                 expected - 1,
                                                                 std::memory_order_acq_rel,
                                                                 std::memory_order_acquire))
    {
    }
    return expected != 0;
}

void LogStateTrace(const char* kind, void* caller, const char* detail) noexcept
{
    if (!ConsumeStateTraceBudget()) return;

    const auto slot = g_stateTraceLogCount.fetch_add(1, std::memory_order_relaxed);
    if (slot >= 512) return;

    const bool currentIsBackbuffer = g_currentRtvResourcePtr == g_backbufferResourcePtr;
    const bool currentIsFullSizeTarget =
        g_backbufferWidth != 0 &&
        g_backbufferHeight != 0 &&
        g_currentRtvWidth == g_backbufferWidth &&
        g_currentRtvHeight == g_backbufferHeight;

    char boundSummary[320] = {};
    FormatBoundPsSummary(boundSummary, sizeof(boundSummary));

    char buffer[1024] = {};
    sprintf_s(buffer,
              "[ME3DISC] TRACE %s caller=%s detail={%s} currentRT=%p %ux%u fmt=%u backbuffer=%d fullSizeRT=%d vp={x=%.1f y=%.1f w=%.1f h=%.1f} boundPS={%s}",
              kind,
              CallerTag(caller).c_str(),
              detail != nullptr ? detail : "",
              reinterpret_cast<void*>(g_currentRtvResourcePtr),
              g_currentRtvWidth,
              g_currentRtvHeight,
              static_cast<unsigned int>(g_currentRtvFormat),
              currentIsBackbuffer ? 1 : 0,
              currentIsFullSizeTarget ? 1 : 0,
              g_haveCurrentViewport ? g_currentViewport.TopLeftX : -1.0f,
              g_haveCurrentViewport ? g_currentViewport.TopLeftY : -1.0f,
              g_haveCurrentViewport ? g_currentViewport.Width : -1.0f,
              g_haveCurrentViewport ? g_currentViewport.Height : -1.0f,
              boundSummary);
    ME2VR::Log::Line(buffer);
}

bool TryDescribeTextureResource(ID3D11Resource* resource, TrackedResource* out) noexcept
{
    if (resource == nullptr || out == nullptr) return false;
    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
    if (FAILED(hr) || texture == nullptr) return false;

    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    SafeRelease(texture);

    out->ptr = reinterpret_cast<std::uintptr_t>(resource);
    out->width = desc.Width;
    out->height = desc.Height;
    out->format = desc.Format;
    return true;
}

bool IsFullSizeTracked(const TrackedResource& resource) noexcept
{
    return g_backbufferWidth != 0 &&
           g_backbufferHeight != 0 &&
           resource.width == g_backbufferWidth &&
           resource.height == g_backbufferHeight;
}

bool IsResourceCopyInteresting(const TrackedResource& dst, const TrackedResource& src) noexcept
{
    return g_pendingStereoCompositeDraws.load(std::memory_order_acquire) != 0 ||
           FindTrackedStereoResource(dst.ptr) != nullptr ||
           FindTrackedStereoResource(src.ptr) != nullptr ||
           dst.ptr == g_backbufferResourcePtr ||
           src.ptr == g_backbufferResourcePtr ||
           IsFullSizeTracked(dst) ||
           IsFullSizeTracked(src);
}

const char* FormatName(DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    default: return "DXGI_FORMAT_OTHER";
    }
}

bool PatchPointerSlot(void** slot, void* hook, void** original, const char* name) noexcept
{
    if (slot == nullptr || hook == nullptr || original == nullptr || *slot == nullptr) return false;
    if (*slot == hook) return true;

    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        ME2VR::Log::WindowsError((std::string("[ME3DISC] VirtualProtect failed for ") + name).c_str(), GetLastError());
        return false;
    }

    *original = *slot;
    *slot = hook;

    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));

    ME2VR::Log::Line(std::string("[ME3DISC] hooked ") + name);
    return true;
}

bool EnsureOverlayRT() noexcept
{
    if (g_uiOverlayTex != nullptr) return true;
    if (g_gameDevice == nullptr || g_backbufferWidth == 0 || g_backbufferHeight == 0) return false;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = g_backbufferWidth; td.Height = g_backbufferHeight; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_gameDevice->CreateTexture2D(&td, nullptr, &g_uiOverlayTex)) || g_uiOverlayTex == nullptr) return false;
    g_gameDevice->CreateRenderTargetView(g_uiOverlayTex, nullptr, &g_uiOverlayRTV);
    g_gameDevice->CreateShaderResourceView(g_uiOverlayTex, nullptr, &g_uiOverlaySRV);
    // Clear to fully transparent immediately so the quad never shows uninitialized garbage before the
    // first per-frame clear (the startup "black bars / weird textures").
    if (g_uiOverlayRTV != nullptr)
    {
        ID3D11DeviceContext* ic = nullptr;
        g_gameDevice->GetImmediateContext(&ic);
        if (ic != nullptr) { const float z[4] = {0, 0, 0, 0}; ic->ClearRenderTargetView(g_uiOverlayRTV, z); ic->Release(); }
    }
    ME2VR::Log::Line("[ME3DISC] UI overlay RT created " + std::to_string(g_backbufferWidth) + "x" + std::to_string(g_backbufferHeight));
    return g_uiOverlayRTV != nullptr;
}

// --- UI-caller histogram (instrument): which exe call-site issues each UI-signature draw? ---------
struct UiCaller { std::uintptr_t rva; unsigned count; };
UiCaller g_uiCallers[48] = {};
std::atomic_flag g_uiCallerLock = ATOMIC_FLAG_INIT;
std::uintptr_t g_exeRangeBase = 0, g_exeRangeSize = 0;

void EnsureExeRange() noexcept
{
    if (g_exeRangeBase != 0) return;
    HMODULE m = GetModuleHandleW(nullptr);
    if (m == nullptr) return;
    g_exeRangeBase = reinterpret_cast<std::uintptr_t>(m);
    __try
    {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_exeRangeBase);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_exeRangeBase + dos->e_lfanew);
        g_exeRangeSize = nt->OptionalHeader.SizeOfImage;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_exeRangeSize = 0x4000000; }
}

void RecordUiCaller() noexcept
{
    EnsureExeRange();
    if (g_exeRangeBase == 0 || g_exeRangeSize == 0) return;
    void* fr[12] = {};
    const USHORT n = CaptureStackBackTrace(1, 12, fr, nullptr);   // skip RecordUiCaller itself
    std::uintptr_t rva = 0;
    for (USHORT i = 0; i < n; ++i)
    {
        const auto addr = reinterpret_cast<std::uintptr_t>(fr[i]);
        if (addr >= g_exeRangeBase && addr < g_exeRangeBase + g_exeRangeSize) { rva = addr - g_exeRangeBase; break; }
    }
    if (rva == 0) return;
    while (g_uiCallerLock.test_and_set(std::memory_order_acquire)) {}
    int freeSlot = -1;
    for (int i = 0; i < 48; ++i)
    {
        if (g_uiCallers[i].count != 0 && g_uiCallers[i].rva == rva) { g_uiCallers[i].count++; g_uiCallerLock.clear(std::memory_order_release); return; }
        if (freeSlot < 0 && g_uiCallers[i].count == 0) freeSlot = i;
    }
    if (freeSlot >= 0) { g_uiCallers[freeSlot].rva = rva; g_uiCallers[freeSlot].count = 1; }
    g_uiCallerLock.clear(std::memory_order_release);
}

void DumpUiCallers(const char* tag) noexcept
{
    UiCaller snap[48];
    while (g_uiCallerLock.test_and_set(std::memory_order_acquire)) {}
    for (int i = 0; i < 48; ++i) { snap[i] = g_uiCallers[i]; g_uiCallers[i].rva = 0; g_uiCallers[i].count = 0; }
    g_uiCallerLock.clear(std::memory_order_release);
    unsigned total = 0; int live = 0;
    for (int i = 0; i < 48; ++i) { total += snap[i].count; if (snap[i].count) ++live; }
    if (total == 0) return;
    // log top 8 by count
    char line[300] = {};
    int len = sprintf_s(line, "[UICALLERS] %s total=%u sites=%d:", tag, total, live);
    for (int top = 0; top < 8; ++top)
    {
        int best = -1; unsigned bestc = 0;
        for (int i = 0; i < 48; ++i) if (snap[i].count > bestc) { bestc = snap[i].count; best = i; }
        if (best < 0) break;
        len += sprintf_s(line + len, sizeof(line) - len, " +0x%llX=%u",
                         static_cast<unsigned long long>(snap[best].rva), snap[best].count);
        snap[best].count = 0;
    }
    ME2VR::Log::Line(line);
}

// A UI draw = full-viewport + alpha-blended (vs the opaque world composite). The caller is
// responsible for establishing that the draw targets the LDR backbuffer. Keeping the pipeline-state
// test separate lets the SFR capture gate validate BOTH facts from the draw's own context instead of
// trusting the cross-thread OMSetRenderTargets census.
bool IsUiPipelineStateNow(ID3D11DeviceContext* ctx, D3D11_VIEWPORT& vpOut,
                          bool recordCaller) noexcept
{
    vpOut = {};
    if (ctx == nullptr || g_backbufferWidth == 0) return false;
    g_hcUiQueries.fetch_add(1, std::memory_order_relaxed);   // [HOOKCOST] a draw paying for D3D state queries
    UINT num = 1;
    ctx->RSGetViewports(&num, &vpOut);
    if (!(num == 1 && vpOut.Width > static_cast<float>(g_backbufferWidth) * 0.75f)) return false;
    ID3D11BlendState* bs = nullptr; float bf[4] = {}; UINT sampleMask = 0;
    ctx->OMGetBlendState(&bs, bf, &sampleMask);
    bool blended = false;
    if (bs != nullptr) { D3D11_BLEND_DESC bd = {}; bs->GetDesc(&bd); blended = bd.RenderTarget[0].BlendEnable != FALSE; bs->Release(); }
    // [UIDEPTH 2026-08-15] A depth-tested draw is a WORLD draw, whatever else it looks like. The three
    // tests above (backbuffer-sized LDR target, near-full-width viewport, blending on) also describe a
    // full-screen blended scene effect - haze, glow, a projected panel - and anything they match gets
    // re-issued at the [UIRATIO] viewport and drawn a SECOND time into the pass-0 capture. Rescaled and
    // double-blended is exactly the "rectangle near the dance floor that is not completely transparent"
    // in the Citadel club.
    // The test is DEPTH TESTING, not depth being bound. First attempt used "a DSV is bound" and that
    // was wrong: the bind counts in the log (18934 backbuffer binds with dsv=NULL vs 301 with depth)
    // describe OMSetRenderTargets EVENTS, not the state in force when Scaleform draws - the engine
    // leaves the scene's DSV bound and simply stops testing against it, so the rule threw the real UI
    // out of EmitUiDraw. With no [UIRATIO] viewport the HUD is drawn full height into the anamorphic
    // buffer and the declared FOV then un-squeezes it: subtitles and the whole HUD stretched
    // vertically (reported immediately). A 2D overlay never COMPARES against scene depth, and a
    // full-screen world effect must, so read the depth-stencil STATE. DepthEnable with func ALWAYS is
    // not a test either, so it stays classified as UI.
    if (blended)
    {
        ID3D11DepthStencilView* dsv = nullptr;
        ctx->OMGetRenderTargets(0, nullptr, &dsv);
        bool depthTested = false;
        if (dsv != nullptr)
        {
            ID3D11DepthStencilState* dss = nullptr; UINT stencilRef = 0;
            ctx->OMGetDepthStencilState(&dss, &stencilRef);
            if (dss != nullptr)
            {
                D3D11_DEPTH_STENCIL_DESC dd = {};
                dss->GetDesc(&dd);
                depthTested = (dd.DepthEnable != FALSE) && (dd.DepthFunc != D3D11_COMPARISON_ALWAYS);
                dss->Release();
            }
            dsv->Release();
        }
        if (depthTested) return false;   // compares against scene depth => world draw, not UI
    }
    // [UICALLERS] These three tests (backbuffer-sized LDR target, near-full-width viewport, blending
    // on) describe Scaleform's UI draws - but they also describe a full-screen blended WORLD effect,
    // and anything they match gets re-issued at the [UIRATIO] viewport and mirrored into the left eye.
    // That is the leading suspect for the semi-transparent rectangle by the Citadel club dance floor
    // (2026-08-15). The histogram was written for exactly this question and then never wired up; the
    // call site RVA separates Scaleform's draws from a world effect. Diagnostics only - a stack walk
    // per UI draw is not something a play session should pay for.
    if (blended && recordCaller && ME2VR::Log::DiagnosticsOn()) RecordUiCaller();
    return blended;
}

// Scaleform draws menus/HUD this way. Fills vpOut with the (full) viewport when true. The bind-time
// globals remain only a cheap classifier hint for ordinary UI duplication; SFR capture does its own
// authoritative render-target query and pipeline-state classification below.
bool IsUiDrawNow(ID3D11DeviceContext* ctx, D3D11_VIEWPORT& vpOut) noexcept
{
    vpOut = {};
    // [CTXRTV] same fix as the capture prefilter: the old cross-thread globals made this classifier
    // answer for the wrong thread's render target on multithreaded renderers - UI treatment applied
    // to world draws and denied to UI draws, machine-dependent. Per-context is race-free by the
    // D3D11 context contract.
    const CtxRtvEntry* bound = CtxBoundRtv(ctx);
    if (bound == nullptr) return false;
    if (!(g_backbufferWidth > 0 && bound->fmt == DXGI_FORMAT_R8G8B8A8_UNORM &&
          bound->w == g_backbufferWidth))
        return false;
    return IsUiPipelineStateNow(ctx, vpOut, true);
}


// ===================== [SFR] same-frame stereo: pass-0 hold + left-eye UI mirror =====================
// SFR renders the frame twice: pass 0 (LEFT, -halfEye) is snapshotted to g_sfrPass0Tex mid-present;
// pass 1 (RIGHT) ends up in the backbuffer. Scaleform draws the HUD ONCE, at end of frame, into the
// backbuffer - i.e. into the RIGHT eye only - so each UI draw is also re-issued into the held pass-0
// texture and the LEFT eye gets the same HUD, fused at screen depth. Ported from ME2 (from ME1's
// SfrMirrorUiDrawIntoPass0). These globals live above the Mh* draw hooks that need them.
ID3D11Texture2D* g_sfrPass0Tex = nullptr;
D3D11_TEXTURE2D_DESC g_sfrPass0Desc = {};
std::atomic<uint64_t> g_sfrPass0Caps{0};
// [SFRCLEAR 2026-08-20] Why the left-eye capture never armed on one machine. The capture
// fires only at a depth clear whose target EXACTLY matches the backbuffer size and is
// single-sampled. Two ordinary game settings break that and neither is written by the installer:
//   - MaxMultisamples > 1 (MSAA): scene depth is multisampled, SampleDesc.Count != 1, rejected.
//   - DynamicResolution != 0: the scene renders at a varying size, never equal to the backbuffer.
// A third possibility is that the size matches but the backbuffer write is never seen before the
// clear, so the capture has no valid moment. These counters separate all three.
std::atomic<uint32_t> g_scClears{0};          // depth clears examined
std::atomic<uint32_t> g_scSceneSized{0};      // passed the size+samples gate
std::atomic<uint32_t> g_scSizeOkMsaaBad{0};   // size matched but multisampled -> MSAA is the cause
std::atomic<uint32_t> g_scBbWrittenAtClear{0};// scene-sized clears that arrived AFTER a backbuffer write
std::atomic<uint32_t> g_scBigW{0}, g_scBigH{0}, g_scBigSamples{0};   // largest depth target seen
uint64_t g_sfrClearsThisPresent = 0;   // scene-sized clears captured this present (render thread)
bool g_sfrCapturedThisPresent = false;   // [SFRCAP] one snapshot per present
// [FLICKER] per-present shimmer counters (render thread only). See FlickerWindow below for the read.
unsigned g_flkUiThisPresent = 0;        // UI draws routed through the SFR EmitUiDraw branch, this present
unsigned g_flkMirrorThisPresent = 0;    // of those, actually mirrored into pass-0 (left eye), this present
unsigned g_flkPresentsSinceCap = 0;     // presents since the last pass-0 capture (0 = captured this present)
// [FLICKER2] the two remaining candidates for the wheel flicker, both measured per-present so one
// launch decides between them instead of another guess:
//  (a) PASS COUNT. SFR must render the scene TWICE every present. [SFR2] showed replays running
//      intermittently while the wheel is up (~0.3/present, not 1). A present that renders only one
//      pass leaves the backbuffer holding the wrong eye's viewpoint, so the right eye jumps between
//      two viewpoints frame to frame. That happens inside the projection layer and would not care
//      what the menu path is doing - which fits "still flickers with mono menus off".
//  (b) UI VIEWPORT SCALE. [UIRATIO] rescales the UI viewport from tan(gameFov)/tan(renderFov) every
//      frame, and it is only frozen during a cine. If the wheel animates the game FOV, the HUD and
//      the wheel graphic get re-scaled every present = the overlay pulsing.
unsigned g_flkPassesThisPresent = 0;    // scene renders seen this present (2 = correct for SFR)
// Completed-present topology, published before the render-thread counter resets in PresentHook.
std::atomic<unsigned> g_lastCompletedScenePasses{0};
float    g_flkUiScaleX = 0.0f;          // last UI viewport X scale actually applied this present
// [CAPJITTER] WHICH scene clear the pass-0 snapshot landed on, this present. Measured 2026-08-16: the
// scene-sized clear fires ~16x per present (14..20), and the capture rule is "first clear after a
// backbuffer write". If that ordinal moves between presents, the snapshot is taken at a different
// stage of the composite each frame - and since SwapEyes=1 feeds the HELD snapshot to the RIGHT eye
// and the live backbuffer to the LEFT, that shows up as exactly the reported symptom: the right eye alone
// shimmering while the left is stable. A steady ordinal kills this theory outright.
unsigned g_flkCapAtClear = 0;
// [SFRCAP2] set by the draw hooks at the first backbuffer-targeting draw of the present (pass 0's
// composite). A scene-sized clear arriving with this set is pass 1 starting - the moment the
// backbuffer provably holds pass 0's finished image. Stream-ordered, thread-agnostic: the
// thread-local replay flag never fired because LE3 records scene commands on another thread.
std::atomic_bool g_sfrBbDrawnThisPresent{false};
// [SFRFRESH] How long since the left eye was actually captured, in presents. A loading screen or a
// prerendered movie has no scene render, so the DrawDetour replay never runs and no capture happens -
// but the SFR submit path still fired, pairing a LIVE backbuffer (right eye) with a STALE capture
// (left eye). That is exactly the reported symptom: right eye shows the loading screen, left eye is
// black or still holding the last gameplay frame, and the 16:9 image is stretched into the narrow
// eye view. Age is the discriminator, and it needs no engine offsets.
std::atomic<uint64_t> g_presentIndex{0};
std::atomic<uint64_t> g_sfrLastCapturePresent{0};
// [BINDRACE 2026-08-16] Excluding UI draws did NOT stop the early capture - capAtClear stayed 1..4 -
// so the arming draw is not a UI draw and classification was never the problem.
// The real defect is one this file has already paid for once. `g_currentRtvResourcePtr` is a plain
// global written by the OMSetRenderTargets hook at BIND time, by whichever thread binds, and it is
// read HERE at DRAW time on a thread that may not be the one that bound it. LE3 records scene commands
// on another thread - already documented in this file, it is why the thread-local replay flag never
// fired. So a bind on one thread can make this test answer "the backbuffer is bound" for a draw issued
// by the other; the flag arms before pass 0 has composited, and the next scene-sized clear snapshots a
// half-built frame. Intermittent by nature - exactly the 1-vs-4 split the probe measured, and exactly
// why it tracks the weapon wheel, which adds work on the other thread.
// THE LESSON ALREADY WRITTEN INTO THIS CODEBASE: a bind-event census does not tell you the pipeline
// state at DRAW time. If a rule depends on state when a draw happens, measure it AT THE DRAW.
// The global is therefore demoted to a cheap pre-filter and the answer confirmed from the context's
// own state. Cost is bounded: the query runs only on draws that pass the pre-filter, and only until
// the flag arms - after that this is one atomic load per draw.
// [BINDRACE2 2026-08-16] The first version still accepted isUiDraw from IsUiDrawNow, whose target
// pre-filter is itself made from the same bind-time globals. That left the exact same race alive in
// the second half of the decision: a real weapon-wheel backbuffer draw could be mislabeled non-UI,
// pass the authoritative target check, and arm the early capture. Once the target is confirmed here,
// classify its viewport/blend/depth state directly from THIS context too. The snapshot gate now uses
// no bind-event-derived answer except as a performance hint.
// [BBARM 2026-08-20] Execution census for the arming chain. One machine (same exe, no MSAA,
// clean 90fps, scene-sized clears flowing) NEVER arms this flag - bbWrittenAtClear=0 across every
// window - so stereo is flat on one setup while identical code arms every present here. Each rejection
// point below gets a counter, printed by [SFRCLEAR] when captures are failing, so the next log says
// WHICH line eats it: prefilterPass=0 -> the stale bind-global fast-reject (this file's own
// [BINDRACE] lesson: bind-event state is not draw-time state - on a machine with different driver
// threading the global may simply never hold the backbuffer when a draw thread reads it);
// confirmed>0 with uiRejected==confirmed -> the composite is blend-classified as UI on that config.
std::atomic<uint32_t> g_bbdCalls{0}, g_bbdPrefilterPass{0}, g_bbdConfirmed{0},
                      g_bbdUiRejected{0}, g_bbdArmedDraw{0}, g_bbdArmedCopy{0},
                      g_bbdNoCtxEntry{0};
// [BBREFRESH] Draws whose bound target is the same size+format as the backbuffer but a DIFFERENT
// resource: if this is non-zero while prefilterPass is 0, the mod is holding a stale backbuffer pointer.
std::atomic<uint32_t> g_bbdSizeMatchOther{0};
std::atomic<std::uintptr_t> g_bbdOtherPtr{0};
void SfrNoteBackbufferDraw(ID3D11DeviceContext* ctx) noexcept
{
    if (ctx == nullptr) return;
    if (g_sfrBbDrawnThisPresent.load(std::memory_order_relaxed)) return;
    if (g_backbufferResourcePtr == 0) return;
    g_bbdCalls.fetch_add(1, std::memory_order_relaxed);
    // [CTXRTV] ask what THIS context bound, not what some other thread bound last. The cross-thread
    // global here is what [BBARM] measured rejecting >1M consecutive draws on two other systems -
    // the whole flat-stereo-in-the-field bug.
    // [BBARM2 2026-08-23] Split the two ways this rejects, because they mean opposite things.
    // bound == nullptr: the context table has never seen an RTV bind from THIS context, so either
    // the draw arrives on a deferred context the mod never recorded, or another injector (overlay,
    // capture tool) wraps the context so the pointer at draw time is not the pointer seen at bind
    // time. resPtr mismatch: the table knows this context and it genuinely has another target
    // bound. One is an identity failure, one is a timing failure - do not conflate them.
    const CtxRtvEntry* bound = CtxBoundRtv(ctx);
    if (bound == nullptr) { g_bbdNoCtxEntry.fetch_add(1, std::memory_order_relaxed); return; }
    if (bound->resPtr != g_backbufferResourcePtr)
    {
        if (bound->w == g_backbufferWidth && bound->h == g_backbufferHeight)
        {
            g_bbdSizeMatchOther.fetch_add(1, std::memory_order_relaxed);
            g_bbdOtherPtr.store(bound->resPtr, std::memory_order_relaxed);
        }
        return;
    }
    g_bbdPrefilterPass.fetch_add(1, std::memory_order_relaxed);
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);                        // authoritative, THIS context
    if (rtv == nullptr) return;
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    rtv->Release();
    if (res == nullptr) return;
    const bool reallyBackbuffer = (reinterpret_cast<std::uintptr_t>(res) == g_backbufferResourcePtr);
    res->Release();
    if (!reallyBackbuffer) return;
    g_bbdConfirmed.fetch_add(1, std::memory_order_relaxed);

    D3D11_VIEWPORT vp = {};
    if (IsUiPipelineStateNow(ctx, vp, false))                         // authoritative, THIS context
    {
        g_bbdUiRejected.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_bbdArmedDraw.fetch_add(1, std::memory_order_relaxed);
    g_sfrBbDrawnThisPresent.store(true, std::memory_order_relaxed);
}
// [CINECAP] copy/resolve counterpart of SfrNoteBackbufferDraw. In-engine cutscenes composite the
// scene to the backbuffer through a COPY, not a draw (measured on the Tuchanka reaper run: the
// bb-draw flag stayed false while clears and replays kept running, so captures fell to ~0.5/s -
// the first bb DRAW of a cine present is the subtitles at end-of-frame, after every clear). The
// pass boundary is the backbuffer WRITE, whatever API performs it.
void SfrNoteBackbufferCopyDst(ID3D11Resource* dst) noexcept
{
    if (!g_sfrBbDrawnThisPresent.load(std::memory_order_relaxed) &&
        g_backbufferResourcePtr != 0 &&
        reinterpret_cast<std::uintptr_t>(dst) == g_backbufferResourcePtr)
    {
        g_bbdArmedCopy.fetch_add(1, std::memory_order_relaxed);   // [BBARM]
        g_sfrBbDrawnThisPresent.store(true, std::memory_order_relaxed);
    }
}
// [CINEMAP] PROBE ONLY - records what a VR-cine present actually does and changes NOTHING. Seven
// cine architectures failed because each ASSUMED a render structure; ME2 solved the identical
// both-eyes-one-viewpoint bug by MAPPING the clear layout ([CLEARMAP]) and reading the boundary out
// of the data. This is that tool for LE3. Written only while a VR cine is active.
constexpr int kCineMapMax = 24;
struct CineClearRec
{
    std::uintptr_t depth;   // depth resource identity (which target this clear hit)
    std::uintptr_t rtv;     // colour target bound at the time
    unsigned w, h;
    bool deferred;          // clear arrived on a DEFERRED context
    bool bbWritten;         // a backbuffer write had already been seen this present
    bool replay;            // CalcViewHook::InReplayPass() as seen from THIS thread
};
CineClearRec g_cineMap[kCineMapMax] = {};
int g_cineMapLen = 0;
int g_cineCapturedAtIndex = -1;        // which accepted clear the CURRENT shipped rule captured at
unsigned long g_cineClearThreadId = 0; // thread the clears arrive on (vs DrawDetour's, logged there)
ID3D11RenderTargetView* g_sfrPass0Rtv = nullptr;   // lazy RTV onto pass 0 for the left-eye mirror

// [GUISCENE] The squad/outfit/character menus render their 3D models through the normal P1 world
// view, then Scaleform samples that scene as an IMAGE into the full-screen GUI. The scene is
// rendered near-square for the headset (the fill), the GUI treats it as 16:9, and the models come
// out vertically stretched while the 2D UI stays correct - the exact reported symptom. Engine-side
// these screens are indistinguishable from datapads (both gm 9, gameplay FOV, gameplay camera,
// unpaused - measured 2026-08-13, VIEWCEN + CINEVR), so the discriminator is the GUI's own
// behaviour: a UI-classified draw whose pixel shader samples a scene-sized RENDER TARGET texture.
// Datapads/HUD sample only small atlases/fonts, so they never trip it. While tripped, GetMenuMode
// routes the frame to the flat mono panel with a raw 16:9 render (same presentation as the gm-7
// menus and ME2's shipped squad screens) and the GUI's scene image is correct by construction.
// This is the auto-detect the F2 manual override existed for.
std::atomic_bool g_guiSceneSeenThisPresent{false};
int g_guiSceneStreak = 0;                       // present-hook thread only
uint64_t g_guiSceneLastReleasePresent = 0;
std::atomic<uint64_t> g_guiSceneLastActivePresent{0};
std::atomic_bool g_guiSceneActive{false};

std::atomic<std::uintptr_t> g_guiSceneLastCallerRva{0};

void GuiSceneProbe(ID3D11DeviceContext* ctx) noexcept
{
    if (g_guiSceneSeenThisPresent.load(std::memory_order_relaxed)) return;   // one sighting is enough
    ID3D11ShaderResourceView* srvs[4] = {};
    ctx->PSGetShaderResources(0, 4, srvs);
    bool hit = false;
    unsigned hitW = 0, hitH = 0;
    for (int i = 0; i < 4; ++i)
    {
        if (srvs[i] == nullptr) continue;
        if (!hit)
        {
            ID3D11Resource* res = nullptr;
            srvs[i]->GetResource(&res);
            if (res != nullptr)
            {
                if (reinterpret_cast<std::uintptr_t>(res) != g_backbufferResourcePtr &&
                    res != g_sfrPass0Tex && res != g_uiOverlayTex)
                {
                    ID3D11Texture2D* t2 = nullptr;
                    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t2))) && t2 != nullptr)
                    {
                        D3D11_TEXTURE2D_DESC d = {};
                        t2->GetDesc(&d);
                        // RENDER_TARGET + scene-scale: GUI atlases/fonts/vignettes are plain
                        // textures or small; only an engine-rendered scene image is both.
                        if ((d.BindFlags & D3D11_BIND_RENDER_TARGET) != 0 &&
                            d.Width >= g_backbufferWidth / 4 && d.Height >= g_backbufferHeight / 4)
                        { hit = true; hitW = d.Width; hitH = d.Height; }
                        t2->Release();
                    }
                }
                res->Release();
            }
        }
        srvs[i]->Release();
    }
    if (hit)
    {
        // [GUISCENE CALLER] Only the GAME's own draws count. The Steam overlay (and any injected
        // code) issues blended full-viewport backbuffer draws on the game's context sampling its
        // own backbuffer-sized RT - indistinguishable from a menu by state alone, and the prime
        // suspect for the phantom gameplay sightings that stuck the latch (09:26 session). A menu
        // draw's stack always passes through MassEffect3.exe; the overlay's never does. The RVA of
        // the game frame is recorded and logged so any future false trigger names its call site.
        EnsureExeRange();
        if (g_exeRangeBase == 0 || g_exeRangeSize == 0) return;
        void* fr[16] = {};
        const USHORT n = CaptureStackBackTrace(1, 16, fr, nullptr);
        std::uintptr_t rva = 0;
        for (USHORT i = 0; i < n; ++i)
        {
            const auto a = reinterpret_cast<std::uintptr_t>(fr[i]);
            if (a >= g_exeRangeBase && a < g_exeRangeBase + g_exeRangeSize) { rva = a - g_exeRangeBase; break; }
        }
        if (rva == 0) return;   // not the game drawing -> not a menu
        g_guiSceneLastCallerRva.store(rva, std::memory_order_relaxed);
        g_guiSceneSeenThisPresent.store(true, std::memory_order_relaxed);
        static std::atomic<int> s_logCount{0};
        if (s_logCount.fetch_add(1, std::memory_order_relaxed) < 6)
        {
            char b[128] = {};
            sprintf_s(b, "[GUISCENE] game UI draw samples scene-sized RT %ux%u caller=+0x%llX",
                      hitW, hitH, static_cast<unsigned long long>(rva));
            ME2VR::Log::Line(b);
        }
    }
}

constexpr int kSfrModeValue = 4;     // VrMode::Sfr
inline bool SfrModeActive() noexcept { return ME2VR::CalcViewHook::GetVrMode() == kSfrModeValue; }

ID3D11RenderTargetView* SfrPass0Rtv() noexcept
{
    if (g_sfrPass0Tex == nullptr || g_gameDevice == nullptr) return nullptr;
    if (g_sfrPass0Rtv == nullptr)
        g_gameDevice->CreateRenderTargetView(g_sfrPass0Tex, nullptr, &g_sfrPass0Rtv);
    return g_sfrPass0Rtv;
}

// Armed once pass-0 has been snapshotted this present (so the mirror composites onto the left-eye
// world image, not a stale/empty texture). UI draws come at end-of-frame, after both passes clear.
inline bool SfrUiMirrorArmed() noexcept
{
    return SfrModeActive() && g_sfrPass0Tex != nullptr && g_sfrClearsThisPresent >= 1;
}

// Only STRAIGHT/PREMULTIPLIED alpha (dst=INV_SRC_ALPHA) is mirrored: fullscreen FX quads (darken=
// multiply, additive) share the "full-viewport backbuffer" signature but must NOT land in the left
// eye with UI state (ME1's "colored overlay on one eye" family). The backbuffer/right eye keeps them
// (the game's own untouched draw); only the left-eye mirror is gated.
bool SfrUiMirrorClassify(ID3D11DeviceContext* ctx) noexcept
{
    ID3D11BlendState* bs = nullptr; float bf[4] = {}; UINT sm = 0;
    ctx->OMGetBlendState(&bs, bf, &sm);
    bool ok = false;
    if (bs != nullptr)
    {
        D3D11_BLEND_DESC bd = {}; bs->GetDesc(&bd);
        ok = bd.RenderTarget[0].BlendEnable != FALSE &&
             bd.RenderTarget[0].DestBlend == D3D11_BLEND_INV_SRC_ALPHA;
        bs->Release();
    }
    return ok;
}

// Re-issue a UI draw into the held pass-0 (left-eye) texture: same viewport/blend/shader state, only
// the color RTV changes. Keep the game's OWN depth-stencil bound (Scaleform clips the HUD with stencil
// masks; a null DSV draws masked elements whole = the ME1 red-radar bug). Render thread only.
template <typename DrawThunk>
void SfrMirrorUiDrawIntoPass0(ID3D11DeviceContext* ctx, DrawThunk&& draw) noexcept
{
    if (!SfrUiMirrorArmed()) return;
    ID3D11RenderTargetView* rtv = SfrPass0Rtv();
    if (rtv == nullptr) return;
    if (!SfrUiMirrorClassify(ctx)) return;
    ID3D11RenderTargetView* savedRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* savedDsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRtvs, &savedDsv);
    ctx->OMSetRenderTargets(1, &rtv, savedDsv);
    draw();
    ++g_flkMirrorThisPresent;   // [FLICKER] this UI draw also reached the left eye
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRtvs, savedDsv);
    for (auto*& r : savedRtvs) if (r != nullptr) { r->Release(); r = nullptr; }
    if (savedDsv != nullptr) savedDsv->Release();
}

// [UIRATIO] Automatic projection match for the HUD. The game computes HUD screen positions - including
// the CROSSHAIR, glued to the aim point - using ITS OWN FOV, while the mod renders the world with the wider
// fill FOV. Every game-projected UI element therefore sits scaled off its world point by
// tan(gameFov)/tan(renderFov) per axis, which is why shots land off the reticle by a constant offset.
// Scaling the UI viewport by exactly that ratio about the EXACT centre puts each element back on its
// world point at any zoom, and the 16:9-into-square vertical stretch dies with it (same mismatch).
// User scale/offset are trims ON TOP. Returns true + fills `s` when a custom viewport is needed.
bool ComputeUiRatioViewport(const D3D11_VIEWPORT& vp, D3D11_VIEWPORT& s) noexcept
{
    if (g_originalRSSetViewports == nullptr) return false;
    float autoX = 1.0f, autoY = 1.0f;
    const float gh = ME2VR::CalcViewHook::GetGameRawFovH();     // what the GAME projected the HUD with
    const float gv = ME2VR::CalcViewHook::GetGameRawFovV();
    const float rh = ME2VR::CalcViewHook::GetRenderHalfFovH();  // what the mod actually RENDERED with
    const float rv = ME2VR::CalcViewHook::GetRenderHalfFovV();
    if (gh > 0.01f && gv > 0.01f && rh > 0.01f && rv > 0.01f)
    {
        autoX = tanf(gh) / tanf(rh);
        autoY = tanf(gv) / tanf(rv);
        if (!(autoX > 0.25f && autoX < 1.5f)) autoX = 1.0f;   // sanity (also catches NaN)
        if (!(autoY > 0.25f && autoY < 1.5f)) autoY = 1.0f;
    }
    // [CINEUI] In-VR conversations/cutscenes render at the headset window while gh tracks the
    // DIRECTOR'S per-shot FOV, so the live ratio rescales the whole UI viewport on every camera
    // cut -- subtitles/wheel stretch and squash with the shot (ME2's exact bug, same cure). Hold
    // the last WIDE-gameplay sample instead (>= 0.55 rejects cine frames, the narrowing transition
    // and ADS alike), so the UI keeps its normal-play size through every cut.
    static float s_gameplayAutoX = 1.0f, s_gameplayAutoY = 1.0f;
    static bool  s_haveGameplayRatio = false;
    static bool  s_gameplayRatioDerived = false;
    static bool  s_wasCineUi = false;
    const bool cineUi = ME2VR::CalcViewHook::GetVrCineActive();
    // [CINEUI2 2026-08-17] "wide and not currently VR-cine" was not proof of gameplay. The
    // Citadel transit session entered a conversation with a cached 1.000/1.000 ratio because the
    // preceding galaxy/menu/photo render had overwritten the real gameplay value (0.673/0.336).
    // Freeze only a sample owned by the engine's gameplay family, a gameplay camera, and neither
    // flat/menu nor cinematic presentation. This cache now describes its name instead of whichever
    // wide-FOV screen happened to draw most recently.
    const int gm = ME2VR::EngineProbe::ReadGameMode();
    const bool genuineGameplaySample = !cineUi && !ME2VR::D3DCapture::GetMenuMode() &&
        !ME2VR::CalcViewHook::GetCinematic() && ME2VR::EngineProbe::IsGameplayCamera() &&
        gm >= 0 && gm <= 4;
    if (genuineGameplaySample && gh >= 0.55f && autoX > 0.25f && autoY > 0.25f)
    {
        s_gameplayAutoX = autoX; s_gameplayAutoY = autoY;
        s_haveGameplayRatio = true;
        s_gameplayRatioDerived = false;
    }
    // [CINEUI3 2026-08-17] A save can begin directly inside a conversation, with no gameplay HUD
    // draw before the wheel appears. Derive the missing cache from the gameplay rest projection and
    // the headset fill target; this is the same ratio a normal gameplay draw would have measured.
    // [CINEUI4 2026-08-18] This used GetGameHalfFov*, which is the CURRENT submitted cine window and
    // can still hold a narrow pre-promotion value. Priority: Earth cold-start produced 1.499/1.479
    // instead of the measured gameplay 0.673/0.336, stretching dialogue vertically. Use the actual
    // invariant fill target. Since this correction maps a narrow game projection into a wider render,
    // ratios above 1 are physically invalid and must never be cached.
    if (cineUi && !s_haveGameplayRatio)
    {
        const float restH = ME2VR::CalcViewHook::GetRestHalfFovH();
        const float restV = ME2VR::CalcViewHook::GetRestHalfFovV();
        const float fillH = ME2VR::CalcViewHook::GetFovFillTargetH();
        const float fillV = ME2VR::CalcViewHook::GetFovFillTargetV();
        const float derivedX = (restH > 0.01f && fillH > 0.01f) ? tanf(restH) / tanf(fillH) : 0.0f;
        const float derivedY = (restV > 0.01f && fillV > 0.01f) ? tanf(restV) / tanf(fillV) : 0.0f;
        if (derivedX > 0.25f && derivedX <= 1.0f &&
            derivedY > 0.25f && derivedY <= 1.0f)
        {
            s_gameplayAutoX = derivedX; s_gameplayAutoY = derivedY;
            s_haveGameplayRatio = true;
            s_gameplayRatioDerived = true;
        }
    }

    if (cineUi && s_haveGameplayRatio) { autoX = s_gameplayAutoX; autoY = s_gameplayAutoY; }
    if (cineUi != s_wasCineUi)
    {
        char b[160];
        std::snprintf(b, sizeof(b), "[CINEUI] UI ratio %s: x=%.3f y=%.3f (source %s)",
                      cineUi ? "FROZEN for cine" : "released to live",
                      autoX, autoY, !s_haveGameplayRatio ? "MISSING - using live" :
                      (s_gameplayRatioDerived ? "projection-derived" : "gameplay sample"));
        ME2VR::Log::Line(b);
        s_wasCineUi = cineUi;
    }
    g_flkUiScaleX = autoX;   // [FLICKER2] record what the UI ratio resolved to this draw
    const float sx = autoX * ME2VR::CalcViewHook::GetSfrUiScaleX();
    const float sy = autoY * ME2VR::CalcViewHook::GetSfrUiScaleY();
    const float ox = ME2VR::CalcViewHook::GetSfrUiOffX();
    const float oy = ME2VR::CalcViewHook::GetSfrUiOffY();
    if (!(sx < 0.999f || sx > 1.001f || sy < 0.999f || sy > 1.001f || ox != 0.0f || oy != 0.0f)) return false;
    s = vp;
    s.Width  = vp.Width  * sx; s.Height = vp.Height * sy;
    s.TopLeftX = vp.TopLeftX + (vp.Width  - s.Width)  * 0.5f + ox * vp.Width;
    s.TopLeftY = vp.TopLeftY + (vp.Height - s.Height) * 0.5f + oy * vp.Height;
    return true;
}

// Per-eye duplicate of a UI draw, preserving aspect (halve W and H, center vertically) so it fuses at
// screen depth without the 2:1 vertical stretch. Caller passes a thunk that re-issues the real draw.
template <typename DrawThunk>
void DupUiDraw(ID3D11DeviceContext* ctx, const D3D11_VIEWPORT& vp, DrawThunk&& draw) noexcept
{
    D3D11_VIEWPORT h = vp;
    h.Width = vp.Width * 0.5f;
    h.Height = vp.Height * 0.5f;
    h.TopLeftY = vp.TopLeftY + vp.Height * 0.25f;
    h.TopLeftX = vp.TopLeftX;                                  // left eye
    g_originalRSSetViewports(ctx, 1, &h);
    draw();
    h.TopLeftX = vp.TopLeftX + vp.Width * 0.5f;                // right eye
    g_originalRSSetViewports(ctx, 1, &h);
    draw();
    g_originalRSSetViewports(ctx, 1, &vp);                     // restore full
}

// AER/DIBR render one full-screen source frame, unlike SBS. Draw the UI once into that frame while
// applying the same raw-FOV -> rendered-FOV correction used by SFR. Duplicating into SBS halves here
// shrinks the HUD twice and leaves every element displaced in the submitted eye.
template <typename DrawThunk>
void DrawUiRatioOnce(ID3D11DeviceContext* ctx, const D3D11_VIEWPORT& vp, DrawThunk&& draw) noexcept
{
    D3D11_VIEWPORT s = {};
    const bool custom = ComputeUiRatioViewport(vp, s);
    if (custom) g_originalRSSetViewports(ctx, 1, &s);
    draw();
    if (custom) g_originalRSSetViewports(ctx, 1, &vp);
}

// Emit a UI draw. OVERLAY mode: redirect it to a private full-screen RT (kept out of the head-tracked
// world layer), presented as a head-locked quad -> UI stays put, no swim. DUP mode: bake per-eye into
// the world (fallback; UI swims with head). The overlay RT is cleared once per frame (first UI draw).
template <typename DrawThunk>
void EmitUiDraw(ID3D11DeviceContext* ctx, const D3D11_VIEWPORT& vp, DrawThunk&& draw) noexcept
{
    // Flat/mono mode (incl. VR off): the world is one full view, so draw the UI once full-screen (NO
    // per-eye dup) - it rides the single flat panel. Dup'ing would double it.
    // MUST be the COMPOSITE menu state, not just the manual F2 flag: during a [GUINAME] auto-mono
    // menu this gate read only g_menuMode, fell through to the SFR branch, and [UIRATIO] rescaled
    // the GUI with a STALE fill FOV (the menu's raw render never runs ApplyFov, so "what the mod
    // rendered with" stays frozen at gameplay's near-square values) - the 2D GUI drew into a
    // ~34%-height viewport while the 3D scene was correct: the "menu squashed, model fine" report.
    if (!ME2VR::CalcViewHook::GetVrEnabled() || ME2VR::D3DCapture::GetMenuMode() || ME2VR::CalcViewHook::GetCinematic()) { draw(); return; }
    // [LOADTOPO 2026-08-17] A loading/photo-result source may report ANY game mode (the Citadel
    // transit load was gm 0), but it stops producing SFR scene captures. Leave its 16:9 UI draw
    // untouched so the flat-quad submit below presents the native image rather than a HUD-scaled
    // loading screen. Do not apply this to a VR cine: a brief capture hitch must never flatten or
    // resize an active conversation/cutscene.
    if (SfrModeActive() && !ME2VR::CalcViewHook::GetVrCineActive() &&
        ME2VR::D3DCapture::SfrCaptureAgePresents() > 3)
    {
        draw();
        return;
    }

    // [SFR] the frame is rendered twice and each pass is a full-screen image, so the UI is drawn ONCE
    // at full size into the backbuffer (= pass 1 / right eye) and mirrored into the held pass-0
    // texture (= left eye). No half-viewport dup: each eye is a whole frame, not half of one.
    if (SfrModeActive())
    {
        // [UIALPHA 2026-08-17] IsUiDrawNow's broad "full viewport + blending" prefilter also
        // catches the club's fullscreen/world FX. The mirror already rejected those non-alpha
        // blend modes, but the mod still applied the HUD's [UIRATIO] viewport to the ORIGINAL draw first.
        // Today's Citadel dance-floor trace proves the shape exactly: with no GUI active there are
        // four classified draws per present, all four fail the mirror (ui/p=4, mir/p=0), and the
        // 0.6728 viewport turns their translucent fullscreen surface into the visible faded
        // rectangle. A draw the mod will not mirror as straight/premultiplied alpha is not HUD and must
        // not receive any other HUD treatment either. Issue it once with the game's untouched state.
        // Keep this gate here rather than changing IsUiPipelineStateNow: that preserves yesterday's
        // field-validated SFR capture boundary and all menu/GUI-scene ownership decisions.
        if (!SfrUiMirrorClassify(ctx))
        {
            draw();
            return;
        }
        ++g_flkUiThisPresent;   // [FLICKER] a UI draw reached the right eye this present
        D3D11_VIEWPORT s = {};
        const bool custom = ComputeUiRatioViewport(vp, s);   // [UIRATIO] crosshair back onto the bullets
        if (custom) g_originalRSSetViewports(ctx, 1, &s);
        draw();                                 // -> backbuffer (right eye)
        SfrMirrorUiDrawIntoPass0(ctx, draw);    // -> pass 0 (left eye); no-op until armed / non-alpha
        if (custom) g_originalRSSetViewports(ctx, 1, &vp);
        return;
    }
    if (g_uiOverlayMode.load(std::memory_order_acquire) && EnsureOverlayRT())
    {
        const unsigned long long fr = g_presentCount.load(std::memory_order_relaxed);
        if (g_overlayClearedFrame.exchange(fr, std::memory_order_acq_rel) != fr)
        {
            const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            ctx->ClearRenderTargetView(g_uiOverlayRTV, zero);
            g_overlayHasContent.store(false, std::memory_order_release);
        }
        ID3D11RenderTargetView* prevRtv = nullptr; ID3D11DepthStencilView* prevDsv = nullptr;
        ctx->OMGetRenderTargets(1, &prevRtv, &prevDsv);
        ctx->OMSetRenderTargets(1, &g_uiOverlayRTV, nullptr);   // full viewport into the full-size RT
        draw();
        ctx->OMSetRenderTargets(1, &prevRtv, prevDsv);
        if (prevRtv) prevRtv->Release();
        if (prevDsv) prevDsv->Release();
        g_overlayHasContent.store(true, std::memory_order_release);
        return;
    }

    const int uiMode = ME2VR::CalcViewHook::GetVrMode();
    if (uiMode == 0 || uiMode == 2 || uiMode == 3)   // Mono / AER / DIBR: one full-screen source
    {
        // All three modes widen the rendered world projection but keep the game's raw HUD projection,
        // so they need one centered raw->render FOV correction. ME3's broad UI prefilter also sees
        // fullscreen blended world effects; retain the field-validated alpha gate before transforming.
        if (!SfrUiMirrorClassify(ctx)) { draw(); return; }
        DrawUiRatioOnce(ctx, vp, static_cast<DrawThunk&&>(draw));
        return;
    }
    if (uiMode == 1 && g_originalRSSetViewports != nullptr)   // SBS Stereo only
    {
        DupUiDraw(ctx, vp, static_cast<DrawThunk&&>(draw));
        return;
    }
    draw();   // Unknown future mode: preserve the game's full-screen UI

}

bool EnsureDistortionScaleBuffer(ID3D11DeviceContext* context) noexcept
{
    if (g_distortionScaleCb != nullptr) return true;
    if (context == nullptr) return false;

    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    if (device == nullptr) return false;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = 16;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    const HRESULT hr = device->CreateBuffer(&desc, nullptr, &g_distortionScaleCb);
    device->Release();
    if (FAILED(hr) || g_distortionScaleCb == nullptr)
    {
        ME2VR::Log::Line("[DISTFIX] focal-scale constant buffer creation failed hr=" + HexHRESULT(hr));
        return false;
    }
    return true;
}

template <typename DrawThunk>
bool EmitReprojectedDistortionDraw(ID3D11DeviceContext* context, DrawThunk&& draw) noexcept
{
    if (context == nullptr) return false;
    ID3D11PixelShader* ps = nullptr;
    context->PSGetShader(&ps, nullptr, nullptr);
    if (ps == nullptr) return false;
    const bool exactComposite =
        ps == g_distortionReplacementPs.load(std::memory_order_acquire);
    ps->Release();
    if (!exactComposite) return false;

    std::lock_guard<std::mutex> guard(g_distortionFixMutex);
    if (!EnsureDistortionScaleBuffer(context)) return false;

    const float rawH = ME2VR::CalcViewHook::GetGameRawFovH();
    const float rawV = ME2VR::CalcViewHook::GetGameRawFovV();
    const float renderH = ME2VR::CalcViewHook::GetRenderHalfFovH();
    const float renderV = ME2VR::CalcViewHook::GetRenderHalfFovV();
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    if (rawH > 0.01f && rawH < 1.55f && renderH > 0.01f && renderH < 1.55f)
        scaleX = tanf(rawH) / tanf(renderH);
    if (rawV > 0.01f && rawV < 1.55f && renderV > 0.01f && renderV < 1.55f)
        scaleY = tanf(rawV) / tanf(renderV);
    scaleX = (std::max)(0.05f, (std::min)(2.0f, scaleX));
    scaleY = (std::max)(0.05f, (std::min)(2.0f, scaleY));

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(g_distortionScaleCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) ||
        mapped.pData == nullptr)
        return false;
    float* values = static_cast<float*>(mapped.pData);
    values[0] = scaleX;
    values[1] = scaleY;
    values[2] = 0.0f;
    values[3] = 0.0f;
    context->Unmap(g_distortionScaleCb, 0);

    ID3D11Buffer* oldCb = nullptr;
    context->PSGetConstantBuffers(13, 1, &oldCb);
    context->PSSetConstantBuffers(13, 1, &g_distortionScaleCb);
    draw();
    context->PSSetConstantBuffers(13, 1, &oldCb);
    if (oldCb != nullptr) oldCb->Release();

    const unsigned long long n =
        g_distortionCorrectedDraws.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || (n % 600) == 0)
    {
        char line[224] = {};
        sprintf_s(line,
                  "[DISTFIX] draw=%llu raw=(%.4f,%.4f) render=(%.4f,%.4f) fieldScale=(%.4f,%.4f)",
                  n, static_cast<double>(rawH), static_cast<double>(rawV),
                  static_cast<double>(renderH), static_cast<double>(renderV),
                  static_cast<double>(scaleX), static_cast<double>(scaleY));
        ME2VR::Log::Line(line);
    }
    return true;
}

void STDMETHODCALLTYPE MhDrawIndexed(ID3D11DeviceContext* ctx, UINT a, UINT b, INT c) noexcept
{
    const uint64_t hc0 = __rdtsc();   // [HOOKCOST]
    g_ftDraws.fetch_add(1, std::memory_order_relaxed);   // [FRAMETIME]
    RtHistNote();                                        // [RTTARGETS]
    // DIBR draw-gate: attribute this draw to the bound depth resource (the depth capture is
    // draw-count gated; without this the scene depth never promotes).
    if (g_depthMapEnabled.load(std::memory_order_relaxed) && g_boundDepthRes != nullptr)
    { uint32_t* dc = DepthDrawCounter(g_boundDepthRes); if (dc != nullptr) (*dc)++; }
    if (g_mhRealDrawIndexed == nullptr) return;

    D3D11_VIEWPORT vp = {};
    const bool isUiDraw = IsUiDrawNow(ctx, vp);
    SfrNoteBackbufferDraw(ctx);   // [SFRCAP2] scene draws only - a UI draw must not arm the capture
    if (isUiDraw) GuiSceneProbe(ctx);   // [GUISCENE]

    if (isUiDraw && g_uiDupEnabled.load(std::memory_order_acquire))
    {
        // UI draws are few; their dup/mirror work is counted whole (it is the mod's).
        EmitUiDraw(ctx, vp, [&] { g_mhRealDrawIndexed(ctx, a, b, c); });
        HookCostDraw(__rdtsc() - hc0);
        return;
    }
    HookCostDraw(__rdtsc() - hc0);
    g_mhRealDrawIndexed(ctx, a, b, c);
}
// Scaleform batches TEXT GLYPHS through DrawIndexedInstanced. Without dup, glyphs render once full-width
// and land cross-eyed (= scrambled text) while panels (DrawIndexed) look right. Dup it the same way.
void STDMETHODCALLTYPE MhDrawIndexedInstanced(ID3D11DeviceContext* ctx, UINT ipc, UINT ic, UINT sil, INT bvl, UINT sii) noexcept
{
    const uint64_t hc0 = __rdtsc();   // [HOOKCOST]
    g_ftDraws.fetch_add(1, std::memory_order_relaxed);   // [FRAMETIME]
    RtHistNote();                                        // [RTTARGETS]
    if (g_mhRealDrawIndexedInstanced == nullptr) return;
    D3D11_VIEWPORT vp = {};
    const bool isUiDraw = IsUiDrawNow(ctx, vp);
    SfrNoteBackbufferDraw(ctx);   // [SFRCAP2] scene draws only
    if (isUiDraw) GuiSceneProbe(ctx);   // [GUISCENE]
    if (isUiDraw && g_uiDupEnabled.load(std::memory_order_acquire))
    {
        EmitUiDraw(ctx, vp, [&] { g_mhRealDrawIndexedInstanced(ctx, ipc, ic, sil, bvl, sii); });
        HookCostDraw(__rdtsc() - hc0);
        return;
    }
    HookCostDraw(__rdtsc() - hc0);
    g_mhRealDrawIndexedInstanced(ctx, ipc, ic, sil, bvl, sii);
}
void STDMETHODCALLTYPE MhDraw(ID3D11DeviceContext* ctx, UINT a, UINT b) noexcept
{
    const uint64_t hc0 = __rdtsc();   // [HOOKCOST]
    g_ftDraws.fetch_add(1, std::memory_order_relaxed);   // [FRAMETIME]
    RtHistNote();                                        // [RTTARGETS]
    if (g_mhRealDraw == nullptr) return;
    if (EmitReprojectedDistortionDraw(ctx, [&] { g_mhRealDraw(ctx, a, b); })) { HookCostDraw(__rdtsc() - hc0); return; }
    // Menu/codex TEXT is drawn via non-indexed Draw; panels use DrawIndexed. Both need the per-eye dup or
    // text renders once full-width -> cross-eyed -> scrambled.
    D3D11_VIEWPORT vp = {};
    const bool isUiDraw = IsUiDrawNow(ctx, vp);
    SfrNoteBackbufferDraw(ctx);   // [SFRCAP2] scene draws only
    if (isUiDraw) GuiSceneProbe(ctx);   // [GUISCENE]
    if (isUiDraw && g_uiDupEnabled.load(std::memory_order_acquire))
    {
        EmitUiDraw(ctx, vp, [&] { g_mhRealDraw(ctx, a, b); });
        HookCostDraw(__rdtsc() - hc0);
        return;
    }
    HookCostDraw(__rdtsc() - hc0);
    g_mhRealDraw(ctx, a, b);
}

// ============================ DIBR (depth-image-based stereo) - functions ============================
bool EnsureGameContext() noexcept
{
    if (g_gameContext != nullptr) return true;
    if (g_gameDevice == nullptr) return false;
    g_gameDevice->GetImmediateContext(&g_gameContext);   // AddRef'd; held for process lifetime
    return g_gameContext != nullptr;
}

void FillDibrParamBuffer(float* p, float eyeScale) noexcept
{
    if (p == nullptr) return;
    p[0]  = g_dibrGain.load(std::memory_order_relaxed);
    p[1]  = g_dibrConvergence.load(std::memory_order_relaxed);
    p[2]  = g_dibrSign.load(std::memory_order_relaxed);
    p[3]  = g_dibrNearCut.load(std::memory_order_relaxed);
    p[4]  = g_dibrNearScale.load(std::memory_order_relaxed);
    p[5]  = g_dibrEdgeScale.load(std::memory_order_relaxed);
    p[6]  = g_dibrCrossScale.load(std::memory_order_relaxed);
    p[7]  = g_dibrLeakScale.load(std::memory_order_relaxed);
    p[8]  = 0.0f;   // (was labMode; layout kept)
    p[9]  = g_dibrSilhouetteScale.load(std::memory_order_relaxed);
    p[10] = g_dibrSourceScale.load(std::memory_order_relaxed);
    p[11] = eyeScale;
}

// SRV-able copy of the finished color frame (the warp samples this while the mod renders the synth eye).
bool EnsureColorCopy(const D3D11_TEXTURE2D_DESC& bbDesc) noexcept
{
    if (g_gameDevice == nullptr) return false;
    if (g_dibrColorCopy != nullptr && g_dibrColorW == bbDesc.Width && g_dibrColorH == bbDesc.Height) return true;
    SafeRelease(g_dibrColorSrv); SafeRelease(g_dibrColorCopy);
    D3D11_TEXTURE2D_DESC cd = bbDesc;
    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE; cd.Usage = D3D11_USAGE_DEFAULT;
    cd.CPUAccessFlags = 0; cd.MiscFlags = 0; cd.MipLevels = 1; cd.ArraySize = 1;
    HRESULT hr = g_gameDevice->CreateTexture2D(&cd, nullptr, &g_dibrColorCopy);
    if (FAILED(hr) || g_dibrColorCopy == nullptr) { ME2VR::Log::Line("[DIBR] color copy create failed hr=" + HexHRESULT(hr)); return false; }
    hr = g_gameDevice->CreateShaderResourceView(g_dibrColorCopy, nullptr, &g_dibrColorSrv);
    if (FAILED(hr) || g_dibrColorSrv == nullptr) { ME2VR::Log::Line("[DIBR] color srv create failed hr=" + HexHRESULT(hr)); SafeRelease(g_dibrColorCopy); return false; }
    g_dibrColorW = bbDesc.Width; g_dibrColorH = bbDesc.Height;
    ME2VR::Log::Line("[DIBR] color copy created " + std::to_string(bbDesc.Width) + "x" + std::to_string(bbDesc.Height));
    return true;
}

// Synthesized-right-eye target (backbuffer format/size, so it CopyResource's straight into the OpenXR eye).
bool EnsureWarpedTex(const D3D11_TEXTURE2D_DESC& bbDesc) noexcept
{
    if (g_gameDevice == nullptr) return false;
    if (g_dibrWarpedTex != nullptr && g_dibrWarpedW == bbDesc.Width && g_dibrWarpedH == bbDesc.Height) return true;
    SafeRelease(g_dibrWarpedSrv); SafeRelease(g_dibrWarpedRtv); SafeRelease(g_dibrWarpedTex);
    D3D11_TEXTURE2D_DESC cd = bbDesc;
    cd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    cd.Usage = D3D11_USAGE_DEFAULT; cd.CPUAccessFlags = 0; cd.MiscFlags = 0; cd.MipLevels = 1; cd.ArraySize = 1;
    HRESULT hr = g_gameDevice->CreateTexture2D(&cd, nullptr, &g_dibrWarpedTex);
    if (FAILED(hr) || g_dibrWarpedTex == nullptr) { ME2VR::Log::Line("[DIBR] warped tex create failed hr=" + HexHRESULT(hr)); return false; }
    hr = g_gameDevice->CreateRenderTargetView(g_dibrWarpedTex, nullptr, &g_dibrWarpedRtv);
    if (FAILED(hr) || g_dibrWarpedRtv == nullptr) { ME2VR::Log::Line("[DIBR] warped rtv create failed hr=" + HexHRESULT(hr)); SafeRelease(g_dibrWarpedTex); return false; }
    g_gameDevice->CreateShaderResourceView(g_dibrWarpedTex, nullptr, &g_dibrWarpedSrv);
    g_dibrWarpedW = bbDesc.Width; g_dibrWarpedH = bbDesc.Height;
    ME2VR::Log::Line("[DIBR] warped eye tex created " + std::to_string(bbDesc.Width) + "x" + std::to_string(bbDesc.Height));
    return true;
}

bool CompileDepthShader(const char* src, const char* entry, const char* profile, ID3DBlob** blob) noexcept
{
    if (blob == nullptr) return false;
    *blob = nullptr;
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    if (comp == nullptr) comp = LoadLibraryW(L"d3dcompiler_43.dll");
    if (comp == nullptr) { ME2VR::Log::Line("[DIBR] no d3dcompiler dll"); return false; }
    using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                          LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    auto compile = reinterpret_cast<D3DCompileFn>(GetProcAddress(comp, "D3DCompile"));
    if (compile == nullptr) { FreeLibrary(comp); ME2VR::Log::Line("[DIBR] no D3DCompile export"); return false; }
    ID3DBlob* err = nullptr;
    const HRESULT hr = compile(src, std::strlen(src), nullptr, nullptr, nullptr, entry, profile, 0, 0, blob, &err);
    if (FAILED(hr))
    {
        if (err) { ME2VR::Log::Line(std::string("[DIBR] compile err: ") + reinterpret_cast<const char*>(err->GetBufferPointer())); err->Release(); }
        FreeLibrary(comp);
        return false;
    }
    if (err) err->Release();
    FreeLibrary(comp);
    return true;
}

bool EnsureDepthVizShaders() noexcept
{
    if (g_depthVizReady) return true;
    if (g_depthVizTried) return false;
    g_depthVizTried = true;
    if (g_gameDevice == nullptr) return false;
    // Guard-gather DIBR warp (verbatim from ME1 - game-agnostic; reversed-Z assumed in the disparity sign).
    const char* src =
        "Texture2D colorTex : register(t0);\n"
        "Texture2D depthTex : register(t1);\n"
        "SamplerState s0 : register(s0);\n"
        "cbuffer DibrParams : register(b0) { float gain; float convergence; float sgn; float nearCut; float nearScale; float edgeScale; float crossScale; float leakScale; float labMode; float silhouetteScale; float sourceScale; float eyeScale; };\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut VSMain(uint id : SV_VertexID){ float2 p=float2((id==2)?3.0:-1.0,(id==1)?3.0:-1.0);\n"
        "  VSOut o; o.pos=float4(p,0.0,1.0); o.uv=float2((p.x+1.0)*0.5, 1.0-((p.y+1.0)*0.5)); return o; }\n"
        "float2 SourceUv(float2 uv){ return saturate((uv - 0.5) / max(sourceScale, 0.01) + 0.5); }\n"
        // EDGE-SHIMMER FIX (2026-07-12): silhouette pixels are where GPU depth is noisiest frame-to-frame,
        // and a raw single-tap depth drives the per-pixel disparity AND every guard comparison - so a 1-frame
        // depth wobble at an edge pixel visibly crawls the edge even with a static camera. Pseudo-median of 5
        // taps (center + 4 diagonals at ~1.25 texels): a single noisy tap is rejected outright instead of
        // averaged in, true edges stay sharp (median preserves steps), and both the disparity and the guards
        // see the same stabilized value.
        "float Med3(float a, float b, float c){ return max(min(a,b), min(max(a,b), c)); }\n"
        "float DepthMed5(float2 uv){\n"
        "  uint tw, th; depthTex.GetDimensions(tw, th);\n"
        "  float2 t = 1.25 / float2(max(tw,1u), max(th,1u));\n"
        "  float a = depthTex.Sample(s0, uv).r;\n"
        "  float b = depthTex.Sample(s0, saturate(uv + float2( t.x,  t.y))).r;\n"
        "  float c = depthTex.Sample(s0, saturate(uv + float2(-t.x,  t.y))).r;\n"
        "  float e = depthTex.Sample(s0, saturate(uv + float2( t.x, -t.y))).r;\n"
        "  float f = depthTex.Sample(s0, saturate(uv + float2(-t.x, -t.y))).r;\n"
        "  return Med3(Med3(a, b, c), e, f);\n"
        "}\n"
        "float DepthEdge(float2 uv, float d){\n"
        "  float2 t=float2(1.0/1024.0,1.0/1024.0);\n"
        "  float dl=depthTex.Sample(s0, float2(saturate(uv.x-t.x),uv.y)).r;\n"
        "  float dr=depthTex.Sample(s0, float2(saturate(uv.x+t.x),uv.y)).r;\n"
        "  float du=depthTex.Sample(s0, float2(uv.x,saturate(uv.y-t.y))).r;\n"
        "  float dd=depthTex.Sample(s0, float2(uv.x,saturate(uv.y+t.y))).r;\n"
        "  return saturate(max(max(abs(d-dl),abs(d-dr)),max(abs(d-du),abs(d-dd))) * edgeScale);\n"
        "}\n"
        "float SilhouetteCross(float2 uv, float2 warpedUv, float d){\n"
        "  float occ=0.0;\n"
        "  float2 u1=lerp(uv, warpedUv, 0.25);\n"
        "  float2 u2=lerp(uv, warpedUv, 0.50);\n"
        "  float2 u3=lerp(uv, warpedUv, 0.75);\n"
        "  float2 u4=warpedUv;\n"
        "  occ=max(occ, saturate((d - depthTex.Sample(s0, u1).r - 0.0010) * silhouetteScale));\n"
        "  occ=max(occ, saturate((d - depthTex.Sample(s0, u2).r - 0.0010) * silhouetteScale));\n"
        "  occ=max(occ, saturate((d - depthTex.Sample(s0, u3).r - 0.0010) * silhouetteScale));\n"
        "  occ=max(occ, saturate((d - depthTex.Sample(s0, u4).r - 0.0010) * silhouetteScale));\n"
        "  return occ;\n"
        "}\n"
        "float4 PSMain(VSOut i):SV_Target{\n"
        "  float2 baseUv = SourceUv(i.uv);\n"
        "  float d = DepthMed5(baseUv);\n"
        "  float disparity = sgn * gain * eyeScale * (convergence - d);\n"
        "  float edge = DepthEdge(baseUv, d);\n"
        "  float2 probeUv = SourceUv(float2(saturate(i.uv.x + disparity), i.uv.y));\n"
        "  float probeD = DepthMed5(probeUv);\n"
        "  float crs = saturate(abs(probeD - d) * crossScale);\n"
        "  float nearFg = saturate((nearCut - d) * nearScale);\n"
        "  float fgLeak = saturate((probeD - d - 0.0005) * leakScale);\n"
        "  float silhouette = SilhouetteCross(baseUv, probeUv, d);\n"
        "  float guard = 1.0 - saturate(max(max(max(edge, crs), fgLeak), silhouette));\n"
        "  guard *= (1.0 - nearFg);\n"
        "  guard = guard * guard;\n"
        "  float2 warpedUv = SourceUv(float2(saturate(i.uv.x + disparity * guard), i.uv.y));\n"
        "  return colorTex.Sample(s0, warpedUv);\n"
        "}\n";
    const char* mapSrc =
        "Texture2D depthTex : register(t1);\n"
        "SamplerState s0 : register(s0);\n"
        "cbuffer DepthMapParams : register(b0) { float nearD; float farD; float flipD; float gammaD; };\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "float4 PSMap(VSOut i):SV_Target{\n"
        "  float d = depthTex.Sample(s0, i.uv).r;\n"
        "  float g = saturate((d - farD) / max(nearD - farD, 1e-5));\n"
        "  if (flipD > 0.5) g = 1.0 - g;\n"
        "  g = pow(saturate(g), max(gammaD, 0.05));\n"
        "  return float4(g, g, g, 1.0);\n"
        "}\n";
    ID3DBlob* vs = nullptr; ID3DBlob* ps = nullptr; ID3DBlob* mapPs = nullptr;
    if (!CompileDepthShader(src, "VSMain", "vs_4_0", &vs) ||
        !CompileDepthShader(src, "PSMain", "ps_4_0", &ps) ||
        !CompileDepthShader(mapSrc, "PSMap", "ps_4_0", &mapPs))
    { if (vs) vs->Release(); if (ps) ps->Release(); if (mapPs) mapPs->Release(); return false; }
    HRESULT hr = g_gameDevice->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_depthVizVs);
    if (SUCCEEDED(hr)) hr = g_gameDevice->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_depthVizPs);
    if (SUCCEEDED(hr)) hr = g_gameDevice->CreatePixelShader(mapPs->GetBufferPointer(), mapPs->GetBufferSize(), nullptr, &g_depthMapPs);
    vs->Release(); ps->Release(); mapPs->Release();
    if (FAILED(hr) || g_depthVizVs == nullptr || g_depthVizPs == nullptr) { ME2VR::Log::Line("[DIBR] shader create failed"); return false; }
    D3D11_SAMPLER_DESC sd = {}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_gameDevice->CreateSamplerState(&sd, &g_depthVizSampler);
    D3D11_RASTERIZER_DESC rd = {}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
    g_gameDevice->CreateRasterizerState(&rd, &g_depthVizRaster);
    D3D11_DEPTH_STENCIL_DESC dd = {}; dd.DepthEnable = FALSE; dd.StencilEnable = FALSE;
    g_gameDevice->CreateDepthStencilState(&dd, &g_depthVizDepthState);
    D3D11_BLEND_DESC bd = {}; bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_gameDevice->CreateBlendState(&bd, &g_depthVizBlend);
    D3D11_BUFFER_DESC cbd = {}; cbd.ByteWidth = 48; cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_gameDevice->CreateBuffer(&cbd, nullptr, &g_dibrParamsCb);
    D3D11_BUFFER_DESC mcbd = {}; mcbd.ByteWidth = 16; mcbd.Usage = D3D11_USAGE_DYNAMIC;
    mcbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; mcbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_gameDevice->CreateBuffer(&mcbd, nullptr, &g_depthMapCb);
    if (g_depthVizSampler == nullptr || g_depthVizRaster == nullptr || g_depthVizDepthState == nullptr) { ME2VR::Log::Line("[DIBR] state create failed"); return false; }
    g_depthVizReady = true;
    ME2VR::Log::Line("[DIBR] warp shaders ready");
    return true;
}

// Copy + SRV formats FOLLOW the source. A hardcoded R24G8 copy of a D32 source is an illegal CopyResource
// that D3D silently drops -> uninitialized flat depth. (TODO-1: add a case if LE2's format isn't here.)
bool EnsureDepthCopy(const D3D11_TEXTURE2D_DESC& srcDesc) noexcept
{
    if (g_gameDevice == nullptr) return false;
    if (g_depthCopy != nullptr && g_depthCopyW == srcDesc.Width && g_depthCopyH == srcDesc.Height &&
        g_depthCopyFmt == srcDesc.Format) return true;
    DXGI_FORMAT copyFmt, srvFmt; int decode;
    switch (srcDesc.Format)
    {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        copyFmt = DXGI_FORMAT_R24G8_TYPELESS; srvFmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; decode = 0; break;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        copyFmt = DXGI_FORMAT_R32_TYPELESS; srvFmt = DXGI_FORMAT_R32_FLOAT; decode = 1; break;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        copyFmt = DXGI_FORMAT_R32G8X24_TYPELESS; srvFmt = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; decode = 2; break;
    default:
        { static int s_fmtLogs = 0; if (s_fmtLogs++ < 3) ME2VR::Log::Line("[DIBR] unsupported depth format " + std::to_string(static_cast<int>(srcDesc.Format))); return false; }
    }
    SafeRelease(g_depthSrv); SafeRelease(g_depthCopy); SafeRelease(g_depthPublished);
    g_depthReady.store(false, std::memory_order_release);
    D3D11_TEXTURE2D_DESC cd = srcDesc;
    cd.Format = copyFmt; cd.BindFlags = D3D11_BIND_SHADER_RESOURCE; cd.Usage = D3D11_USAGE_DEFAULT;
    cd.CPUAccessFlags = 0; cd.MiscFlags = 0; cd.MipLevels = 1; cd.ArraySize = 1;
    HRESULT hr = g_gameDevice->CreateTexture2D(&cd, nullptr, &g_depthCopy);
    if (FAILED(hr) || g_depthCopy == nullptr) { ME2VR::Log::Line("[DIBR] copy tex create failed hr=" + HexHRESULT(hr)); return false; }
    hr = g_gameDevice->CreateTexture2D(&cd, nullptr, &g_depthPublished);
    if (FAILED(hr) || g_depthPublished == nullptr) { ME2VR::Log::Line("[DIBR] published tex create failed hr=" + HexHRESULT(hr)); SafeRelease(g_depthCopy); return false; }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = srvFmt; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    hr = g_gameDevice->CreateShaderResourceView(g_depthPublished, &sd, &g_depthSrv);
    if (FAILED(hr) || g_depthSrv == nullptr) { ME2VR::Log::Line("[DIBR] srv create failed hr=" + HexHRESULT(hr)); SafeRelease(g_depthCopy); SafeRelease(g_depthPublished); return false; }
    SafeRelease(g_depthStaging[0]); SafeRelease(g_depthStaging[1]);
    SafeRelease(g_depthGpuSnap[0]); SafeRelease(g_depthGpuSnap[1]);
    g_depthStagingInFlight[0] = g_depthStagingInFlight[1] = false; g_depthStagingWrite = 0;
    D3D11_TEXTURE2D_DESC stg = cd; stg.BindFlags = 0; stg.Usage = D3D11_USAGE_STAGING; stg.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    g_gameDevice->CreateTexture2D(&stg, nullptr, &g_depthStaging[0]);
    g_gameDevice->CreateTexture2D(&stg, nullptr, &g_depthStaging[1]);
    g_gameDevice->CreateTexture2D(&cd, nullptr, &g_depthGpuSnap[0]);
    g_gameDevice->CreateTexture2D(&cd, nullptr, &g_depthGpuSnap[1]);
    g_depthCopyW = srcDesc.Width; g_depthCopyH = srcDesc.Height;
    g_depthCopyFmt = srcDesc.Format; g_depthDecodeKind = decode;
    char fbuf[144];
    std::snprintf(fbuf, sizeof(fbuf), "[DIBR] sampleable copy created %ux%u srcFmt=%d copyFmt=%d decode=%d",
                  srcDesc.Width, srcDesc.Height, static_cast<int>(srcDesc.Format), static_cast<int>(copyFmt), decode);
    ME2VR::Log::Line(fbuf);
    return true;
}

// Snapshot 'tex' into scratch, probe every 4th capture (non-blocking staging ring), and PROMOTE a
// spread-verified capture to g_depthPublished (hold-last-good otherwise). Render-thread only.
void RunDepthCapturePipeline(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& d) noexcept
{
    if (ctx == nullptr || tex == nullptr || !EnsureDepthCopy(d)) return;
    ctx->CopyResource(g_depthCopy, tex);
    g_depthCopyCount.fetch_add(1, std::memory_order_relaxed);
    const uint32_t pc = g_depthProbeLogs.fetch_add(1, std::memory_order_relaxed);
    // Every 2nd capture (was 4th): the auto-convergence input was updating at quarter rate and adding lag on
    // top of its own smoothing. The staging ring is 2-deep and non-blocking, so 1/2 cadence is still safe.
    if ((pc % 2u) != 0u || g_depthStaging[0] == nullptr || g_depthStaging[1] == nullptr) return;
    const int w = g_depthStagingWrite; const int r = 1 - w;
    ctx->CopyResource(g_depthStaging[w], g_depthCopy);
    if (g_depthGpuSnap[w] != nullptr) ctx->CopyResource(g_depthGpuSnap[w], g_depthCopy);
    g_depthStagingInFlight[w] = true; g_depthStagingWrite = r;
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (!g_depthStagingInFlight[r] ||
        FAILED(ctx->Map(g_depthStaging[r], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) return;
    g_depthStagingInFlight[r] = false;
    const int decodeKind = g_depthDecodeKind;
    auto sample = [&](UINT x, UINT y) -> float {
        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
        if (decodeKind == 1) return *reinterpret_cast<const float*>(row + static_cast<size_t>(x) * 4u);
        if (decodeKind == 2) return *reinterpret_cast<const float*>(row + static_cast<size_t>(x) * 8u);
        const uint32_t v = *reinterpret_cast<const uint32_t*>(row + static_cast<size_t>(x) * 4u);
        return static_cast<float>(v & 0x00FFFFFFu) / 16777215.0f;
    };
    // CONVERGENCE STABILITY (2026-07-12: auto-convergence "keeps moving around"): the convergence
    // input was a SINGLE center pixel - any depth noise or a thin prop crossing one pixel yanked the whole
    // stereo convergence plane. Now: median of a 3x3 grid spread over the central ~20% of the frame. The
    // median rejects outlier pixels entirely (a wall edge or particle through one tap changes nothing) while
    // still tracking what the player is actually looking at. Raw center kept for the log comparison.
    const float centerRaw = sample(d.Width / 2, d.Height / 2);
    float med[9];
    {
        const UINT cx = d.Width / 2, cy = d.Height / 2;
        const UINT ox = d.Width / 10, oy = d.Height / 10;
        int n9 = 0;
        for (int gy = -1; gy <= 1; ++gy)
            for (int gx = -1; gx <= 1; ++gx)
                med[n9++] = sample(cx + gx * ox, cy + gy * oy);
        // insertion sort 9 floats; median = med[4]
        for (int i = 1; i < 9; ++i)
        {
            const float v = med[i];
            int j = i - 1;
            while (j >= 0 && med[j] > v) { med[j + 1] = med[j]; --j; }
            med[j + 1] = v;
        }
    }
    const float center = med[4];
    const float tl = sample(24, 24);
    const float br = sample(d.Width - 24, d.Height - 24);
    const float tr = sample(d.Width - 24, 24);
    const float lc = sample(d.Width / 4, d.Height / 2);
    const float rc = sample((d.Width * 3) / 4, d.Height / 2);
    g_probeCenter.store(center, std::memory_order_relaxed);
    g_probeTL.store(tl, std::memory_order_relaxed); g_probeBR.store(br, std::memory_order_relaxed);
    g_probeTR.store(tr, std::memory_order_relaxed); g_probeLC.store(lc, std::memory_order_relaxed);
    g_probeRC.store(rc, std::memory_order_relaxed);
    float mn = center, mx = center;
    const float taps[5] = { tl, br, tr, lc, rc };
    for (float t : taps) { if (t < mn) mn = t; if (t > mx) mx = t; }
    const bool good = (mx - mn) > 0.005f;
    if (pc < 45u || (pc % 240u) == 0u)
    {
        char buf[224];
        std::snprintf(buf, sizeof(buf),
                      "[DIBR_PROBE] depth centerMed=%.5f centerRaw=%.5f TL=%.5f BR=%.5f TR=%.5f L=%.5f R=%.5f spread=%.5f %s",
                      center, centerRaw, tl, br, tr, lc, rc, mx - mn, good ? "PROMOTE" : "hold");
        ME2VR::Log::Line(buf);
    }
    ctx->Unmap(g_depthStaging[r], 0);
    if (good && g_depthPublished != nullptr && g_depthGpuSnap[r] != nullptr)
    {
        ctx->CopyResource(g_depthPublished, g_depthGpuSnap[r]);
        g_depthReady.store(true, std::memory_order_release);
    }
}

// [SFR] Hold the pass-0 (left eye) image. The texture is a full backbuffer copy with BIND_RENDER_TARGET
// so UI draws can also be mirrored into it; it remains a valid CopyResource source for the submit.
bool SfrEnsurePass0Tex(const D3D11_TEXTURE2D_DESC& bb) noexcept
{
    if (g_sfrPass0Tex != nullptr &&
        g_sfrPass0Desc.Width == bb.Width && g_sfrPass0Desc.Height == bb.Height &&
        g_sfrPass0Desc.Format == bb.Format)
        return true;
    if (g_sfrPass0Tex != nullptr) { g_sfrPass0Tex->Release(); g_sfrPass0Tex = nullptr; }
    if (g_sfrPass0Rtv != nullptr) { g_sfrPass0Rtv->Release(); g_sfrPass0Rtv = nullptr; }   // stale after recreate
    D3D11_TEXTURE2D_DESC d = bb;
    d.BindFlags = D3D11_BIND_RENDER_TARGET; d.CPUAccessFlags = 0; d.MiscFlags = 0; d.Usage = D3D11_USAGE_DEFAULT;
    if (g_gameDevice == nullptr || FAILED(g_gameDevice->CreateTexture2D(&d, nullptr, &g_sfrPass0Tex)))
    { g_sfrPass0Tex = nullptr; return false; }
    g_sfrPass0Desc = d;
    return true;
}

// Snapshot the backbuffer at EVERY scene-sized depth clear. Each pass clears depth at its start and
// composites at its end, so at a clear the backbuffer holds the PREVIOUS pass's final image; pass 1
// (the replay) is last, so its clear = the last snapshot before present = pass 0's finished left-eye
// image. No flag or cross-thread marker is involved, so there is no race with the game thread.
void SfrMaybeCapturePass0AtClear(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv) noexcept
{
    if (ctx == nullptr || dsv == nullptr || g_gameSwapChain == nullptr) return;
    ID3D11Resource* dres = nullptr;
    dsv->GetResource(&dres);
    if (dres == nullptr) return;
    bool sceneSized = false;
    const std::uintptr_t depthPtr = reinterpret_cast<std::uintptr_t>(dres);
    unsigned depthW = 0, depthH = 0;
    ID3D11Texture2D* dtex = nullptr;
    if (SUCCEEDED(dres->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&dtex))) && dtex != nullptr)
    {
        D3D11_TEXTURE2D_DESC dd = {}; dtex->GetDesc(&dd);
        const UINT rw = g_sceneRenderW.load(std::memory_order_acquire);
        const UINT rh = g_sceneRenderH.load(std::memory_order_acquire);
        depthW = dd.Width; depthH = dd.Height;
        // [SFRCLEAR] census before any gate, so a rejected clear still convicts itself in the log.
        g_scClears.fetch_add(1, std::memory_order_relaxed);
        if (static_cast<uint64_t>(dd.Width) * dd.Height >
            static_cast<uint64_t>(g_scBigW.load(std::memory_order_relaxed)) * g_scBigH.load(std::memory_order_relaxed))
        {
            g_scBigW.store(dd.Width, std::memory_order_relaxed);
            g_scBigH.store(dd.Height, std::memory_order_relaxed);
            g_scBigSamples.store(dd.SampleDesc.Count, std::memory_order_relaxed);
        }
        if (rw != 0 && dd.Width == rw && dd.Height == rh && dd.SampleDesc.Count != 1)
            g_scSizeOkMsaaBad.fetch_add(1, std::memory_order_relaxed);
        // [MSAAOK 2026-08-20] The sample-count test is dropped. This clear is only a TIMING marker -
        // the image the mod snapshots is the BACKBUFFER, which is never multisampled - so whether the depth
        // target carries MSAA says nothing about whether the scene pass just started. The size match
        // against the backbuffer is what identifies the scene pass and already excludes shadow maps
        // and effect buffers. Keeping the old ==1 test meant anyone playing with MSAA enabled got no
        // capture at all, both eyes fell back to the live backbuffer, and stereo rendered FLAT.
        sceneSized = (rw != 0) && (dd.Width == rw) && (dd.Height == rh);
        // [SCENEH 2026-08-22] The HEIGHT equality is the fragile half of this test, and it
        // cost a flat boot. Measured from a 07:33 session: the scene rendered into a
        // 2048x1440 depth target while the backbuffer was 2048x1152, so sceneSized went to 0
        // for the whole run, no clear ever qualified, the left eye never captured and both
        // eyes fell back to the live backbuffer = FLAT. The same 2048x1152 setting captured
        // perfectly in the session an hour earlier, where the scene target WAS 2048x1152, so
        // this is the engine choosing a different scene height, not a setting.
        // [CINECLEAR] below already had to solve exactly this for cutscenes. Same reasoning
        // applies in gameplay: this clear is only a TIMING marker - the image copied is the
        // BACKBUFFER - so the height does not have to match anything. Accept a full-WIDTH,
        // single-sample, non-square depth clear whose height is in a sane band around the
        // backbuffer. Shadow maps are square, GUI/effect buffers are narrower, and the one
        // large odd target in that log (3072x3264) has the wrong width, so all stay excluded.
        if (!sceneSized && rw != 0 && dd.Width == rw && dd.SampleDesc.Count == 1 &&
            dd.Width != dd.Height && rh != 0 && dd.Height >= rh / 2 && dd.Height <= rh * 2)
        {
            sceneSized = true;
            static UINT s_shW = 0, s_shH = 0;   // one line per unique size, names it in the log
            if (s_shW != dd.Width || s_shH != dd.Height)
            {
                s_shW = dd.Width; s_shH = dd.Height;
                ME2VR::Log::Line("[SCENEH] scene depth " + std::to_string(dd.Width) + "x" +
                                 std::to_string(dd.Height) + " accepted against backbuffer " +
                                 std::to_string(rw) + "x" + std::to_string(rh) +
                                 " (height differs; capture would otherwise be lost = flat)");
            }
        }
        if (sceneSized) g_scSceneSized.fetch_add(1, std::memory_order_relaxed);
        // [CINECLEAR] In-engine cutscenes (Tuchanka reaper run, measured) render their scene into a
        // DIFFERENT-sized depth target than gameplay: scene-sized clears dropped to 0 the moment the
        // cine started, the left-eye capture went stale, and the submit fell back to the mono quad -
        // flat, squashed, no tracking, exactly what a "VR cutscene" must not be. The clear is only a
        // TIMING marker (the copy source is the backbuffer), so during a VR cine accept any large
        // non-square single-sample depth clear as the marker: an earlier qualifying clear in the
        // pass0-composited window snapshots the same (correct) backbuffer content. Square targets
        // (shadow maps) and small effect buffers stay excluded.
        if (!sceneSized && ME2VR::CalcViewHook::GetVrCineActive() &&
            dd.SampleDesc.Count == 1 && dd.Width != dd.Height &&
            rw != 0 && dd.Width >= rw / 2)
        {
            sceneSized = true;
            // One line per unique size per session: names the cine scene target for the log.
            static UINT s_seenW[8] = {}, s_seenH[8] = {};
            static int s_seenCount = 0;
            bool known = false;
            for (int i = 0; i < s_seenCount; ++i)
                if (s_seenW[i] == dd.Width && s_seenH[i] == dd.Height) { known = true; break; }
            if (!known && s_seenCount < 8)
            {
                s_seenW[s_seenCount] = dd.Width; s_seenH[s_seenCount] = dd.Height; ++s_seenCount;
                ME2VR::Log::Line("[CINECLEAR] accepting cine depth clear " + std::to_string(dd.Width) +
                                 "x" + std::to_string(dd.Height) + " (gameplay scene=" + std::to_string(rw) +
                                 "x" + std::to_string(rh) + ")");
            }
        }
        dtex->Release();
    }
    dres->Release();
    if (!sceneSized) return;
    ++g_flkPassesThisPresent;   // [FLICKER2] a scene render started; SFR must see 2 of these per present

    ID3D11Texture2D* bb = nullptr;
    if (FAILED(g_gameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || bb == nullptr)
        return;
    D3D11_TEXTURE2D_DESC bd = {}; bb->GetDesc(&bd);
    // [SFRCAP] WHICH clear to snapshot at is the whole game. The LE2 rule ("overwrite at every
    // scene-sized clear; the last one before present is pass 0's finished image") is wrong on LE3:
    // scene-sized depth clears also happen AFTER pass 1 composites to the backbuffer, so the last
    // snapshot was pass 1's image and BOTH eyes showed the same frame - depth was zero, and raising
    // eye separation just shifted the world sideways ([SFR2] proved the cameras themselves differed:
    // calcViews ran at exactly 2x replays). Instead: capture ONCE per present, at the FIRST scene-
    // sized clear recorded during the REPLAY pass - at that moment the backbuffer provably still
    // holds pass 0. In-order in the same command stream, so no cross-thread race (the flag is
    // thread-local to the recording thread). If replay-pass clears never reach this thread, fall
    // back to the legacy every-clear rule rather than leaving the left eye permanently stale.
    // [SFRCAP2] capture exactly once per present, at the first scene-sized clear AFTER pass 0 has
    // composited to the backbuffer. Before that draw the backbuffer holds LAST frame; after pass 1
    // composites, snapshots would show pass 1 (the both-eyes-identical bug this replaces).
    const bool bbWritten = g_sfrBbDrawnThisPresent.load(std::memory_order_relaxed);
    if (bbWritten) g_scBbWrittenAtClear.fetch_add(1, std::memory_order_relaxed);   // [SFRCLEAR]
    // [CINEMAP] PROBE: record this accepted clear. Pure observation - nothing below reads it, and the
    // capture rule is byte-for-byte the shipped one. The recorded index of the capture is what shows
    // where the current rule actually lands inside the pass structure.
    if (ME2VR::CalcViewHook::GetVrCineActive() && g_cineMapLen < kCineMapMax)
    {
        g_cineClearThreadId = GetCurrentThreadId();
        CineClearRec& r = g_cineMap[g_cineMapLen++];
        r.depth = depthPtr; r.rtv = g_currentRtvResourcePtr;
        r.w = depthW; r.h = depthH;
        r.bbWritten = bbWritten;
        r.deferred = (ctx->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED);
        r.replay = ME2VR::CalcViewHook::InReplayPass();
    }
    // [CINECAP] fallback for a cine whose composite path is invisible to every hook (e.g. recorded
    // on a deferred context): the accepted cine clears are the pass STARTS, so by the second one
    // the stream has recorded pass 0's composite - capture there instead of dropping to the mono
    // quad. g_sfrClearsThisPresent counts clears already seen this present, so >=1 here means this
    // is at least the second.
    // [CINECAP REMOVED 2026-08-14] The second-clear cine fallback WAS the broken-cutscene bug, and
    // [CINEMAP] measured it directly. A cine present's accepted clears look like this (n=8):
    //     idx 0: 1af60 rtv=0000   <- pre-pass/shadow target, NO colour target bound
    //     idx 1: 1b220 rtv=0000   <- ditto; the fallback captured HERE
    //     idx 2..7: 0b7e0         <- the actual scene target
    //     bbAt=5                  <- a backbuffer WRITE is visible at index 5
    // The fallback fired at index 1, before anything had been drawn, so it snapshotted the PREVIOUS
    // present's finished image - which is pass 1, the right eye. Held "left" eye = a stale right eye,
    // both eyes one viewpoint, zero disparity: raising eye separation could only slide the whole
    // world sideways instead of changing depth. That is exactly the reported symptom, and it is the
    // difference from ME2, where the same slider changes SCALE because the eyes really do differ.
    // It also poisoned the correct path: capturing at index 1 set g_sfrCapturedThisPresent, so the
    // backbuffer-write trigger below could never run.
    // The measurement also shows there was never a need for a special case. bbAt tracks the scene
    // (4 at n=6, 5 at n=8, 6 at n=10) and marks the FIRST backbuffer write of the present = pass 0's
    // composite - the identical signal gameplay already captures on, and ME2 ships no cine-specific
    // capture at all. So cinematics now use the one proven rule.
    // Measured dead, do not reintroduce: fixed clear ordinals (n swings 6-10 between shots, so #5
    // flickered), deferred-context theories (defer=0, clears are on the immediate context), and
    // the thread-local replay flag (replayAt=-1; Draw and the clears run on different threads).
    const bool doCapture = bbWritten && !g_sfrCapturedThisPresent;
    if (SfrEnsurePass0Tex(bd))
    {
        if (doCapture)
        {
            ctx->CopyResource(g_sfrPass0Tex, bb);
            g_flkCapAtClear = g_flkPassesThisPresent;   // [CAPJITTER] which clear of this present it landed on
            g_sfrLastCapturePresent.store(g_presentIndex.load(std::memory_order_relaxed), std::memory_order_relaxed);
            g_sfrCapturedThisPresent = true;
            if (ME2VR::CalcViewHook::GetVrCineActive() && g_cineCapturedAtIndex < 0)
                g_cineCapturedAtIndex = g_cineMapLen - 1;   // [CINEMAP] probe: where this rule landed
            g_sfrPass0Caps.fetch_add(1, std::memory_order_relaxed);
            // Which mechanism armed the CINE capture - one line per session, the next diagnostic.
            static bool s_cineArmLogged = false;
            if (!s_cineArmLogged && ME2VR::CalcViewHook::GetVrCineActive())
            {
                s_cineArmLogged = true;
                ME2VR::Log::Line("[CINECAP] cine capture armed by the backbuffer-write flag "
                                 "(same rule as gameplay)");
            }
        }
        ++g_sfrClearsThisPresent;
        static bool s_capModeLogged = false;
        if (doCapture && !s_capModeLogged)
        {
            s_capModeLogged = true;
            ME2VR::Log::Line("[SFRCAP2] pass-0 capture keyed to the first clear after the backbuffer composite.");
        }
    }
    bb->Release();
}

// Capture the scene depth into the sampleable copy BEFORE the game wipes it. Only active in DIBR mode
// (g_depthMapEnabled); DIBR renders mono so the clear-time capture is intact (no stereo trample).
void STDMETHODCALLTYPE MhClearDepthStencilView(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv,
                                              UINT flags, FLOAT depthVal, UINT8 stencil) noexcept
{
    if (SfrModeActive()) SfrMaybeCapturePass0AtClear(ctx, dsv);   // [SFR] hold pass 0 = the left eye
    // Bring-up instrumentation: confirm the hook actually fires + whether capture is enabled.
    {
        static std::atomic<int> s_dsvFires{0};
        const int f = s_dsvFires.fetch_add(1, std::memory_order_relaxed);
        if (f < 3 || (g_depthMapEnabled.load(std::memory_order_relaxed) && (f % 600) == 0))
            ME2VR::Log::Line("[DIBR] ClearDSV fired #" + std::to_string(f) + " enabled=" +
                             std::to_string(g_depthMapEnabled.load(std::memory_order_relaxed) ? 1 : 0));
    }
    if (ctx != nullptr && dsv != nullptr && g_depthMapEnabled.load(std::memory_order_relaxed))
    {
        ID3D11Resource* res = nullptr;
        dsv->GetResource(&res);
        if (res != nullptr)
        {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex != nullptr)
            {
                D3D11_TEXTURE2D_DESC d = {}; tex->GetDesc(&d);
                const UINT renderW = g_sceneRenderW.load(std::memory_order_acquire);
                const UINT renderH = g_sceneRenderH.load(std::memory_order_acquire);
                const bool sceneSized = (renderW != 0) && (d.Width == renderW) && (d.Height == renderH);
                if (!sceneSized && d.SampleDesc.Count == 1 && d.Width >= 512)
                {
                    const int dn = g_depthMissLogs.fetch_add(1, std::memory_order_relaxed);
                    if (dn < 20)
                        ME2VR::Log::Line("[DIBR] non-scene depth clear " + std::to_string(d.Width) + "x" +
                                         std::to_string(d.Height) + " (render=" + std::to_string(renderW) + "x" +
                                         std::to_string(renderH) + ")");
                }
                if (sceneSized && d.SampleDesc.Count == 1)
                {
                    uint32_t* counter = DepthDrawCounter(res);
                    const uint32_t drawsSince = (counter != nullptr) ? *counter : 0;
                    if (counter != nullptr) *counter = 0;
                    // FLICKER FIX (LE2): two scene-sized depth buffers compete each frame - the real full scene
                    // (~1200 draws) and a partial pre-pass/reflection (~58 draws). Capturing BOTH alternated the
                    // published depth -> flicker. Reject anything well below the running PEAK draw count (adapts to
                    // scene complexity instead of a brittle fixed threshold), so only the richest depth wins.
                    static uint32_t s_peakDraws = 0;
                    if (drawsSince > s_peakDraws) s_peakDraws = drawsSince;
                    else if (s_peakDraws > 8) s_peakDraws -= (s_peakDraws >> 7);   // slow decay (~0.8%/clear)
                    const uint32_t adaptiveGate = s_peakDraws / 3;
                    const uint32_t gate = (adaptiveGate > kSceneDepthDrawThreshold) ? adaptiveGate : kSceneDepthDrawThreshold;
                    const bool populated = drawsSince >= gate;
                    const int n = g_depthCapLogs.fetch_add(1, std::memory_order_relaxed);
                    if (n < 60 || (n % 600) == 0)
                    {
                        char clbuf[208];
                        std::snprintf(clbuf, sizeof(clbuf),
                                      "[DIBR] scene clear %ux%u fmt=%d drawsSince=%u gate=%u populated=%d clearVal=%.2f",
                                      d.Width, d.Height, static_cast<int>(d.Format), drawsSince, gate, populated ? 1 : 0, depthVal);
                        ME2VR::Log::Line(clbuf);
                    }
                    if (populated) RunDepthCapturePipeline(ctx, tex, d);
                }
                tex->Release();
            }
            res->Release();
        }
    }
    if (g_mhRealClearDSV != nullptr) g_mhRealClearDSV(ctx, dsv, flags, depthVal, stencil);
}

// Warp the finished color frame into 'outTex' using the captured depth. Full pipeline save/restore.
ID3D11Texture2D* RenderDibrEye(ID3D11Texture2D* backBuffer, ID3D11Texture2D* outTex, ID3D11RenderTargetView* outRtv, float eyeScale) noexcept
{
    if (!EnsureGameContext() || backBuffer == nullptr || outTex == nullptr || outRtv == nullptr) return nullptr;
    if (!g_depthReady.load(std::memory_order_acquire) || g_depthSrv == nullptr) return nullptr;
    if (!EnsureDepthVizShaders()) return nullptr;
    D3D11_TEXTURE2D_DESC bbd = {}; backBuffer->GetDesc(&bbd);
    g_gameContext->CopyResource(g_dibrColorCopy, backBuffer);

    ID3D11RenderTargetView* oldRtv[8] = {}; ID3D11DepthStencilView* oldDsv = nullptr;
    g_gameContext->OMGetRenderTargets(8, oldRtv, &oldDsv);
    D3D11_VIEWPORT oldVp[16] = {}; UINT oldVpN = 16; g_gameContext->RSGetViewports(&oldVpN, oldVp);
    ID3D11RasterizerState* oldRs = nullptr; g_gameContext->RSGetState(&oldRs);
    ID3D11DepthStencilState* oldDs = nullptr; UINT oldRef = 0; g_gameContext->OMGetDepthStencilState(&oldDs, &oldRef);
    float oldBlendFactor[4] = {}; UINT oldSampleMask = 0xffffffff; ID3D11BlendState* oldBlend = nullptr;
    g_gameContext->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);
    D3D11_PRIMITIVE_TOPOLOGY oldTopo; g_gameContext->IAGetPrimitiveTopology(&oldTopo);
    ID3D11InputLayout* oldIl = nullptr; g_gameContext->IAGetInputLayout(&oldIl);
    ID3D11VertexShader* oldVs = nullptr; g_gameContext->VSGetShader(&oldVs, nullptr, nullptr);
    ID3D11PixelShader* oldPs = nullptr; g_gameContext->PSGetShader(&oldPs, nullptr, nullptr);
    ID3D11ShaderResourceView* oldSrv[2] = {}; g_gameContext->PSGetShaderResources(0, 2, oldSrv);
    ID3D11SamplerState* oldSamp = nullptr; g_gameContext->PSGetSamplers(0, 1, &oldSamp);

    D3D11_VIEWPORT vp = {}; vp.Width = static_cast<float>(bbd.Width); vp.Height = static_cast<float>(bbd.Height); vp.MaxDepth = 1.0f;
    const float bf[4] = {0, 0, 0, 0};
    g_gameContext->OMSetRenderTargets(1, &outRtv, nullptr);
    g_gameContext->RSSetViewports(1, &vp);
    g_gameContext->RSSetState(g_depthVizRaster);
    g_gameContext->OMSetDepthStencilState(g_depthVizDepthState, 0);
    g_gameContext->OMSetBlendState(g_depthVizBlend, bf, 0xffffffff);
    g_gameContext->IASetInputLayout(nullptr);
    g_gameContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_gameContext->VSSetShader(g_depthVizVs, nullptr, 0);
    g_gameContext->PSSetShader(g_depthVizPs, nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = { g_dibrColorSrv, g_depthSrv };
    g_gameContext->PSSetShaderResources(0, 2, srvs);
    g_gameContext->PSSetSamplers(0, 1, &g_depthVizSampler);
    if (g_dibrParamsCb != nullptr)
    {
        D3D11_MAPPED_SUBRESOURCE mp = {};
        if (SUCCEEDED(g_gameContext->Map(g_dibrParamsCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp)))
        {
            FillDibrParamBuffer(static_cast<float*>(mp.pData), eyeScale);
            g_gameContext->Unmap(g_dibrParamsCb, 0);
        }
        g_gameContext->PSSetConstantBuffers(0, 1, &g_dibrParamsCb);
    }
    g_gameContext->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv[2] = {}; g_gameContext->PSSetShaderResources(0, 2, nullSrv);
    g_gameContext->OMSetRenderTargets(8, oldRtv, oldDsv);
    g_gameContext->RSSetViewports(oldVpN, oldVp);
    g_gameContext->RSSetState(oldRs);
    g_gameContext->OMSetDepthStencilState(oldDs, oldRef);
    g_gameContext->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);
    g_gameContext->IASetPrimitiveTopology(oldTopo);
    g_gameContext->IASetInputLayout(oldIl);
    g_gameContext->VSSetShader(oldVs, nullptr, 0);
    g_gameContext->PSSetShader(oldPs, nullptr, 0);
    g_gameContext->PSSetShaderResources(0, 2, oldSrv);
    g_gameContext->PSSetSamplers(0, 1, &oldSamp);
    for (auto*& rt : oldRtv) SafeRelease(rt);
    SafeRelease(oldDsv); SafeRelease(oldRs); SafeRelease(oldDs); SafeRelease(oldBlend); SafeRelease(oldIl);
    SafeRelease(oldVs); SafeRelease(oldPs); SafeRelease(oldSrv[0]); SafeRelease(oldSrv[1]); SafeRelease(oldSamp);
    return outTex;
}
// ============================ end DIBR functions ============================

void InstallContextHooks(ID3D11DeviceContext* context) noexcept
{
    if (context == nullptr ||
        (g_psSetShaderResourcesSlot != nullptr &&
         g_drawIndexedSlot != nullptr &&
         g_drawSlot != nullptr &&
         g_drawIndexedInstancedSlot != nullptr &&
         g_drawInstancedSlot != nullptr &&
         g_omSetRenderTargetsSlot != nullptr &&
         g_drawAutoSlot != nullptr &&
         g_drawIndexedInstancedIndirectSlot != nullptr &&
         g_drawInstancedIndirectSlot != nullptr &&
         g_dispatchSlot != nullptr &&
         g_dispatchIndirectSlot != nullptr &&
         g_rsSetViewportsSlot != nullptr &&
         g_rsSetScissorRectsSlot != nullptr &&
         g_copySubresourceRegionSlot != nullptr &&
         g_copyResourceSlot != nullptr &&
         g_resolveSubresourceSlot != nullptr &&
         g_executeCommandListSlot != nullptr))
    {
        return;
    }
    void** vtable = *reinterpret_cast<void***>(context);
    if (vtable == nullptr) return;
    g_immediateVtable = vtable;

    // MinHook the real draw FUNCTIONS (not just vtable slots) so cached-pointer callers are caught.
    if (!g_mhDrawInstalled.exchange(true))
    {
        MH_Initialize();
        void* realDI = vtable[kDrawIndexedSlot];
        void* realD = vtable[kDrawSlot];
        const bool diOk = (MH_CreateHook(realDI, reinterpret_cast<void*>(&MhDrawIndexed),
                                         reinterpret_cast<void**>(&g_mhRealDrawIndexed)) == MH_OK) &&
                          (MH_EnableHook(realDI) == MH_OK);
        const bool dOk = (MH_CreateHook(realD, reinterpret_cast<void*>(&MhDraw),
                                        reinterpret_cast<void**>(&g_mhRealDraw)) == MH_OK) &&
                         (MH_EnableHook(realD) == MH_OK);
        // MinHook DrawIndexedInstanced too (Scaleform text). Read the REAL fn before any vtable patch.
        void* realDII = vtable[kDrawIndexedInstancedSlot];
        const bool diiOk = (MH_CreateHook(realDII, reinterpret_cast<void*>(&MhDrawIndexedInstanced),
                                          reinterpret_cast<void**>(&g_mhRealDrawIndexedInstanced)) == MH_OK) &&
                           (MH_EnableHook(realDII) == MH_OK);
        g_drawIndexedInstancedSlot = &vtable[kDrawIndexedInstancedSlot];   // mark done (MinHooked, not patched)
        ME2VR::Log::Line("[ME3DISC] MinHook real DrawIndexed=" + HexPointer(realDI) + " (" + std::to_string(diOk) +
                         ") Draw=" + HexPointer(realD) + " (" + std::to_string(dOk) + ")" +
                         " DrawIndexedInstanced=" + HexPointer(realDII) + " (" + std::to_string(diiOk) + ")");

        // DIBR: MinHook the REAL ClearDepthStencilView too - depth clears flow through the same
        // deferred/RHI path as draws, so a vtable patch never fires. Read the real fn from the slot.
        if (!g_mhClearDSVInstalled)
        {
            void* realClearDSV = vtable[kClearDepthStencilViewSlot];
            const bool cOk = (MH_CreateHook(realClearDSV, reinterpret_cast<void*>(&MhClearDepthStencilView),
                                            reinterpret_cast<void**>(&g_mhRealClearDSV)) == MH_OK) &&
                             (MH_EnableHook(realClearDSV) == MH_OK);
            g_mhClearDSVInstalled = true;
            ME2VR::Log::Line("[DIBR] MinHook real ClearDepthStencilView=" + HexPointer(realClearDSV) +
                             " (" + std::to_string(cOk) + ")");
        }
    }

    if (g_psSetShaderResourcesSlot == nullptr &&
        PatchPointerSlot(&vtable[kPSSetShaderResourcesSlot],
                         reinterpret_cast<void*>(&PSSetShaderResourcesHook),
                         reinterpret_cast<void**>(&g_originalPSSetShaderResources),
                         "ID3D11DeviceContext::PSSetShaderResources"))
    {
        g_psSetShaderResourcesSlot = &vtable[kPSSetShaderResourcesSlot];
    }

    if (g_drawIndexedSlot == nullptr &&
        PatchPointerSlot(&vtable[kDrawIndexedSlot],
                         reinterpret_cast<void*>(&DrawIndexedHook),
                         reinterpret_cast<void**>(&g_originalDrawIndexed),
                         "ID3D11DeviceContext::DrawIndexed"))
    {
        g_drawIndexedSlot = &vtable[kDrawIndexedSlot];
    }

    if (g_drawSlot == nullptr &&
        PatchPointerSlot(&vtable[kDrawSlot],
                         reinterpret_cast<void*>(&DrawHook),
                         reinterpret_cast<void**>(&g_originalDraw),
                         "ID3D11DeviceContext::Draw"))
    {
        g_drawSlot = &vtable[kDrawSlot];
    }

    // DrawIndexedInstanced is MinHooked above (not vtable-patched) so it actually fires for the
    // cached-pointer Scaleform text path.

    if (g_drawInstancedSlot == nullptr &&
        PatchPointerSlot(&vtable[kDrawInstancedSlot],
                         reinterpret_cast<void*>(&DrawInstancedHook),
                         reinterpret_cast<void**>(&g_originalDrawInstanced),
                         "ID3D11DeviceContext::DrawInstanced"))
    {
        g_drawInstancedSlot = &vtable[kDrawInstancedSlot];
    }

    // [RTVDURABLE] MinHook the REAL OMSetRenderTargets instead of patching the vtable slot.
    // Ported from ME2 2026-07-28. A vtable-slot patch here is NOT durable: Meta evicts it at
    // XR-session start, and on EVERY runtime an alt-tab tears down device state and replaces the
    // vtable. Once detached this tracker stops populating, IsUiDrawNow rejects every draw, the UI
    // treatment never runs, and 16:9 UI drawn into a taller-than-wide eye view is stretched 2x
    // vertically. Re-patching the slot did not restore it. The draw and clear hooks are MinHook
    // inline hooks and survive both events untouched, so use the same mechanism: it patches the
    // function body, and replacing the vtable cannot detach it.
    if (!g_mhOMSetRTInstalled)
    {
        g_mhOMSetRTInstalled = true;
        void* realOMSetRT = vtable[kOMSetRenderTargetsSlot];
        const bool oOk = (MH_CreateHook(realOMSetRT, reinterpret_cast<void*>(&OMSetRenderTargetsHook),
                                        reinterpret_cast<void**>(&g_originalOMSetRenderTargets)) == MH_OK) &&
                         (MH_EnableHook(realOMSetRT) == MH_OK);
        ME2VR::Log::Line("[RTVDURABLE] MinHook real OMSetRenderTargets=" + HexPointer(realOMSetRT) +
                         " (" + std::to_string(oOk) + ")");
        if (oOk) g_omSetRenderTargetsSlot = &vtable[kOMSetRenderTargetsSlot];
    }

    // [EXITKILL] Ported from ME1/ME2. On Meta's PC runtime the game wedges on exit: once the game
    // stops presenting the mod never drives a clean xrEndSession, and the runtime DLL then hangs inside its
    // own DllMain(DETACH) during the ExitProcess unload chain. Intercept ExitProcess at ENTRY and
    // hard-terminate - no detach handlers, no CRT atexit, no hang. Safe because the ini is written
    // during play, never at exit. Harmless on runtimes that never hung.
    if (!g_exitKillInstalled)
    {
        g_exitKillInstalled = true;
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        void* ep = (k32 != nullptr) ? reinterpret_cast<void*>(GetProcAddress(k32, "ExitProcess")) : nullptr;
        const bool eOk = ep != nullptr &&
                         MH_CreateHook(ep, reinterpret_cast<void*>(&ExitProcessHook),
                                       reinterpret_cast<void**>(&g_origExitProcess)) == MH_OK &&
                         MH_EnableHook(ep) == MH_OK;
        ME2VR::Log::Line(std::string("[EXITKILL] ExitProcess entry hook installed (") + (eOk ? "1" : "0") + ")");
    }

    if (g_drawAutoSlot == nullptr &&
        PatchPointerSlot(&vtable[kDrawAutoSlot],
                         reinterpret_cast<void*>(&DrawAutoHook),
                         reinterpret_cast<void**>(&g_originalDrawAuto),
                         "ID3D11DeviceContext::DrawAuto"))
    {
        g_drawAutoSlot = &vtable[kDrawAutoSlot];
    }

    if (g_drawIndexedInstancedIndirectSlot == nullptr &&
        PatchPointerSlot(&vtable[kDrawIndexedInstancedIndirectSlot],
                         reinterpret_cast<void*>(&DrawIndexedInstancedIndirectHook),
                         reinterpret_cast<void**>(&g_originalDrawIndexedInstancedIndirect),
                         "ID3D11DeviceContext::DrawIndexedInstancedIndirect"))
    {
        g_drawIndexedInstancedIndirectSlot = &vtable[kDrawIndexedInstancedIndirectSlot];
    }

    if (g_drawInstancedIndirectSlot == nullptr &&
        PatchPointerSlot(&vtable[kDrawInstancedIndirectSlot],
                         reinterpret_cast<void*>(&DrawInstancedIndirectHook),
                         reinterpret_cast<void**>(&g_originalDrawInstancedIndirect),
                         "ID3D11DeviceContext::DrawInstancedIndirect"))
    {
        g_drawInstancedIndirectSlot = &vtable[kDrawInstancedIndirectSlot];
    }

    if (g_dispatchSlot == nullptr &&
        PatchPointerSlot(&vtable[kDispatchSlot],
                         reinterpret_cast<void*>(&DispatchHook),
                         reinterpret_cast<void**>(&g_originalDispatch),
                         "ID3D11DeviceContext::Dispatch"))
    {
        g_dispatchSlot = &vtable[kDispatchSlot];
    }

    if (g_dispatchIndirectSlot == nullptr &&
        PatchPointerSlot(&vtable[kDispatchIndirectSlot],
                         reinterpret_cast<void*>(&DispatchIndirectHook),
                         reinterpret_cast<void**>(&g_originalDispatchIndirect),
                         "ID3D11DeviceContext::DispatchIndirect"))
    {
        g_dispatchIndirectSlot = &vtable[kDispatchIndirectSlot];
    }

    if (g_rsSetViewportsSlot == nullptr &&
        PatchPointerSlot(&vtable[kRSSetViewportsSlot],
                         reinterpret_cast<void*>(&RSSetViewportsHook),
                         reinterpret_cast<void**>(&g_originalRSSetViewports),
                         "ID3D11DeviceContext::RSSetViewports"))
    {
        g_rsSetViewportsSlot = &vtable[kRSSetViewportsSlot];
    }

    if (g_rsSetScissorRectsSlot == nullptr &&
        PatchPointerSlot(&vtable[kRSSetScissorRectsSlot],
                         reinterpret_cast<void*>(&RSSetScissorRectsHook),
                         reinterpret_cast<void**>(&g_originalRSSetScissorRects),
                         "ID3D11DeviceContext::RSSetScissorRects"))
    {
        g_rsSetScissorRectsSlot = &vtable[kRSSetScissorRectsSlot];
    }

    if (g_copySubresourceRegionSlot == nullptr &&
        PatchPointerSlot(&vtable[kCopySubresourceRegionSlot],
                         reinterpret_cast<void*>(&CopySubresourceRegionHook),
                         reinterpret_cast<void**>(&g_originalCopySubresourceRegion),
                         "ID3D11DeviceContext::CopySubresourceRegion"))
    {
        g_copySubresourceRegionSlot = &vtable[kCopySubresourceRegionSlot];
    }

    if (g_copyResourceSlot == nullptr &&
        PatchPointerSlot(&vtable[kCopyResourceSlot],
                         reinterpret_cast<void*>(&CopyResourceHook),
                         reinterpret_cast<void**>(&g_originalCopyResource),
                         "ID3D11DeviceContext::CopyResource"))
    {
        g_copyResourceSlot = &vtable[kCopyResourceSlot];
    }

    if (g_resolveSubresourceSlot == nullptr &&
        PatchPointerSlot(&vtable[kResolveSubresourceSlot],
                         reinterpret_cast<void*>(&ResolveSubresourceHook),
                         reinterpret_cast<void**>(&g_originalResolveSubresource),
                         "ID3D11DeviceContext::ResolveSubresource"))
    {
        g_resolveSubresourceSlot = &vtable[kResolveSubresourceSlot];
    }

    if (g_executeCommandListSlot == nullptr &&
        PatchPointerSlot(&vtable[kExecuteCommandListSlot],
                         reinterpret_cast<void*>(&ExecuteCommandListHook),
                         reinterpret_cast<void**>(&g_originalExecuteCommandList),
                         "ID3D11DeviceContext::ExecuteCommandList"))
    {
        g_executeCommandListSlot = &vtable[kExecuteCommandListSlot];
    }
}

UINT HashShaderBytes(const void* data, SIZE_T bytes) noexcept
{
    if (data == nullptr || bytes == 0) return 0;
    const auto* p = static_cast<const unsigned char*>(data);
    UINT hash = 2166136261u;
    for (SIZE_T i = 0; i < bytes; ++i) hash = (hash ^ p[i]) * 16777619u;
    return hash;
}

bool CreateVrDistortionComposite(ID3D11Device* device, CreatePixelShaderFn createPs,
                                 ID3D11PixelShader** outShader) noexcept
{
    if (outShader != nullptr) *outShader = nullptr;
    if (device == nullptr || createPs == nullptr || outShader == nullptr) return false;

    const char* source =
        "cbuffer Globals : register(b0) { float4 SceneColorRect; };\n"
        "cbuffer VrDistortion : register(b13) { float2 VrScale; float2 VrPad; };\n"
        "Texture2D SceneColorTexture : register(t0);\n"
        "Texture2D AccumulatedDistortionTexture : register(t1);\n"
        "SamplerState SceneColorTextureSampler : register(s0);\n"
        "SamplerState AccumulatedDistortionTextureSampler : register(s1);\n"
        "struct PSIn { float2 uv : TEXCOORD0; };\n"
        "float4 main(PSIn i) : SV_Target {\n"
        "  float2 scale = all(VrScale > 0.01) ? VrScale : float2(1.0, 1.0);\n"
        "  float2 distUv = (i.uv - float2(0.5, 0.5)) / scale + float2(0.5, 0.5);\n"
        "  float4 d = float4(0.0, 0.0, 0.0, 0.0);\n"
        "  if (all(distUv >= float2(0.0, 0.0)) && all(distUv <= float2(1.0, 1.0)))\n"
        "    d = AccumulatedDistortionTexture.Sample(AccumulatedDistortionTextureSampler, distUv);\n"
        "  float2 uv = i.uv + (d.xy - d.zw) * float2(0.25, -0.25) * scale;\n"
        "  if (uv.x < SceneColorRect.x || uv.y < SceneColorRect.y || uv.x > SceneColorRect.z || uv.y > SceneColorRect.w) uv = i.uv;\n"
        "  return float4(SceneColorTexture.Sample(SceneColorTextureSampler, uv).rgb, 0.0);\n"
        "}\n";

    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (compiler == nullptr) compiler = LoadLibraryW(L"d3dcompiler_43.dll");
    if (compiler == nullptr) return false;
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                      LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    auto compile = reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile"));
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = compile != nullptr
        ? compile(source, std::strlen(source), "ME3VR_DistortionComposite", nullptr, nullptr,
                  "main", "ps_5_0", 0, 0, &blob, &errors)
        : E_FAIL;
    if (FAILED(hr) && errors != nullptr)
        ME2VR::Log::Line(std::string("[DISTFIX] shader compile failed: ") +
                         static_cast<const char*>(errors->GetBufferPointer()));
    if (SUCCEEDED(hr) && blob != nullptr)
        hr = createPs(device, blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, outShader);
    if (errors != nullptr) errors->Release();
    if (blob != nullptr) blob->Release();
    FreeLibrary(compiler);
    return SUCCEEDED(hr) && *outShader != nullptr;
}

HRESULT STDMETHODCALLTYPE CreatePixelShaderHook(ID3D11Device* device, const void* bytecode,
                                                 SIZE_T bytecodeLength,
                                                 ID3D11ClassLinkage* linkage,
                                                 ID3D11PixelShader** pixelShader) noexcept
{
    CreatePixelShaderFn original = g_mhRealCreatePixelShader;
    if (original == nullptr) return E_FAIL;

    const UINT hash = HashShaderBytes(bytecode, bytecodeLength);
    const HRESULT hr = original(device, bytecode, bytecodeLength, linkage, pixelShader);
    if (SUCCEEDED(hr) && hash == kDistortionCompositeHash &&
        pixelShader != nullptr && *pixelShader != nullptr)
    {
        ID3D11PixelShader* replacement = nullptr;
        if (CreateVrDistortionComposite(device, original, &replacement))
        {
            (*pixelShader)->Release();
            *pixelShader = replacement;
            g_distortionReplacementPs.store(replacement, std::memory_order_release);
            ME2VR::Log::Line(
                "[DISTFIX] exact 50414EE1 distortion composite replaced; raw-to-wide field reprojection active");
        }
        else
        {
            ME2VR::Log::Line(
                "[DISTFIX] exact 50414EE1 found but replacement creation FAILED; stock shader retained");
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CreateDeferredContextHook(ID3D11Device* device, UINT flags, ID3D11DeviceContext** ppCtx) noexcept
{
    HRESULT hr = g_originalCreateDeferredContext ? g_originalCreateDeferredContext(device, flags, ppCtx) : E_FAIL;
    if (SUCCEEDED(hr) && ppCtx != nullptr && *ppCtx != nullptr && !g_deferredLogged.exchange(true))
    {
        void** vt = *reinterpret_cast<void***>(*ppCtx);
        ME2VR::Log::Line(std::string("[ME3DISC] *** CreateDeferredContext CALLED ctx=") + HexPointer(*ppCtx) +
                         " vtable=" + HexPointer(vt) + " immediateVtable=" + HexPointer(g_immediateVtable) +
                         " sameVtable=" + std::to_string(vt == g_immediateVtable ? 1 : 0) + " ***");
    }
    return hr;
}

void CaptureSwapChainInfoOnce(IDXGISwapChain* swapChain) noexcept
{
    bool expected = false;
    if (!g_captured.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    if (swapChain == nullptr) return;

    if (g_gameSwapChain == nullptr) { g_gameSwapChain = swapChain; swapChain->AddRef(); }

    ME2VR::Log::Line("[ME3DISC] game swapchain " + HexPointer(swapChain));

    DXGI_SWAP_CHAIN_DESC scd = {};
    HRESULT hr = swapChain->GetDesc(&scd);
    if (SUCCEEDED(hr))
    {
        ME2VR::Log::Line("[ME3DISC] swapchain desc buffer=" + std::to_string(scd.BufferDesc.Width) + "x" +
                         std::to_string(scd.BufferDesc.Height) +
                         " fmt=" + std::to_string(static_cast<int>(scd.BufferDesc.Format)) +
                         " windowed=" + std::to_string(scd.Windowed ? 1 : 0));
    }

    ID3D11Texture2D* backBuffer = nullptr;
    hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    if (FAILED(hr) || backBuffer == nullptr)
    {
        ME2VR::Log::Line("[ME3DISC] GetBuffer(0) failed: " + HexHRESULT(hr));
        return;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    backBuffer->GetDesc(&desc);
    g_backbufferResourcePtr = reinterpret_cast<std::uintptr_t>(backBuffer);
    g_backbufferWidth = desc.Width;
    g_backbufferHeight = desc.Height;
    // DIBR: cache the immediate context (for the warp pass) + push the LIVE render res = full
    // backbuffer. The scene-depth capture gate matches THIS.
    if (g_gameContext == nullptr && g_gameDevice != nullptr) g_gameDevice->GetImmediateContext(&g_gameContext);
    g_sceneRenderW.store(desc.Width, std::memory_order_release);
    g_sceneRenderH.store(desc.Height, std::memory_order_release);
    ME2VR::Log::Line("[ME3DISC] backbuffer " + std::to_string(desc.Width) + "x" +
                     std::to_string(desc.Height) +
                     " fmt=" + std::to_string(static_cast<int>(desc.Format)) + " " + FormatName(desc.Format) +
                     " bind=0x" + std::to_string(desc.BindFlags));
    SafeRelease(backBuffer);

    ID3D11Device* device = nullptr;
    hr = swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device));
    if (SUCCEEDED(hr) && device != nullptr)
    {
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        ME2VR::Log::Line("[ME3DISC] ID3D11Device " + HexPointer(device) +
                         " immediateContext=" + HexPointer(context));
        // [HOST] name the GPU once. Field debugging has twice needed the user's adapter and had to
        // ask; the device already knows. AMD vs NVIDIA vs Intel changes driver threading and clear
        // behaviour, both of which the SFR capture path is sensitive to.
        {
            IDXGIDevice* dxgiDev = nullptr;
            if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev))) && dxgiDev)
            {
                IDXGIAdapter* adapter = nullptr;
                if (SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && adapter)
                {
                    DXGI_ADAPTER_DESC ad = {};
                    if (SUCCEEDED(adapter->GetDesc(&ad)))
                    {
                        char name[256] = {};
                        WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name), nullptr, nullptr);
                        char b[400] = {};
                        sprintf_s(b, "[HOST] gpu=%s vendor=0x%04X vram=%lluMB", name, ad.VendorId,
                                  static_cast<unsigned long long>(ad.DedicatedVideoMemory / (1024ull * 1024ull)));
                        ME2VR::Log::Line(b);
                    }
                    adapter->Release();
                }
                dxgiDev->Release();
            }
        }
        InstallContextHooks(context);
        ME2VR::Menu::Init(device, context, 900, 720);   // in-headset Insert menu
        if (g_gameDevice == nullptr) { g_gameDevice = device; device->AddRef(); }

        // Intercept only the exact native distortion composite identified by bytecode hash.
        if (!g_mhCreatePixelShaderInstalled.exchange(true))
        {
            MH_Initialize();
            void** dvt = *reinterpret_cast<void***>(device);
            void* realCreatePs = dvt != nullptr ? dvt[kCreatePixelShaderSlot] : nullptr;
            const bool psOk =
                realCreatePs != nullptr &&
                MH_CreateHook(realCreatePs, reinterpret_cast<void*>(&CreatePixelShaderHook),
                              reinterpret_cast<void**>(&g_mhRealCreatePixelShader)) == MH_OK &&
                MH_EnableHook(realCreatePs) == MH_OK;
            ME2VR::Log::Line("[DISTFIX] CreatePixelShader hook=" + std::to_string(psOk ? 1 : 0));
        }

        // Hook the device's CreateDeferredContext (vtable slot 27) to detect deferred-context rendering
        // - the likely home of the DrawIndexed calls (incl. the UI) that never hit the mod's immediate-context hooks.
        if (g_createDeferredContextSlot == nullptr)
        {
            void** dvt = *reinterpret_cast<void***>(device);
            if (dvt != nullptr &&
                PatchPointerSlot(&dvt[27], reinterpret_cast<void*>(&CreateDeferredContextHook),
                                 reinterpret_cast<void**>(&g_originalCreateDeferredContext),
                                 "ID3D11Device::CreateDeferredContext"))
            {
                g_createDeferredContextSlot = &dvt[27];
            }
        }
        SafeRelease(context);
        SafeRelease(device);
    }
}

void InstallPresentHook(IDXGISwapChain* swapChain) noexcept
{
    if (swapChain == nullptr || g_presentSlot != nullptr) return;
    void** vtable = *reinterpret_cast<void***>(swapChain);
    if (vtable == nullptr) return;
    if (PatchPointerSlot(&vtable[kPresentSlot],
                         reinterpret_cast<void*>(&PresentHook),
                         reinterpret_cast<void**>(&g_originalPresent),
                         "IDXGISwapChain::Present"))
    {
        g_presentSlot = &vtable[kPresentSlot];
    }
}

void OnSwapChainCreated(IDXGISwapChain* swapChain) noexcept
{
    if (swapChain == nullptr) return;
    std::lock_guard<std::mutex> lock(g_hookMutex);
    CaptureSwapChainInfoOnce(swapChain);
    InstallPresentHook(swapChain);
}

// [MIRRORTHROTTLE] present the flat mirror 1 frame in N while VR is on (see the block at the end of
// PresentHook). 8 => mirror refreshes ~8-15fps, enough to see the game is alive on the monitor, rare
// enough that the compositor never gates the render thread.
// Runtime-tunable via MirrorPresentEvery because the monitor is not always just a monitor: capturing
// footage of the mod records THIS window, and at 8 the recording is ~8-15fps however smooth the
// headset is. 1 = every frame (recording quality, but re-arms the DWM back-pressure cap described
// below), 2-4 = usable compromise. Clamped to 1..32; 0 in the ini means "leave the default".
constexpr uint64_t kMirrorPresentEveryDefault = 8;
std::atomic<uint64_t> g_mirrorPresentEvery{kMirrorPresentEveryDefault};

// [FRAMETIME] Present-to-present accounting, one line per 5s. Splits each frame into the game's
// own time (outside this hook), the runtime's xrWaitFrame block, and the mod's work in between,
// and counts frames over 12ms/20ms - so "spotty" reads as a number with an owner instead of a
// feeling. Also reports draws/present, log-write cost and the census walk time in the same line,
// because those are the three mod-side suspects for render-thread stalls.
struct FrameTimeWindow
{
    LARGE_INTEGER freq{}, prevEntry{}, winStart{};
    unsigned n = 0, over12 = 0, over20 = 0;
    double sumInt = 0, maxInt = 0, sumHook = 0, maxHook = 0, sumWait = 0, maxWait = 0, sumMod = 0, maxMod = 0, sumGame = 0, maxGame = 0;
};
FrameTimeWindow g_ft;

// [XSTATE] How many presentation-mode and VR-cine edges fired in this window, and the worst
// single edge's wall-clock cost (QPC, microseconds - edges are rare enough per window that a
// direct QueryPerformanceCounter pair per edge costs nothing, unlike a per-draw hook). Party/hub
// NPCs each trigger a short banter conversation, and every one is a MENU_MONO-enter ->
// GAMEPLAY_STEREO-return pair (sometimes a VrCineActive edge too) - so a burst of these lets a
// "random" stutter be read as "N transitions fired this window" instead of hand-correlating a
// hitch against the surrounding log by timestamp.
std::atomic<uint32_t> g_xsPresTrans{0}, g_xsCineTrans{0};
std::atomic<uint64_t> g_xsPresTransSumUs{0}, g_xsCineTransSumUs{0};
std::atomic<uint32_t> g_xsPresTransMaxUs{0}, g_xsCineTransMaxUs{0};

void XsNoteTransition(const LARGE_INTEGER& t0, const LARGE_INTEGER& t1, bool cine) noexcept
{
    LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
    const uint32_t us = static_cast<uint32_t>((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart);
    auto& count = cine ? g_xsCineTrans : g_xsPresTrans;
    auto& sum   = cine ? g_xsCineTransSumUs : g_xsPresTransSumUs;
    auto& mx    = cine ? g_xsCineTransMaxUs : g_xsPresTransMaxUs;
    count.fetch_add(1, std::memory_order_relaxed);
    sum.fetch_add(us, std::memory_order_relaxed);
    uint32_t m = mx.load(std::memory_order_relaxed);
    while (us > m && !mx.compare_exchange_weak(m, us, std::memory_order_relaxed)) {}
}

// [FRAMEOWNER] Who owns the frame when "game" is large: the render thread's CPU, the game thread's
// CPU, or the GPU. [FRAMETIME]'s "game" bucket is everything between the mod's hook returning and the
// next Present call - which is the render thread submitting draws, the render thread waiting on
// the game thread, AND the render thread waiting on the GPU (UE3 spins on last frame's occlusion
// queries, and with the mirror present throttled the D3D queue never back-pressures inside
// Present). Three numbers settle it per window:
//   rt%  = present-thread CPU busy over the window (GetThreadTimes; 15ms granularity is nothing
//          against a 5s window). ~100% with gpu well below 100 = draw submission / engine CPU.
//   gt%  = FViewportClient::Draw thread CPU busy. ~100% = the game thread is the wall.
//   gpu% = 3D-engine utilization from the OS's own GPU scheduler accounting (the number Task
//          Manager shows), total and this process. ~95-100% = pixels, not CPU. Note that the
//          render thread can read 100% busy while GPU-bound because the engine's query wait spins,
//          so gpu% is the tiebreaker, not rt%.
// The GPU counter comes from PDH ("GPU Engine" counterset, Win10 1709+). PDH is slow to open
// (hundreds of ms, perf-provider load) and must never run on the render thread, so a low-priority
// sampler thread collects every 5s and publishes atomics; the present hook only reads them.
// Loaded dynamically: no import dependency, no cost if pdh.dll is unavailable.
std::atomic<int> g_gpuUtilAll{-1};    // tenths of a percent, -1 = no sample yet / unavailable
std::atomic<int> g_gpuUtilOurs{-1};
std::atomic_bool g_gpuSamplerStarted{false};

typedef PDH_STATUS (WINAPI* PfnPdhOpenQueryW)(LPCWSTR, DWORD_PTR, PDH_HQUERY*);
typedef PDH_STATUS (WINAPI* PfnPdhAddEnglishCounterW)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*);
typedef PDH_STATUS (WINAPI* PfnPdhCollectQueryData)(PDH_HQUERY);
typedef PDH_STATUS (WINAPI* PfnPdhGetFormattedCounterArrayW)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PPDH_FMT_COUNTERVALUE_ITEM_W);

DWORD WINAPI GpuUtilSamplerThread(LPVOID) noexcept
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    HMODULE pdh = LoadLibraryW(L"pdh.dll");
    if (pdh == nullptr) return 0;
    const auto openQuery = reinterpret_cast<PfnPdhOpenQueryW>(GetProcAddress(pdh, "PdhOpenQueryW"));
    const auto addCounter = reinterpret_cast<PfnPdhAddEnglishCounterW>(GetProcAddress(pdh, "PdhAddEnglishCounterW"));
    const auto collect = reinterpret_cast<PfnPdhCollectQueryData>(GetProcAddress(pdh, "PdhCollectQueryData"));
    const auto getArray = reinterpret_cast<PfnPdhGetFormattedCounterArrayW>(GetProcAddress(pdh, "PdhGetFormattedCounterArrayW"));
    if (!openQuery || !addCounter || !collect || !getArray) return 0;
    PDH_HQUERY query = nullptr; PDH_HCOUNTER counter = nullptr;
    if (openQuery(nullptr, 0, &query) != ERROR_SUCCESS || query == nullptr) return 0;
    if (addCounter(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter) != ERROR_SUCCESS || counter == nullptr)
    {
        ME2VR::Log::Line("[FRAMEOWNER] GPU Engine counter unavailable - gpu% will read n/a");
        return 0;
    }
    wchar_t pidTag[32] = {};
    swprintf_s(pidTag, L"pid_%lu_", GetCurrentProcessId());
    std::string buf;
    collect(query);   // rate counter: the first sample only primes it
    for (;;)
    {
        Sleep(5000);
        if (collect(query) != ERROR_SUCCESS) continue;
        DWORD bytes = 0, items = 0;
        PDH_STATUS st = getArray(counter, PDH_FMT_DOUBLE, &bytes, &items, nullptr);
        if (st != static_cast<PDH_STATUS>(PDH_MORE_DATA) || bytes == 0) continue;
        buf.resize(bytes + 64);
        st = getArray(counter, PDH_FMT_DOUBLE, &bytes, &items,
                      reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(&buf[0]));
        if (st != ERROR_SUCCESS) continue;
        const auto* arr = reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        double all = 0.0, ours = 0.0;
        for (DWORD i = 0; i < items; ++i)
        {
            if (arr[i].szName == nullptr || arr[i].FmtValue.CStatus != 0) continue;
            if (wcsstr(arr[i].szName, L"engtype_3D") == nullptr) continue;
            all += arr[i].FmtValue.doubleValue;
            if (wcsstr(arr[i].szName, pidTag) != nullptr) ours += arr[i].FmtValue.doubleValue;
        }
        g_gpuUtilAll.store(static_cast<int>(all * 10.0 + 0.5), std::memory_order_relaxed);
        g_gpuUtilOurs.store(static_cast<int>(ours * 10.0 + 0.5), std::memory_order_relaxed);
    }
}

// [RTPROF] Poor man's sampling profiler on the render thread: every 4ms suspend it, read RIP,
// resume, attribute. Module split answers the only question that matters once the thread is known
// to be CPU-bound: is the time in the ENGINE (MassEffect3.exe: InitViews, occlusion, particle
// vertex fill, skinned submission), the D3D RUNTIME (d3d11.dll: per-draw validation and constant
// updates), the DRIVER (nvwgf2umx/nvldumdx: command building), a WAIT (ntdll/KernelBase: blocked, not
// computing), or US (this dxgi.dll). For exe samples a 4KB-bucket RVA histogram names the hot
// regions. Safety: nothing that can take a lock (module lookup, logging) runs while the target is
// suspended - suspend, copy RIP, resume, THEN classify. Cost: ~10-20us per sample = <0.5%.
std::atomic<unsigned long> g_rtProfThreadId{0};
struct RtProfMod { HMODULE h; std::uintptr_t lo, hi; unsigned count; char name[24]; };
struct RtProfBucket { std::uintptr_t rvaPage; unsigned count; };

std::uintptr_t RtProfModuleSize(std::uintptr_t base) noexcept   // SizeOfImage from the PE header, SEH-guarded
{
    __try
    {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        return nt->OptionalHeader.SizeOfImage;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0x4000000; }
}

DWORD WINAPI RtProfSamplerThread(LPVOID) noexcept
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);   // must preempt to sample fairly
    unsigned long tid = 0;
    while ((tid = g_rtProfThreadId.load(std::memory_order_relaxed)) == 0) Sleep(100);
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (th == nullptr) { ME2VR::Log::Line("[RTPROF] OpenThread failed - no render-thread profile"); return 0; }
    const std::uintptr_t exeBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&RtProfSamplerThread), &self);
    RtProfMod mods[12] = {}; int nMods = 0;
    RtProfBucket buckets[48] = {}; int nBuckets = 0; unsigned bucketDropped = 0;
    unsigned samples = 0, failed = 0;
    ULONGLONG lastLog = GetTickCount64();
    CONTEXT* ctx = static_cast<CONTEXT*>(_aligned_malloc(sizeof(CONTEXT), 16));
    if (ctx == nullptr) { CloseHandle(th); return 0; }
    for (;;)
    {
        Sleep(4);
        std::uintptr_t rip = 0;
        if (SuspendThread(th) != static_cast<DWORD>(-1))
        {
            memset(ctx, 0, sizeof(CONTEXT));
            ctx->ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(th, ctx)) rip = static_cast<std::uintptr_t>(ctx->Rip);
            ResumeThread(th);
        }
        if (rip == 0) { ++failed; continue; }
        ++samples;
        // classify (target is running again; lock-taking calls are safe here)
        int mi = -1;
        for (int i = 0; i < nMods; ++i) if (rip >= mods[i].lo && rip < mods[i].hi) { mi = i; break; }
        if (mi < 0)
        {
            HMODULE h = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(rip), &h) && h != nullptr)
            {
                for (int i = 0; i < nMods; ++i) if (mods[i].h == h) { mi = i; break; }
                if (mi < 0 && nMods < 12)
                {
                    RtProfMod& m = mods[nMods];
                    m.h = h;
                    // module extent from its PE header; fall back to a 64MB window
                    m.lo = reinterpret_cast<std::uintptr_t>(h);
                    m.hi = m.lo + RtProfModuleSize(m.lo);
                    wchar_t path[MAX_PATH] = {};
                    GetModuleFileNameW(h, path, MAX_PATH);
                    const wchar_t* base = wcsrchr(path, L'\\'); base = base ? base + 1 : path;
                    if (h == self) strcpy_s(m.name, "dxgi(ours)");
                    else { for (int c = 0; c < 23 && base[c]; ++c) m.name[c] = static_cast<char>(base[c] < 128 ? base[c] : '?'); }
                    m.count = 0;
                    mi = nMods++;
                }
            }
            else if (nMods < 12)
            {
                RtProfMod& m = mods[nMods]; m.h = nullptr; m.lo = rip & ~0xFFFFull; m.hi = m.lo + 0x10000; strcpy_s(m.name, "unknown"); m.count = 0; mi = nMods++;
            }
        }
        if (mi >= 0) ++mods[mi].count;
        if (exeBase != 0 && mi >= 0 && mods[mi].h == reinterpret_cast<HMODULE>(exeBase))
        {
            const std::uintptr_t page = (rip - exeBase) >> 12;
            int bi = -1;
            for (int i = 0; i < nBuckets; ++i) if (buckets[i].rvaPage == page) { bi = i; break; }
            if (bi < 0) { if (nBuckets < 48) { buckets[nBuckets].rvaPage = page; buckets[nBuckets].count = 0; bi = nBuckets++; } else ++bucketDropped; }
            if (bi >= 0) ++buckets[bi].count;
        }

        const ULONGLONG now = GetTickCount64();
        if (now - lastLog < 5000ull) continue;
        lastLog = now;
        if (samples == 0) continue;
        char b[700] = {};
        int off = sprintf_s(b, "[RTPROF] samples=%u", samples);
        // modules by count, descending
        for (int pass = 0; pass < nMods && off < 400; ++pass)
        {
            int best = -1; unsigned bc = 0;
            for (int i = 0; i < nMods; ++i) if (mods[i].count > bc) { bc = mods[i].count; best = i; }
            if (best < 0) break;
            off += sprintf_s(b + off, sizeof(b) - off, " %s=%.0f%%", mods[best].name, 100.0 * bc / samples);
            mods[best].count = 0;
        }
        off += sprintf_s(b + off, sizeof(b) - off, " | exe hot 4K pages:");
        for (int top = 0; top < 8 && off < 640; ++top)
        {
            int best = -1; unsigned bc = 0;
            for (int i = 0; i < nBuckets; ++i) if (buckets[i].count > bc) { bc = buckets[i].count; best = i; }
            if (best < 0 || bc == 0) break;
            off += sprintf_s(b + off, sizeof(b) - off, " +0x%llX000=%u", static_cast<unsigned long long>(buckets[best].rvaPage), bc);
            buckets[best].count = 0;
        }
        if (bucketDropped) off += sprintf_s(b + off, sizeof(b) - off, " dropped=%u", bucketDropped);
        if (failed) off += sprintf_s(b + off, sizeof(b) - off, " failed=%u", failed);
        ME2VR::Log::Line(b);
        samples = 0; failed = 0; nBuckets = 0; bucketDropped = 0;
    }
}

// Thread CPU time (user+kernel, 100ns units) for the present thread and the Draw thread, read at
// each window boundary so the line can print busy% over the window.
HANDLE g_ownerRtHandle = nullptr;     // present thread (render thread) - opened once from itself
HANDLE g_ownerGtHandle = nullptr;     // Draw thread - opened once its id is known
unsigned long g_ownerGtId = 0;
ULONGLONG g_ownerRtPrev = 0, g_ownerGtPrev = 0;

ULONGLONG ThreadCpu100ns(HANDLE h) noexcept
{
    if (h == nullptr) return 0;
    FILETIME c, e, k, u;
    if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
    const ULONGLONG kk = (static_cast<ULONGLONG>(k.dwHighDateTime) << 32) | k.dwLowDateTime;
    const ULONGLONG uu = (static_cast<ULONGLONG>(u.dwHighDateTime) << 32) | u.dwLowDateTime;
    return kk + uu;
}

// [FLICKER] window accumulator (the three per-present counters are declared up with the SFR
// per-present globals because the draw hooks that increment them are defined earlier in this file).
struct FlickerWindow
{
    unsigned long long startMs = 0;
    unsigned presents = 0, stereoP = 0, capMiss = 0, maxGap = 0;
    unsigned uiMin = 0xFFFFFFFFu, uiMax = 0, uiSum = 0;
    unsigned mirMin = 0xFFFFFFFFu, mirMax = 0, mirSum = 0;
    unsigned reoSum = 0, reoWorst = 0;   // right-eye-only UI draws (ui - mir): total and worst single present
    // [FLICKER2]
    unsigned passMin = 0xFFFFFFFFu, passMax = 0, passSum = 0, passNot2 = 0;
    float uiSclMin = 1e9f, uiSclMax = -1e9f;
    unsigned capMinIdx = 0xFFFFFFFFu, capMaxIdx = 0;   // [CAPJITTER]
};
FlickerWindow g_flk;

void FrameTimeAccount(const LARGE_INTEGER& entry, const LARGE_INTEGER& hookEnd, unsigned waitUs) noexcept
{
    if (g_ft.freq.QuadPart == 0) QueryPerformanceFrequency(&g_ft.freq);
    const double f = static_cast<double>(g_ft.freq.QuadPart);
    if (g_ft.prevEntry.QuadPart != 0)
    {
        const double intMs  = static_cast<double>(entry.QuadPart - g_ft.prevEntry.QuadPart) * 1000.0 / f;
        const double hookMs = static_cast<double>(hookEnd.QuadPart - entry.QuadPart) * 1000.0 / f;
        const double waitMs = static_cast<double>(waitUs) / 1000.0;
        const double modMs  = (hookMs > waitMs) ? (hookMs - waitMs) : 0.0;
        const double gameMs = (intMs > hookMs) ? (intMs - hookMs) : 0.0;
        ++g_ft.n;
        g_ft.sumInt += intMs;   if (intMs > g_ft.maxInt) g_ft.maxInt = intMs;
        g_ft.sumHook += hookMs; if (hookMs > g_ft.maxHook) g_ft.maxHook = hookMs;
        g_ft.sumWait += waitMs; if (waitMs > g_ft.maxWait) g_ft.maxWait = waitMs;
        g_ft.sumMod += modMs;   if (modMs > g_ft.maxMod) g_ft.maxMod = modMs;
        g_ft.sumGame += gameMs; if (gameMs > g_ft.maxGame) g_ft.maxGame = gameMs;
        if (intMs > 12.0) ++g_ft.over12;
        if (intMs > 20.0) ++g_ft.over20;
    }
    g_ft.prevEntry = entry;
    if (g_ft.winStart.QuadPart == 0)
    {
        g_ft.winStart = entry;
        // [FRAMEOWNER] first present: this is the render thread. Open handles once, start the sampler.
        if (g_ownerRtHandle == nullptr)
            g_ownerRtHandle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
        g_ownerRtPrev = ThreadCpu100ns(g_ownerRtHandle);
        if (!g_gpuSamplerStarted.exchange(true))
        {
            // [LOGQUIET 2026-08-22] The PDH GPU sampler exists only to fill in the [FRAMEOWNER]
            // telemetry line, which no longer prints in a play session. No consumer, no thread:
            // it opened a PDH query and enumerated every GPU engine counter on a timer for the
            // whole run. Diagnostics=1 brings both back together.
            if (ME2VR::Log::DiagnosticsOn())
            {
                HANDLE t = CreateThread(nullptr, 0, GpuUtilSamplerThread, nullptr, 0, nullptr);
                if (t != nullptr) CloseHandle(t);
            }
            // [RTPROF] answered 2026-08-19 (Citadel: d3d11.dll 52%, exe 21%, ntdll 14%, driver 5% =
            // API-bound draw submission doubled by the second pass). Suspending the render thread
            // 250x/s is not something a play session should pay for; Diagnostics=1 brings it back.
            if (ME2VR::Log::DiagnosticsOn())
            {
                g_rtProfThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
                HANDLE p = CreateThread(nullptr, 0, RtProfSamplerThread, nullptr, 0, nullptr);
                if (p != nullptr) CloseHandle(p);
            }
        }
        return;
    }
    const double winSec = static_cast<double>(entry.QuadPart - g_ft.winStart.QuadPart) / f;
    if (winSec < 5.0 || g_ft.n == 0) return;

    const uint64_t draws = g_ftDraws.exchange(0, std::memory_order_relaxed);
    unsigned logLines = 0, logMaxUs = 0;
    ME2VR::Log::TakeStats(&logLines, &logMaxUs);
    unsigned cenFastUs = 0, cenSlowUs = 0; int cenObjs = 0;
    ME2VR::EngineProbe::TakeCensusStats(&cenFastUs, &cenSlowUs, &cenObjs);
    const double inv = 1.0 / static_cast<double>(g_ft.n);
    char b[400] = {};
    sprintf_s(b, "[FRAMETIME] presents=%u avg=%.2fms max=%.2f >12ms=%u >20ms=%u | game=%.2f/%.2f wait=%.2f/%.2f mod=%.2f/%.2f (avg/max ms) | draws/present=%llu | log lines=%u maxWrite=%.2fms | census fast=%.1fms slow=%.1fms objs=%d",
              g_ft.n, g_ft.sumInt * inv, g_ft.maxInt, g_ft.over12, g_ft.over20,
              g_ft.sumGame * inv, g_ft.maxGame, g_ft.sumWait * inv, g_ft.maxWait, g_ft.sumMod * inv, g_ft.maxMod,
              static_cast<unsigned long long>(draws / g_ft.n),
              logLines, static_cast<double>(logMaxUs) / 1000.0,
              static_cast<double>(cenFastUs) / 1000.0, static_cast<double>(cenSlowUs) / 1000.0, cenObjs);
    ME2VR::Log::Line(b);

    // [XSTATE] presentation/cine transitions this window. A banter-heavy scene (party, hub crowd)
    // fires a MENU_MONO-enter/GAMEPLAY_STEREO-return pair (and often a VrCineActive edge) every
    // time a conversation starts or ends; a burst of these lining up with [FRAMETIME]'s max for
    // the same window is the signature of "stutters every time an NPC starts talking", as opposed
    // to the steady, area-based CPU-submission cost (that one has ZERO transitions and just runs
    // more draws/present). Silent when nothing transitioned - most 5s windows in a corridor won't.
    {
        const uint32_t pn = g_xsPresTrans.exchange(0, std::memory_order_relaxed);
        const uint64_t pSum = g_xsPresTransSumUs.exchange(0, std::memory_order_relaxed);
        const uint32_t pMax = g_xsPresTransMaxUs.exchange(0, std::memory_order_relaxed);
        const uint32_t cn = g_xsCineTrans.exchange(0, std::memory_order_relaxed);
        const uint64_t cSum = g_xsCineTransSumUs.exchange(0, std::memory_order_relaxed);
        const uint32_t cMax = g_xsCineTransMaxUs.exchange(0, std::memory_order_relaxed);
        if (pn != 0 || cn != 0)
        {
            char x[220] = {};
            sprintf_s(x, "[XSTATE] pres=%u avg=%.2fms max=%.2fms | cine=%u avg=%.2fms max=%.2fms",
                      pn, pn ? (static_cast<double>(pSum) / pn / 1000.0) : 0.0, pMax / 1000.0,
                      cn, cn ? (static_cast<double>(cSum) / cn / 1000.0) : 0.0, cMax / 1000.0);
            ME2VR::Log::Line(x);
        }
    }

    // [XRSUBMIT] RunFrame's own wall time this window, with the already-known wait time (g_ft's
    // windowed sum/max, same window) subtracted out - what's left is pacing + per-eye texture
    // copies + the swapchain acquire/wait/release cycle + xrEndFrame. A big number here that ISN'T
    // matched by a HOOKCOST or XSTATE spike points outside the mod's own code: the OpenXR runtime's
    // compositor (e.g. Virtual Desktop's encoder/network link stalling on a visually busy frame).
    {
        unsigned xrCount = 0, xrSumUs = 0, xrMaxUs = 0;
        ME2VR::Me2Xr::TakeXrSubmitStats(&xrCount, &xrSumUs, &xrMaxUs);
        if (xrCount != 0)
        {
            const double waitSumUs = g_ft.sumWait * 1000.0;
            const double waitMaxUs = g_ft.maxWait * 1000.0;
            const double subSumUs = (static_cast<double>(xrSumUs) > waitSumUs) ? static_cast<double>(xrSumUs) - waitSumUs : 0.0;
            const double subMaxUs = (static_cast<double>(xrMaxUs) > waitMaxUs) ? static_cast<double>(xrMaxUs) - waitMaxUs : 0.0;
            char y[200] = {};
            sprintf_s(y, "[XRSUBMIT] n=%u avg=%.2fms max=%.2fms (wait subtracted)",
                      xrCount, subSumUs / xrCount / 1000.0, subMaxUs / 1000.0);
            ME2VR::Log::Line(y);
        }
    }

    // [SFRHEALTH 2026-08-20] Is stereo actually stereo? A v1.0 report described "flat on Stereo", and
    // that symptom has three completely different causes which this line separates in one read:
    //   replays=0        -> the FViewportClient::Draw replay is NOT running, so only ONE pass renders
    //                       and both eyes receive the same image. Almost always a game-exe mismatch:
    //                       the mod hooks hardcoded MassEffect3.exe RVAs, so a different build (EA App
    //                       vs Steam, a patch, a repacked exe) lands the hook on wrong bytes. Check
    //                       the [HOST] exe fingerprint at the top of the log.
    //   replays>0, caps=0-> the second pass renders but the left-eye capture never arms, so the submit
    //                       falls back to the live backbuffer for both eyes. A capture/clear-pattern
    //                       problem, not a hook problem.
    //   both >0          -> geometry is genuinely stereo; if it still LOOKS flat the cause is
    //                       presentation (check [PRESENTATION] submit=) or eye separation being ~0.
    // Always on, gameplay-stereo only, one line per window.
    if (ME2VR::CalcViewHook::GetVrMode() == 4 && !g_menuPresentation.load(std::memory_order_acquire))
    {
        static uint64_t s_prevReplays = 0, s_prevCalcViews = 0, s_prevCaps = 0;
        const uint64_t rp = ME2VR::CalcViewHook::GetSfrReplays();
        const uint64_t cv = ME2VR::CalcViewHook::GetSfrCalcViews();
        const uint64_t cp = g_sfrPass0Caps.load(std::memory_order_relaxed);
        const uint64_t dRp = (rp > s_prevReplays) ? rp - s_prevReplays : 0;
        const uint64_t dCv = (cv > s_prevCalcViews) ? cv - s_prevCalcViews : 0;
        const uint64_t dCp = (cp > s_prevCaps) ? cp - s_prevCaps : 0;
        s_prevReplays = rp; s_prevCalcViews = cv; s_prevCaps = cp;
        char h[240] = {};
        sprintf_s(h, "[SFRHEALTH] replays=%llu calcViews=%llu captures=%llu per window (presents=%u)%s",
                  static_cast<unsigned long long>(dRp), static_cast<unsigned long long>(dCv),
                  static_cast<unsigned long long>(dCp), g_ft.n,
                  (dRp == 0) ? "  <-- NO SECOND PASS: both eyes identical, check [HOST] exe fingerprint"
                             : ((dCp == 0) ? "  <-- NO CAPTURES: left eye falling back to live frame" : ""));
        // [LOGQUIET 2026-08-22] Silent while stereo is healthy, loud the moment it is not.
        // Flat stereo is the number one field report and this line is the only proof of it,
        // so it has to survive a normal play session - but a healthy window every 5s is pure
        // noise. A window with no replays or no captures always prints; a good one needs
        // Diagnostics. [SFRCLEAR] below was already failure-only.
        const bool sfrUnhealthy = (dRp == 0 || dCp == 0);
        if (sfrUnhealthy || ME2VR::Log::DiagnosticsOn()) ME2VR::Log::Line(h);

        // [SFRCLEAR] Only when captures are failing: say WHY the clear gate never accepted anything.
        //   sceneSized=0 with a biggest-depth size != the backbuffer -> the scene is not rendering at
        //     backbuffer size. DynamicResolution in GamerSettings.ini is the usual cause.
        //   sizeOkButMsaa>0 -> MSAA (MaxMultisamples) was the blocker on the old build.
        //   sceneSized>0 but bbWrittenAtClear=0 -> the size gate passes but the backbuffer write is
        //     never seen before the clear, so there is no valid moment to snapshot pass 0.
        if (dCp == 0 && dRp != 0)
        {
            const uint32_t cl = g_scClears.exchange(0, std::memory_order_relaxed);
            const uint32_t ss = g_scSceneSized.exchange(0, std::memory_order_relaxed);
            const uint32_t mb = g_scSizeOkMsaaBad.exchange(0, std::memory_order_relaxed);
            const uint32_t bw = g_scBbWrittenAtClear.exchange(0, std::memory_order_relaxed);
            char c[260] = {};
            sprintf_s(c, "[SFRCLEAR] clears=%u sceneSized=%u bbWrittenAtClear=%u sizeOkButMsaa=%u | "
                         "biggest depth seen %ux%u samples=%u | backbuffer %ux%u",
                      cl, ss, bw, mb,
                      g_scBigW.load(std::memory_order_relaxed), g_scBigH.load(std::memory_order_relaxed),
                      g_scBigSamples.load(std::memory_order_relaxed),
                      g_backbufferWidth, g_backbufferHeight);
            ME2VR::Log::Line(c);
            // [BBARM] which rejection point in the arming chain ate every draw this window.
            char a[360] = {};
            sprintf_s(a, "[BBARM] bbPtr=%u ctxN=%d calls=%u noCtxEntry=%u prefilterPass=%u "
                         "confirmed=%u uiRejected=%u armedDraw=%u armedCopy=%u sizeMatchOther=%u otherPtr=%p bbPtrVal=%p",
                      (g_backbufferResourcePtr != 0) ? 1u : 0u,
                      g_ctxRtvCount.load(std::memory_order_relaxed),
                      g_bbdCalls.exchange(0, std::memory_order_relaxed),
                      g_bbdNoCtxEntry.exchange(0, std::memory_order_relaxed),
                      g_bbdPrefilterPass.exchange(0, std::memory_order_relaxed),
                      g_bbdConfirmed.exchange(0, std::memory_order_relaxed),
                      g_bbdUiRejected.exchange(0, std::memory_order_relaxed),
                      g_bbdArmedDraw.exchange(0, std::memory_order_relaxed),
                      g_bbdArmedCopy.exchange(0, std::memory_order_relaxed),
                      g_bbdSizeMatchOther.exchange(0, std::memory_order_relaxed),
                      reinterpret_cast<void*>(g_bbdOtherPtr.load(std::memory_order_relaxed)),
                      reinterpret_cast<void*>(g_backbufferResourcePtr));
            ME2VR::Log::Line(a);
        }
    }

    // [POSEHB] pose-path execution proof: pushes should track presents in stereo, seeds should be
    // rare (recenters only), freezes = presents that re-used the previous eye pair. pushes=0 in a
    // stereo window means DriveHeadLook is not running - which is a finding, not a formality.
    {
        unsigned pp = 0, ps = 0, pf = 0;
        ME2VR::Me2Xr::TakePoseStats(&pp, &ps, &pf);
        if (pp != 0 || ps != 0 || pf != 0)
        {
            char z[120] = {};
            sprintf_s(z, "[POSEHB] pushes=%u seeds=%u freezes=%u", pp, ps, pf);
            ME2VR::Log::Line(z);
        }
    }

    // [FRAMEOWNER] companion line: who was the wall this window. HOW TO READ:
    //   gpu ~95-100%            -> pixels. Render resolution is the lever; CPU work is irrelevant.
    //   gpu well below, rt ~100 -> render-thread CPU (draw submission / engine per-view work).
    //   gpu well below, gt ~100 -> game-thread CPU. p1 >> p0 there means the replay itself costs
    //                              the game thread, not the render thread.
    //   p0/p1 = wall time of pass 0 / pass 1 inside FViewportClient::Draw as the game thread sees it.
    {
        const unsigned long gtId = ME2VR::CalcViewHook::GetDrawThreadId();
        if (gtId != 0 && gtId != g_ownerGtId)
        {
            if (g_ownerGtHandle != nullptr) CloseHandle(g_ownerGtHandle);
            g_ownerGtHandle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, gtId);
            g_ownerGtId = gtId;
            g_ownerGtPrev = ThreadCpu100ns(g_ownerGtHandle);
        }
        const ULONGLONG rtNow = ThreadCpu100ns(g_ownerRtHandle);
        const ULONGLONG gtNow = ThreadCpu100ns(g_ownerGtHandle);
        const double winMs = winSec * 1000.0;
        const double rtPct = (rtNow >= g_ownerRtPrev && g_ownerRtHandle) ? static_cast<double>(rtNow - g_ownerRtPrev) / 10000.0 / winMs * 100.0 : -1.0;
        const double gtPct = (gtNow >= g_ownerGtPrev && g_ownerGtHandle) ? static_cast<double>(gtNow - g_ownerGtPrev) / 10000.0 / winMs * 100.0 : -1.0;
        g_ownerRtPrev = rtNow; g_ownerGtPrev = gtNow;
        unsigned dpN = 0, p0Avg = 0, p0Max = 0, p1Avg = 0, p1Max = 0;
        ME2VR::CalcViewHook::TakeDrawPassStats(&dpN, &p0Avg, &p0Max, &p1Avg, &p1Max);
        const int gAll = g_gpuUtilAll.load(std::memory_order_relaxed);
        const int gOurs = g_gpuUtilOurs.load(std::memory_order_relaxed);
        // [HOOKCOST] TSC calibrated against this window's QPC span.
        static uint64_t s_tscStart = 0;
        const uint64_t tscNow = __rdtsc();
        const double cyclesPerMs = (s_tscStart != 0) ? static_cast<double>(tscNow - s_tscStart) / winMs : 0.0;
        s_tscStart = tscNow;
        const uint64_t hcDraw = g_hcDrawCyc.exchange(0, std::memory_order_relaxed);
        const uint64_t hcBind = g_hcBindCyc.exchange(0, std::memory_order_relaxed);
        const uint64_t hcUiq = g_hcUiQueries.exchange(0, std::memory_order_relaxed);
        const double hookDrawMs = (cyclesPerMs > 0) ? static_cast<double>(hcDraw) / cyclesPerMs * inv : -1.0;
        const double hookBindMs = (cyclesPerMs > 0) ? static_cast<double>(hcBind) / cyclesPerMs * inv : -1.0;
        char o[400] = {};
        int off = sprintf_s(o, "[FRAMEOWNER] rt=%.0f%% gt=%.0f%% | gpu3d=", rtPct, gtPct);
        if (gAll >= 0) off += sprintf_s(o + off, sizeof(o) - off, "%.1f%% (this process %.1f%%)", gAll / 10.0, gOurs / 10.0);
        else           off += sprintf_s(o + off, sizeof(o) - off, "n/a");
        off += sprintf_s(o + off, sizeof(o) - off, " | hooks/present: draw=%.2fms bind=%.2fms uiq=%llu",
                         hookDrawMs, hookBindMs, static_cast<unsigned long long>(hcUiq / g_ft.n));
        sprintf_s(o + off, sizeof(o) - off, " | draws=%u p0=%.2f/%.2fms p1=%.2f/%.2fms (avg/max, game thread)",
                  dpN, p0Avg / 1000.0, p0Max / 1000.0, p1Avg / 1000.0, p1Max / 1000.0);
        ME2VR::Log::Line(o);

        // [RTTARGETS] draws per present by render target (w x h fmt), top 8, then reset.
        {
            uint64_t keys[kRtHistN]; uint32_t counts[kRtHistN];
            for (int i = 0; i < kRtHistN; ++i)
            {
                keys[i] = g_rtHistKey[i].exchange(0, std::memory_order_relaxed);
                counts[i] = g_rtHistCount[i].exchange(0, std::memory_order_relaxed);
            }
            const uint32_t dropped = g_rtHistDropped.exchange(0, std::memory_order_relaxed);
            char h[400] = {};
            int hoff = sprintf_s(h, "[RTTARGETS] draws/present by target:");
            for (int top = 0; top < 8 && hoff < 340; ++top)
            {
                int best = -1; uint32_t bc = 0;
                for (int i = 0; i < kRtHistN; ++i) if (keys[i] != 0 && counts[i] > bc) { bc = counts[i]; best = i; }
                if (best < 0) break;
                const unsigned w = static_cast<unsigned>((keys[best] >> 40) & 0xFFFFF);
                const unsigned hh = static_cast<unsigned>((keys[best] >> 16) & 0xFFFFFF);
                const unsigned fmt = static_cast<unsigned>(keys[best] & 0xFFFF);
                hoff += sprintf_s(h + hoff, sizeof(h) - hoff, " %ux%u/f%u=%u", w, hh, fmt, static_cast<unsigned>(bc / g_ft.n));
                counts[best] = 0;
            }
            if (dropped) sprintf_s(h + hoff, sizeof(h) - hoff, " dropped=%u", dropped);
            ME2VR::Log::Line(h);
        }
    }
    const LARGE_INTEGER keepFreq = g_ft.freq;
    g_ft = FrameTimeWindow{};
    g_ft.freq = keepFreq;
    g_ft.prevEntry = entry;
    g_ft.winStart = entry;
}

HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) noexcept
{
    LARGE_INTEGER ftEntry; QueryPerformanceCounter(&ftEntry);   // [FRAMETIME]
    const auto n = g_presentCount.fetch_add(1, std::memory_order_relaxed) + 1;
    // [BBREFRESH] The backbuffer resource pointer was captured once, at the first Present. If the
    // game recreates or resizes the swapchain buffers afterwards (applying a resolution, leaving a
    // menu, alt-tab), that pointer goes stale and the pass-0 arming test can never match again -
    // stereo then renders flat. Re-read buffer 0 every present and follow it.
    if (swapChain != nullptr && swapChain == g_gameSwapChain)
    {
        ID3D11Texture2D* bbNow = nullptr;
        if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bbNow))) && bbNow != nullptr)
        {
            const std::uintptr_t p = reinterpret_cast<std::uintptr_t>(bbNow);
            if (p != g_backbufferResourcePtr)
            {
                D3D11_TEXTURE2D_DESC bd = {};
                bbNow->GetDesc(&bd);
                char l[200];
                std::snprintf(l, sizeof(l), "[BBREFRESH] backbuffer resource changed %p -> %p (%ux%u, was %ux%u) at present %llu",
                              reinterpret_cast<void*>(g_backbufferResourcePtr), reinterpret_cast<void*>(p),
                              bd.Width, bd.Height, g_backbufferWidth, g_backbufferHeight,
                              static_cast<unsigned long long>(n));
                ME2VR::Log::Line(l);
                g_backbufferResourcePtr = p;
                g_backbufferWidth = bd.Width;
                g_backbufferHeight = bd.Height;
            }
            bbNow->Release();
        }
    }
    if (n == 1)
    {
        ME2VR::Log::Line("[ME3DISC] first Present seen. thread=" + std::to_string(GetCurrentThreadId()));
    }
    else if (n == 600)
    {
        ME2VR::Log::Line("[ME3DISC] 600 Presents seen; hook stable.");
    }
    // [SFR] per-present snapshot bookkeeping. g_sfrPass0Tex KEEPS its contents across the reset (the
    // submit reads it); only the "captured this present" arming counter is cleared.
    if (SfrModeActive() && (n % 600) == 0)
        ME2VR::Log::Line("[SFRDIAG] sceneClears/present=" + std::to_string(g_sfrClearsThisPresent) +
                         " totalCaps=" + std::to_string(g_sfrPass0Caps.load(std::memory_order_relaxed)));
    // [CINEMAP] PROBE: emit the map of the cine present that just ended, then reset it. Reading guide
    // is in MENU_MONO_HANDOFF. Nothing here feeds behaviour - this build is deliberately observation
    // only, because seven behaviour changes have now been shipped against an assumed structure.
    if (g_cineMapLen > 0)
    {
        bool halves = false;
        if (g_cineMapLen >= 4 && (g_cineMapLen % 2) == 0)
        {
            const int half = g_cineMapLen / 2;
            halves = true;
            for (int i = 0; i < half; ++i)
                if (g_cineMap[i].depth != g_cineMap[i + half].depth) { halves = false; break; }
        }
        int bbAt = -1, replayAt = -1;
        for (int i = 0; i < g_cineMapLen; ++i)
        {
            if (bbAt < 0 && g_cineMap[i].bbWritten) bbAt = i;
            if (replayAt < 0 && g_cineMap[i].replay) replayAt = i;
        }
        // Log on any structural change, else once a second: a shot cut is the interesting moment.
        static int s_prevN = -1, s_prevCap = -2;
        static bool s_prevHalves = false;
        static unsigned long long s_lastMapMs = 0;
        const unsigned long long nowMs = GetTickCount64();
        const bool changed = (g_cineMapLen != s_prevN) || (halves != s_prevHalves) ||
                             (g_cineCapturedAtIndex != s_prevCap);
        if (changed || nowMs - s_lastMapMs >= 1000ull)
        {
            s_prevN = g_cineMapLen; s_prevHalves = halves; s_prevCap = g_cineCapturedAtIndex;
            s_lastMapMs = nowMs;
            char b[640] = {};
            int off = sprintf_s(b, "[CINEMAP] n=%d halves=%d cap@%d bbAt=%d replayAt=%d defer=%d tid=%lu %ux%u |",
                                g_cineMapLen, halves ? 1 : 0, g_cineCapturedAtIndex, bbAt, replayAt,
                                g_cineMap[0].deferred ? 1 : 0, g_cineClearThreadId,
                                g_cineMap[0].w, g_cineMap[0].h);
            for (int i = 0; i < g_cineMapLen && off > 0 && off < 560; ++i)
                off += sprintf_s(b + off, sizeof(b) - off, " %c%05llx/%04llx",
                                 (i == g_cineCapturedAtIndex) ? '*' : ' ',
                                 static_cast<unsigned long long>(g_cineMap[i].depth & 0xFFFFFull),
                                 static_cast<unsigned long long>(g_cineMap[i].rtv & 0xFFFFull));
            ME2VR::Log::Line(b);
        }
        g_cineMapLen = 0;
        g_cineCapturedAtIndex = -1;
    }
    // [FLICKER] fold this present into the 1s window, emit on the boundary (Diagnostics=1 only).
    if (ME2VR::Log::DiagnosticsOn())
    {
        const bool sfr = SfrModeActive();
        g_flkPresentsSinceCap = g_sfrCapturedThisPresent ? 0 : (g_flkPresentsSinceCap + 1);
        ++g_flk.presents;
        if (sfr)
        {
            ++g_flk.stereoP;
            if (!g_sfrCapturedThisPresent) ++g_flk.capMiss;
            if (g_flkPresentsSinceCap > g_flk.maxGap) g_flk.maxGap = g_flkPresentsSinceCap;
            const unsigned ui = g_flkUiThisPresent, mir = g_flkMirrorThisPresent;
            const unsigned reo = (ui > mir) ? (ui - mir) : 0u;   // reached right eye but not left
            g_flk.uiSum += ui; if (ui < g_flk.uiMin) g_flk.uiMin = ui; if (ui > g_flk.uiMax) g_flk.uiMax = ui;
            g_flk.mirSum += mir; if (mir < g_flk.mirMin) g_flk.mirMin = mir; if (mir > g_flk.mirMax) g_flk.mirMax = mir;
            g_flk.reoSum += reo; if (reo > g_flk.reoWorst) g_flk.reoWorst = reo;
            // [FLICKER2] (a) did BOTH scene passes run this present?
            const unsigned pz = g_flkPassesThisPresent;
            g_flk.passSum += pz;
            if (pz < g_flk.passMin) g_flk.passMin = pz;
            if (pz > g_flk.passMax) g_flk.passMax = pz;
            if (pz != 2) ++g_flk.passNot2;
            // [FLICKER2] (b) did the UI viewport scale move this present?
            if (g_flkUiScaleX > 0.0f)
            {
                if (g_flkUiScaleX < g_flk.uiSclMin) g_flk.uiSclMin = g_flkUiScaleX;
                if (g_flkUiScaleX > g_flk.uiSclMax) g_flk.uiSclMax = g_flkUiScaleX;
            }
            // [CAPJITTER] only meaningful on presents that actually captured
            if (g_sfrCapturedThisPresent)
            {
                if (g_flkCapAtClear < g_flk.capMinIdx) g_flk.capMinIdx = g_flkCapAtClear;
                if (g_flkCapAtClear > g_flk.capMaxIdx) g_flk.capMaxIdx = g_flkCapAtClear;
            }
        }
        const unsigned long long nowMs = GetTickCount64();
        if (g_flk.startMs == 0) g_flk.startMs = nowMs;
        else if (nowMs - g_flk.startMs >= 1000ull && g_flk.presents > 0)
        {
            const unsigned sp = g_flk.stereoP ? g_flk.stereoP : 1u;
            char fb[480] = {};
            sprintf_s(fb,
                "[FLICKER] pres=%u stereo=%u | pass0 miss=%u maxGap=%u | ui/p=%u..%u avg=%.1f mir/p=%u..%u | rightEyeOnly sum=%u worst=%u"
                " || scenePasses=%u..%u avg=%.2f | uiScale=%.4f..%.4f | [CAPJITTER] capAtClear=%u..%u",
                g_flk.presents, g_flk.stereoP, g_flk.capMiss, g_flk.maxGap,
                (g_flk.uiMin == 0xFFFFFFFFu ? 0u : g_flk.uiMin), g_flk.uiMax,
                static_cast<double>(g_flk.uiSum) / static_cast<double>(sp),
                (g_flk.mirMin == 0xFFFFFFFFu ? 0u : g_flk.mirMin), g_flk.mirMax,
                g_flk.reoSum, g_flk.reoWorst,
                (g_flk.passMin == 0xFFFFFFFFu ? 0u : g_flk.passMin), g_flk.passMax,
                static_cast<double>(g_flk.passSum) / static_cast<double>(sp),
                static_cast<double>(g_flk.uiSclMin > 1e8f ? 0.0f : g_flk.uiSclMin),
                static_cast<double>(g_flk.uiSclMax < -1e8f ? 0.0f : g_flk.uiSclMax),
                (g_flk.capMinIdx == 0xFFFFFFFFu ? 0u : g_flk.capMinIdx), g_flk.capMaxIdx);
            ME2VR::Log::Line(fb);
            g_flk = FlickerWindow{};
            g_flk.startMs = nowMs;
        }
    }
    g_flkUiThisPresent = 0;
    g_flkMirrorThisPresent = 0;
    g_lastCompletedScenePasses.store(g_flkPassesThisPresent, std::memory_order_release);
    g_flkPassesThisPresent = 0;   // [FLICKER2]
    g_flkUiScaleX = 0.0f;
    g_flkCapAtClear = 0;          // [CAPJITTER]

    g_sfrClearsThisPresent = 0;
    g_sfrCapturedThisPresent = false;   // [SFRCAP]
    g_sfrBbDrawnThisPresent.store(false, std::memory_order_relaxed);   // [SFRCAP2]
    g_presentIndex.fetch_add(1, std::memory_order_relaxed);   // [SFRFRESH]

    // [GUISCENE] Final design after a day of measured failures. Game MODE cannot identify these
    // menus (the squad menu LIVES at gm 0 with blips to 9 - the gm override squash-cycled, 09:35).
    // The CAMERA cannot either (the squad menu runs a gameplay-class camera - the camera gate
    // never armed, 09:50). Raw sightings could not exclude gameplay (phantom scene-RT samples
    // every gameplay frame stuck the latch, 09:26) - but those phantoms come from code OUTSIDE
    // the game module (Steam overlay drawing on the game's context), so the probe now requires a
    // game-exe frame on the draw's stack and the phantom dies at the source. What remains is the
    // decision the user actually asked for: ANY game menu that composites the 3D scene into its
    // GUI presents mono, no second-guessing. Arm = 3 consecutive sighted presents (1 within 600
    // of a release - tab switches pause sampling briefly); release = quiet 240 (~2.7s, idle menus
    // pause redraws ~0.7s). The ACTIVE line logs the sighting call site (+rva) so any future
    // false trigger names itself.
    {
        const bool seen = g_guiSceneSeenThisPresent.exchange(false, std::memory_order_relaxed);
        const uint64_t pi = g_presentIndex.load(std::memory_order_relaxed);
        if (seen) { if (g_guiSceneStreak < 1000) ++g_guiSceneStreak; }
        else g_guiSceneStreak = 0;
        const bool active = g_guiSceneActive.load(std::memory_order_acquire);
        const int enterStreak = (pi - g_guiSceneLastReleasePresent < 600) ? 1 : 3;
        if (g_guiSceneStreak >= enterStreak)
        {
            g_guiSceneLastActivePresent.store(pi, std::memory_order_relaxed);
            if (!active)
            {
                g_guiSceneActive.store(true, std::memory_order_release);
                char b[160] = {};
                sprintf_s(b, "[GUISCENE] GUI-scene menu ACTIVE -> flat panel, raw render (gm %d, caller +0x%llX)",
                          g_autoGameMode.load(std::memory_order_acquire),
                          static_cast<unsigned long long>(g_guiSceneLastCallerRva.load(std::memory_order_relaxed)));
                ME2VR::Log::Line(b);
            }
        }
        else if (active)
        {
            const uint64_t quiet = pi - g_guiSceneLastActivePresent.load(std::memory_order_relaxed);
            if (quiet > 240)
            {
                g_guiSceneActive.store(false, std::memory_order_release);
                g_guiSceneLastReleasePresent = pi;
                ME2VR::Log::Line("[GUISCENE] released (quiet) -> back to VR");
            }
        }
    }

    // [UICALLERS] periodic histogram of which exe call sites are being CLASSIFIED as UI draws.
    // Diagnostics only. Read it in the club: a site that only appears there is the misclassified
    // world effect, and the fix is a tighter IsUiDrawNow - not another viewport tweak.
    if (ME2VR::Log::DiagnosticsOn())
    {
        static ULONGLONG s_lastUiDump = 0;
        const ULONGLONG nowMs = GetTickCount64();
        if (nowMs - s_lastUiDump >= 3000) { s_lastUiDump = nowMs; DumpUiCallers("3s"); }
    }

    // F2 = manual flat/mono override (for journal/squad etc. that the mod doesn't auto-detect yet).
    {
        static bool s_f2Down = false;
        const bool f2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
        if (f2 && !s_f2Down)
        {
            const bool nm = !g_manualFlat.load(std::memory_order_acquire);
            g_manualFlat.store(nm, std::memory_order_release);
            ME2VR::Log::Line(std::string("[ME3DISC] manual flat override ") + (nm ? "ON (F2)" : "OFF (F2)"));
        }
        s_f2Down = f2;
    }

    // Galaxy presence is only a raw signal. UpdatePresentationState owns the mono transition.
    {
        static bool s_galaxyWas = false;
        const bool galaxy = ME2VR::EngineProbe::IsGalaxyMapOpen();
        if (galaxy != s_galaxyWas)
        {
            ME2VR::Log::Line(std::string("[GUISIGNAL] galaxy ") + (galaxy ? "ON" : "OFF"));
            s_galaxyWas = galaxy;
        }
    }

    // F3 dumps the GFx UI object graph (class names) for discovery: press it in gameplay, then again
    // in the galaxy map / journal, and compare to find which object/field marks a full-screen screen.
    {
        static bool s_f3Down = false;
        const bool f3 = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
        if (f3 && !s_f3Down) ME2VR::EngineProbe::DumpGfxState();
        s_f3Down = f3;
    }

    // F4 = master VR on/off (OFF = vanilla flat, for first-person tuning).
    {
        static bool s_f4Down = false;
        const bool f4 = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
        if (f4 && !s_f4Down)
        {
            const bool on = !ME2VR::CalcViewHook::GetVrEnabled();
            ME2VR::CalcViewHook::SetVrEnabled(on);
            ME2VR::Log::Line(std::string("[ME3DISC] VR ") + (on ? "ON (F4)" : "OFF (F4) - flat"));
        }
        s_f4Down = f4;
    }

    // Rebindable first-person toggle. One authoritative key (default K), matching ME2. F7 = head-hide test.
    {
        static bool s_fpKeyDown = false;
        const int fpVk = ME2VR::Menu::GetFpToggleKey();
        const bool fpKey = !ME2VR::Menu::IsRebindingFpToggle() && fpVk != 0 &&
                        (GetAsyncKeyState(fpVk) & 0x8000) != 0;
        if (fpKey && !s_fpKeyDown)
        {
            const bool on = !ME2VR::EngineProbe::GetFirstPerson();
            ME2VR::EngineProbe::SetFirstPerson(on);
            ME2VR::Log::Line(std::string("[ME3DISC] first-person ") + (on ? "ON" : "OFF"));
        }
        s_fpKeyDown = fpKey;
        static bool s_rcDown = false;
        const int rcKey = ME2VR::Menu::GetRecenterKey();
        const bool rc = rcKey != 0 && (GetAsyncKeyState(rcKey) & 0x8000) != 0;
        if (rc && !s_rcDown) ME2VR::Me2Xr::Recenter();
        s_rcDown = rc;
        static bool s_f7Down = false;
        const bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (f7 && !s_f7Down)
        {
            const bool on = !ME2VR::EngineProbe::GetMeshHide();
            ME2VR::EngineProbe::SetMeshHide(on);
            ME2VR::Log::Line(std::string("[ME3DISC] mesh-hide ") + (on ? "ON (F7)" : "OFF (F7)"));
        }
        s_f7Down = f7;
    }

    (void)g_menuEma;

    // Ordered single-frame op trace: end the previous traced frame, arm a few more at intervals.
    if (g_traceActive.load(std::memory_order_acquire))
    {
        ME2VR::Log::Line("[ME3DISC] === TRACE FRAME END (ops=" + std::to_string(g_traceSeq.load()) + ") ===");
        g_traceActive.store(false, std::memory_order_release);
    }
    if (n == 400 || n == 800)
    {
        g_traceSeq.store(0, std::memory_order_release);
        g_traceActive.store(true, std::memory_order_release);
        ME2VR::Log::Line("[ME3DISC] === TRACE FRAME START (present " + std::to_string(n) + ") ===");
    }

    // ME3 MILESTONE 2: full stereo. All RVAs found (CalcSceneView +0x6CFFD0, Draw +0x6D1E30,
    // AllocateViewState +0x3440F0). Finder retired (its 195-candidate probe is what crashed).
    ME2VR::EngineProbe::TryDumpOnce();
    ME2VR::HudProbe::Tick();                 // guarded baked per-element HUD layout
    ME2VR::ConvoFp::Tick();                  // exact live LE3 conversation controller -> staged-eye pose
    ME2VR::EngineProbe::ApplyFirstPerson();   // per-state gameplay FP camera + head/body hide (configured key; default K)
    ME2VR::D3DCapture::UpdatePresentationState();
    ME2VR::CalcViewHook::Tick();
    ME2VR::Menu::OnPresent(swapChain);
    ME2VR::Me2Xr::Tick();

    // Flat mode (VR off): the VR path that composites the menu is skipped, so draw it straight to the
    // backbuffer here - otherwise the Insert menu is invisible while tuning FP flat.
    if (!ME2VR::CalcViewHook::GetVrEnabled())
        ME2VR::Menu::RenderToBackbuffer(swapChain);

    PresentFn original = g_originalPresent;
    if (original == nullptr) return DXGI_ERROR_INVALID_CALL;
    // The flat window is only a mirror while VR is on: the headset is paced by xrWaitFrame and the
    // mod's own pacer, so the game's vsync request is dropped here and the real pacers own the
    // cadence. VR off (F4 flat dev) keeps the game's own sync untouched.
    const bool vrOn = ME2VR::CalcViewHook::GetVrEnabled();
    const UINT effSync = vrOn ? 0u : syncInterval;

    // [MIRRORTHROTTLE] the 60fps cap, root-caused on ME2 2026-07-31 by comparing focused and
    // unfocused frame windows: focused ran a metronomic 59.6-60.2fps, exactly half the 119.88Hz
    // display, while unfocused ran a variable 76-113fps on identical engine work. Locked versus
    // variable is the tell. While the borderless window is composited by DWM, presentation
    // back-pressure quantizes the frame to two vblanks. It does NOT block inside Present (measured
    // 0.14ms); the runtime stalls the render thread later, when it needs a backbuffer DWM has not
    // released. In VR nobody looks at the flat window, and the headset image is already submitted
    // by Me2Xr::Tick() above, before this call. So present it rarely: the queue never fills, the
    // stall never happens, and the engine free-runs at its real rate (ME2 measured 119.7fps after
    // this change). The mirror updating at ~8-15fps is the intended trade. VR off is untouched.
    static uint64_t s_mirrorN = 0;
    const uint64_t every = g_mirrorPresentEvery.load(std::memory_order_relaxed);
    const bool skipMirror = vrOn && (every > 1) && ((++s_mirrorN % every) != 0);
    {
        LARGE_INTEGER ftEnd; QueryPerformanceCounter(&ftEnd);   // [FRAMETIME]
        FrameTimeAccount(ftEntry, ftEnd, ME2VR::Me2Xr::LastWaitFrameUs());
    }
    if (skipMirror) return S_OK;
    return original(swapChain, effSync, flags);
}

void STDMETHODCALLTYPE RSSetViewportsHook(ID3D11DeviceContext* context,
                                          UINT numViewports,
                                          const D3D11_VIEWPORT* viewports) noexcept
{
    const uint64_t hc0 = __rdtsc();   // [HOOKCOST]
    const auto n = g_viewportSetCount.fetch_add(1, std::memory_order_relaxed) + 1;
    bool viewportChanged = false;
    if (numViewports > 0 && viewports != nullptr)
    {
        const D3D11_VIEWPORT& vp = viewports[0];
        g_currentViewport = vp;
        g_haveCurrentViewport = true;
        viewportChanged =
            !g_haveLastLoggedViewport ||
            std::fabs(vp.TopLeftX - g_lastLoggedViewport.TopLeftX) > 0.5f ||
            std::fabs(vp.TopLeftY - g_lastLoggedViewport.TopLeftY) > 0.5f ||
            std::fabs(vp.Width - g_lastLoggedViewport.Width) > 0.5f ||
            std::fabs(vp.Height - g_lastLoggedViewport.Height) > 0.5f ||
            std::fabs(vp.MinDepth - g_lastLoggedViewport.MinDepth) > 0.01f ||
            std::fabs(vp.MaxDepth - g_lastLoggedViewport.MaxDepth) > 0.01f;
    }

    const bool shouldLog = n <= 80 || numViewports != 1 || viewportChanged;
    if (shouldLog && g_viewportLogCount.load(std::memory_order_relaxed) < 512)
    {
        const auto slot = g_viewportLogCount.fetch_add(1, std::memory_order_relaxed);
        if (slot < 512)
        {
            std::string line = "[ME3DISC] RSSetViewports call=" + std::to_string(n) +
                               " count=" + std::to_string(numViewports) +
                               " caller=" + CallerTag(_ReturnAddress());
            const UINT maxLog = (std::min)(numViewports, 4u);
            for (UINT i = 0; i < maxLog && viewports != nullptr; ++i)
            {
                char buffer[256] = {};
                sprintf_s(buffer,
                          " vp%u={x=%.1f y=%.1f w=%.1f h=%.1f minZ=%.2f maxZ=%.2f}",
                          i,
                          viewports[i].TopLeftX,
                          viewports[i].TopLeftY,
                          viewports[i].Width,
                          viewports[i].Height,
                          viewports[i].MinDepth,
                          viewports[i].MaxDepth);
                line += buffer;
            }
            ME2VR::Log::Line(line);

            if (IsInterestingViewport(numViewports, viewports) &&
                g_stackLogCount.fetch_add(1, std::memory_order_relaxed) < 96)
            {
                ME2VR::Log::Line("[ME3DISC] STACK RSSetViewports call=" + std::to_string(n) +
                                 " " + StackTag());
            }

            if (numViewports > 0 && viewports != nullptr)
            {
                g_lastLoggedViewport = viewports[0];
                g_haveLastLoggedViewport = true;
            }
        }
    }

    if (g_traceAfterFullSizeStereoBind.load(std::memory_order_acquire) != 0)
    {
        char detail[256] = {};
        if (numViewports > 0 && viewports != nullptr)
        {
            sprintf_s(detail,
                      "count=%u vp0=%.1f,%.1f %.1fx%.1f z=%.2f..%.2f",
                      numViewports,
                      viewports[0].TopLeftX,
                      viewports[0].TopLeftY,
                      viewports[0].Width,
                      viewports[0].Height,
                      viewports[0].MinDepth,
                      viewports[0].MaxDepth);
        }
        else
        {
            sprintf_s(detail, "count=%u", numViewports);
        }
        LogStateTrace("RSSetViewports", _ReturnAddress(), detail);
    }

    HookCostBind(__rdtsc() - hc0);   // [HOOKCOST]
    RSSetViewportsFn original = g_originalRSSetViewports;
    if (original != nullptr) original(context, numViewports, viewports);
}

void STDMETHODCALLTYPE PSSetShaderResourcesHook(ID3D11DeviceContext* context,
                                                UINT startSlot,
                                                UINT numViews,
                                                ID3D11ShaderResourceView* const* shaderResourceViews) noexcept
{
    const uint64_t hc0 = __rdtsc();   // [HOOKCOST]
    const auto n = g_psSetShaderResourcesCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (numViews > 0 && startSlot < _countof(g_boundPsTrackedResources))
    {
        const UINT trackedSlotLimit = (std::min)(numViews, static_cast<UINT>(_countof(g_boundPsTrackedResources) - startSlot));
        for (UINT i = 0; i < trackedSlotLimit; ++i)
        {
            g_boundPsTrackedResources[startSlot + i] = {};
        }
    }

    if (g_trackedStereoResourceCount > 0 && shaderResourceViews != nullptr && numViews > 0)
    {
        const UINT maxViews = (std::min)(numViews, static_cast<UINT>(_countof(g_boundPsTrackedResources) - (std::min)(startSlot, static_cast<UINT>(_countof(g_boundPsTrackedResources)))));
        for (UINT i = 0; i < maxViews; ++i)
        {
            ID3D11Resource* resource = nullptr;
            if (shaderResourceViews[i] != nullptr)
            {
                shaderResourceViews[i]->GetResource(&resource);
            }

            const auto resourcePtr = reinterpret_cast<std::uintptr_t>(resource);
            const TrackedResource* tracked = FindTrackedStereoResource(resourcePtr);
            if (tracked != nullptr)
            {
                g_boundPsTrackedResources[startSlot + i] = *tracked;

                const bool currentIsBackbuffer = g_currentRtvResourcePtr == g_backbufferResourcePtr;
                const bool currentIsFullSizeTarget =
                    g_backbufferWidth != 0 &&
                    g_backbufferHeight != 0 &&
                    g_currentRtvWidth == g_backbufferWidth &&
                    g_currentRtvHeight == g_backbufferHeight;

                if (currentIsBackbuffer || currentIsFullSizeTarget)
                {
                    g_pendingStereoSlot = startSlot + i;
                    g_pendingStereoResource = *tracked;
                    g_pendingStereoCompositeDraws.store(32, std::memory_order_release);
                    g_traceAfterFullSizeStereoBind.store(48, std::memory_order_release);
                }

                const auto slot = g_psSetShaderResourcesLogCount.fetch_add(1, std::memory_order_relaxed);
                if (slot < 256)
                {
                    char buffer[512] = {};
                    sprintf_s(buffer,
                              "[ME3DISC] PSSetShaderResources uses stereoTex call=%llu start=%u slot=%u tex=%p %ux%u fmt=%u caller=%s currentRT=%p %ux%u fmt=%u backbuffer=%d fullSizeRT=%d",
                              static_cast<unsigned long long>(n),
                              startSlot,
                              startSlot + i,
                              reinterpret_cast<void*>(resourcePtr),
                              tracked->width,
                              tracked->height,
                              static_cast<unsigned int>(tracked->format),
                              CallerTag(_ReturnAddress()).c_str(),
                              reinterpret_cast<void*>(g_currentRtvResourcePtr),
                              g_currentRtvWidth,
                              g_currentRtvHeight,
                              static_cast<unsigned int>(g_currentRtvFormat),
                              currentIsBackbuffer ? 1 : 0,
                              currentIsFullSizeTarget ? 1 : 0);
                    ME2VR::Log::Line(buffer);
                }
            }

            SafeRelease(resource);
        }
    }

    if (g_traceAfterFullSizeStereoBind.load(std::memory_order_acquire) != 0)
    {
        char detail[192] = {};
        sprintf_s(detail, "start=%u num=%u", startSlot, numViews);
        LogStateTrace("PSSetShaderResources", _ReturnAddress(), detail);
    }

    HookCostBind(__rdtsc() - hc0);   // [HOOKCOST]
    PSSetShaderResourcesFn original = g_originalPSSetShaderResources;
    if (original != nullptr) original(context, startSlot, numViews, shaderResourceViews);
}

void LogStereoBoundDraw(const char* drawKind,
                        void* caller,
                        UINT count,
                        UINT start,
                        INT baseVertex,
                        bool hasBaseVertex) noexcept
{
    const bool currentIsBackbuffer = g_currentRtvResourcePtr == g_backbufferResourcePtr;
    const bool currentIsFullSizeTarget =
        g_backbufferWidth != 0 &&
        g_backbufferHeight != 0 &&
        g_currentRtvWidth == g_backbufferWidth &&
        g_currentRtvHeight == g_backbufferHeight;
    if (!currentIsBackbuffer && !currentIsFullSizeTarget) return;

    UINT boundSlot = 0;
    const TrackedResource* bound = FindBoundPsStereoResource(&boundSlot);
    if (bound == nullptr) return;

    const auto slot = g_stereoBoundDrawLogCount.fetch_add(1, std::memory_order_relaxed);
    if (slot >= 256) return;

    char buffer[768] = {};
    if (hasBaseVertex)
    {
        sprintf_s(buffer,
                  "[ME3DISC] STEREO_BOUND_DRAW %s caller=%s count=%u start=%u base=%d boundSlot=%u stereoTex=%p %ux%u fmt=%u currentRT=%p %ux%u fmt=%u backbuffer=%d fullSizeRT=%d vp={x=%.1f y=%.1f w=%.1f h=%.1f}",
                  drawKind,
                  CallerTag(caller).c_str(),
                  count,
                  start,
                  baseVertex,
                  boundSlot,
                  reinterpret_cast<void*>(bound->ptr),
                  bound->width,
                  bound->height,
                  static_cast<unsigned int>(bound->format),
                  reinterpret_cast<void*>(g_currentRtvResourcePtr),
                  g_currentRtvWidth,
                  g_currentRtvHeight,
                  static_cast<unsigned int>(g_currentRtvFormat),
                  currentIsBackbuffer ? 1 : 0,
                  currentIsFullSizeTarget ? 1 : 0,
                  g_haveCurrentViewport ? g_currentViewport.TopLeftX : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.TopLeftY : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.Width : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.Height : -1.0f);
    }
    else
    {
        sprintf_s(buffer,
                  "[ME3DISC] STEREO_BOUND_DRAW %s caller=%s count=%u start=%u boundSlot=%u stereoTex=%p %ux%u fmt=%u currentRT=%p %ux%u fmt=%u backbuffer=%d fullSizeRT=%d vp={x=%.1f y=%.1f w=%.1f h=%.1f}",
                  drawKind,
                  CallerTag(caller).c_str(),
                  count,
                  start,
                  boundSlot,
                  reinterpret_cast<void*>(bound->ptr),
                  bound->width,
                  bound->height,
                  static_cast<unsigned int>(bound->format),
                  reinterpret_cast<void*>(g_currentRtvResourcePtr),
                  g_currentRtvWidth,
                  g_currentRtvHeight,
                  static_cast<unsigned int>(g_currentRtvFormat),
                  currentIsBackbuffer ? 1 : 0,
                  currentIsFullSizeTarget ? 1 : 0,
                  g_haveCurrentViewport ? g_currentViewport.TopLeftX : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.TopLeftY : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.Width : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.Height : -1.0f);
    }
    ME2VR::Log::Line(buffer);
}

void LogPendingCompositeDraw(const char* drawKind,
                             void* caller,
                             UINT count,
                             UINT start,
                             INT baseVertex,
                             bool hasBaseVertex) noexcept
{
    unsigned expected = g_pendingStereoCompositeDraws.load(std::memory_order_acquire);
    while (expected != 0 &&
           !g_pendingStereoCompositeDraws.compare_exchange_weak(expected,
                                                                expected - 1,
                                                                std::memory_order_acq_rel,
                                                                std::memory_order_acquire))
    {
    }
    if (expected == 0) return;

    const unsigned remaining = expected - 1;
    const auto slot = g_drawCompositeLogCount.fetch_add(1, std::memory_order_relaxed);
    if (slot >= 128) return;

    const bool currentIsBackbuffer = g_currentRtvResourcePtr == g_backbufferResourcePtr;
    const bool currentIsFullSizeTarget =
        g_backbufferWidth != 0 &&
        g_backbufferHeight != 0 &&
        g_currentRtvWidth == g_backbufferWidth &&
        g_currentRtvHeight == g_backbufferHeight;

    char buffer[768] = {};
    if (hasBaseVertex)
    {
        sprintf_s(buffer,
                  "[ME3DISC] COMPOSITE %s caller=%s count=%u start=%u base=%d pendingSlot=%u stereoTex=%p %ux%u fmt=%u currentRT=%p %ux%u fmt=%u backbuffer=%d fullSizeRT=%d vp={x=%.1f y=%.1f w=%.1f h=%.1f} remaining=%u",
                  drawKind,
                  CallerTag(caller).c_str(),
                  count,
                  start,
                  baseVertex,
                  g_pendingStereoSlot,
                  reinterpret_cast<void*>(g_pendingStereoResource.ptr),
                  g_pendingStereoResource.width,
                  g_pendingStereoResource.height,
                  static_cast<unsigned int>(g_pendingStereoResource.format),
                  reinterpret_cast<void*>(g_currentRtvResourcePtr),
                  g_currentRtvWidth,
                  g_currentRtvHeight,
                  static_cast<unsigned int>(g_currentRtvFormat),
                  currentIsBackbuffer ? 1 : 0,
                  currentIsFullSizeTarget ? 1 : 0,
                  g_haveCurrentViewport ? g_currentViewport.TopLeftX : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.TopLeftY : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.Width : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.Height : -1.0f,
                  remaining);
    }
    else
    {
        sprintf_s(buffer,
                  "[ME3DISC] COMPOSITE %s caller=%s count=%u start=%u pendingSlot=%u stereoTex=%p %ux%u fmt=%u currentRT=%p %ux%u fmt=%u backbuffer=%d fullSizeRT=%d vp={x=%.1f y=%.1f w=%.1f h=%.1f} remaining=%u",
                  drawKind,
                  CallerTag(caller).c_str(),
                  count,
                  start,
                  g_pendingStereoSlot,
                  reinterpret_cast<void*>(g_pendingStereoResource.ptr),
                  g_pendingStereoResource.width,
                  g_pendingStereoResource.height,
                  static_cast<unsigned int>(g_pendingStereoResource.format),
                  reinterpret_cast<void*>(g_currentRtvResourcePtr),
                  g_currentRtvWidth,
                  g_currentRtvHeight,
                  static_cast<unsigned int>(g_currentRtvFormat),
                  currentIsBackbuffer ? 1 : 0,
                  currentIsFullSizeTarget ? 1 : 0,
                  g_haveCurrentViewport ? g_currentViewport.TopLeftX : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.TopLeftY : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.Width : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.Height : -1.0f,
                  remaining);
    }
    ME2VR::Log::Line(buffer);
}

void STDMETHODCALLTYPE DrawIndexedHook(ID3D11DeviceContext* context,
                                       UINT indexCount,
                                       UINT startIndexLocation,
                                       INT baseVertexLocation) noexcept
{
    const uint64_t hc0 = __rdtsc();   // [HOOKCOST]
    // DIBR draw-gate: attribute this draw to the bound depth resource (the depth capture is
    // draw-count gated; without this the scene depth never promotes).
    if (g_depthMapEnabled.load(std::memory_order_relaxed) && g_boundDepthRes != nullptr)
    { uint32_t* dc = DepthDrawCounter(g_boundDepthRes); if (dc != nullptr) (*dc)++; }
    FirstFire("DrawIndexed", 0);
    LogStereoBoundDraw("DrawIndexed", _ReturnAddress(), indexCount, startIndexLocation, baseVertexLocation, true);
    LogUiSeamStack("DrawIndexed", indexCount);
    TraceOp("DrawIndexed", context, indexCount);
    // Format the trace detail only while the state trace is armed: this ran an sprintf on EVERY draw
    // of every pass (SFR = twice per present) for a line that is dropped unless a stereo-bound trace
    // window is open, which in ME3 never happens (no 1863x1048 stereo textures exist here).
    if (g_traceAfterFullSizeStereoBind.load(std::memory_order_acquire) != 0)
    {
        char detail[160] = {};
        sprintf_s(detail, "count=%u start=%u base=%d", indexCount, startIndexLocation, baseVertexLocation);
        LogStateTrace("DrawIndexed", _ReturnAddress(), detail);
    }
    LogPendingCompositeDraw("DrawIndexed", _ReturnAddress(), indexCount, startIndexLocation, baseVertexLocation, true);

    if (CaptureUiCompositeAndSkip(context, indexCount)) { HookCostDraw(__rdtsc() - hc0); return; }
    HookCostDraw(__rdtsc() - hc0);
    DrawIndexedFn original = g_originalDrawIndexed;
    if (original != nullptr) original(context, indexCount, startIndexLocation, baseVertexLocation);
}

void STDMETHODCALLTYPE DrawHook(ID3D11DeviceContext* context,
                                UINT vertexCount,
                                UINT startVertexLocation) noexcept
{
    const uint64_t hc0 = __rdtsc();   // [HOOKCOST]
    // DIBR draw-gate: attribute this draw to the bound depth resource (the depth capture is
    // draw-count gated; without this the scene depth never promotes).
    if (g_depthMapEnabled.load(std::memory_order_relaxed) && g_boundDepthRes != nullptr)
    { uint32_t* dc = DepthDrawCounter(g_boundDepthRes); if (dc != nullptr) (*dc)++; }
    FirstFire("Draw", 1);
    LogStereoBoundDraw("Draw", _ReturnAddress(), vertexCount, startVertexLocation, 0, false);
    LogUiSeamStack("Draw", vertexCount);
    TraceOp("Draw", context, vertexCount);
    if (g_traceAfterFullSizeStereoBind.load(std::memory_order_acquire) != 0)
    {
        char detail[160] = {};
        sprintf_s(detail, "count=%u start=%u", vertexCount, startVertexLocation);
        LogStateTrace("Draw", _ReturnAddress(), detail);
    }
    LogPendingCompositeDraw("Draw", _ReturnAddress(), vertexCount, startVertexLocation, 0, false);

    if (CaptureUiCompositeAndSkip(context, vertexCount)) { HookCostDraw(__rdtsc() - hc0); return; }
    HookCostDraw(__rdtsc() - hc0);
    DrawFn original = g_originalDraw;
    if (original != nullptr) original(context, vertexCount, startVertexLocation);
}

void STDMETHODCALLTYPE DrawIndexedInstancedHook(ID3D11DeviceContext* context,
                                                UINT indexCountPerInstance,
                                                UINT instanceCount,
                                                UINT startIndexLocation,
                                                INT baseVertexLocation,
                                                UINT startInstanceLocation) noexcept
{
    LogStereoBoundDraw("DrawIndexedInstanced",
                       _ReturnAddress(),
                       indexCountPerInstance,
                       startIndexLocation,
                       baseVertexLocation,
                       true);
    FirstFire("DrawIndexedInstanced", 2);
    LogUiSeamStack("DrawIndexedInstanced", indexCountPerInstance);
    TraceOp("DrawIdxInst", context, indexCountPerInstance);
    if (g_traceAfterFullSizeStereoBind.load(std::memory_order_acquire) != 0)
    {
        char detail[192] = {};
        sprintf_s(detail,
                  "idxPerInst=%u inst=%u start=%u base=%d startInst=%u",
                  indexCountPerInstance,
                  instanceCount,
                  startIndexLocation,
                  baseVertexLocation,
                  startInstanceLocation);
        LogStateTrace("DrawIndexedInstanced", _ReturnAddress(), detail);
    }
    LogPendingCompositeDraw("DrawIndexedInstanced",
                            _ReturnAddress(),
                            indexCountPerInstance,
                            startIndexLocation,
                            baseVertexLocation,
                            true);

    DrawIndexedInstancedFn original = g_originalDrawIndexedInstanced;
    if (original != nullptr)
    {
        original(context,
                 indexCountPerInstance,
                 instanceCount,
                 startIndexLocation,
                 baseVertexLocation,
                 startInstanceLocation);
    }
}

void STDMETHODCALLTYPE DrawInstancedHook(ID3D11DeviceContext* context,
                                         UINT vertexCountPerInstance,
                                         UINT instanceCount,
                                         UINT startVertexLocation,
                                         UINT startInstanceLocation) noexcept
{
    FirstFire("DrawInstanced", 3);
    LogStereoBoundDraw("DrawInstanced", _ReturnAddress(), vertexCountPerInstance, startVertexLocation, 0, false);
    LogUiSeamStack("DrawInstanced", vertexCountPerInstance);
    TraceOp("DrawInst", context, vertexCountPerInstance);
    if (g_traceAfterFullSizeStereoBind.load(std::memory_order_acquire) != 0)
    {
        char detail[192] = {};
        sprintf_s(detail,
                  "vtxPerInst=%u inst=%u start=%u startInst=%u",
                  vertexCountPerInstance,
                  instanceCount,
                  startVertexLocation,
                  startInstanceLocation);
        LogStateTrace("DrawInstanced", _ReturnAddress(), detail);
    }
    LogPendingCompositeDraw("DrawInstanced", _ReturnAddress(), vertexCountPerInstance, startVertexLocation, 0, false);

    DrawInstancedFn original = g_originalDrawInstanced;
    if (original != nullptr)
    {
        original(context, vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation);
    }
}

void STDMETHODCALLTYPE DrawAutoHook(ID3D11DeviceContext* context) noexcept
{
    FirstFire("DrawAuto", 4);
    LogStereoBoundDraw("DrawAuto", _ReturnAddress(), 0, 0, 0, false);
    TraceOp("DrawAuto", context, 0);
    LogStateTrace("DrawAuto", _ReturnAddress(), "");
    LogPendingCompositeDraw("DrawAuto", _ReturnAddress(), 0, 0, 0, false);

    DrawAutoFn original = g_originalDrawAuto;
    if (original != nullptr) original(context);
}

void STDMETHODCALLTYPE DrawIndexedInstancedIndirectHook(ID3D11DeviceContext* context,
                                                        ID3D11Buffer* bufferForArgs,
                                                        UINT alignedByteOffsetForArgs) noexcept
{
    LogStereoBoundDraw("DrawIndexedInstancedIndirect",
                       _ReturnAddress(),
                       alignedByteOffsetForArgs,
                       0,
                       0,
                       false);
    FirstFire("DrawIndexedInstancedIndirect", 5);
    TraceOp("DrawIdxIndir", context, 0);
    char detail[160] = {};
    sprintf_s(detail, "args=%p offset=%u", bufferForArgs, alignedByteOffsetForArgs);
    LogStateTrace("DrawIndexedInstancedIndirect", _ReturnAddress(), detail);
    LogPendingCompositeDraw("DrawIndexedInstancedIndirect",
                            _ReturnAddress(),
                            alignedByteOffsetForArgs,
                            0,
                            0,
                            false);

    DrawIndexedInstancedIndirectFn original = g_originalDrawIndexedInstancedIndirect;
    if (original != nullptr) original(context, bufferForArgs, alignedByteOffsetForArgs);
}

void STDMETHODCALLTYPE DrawInstancedIndirectHook(ID3D11DeviceContext* context,
                                                 ID3D11Buffer* bufferForArgs,
                                                 UINT alignedByteOffsetForArgs) noexcept
{
    LogStereoBoundDraw("DrawInstancedIndirect", _ReturnAddress(), alignedByteOffsetForArgs, 0, 0, false);
    FirstFire("DrawInstancedIndirect", 6);
    TraceOp("DrawInstIndir", context, 0);
    char detail[160] = {};
    sprintf_s(detail, "args=%p offset=%u", bufferForArgs, alignedByteOffsetForArgs);
    LogStateTrace("DrawInstancedIndirect", _ReturnAddress(), detail);
    LogPendingCompositeDraw("DrawInstancedIndirect",
                            _ReturnAddress(),
                            alignedByteOffsetForArgs,
                            0,
                            0,
                            false);

    DrawInstancedIndirectFn original = g_originalDrawInstancedIndirect;
    if (original != nullptr) original(context, bufferForArgs, alignedByteOffsetForArgs);
}

void STDMETHODCALLTYPE DispatchHook(ID3D11DeviceContext* context,
                                    UINT threadGroupCountX,
                                    UINT threadGroupCountY,
                                    UINT threadGroupCountZ) noexcept
{
    if (g_traceAfterFullSizeStereoBind.load(std::memory_order_acquire) != 0)
    {
        char detail[160] = {};
        sprintf_s(detail, "groups=%u,%u,%u", threadGroupCountX, threadGroupCountY, threadGroupCountZ);
        LogStateTrace("Dispatch", _ReturnAddress(), detail);
    }
    LogPendingCompositeDraw("Dispatch", _ReturnAddress(), threadGroupCountX, threadGroupCountY, threadGroupCountZ, true);

    DispatchFn original = g_originalDispatch;
    if (original != nullptr) original(context, threadGroupCountX, threadGroupCountY, threadGroupCountZ);
}

void STDMETHODCALLTYPE DispatchIndirectHook(ID3D11DeviceContext* context,
                                            ID3D11Buffer* bufferForArgs,
                                            UINT alignedByteOffsetForArgs) noexcept
{
    char detail[160] = {};
    sprintf_s(detail, "args=%p offset=%u", bufferForArgs, alignedByteOffsetForArgs);
    LogStateTrace("DispatchIndirect", _ReturnAddress(), detail);
    LogPendingCompositeDraw("DispatchIndirect", _ReturnAddress(), alignedByteOffsetForArgs, 0, 0, false);

    DispatchIndirectFn original = g_originalDispatchIndirect;
    if (original != nullptr) original(context, bufferForArgs, alignedByteOffsetForArgs);
}

void STDMETHODCALLTYPE ExecuteCommandListHook(ID3D11DeviceContext* context,
                                              ID3D11CommandList* commandList,
                                              BOOL restoreContextState) noexcept
{
    const auto n = g_executeCommandListCount.fetch_add(1, std::memory_order_relaxed) + 1;
    FirstFire("ExecuteCommandList", 7);
    TraceOp("ExecCmdList", nullptr, 0xFFFFFFFFu);
    const unsigned pending = g_pendingStereoCompositeDraws.load(std::memory_order_acquire);
    const auto slot = g_executeCommandListLogCount.fetch_add(1, std::memory_order_relaxed);
    if (pending != 0 || slot < 64)
    {
        const bool currentIsBackbuffer = g_currentRtvResourcePtr == g_backbufferResourcePtr;
        const bool currentIsFullSizeTarget =
            g_backbufferWidth != 0 &&
            g_backbufferHeight != 0 &&
            g_currentRtvWidth == g_backbufferWidth &&
            g_currentRtvHeight == g_backbufferHeight;

        char buffer[768] = {};
        sprintf_s(buffer,
                  "[ME3DISC] ExecuteCommandList call=%llu caller=%s commandList=%p restore=%d pendingStereo=%u pendingSlot=%u currentRT=%p %ux%u fmt=%u backbuffer=%d fullSizeRT=%d",
                  static_cast<unsigned long long>(n),
                  CallerTag(_ReturnAddress()).c_str(),
                  commandList,
                  restoreContextState ? 1 : 0,
                  pending,
                  g_pendingStereoSlot,
                  reinterpret_cast<void*>(g_currentRtvResourcePtr),
                  g_currentRtvWidth,
                  g_currentRtvHeight,
                  static_cast<unsigned int>(g_currentRtvFormat),
                  currentIsBackbuffer ? 1 : 0,
                  currentIsFullSizeTarget ? 1 : 0);
        ME2VR::Log::Line(buffer);
    }

    ExecuteCommandListFn original = g_originalExecuteCommandList;
    if (original != nullptr) original(context, commandList, restoreContextState);
}

void LogCopyPath(const char* kind,
                 void* caller,
                 const TrackedResource& dst,
                 const TrackedResource& src,
                 UINT extraA,
                 UINT extraB) noexcept
{
    if (!IsResourceCopyInteresting(dst, src)) return;
    const auto slot = g_copyPathLogCount.fetch_add(1, std::memory_order_relaxed);
    if (slot >= 256) return;

    char buffer[768] = {};
    sprintf_s(buffer,
              "[ME3DISC] COPY %s caller=%s dst=%p %ux%u fmt=%u src=%p %ux%u fmt=%u dstBackbuffer=%d srcBackbuffer=%d dstFull=%d srcFull=%d pendingStereo=%u a=%u b=%u",
              kind,
              CallerTag(caller).c_str(),
              reinterpret_cast<void*>(dst.ptr),
              dst.width,
              dst.height,
              static_cast<unsigned int>(dst.format),
              reinterpret_cast<void*>(src.ptr),
              src.width,
              src.height,
              static_cast<unsigned int>(src.format),
              dst.ptr == g_backbufferResourcePtr ? 1 : 0,
              src.ptr == g_backbufferResourcePtr ? 1 : 0,
              IsFullSizeTracked(dst) ? 1 : 0,
              IsFullSizeTracked(src) ? 1 : 0,
              g_pendingStereoCompositeDraws.load(std::memory_order_acquire),
              extraA,
              extraB);
    ME2VR::Log::Line(buffer);
}

void STDMETHODCALLTYPE CopySubresourceRegionHook(ID3D11DeviceContext* context,
                                                 ID3D11Resource* dstResource,
                                                 UINT dstSubresource,
                                                 UINT dstX,
                                                 UINT dstY,
                                                 UINT dstZ,
                                                 ID3D11Resource* srcResource,
                                                 UINT srcSubresource,
                                                 const D3D11_BOX* srcBox) noexcept
{
    SfrNoteBackbufferCopyDst(dstResource);   // [CINECAP] pass-boundary tracking (cine composites copy)
    TrackedResource dst = {};
    TrackedResource src = {};
    if (TryDescribeTextureResource(dstResource, &dst) && TryDescribeTextureResource(srcResource, &src))
    {
        LogCopyPath("CopySubresourceRegion", _ReturnAddress(), dst, src, dstSubresource, srcSubresource);
        TraceCopy("CopySubRegion", dst, src);
        char detail[320] = {};
        sprintf_s(detail,
                  "dst=%p/%ux%u/f%u src=%p/%ux%u/f%u dstSub=%u srcSub=%u dstXYZ=%u,%u,%u",
                  reinterpret_cast<void*>(dst.ptr),
                  dst.width,
                  dst.height,
                  static_cast<unsigned int>(dst.format),
                  reinterpret_cast<void*>(src.ptr),
                  src.width,
                  src.height,
                  static_cast<unsigned int>(src.format),
                  dstSubresource,
                  srcSubresource,
                  dstX,
                  dstY,
                  dstZ);
        LogStateTrace("CopySubresourceRegion", _ReturnAddress(), detail);
    }

    CopySubresourceRegionFn original = g_originalCopySubresourceRegion;
    if (original != nullptr)
    {
        original(context, dstResource, dstSubresource, dstX, dstY, dstZ, srcResource, srcSubresource, srcBox);
    }
}

void STDMETHODCALLTYPE CopyResourceHook(ID3D11DeviceContext* context,
                                        ID3D11Resource* dstResource,
                                        ID3D11Resource* srcResource) noexcept
{
    SfrNoteBackbufferCopyDst(dstResource);   // [CINECAP] pass-boundary tracking (cine composites copy)
    TrackedResource dst = {};
    TrackedResource src = {};
    if (TryDescribeTextureResource(dstResource, &dst) && TryDescribeTextureResource(srcResource, &src))
    {
        LogCopyPath("CopyResource", _ReturnAddress(), dst, src, 0, 0);
        TraceCopy("CopyResource", dst, src);
        char detail[256] = {};
        sprintf_s(detail,
                  "dst=%p/%ux%u/f%u src=%p/%ux%u/f%u",
                  reinterpret_cast<void*>(dst.ptr),
                  dst.width,
                  dst.height,
                  static_cast<unsigned int>(dst.format),
                  reinterpret_cast<void*>(src.ptr),
                  src.width,
                  src.height,
                  static_cast<unsigned int>(src.format));
        LogStateTrace("CopyResource", _ReturnAddress(), detail);
    }

    CopyResourceFn original = g_originalCopyResource;
    if (original != nullptr) original(context, dstResource, srcResource);
}

void STDMETHODCALLTYPE ResolveSubresourceHook(ID3D11DeviceContext* context,
                                              ID3D11Resource* dstResource,
                                              UINT dstSubresource,
                                              ID3D11Resource* srcResource,
                                              UINT srcSubresource,
                                              DXGI_FORMAT format) noexcept
{
    SfrNoteBackbufferCopyDst(dstResource);   // [CINECAP] pass-boundary tracking (cine composites copy)
    TrackedResource dst = {};
    TrackedResource src = {};
    if (TryDescribeTextureResource(dstResource, &dst) && TryDescribeTextureResource(srcResource, &src))
    {
        LogCopyPath("ResolveSubresource", _ReturnAddress(), dst, src, dstSubresource, srcSubresource);
        TraceCopy("Resolve", dst, src);
        char detail[288] = {};
        sprintf_s(detail,
                  "dst=%p/%ux%u/f%u src=%p/%ux%u/f%u dstSub=%u srcSub=%u resolveFmt=%u",
                  reinterpret_cast<void*>(dst.ptr),
                  dst.width,
                  dst.height,
                  static_cast<unsigned int>(dst.format),
                  reinterpret_cast<void*>(src.ptr),
                  src.width,
                  src.height,
                  static_cast<unsigned int>(src.format),
                  dstSubresource,
                  srcSubresource,
                  static_cast<unsigned int>(format));
        LogStateTrace("ResolveSubresource", _ReturnAddress(), detail);
    }

    ResolveSubresourceFn original = g_originalResolveSubresource;
    if (original != nullptr) original(context, dstResource, dstSubresource, srcResource, srcSubresource, format);
}

void STDMETHODCALLTYPE OMSetRenderTargetsHook(ID3D11DeviceContext* context,
                                              UINT numViews,
                                              ID3D11RenderTargetView* const* renderTargetViews,
                                              ID3D11DepthStencilView* depthStencilView) noexcept
{
    const uint64_t hc0 = __rdtsc();   // [HOOKCOST]
    const auto n = g_omSetRenderTargetsCount.fetch_add(1, std::memory_order_relaxed) + 1;

    // DIBR: resolve the bound depth-stencil RESOURCE so the draw hooks can attribute geometry to it
    // (draw-gated depth capture). Identity only - released immediately, never dereferenced.
    if (depthStencilView != nullptr)
    {
        ID3D11Resource* dres = nullptr;
        depthStencilView->GetResource(&dres);
        g_boundDepthRes = dres;
        if (dres != nullptr) dres->Release();
    }
    else
    {
        g_boundDepthRes = nullptr;
    }

    ID3D11Resource* resource = nullptr;
    ID3D11Texture2D* texture = nullptr;
    D3D11_TEXTURE2D_DESC desc = {};
    std::uintptr_t resourcePtr = 0;
    bool gotDesc = false;

    if (numViews > 0 && renderTargetViews != nullptr && renderTargetViews[0] != nullptr)
    {
        renderTargetViews[0]->GetResource(&resource);
        resourcePtr = reinterpret_cast<std::uintptr_t>(resource);
        if (resource != nullptr &&
            SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) &&
            texture != nullptr)
        {
            texture->GetDesc(&desc);
            gotDesc = true;
            TrackStereoResource(resourcePtr, desc);
        }
    }

    g_currentRtvResourcePtr = resourcePtr;
    if (gotDesc)
    {
        g_currentRtvWidth = desc.Width;
        g_currentRtvHeight = desc.Height;
        g_currentRtvFormat = desc.Format;
    }
    else
    {
        g_currentRtvWidth = 0;
        g_currentRtvHeight = 0;
        g_currentRtvFormat = DXGI_FORMAT_UNKNOWN;
    }
    // [CTXRTV] the per-context record - see CtxNoteRtvBind. The globals above stay for the readers
    // not yet migrated, but anything deciding per DRAW must use the per-context lookup.
    CtxNoteRtvBind(context, resourcePtr,
                   gotDesc ? desc.Width : 0, gotDesc ? desc.Height : 0,
                   gotDesc ? desc.Format : DXGI_FORMAT_UNKNOWN);

    TraceOp("OMSetRT", nullptr, 0xFFFFFFFFu);

    const bool changed =
        n <= 80 ||
        numViews != g_lastLoggedRtvCount ||
        resourcePtr != g_lastLoggedRtvResourcePtr ||
        (gotDesc && (desc.Width != g_lastLoggedRtvWidth ||
                     desc.Height != g_lastLoggedRtvHeight ||
                     desc.Format != g_lastLoggedRtvFormat));

    if (changed && g_omSetRenderTargetsLogCount.load(std::memory_order_relaxed) < 768)
    {
        const auto slot = g_omSetRenderTargetsLogCount.fetch_add(1, std::memory_order_relaxed);
        if (slot < 768)
        {
            std::string line = "[ME3DISC] OMSetRenderTargets call=" + std::to_string(n) +
                               " count=" + std::to_string(numViews) +
                               " caller=" + CallerTag(_ReturnAddress());
            if (gotDesc)
            {
                char buffer[384] = {};
                sprintf_s(buffer,
                          " rtv0=%p tex=%p %ux%u fmt=%u %s bind=0x%X backbuffer=%d dsv=%p",
                          (numViews > 0 && renderTargetViews != nullptr) ? renderTargetViews[0] : nullptr,
                          reinterpret_cast<void*>(resourcePtr),
                          desc.Width,
                          desc.Height,
                          static_cast<unsigned int>(desc.Format),
                          FormatName(desc.Format),
                          desc.BindFlags,
                          resourcePtr == g_backbufferResourcePtr ? 1 : 0,
                          depthStencilView);
                line += buffer;
            }
            else
            {
                char buffer[192] = {};
                sprintf_s(buffer,
                          " rtv0=%p tex=%p desc=unavailable dsv=%p",
                          (numViews > 0 && renderTargetViews != nullptr) ? renderTargetViews[0] : nullptr,
                          reinterpret_cast<void*>(resourcePtr),
                          depthStencilView);
                line += buffer;
            }
            ME2VR::Log::Line(line);

            g_lastLoggedRtvCount = numViews;
            g_lastLoggedRtvResourcePtr = resourcePtr;
            if (gotDesc)
            {
                g_lastLoggedRtvWidth = desc.Width;
                g_lastLoggedRtvHeight = desc.Height;
                g_lastLoggedRtvFormat = desc.Format;
            }
        }
    }

    if (g_traceAfterFullSizeStereoBind.load(std::memory_order_acquire) != 0)
    {
        char detail[256] = {};
        if (gotDesc)
        {
            sprintf_s(detail,
                      "count=%u rtv0=%p tex=%p %ux%u f%u dsv=%p",
                      numViews,
                      (numViews > 0 && renderTargetViews != nullptr) ? renderTargetViews[0] : nullptr,
                      reinterpret_cast<void*>(resourcePtr),
                      desc.Width,
                      desc.Height,
                      static_cast<unsigned int>(desc.Format),
                      depthStencilView);
        }
        else
        {
            sprintf_s(detail,
                      "count=%u rtv0=%p tex=%p desc=none dsv=%p",
                      numViews,
                      (numViews > 0 && renderTargetViews != nullptr) ? renderTargetViews[0] : nullptr,
                      reinterpret_cast<void*>(resourcePtr),
                      depthStencilView);
        }
        LogStateTrace("OMSetRenderTargets", _ReturnAddress(), detail);
    }

    SafeRelease(texture);
    SafeRelease(resource);

    HookCostBind(__rdtsc() - hc0);   // [HOOKCOST]
    OMSetRenderTargetsFn original = g_originalOMSetRenderTargets;
    if (original != nullptr) original(context, numViews, renderTargetViews, depthStencilView);
}

void STDMETHODCALLTYPE RSSetScissorRectsHook(ID3D11DeviceContext* context,
                                             UINT numRects,
                                             const D3D11_RECT* rects) noexcept
{
    const auto n = g_scissorSetCount.fetch_add(1, std::memory_order_relaxed) + 1;
    bool rectChanged = false;
    if (numRects > 0 && rects != nullptr)
    {
        const D3D11_RECT& rect = rects[0];
        rectChanged =
            !g_haveLastLoggedScissor ||
            rect.left != g_lastLoggedScissor.left ||
            rect.top != g_lastLoggedScissor.top ||
            rect.right != g_lastLoggedScissor.right ||
            rect.bottom != g_lastLoggedScissor.bottom;
    }

    const bool shouldLog = n <= 120 || numRects != 1 || rectChanged;
    if (shouldLog && g_scissorLogCount.load(std::memory_order_relaxed) < 48)
    {
        const auto slot = g_scissorLogCount.fetch_add(1, std::memory_order_relaxed);
        if (slot < 48)
        {
            std::string line = "[ME3DISC] RSSetScissorRects call=" + std::to_string(n) +
                               " count=" + std::to_string(numRects) +
                               " caller=" + CallerTag(_ReturnAddress());
            const UINT maxLog = (std::min)(numRects, 4u);
            for (UINT i = 0; i < maxLog && rects != nullptr; ++i)
            {
                char buffer[192] = {};
                sprintf_s(buffer,
                          " rect%u={l=%ld t=%ld r=%ld b=%ld w=%ld h=%ld}",
                          i,
                          rects[i].left,
                          rects[i].top,
                          rects[i].right,
                          rects[i].bottom,
                          rects[i].right - rects[i].left,
                          rects[i].bottom - rects[i].top);
                line += buffer;
            }
            ME2VR::Log::Line(line);

            if (IsInterestingScissor(numRects, rects) &&
                g_stackLogCount.fetch_add(1, std::memory_order_relaxed) < 96)
            {
                ME2VR::Log::Line("[ME3DISC] STACK RSSetScissorRects call=" + std::to_string(n) +
                                 " " + StackTag());
            }

            if (numRects > 0 && rects != nullptr)
            {
                g_lastLoggedScissor = rects[0];
                g_haveLastLoggedScissor = true;
            }
        }
    }

    if (g_traceAfterFullSizeStereoBind.load(std::memory_order_acquire) != 0)
    {
        char detail[256] = {};
        if (numRects > 0 && rects != nullptr)
        {
            sprintf_s(detail,
                      "count=%u rect0=%ld,%ld %ldx%ld",
                      numRects,
                      rects[0].left,
                      rects[0].top,
                      rects[0].right - rects[0].left,
                      rects[0].bottom - rects[0].top);
        }
        else
        {
            sprintf_s(detail, "count=%u", numRects);
        }
        LogStateTrace("RSSetScissorRects", _ReturnAddress(), detail);
    }

    RSSetScissorRectsFn original = g_originalRSSetScissorRects;
    if (original != nullptr) original(context, numRects, rects);
}

// [SQUAREBUF] Force the swapchain to the square size, at the one place the buffer size is actually
// decided.
//
// Persuading the game to render square via its own config does NOT work, and the log proves it: the mod
// wrote ResX/ResY=3072x3072 and armed the monitor spoof to 3072x3072, and the game still created a
// 3072x1620 buffer - exactly its own 1.896:1 aspect fitted to the reported width - then rewrote
// GamerSettings back to 6144x3240 itself. It keeps its aspect and derives height, so every Win32
// display spoof just yields a narrower rectangle and the same letterbox.
//
// So stop negotiating and edit the request in flight. The engine sizes its render targets, viewport
// and projection from the swapchain it gets back, so a square buffer makes the whole chain square and
// the FOV the mod publishes (read from the resulting projection matrix) stays self-consistent.
// Set by ArmResolutionSpoof at attach (which runs long before any swapchain is created). Kept
// separate from the DISPQ spoof globals purely because those are defined further down this file.
std::atomic<UINT> g_squareBufW{0};
std::atomic<UINT> g_squareBufH{0};

// DISABLED. Forcing a square SWAPCHAIN does not force a square RENDER: the game kept drawing its
// 3072x1620 viewport into the top of the square buffer, so the letterbox became a black bottom half
// instead of two bands. The buffer is not where the aspect is decided - the engine's viewport is -
// and enlarging the buffer alone only moves the black. Left in place, returning false, because the
// measurement it produced is worth keeping next to the code it disproves.
bool DispqSquareTarget(UINT* w, UINT* h) noexcept
{
    (void)w; (void)h;
    return false;
}

HRESULT STDMETHODCALLTYPE CreateSwapChainHook(IDXGIFactory* factory,
                                              IUnknown* device,
                                              DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** swapChain) noexcept
{
    CreateSwapChainFn original = g_originalCreateSwapChain;
    if (original == nullptr) return DXGI_ERROR_INVALID_CALL;
    UINT sw = 0, sh = 0;
    if (desc != nullptr && DispqSquareTarget(&sw, &sh) &&
        (desc->BufferDesc.Width != sw || desc->BufferDesc.Height != sh))
    {
        char l[192];
        std::snprintf(l, sizeof(l), "[SQUAREBUF] CreateSwapChain %ux%u -> %ux%u",
                      desc->BufferDesc.Width, desc->BufferDesc.Height, sw, sh);
        ME2VR::Log::Line(l);
        desc->BufferDesc.Width = sw;
        desc->BufferDesc.Height = sh;
    }
    const HRESULT hr = original(factory, device, desc, swapChain);
    ME2VR::Log::Line("[ME3DISC] CreateSwapChain returned " + HexHRESULT(hr));
    if (SUCCEEDED(hr) && swapChain != nullptr && *swapChain != nullptr) OnSwapChainCreated(*swapChain);
    return hr;
}

HRESULT STDMETHODCALLTYPE CreateSwapChainForHwndHook(IDXGIFactory2* factory,
                                                     IUnknown* device,
                                                     HWND hwnd,
                                                     const DXGI_SWAP_CHAIN_DESC1* desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                                     IDXGIOutput* output,
                                                     IDXGISwapChain1** swapChain) noexcept
{
    CreateSwapChainForHwndFn original = g_originalCreateSwapChainForHwnd;
    if (original == nullptr) return DXGI_ERROR_INVALID_CALL;
    // [SQUAREBUF] same override; desc is const here, so edit a copy and pass that.
    DXGI_SWAP_CHAIN_DESC1 descCopy = {};
    UINT sw = 0, sh = 0;
    if (desc != nullptr && DispqSquareTarget(&sw, &sh) && (desc->Width != sw || desc->Height != sh))
    {
        char l[192];
        std::snprintf(l, sizeof(l), "[SQUAREBUF] CreateSwapChainForHwnd %ux%u -> %ux%u",
                      desc->Width, desc->Height, sw, sh);
        ME2VR::Log::Line(l);
        descCopy = *desc;
        descCopy.Width = sw;
        descCopy.Height = sh;
        desc = &descCopy;
    }
    const HRESULT hr = original(factory, device, hwnd, desc, fullscreenDesc, output, swapChain);
    ME2VR::Log::Line("[ME3DISC] CreateSwapChainForHwnd returned " + HexHRESULT(hr));
    if (SUCCEEDED(hr) && swapChain != nullptr && *swapChain != nullptr) OnSwapChainCreated(*swapChain);
    return hr;
}

HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindowHook(IDXGIFactory2* factory,
                                                           IUnknown* device,
                                                           IUnknown* window,
                                                           const DXGI_SWAP_CHAIN_DESC1* desc,
                                                           IDXGIOutput* output,
                                                           IDXGISwapChain1** swapChain) noexcept
{
    CreateSwapChainForCoreWindowFn original = g_originalCreateSwapChainForCoreWindow;
    if (original == nullptr) return DXGI_ERROR_INVALID_CALL;
    const HRESULT hr = original(factory, device, window, desc, output, swapChain);
    ME2VR::Log::Line("[ME3DISC] CreateSwapChainForCoreWindow returned " + HexHRESULT(hr));
    if (SUCCEEDED(hr) && swapChain != nullptr && *swapChain != nullptr) OnSwapChainCreated(*swapChain);
    return hr;
}

HRESULT STDMETHODCALLTYPE CreateSwapChainForCompositionHook(IDXGIFactory2* factory,
                                                           IUnknown* device,
                                                           const DXGI_SWAP_CHAIN_DESC1* desc,
                                                           IDXGIOutput* output,
                                                           IDXGISwapChain1** swapChain) noexcept
{
    CreateSwapChainForCompositionFn original = g_originalCreateSwapChainForComposition;
    if (original == nullptr) return DXGI_ERROR_INVALID_CALL;
    const HRESULT hr = original(factory, device, desc, output, swapChain);
    ME2VR::Log::Line("[ME3DISC] CreateSwapChainForComposition returned " + HexHRESULT(hr));
    if (SUCCEEDED(hr) && swapChain != nullptr && *swapChain != nullptr) OnSwapChainCreated(*swapChain);
    return hr;
}

// ============================================================================
// [DISPQ] Render above display resolution - ported from ME1/ME2. LE3 clamps its render resolution to the
// primary monitor rect, read via GetMonitorInfoW, before DXGI ever sees a size. Spoofing that one
// call makes the game render at whatever ResX/ResY is in GamerSettings.ini even if it's bigger than
// the physical monitor - the fix for "the VR image is soft" on sub-4K systems, since the mod only
// captures the backbuffer the game already rendered. Requires BorderlessWindow=True (a titled window
// clamps its client HEIGHT to the real desktop -> vertical squish). Safety: arms only when the ini
// target exceeds the real desktop, and only rewrites the real primary rect (a 2nd monitor is left alone).
// ============================================================================
UINT g_dispqRealPrimaryW = 0;
UINT g_dispqRealPrimaryH = 0;
UINT g_dispqSpoofW = 0;   // 0 = spoof disabled
UINT g_dispqSpoofH = 0;

bool DispqSpoofActive() noexcept { return g_dispqSpoofW != 0 && g_dispqSpoofH != 0; }

bool DispqIsPrimaryRect(const RECT& r) noexcept
{
    return r.left == 0 && r.top == 0 &&
           static_cast<UINT>(r.right - r.left) == g_dispqRealPrimaryW &&
           static_cast<UINT>(r.bottom - r.top) == g_dispqRealPrimaryH;
}

using DispqGetMonitorInfoWFn = BOOL(WINAPI*)(HMONITOR, LPMONITORINFO);
using DispqGetMonitorInfoAFn = BOOL(WINAPI*)(HMONITOR, LPMONITORINFO);
using DispqEnumDisplaySettingsWFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DEVMODEW*);
using DispqEnumDisplaySettingsAFn = BOOL(WINAPI*)(LPCSTR, DWORD, DEVMODEA*);
using DispqGetSystemMetricsFn = int(WINAPI*)(int);
using DispqGetDeviceCapsFn = int(WINAPI*)(HDC, int);

DispqGetMonitorInfoWFn g_origGetMonitorInfoW = nullptr;
DispqGetMonitorInfoAFn g_origGetMonitorInfoA = nullptr;
DispqEnumDisplaySettingsWFn g_origEnumDisplaySettingsW = nullptr;
DispqEnumDisplaySettingsAFn g_origEnumDisplaySettingsA = nullptr;
DispqGetSystemMetricsFn g_origGetSystemMetrics = nullptr;
DispqGetDeviceCapsFn g_origGetDeviceCaps = nullptr;

std::atomic<int> g_dispqMonInfoLogs{0};

// THE mechanism. LE3 asks for the primary monitor's rect; hand back the spoofed size.
BOOL WINAPI DispqGetMonitorInfoWHook(HMONITOR mon, LPMONITORINFO mi) noexcept
{
    const BOOL ok = (g_origGetMonitorInfoW != nullptr) ? g_origGetMonitorInfoW(mon, mi) : FALSE;
    bool rewrote = false;
    if (ok && mi != nullptr && DispqSpoofActive() &&
        (mi->dwFlags & MONITORINFOF_PRIMARY) != 0 && DispqIsPrimaryRect(mi->rcMonitor))
    {
        mi->rcMonitor.right = mi->rcMonitor.left + static_cast<LONG>(g_dispqSpoofW);
        mi->rcMonitor.bottom = mi->rcMonitor.top + static_cast<LONG>(g_dispqSpoofH);
        mi->rcWork.right = mi->rcWork.left + static_cast<LONG>(g_dispqSpoofW);
        mi->rcWork.bottom = mi->rcWork.top + static_cast<LONG>(g_dispqSpoofH);
        rewrote = true;
    }
    const int n = g_dispqMonInfoLogs.fetch_add(1, std::memory_order_relaxed);
    if (n < 16 && ok && mi != nullptr)
    {
        const RECT& r = mi->rcMonitor;
        char line[192];
        std::snprintf(line, sizeof(line),
                      "[DISPQ] GetMonitorInfoW -> rcMonitor=%ldx%ld primary=%d spoofed=%d",
                      r.right - r.left, r.bottom - r.top,
                      (mi->dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0, rewrote ? 1 : 0);
        ME2VR::Log::Line(line);
    }
    return ok;
}

BOOL WINAPI DispqGetMonitorInfoAHook(HMONITOR mon, LPMONITORINFO mi) noexcept
{
    const BOOL ok = (g_origGetMonitorInfoA != nullptr) ? g_origGetMonitorInfoA(mon, mi) : FALSE;
    if (ok && mi != nullptr && DispqSpoofActive() &&
        (mi->dwFlags & MONITORINFOF_PRIMARY) != 0 && DispqIsPrimaryRect(mi->rcMonitor))
    {
        mi->rcMonitor.right = mi->rcMonitor.left + static_cast<LONG>(g_dispqSpoofW);
        mi->rcMonitor.bottom = mi->rcMonitor.top + static_cast<LONG>(g_dispqSpoofH);
        mi->rcWork.right = mi->rcWork.left + static_cast<LONG>(g_dispqSpoofW);
        mi->rcWork.bottom = mi->rcWork.top + static_cast<LONG>(g_dispqSpoofH);
    }
    return ok;
}

BOOL WINAPI DispqEnumDisplaySettingsWHook(LPCWSTR device, DWORD modeNum, DEVMODEW* dm) noexcept
{
    const BOOL ok = (g_origEnumDisplaySettingsW != nullptr) ? g_origEnumDisplaySettingsW(device, modeNum, dm) : FALSE;
    if (ok && dm != nullptr && DispqSpoofActive() &&
        dm->dmPelsWidth == g_dispqRealPrimaryW && dm->dmPelsHeight == g_dispqRealPrimaryH)
    {
        dm->dmPelsWidth = g_dispqSpoofW;
        dm->dmPelsHeight = g_dispqSpoofH;
    }
    return ok;
}

BOOL WINAPI DispqEnumDisplaySettingsAHook(LPCSTR device, DWORD modeNum, DEVMODEA* dm) noexcept
{
    const BOOL ok = (g_origEnumDisplaySettingsA != nullptr) ? g_origEnumDisplaySettingsA(device, modeNum, dm) : FALSE;
    if (ok && dm != nullptr && DispqSpoofActive() &&
        dm->dmPelsWidth == g_dispqRealPrimaryW && dm->dmPelsHeight == g_dispqRealPrimaryH)
    {
        dm->dmPelsWidth = g_dispqSpoofW;
        dm->dmPelsHeight = g_dispqSpoofH;
    }
    return ok;
}

int WINAPI DispqGetSystemMetricsHook(int index) noexcept
{
    int value = (g_origGetSystemMetrics != nullptr) ? g_origGetSystemMetrics(index) : 0;
    if (DispqSpoofActive())
    {
        if ((index == SM_CXSCREEN || index == SM_CXFULLSCREEN) && value == static_cast<int>(g_dispqRealPrimaryW))
            value = static_cast<int>(g_dispqSpoofW);
        else if ((index == SM_CYSCREEN || index == SM_CYFULLSCREEN) && value == static_cast<int>(g_dispqRealPrimaryH))
            value = static_cast<int>(g_dispqSpoofH);
    }
    return value;
}

int WINAPI DispqGetDeviceCapsHook(HDC hdc, int index) noexcept
{
    return (g_origGetDeviceCaps != nullptr) ? g_origGetDeviceCaps(hdc, index) : 0;
}

// ...\Game\ME3\Binaries\Win64\MassEffect3.exe -> ...\Game\ME3\BioGame\Config\GamerSettings.ini
std::wstring DispqGameConfigPath() noexcept
{
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return L"";
    std::wstring path(exePath);
    const size_t cut = path.rfind(L"\\Binaries\\");
    if (cut == std::wstring::npos) return L"";
    return path.substr(0, cut) + L"\\BioGame\\Config\\GamerSettings.ini";
}

// Rewrite ResX/ResY in GamerSettings.ini in place (the GAME owns its render size; the DISPQ spoof only
// PERMITS a size larger than the monitor). Each matching line is hand-edited so the rest of the file is
// byte-preserved. Called once at attach, before the engine reads its config.
bool DispqWriteRes(UINT w, UINT h) noexcept
{
    const std::wstring path = DispqGameConfigPath();
    if (path.empty() || w < 640 || h < 360) return false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr) return false;
    std::string text;
    char buf[512];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);

    std::string out;
    out.reserve(text.size() + 32);
    size_t i = 0;
    bool changed = false;
    while (i < text.size())
    {
        size_t eol = text.find('\n', i);
        const size_t lineEnd = (eol == std::string::npos) ? text.size() : eol + 1;
        std::string line = text.substr(i, lineEnd - i);
        size_t s = 0; while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
        auto keyIs = [&](const char* key) {
            size_t k = s, j = 0;
            while (key[j] && k < line.size() && line[k] == key[j]) { ++k; ++j; }
            if (key[j] != '\0') return false;
            while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
            return k < line.size() && line[k] == '=';
        };
        if (keyIs("ResX")) { char t[32]; std::snprintf(t, sizeof(t), "ResX=%u\r\n", w); out += t; changed = true; }
        else if (keyIs("ResY")) { char t[32]; std::snprintf(t, sizeof(t), "ResY=%u\r\n", h); out += t; changed = true; }
        else out += line;
        i = lineEnd;
    }
    if (!changed) return false;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || f == nullptr) return false;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return true;
}

// [DOFWASH] Write the DepthOfField key in GamerSettings.ini. This USED to default to False on the
// reasoning that blur outside a focal plane reads as "eyes will not focus" in a headset. That was
// wrong, and it cost most of a day on ME2 before the cause was found: in UE3 depth of field, motion
// blur and TONEMAPPING share the same uber-post-process pass, so DepthOfField=False selects a shader
// permutation that skips the tonemap entirely. The frame comes out ungraded - washed out, flat, on
// every runtime. Depth of field costs far less than a broken tonemapper, so the default is now False
// for the KEY meaning "leave DoF on". Same byte-preserving hand-edit;
// only rewrites when the value actually differs, so the mod never churns the file (or the engine's shader
// permutations) when it is already correct. LE ships this key absent, so it is appended if missing.
bool DispqWriteBoolKey(const char* key, bool value) noexcept
{
    const std::wstring path = DispqGameConfigPath();
    if (path.empty() || key == nullptr) return false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr) return false;
    std::string text;
    char buf[512];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);

    const char* want = value ? "True" : "False";
    std::string out;
    out.reserve(text.size() + 64);
    size_t i = 0;
    bool found = false, changed = false;
    size_t sysSettingsEnd = std::string::npos;
    while (i < text.size())
    {
        size_t eol = text.find('\n', i);
        const size_t lineEnd = (eol == std::string::npos) ? text.size() : eol + 1;
        std::string line = text.substr(i, lineEnd - i);
        size_t s = 0; while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
        size_t k = s, j = 0;
        while (key[j] && k < line.size() && line[k] == key[j]) { ++k; ++j; }
        bool isKey = (key[j] == '\0');
        if (isKey)
        {
            size_t q = k; while (q < line.size() && (line[q] == ' ' || line[q] == '\t')) ++q;
            isKey = (q < line.size() && line[q] == '=');
        }
        if (isKey)
        {
            found = true;
            const bool already = (line.find(want) != std::string::npos);
            if (already) { out += line; }
            else { char t[96]; std::snprintf(t, sizeof(t), "%s=%s\r\n", key, want); out += t; changed = true; }
        }
        else
        {
            out += line;
            if (line.find("[SystemSettings]") != std::string::npos) sysSettingsEnd = out.size();
        }
        i = lineEnd;
    }
    if (!found && sysSettingsEnd != std::string::npos)
    {
        char t[96]; std::snprintf(t, sizeof(t), "%s=%s\r\n", key, want);
        out.insert(sysSettingsEnd, t);
        changed = true;
    }
    if (!changed) return false;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || f == nullptr) return false;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return true;
}

bool DispqReadRequestedRes(UINT* outW, UINT* outH) noexcept
{
    const std::wstring path = DispqGameConfigPath();
    if (path.empty()) return false;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || f == nullptr) return false;

    UINT w = 0, h = 0;
    char buf[256];
    while (fgets(buf, sizeof(buf), f) != nullptr)
    {
        unsigned v = 0;
        if (sscanf_s(buf, " ResX = %u", &v) == 1 || sscanf_s(buf, " ResX=%u", &v) == 1) w = v;
        else if (sscanf_s(buf, " ResY = %u", &v) == 1 || sscanf_s(buf, " ResY=%u", &v) == 1) h = v;
    }
    fclose(f);

    if (w == 0 || h == 0) return false;
    *outW = w;
    *outH = h;
    return true;
}

void DispqInstallHook(void* target, void* hook, void** original, const char* name) noexcept
{
    if (target == nullptr || hook == nullptr || original == nullptr) return;
    const MH_STATUS created = MH_CreateHook(target, hook, original);
    if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED)
    {
        ME2VR::Log::Line(std::string("[DISPQ] MH_CreateHook failed for ") + name +
                         " status=" + std::to_string(static_cast<int>(created)));
        return;
    }
    const MH_STATUS enabled = MH_EnableHook(target);
    if (enabled != MH_OK && enabled != MH_ERROR_ENABLED)
    {
        ME2VR::Log::Line(std::string("[DISPQ] MH_EnableHook failed for ") + name +
                         " status=" + std::to_string(static_cast<int>(enabled)));
        return;
    }
    ME2VR::Log::Line(std::string("[DISPQ] hooked ") + name);
}
}

namespace ME2VR::D3DCapture
{
ID3D11Device* GetGameDevice() noexcept { return g_gameDevice; }
IDXGISwapChain* GetGameSwapChain() noexcept { return g_gameSwapChain; }
// [SFR] the held pass-0 image = the LEFT eye. Null until the first scene-sized clear of a replay
// present, so the submit falls back to mono rather than showing a black or stale left eye.
ID3D11Texture2D* GetSfrPass0Texture() noexcept { return g_sfrPass0Tex; }
// [SFRFRESH] presents since the last left-eye capture. Large => no scene is rendering (loading
// screen, movie), so an SFR pair would be half-stale and the caller should present mono instead.
uint64_t SfrCaptureAgePresents() noexcept
{
    const uint64_t now = g_presentIndex.load(std::memory_order_relaxed);
    const uint64_t last = g_sfrLastCapturePresent.load(std::memory_order_relaxed);
    return (now > last) ? (now - last) : 0;
}
// [FREEZETAG] Monotonic count of completed pass-0 captures. The submit compares it against the value
// it saw last present: unchanged means the eye pair is the SAME PIXELS as last time, so re-tagging
// them with a newly located pose would tell the compositor those old pixels belong to a new head
// position and defeat its reprojection. Age in presents (above) only catches a long stall; this
// catches the ordinary single dropped frame, which is the common case once the frame rate is under
// the present rate.
uint64_t GetSfrPass0CaptureSeq() noexcept { return g_sfrPass0Caps.load(std::memory_order_relaxed); }
unsigned GetBackbufferWidth() noexcept { return g_backbufferWidth; }
unsigned GetBackbufferHeight() noexcept { return g_backbufferHeight; }
void UpdatePresentationState() noexcept
{
    enum : int { Gameplay = 0, Manual = 1, InputOwner = 2, Galaxy = 3 };

    const bool manual = g_manualFlat.load(std::memory_order_acquire);
    const bool autoMenus = g_monoMenus.load(std::memory_order_acquire);
    unsigned char ignoreMove = 0, ignoreLook = 0;
    const bool inputOwned = ME2VR::EngineProbe::IsMenuInputOwned(&ignoreMove, &ignoreLook);
    const bool insertOnlyInputOwner = ME2VR::Menu::IsSoleInputLockOwner();
    const bool nativeInputOwned = inputOwned && !insertOnlyInputOwner;
    const bool gameplayCam = ME2VR::EngineProbe::IsGameplayCamera();
    const bool convoCam = ME2VR::EngineProbe::IsConvoCamera();
    const bool fpConvo = ME2VR::ConvoFp::OwnsCamera();
    // [CINETAIL2 2026-08-16] The cutscene's input locks outlive its camera by about 1-2 seconds.
    // The old 90-present timer guessed when ownership ended; on Tuchanka it expired at 19:12:38.760
    // while the same 3/3 locks were still continuously held, creating a one-second mono flash AFTER
    // the VR cutscene had already returned to gameplay. Ownership has a real boundary: lock release.
    // Latch provenance whenever a cinematic owns native locks, keep it while those exact locks remain
    // continuously asserted, and clear only on release or a positively named full-screen menu. This
    // cannot time out late or early and a fresh menu lock edge after release starts unambiguously.
    const bool namedGui = ME2VR::EngineProbe::GetNamedGuiActive();
    const bool cineNow = ME2VR::CalcViewHook::GetVrCineActive() || ME2VR::CalcViewHook::GetCinematic();
    static bool s_cineLocksInherited = false;
    if (!nativeInputOwned || namedGui) s_cineLocksInherited = false;
    else if (cineNow) s_cineLocksInherited = true;
    const bool cineLocksInherited = s_cineLocksInherited;
    // [SCRIPTLOCK 2026-08-14] The input locks are NOT menu-specific: the game asserts the same two
    // fields for any moment it takes control away, which includes ordinary gameplay. Measured on the
    // Atlas: entering it, exiting it and firing the LT missile each locked input under a gameplay
    // camera, so all three read as "menu" and dropped the player to mono mid-fight (05:41:24-51,
    // episodes of 0.8-4.6s). Game mode alone cannot fix it - across that session modes 0, 2, 8, 9
    // and 10 ALL appeared in both mono and stereo frames.
    // But the two signals the mod has cover each other's gaps exactly:
    //   - The Atlas mono was entirely gm 0 (Default) and gm 2 (WeaponWheel). Modes 0-4 are the
    //     engine's own GAMEPLAY family (Default/PowerWheel/WeaponWheel/Command/Vehicle) - the same
    //     family LE2 treats as definitively-not-cinematic in [GAMEPLAYVR].
    //   - The one full-screen menu family measured to live at gm 0 (squad/bench/team select, the
    //     2026-08-13 finding that killed every game-mode heuristic) is precisely what the named-GUI
    //     census DOES cover.
    // So: a gameplay-family mode only counts as a menu when a named full-screen menu object is
    // actually present. Menus outside that family (journal/pause/datapad at gm 7/8/9/10) are
    // unaffected and still go mono on the first present, with no added latency.
    const int gmNow = g_autoGameMode.load(std::memory_order_acquire);
    const bool gameplayModeFamily = (gmNow >= 0 && gmNow <= 4);
    // [WHEELVR 2026-08-16] The native weapon wheel reports the same gm 9 + both-input-locks +
    // namedGui 0 tuple as several real flat menus, so its physical L1/LB owner is the discriminator.
    //
    // [WHEELEDGE 2026-08-17] A fresh sample is still not sufficient ownership. The mission-setup
    // session kept polling XInput and reporting LB held as the loading movie returned to gm 9. That
    // made the first outfit window stereo from 11:28:32.603 until its named UObject appeared three
    // seconds later. Qualify the hold by provenance instead: its UP->DOWN edge must originate after
    // at least one present of unlocked, non-cinematic gameplay. Keep that ownership until release.
    // Thus a held/reappearing bit across a load cannot exempt Personalization, while an actual wheel
    // press made during gameplay still owns VR even if the game changes mode and locks input before
    // the following present.
    const bool rawWheelButtonHeld = ME2VR::Menu::PadLeftShoulderHeld();
    static bool s_wheelButtonWasHeld = false;
    static bool s_weaponWheelOwned = false;
    static unsigned s_cleanGameplayPresents = 0;
    if (!rawWheelButtonHeld)
    {
        s_weaponWheelOwned = false;
    }
    else if (!s_wheelButtonWasHeld)
    {
        s_weaponWheelOwned = s_cleanGameplayPresents != 0;
        char b[192] = {};
        sprintf_s(b, "[WHEELOWNER] LB edge: qualified=%d priorGameplay=%u gm=%d locks=%u/%u cine=%d",
                  s_weaponWheelOwned ? 1 : 0, s_cleanGameplayPresents, gmNow,
                  static_cast<unsigned>(ignoreMove), static_cast<unsigned>(ignoreLook), cineNow ? 1 : 0);
        ME2VR::Log::Line(b);
    }
    s_wheelButtonWasHeld = rawWheelButtonHeld;
    const bool weaponWheelHeld = rawWheelButtonHeld && s_weaponWheelOwned;

    const bool cleanGameplayNow = gameplayModeFamily && !nativeInputOwned && gameplayCam &&
                                  !convoCam && !cineNow &&
                                  !g_menuPresentation.load(std::memory_order_acquire);
    if (cleanGameplayNow)
    {
        if (s_cleanGameplayPresents < 1000) ++s_cleanGameplayPresents;
    }
    else s_cleanGameplayPresents = 0;

    // [OUTFITTOPO 2026-08-17] The mission outfit/weapon preview convicts itself before its named
    // UObject exists: it builds exactly 24 scene views per present while ordinary stereo/menu
    // traffic in the same run is 2..7. The bad window was 11:40:55.626..58.636, entirely 24-pass,
    // gm 9, gameplay camera and native input locks. Use the completed present as a one-frame-late
    // positive owner. The remaining gates keep cutscenes and scripted gameplay out, and a qualified
    // physical wheel press wins explicitly, preserving the field-validated weapon-wheel fix.
    const unsigned completedScenePasses =
        g_lastCompletedScenePasses.load(std::memory_order_acquire);
    const bool outfitCompositeOwner = autoMenus && nativeInputOwned && gameplayCam && !convoCam && !fpConvo &&
                                      !cineNow && !weaponWheelHeld && gmNow == 9 &&
                                      completedScenePasses >= 12;
    const bool ordinaryMenuOwner = autoMenus && nativeInputOwned && gameplayCam && !convoCam && !fpConvo &&
                                   !cineLocksInherited &&
                                   (namedGui || !gameplayModeFamily) && !weaponWheelHeld;
    const bool menuInputOwner = outfitCompositeOwner || ordinaryMenuOwner;
    // If a gameplay-family lock is ever suppressed for a long stretch, that is either a scripted
    // sequence working correctly or a menu this rule cannot see. Name it once per episode so a
    // missed menu convicts itself in the log instead of only in the headset.
    {
        static int s_suppressRun = 0;
        const bool suppressed = autoMenus && nativeInputOwned && gameplayCam && !convoCam &&
                                !cineLocksInherited && !namedGui && gameplayModeFamily;
        if (suppressed)
        {
            if (++s_suppressRun == 240)   // ~3s, longer than any measured scripted sequence
                ME2VR::Log::Line("[SCRIPTLOCK] gameplay-family input lock held 3s+ without a named "
                                 "menu (gm " + std::to_string(gmNow) + ") - scripted sequence, or a "
                                 "menu the census cannot see");
        }
        else s_suppressRun = 0;
    }
    const bool galaxy = autoMenus && !fpConvo && ME2VR::EngineProbe::IsGalaxyMapOpen();
    const int demand = manual ? Manual : menuInputOwner ? InputOwner : galaxy ? Galaxy : Gameplay;
    const bool wasMenu = g_menuPresentation.load(std::memory_order_acquire);

    static int previousInput = -1;
    const int inputState = (static_cast<int>(ignoreMove) << 16) |
                           (static_cast<int>(ignoreLook) << 8) |
                           (insertOnlyInputOwner ? 1 : 0);
    if (inputState != previousInput)
    {
        previousInput = inputState;
        char b[160] = {};
        sprintf_s(b, "[INPUTOWNER] move=%u look=%u insertOnly=%d gameplayCam=%d convoCam=%d",
                  static_cast<unsigned>(ignoreMove), static_cast<unsigned>(ignoreLook),
                  insertOnlyInputOwner ? 1 : 0,
                  gameplayCam ? 1 : 0, convoCam ? 1 : 0);
        ME2VR::Log::Line(b);
    }

    if (demand != Gameplay)
    {
        g_menuPresentationReason.store(demand, std::memory_order_release);
        if (!wasMenu)
        {
            LARGE_INTEGER xt0; QueryPerformanceCounter(&xt0);   // [XSTATE]
            g_menuPresentation.store(true, std::memory_order_release);
            static const char* names[] = { "gameplay", "manual", "input owner", "galaxy owner" };
            // Log the deciding signals with the transition. Attributing a mono episode to a SCREEN
            // previously meant correlating this line against [GUICEN] timestamps by hand, or asking
            // the player to remember what they opened - and most screens (map, photo mode, ambience,
            // yes/no prompts, store) create no census-visible object at all, so the object log alone
            // cannot name them. Carrying the inputs here makes every episode self-describing.
            char b[256] = {};
            sprintf_s(b, "[PRESENTATION] MENU_MONO enter: %s  (gm %d, gameplayCam %d, convoCam %d, "
                         "namedGui %d, locks %u/%u, cineLocks %d, wheelHeld %d, scenePasses %u)",
                      names[demand], gmNow, gameplayCam ? 1 : 0, convoCam ? 1 : 0,
                      namedGui ? 1 : 0, static_cast<unsigned>(ignoreMove),
                      static_cast<unsigned>(ignoreLook), cineLocksInherited ? 1 : 0,
                      weaponWheelHeld ? 1 : 0, completedScenePasses);
            ME2VR::Log::Line(b);
            LARGE_INTEGER xt1; QueryPerformanceCounter(&xt1);
            XsNoteTransition(xt0, xt1, false);
        }
        return;
    }

    if (!wasMenu) return;
    // [MENUSTUCK 2026-08-14] Release as soon as nothing owns the screen. Reaching this line already
    // proves demand == Gameplay, i.e. manual is off, galaxy is off and menuInputOwner is false - so
    // the old extra conditions were partly redundant and partly a TRAP.
    // The trap: `gameplayCam &&` was required to RELEASE, but entry already requires gameplayCam too.
    // When a cutscene starts the camera leaves the SFXCameraMode system and gameplayCam goes 0, so a
    // menu latched before that moment could never satisfy the release and MENU_MONO stuck for the
    // whole scene. Measured on the Omega DLC cutscene 2026-08-14 05:06:57: every cine signal read
    // cinematic and VR-eligible (gm 7, convoCam 0, gameplayCam 0, sceneAlive 1, fov 50) yet
    // [CINEHB] reported menuMode 1 continuously for 9+ seconds and the scene played flat. That is
    // the "randomly a cutscene goes flat" report - it depended purely on whether a menu or input
    // lock happened to be up when the scene began.
    // Cutscenes assert the same input locks menus do, which is WHY entry keys on the gameplay
    // camera; that entry guard is untouched, so a cutscene still cannot enter menu mono.
    LARGE_INTEGER xt0; QueryPerformanceCounter(&xt0);   // [XSTATE]
    g_menuPresentationReason.store(Gameplay, std::memory_order_release);
    g_menuPresentation.store(false, std::memory_order_release);
    ME2VR::Log::Line(std::string("[PRESENTATION] GAMEPLAY_STEREO enter: no owner (gameplayCam ") +
                     (gameplayCam ? "1" : "0") + ", inputOwned " + (nativeInputOwned ? "1" : "0") + ")");
    LARGE_INTEGER xt1; QueryPerformanceCounter(&xt1);
    XsNoteTransition(xt0, xt1, false);
}
bool GetMenuMode() noexcept { return g_menuPresentation.load(std::memory_order_acquire); }
void NoteCineTransition(const LARGE_INTEGER& t0, const LARGE_INTEGER& t1) noexcept { XsNoteTransition(t0, t1, true); }
bool GetMenuModeManual() noexcept { return g_manualFlat.load(std::memory_order_acquire); }
void SetAutoGameMode(int mode) noexcept { g_autoGameMode.store(mode, std::memory_order_release); }
int  GetAutoGameMode() noexcept { return g_autoGameMode.load(std::memory_order_acquire); }
bool GetMonoMenus() noexcept { return g_monoMenus.load(std::memory_order_acquire); }
void SetMonoMenus(bool on) noexcept { g_monoMenus.store(on, std::memory_order_release); }
int  GetMirrorPresentEvery() noexcept { return static_cast<int>(g_mirrorPresentEvery.load(std::memory_order_relaxed)); }
void SetMirrorPresentEvery(int n) noexcept
{
    if (n <= 0) n = static_cast<int>(kMirrorPresentEveryDefault);   // 0/unset -> default
    if (n > 32) n = 32;
    g_mirrorPresentEvery.store(static_cast<uint64_t>(n), std::memory_order_relaxed);
}
bool GetUiDupEnabled() noexcept { return g_uiDupEnabled.load(std::memory_order_acquire); }
void SetUiDupEnabled(bool on) noexcept { g_uiDupEnabled.store(on, std::memory_order_release); }

// Call from DllMain (DLL_PROCESS_ATTACH), BEFORE the worker thread - file I/O + GetSystemMetrics only,
// safe under the loader lock. The engine reads GamerSettings.ini during its own init ahead of any thread.
void ArmResolutionSpoof() noexcept
{
    g_dispqRealPrimaryW = static_cast<UINT>(GetSystemMetrics(SM_CXSCREEN));
    g_dispqRealPrimaryH = static_cast<UINT>(GetSystemMetrics(SM_CYSCREEN));

    // [SQUARERES] The render size comes from the GAME's GamerSettings.ini, so an in-menu resolution
    // choice is stored in THE MOD'S ini and stamped into theirs here, before the engine reads its config
    // (hence restart-to-apply). A SQUARE render is the recommended shape: the eye buffers are roughly
    // square, so rendering 16:9 wastes pixels horizontally and stretches everything vertically.
    {
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) != 0)
        {
            std::wstring ini(exePath);
            const size_t slash = ini.find_last_of(L'\\');
            if (slash != std::wstring::npos)
            {
                ini = ini.substr(0, slash + 1) + L"MELE3VR.ini";
                UINT rw = static_cast<UINT>(GetPrivateProfileIntW(L"VR", L"RenderW", 0, ini.c_str()));
                UINT rh = static_cast<UINT>(GetPrivateProfileIntW(L"VR", L"RenderH", 0, ini.c_str()));

                // NOTHING is forced here any more. Two things were learned the hard way:
                //
                //  1. A square render never fixed the letterbox - the projection did. LE2/LE3 revert a
                //     square ResX/ResY anyway, so forcing one only changed the render SIZE.
                //  2. Changing the render size BREAKS SFR. The pass-0 capture fires on a depth clear
                //     whose target matches the backbuffer exactly; at the 3072x1620 the game produced
                //     from a forced 3072x3072 that match never happened, so nothing was ever captured
                //     (SFRDIAG totalCaps=0), the left eye fell back to the backbuffer and both eyes
                //     showed the same image - VR silently became mono.
                //
                // So the resolution is the player's, honoured only when they explicitly set one.
                if (rw >= 640 && rh >= 360)
                {
                    g_vrresW = rw; g_vrresH = rh;   // re-written at ExitProcess, see ExitProcessHook
                    const bool wrote = DispqWriteRes(rw, rh);
                    char l[224];
                    std::snprintf(l, sizeof(l), "[VRRES] user render override %ux%u%s", rw, rh,
                                  wrote ? " -> wrote GamerSettings.ini" : " -> already correct / write FAILED");
                    ME2VR::Log::Line(l);
                }
                else
                {
                    ME2VR::Log::Line("[VRRES] no render override set - leaving the game's own resolution alone");
                }
                // [DOFOFF] depth of field reads as "eyes won't focus" in a headset.
                // [DOFWASH] default 0. See the DispqWriteBoolKey note: disabling DoF also disables the
                // tonemapper and washes the whole image out. ME2 shipped with this flipped after the
                // same bug; ME3 inherited the bad default in the parity port.
                const bool disableDof = GetPrivateProfileIntW(L"VR", L"DisableDof", 0, ini.c_str()) != 0;
                const bool wroteDof = DispqWriteBoolKey("DepthOfField", !disableDof);
                ME2VR::Log::Line(std::string("[DOFOFF] DisableDof=") + (disableDof ? "1" : "0") +
                                 (wroteDof ? " -> wrote GamerSettings.ini" : " -> already correct (no write)"));
            }
        }
    }

    UINT wantW = 0, wantH = 0;
    const bool haveWant = DispqReadRequestedRes(&wantW, &wantH);

    // Arm ONLY when the target exceeds the desktop - at or below native this is a no-op pass-through.
    if (haveWant && g_dispqRealPrimaryW != 0 && g_dispqRealPrimaryH != 0 &&
        (wantW > g_dispqRealPrimaryW || wantH > g_dispqRealPrimaryH))
    {
        g_dispqSpoofW = wantW;
        g_dispqSpoofH = wantH;
    }

    char line[224];
    std::snprintf(line, sizeof(line),
                  "[DISPQ] realPrimary=%ux%u target=%ux%u(read=%d) spoof=%s -> %ux%u",
                  g_dispqRealPrimaryW, g_dispqRealPrimaryH, wantW, wantH, haveWant ? 1 : 0,
                  DispqSpoofActive() ? "ARMED" : "off", g_dispqSpoofW, g_dispqSpoofH);
    ME2VR::Log::Line(line);
}

// Installed from the worker thread, ahead of the game's D3D init - no race with ArmResolutionSpoof
// (which ran synchronously in DllMain).
// [SVRFIX] / [SVRFIX2] SteamVR scene-submit repair. Ported from ME1 (_shipconv_build/d3d_capture.cpp)
// 2026-07-26 -- ME2 had the [LINKFOV] FOV half of the SteamVR story but none of the device half.
//
// Diagnosed on ME1 2026-07-20: SteamVR's in-process client creates a keyed-mutex "sync texture" on the
// session's D3D11 device before it will accept scene frames. If the game's device cannot back a
// keyed-mutex shared texture, every ComposeLayerProjection fails with
// VRCompositorError_SharedTexturesNotSupported while xrEndFrame STILL reports success -- so the
// projection layer is silently dropped and you get a black world with working audio and working quad
// overlays (menus). Two independent causes, both fixed at device creation because neither can be
// changed on a live device:
//   [SVRFIX]  D3D11_CREATE_DEVICE_SINGLETHREADED makes D3D refuse keyed-mutex shared textures
//             (CreateTexture2D KEYEDMUTEX -> E_INVALIDARG while plain SHARED succeeds). Clearing it
//             only restores D3D's own internal locking -- a strict superset of singlethreaded
//             semantics -- and the mod already drives this device from the Present and XR threads, so the
//             flag was a lie the moment the mod injected.
//   [SVRFIX2] An UNKNOWN driverType + explicit adapter device is keyed-mutex-incapable here, while a
//             null-adapter HARDWARE device on the same GPU/process/flags is capable. Force the proven
//             recipe only for that exact failing pattern.
// Both gate on SteamVR being the ACTIVE runtime, so on Meta/Quest Link and VDXR neither fires and
// device creation stays bit-identical to stock.
// ================================================================================================
using D3D11CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                             const D3D_FEATURE_LEVEL*, UINT, UINT,
                                             ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using D3D11CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                         const D3D_FEATURE_LEVEL*, UINT, UINT,
                                                         const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
                                                         ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
D3D11CreateDeviceFn g_origD3D11CreateDevice = nullptr;
D3D11CreateDeviceAndSwapChainFn g_origD3D11CreateDeviceAndSwapChain = nullptr;

// [SVRFIX2] Is SteamVR the active OpenXR runtime? Read HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime
// (the JSON path the loader will use) BEFORE any XR instance exists, so device creation can gate on it.
// Cached: the hook fires several times and the registry value does not change mid-run.
bool SvrIsSteamVrActiveRuntime() noexcept
{
    static int cached = -1;
    if (cached >= 0) return cached != 0;
    cached = 0;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", 0, KEY_READ, &key) == ERROR_SUCCESS)
    {
        wchar_t path[1024] = {};
        DWORD cb = sizeof(path) - sizeof(wchar_t);
        DWORD type = 0;
        if (RegQueryValueExW(key, L"ActiveRuntime", nullptr, &type,
                             reinterpret_cast<LPBYTE>(path), &cb) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ))
        {
            for (wchar_t* p = path; *p; ++p) *p = towlower(*p);
            if (wcsstr(path, L"steamvr") != nullptr || wcsstr(path, L"steamxr") != nullptr) cached = 1;
        }
        RegCloseKey(key);
    }
    ME2VR::Log::Line(std::string("[SVRFIX2] active OpenXR runtime is SteamVR: ") + (cached ? "yes" : "no"));
    return cached != 0;
}

UINT SvrStripSingleThreadedFlag(UINT flags, const char* which) noexcept
{
    if ((flags & D3D11_CREATE_DEVICE_SINGLETHREADED) == 0) return flags;
    ME2VR::Log::Line(std::string("[SVRFIX] ") + which + " requested SINGLETHREADED (flags=" +
                     std::to_string(flags) + ") - stripping so SteamVR's keyed-mutex sync texture can be created.");
    return flags & ~static_cast<UINT>(D3D11_CREATE_DEVICE_SINGLETHREADED);
}

void SvrMaybeForceKeyedMutexRecipe(IDXGIAdapter*& adapter, D3D_DRIVER_TYPE& driverType, const char* which) noexcept
{
    if (!SvrIsSteamVrActiveRuntime()) return;
    if (driverType != D3D_DRIVER_TYPE_UNKNOWN || adapter == nullptr) return;
    ME2VR::Log::Line(std::string("[SVRFIX2] ") + which +
                     " used UNKNOWN+explicit-adapter (keyed-mutex-incapable on SteamVR); forcing null-adapter HARDWARE.");
    adapter = nullptr;
    driverType = D3D_DRIVER_TYPE_HARDWARE;
}

HRESULT WINAPI SvrD3D11CreateDeviceHook(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software,
                                        UINT flags, const D3D_FEATURE_LEVEL* levels, UINT levelCount,
                                        UINT sdkVersion, ID3D11Device** device, D3D_FEATURE_LEVEL* outLevel,
                                        ID3D11DeviceContext** context) noexcept
{
    // [SVRFIX] strip only when SteamVR is active: on Meta/VDXR the game's device is left exactly as it
    // asked for it, so this port cannot change behaviour on the runtime that is actually in use.
    if (SvrIsSteamVrActiveRuntime()) flags = SvrStripSingleThreadedFlag(flags, "D3D11CreateDevice");
    SvrMaybeForceKeyedMutexRecipe(adapter, driverType, "D3D11CreateDevice");
    return g_origD3D11CreateDevice != nullptr
               ? g_origD3D11CreateDevice(adapter, driverType, software, flags, levels, levelCount,
                                         sdkVersion, device, outLevel, context)
               : E_FAIL;
}

HRESULT WINAPI SvrD3D11CreateDeviceAndSwapChainHook(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType,
                                                    HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels,
                                                    UINT levelCount, UINT sdkVersion,
                                                    const DXGI_SWAP_CHAIN_DESC* scDesc, IDXGISwapChain** swapChain,
                                                    ID3D11Device** device, D3D_FEATURE_LEVEL* outLevel,
                                                    ID3D11DeviceContext** context) noexcept
{
    if (SvrIsSteamVrActiveRuntime()) flags = SvrStripSingleThreadedFlag(flags, "D3D11CreateDeviceAndSwapChain");
    SvrMaybeForceKeyedMutexRecipe(adapter, driverType, "D3D11CreateDeviceAndSwapChain");
    return g_origD3D11CreateDeviceAndSwapChain != nullptr
               ? g_origD3D11CreateDeviceAndSwapChain(adapter, driverType, software, flags, levels, levelCount,
                                                     sdkVersion, scDesc, swapChain, device, outLevel, context)
               : E_FAIL;
}

void InstallDisplayQueryHooks() noexcept
{

    MH_Initialize();   // idempotent if another path already called it

    // [SVRFIX] device-creation hooks FIRST: this runs from the startup worker seconds before the game
    // creates its device, and both the flag and the adapter/driverType recipe must be right AT creation --
    // neither can be changed on a live device.
    {
        HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
        if (d3d11 == nullptr) d3d11 = LoadLibraryW(L"d3d11.dll");
        if (d3d11 != nullptr)
        {
            void* cd = reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice"));
            if (cd != nullptr)
                DispqInstallHook(cd, reinterpret_cast<void*>(&SvrD3D11CreateDeviceHook),
                                 reinterpret_cast<void**>(&g_origD3D11CreateDevice), "D3D11CreateDevice");
            void* cds = reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
            if (cds != nullptr)
                DispqInstallHook(cds, reinterpret_cast<void*>(&SvrD3D11CreateDeviceAndSwapChainHook),
                                 reinterpret_cast<void**>(&g_origD3D11CreateDeviceAndSwapChain),
                                 "D3D11CreateDeviceAndSwapChain");
        }
        else
        {
            ME2VR::Log::Line("[SVRFIX] d3d11.dll not loadable - SteamVR device fixes unavailable this run.");
        }
    }
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr)
    {
        ME2VR::Log::Line("[DISPQ] user32.dll not loaded - display query hooks skipped.");
        return;
    }

    MH_Initialize();   // idempotent

    void* monInfoW = reinterpret_cast<void*>(GetProcAddress(user32, "GetMonitorInfoW"));
    DispqInstallHook(monInfoW, reinterpret_cast<void*>(&DispqGetMonitorInfoWHook),
                     reinterpret_cast<void**>(&g_origGetMonitorInfoW), "GetMonitorInfoW");

    void* monInfoA = reinterpret_cast<void*>(GetProcAddress(user32, "GetMonitorInfoA"));
    DispqInstallHook(monInfoA, reinterpret_cast<void*>(&DispqGetMonitorInfoAHook),
                     reinterpret_cast<void**>(&g_origGetMonitorInfoA), "GetMonitorInfoA");

    void* enumW = reinterpret_cast<void*>(GetProcAddress(user32, "EnumDisplaySettingsW"));
    DispqInstallHook(enumW, reinterpret_cast<void*>(&DispqEnumDisplaySettingsWHook),
                     reinterpret_cast<void**>(&g_origEnumDisplaySettingsW), "EnumDisplaySettingsW");

    void* enumA = reinterpret_cast<void*>(GetProcAddress(user32, "EnumDisplaySettingsA"));
    DispqInstallHook(enumA, reinterpret_cast<void*>(&DispqEnumDisplaySettingsAHook),
                     reinterpret_cast<void**>(&g_origEnumDisplaySettingsA), "EnumDisplaySettingsA");

    void* metrics = reinterpret_cast<void*>(GetProcAddress(user32, "GetSystemMetrics"));
    DispqInstallHook(metrics, reinterpret_cast<void*>(&DispqGetSystemMetricsHook),
                     reinterpret_cast<void**>(&g_origGetSystemMetrics), "GetSystemMetrics");

    HMODULE gdi32 = GetModuleHandleW(L"gdi32.dll");
    if (gdi32 == nullptr) gdi32 = LoadLibraryW(L"gdi32.dll");
    if (gdi32 != nullptr)
    {
        void* caps = reinterpret_cast<void*>(GetProcAddress(gdi32, "GetDeviceCaps"));
        DispqInstallHook(caps, reinterpret_cast<void*>(&DispqGetDeviceCapsHook),
                         reinterpret_cast<void**>(&g_origGetDeviceCaps), "GetDeviceCaps");
    }
}
bool GetManualFlat() noexcept { return g_manualFlat.load(std::memory_order_acquire); }
void SetManualFlat(bool on) noexcept { g_manualFlat.store(on, std::memory_order_release); }
ID3D11Texture2D* GetUiOverlayTexture() noexcept { return g_uiOverlayTex; }
bool GetUiOverlayActive() noexcept { return g_overlayHasContent.load(std::memory_order_acquire); }
bool GetUiOverlayMode() noexcept { return g_uiOverlayMode.load(std::memory_order_acquire); }
void SetUiOverlayMode(bool on) noexcept { g_uiOverlayMode.store(on, std::memory_order_release); }
ID3D11Texture2D* GetUiTexture() noexcept { return g_uiSrcTexture; }
unsigned GetUiTexWidth() noexcept { return g_uiTexW; }
unsigned GetUiTexHeight() noexcept { return g_uiTexH; }

// --- DIBR public API (called from me2_xr RunFrame; internals live in the anon namespace above) ---
void SetDepthMapEnabled(bool enabled) noexcept { g_depthMapEnabled.store(enabled, std::memory_order_relaxed); }
bool GetDepthMapEnabled() noexcept { return g_depthMapEnabled.load(std::memory_order_relaxed); }
bool IsDibrStereoReady() noexcept
{
    return g_depthMapEnabled.load(std::memory_order_relaxed) &&
           g_depthReady.load(std::memory_order_acquire) && g_depthSrv != nullptr;
}
void SetDibrWarp(float gain, float convergence, bool flip) noexcept
{
    auto clampf = [](float v, float lo, float hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); };
    g_dibrGain.store(clampf(gain, 0.0f, 10.0f), std::memory_order_relaxed);
    g_dibrConvergence.store(clampf(convergence, 0.90f, 1.005f), std::memory_order_relaxed);
    g_dibrSign.store(flip ? -1.0f : 1.0f, std::memory_order_relaxed);
}
void GetDepthProbe(float* center, float* tl, float* br, float* tr) noexcept
{
    if (center) *center = g_probeCenter.load(std::memory_order_relaxed);
    if (tl)     *tl     = g_probeTL.load(std::memory_order_relaxed);
    if (br)     *br     = g_probeBR.load(std::memory_order_relaxed);
    if (tr)     *tr     = g_probeTR.load(std::memory_order_relaxed);
}
// Synthesized right eye (backbuffer-sized). nullptr if not ready -> caller submits the backbuffer (never black).
ID3D11Texture2D* GetDibrRightEye(ID3D11Texture2D* backBuffer) noexcept
{
    if (g_gameDevice == nullptr || backBuffer == nullptr) return nullptr;
    D3D11_TEXTURE2D_DESC bbd = {}; backBuffer->GetDesc(&bbd);
    if (!EnsureColorCopy(bbd) || !EnsureWarpedTex(bbd)) return nullptr;
    return RenderDibrEye(backBuffer, g_dibrWarpedTex, g_dibrWarpedRtv, 1.0f);
}

void InstallBreakpointProbes() noexcept
{
    ME2VR::Log::Line("[ME3DISC] breakpoint probes disabled for composite draw trace build.");
    return;

    bool expected = false;
    if (!g_breakpointsInstalled.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

    HMODULE exe = GetModuleHandleW(nullptr);
    if (exe == nullptr)
    {
        ME2VR::Log::Line("[ME3DISC] breakpoint probes skipped: exe module missing");
        return;
    }

    g_vectoredHandler = AddVectoredExceptionHandler(1, BreakpointVectoredHandler);
    if (g_vectoredHandler == nullptr)
    {
        ME2VR::Log::WindowsError("[ME3DISC] AddVectoredExceptionHandler", GetLastError());
        return;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(exe);
    unsigned installed = 0;
    for (BreakpointProbe& probe : g_breakpointProbes)
    {
        probe.address = reinterpret_cast<BYTE*>(base + probe.rva);
        probe.originalByte = *probe.address;
        if (probe.originalByte == 0xCC)
        {
            char buffer[192] = {};
            sprintf_s(buffer,
                      "[ME3DISC] breakpoint probe skipped %s rva=0x%llX already int3",
                      probe.name,
                      static_cast<unsigned long long>(probe.rva));
            ME2VR::Log::Line(buffer);
            continue;
        }

        if (WriteByte(probe.address, 0xCC))
        {
            probe.armed = true;
            ++installed;
            char buffer[192] = {};
            sprintf_s(buffer,
                      "[ME3DISC] breakpoint probe armed %s rva=0x%llX original=0x%02X",
                      probe.name,
                      static_cast<unsigned long long>(probe.rva),
                      static_cast<unsigned int>(probe.originalByte));
            ME2VR::Log::Line(buffer);
        }
        else
        {
            char buffer[192] = {};
            sprintf_s(buffer,
                      "[ME3DISC] breakpoint probe failed %s rva=0x%llX",
                      probe.name,
                      static_cast<unsigned long long>(probe.rva));
            ME2VR::Log::Line(buffer);
        }
    }

    ME2VR::Log::Line("[ME3DISC] breakpoint probes installed count=" + std::to_string(installed));
}

void TryInstallFactoryHooks(void* factory, const IID&) noexcept
{
    if (factory == nullptr) return;
    std::lock_guard<std::mutex> lock(g_hookMutex);

    void** vtable = *reinterpret_cast<void***>(factory);
    if (vtable == nullptr) return;

    if (g_createSwapChainSlot == nullptr &&
        PatchPointerSlot(&vtable[kCreateSwapChainSlot],
                         reinterpret_cast<void*>(&CreateSwapChainHook),
                         reinterpret_cast<void**>(&g_originalCreateSwapChain),
                         "IDXGIFactory::CreateSwapChain"))
    {
        g_createSwapChainSlot = &vtable[kCreateSwapChainSlot];
    }

    if (g_createSwapChainForHwndSlot == nullptr &&
        PatchPointerSlot(&vtable[kCreateSwapChainForHwndSlot],
                         reinterpret_cast<void*>(&CreateSwapChainForHwndHook),
                         reinterpret_cast<void**>(&g_originalCreateSwapChainForHwnd),
                         "IDXGIFactory2::CreateSwapChainForHwnd"))
    {
        g_createSwapChainForHwndSlot = &vtable[kCreateSwapChainForHwndSlot];
    }

    if (g_createSwapChainForCoreWindowSlot == nullptr &&
        PatchPointerSlot(&vtable[kCreateSwapChainForCoreWindowSlot],
                         reinterpret_cast<void*>(&CreateSwapChainForCoreWindowHook),
                         reinterpret_cast<void**>(&g_originalCreateSwapChainForCoreWindow),
                         "IDXGIFactory2::CreateSwapChainForCoreWindow"))
    {
        g_createSwapChainForCoreWindowSlot = &vtable[kCreateSwapChainForCoreWindowSlot];
    }

    if (g_createSwapChainForCompositionSlot == nullptr &&
        PatchPointerSlot(&vtable[kCreateSwapChainForCompositionSlot],
                         reinterpret_cast<void*>(&CreateSwapChainForCompositionHook),
                         reinterpret_cast<void**>(&g_originalCreateSwapChainForComposition),
                         "IDXGIFactory2::CreateSwapChainForComposition"))
    {
        g_createSwapChainForCompositionSlot = &vtable[kCreateSwapChainForCompositionSlot];
    }
}
}
