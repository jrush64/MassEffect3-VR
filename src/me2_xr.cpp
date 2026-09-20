#include "me2_xr.h"

#include "calcview_hook.h"
#include "convo_fp.h"
#include "d3d_capture.h"
#include "engine_probe.h"
#include "logger.h"
#include "me2_menu.h"
#include "xr_types.h"

#include <Windows.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <vector>

namespace
{
using namespace MELEVR::Xr;

HMODULE g_loader = nullptr;
PFN_xrGetInstanceProcAddr g_getProc = nullptr;
PFN_xrPollEvent g_pollEvent = nullptr;
XrInstance g_instance = nullptr;
XrSystemId g_system = 0;
XrSession g_session = nullptr;
XrSpace g_localSpace = nullptr;  // re-origined on recenter; locate + submit happen against this
XrSpace g_baseSpace = nullptr;   // LOCAL, never re-origined; reads the absolute head at recenter
XrSpace g_viewSpace = nullptr;   // (unused since B2b; kept for reference)
Functions g_fn;
std::atomic_bool g_recenterRequested{false};

// Flat-mono quad placement (LE1 "theater" style; tune for comfort).
constexpr float kQuadDistanceM = 2.2f;   // metres in front of the head
constexpr float kQuadWidthM = 3.2f;      // panel width in metres

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;

// Per-eye swapchains (sized to the game backbuffer so CopyResource is a straight full-frame copy).
XrSwapchain g_swap[2] = {};
std::vector<ID3D11Texture2D*> g_images[2];
uint32_t g_swapW = 0, g_swapH = 0;   // per-EYE swapchain dims (half the SBS backbuffer width)
uint32_t g_bbW = 0;                  // full SBS backbuffer width
int64_t g_swapFormat = 0;

// UI quad layer (the captured GFx UI texture, shown zero-disparity over the world).
XrSwapchain g_uiSwap = nullptr;
std::vector<ID3D11Texture2D*> g_uiImages;
uint32_t g_uiW = 0, g_uiH = 0;
bool g_uiSwapTried = false;
// Game-UI overlay (HUD + menus, from D3DCapture's overlay RT) shown as one flat layer over the world.
XrSwapchain g_gameUiSwap = nullptr;
std::vector<ID3D11Texture2D*> g_gameUiImages;
uint32_t g_gameUiW = 0, g_gameUiH = 0;
constexpr XrFlags64 kLayerSrcAlpha = 0x00000002;   // XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
constexpr float kUiQuadDistanceM = 2.0f;

// Mono fallback: full-screen frames (menus/loading) where the SBS split did NOT run are shown as a
// single full-backbuffer quad to BOTH eyes (no halving -> not cross-eyed).
XrSwapchain g_monoSwap = nullptr;
std::vector<ID3D11Texture2D*> g_monoImages;
unsigned long long g_prevSplitSeq = 0;
unsigned g_framesSinceSplit = 9999;          // hysteresis: stay SBS unless the split has been idle a while
constexpr unsigned kSbsHoldFrames = 30;      // ~0.5s - covers gaps where the split skips a present
// [STALEEYE] presents the held pass-0 capture may be behind the live backbuffer before the SFR submit
// stops pairing them. Gameplay captures every present (age 0-1), so this only trips on a real stall;
// 3 presents is ~25ms at 120Hz, short enough to be invisible and long enough that a single missed
// capture marker never trips it.
constexpr uint64_t kStaleCapturePresents = 3;
std::atomic<unsigned long long> g_modeLogCount{0};
std::atomic<unsigned long long> g_aerModeLogCount{0};   // dedicated: the shared g_modeLogCount is spent by SBS startup lines before AER ever runs
// DIBR never-black fallback: last good located pose/FOV, reused on any frame the locate/backbuffer guard
// hiccups so DIBR (which has NO mono-quad fallback, unlike ME1) can never submit 0 layers = a hard black flash.
XrPosef g_dibrLastHeadPose = {};
XrFovf  g_dibrLastFovL = {}, g_dibrLastFovR = {};
bool    g_dibrHaveLast = false;
unsigned g_dibrFullFrames = 0, g_dibrFallbackFrames = 0, g_dibrBlackFrames = 0;
ULONGLONG g_dibrSubStatMs = 0;
// [COMFORT] Panel placement, runtime-tunable (ME2 parity). Were fixed constants; the cine/mono
// screen and the Insert panel both needed moving per-person, which is what the Comfort tab is for.
std::atomic<float> g_monoQuadDistanceM{2.2f};   // meters in front of the face
std::atomic<float> g_monoQuadWidthM{3.2f};      // meters wide
std::atomic<float> g_cineScreenZoom{1.0f};      // [VRCINE] flat cine screen zoom, ME2 range 0.5-3
std::atomic<float> g_menuQuadDistanceM{1.5f};
std::atomic<float> g_menuQuadWidthM{1.4f};
std::atomic<float> g_menuQuadOffXM{0.0f};
std::atomic<float> g_menuQuadOffYM{0.0f};

// --- AER (alternate-eye rendering), ported from ME1 xr_session.cpp 2026-07-04 -----------------
// Full-size eye swapchains: ME2's g_swap[2] are SBS-HALF (bbW/2) -> wrong for AER's full-frame copy.
XrSwapchain g_aerSwap[2] = {};
std::vector<ID3D11Texture2D*> g_aerImages[2];
uint32_t g_aerSwapW = 0, g_aerSwapH = 0;
// 2-slot full-backbuffer history bank: one eye captured fresh each present, the other held ~1 frame.
ID3D11Texture2D* g_aerHist[2] = { nullptr, nullptr };
bool g_aerHistValid[2] = { false, false };
// [AERBLINK] true once this eye's SWAPCHAIN holds real stereo content. OpenXR keeps presenting a
// swapchain's most recently RELEASED image until a new one is released, so an eye only needs an
// acquire/copy when its content actually changes - see SubmitAerFrame.
bool g_aerEyeFilled[2] = { false, false };
D3D11_TEXTURE2D_DESC g_aerHistDesc = {};
bool g_aerHistDescValid = false;
// THE anti-ghost pose-tag (a core invariant of this mod): each slot remembers the head pose it was CAPTURED at, so
// the STALE eye is submitted at the orientation it was actually rendered facing - not the current head yaw.
// Tagging the stale eye with the current pose (old shared-pose behavior, and ME1's too) is what ghosts on turn.
XrPosef g_aerHistPose[2] = {};
bool g_aerHistPoseValid[2] = { false, false };
uint64_t g_aerLastSeq = 0;           // last stamp seq consumed (the drift-free handshake)
uint64_t g_aerPresent = 0;
ULONGLONG g_aerHzWindowStartMs = 0;  // [AERHZ] cadence meter
unsigned int g_aerHzCaptures = 0;
int g_aerHzEmitted = 0;   // stop after a bounded number of ~2s windows - health-check on launch, not forever
// Alternation-health instrumentation (2026-07-04): PROVE whether the eye ping-pongs L,R,L,R cleanly or
// stalls/skips. sameEyeTwice>0 = the handshake mislabels (the shear source); seqGapBad>0 = renders were
// missed/doubled between presents (late-frame staleness). If both stay 0 the handshake is clean = the shake
// is pure late-frame staleness (perf), not a code bug. This makes the next headset test conclusive.
int g_aerPrevCaptureEye = -1;
unsigned int g_aerSameEyeTwice = 0;
unsigned int g_aerSeqGapBad = 0;
uint64_t g_aerMaxSeqGap = 0;
bool g_wasAer = false;               // edge-detect for ResetAerHistory on mode-leave

void XLog(const std::string& s);   // fwd (defined below) - the pacing helpers log through it

// Display-locked frame pacing (ported from ME2). The game is engine-bound at ~60fps and the headset is
// 120Hz, so holding the present to display/2 (=60fps) makes each frame persist EXACTLY 2 display frames ->
// no reprojection jitter (the stereo micro-shake). Not a halving: the engine can't feed 120fps anyway, so
// display/2 only REGULARISES the ~60 it already produces. The Hz comes from XR_FB_display_refresh_rate
// when supported, with a one-shot filtered predictedDisplayTime fallback (or an explicit menu pin).
// Once acquired, the rate stays locked for the session just as it does in ME1/ME2.
// [VRFILL] user controls for the headset fill (ME2 parity).
std::atomic_bool g_vrFovFillEnabled{true};
std::atomic<float> g_vrFillH{1.0f};
std::atomic<float> g_vrFillV{1.0f};
// [LINKFOV] Runtime quirks, set once at instance creation. Ported from ME2 2026-07-28.
// [LINKFOV] last runtime-located per-eye FOV, captured in the crop block below.
XrFovf g_lastRtFov[2] = {};
bool g_lastRtFovValid = false;
bool g_isOculusRuntime = false;   // Meta PC runtime: ignores declared FOV -> doubled image without the crop
bool g_isSteamVrRuntime = false;  // SteamVR: voids any view declared wider than its own frustum
std::atomic_bool g_questFovMatch{true};   // ini QuestFovMatch - escape hatch for the Meta crop
double g_displayPeriodSec = 1.0 / 120.0;
int g_paceWarmCount = 0;
XrTime g_lastPredictedDisplayTime = 0;
bool g_paceLocked = false;
bool g_refreshRateApplied = false;
bool g_hasRefreshRateExt = false;
LARGE_INTEGER g_paceQpcFreq = {};
LARGE_INTEGER g_lastPaceQpc = {};
std::atomic<unsigned> g_lastWaitFrameUs{0};   // [FRAMETIME] xrWaitFrame block time, last RunFrame
// [XRSUBMIT] windowed total RunFrame wall time (xrWaitFrame included; the FRAMETIME consumer
// subtracts LastWaitFrameUs to isolate the submit-side cost). Read+reset once per 5s window.
std::atomic<uint32_t> g_xrSubmitCount{0};
std::atomic<uint64_t> g_xrSubmitSumUs{0};
std::atomic<uint32_t> g_xrSubmitMaxUs{0};
constexpr int kPaceWarmFrames = 90;
// [PACEFIX] robust one-shot fallback cadence learning - see UpdatePaceWarmup.
constexpr double kPaceSnapTol = 0.06;      // a measurement >6% from every known refresh is rejected
double g_paceSamples[kPaceWarmFrames] = {};
// [PACE2] per-~2s telemetry: presents that arrived AFTER the display/2 deadline (the clamp can only slow a
// fast frame, never rescue a late one -> a late frame = a cadence slip). Measures smoothness directly.
ULONGLONG g_paceWinStartMs = 0;
unsigned int g_paceWinFrames = 0;
unsigned int g_paceWinLate = 0;
double g_paceWinMaxMs = 0.0;
double g_paceWinSumMs = 0.0;
int g_paceWinEmitted = 0;   // stop after a bounded number of ~2s windows - health-check on launch, not forever

// Learn the headset refresh once: pinned Hz -> runtime query -> robust measured fallback.
// ME1/ME2 keep this lock for the session. ME3 previously re-checked the minimum delta forever; a
// single short VirtualDesktopXR timing sample then looked like 177-227Hz and made the pacer oscillate
// 120 -> 144 -> 120. Those target changes are visible as intermittent head-tracking judder.
void UpdatePaceWarmup(XrTime predictedDisplayTime, int pinnedHz) noexcept
{
    static int s_previousPinnedHz = 0;
    if (pinnedHz <= 0 && s_previousPinnedHz > 0)
    {
        // Returning the menu control to Auto must query/learn again instead of retaining the old pin.
        g_paceLocked = false;
        g_refreshRateApplied = false;
        g_paceWarmCount = 0;
        g_lastPredictedDisplayTime = 0;
        g_lastPaceQpc.QuadPart = 0;
    }
    s_previousPinnedHz = pinnedHz;

    if (pinnedHz > 0)
    {
        const double p = 1.0 / static_cast<double>(pinnedHz);
        if (!g_paceLocked || std::fabs(p - g_displayPeriodSec) > 1e-6)
        {
            g_displayPeriodSec = p; g_paceLocked = true; g_refreshRateApplied = true;
            XLog("[PACE] cadence: pinned " + std::to_string(pinnedHz) + "Hz");
        }
        return;
    }

    if (!g_refreshRateApplied && g_fn.getDisplayRefreshRate != nullptr && g_session != nullptr)
    {
        float hz = 0.0f;
        if (XrSucceeded(g_fn.getDisplayRefreshRate(g_session, &hz)) && hz > 20.0f && hz < 1000.0f)
        {
            g_displayPeriodSec = 1.0 / static_cast<double>(hz);
            g_paceLocked = true;
            g_refreshRateApplied = true;
            XLog("[PACE] cadence: runtime reports " + std::to_string(static_cast<int>(hz + 0.5f)) +
                 "Hz -> stable session lock (no frame-delta guess)");
            return;
        }
    }
    if (g_paceLocked) return;

    if (g_lastPredictedDisplayTime != 0 && predictedDisplayTime > g_lastPredictedDisplayTime)
    {
        const double dt = static_cast<double>(predictedDisplayTime - g_lastPredictedDisplayTime) * 1e-9;
        if (dt > 0.004 && dt < 0.05 && g_paceWarmCount < kPaceWarmFrames)
            g_paceSamples[g_paceWarmCount++] = dt;
    }
    g_lastPredictedDisplayTime = predictedDisplayTime;
    if (g_paceWarmCount < kPaceWarmFrames) return;

    // Hitches only lengthen predicted-display deltas, while VDXR occasionally emits isolated deltas
    // that are too short. A low percentile rejects both kinds of outlier; unlike the old absolute
    // minimum, one bad short sample cannot claim that a 120Hz headset suddenly became 144/227Hz.
    double sorted[kPaceWarmFrames] = {};
    std::copy(g_paceSamples, g_paceSamples + kPaceWarmFrames, sorted);
    std::sort(sorted, sorted + kPaceWarmFrames);
    const double sampleDt = sorted[kPaceWarmFrames / 10];
    const double hz = 1.0 / sampleDt;
    const double cands[] = { 60.0, 72.0, 80.0, 90.0, 100.0, 120.0, 144.0 };
    double best = 0.0, bestErr = 1e9;
    for (double c : cands) { const double e = (c > hz) ? (c - hz) : (hz - c); if (e < bestErr) { bestErr = e; best = c; } }
    if (best <= 0.0 || (bestErr / best) > kPaceSnapTol)
    {
        static int s_rejects = 0;
        if (s_rejects < 4)
        {
            ++s_rejects;
            XLog("[PACE] cadence sample ~" + std::to_string(static_cast<int>(hz + 0.5)) +
                 "Hz is not near any known refresh - discarding window, still measuring");
        }
        g_paceWarmCount = 0;
        return;
    }
    g_displayPeriodSec = 1.0 / best; g_paceLocked = true;
    XLog("[PACE] cadence: measured ~" + std::to_string(static_cast<int>(hz + 0.5)) +
         "Hz -> stable " + std::to_string(static_cast<int>(best + 0.5)) + "Hz session lock (10th-percentile fallback).");
}

// Hold the present to 2x display period (= display/2) so each frame lands on a stable integer fraction of the
// headset refresh. Coarse Sleep(1) then a tight QPC spin. (ME2 verbatim.)
// [PACE1X] allow1x + the FullRefreshPacing knob pick full vs half rate. Deliberately a FIXED user
// choice, not adaptive: ME2 tried choosing automatically from measured frame time and it oscillated,
// because pacing to half lets the game coast (looks fast -> upgrade) while pacing to full loads it
// (looks slow -> downgrade). The measurement is changed by the decision, so no threshold fixes that
// loop. [AERFULL 2026-08-20] AER used to be forced to half here ("the eye-swap cadence
// requirement"); that requirement was really about REGULARITY, which full-rate pacing provides just
// as well - at exactly display rate the alternating eyes land on alternate vsyncs, each eye a steady
// display/2. The forced half was costing AER its entire point (22Hz/eye at 90Hz).
void PaceDisplayLocked(bool allow1x) noexcept
{
    if (!g_paceLocked) { g_lastPaceQpc.QuadPart = 0; return; }
    if (g_paceQpcFreq.QuadPart == 0) QueryPerformanceFrequency(&g_paceQpcFreq);
    const bool full = allow1x && ME2VR::CalcViewHook::GetFullRefreshPacing();
    const double target = full ? g_displayPeriodSec : 2.0 * g_displayPeriodSec;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double entryElapsed = -1.0;
    if (g_lastPaceQpc.QuadPart != 0)
    {
        entryElapsed = static_cast<double>(now.QuadPart - g_lastPaceQpc.QuadPart) / static_cast<double>(g_paceQpcFreq.QuadPart);
        double elapsed = entryElapsed;
        while (target - elapsed > 0.0)
        {
            if (target - elapsed > 0.0015) Sleep(1);
            QueryPerformanceCounter(&now);
            elapsed = static_cast<double>(now.QuadPart - g_lastPaceQpc.QuadPart) / static_cast<double>(g_paceQpcFreq.QuadPart);
        }
    }
    QueryPerformanceCounter(&g_lastPaceQpc);

    if (entryElapsed >= 0.0)
    {
        const double targetMs = target * 1000.0;
        const double rawMs = entryElapsed * 1000.0;
        ++g_paceWinFrames;
        g_paceWinSumMs += rawMs;
        if (rawMs > g_paceWinMaxMs) g_paceWinMaxMs = rawMs;
        if (rawMs > targetMs * 1.02) ++g_paceWinLate;
        const ULONGLONG nowMs = GetTickCount64();
        if (g_paceWinStartMs == 0) g_paceWinStartMs = nowMs;
        if (nowMs - g_paceWinStartMs >= 2000 && g_paceWinFrames > 0 && g_paceWinEmitted < 30)
        {
            char line[220] = {};
            sprintf_s(line, "[PACE2] presents=%u lateFrames=%u (%.0f%%) targetMs=%.2f avgMs=%.2f maxMs=%.2f",
                      g_paceWinFrames, g_paceWinLate,
                      100.0 * static_cast<double>(g_paceWinLate) / static_cast<double>(g_paceWinFrames),
                      targetMs, g_paceWinSumMs / static_cast<double>(g_paceWinFrames), g_paceWinMaxMs);
            XLog(line);
            ++g_paceWinEmitted;
            g_paceWinStartMs = nowMs; g_paceWinFrames = 0; g_paceWinLate = 0; g_paceWinMaxMs = 0.0; g_paceWinSumMs = 0.0;
        }
    }
}

std::atomic_bool g_tried{false};       // A1 init attempted
bool g_initOk = false;                 // session + swapchains ready
bool g_begun = false;                  // xrBeginSession done
XrSessionState g_state = 0;
std::atomic_bool g_submitLogged{false};

constexpr XrViewConfigurationType kStereo = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE;

std::string FmtXr(XrResult r) { return std::to_string(static_cast<int>(r)); }
void XLog(const std::string& s) { ME2VR::Log::Line("[ME3XR] " + s); }

bool Resolve(const char* name, PFN_xrVoidFunction* out) noexcept
{
    *out = nullptr;
    const XrResult r = g_getProc(g_instance, name, out);
    if (!XrSucceeded(r) || *out == nullptr) { XLog(std::string("resolve FAILED: ") + name + " (" + FmtXr(r) + ")"); return false; }
    return true;
}

#define RESOLVE(name, field, type)                                  \
    do { PFN_xrVoidFunction p = nullptr;                            \
         if (!Resolve(name, &p)) return false;                      \
         g_fn.field = reinterpret_cast<type>(p); } while (0)

bool ResolveFunctions() noexcept
{
    RESOLVE("xrDestroyInstance", destroyInstance, PFN_xrDestroyInstance);
    RESOLVE("xrGetSystem", getSystem, PFN_xrGetSystem);
    RESOLVE("xrEnumerateViewConfigurationViews", enumerateViewConfigurationViews, PFN_xrEnumerateViewConfigurationViews);
    RESOLVE("xrGetD3D11GraphicsRequirementsKHR", getD3D11GraphicsRequirements, PFN_xrGetD3D11GraphicsRequirementsKHR);
    RESOLVE("xrCreateSession", createSession, PFN_xrCreateSession);
    RESOLVE("xrDestroySession", destroySession, PFN_xrDestroySession);
    RESOLVE("xrEnumerateSwapchainFormats", enumerateSwapchainFormats, PFN_xrEnumerateSwapchainFormats);
    RESOLVE("xrCreateSwapchain", createSwapchain, PFN_xrCreateSwapchain);
    RESOLVE("xrEnumerateSwapchainImages", enumerateSwapchainImages, PFN_xrEnumerateSwapchainImages);
    RESOLVE("xrAcquireSwapchainImage", acquireSwapchainImage, PFN_xrAcquireSwapchainImage);
    RESOLVE("xrWaitSwapchainImage", waitSwapchainImage, PFN_xrWaitSwapchainImage);
    RESOLVE("xrReleaseSwapchainImage", releaseSwapchainImage, PFN_xrReleaseSwapchainImage);
    RESOLVE("xrBeginSession", beginSession, PFN_xrBeginSession);
    RESOLVE("xrEndSession", endSession, PFN_xrEndSession);
    RESOLVE("xrWaitFrame", waitFrame, PFN_xrWaitFrame);
    RESOLVE("xrBeginFrame", beginFrame, PFN_xrBeginFrame);
    RESOLVE("xrEndFrame", endFrame, PFN_xrEndFrame);
    RESOLVE("xrCreateReferenceSpace", createReferenceSpace, PFN_xrCreateReferenceSpace);
    RESOLVE("xrDestroySpace", destroySpace, PFN_xrDestroySpace);
    RESOLVE("xrLocateViews", locateViews, PFN_xrLocateViews);
    PFN_xrVoidFunction p = nullptr;
    if (!Resolve("xrPollEvent", &p)) return false;
    g_pollEvent = reinterpret_cast<PFN_xrPollEvent>(p);
    return true;
}

bool ChooseSwapchainFormat() noexcept
{
    uint32_t count = 0;
    if (!XrSucceeded(g_fn.enumerateSwapchainFormats(g_session, 0, &count, nullptr)) || count == 0) return false;
    std::vector<int64_t> formats(count);
    if (!XrSucceeded(g_fn.enumerateSwapchainFormats(g_session, count, &count, formats.data()))) return false;
    // Prefer an SRGB-declared format: the game backbuffer holds already display-encoded bytes, so an
    // SRGB swapchain tells the runtime not to re-apply gamma (fixes the washed-out color). LE1 does this.
    auto has = [&](int64_t f) { for (int64_t x : formats) if (x == f) return true; return false; };
    int64_t pick = formats[0];
    if (has(29)) pick = 29;          // R8G8B8A8_UNORM_SRGB
    else if (has(91)) pick = 91;     // B8G8R8A8_UNORM_SRGB
    else if (has(28)) pick = 28;     // R8G8B8A8_UNORM (no gamma correction available)
    g_swapFormat = pick;
    XLog("swapchain format chosen=" + std::to_string(pick) + " (SRGB preferred; offered " + std::to_string(count) + ")");
    return true;
}

bool CreateEyeSwapchains() noexcept
{
    uint32_t viewCount = 0;
    if (!XrSucceeded(g_fn.enumerateViewConfigurationViews(g_instance, g_system, kStereo, 0, &viewCount, nullptr)) || viewCount < 2)
        return false;
    std::vector<XrViewConfigurationView> vcv(viewCount);
    for (auto& v : vcv) v.type = XR_TYPE_VIEW_CONFIGURATION_VIEW_VALUE;
    g_fn.enumerateViewConfigurationViews(g_instance, g_system, kStereo, viewCount, &viewCount, vcv.data());

    // B2b (stereo): per-eye swapchain = HALF the SBS backbuffer (left half / right half).
    g_bbW = ME2VR::D3DCapture::GetBackbufferWidth();
    const uint32_t bbH = ME2VR::D3DCapture::GetBackbufferHeight();
    if (g_bbW == 0 || bbH == 0) return false;
    g_swapW = g_bbW / 2;
    g_swapH = bbH;
    XLog("view config views=" + std::to_string(viewCount) +
         " recommended=" + std::to_string(vcv[0].recommendedImageRectWidth) + "x" + std::to_string(vcv[0].recommendedImageRectHeight) +
         " ; per-eye swapchain " + std::to_string(g_swapW) + "x" + std::to_string(g_swapH) + " (SBS half)");

    for (int eye = 0; eye < 2; ++eye)
    {
        XrSwapchainCreateInfo sc = {};
        sc.type = XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE;
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE | XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE;
        sc.format = g_swapFormat;
        sc.sampleCount = 1;
        sc.width = g_swapW;
        sc.height = g_swapH;
        sc.faceCount = 1;
        sc.arraySize = 1;
        sc.mipCount = 1;
        XrResult r = g_fn.createSwapchain(g_session, &sc, &g_swap[eye]);
        if (!XrSucceeded(r)) { XLog("xrCreateSwapchain eye " + std::to_string(eye) + " FAILED " + FmtXr(r)); return false; }

        uint32_t imgCount = 0;
        g_fn.enumerateSwapchainImages(g_swap[eye], 0, &imgCount, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> imgs(imgCount);
        for (auto& im : imgs) im.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE;
        g_fn.enumerateSwapchainImages(g_swap[eye], imgCount, &imgCount,
                                      reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
        g_images[eye].clear();
        for (auto& im : imgs) g_images[eye].push_back(im.texture);
    }
    XLog("eye swapchains created (" + std::to_string(g_images[0].size()) + " images/eye)");
    return true;
}

bool BringUp(ID3D11Device* device) noexcept
{
    g_device = device;
    g_device->GetImmediateContext(&g_ctx);

    g_loader = LoadLibraryW(L"openxr_loader.dll");
    if (g_loader == nullptr) { XLog("openxr_loader.dll not found. Skipping VR."); return false; }
    g_getProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(g_loader, "xrGetInstanceProcAddr"));
    if (g_getProc == nullptr) { XLog("xrGetInstanceProcAddr missing"); return false; }

    PFN_xrVoidFunction createInstanceRaw = nullptr;
    if (!XrSucceeded(g_getProc(nullptr, "xrCreateInstance", &createInstanceRaw)) || !createInstanceRaw) { XLog("no xrCreateInstance"); return false; }

    // Match ME1/ME2: ask OpenXR for the headset's true refresh instead of inferring it from game timing.
    // This extension is optional, so advertise it only when the active runtime supports it.
    g_hasRefreshRateExt = false;
    {
        PFN_xrVoidFunction enumRaw = nullptr;
        if (XrSucceeded(g_getProc(nullptr, "xrEnumerateInstanceExtensionProperties", &enumRaw)) && enumRaw != nullptr)
        {
            auto enumFn = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(enumRaw);
            uint32_t n = 0;
            if (XrSucceeded(enumFn(nullptr, 0, &n, nullptr)) && n > 0)
            {
                std::vector<XrExtensionProperties> props(n);
                for (auto& p : props) { p.type = XR_TYPE_EXTENSION_PROPERTIES_VALUE; p.next = nullptr; }
                if (XrSucceeded(enumFn(nullptr, n, &n, props.data())))
                    for (auto& p : props)
                        if (strcmp(p.extensionName, kDisplayRefreshRateExtensionName) == 0)
                        {
                            g_hasRefreshRateExt = true;
                            break;
                        }
            }
        }
    }
    const char* exts[2] = { kD3D11ExtensionName, kDisplayRefreshRateExtensionName };
    XrInstanceCreateInfo ci = {};
    ci.type = XR_TYPE_INSTANCE_CREATE_INFO_VALUE;
    ci.applicationInfo.apiVersion = MakeXrVersion(1, 0, 34);
    strcpy_s(ci.applicationInfo.applicationName, "ME2VR");
    strcpy_s(ci.applicationInfo.engineName, "ME2VR-M0");
    ci.enabledExtensionCount = g_hasRefreshRateExt ? 2u : 1u;
    ci.enabledExtensionNames = exts;
    XrResult r = reinterpret_cast<PFN_xrCreateInstance>(createInstanceRaw)(&ci, &g_instance);
    XLog("xrCreateInstance: " + FmtXr(r));

    // [LINKFOV] Which runtime is this? Meta's PC runtime composites projection layers assuming ITS OWN
    // per-eye frustum no matter what FOV the mod declares (doubled image); SteamVR voids a view declared wider
    // than its own. Both are cured by cropping the submit to the intersection - see the submit loop.
    g_isOculusRuntime = false;
    g_isSteamVrRuntime = false;
    if (XrSucceeded(r) && g_instance != nullptr)
    {
        PFN_xrVoidFunction propsRaw = nullptr;
        if (XrSucceeded(g_getProc(g_instance, "xrGetInstanceProperties", &propsRaw)) && propsRaw != nullptr)
        {
            XrInstanceProperties props = {};
            props.type = XR_TYPE_INSTANCE_PROPERTIES_VALUE;
            if (XrSucceeded(reinterpret_cast<PFN_xrGetInstanceProperties>(propsRaw)(g_instance, &props)))
            {
                g_isOculusRuntime = std::strstr(props.runtimeName, "Oculus") != nullptr ||
                                    std::strstr(props.runtimeName, "Meta") != nullptr;
                g_isSteamVrRuntime = std::strstr(props.runtimeName, "SteamVR") != nullptr;
                XLog(std::string("VR runtime: ") + props.runtimeName + ".");
                XLog(std::string("[XRAPI] [XRRUNTIME] name='") + props.runtimeName +
                     "' oculusFovQuirk=" + (g_isOculusRuntime ? "1" : "0") +
                     " steamVrFovCrop=" + (g_isSteamVrRuntime ? "1" : "0"));
            }
        }
    }
    if (!XrSucceeded(r)) return false;
    if (!ResolveFunctions()) return false;

    if (g_hasRefreshRateExt)
    {
        PFN_xrVoidFunction p = nullptr;
        if (XrSucceeded(g_getProc(g_instance, "xrGetDisplayRefreshRateFB", &p)) && p != nullptr)
            g_fn.getDisplayRefreshRate = reinterpret_cast<PFN_xrGetDisplayRefreshRateFB>(p);
        XLog(std::string("XR_FB_display_refresh_rate: ") +
             (g_fn.getDisplayRefreshRate ? "available" : "advertised-but-unresolved"));
    }

    XrSystemGetInfo sgi = {};
    sgi.type = XR_TYPE_SYSTEM_GET_INFO_VALUE;
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY_VALUE;
    r = g_fn.getSystem(g_instance, &sgi, &g_system);
    XLog("xrGetSystem(HMD): " + FmtXr(r));
    if (!XrSucceeded(r)) return false;

    XrGraphicsRequirementsD3D11KHR req = {};
    req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR_VALUE;
    g_fn.getD3D11GraphicsRequirements(g_instance, g_system, &req);

    XrGraphicsBindingD3D11KHR binding = {};
    binding.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR_VALUE;
    binding.device = device;
    XrSessionCreateInfo sci = {};
    sci.type = XR_TYPE_SESSION_CREATE_INFO_VALUE;
    sci.next = &binding;
    sci.systemId = g_system;
    r = g_fn.createSession(g_instance, &sci, &g_session);
    XLog("xrCreateSession(GAME device): " + FmtXr(r));
    if (!XrSucceeded(r)) return false;

    XrReferenceSpaceCreateInfo rsci = {};
    rsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_VALUE;
    rsci.poseInReferenceSpace = IdentityPose();
    g_fn.createReferenceSpace(g_session, &rsci, &g_localSpace);

    // Base LOCAL space, never re-origined - used to read the absolute head at recenter time.
    XrReferenceSpaceCreateInfo bsci = {};
    bsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
    bsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_VALUE;
    bsci.poseInReferenceSpace = IdentityPose();
    g_fn.createReferenceSpace(g_session, &bsci, &g_baseSpace);

    // Head-locked space for the flat mono quad (the panel follows your head, like LE1's menu).
    XrReferenceSpaceCreateInfo vsci = {};
    vsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
    vsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW_VALUE;
    vsci.poseInReferenceSpace = IdentityPose();
    g_fn.createReferenceSpace(g_session, &vsci, &g_viewSpace);

    if (!ChooseSwapchainFormat()) { XLog("no swapchain format"); return false; }
    if (!CreateEyeSwapchains()) { XLog("eye swapchain creation failed"); return false; }

    g_initOk = true;
    XLog("VR session ready; starting frame submission.");
    return true;
}

void PumpEvents() noexcept
{
    XrEventDataBuffer ev = {};
    for (;;)
    {
        ev.type = XR_TYPE_EVENT_DATA_BUFFER_VALUE;
        const XrResult r = g_pollEvent(g_instance, &ev);
        if (r != XR_SUCCESS_VALUE) break;   // XR_EVENT_UNAVAILABLE or error
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED_VALUE)
        {
            const auto* ss = reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev);
            g_state = ss->state;
            if (g_state == XR_SESSION_STATE_READY_VALUE && !g_begun)
            {
                XrSessionBeginInfo bi = {};
                bi.type = XR_TYPE_SESSION_BEGIN_INFO_VALUE;
                bi.primaryViewConfigurationType = kStereo;
                const XrResult br = g_fn.beginSession(g_session, &bi);
                XLog("xrBeginSession: " + FmtXr(br));
                g_begun = XrSucceeded(br);
            }
            else if ((g_state == XR_SESSION_STATE_STOPPING_VALUE) && g_begun)
            {
                g_fn.endSession(g_session);
                g_begun = false;
            }
        }
    }
}

// Copy one HALF of the SBS backbuffer (starting at srcX) into this eye's swapchain image.
void CopyHalfToEye(int eye, ID3D11Texture2D* src, uint32_t srcX) noexcept
{
    if (g_swap[eye] == nullptr || src == nullptr) return;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {}; ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
    if (!XrSucceeded(g_fn.acquireSwapchainImage(g_swap[eye], &ai, &idx))) return;
    XrSwapchainImageWaitInfo wi = {}; wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE; wi.timeout = XR_INFINITE_DURATION_VALUE;
    if (XrSucceeded(g_fn.waitSwapchainImage(g_swap[eye], &wi)) && idx < g_images[eye].size())
    {
        D3D11_BOX box = {};
        box.left = srcX; box.right = srcX + g_swapW;
        box.top = 0;     box.bottom = g_swapH;
        box.front = 0;   box.back = 1;
        g_ctx->CopySubresourceRegion(g_images[eye][idx], 0, 0, 0, 0, src, 0, &box);
    }
    XrSwapchainImageReleaseInfo ri = {}; ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
    g_fn.releaseSwapchainImage(g_swap[eye], &ri);
}

// --- free head-look (orientation), ported from LE1 ---------------------------------------
void HeadEulerDegrees(const XrQuaternionf& q, float& yawDeg, float& pitchDeg) noexcept
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float fx = -2.0f * (x * z + w * y);
    const float fy = -2.0f * (y * z - w * x);
    const float fz = -(1.0f - 2.0f * (x * x + y * y));
    constexpr float kRad2Deg = 57.2957795f;
    const float cfy = fy < -1.0f ? -1.0f : (fy > 1.0f ? 1.0f : fy);
    yawDeg = atan2f(fx, -fz) * kRad2Deg;
    pitchDeg = asinf(cfy) * kRad2Deg;
}

// Strip head ROLL: keep where you're looking (forward) but force "up" toward world-up, so tilting
// your head never rolls the image. The submitted pose must be roll-free to match the roll-free render.
XrQuaternionf RemoveRoll(const XrQuaternionf& q) noexcept
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    float fx = -2.0f * (x * z + w * y);
    float fy = -2.0f * (y * z - w * x);
    float fz = -(1.0f - 2.0f * (x * x + y * y));
    const float fl = sqrtf(fx * fx + fy * fy + fz * fz);
    if (fl < 1e-5f) return q;
    fx /= fl; fy /= fl; fz /= fl;
    const float zx = -fx, zy = -fy, zz = -fz;             // camera back axis = -forward
    float rx = zz, ry = 0.0f, rz = -zx;                   // right = horizontal (worldUp x back)
    const float rl = sqrtf(rx * rx + ry * ry + rz * rz);
    if (rl < 1e-5f) return q;                             // looking straight up/down: leave as-is
    rx /= rl; ry /= rl; rz /= rl;
    const float ux = zy * rz - zz * ry;                   // up = back x right
    const float uy = zz * rx - zx * rz;
    const float uz = zx * ry - zy * rx;
    const float m00 = rx, m01 = ux, m02 = zx;
    const float m10 = ry, m11 = uy, m12 = zy;
    const float m20 = rz, m21 = uz, m22 = zz;
    XrQuaternionf o;
    const float tr = m00 + m11 + m22;
    if (tr > 0.0f)
    {
        const float s = sqrtf(tr + 1.0f) * 2.0f;
        o.w = 0.25f * s; o.x = (m21 - m12) / s; o.y = (m02 - m20) / s; o.z = (m10 - m01) / s;
    }
    else if (m00 > m11 && m00 > m22)
    {
        const float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        o.w = (m21 - m12) / s; o.x = 0.25f * s; o.y = (m01 + m10) / s; o.z = (m02 + m20) / s;
    }
    else if (m11 > m22)
    {
        const float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        o.w = (m02 - m20) / s; o.x = (m01 + m10) / s; o.y = 0.25f * s; o.z = (m12 + m21) / s;
    }
    else
    {
        const float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        o.w = (m10 - m01) / s; o.x = (m02 + m20) / s; o.y = (m12 + m21) / s; o.z = 0.25f * s;
    }
    return o;
}

XrQuaternionf YawToQuat(float yawDeg) noexcept
{
    const float h = -yawDeg * 0.5f / 57.2957795f;
    XrQuaternionf q; q.x = 0.0f; q.y = sinf(h); q.z = 0.0f; q.w = cosf(h);
    return q;
}

float WrapDeg(float d) noexcept { while (d > 180.0f) d -= 360.0f; while (d < -180.0f) d += 360.0f; return d; }

// Re-origin g_localSpace to the current head yaw (level) + position, read from the never-moved base space.
// [POSETAG] state (declared here because RecenterAppSpace below resets the ring; the smoothing and
// push helpers that use it live further down with DriveHeadLook).
XrQuaternionf g_smoothQuat = { 0, 0, 0, 1 };
bool g_smoothQuatInit = false;
// [POSETAG-TIME 2026-08-19] The ring is indexed by TIME, not by frame count.
// The old ring was 4 deep and indexed by presents: "delay 3" meant "the pose from 3 presents ago".
// That is only a fixed LATENCY while the frame rate is fixed. At a locked 120Hz, 3 presents = 25ms
// and the compositor reprojects against a constant offset, which is invisible. When the frame rate
// swings - exactly what the apartment party and the Citadel hubs do - 3 presents is 25ms at 120fps
// but 66ms at 45fps, so the pose-to-image offset CHANGES EVERY FRAME. A varying offset is what the
// eye reads as judder on head turns, and it is why this got worse precisely when the frame rate
// dropped. ME1/ME2 run at a stable rate, so the identical frame-indexed code never exposed it there.
// Now: each sample is stamped with its own QPC time, the delay is converted to seconds ONCE using
// the headset's nominal display period (g_displayPeriodSec, learned and locked for the session -
// never the live frame interval, which is the thing that is unstable), and the lookup interpolates
// between the two samples bracketing that timestamp. At a healthy 120fps this reproduces the old
// behaviour almost exactly; when the frame rate collapses the submitted latency stays put.
constexpr int kTagRingN = 16;   // 16 @ 120Hz = 133ms of history, far past any usable delay
struct TagSample { XrPosef pose; double t; };
TagSample g_tagRing[kTagRingN] = {};
int  g_tagRingHead = -1;        // index of the newest sample
int  g_tagRingCount = 0;
XrPosef g_delayedTagPose = { {0,0,0,1}, {0,0,0} };   // read by the SFR submit
std::atomic_bool g_poseTagReset{ true };             // reset the ring on recenter / first use
// [POSEHB] execution-proof counters. The 08-19 session produced ZERO [POSETAG] lines from a binary
// that verifiably contained the code, so nothing in this path gets to claim it ran without a number
// in the log. Read+reset once per FRAMETIME window by TakePoseStats.
std::atomic<uint32_t> g_poseTagPushes{0}, g_poseTagSeeds{0}, g_poseTagFreezes{0};

void RecenterAppSpace(XrTime displayTime) noexcept
{
    g_poseTagReset.store(true, std::memory_order_relaxed);   // [POSETAG] old ring samples are meaningless after re-origin
    if (g_baseSpace == nullptr) return;
    XrViewLocateInfo li = {};
    li.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
    li.viewConfigurationType = kStereo;
    li.displayTime = displayTime;
    li.space = g_baseSpace;
    XrViewState vs = {}; vs.type = XR_TYPE_VIEW_STATE_VALUE;
    uint32_t cnt = 0;
    XrView bv[2] = {}; bv[0].type = bv[1].type = XR_TYPE_VIEW_VALUE;
    if (!XrSucceeded(g_fn.locateViews(g_session, &li, &vs, 2, &cnt, bv)) || cnt < 2 ||
        (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT_VALUE) == 0)
        return;
    float yawDeg = 0.0f, pitchDeg = 0.0f;
    HeadEulerDegrees(bv[0].pose.orientation, yawDeg, pitchDeg);
    XrReferenceSpaceCreateInfo ci = {};
    ci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
    ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_VALUE;
    ci.poseInReferenceSpace.orientation = YawToQuat(yawDeg);
    ci.poseInReferenceSpace.position.x = (bv[0].pose.position.x + bv[1].pose.position.x) * 0.5f;
    ci.poseInReferenceSpace.position.y = (bv[0].pose.position.y + bv[1].pose.position.y) * 0.5f;
    ci.poseInReferenceSpace.position.z = (bv[0].pose.position.z + bv[1].pose.position.z) * 0.5f;
    XrSpace ns = nullptr;
    if (XrSucceeded(g_fn.createReferenceSpace(g_session, &ci, &ns)) && ns != nullptr)
    {
        if (g_localSpace != nullptr) g_fn.destroySpace(g_localSpace);
        g_localSpace = ns;
        XLog("recenter - app space re-origined to head yaw=" + std::to_string(static_cast<int>(yawDeg)));
    }
}

// Located head (already relative to the recentered localSpace) -> drive the render-side head-look.
// Normalized-lerp between two quaternions (shortest arc via sign fix). Cheap and stable for the small
// per-frame deltas here - no slerp needed.
XrQuaternionf NlerpQuat(const XrQuaternionf& a, XrQuaternionf b, float t) noexcept
{
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.0f) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; }
    XrQuaternionf r;
    r.x = a.x + (b.x - a.x) * t; r.y = a.y + (b.y - a.y) * t;
    r.z = a.z + (b.z - a.z) * t; r.w = a.w + (b.w - a.w) * t;
    const float l = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    if (l > 1e-6f) { r.x /= l; r.y /= l; r.z /= l; r.w /= l; }
    return r;
}
float QuatAngleDeg(const XrQuaternionf& a, const XrQuaternionf& b) noexcept
{
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    d = fabsf(d); if (d > 1.0f) d = 1.0f;
    return 2.0f * acosf(d) * 57.2957795f;
}

// [POSETAG] Head-tracking smoothness, ported from ME2 (originally ME1's HEAD_TRACKING_SMOOTHNESS_FIX).
// Two separate bugs are fixed here:
//  - ONE smoothed quaternion feeds BOTH the render (yaw/pitch -> SetHeadLook) and the submit tag, so
//    the pixels and the pose they are labelled with can never disagree. A per-axis Euler low-pass that
//    only smoothed the render is what produced the slow-turn "wobble then settle".
//  - A 4-deep ring of render poses lets the submit tag the captured backbuffer with the pose that
//    ACTUALLY rendered it (~poseTagDelayFrames presents back, because the mod arms on present and capture on
//    the next one, plus the DXGI queue depth) instead of the pose located this instant.
void PoseTagPush(const XrQuaternionf& smoothedRollFree, const XrVector3f& pos, double nowSec) noexcept
{
    XrPosef cur; cur.orientation = smoothedRollFree; cur.position = pos;
    if (g_poseTagReset.exchange(false, std::memory_order_relaxed) || g_tagRingCount == 0)
    {
        g_poseTagSeeds.fetch_add(1, std::memory_order_relaxed);   // [POSEHB]
        // Fresh start / post-recenter: seed the whole ring at this instant so any lookup resolves to
        // the current pose instead of interpolating against poses from a now-meaningless origin.
        for (int i = 0; i < kTagRingN; ++i) { g_tagRing[i].pose = cur; g_tagRing[i].t = nowSec; }
        g_tagRingHead = 0;
        g_tagRingCount = kTagRingN;
        g_delayedTagPose = cur;
        return;
    }
    g_tagRingHead = (g_tagRingHead + 1) % kTagRingN;
    g_tagRing[g_tagRingHead].pose = cur;
    g_tagRing[g_tagRingHead].t = nowSec;
    g_poseTagPushes.fetch_add(1, std::memory_order_relaxed);   // [POSETAG] stats, read per FRAMETIME window

    // [POSETAG-REVERT 2026-08-19] FRAME-indexed lookup, as ME1/ME2 always had it. The 08-19
    // time-indexed version was a wrong theory: the delay compensates the arm-on-present ->
    // render-next-present pipeline, and that latency is measured in GAME FRAMES, so at low fps the
    // correct tag really is proportionally older in wall time. Converting the delay to a fixed
    // wall-clock offset made the tag WRONGER exactly when the frame rate dropped. The timestamped
    // ring stays (it costs nothing and the stats read from it); only the lookup semantics revert.
    const float delay = ME2VR::CalcViewHook::GetPoseTagDelayFrames();
    int k0 = static_cast<int>(delay); if (k0 > kTagRingN - 1) k0 = kTagRingN - 1;
    int k1 = k0 + 1; if (k1 > kTagRingN - 1) k1 = kTagRingN - 1;
    const float frac = delay - static_cast<float>(k0);
    const int i0 = (g_tagRingHead - k0 + kTagRingN * 2) % kTagRingN;
    const int i1 = (g_tagRingHead - k1 + kTagRingN * 2) % kTagRingN;
    const XrPosef& a = g_tagRing[i0].pose;
    const XrPosef& b = g_tagRing[i1].pose;
    g_delayedTagPose.orientation = NlerpQuat(a.orientation, b.orientation, frac);
    g_delayedTagPose.position.x = a.position.x + (b.position.x - a.position.x) * frac;
    g_delayedTagPose.position.y = a.position.y + (b.position.y - a.position.y) * frac;
    g_delayedTagPose.position.z = a.position.z + (b.position.z - a.position.z) * frac;
}

void DriveHeadLook(const XrView& centerView) noexcept
{
    float yawDeg = 0.0f, pitchDeg = 0.0f;
    // Positional 6DOF: pose.position is the head's translation from recenter, in meters (XR axes,
    // already relative to the recentered localSpace). The render side maps it through the camera
    // basis in ApplyHeadPosition. Ported from ME2 2026-08-13.
    ME2VR::CalcViewHook::SetHeadPos(centerView.pose.position.x, centerView.pose.position.y, centerView.pose.position.z);

    // Adaptive follow: near 1:1 when the head moves fast (no lag on real turns), heavy low-pass when
    // nearly still (kills micro-jitter and lets the game's TAA converge).
    // [POSETAG-TIME 2026-08-19] Both halves of this were frame-rate dependent, the same defect as the
    // pose ring below, and both make head tracking feel worse exactly when the frame rate drops:
    //   - `speedDeg` is the head's movement in ONE FRAME, so the same real head speed reads as twice
    //     the "speed" at half the frame rate and wrongly saturates the pass-through ramp.
    //   - a fixed per-frame lerp coefficient is a low-pass whose TIME CONSTANT scales with frame time,
    //     so the smoothing lag itself grows and shrinks as the frame rate moves.
    // Both are now normalised against the nominal display period, so the filter behaves identically
    // in wall-clock terms at any frame rate. The ratio is clamped so one long hitch cannot make the
    // filter jump the head.
    const XrQuaternionf rawQ = centerView.pose.orientation;
    if (!g_smoothQuatInit) { g_smoothQuat = rawQ; g_smoothQuatInit = true; }
    LARGE_INTEGER hlNow; QueryPerformanceCounter(&hlNow);
    if (g_paceQpcFreq.QuadPart == 0) QueryPerformanceFrequency(&g_paceQpcFreq);
    const double nowSec = static_cast<double>(hlNow.QuadPart) / static_cast<double>(g_paceQpcFreq.QuadPart);
    static double s_lastHeadSec = 0.0;
    const double nominal = (g_displayPeriodSec > 1e-4 && g_displayPeriodSec < 0.1) ? g_displayPeriodSec : (1.0 / 120.0);
    double dt = (s_lastHeadSec > 0.0) ? (nowSec - s_lastHeadSec) : nominal;
    s_lastHeadSec = nowSec;
    if (dt < 1e-5) dt = 1e-5;
    double ratio = dt / nominal;                       // 1.0 at the headset's own rate
    if (ratio < 0.25) ratio = 0.25; else if (ratio > 8.0) ratio = 8.0;

    const float smoothing = ME2VR::CalcViewHook::GetHeadLookSmoothing();   // 0=off .. ~0.4 default
    const float speedRaw  = QuatAngleDeg(g_smoothQuat, rawQ);              // this frame's head jump
    const float speedDeg  = speedRaw / static_cast<float>(ratio);          // -> degrees per NOMINAL frame
    const float followSlow = 1.0f - smoothing * 0.95f;                     // heavy smoothing when still
    const float ramp = (speedDeg > 2.0f) ? 1.0f : (speedDeg * 0.5f);       // >2 deg/frame -> pass through ~1:1
    const float follow = followSlow + (1.0f - followSlow) * ramp;
    // Frame-rate-compensated exponential blend: equals `follow` exactly at the nominal rate.
    float followComp = follow;
    if (follow < 0.999f) followComp = 1.0f - powf(1.0f - follow, static_cast<float>(ratio));
    if (followComp > 1.0f) followComp = 1.0f; else if (followComp < 0.0f) followComp = 0.0f;
    g_smoothQuat = NlerpQuat(g_smoothQuat, rawQ, (smoothing > 0.001f) ? followComp : 1.0f);
    HeadEulerDegrees(g_smoothQuat, yawDeg, pitchDeg);                      // render consumes the SMOOTHED angle
    PoseTagPush(RemoveRoll(g_smoothQuat), centerView.pose.position, nowSec);   // submit tag = same pose, delayed

    // [VRCINE] head-tracking-OFF during a VR cine: park render rotation so the head does not
    // turn the view; the submit anchors the stereo pair at the current head instead (below), so
    // it behaves as a head-locked 3D picture - ME2's exact arrangement.
    if (ME2VR::CalcViewHook::GetVrCineActive() && !ME2VR::CalcViewHook::GetCineVrHeadTracking())
    {
        ME2VR::CalcViewHook::SetHeadLook(0, 0, false);
        return;
    }

    constexpr float kDegToUU = 65536.0f / 360.0f;
    // [HEADAIM] ME2's arrangement, ported exactly after the first cut broke SFR head tracking:
    // when aim owns rotation the camera follows ControlRotation, so view-look must not also turn.
    // But during the handoff ramp the UNTRANSFERRED remainder is still rendered as view-look
    // (signs converted back) so the two halves always sum to the full offset - the view never
    // moves while the aim glides to the gaze. The first cut instead set SetHeadLook(0,0,true)
    // with no remainder: view rotation froze outright while the pose tag still claimed head
    // tracking, and the compositor's reprojection made the whole world swim.
    // [FPSTORM] Storming with a weapon made looking left and right go the OPPOSITE way. Cause: head
    // aim stayed engaged during the sprint. Aim writes ControlRotation, but the game steers
    // ControlRotation itself while storming to face the run - so the mod's delta and the engine fight over
    // the same value every frame and the net response inverts. ME2 fixes this by dropping head aim
    // for the duration of the sprint and letting the ordinary render-side view-look handle looking
    // around, which nothing contests.
    // ME2 detects the sprint from its camera-mode name (SFXCameraMode_CombatStorm). LE3 has no camera
    // modes at all - it uses BioCameraBehavior - so the sprint is read straight off the pad instead:
    // A held with the stick pushed. HOLD while both persist, drop after 12 quiet frames so a momentary
    // stick dip mid-sprint cannot flap it. Gated on Default game mode so a menu or cutscene that
    // happens to leave a button latched can never hold the sprint state.
    const int  stickMag = ME2VR::Menu::LeftStickMagnitude();
    const bool sprintHeld = ME2VR::Menu::PadSprintHeld();
    const bool gameplay = (ME2VR::EngineProbe::ReadGameMode() == 0);
    static bool s_stormLatched = false;
    static int  s_stormRelease = 0;
    if (!gameplay) { s_stormLatched = false; s_stormRelease = 0; }
    else if (sprintHeld && stickMag > 20000) { s_stormLatched = true; s_stormRelease = 0; }
    else if (s_stormLatched)
    {
        if (!sprintHeld || stickMag < 12000) { if (++s_stormRelease > 12) s_stormLatched = false; }
        else s_stormRelease = 0;
    }

    // [HEADAIM] weapon-out gate (2026-08-13): the menu option always said "(combat)" but aim was
    // engaged through ALL of gameplay, so holstered exploring turned the whole character with the
    // head. Aim now engages only while a Combat-family camera mode is live (weapon out); holstered
    // walking keeps the free view-look below, which nothing contests. The handoff ramp in
    // DriveHeadAim covers draw/holster transitions the same way it covers the storm latch.
    // [AIMSCOPE 2026-08-14] The weapon-drawn restriction is now OPT-IN and off by default. Head aim
    // writes ControlRotation - the direction the CHARACTER faces - and LE3 resolves world
    // interactions against that facing, not against the mod's injected view rotation. Restricting aim to
    // combat therefore meant that while holstered the player could look directly at a vendor and the
    // game did not register them as facing it: measured 2026-08-14, shops were completely
    // uninteractable for the whole holstered session. Kept as a setting because the coupling is also
    // what makes exploration feel combat-like, which is the reason it was gated in the first place.
    // [COMBATCTX] One authoritative answer instead of a camera-name guess. In a hub the pawn is a
    // *NonCombat class and no weapon can ever be drawn, so head aim must never engage there however
    // the camera blends; in a mission area the camera family decides. IsCombatContext() holds its
    // value through SFXCameraMode_Interpolate, so transitions cannot produce a one-frame flip.
    // [AIMCOVER 2026-08-21] Cover is the same conflict as the storm latch above, and it went out in
    // v1.0: in cover the GAME writes ControlRotation itself for the lean, the peek and the
    // blind-fire pivot, so head aim writing it too means two authors every frame and the camera
    // swings off wherever the player is looking. It was described as "impossible to aim" from cover
    // and it was reproduced. Stand down while the game owns the value - looking around still
    // works, uncontested, through the render-side view-look below. DriveHeadAim ramps out (see
    // [AIMUNWIND]) so entering cover glides instead of snapping the view.
    const bool inCover = ME2VR::EngineProbe::IsInCover() && ME2VR::EngineProbe::GetHeadAimCoverOff();
    const bool aimOn = ME2VR::EngineProbe::GetHeadAimEnabled() && !s_stormLatched && !inCover &&
                       (!ME2VR::EngineProbe::GetHeadAimWeaponOnly() || ME2VR::EngineProbe::IsCombatContext()) &&
                       !ME2VR::CalcViewHook::GetCinematic() && !ME2VR::D3DCapture::GetMenuMode() &&
                       !ME2VR::CalcViewHook::GetVrCineActive();   // [VRCINE] the game owns the cine camera
    ME2VR::EngineProbe::DriveHeadAim(yawDeg, pitchDeg, aimOn);
    if (ME2VR::EngineProbe::IsHeadAimActive())
    {
        int remY = 0, remP = 0;
        ME2VR::EngineProbe::GetAimSeedRem(&remY, &remP);
        if (remY != 0 || remP != 0) ME2VR::CalcViewHook::SetHeadLook(-remY, remP, true);
        else                        ME2VR::CalcViewHook::SetHeadLook(0, 0, false);
    }
    else if (!ME2VR::CalcViewHook::GetHeadLookUserEnabled())
    {
        ME2VR::CalcViewHook::SetHeadLook(0, 0, false);
    }
    else
    {
        // These tune render-side free look only. Head aim keeps its independent inversion controls
        // and raw 1:1 angles so changing view comfort never changes weapon aiming.
        const float sensitivity = ME2VR::CalcViewHook::GetLookSensitivity();
        const float yawSign = ME2VR::CalcViewHook::GetInvertLookYaw() ? 1.0f : -1.0f;
        const float pitchSign = ME2VR::CalcViewHook::GetInvertLookPitch() ? -1.0f : 1.0f;
        float tunedPitch = pitchSign * pitchDeg * sensitivity;
        if (tunedPitch > 85.0f) tunedPitch = 85.0f;
        if (tunedPitch < -85.0f) tunedPitch = -85.0f;
        const int yawUU = static_cast<int>(yawSign * WrapDeg(yawDeg) * sensitivity * kDegToUU);
        const int pitchUU = static_cast<int>(tunedPitch * kDegToUU);
        ME2VR::CalcViewHook::SetHeadLook(yawUU, pitchUU, true);
    }
}

void EnsureGameUiSwapchain(uint32_t w, uint32_t h) noexcept
{
    if (g_gameUiSwap != nullptr) return;
    if (w == 0 || h == 0) return;
    g_gameUiW = w; g_gameUiH = h;
    XrSwapchainCreateInfo sc = {};
    sc.type = XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE;
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE | XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE;
    sc.format = g_swapFormat; sc.sampleCount = 1; sc.width = w; sc.height = h;
    sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
    if (!XrSucceeded(g_fn.createSwapchain(g_session, &sc, &g_gameUiSwap)) || g_gameUiSwap == nullptr) { g_gameUiSwap = nullptr; return; }
    uint32_t c = 0;
    g_fn.enumerateSwapchainImages(g_gameUiSwap, 0, &c, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(c);
    for (auto& im : imgs) im.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE;
    g_fn.enumerateSwapchainImages(g_gameUiSwap, c, &c, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    g_gameUiImages.clear();
    for (auto& im : imgs) g_gameUiImages.push_back(im.texture);
    XLog("game-UI overlay swapchain created " + std::to_string(w) + "x" + std::to_string(h));
}

void EnsureUiSwapchain(uint32_t w, uint32_t h) noexcept
{
    if (g_uiSwap != nullptr) return;
    if (w == 0 || h == 0) return;
    g_uiW = w; g_uiH = h;
    XrSwapchainCreateInfo sc = {};
    sc.type = XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE;
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE | XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE;
    sc.format = g_swapFormat;
    sc.sampleCount = 1; sc.width = w; sc.height = h; sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
    if (!XrSucceeded(g_fn.createSwapchain(g_session, &sc, &g_uiSwap)) || g_uiSwap == nullptr) { g_uiSwap = nullptr; return; }
    uint32_t c = 0;
    g_fn.enumerateSwapchainImages(g_uiSwap, 0, &c, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(c);
    for (auto& im : imgs) im.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE;
    g_fn.enumerateSwapchainImages(g_uiSwap, c, &c, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    g_uiImages.clear();
    for (auto& im : imgs) g_uiImages.push_back(im.texture);
    XLog("UI quad swapchain created " + std::to_string(w) + "x" + std::to_string(h));
}

void EnsureMonoSwapchain() noexcept
{
    if (g_monoSwap != nullptr || g_bbW == 0 || g_swapH == 0) return;
    XrSwapchainCreateInfo sc = {};
    sc.type = XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE;
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE | XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE;
    sc.format = g_swapFormat;
    sc.sampleCount = 1; sc.width = g_bbW; sc.height = g_swapH; sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
    if (!XrSucceeded(g_fn.createSwapchain(g_session, &sc, &g_monoSwap)) || g_monoSwap == nullptr) { g_monoSwap = nullptr; return; }
    uint32_t c = 0;
    g_fn.enumerateSwapchainImages(g_monoSwap, 0, &c, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(c);
    for (auto& im : imgs) im.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE;
    g_fn.enumerateSwapchainImages(g_monoSwap, c, &c, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    g_monoImages.clear();
    for (auto& im : imgs) g_monoImages.push_back(im.texture);
    XLog("mono fallback swapchain created " + std::to_string(g_bbW) + "x" + std::to_string(g_swapH));
}

// ============================ AER (alternate-eye rendering) ============================
// Full-size eye swapchains (full backbuffer, NOT the SBS half) - AER copies a whole frame per eye.
bool EnsureAerSwapchains() noexcept
{
    if (g_aerSwap[0] != nullptr && g_aerSwap[1] != nullptr) return true;
    g_aerSwapW = g_bbW;
    g_aerSwapH = g_swapH;   // = full backbuffer height
    if (g_aerSwapW == 0 || g_aerSwapH == 0) return false;
    for (int eye = 0; eye < 2; ++eye)
    {
        XrSwapchainCreateInfo sc = {};
        sc.type = XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE;
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE | XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE;
        sc.format = g_swapFormat;
        sc.sampleCount = 1; sc.width = g_aerSwapW; sc.height = g_aerSwapH;
        sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
        if (!XrSucceeded(g_fn.createSwapchain(g_session, &sc, &g_aerSwap[eye])) || g_aerSwap[eye] == nullptr)
        { g_aerSwap[eye] = nullptr; return false; }
        uint32_t c = 0;
        g_fn.enumerateSwapchainImages(g_aerSwap[eye], 0, &c, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> imgs(c);
        for (auto& im : imgs) im.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE;
        g_fn.enumerateSwapchainImages(g_aerSwap[eye], c, &c, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
        g_aerImages[eye].clear();
        for (auto& im : imgs) g_aerImages[eye].push_back(im.texture);
    }
    XLog("AER full-size eye swapchains created " + std::to_string(g_aerSwapW) + "x" + std::to_string(g_aerSwapH));
    return true;
}

void ResetAerHistory() noexcept
{
    for (int e = 0; e < 2; ++e)
    {
        if (g_aerHist[e] != nullptr) { g_aerHist[e]->Release(); g_aerHist[e] = nullptr; }
        g_aerHistValid[e] = false;
        g_aerEyeFilled[e] = false;   // [AERBLINK] swapchain content is stale for this bank
    }
    g_aerHistDescValid = false;
    g_aerHistPoseValid[0] = false; g_aerHistPoseValid[1] = false;
    g_aerLastSeq = 0;
    g_aerPrevCaptureEye = -1;
    g_aerSameEyeTwice = 0; g_aerSeqGapBad = 0; g_aerMaxSeqGap = 0;
    ME2VR::CalcViewHook::SetAerRenderEye(0);
}

bool EnsureAerHistory(ID3D11Texture2D* src) noexcept
{
    if (g_device == nullptr || src == nullptr) return false;
    D3D11_TEXTURE2D_DESC d = {};
    src->GetDesc(&d);
    d.BindFlags = 0; d.CPUAccessFlags = 0; d.MiscFlags = 0; d.Usage = D3D11_USAGE_DEFAULT;
    if (g_aerHistDescValid && g_aerHist[0] && g_aerHist[1] &&
        g_aerHistDesc.Width == d.Width && g_aerHistDesc.Height == d.Height && g_aerHistDesc.Format == d.Format)
        return true;
    ResetAerHistory();
    for (int e = 0; e < 2; ++e)
        if (FAILED(g_device->CreateTexture2D(&d, nullptr, &g_aerHist[e]))) { ResetAerHistory(); XLog("AER history texture creation FAILED"); return false; }
    g_aerHistDesc = d; g_aerHistDescValid = true;
    XLog("AER history bank created " + std::to_string(d.Width) + "x" + std::to_string(d.Height));
    return true;
}

// Full-frame -> whole eye (the UN-SQUASH). src/history/g_aerSwap are all full-backbuffer size, so a straight
// CopyResource works (no aspect crop = no squash of the game's ~square-FOV-rendered-into-16:9 backbuffer).
// shiftPx != 0 shifts the copied image horizontally inside the eye texture ([SFRCONV] convergence).
// The destination is cleared first so the vacated strip is black rather than stale. shiftPx == 0 takes
// the plain whole-resource copy, so every non-SFR path is bit-identical to before.
bool CopyTextureToEyeFullFrame(int eye, ID3D11Texture2D* src, int shiftPx) noexcept
{
    if (src == nullptr || g_aerSwap[eye] == nullptr) return false;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {}; ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
    if (!XrSucceeded(g_fn.acquireSwapchainImage(g_aerSwap[eye], &ai, &idx))) return false;
    bool copied = false;   // [AERBLINK] the caller must know if the image really changed
    XrSwapchainImageWaitInfo wi = {}; wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE; wi.timeout = XR_INFINITE_DURATION_VALUE;
    if (XrSucceeded(g_fn.waitSwapchainImage(g_aerSwap[eye], &wi)) && idx < g_aerImages[eye].size())
    {
        if (shiftPx == 0)
        {
            g_ctx->CopyResource(g_aerImages[eye][idx], src);
            copied = true;
        }
        else
        {
            const int w = static_cast<int>(g_aerSwapW), h = static_cast<int>(g_aerSwapH);
            int sx0 = 0, dx0 = 0;
            if (shiftPx > 0) { dx0 = shiftPx; }     // content moves right; left edge stays cleared
            else             { sx0 = -shiftPx; }    // content moves left
            const int copyW = w - ((shiftPx > 0) ? shiftPx : -shiftPx);
            if (copyW > 0)
            {
                D3D11_BOX box = {};
                box.left = static_cast<UINT>(sx0); box.right = static_cast<UINT>(sx0 + copyW);
                box.top = 0; box.bottom = static_cast<UINT>(h);
                box.front = 0; box.back = 1;
                g_ctx->CopySubresourceRegion(g_aerImages[eye][idx], 0,
                                             static_cast<UINT>(dx0), 0, 0, src, 0, &box);
                copied = true;
            }
        }
    }
    XrSwapchainImageReleaseInfo ri = {}; ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
    g_fn.releaseSwapchainImage(g_aerSwap[eye], &ri);
    return copied;
}
inline bool CopyTextureToEyeFullFrame(int eye, ID3D11Texture2D* src) noexcept
{ return CopyTextureToEyeFullFrame(eye, src, 0); }

// Capture the freshly-rendered eye into its history slot (drift-free stamp handshake), then submit BOTH
// slots (one fresh, one ~1 frame old). The eye that actually landed drives which eye renders next.
// headPose = the current head pose (roll-removed, centered); stored per-slot so the STALE eye is later
// submitted at the orientation it was rendered facing (anti-ghost pose-tag).
bool SubmitAerFrame(ID3D11Texture2D* backBuffer, const XrPosef& headPose) noexcept
{
    if (backBuffer == nullptr) return false;
    ++g_aerPresent;
    if (!EnsureAerSwapchains() || !EnsureAerHistory(backBuffer))
    {
        CopyTextureToEyeFullFrame(0, backBuffer);   // bootstrap on alloc failure
        CopyTextureToEyeFullFrame(1, backBuffer);
        return true;
    }
    // [AERSHAKE, ported from ME2] FIFO consume: the OLDEST unconsumed build - the one whose pixels
    // this present actually shows - never "the latest stamp", whose eye can belong to a build still
    // in flight. The latest-read is what made the world vibrate between the two eye positions at
    // full rate (pipeline depth flaps -> eye label flaps against the pixels).
    uint64_t seq = 0; int eye = -1; bool resynced = false;
    const bool newFrame = ME2VR::CalcViewHook::ConsumeAerStamp(g_aerLastSeq, &seq, &eye, &resynced) &&
                          eye >= 0 && eye <= 1;
    if (newFrame)
    {
        // Alternation health. With the FIFO, a resync (fell a whole ring behind: mode switch, long
        // menu, load) is the only legitimate discontinuity; anything else here is a real bug again.
        if (resynced) { ++g_aerSeqGapBad; if (seq - g_aerLastSeq > g_aerMaxSeqGap) g_aerMaxSeqGap = seq - g_aerLastSeq; }
        if (eye == g_aerPrevCaptureEye) ++g_aerSameEyeTwice;   // same eye twice = alternation stall = shear
        g_aerPrevCaptureEye = eye;

        g_ctx->CopyResource(g_aerHist[eye], backBuffer);
        g_aerHistValid[eye] = true;
        g_aerHistPose[eye] = headPose;          // tag THIS eye with the pose it was rendered at (anti-ghost)
        g_aerHistPoseValid[eye] = true;
        g_aerLastSeq = seq;
        const ULONGLONG now = GetTickCount64();
        if (g_aerHzWindowStartMs == 0) { g_aerHzWindowStartMs = now; g_aerHzCaptures = 0; }
        ++g_aerHzCaptures;
        const ULONGLONG el = now - g_aerHzWindowStartMs;
        if (el >= 2000 && g_aerHzEmitted < 30)
        {
            const double capHz = static_cast<double>(g_aerHzCaptures) * 1000.0 / static_cast<double>(el);
            char line[256] = {};
            sprintf_s(line, "[AERHZ] captureHz=%.1f captures=%u sameEyeTwice=%u seqGapBad=%u maxSeqGap=%llu halfEyeUU=%.2f",
                      capHz, g_aerHzCaptures, g_aerSameEyeTwice, g_aerSeqGapBad,
                      static_cast<unsigned long long>(g_aerMaxSeqGap), ME2VR::CalcViewHook::GetAerHalfEyeUU());
            XLog(line);
            ++g_aerHzEmitted;
            g_aerHzWindowStartMs = now; g_aerHzCaptures = 0;
            g_aerSameEyeTwice = 0; g_aerSeqGapBad = 0; g_aerMaxSeqGap = 0;
        }
    }
    // [AERBLINK 2026-08-21] Copy ONLY what changed. This used to acquire/wait/copy BOTH full-size
    // eyes every present - three 5120x2880 copies and two swapchain waits per present, two of which
    // re-uploaded content that had not changed since the previous present. Every swapchain wait is a
    // chance for the runtime's compositor (VDXR streams over the network) to block the render thread
    // mid-present; the [PACE2]/[XRSUBMIT] windows show exactly those multi-frame stalls, and each
    // one lands in the headset as a blink. OpenXR keeps presenting a swapchain's most recently
    // RELEASED image until a new one is released, so the STALE eye needs no work at all, and a
    // present with no fresh render needs no copies whatsoever.
    if (g_aerHistValid[0] && g_aerHistValid[1])
    {
        if (newFrame && CopyTextureToEyeFullFrame(eye, g_aerHist[eye])) g_aerEyeFilled[eye] = true;
        // Catch-up: an eye whose swapchain has never held real stereo content (the first pair after
        // bootstrap, or a swapchain that outlived a history reset) gets one top-up copy.
        for (int e = 0; e < 2; ++e)
            if (!g_aerEyeFilled[e] && CopyTextureToEyeFullFrame(e, g_aerHist[e])) g_aerEyeFilled[e] = true;
    }
    else
    {
        // Bootstrap until both slots captured once: the same mono frame to both eyes, refreshed
        // every present. Deliberately does NOT set g_aerEyeFilled - the first real stereo pair must
        // overwrite this mono content via the catch-up above.
        CopyTextureToEyeFullFrame(0, backBuffer);
        CopyTextureToEyeFullFrame(1, backBuffer);
    }
    return true;
}


// [VRFILL] Widen the game view to fill the headset. Ported from ME2 2026-07-28, where this was a shared
// function; ME3 had it copy-pasted at four call sites (stereo / AER / DIBR / mono), which is how the
// LINKFOV2 clamp nearly went in three times and got missed once. One definition, four callers.
//   VrFovFill off  -> render at the game's own FOV (no widening at all).
//   VrFillH / VrFillV -> per-axis trim, 1.0 = fill the headset exactly, lower = narrower.
void ApplyFovFill(const XrView* views) noexcept
{
    const float aspect = (g_swapH != 0) ? static_cast<float>(g_swapW) / static_cast<float>(g_swapH) : 0.889f;
    const float headVHalf = (views[0].fov.angleUp - views[0].fov.angleDown) * 0.5f;
    if (headVHalf <= 0.1f) return;
    if (!g_vrFovFillEnabled.load(std::memory_order_relaxed))
    {
        ME2VR::CalcViewHook::SetFovFill(0.0f, 0.0f, false);   // off -> render at the game's own FOV
        return;
    }
    const float fh = g_vrFillH.load(std::memory_order_relaxed);
    const float fv = g_vrFillV.load(std::memory_order_relaxed);
    float tV = atanf(tanf(headVHalf) * fv);
    float tH = atanf(tanf(tV) * aspect * (fh / (fv > 1e-4f ? fv : 1.0f)));
    // [LINKFOV2] On the Meta runtime the fill must also COVER the runtime's asymmetric per-eye frustum:
    // the compositor maps whatever rect the mod submits onto ITS OWN frustum, and the submit-side crop can only
    // trim a declared window DOWN, never grow one that falls short. An aspect-derived target sits inside
    // Quest's outer edge, so each eye gets stretched outward in opposite directions = unfusable double
    // image, independent of separation and convergence. Scale up by ONE uniform tan-space factor so the
    // crop lands exactly on the frustum; per-axis clamping changes the rendered H:V ratio and warps the UI.
    if (g_isOculusRuntime && g_questFovMatch.load(std::memory_order_relaxed))
    {
        float hNeed = 0.0f, vNeed = 0.0f;
        for (int e = 0; e < 2; ++e)
        {
            const XrFovf& f = views[e].fov;
            if (-f.angleLeft > hNeed) hNeed = -f.angleLeft;
            if (f.angleRight > hNeed) hNeed = f.angleRight;
            if (f.angleUp    > vNeed) vNeed = f.angleUp;
            if (-f.angleDown > vNeed) vNeed = -f.angleDown;
        }
        const float tanH = tanf(tH), tanV = tanf(tV);
        if (hNeed > 0.1f && hNeed < 1.4f && vNeed > 0.1f && vNeed < 1.4f && tanH > 1e-4f && tanV > 1e-4f)
        {
            float k = 1.0f;
            const float kH = tanf(hNeed) / tanH, kV = tanf(vNeed) / tanV;
            if (kH > k) k = kH;
            if (kV > k) k = kV;
            if (k > 1.0f) { tH = atanf(tanH * k); tV = atanf(tanV * k); }
        }
    }
    ME2VR::CalcViewHook::SetFovFill(tH, tV, true);
}

void RunFrame() noexcept
{
    // Display-locked pacing: hold the present to display/2 BEFORE waitFrame so each frame lands on a stable
    // integer fraction of the headset refresh -> kills the 60fps-into-120Hz micro-shake. No-op until Hz known.
    // VR mode (0=Mono, 1=Stereo, 2=AER, 3=DIBR) - ported from ME2 2026-07-12.
    constexpr int kModeStereo = 1;
    constexpr int kModeAer = 2;
    constexpr int kModeDibr = 3;
    constexpr int kModeSfr = 4;
    const int vrMode = ME2VR::CalcViewHook::GetVrMode();
    const bool aerMode = (vrMode == kModeAer);
    const bool stereoMode = (vrMode == kModeStereo);
    const bool dibrMode = (vrMode == kModeDibr);
    const bool sfrMode = (vrMode == kModeSfr);   // [SFR] same-frame stereo: double-render, pass0->L pass1->R

    // Free the AER history bank + reset the eye toggle whenever AER isn't the active mode (edge-triggered).
    if (!aerMode && g_wasAer) ResetAerHistory();
    g_wasAer = aerMode;

    // DIBR: depth is captured at ClearDSV (mode-gated), the warp shader synthesizes the right eye from
    // the mono frame + depth. Enable the whole capture/warp pipeline ONLY in mode 3.
    ME2VR::D3DCapture::SetDepthMapEnabled(dibrMode);
    if (dibrMode)
    {
        // Auto-convergence: put whatever the player is looking at on the screen plane.
        // STABILITY (ME2 2026-07-12 rework, ported): median-of-9 central region input (d3d_capture) +
        // DEADBAND + RATE LIMIT here - sub-noise wobble never moves the plane, a real subject change
        // glides over ~1s, a scene cut snaps in a few frames.
        static float s_dibrAutoConv = 0.985f;
        float pcC = 0.0f, tl = 0.0f, br = 0.0f, tr = 0.0f;
        ME2VR::D3DCapture::GetDepthProbe(&pcC, &tl, &br, &tr);
        const bool autoConv = ME2VR::CalcViewHook::GetDibrAutoConverge();
        if (autoConv && pcC > 0.0f)
        {
            const float err = pcC - s_dibrAutoConv;
            const float mag = (err < 0.0f) ? -err : err;
            if (mag > 0.0015f)   // deadband: depth noise smaller than this never moves the plane
            {
                float step = 0.10f * err;
                const float maxStep = (mag > 0.03f) ? 0.012f : 0.0012f;   // scene cut: fast; otherwise: glide
                if (step > maxStep) step = maxStep;
                else if (step < -maxStep) step = -maxStep;
                s_dibrAutoConv += step;
            }
        }
        const float conv = autoConv ? s_dibrAutoConv : ME2VR::CalcViewHook::GetDepthWarpConv();
        ME2VR::D3DCapture::SetDibrWarp(ME2VR::CalcViewHook::GetDepthWarpGain(), conv,
                                       ME2VR::CalcViewHook::GetDepthWarpFlip());
    }

    // Display-locked cadence: AER needs it so the eye-swap can't drift into flicker; stereo keeps its
    // existing 60fps-into-120Hz judder fix. SFR renders twice per present so it sits around the same
    // engine-bound rate - same regularisation, same StereoFramePacing knob (SFR IS "Stereo" in the
    // picker now; the old stereoMode-only gate left mode 4 with no pacing at all).
    const bool wantPace = (aerMode && ME2VR::CalcViewHook::GetAerFramePacing()) ||
                          ((stereoMode || sfrMode) && ME2VR::CalcViewHook::GetStereoFramePacing());
    // [AERFULL 2026-08-20] AER is no longer forced to display/2. The old clamp protected the Hz
    // auto-learner from an oscillation loop (pace-to-half lets the game coast -> looks fast ->
    // upgrade -> pace-to-full loads it -> looks slow -> downgrade); with the refresh now locked once
    // per session from XR_FB_display_refresh_rate that loop cannot form, and the clamp was costing
    // AER exactly its reason to exist: at 90Hz it meant 45 presents/s and each eye crawling at
    // ~22Hz - "slow and doesn't run well". Full-rate AER is the AFW shape: one eye rendered per
    // present at up to the display rate, so each eye refreshes at ~display/2 while the engine pays
    // for ONE view per frame - half of SFR's submission cost, which is the measured hub bottleneck.
    // FullRefreshPacing (the same knob Stereo uses) picks full vs half; the shipped ini already has it on.
    if (wantPace) PaceDisplayLocked(true);

    XrFrameWaitInfo fwi = {}; fwi.type = XR_TYPE_FRAME_WAIT_INFO_VALUE;
    XrFrameState fs = {}; fs.type = XR_TYPE_FRAME_STATE_VALUE;
    // [FRAMETIME] time the runtime's own throttle so the present-hook accounting can subtract it.
    if (g_paceQpcFreq.QuadPart == 0) QueryPerformanceFrequency(&g_paceQpcFreq);
    LARGE_INTEGER wf0; QueryPerformanceCounter(&wf0);
    const XrResult wfr = g_fn.waitFrame(g_session, &fwi, &fs);
    LARGE_INTEGER wf1; QueryPerformanceCounter(&wf1);
    g_lastWaitFrameUs.store(static_cast<unsigned>((wf1.QuadPart - wf0.QuadPart) * 1000000ll / g_paceQpcFreq.QuadPart),
                            std::memory_order_relaxed);
    if (!XrSucceeded(wfr)) return;

    // Learn the headset refresh so the pace above locks correctly (pinned Hz else measured).
    // Only while pacing is actually wanted: with pacing off the learner has no consumer, and on
    // VDXR the predictedDisplayTime jitter made it unlock/relock every ~6s = 40 [PACE] lines per
    // two-minute session for a feature that was not running.
    if (wantPace)
        UpdatePaceWarmup(fs.predictedDisplayTime,
                         aerMode ? ME2VR::CalcViewHook::GetAerFramePacingHz()
                                 : ME2VR::CalcViewHook::GetStereoFramePacingHz());

    XrFrameBeginInfo fbi = {}; fbi.type = XR_TYPE_FRAME_BEGIN_INFO_VALUE;
    g_fn.beginFrame(g_session, &fbi);

    // B2b stereo: project the two SBS halves to the two eyes. Each eye declares the GAME's rendered
    // FOV (not the headset FOV -> no zoom) and the runtime's located eye pose (head tracking).
    XrCompositionLayerProjectionView projViews[2] = {};
    XrCompositionLayerProjection layer = {};
    bool rendered = false;

    // SBS this frame? The split bumps a counter each gameplay frame, but it can skip the odd present
    // (~7 of 8). Switching mono<->SBS on a single skipped frame caused the flashing. Use hysteresis:
    // reset on any split, count idle presents, and stay in SBS until the split has been idle a while.
    const unsigned long long curSplit = ME2VR::CalcViewHook::GetSplitSeq();
    if (curSplit != g_prevSplitSeq) { g_prevSplitSeq = curSplit; g_framesSinceSplit = 0; }
    else if (g_framesSinceSplit < 100000u) { ++g_framesSinceSplit; }
    // Final mono/stereo choice is made below, after both authoritative controllers update.
    bool sbsFrame = false;

    // Every frame: detect conversation/cutscene by RAW half-FOV magnitude (cinematics narrow <=30deg,
    // gameplay >=35deg). Hysteresis in the gap (enter <32, exit >34) so it never sticks. RAW so the
    // gameplay fill can't fool it. calcview keeps publishing FOV in mono so the mod also detects the END.
    {
        constexpr float kEnterRad = 0.5585f;   // 32 deg
        constexpr float kExitRad  = 0.5934f;   // 34 deg
        const float gh = ME2VR::CalcViewHook::GetGameRawFovH();
        // [VRCINE] hysteresis memory lives HERE now, not in the stored flag: the stored flag
        // becomes "present this cine FLAT", which is false while a cine renders in VR - reading
        // it back for hysteresis would flap the detector between enter and exit thresholds.
        static bool s_fovCine = false;
        bool cine = s_fovCine;

        // [ADSVR] Aiming down sights narrows the FOV, and FOV alone was the ONLY thing deciding
        // "this is a cutscene" - so every ADS got forced to flat mono, which is exactly what it
        // looked like in the headset: the camera zooms and the world goes 2D. ME2 vetoes this with
        // weapon-out. LE3 has no equivalent the mod can reach, but the game mode enum says the same thing
        // more directly: a real cutscene is Conversation(5) or Cinematic(6) and NEVER Default(0), so a
        // narrow FOV while the mode is Default is a zoom, not a cutscene. Mode -1 (unreadable) falls
        // through to the old FOV-only rule rather than trusting a failed read.
        // [GMSTICKY] a failed mode read carries no information, so HOLD the last good value
        // instead of publishing -1. -1 was fail-open to VR-eligible below, and a one-frame read
        // glitch during a FLAT-classified menu flipped it into "VR cutscene" (measured 2026-08-13,
        // twice: 07:42:39 and 07:46:51, both mid-menu at fov 48.7) - the fill widened the menu's
        // 3D render onto the flat panel for the length of the glitch. Read failures cluster around
        // map transitions where every consumer wants the previous state anyway.
        const int gmRead = ME2VR::EngineProbe::ReadGameMode();
        static int s_lastGoodGm = -1;
        if (gmRead >= 0) s_lastGoodGm = gmRead;
        const int gm = (gmRead >= 0) ? gmRead : s_lastGoodGm;
        // [AUTOMENU] publish it for the menu/flat gate: GUI, movie, galaxy and orbital are flat
        // sources, and every mono call site already reads GetMenuMode().
        ME2VR::D3DCapture::SetAutoGameMode(gm);
        // [CINECAM] The ADS veto originally keyed on gm==0 - but LE3 has NO Conversation(5) or
        // Cinematic(6) modes (neither has EVER appeared in a log), and its in-engine cutscenes stay
        // at gm 0, so the veto was silently killing cutscene detection outright. The reliable LE3
        // signal is the CAMERA: gameplay - including ADS, the case the veto exists for - always runs
        // an SFXCameraMode_* camera, while conversations run BioCameraBehaviorConversation (observed
        // live) and staged cutscenes leave the mode system entirely. Veto only under a gameplay
        // camera; force cine under a conversation camera regardless of FOV.
        const bool convoCam    = ME2VR::EngineProbe::IsConvoCamera();
        const bool fpConvo = ME2VR::ConvoFp::OwnsCamera();
        const bool fpController = ME2VR::ConvoFp::IsArmed();
        const bool gameplayCam = ME2VR::EngineProbe::IsGameplayCamera();
        // [GAMEPLAYVR] ME2 parity. LE2 treats the WHOLE gameplay family as definitively-not-cinematic
        // (0 Default, 1 PowerWheel, 2 WeaponWheel, 3 Command, 4 Vehicle); ME3 only ever vetoed mode 0.
        // That gap is a flat-screen trap for anything the player PILOTS: a vehicle/mech cockpit view
        // is a gameplay camera (so it fails the !gameplayCam eligibility guard) with a narrowish
        // framing FOV (so it trips the cine detector) - the exact combination that rendered photo mode
        // flat. The Atlas already runs at gm 2, and mode 4 is literally Vehicle, so the Leviathan
        // DLC's Triton deep-diving mech is squarely in this hole. Conversations are unaffected: they
        // run gm 7, and the convoCam force below outranks this regardless.
        const bool gameplayVeto = (gm >= 0 && gm <= 4) && gameplayCam;
        if (gh > 0.01f)
        {
            if (gh < kEnterRad && !gameplayVeto) cine = true;
            else if (gh > kExitRad || gameplayVeto) cine = false;
        }
        if (convoCam) cine = true;
        if (fpConvo) cine = true;

        // [ENGCINE] gm 8 "Movie" covers TWO different things in LE3: prerendered bik movies AND
        // in-engine staged cutscenes (the Tuchanka reaper run was observed live at gm 8, wide FOV,
        // scene camera fully alive - which is why "gm 8 = always flat" silently killed VR cutscenes).
        // The discriminator is CalcSceneView activity: a bik builds no P1 scene views (calcViews
        // ~0/s observed during one), an in-engine cutscene builds them every frame. The seq counter
        // bumps on EVERY calcview branch, so forcing mono can't stall it into a false "bik" read.
        static uint64_t s_prevCalcSeq = 0;
        static int s_calcStalePresents = 0;
        static int s_aliveRun = 0;   // consecutive presents WITH calcview activity (entry discipline)
        const uint64_t calcSeq = ME2VR::CalcViewHook::GetP1CalcSeq();
        if (calcSeq != s_prevCalcSeq)
        {
            s_prevCalcSeq = calcSeq; s_calcStalePresents = 0;
            if (s_aliveRun < 100000) ++s_aliveRun;
        }
        else { if (s_calcStalePresents < 1000) ++s_calcStalePresents; s_aliveRun = 0; }
        const bool sceneAlive = s_calcStalePresents <= 4;
        // [GM8FOV 2026-08-18] A second LE3 in-engine cinematic path updates the director camera only
        // intermittently, so it fails both historical gm-8 proofs: the retained SFX gameplay camera
        // stays visible and P1 CalcSceneView is stale for >4 presents. The Priority: Earth scene
        // measured 70 -> 52 -> 28 -> 40 degrees while every recorded loading/movie episode held an
        // exactly stale 70.0 degrees. Accumulate real camera motion and latch it for this gm-8 episode;
        // three distinct changes spanning 1.5 degrees rejects float noise without delaying the scene.
        // The latch resets only when gm-8 ends, so static shots inside the same cutscene cannot flap.
        static int s_prevGmForFov = -1;
        static float s_gm8LastFovDeg = 0.0f;
        static float s_gm8MinFovDeg = 0.0f;
        static float s_gm8MaxFovDeg = 0.0f;
        static int s_gm8FovMotionSamples = 0;
        static bool s_gm8AnimatedCamera = false;
        const float rawFovDeg = gh * 2.0f * 57.2957795f;
        if (gm != 8)
        {
            s_gm8LastFovDeg = s_gm8MinFovDeg = s_gm8MaxFovDeg = 0.0f;
            s_gm8FovMotionSamples = 0;
            s_gm8AnimatedCamera = false;
        }
        else if (rawFovDeg > 1.0f)
        {
            if (s_prevGmForFov != 8 || s_gm8LastFovDeg <= 1.0f)
            {
                s_gm8LastFovDeg = s_gm8MinFovDeg = s_gm8MaxFovDeg = rawFovDeg;
                s_gm8FovMotionSamples = 0;
            }
            else
            {
                if (rawFovDeg < s_gm8MinFovDeg) s_gm8MinFovDeg = rawFovDeg;
                if (rawFovDeg > s_gm8MaxFovDeg) s_gm8MaxFovDeg = rawFovDeg;
                if (fabsf(rawFovDeg - s_gm8LastFovDeg) >= 0.20f)
                {
                    s_gm8LastFovDeg = rawFovDeg;
                    if (s_gm8FovMotionSamples < 100000) ++s_gm8FovMotionSamples;
                }
                if (!s_gm8AnimatedCamera && s_gm8FovMotionSamples >= 3 &&
                    s_gm8MaxFovDeg - s_gm8MinFovDeg >= 1.5f)
                {
                    s_gm8AnimatedCamera = true;
                    char b[192] = {};
                    sprintf_s(b, "[GM8FOV] animated cinematic earned VR (range %.1f..%.1f, samples %d)",
                              static_cast<double>(s_gm8MinFovDeg),
                              static_cast<double>(s_gm8MaxFovDeg), s_gm8FovMotionSamples);
                    XLog(b);
                }
            }
        }
        s_prevGmForFov = gm;
        // [MOVIEMONO] gm 8 is overloaded. A real bik/loading movie has no live scene; a genuine
        // in-engine cutscene leaves the gameplay camera. But the enum also blips to 8 while BOTH a
        // live scene and gameplay camera remain active (measured 2026-08-13 at 10:59:34), which used
        // to flatten ordinary gameplay for about a second. Classify mode 8 only with corroborating
        // scene/camera evidence. A conversation camera remains cinematic regardless of the enum.
        // Do not let render/capture staleness set the cinematic suppression flag when the gameplay
        // camera has returned. SetCinematic suppresses SFR generation itself, so using missing
        // captures or CalcSceneView work here forms a circular mono lock: a save can restore its
        // SFX gameplay camera in gm 8 but remain flat until the enum eventually reaches gm 0.
        // Movie/loading ownership is still proved by the missing gameplay camera; if a flat source
        // happens to retain one, the independent submit-topology capture guard keeps it mono without
        // preventing SFR from probing for the first recoverable gameplay pair.
        if (gm == 8 && !convoCam)
            cine = s_gm8AnimatedCamera || !gameplayCam;
        // [PHOTOVR] Photo mode is a free camera for framing shots - it is neither a cinematic nor a
        // menu, and it is the one place the player is deliberately LOOKING at the world, so flat is
        // the worst possible presentation for it. Vetoing cine here drops it through to the ordinary
        // gameplay path (full SFR stereo with fill) rather than routing it through the cine fill and
        // its zoom, which is meant for director-framed shots. Applied last so the FOV hysteresis and
        // the mode-8 rule above cannot put it back. Exact class match, so nothing else is affected.
        if (ME2VR::EngineProbe::IsPhotoCamera()) cine = false;
        s_fovCine = cine;

        // [CONVOLATCH] LE3 swaps the camera CLASS per shot (a Grissom close-up ran ~4.5s on a
        // non-Conversation class), so once the conversation CAMERA has been seen, LATCH the convo
        // presentation for as long as the scene stays cinematic - per-shot classes cannot flap it.
        // ENTRY is the conversation camera ONLY: the first cut keyed entry on "gm 7 + unpaused"
        // (pause was documented as the menu discriminator), but the squad/loadout menus run gm 7
        // UNPAUSED in LE3 (measured: every gm-7 [CINEVR] line logged paused 0), so those menus
        // classified as conversations, VrCineActive disabled the automenu, and ApplyFov's fill
        // widened the 3D Shepard render onto the 16:9 panel = the stretched menu Shepard. The cost
        // of camera-only entry is ~1s of flat panel at convo start until the camera class arrives.
        // The latch still drops on pause so a menu opened MID-convo presents flat.
        const bool paused = ME2VR::EngineProbe::IsGamePaused();
        // [LATCHDECAY] the latch's only exits were !cine and pause - but the squad/outfit menus
        // run 38-50 deg UNPAUSED (cine stays true), so entering one straight from a conversation
        // (briefing -> squad select) carried the latch for the whole visit: VrCineActive held,
        // GetMenuMode stayed false, and the fill widened the menu's 3D models = the stretched
        // menu Shepard again, through a door CONVOLATCH left open. In a real conversation the
        // BioCameraBehaviorConversation class recurs at every shot change (longest measured
        // non-convo-class shot: ~4.5s, the Grissom close-up); in a menu it never comes back. So:
        // decay the latch after 12s without the convo camera. False decay in an unusually long
        // single shot costs a flat blip that self-heals at the next shot change (convoCam
        // re-latches); a carried latch in a menu costs stretched models for the entire visit.
        static bool s_convoVrLatch = false;
        static unsigned long long s_lastConvoCamMs = 0;
        const unsigned long long nowMs = GetTickCount64();
        if (convoCam) s_lastConvoCamMs = nowMs;
        if (!cine || paused) s_convoVrLatch = false;
        else if (convoCam) s_convoVrLatch = true;
        else if (s_convoVrLatch && nowMs - s_lastConvoCamMs > 12000ull) s_convoVrLatch = false;
        // [CONVOSCOPE 2026-08-18] ME3 can keep the conversation controller alive while handing an
        // embedded shot to a director/cutscene camera. With first-person conversations enabled,
        // the old controller/latch classification relocated those authored shots to Shepard's eyes
        // and kept the head hidden. Current conversation-camera ownership is the boundary: dialogue
        // shots use first person; controller-alive director shots use the VR-cutscene toggle.
        const bool hybridDirectorShot = ME2VR::ConvoFp::GetEnabled() && fpController && !fpConvo;
        const bool convoLike = fpConvo || convoCam || (s_convoVrLatch && !hybridDirectorShot);

        // [ENGCINE] gm-8 ENTRY discipline: level-streaming loads flap calcview activity in bursts
        // while gameplayCam and the FOV hold STALE gameplay values (27 false "VR cutscene" flips at
        // fov 73.5 / gameplayCam 1 in one session - the loading movie stretched into stereo, and
        // movie subtitles drawn in those windows took the frozen cine UI ratio = squashed). A real
        // staged cutscene leaves the SFXCameraMode system and runs CalcSceneView continuously. Enter
        // only after 20 consecutive live presents with a non-gameplay camera, and exit when that
        // scene activity goes stale. Capture age is intentionally NOT an ownership signal here:
        // VR-cine must be enabled before SFR can produce its first post-movie capture. The independent
        // submit-topology guard below holds mono until that fresh pair actually exists. Consulting
        // capture age in both places created the measured two-second movie-exit mono lock.
        static bool s_gm8Vr = false;
        if (gm != 8) s_gm8Vr = false;
        else if (!s_gm8Vr)
            s_gm8Vr = ((s_aliveRun >= 20) && !gameplayCam) || s_gm8AnimatedCamera;
        else if (!s_gm8AnimatedCamera && !sceneAlive)
            s_gm8Vr = false;

        // [VRCINE] ME2's split, with LE3's convo signal: when the matching toggle is ON, the
        // cine renders in the active VR mode instead of dropping to the flat panel. Real biks,
        // loading movies and menus are flat SOURCES/overlays - never eligible no matter the
        // toggles. Conversation = convoLike above; anything else = cutscene, including QUALIFIED
        // in-engine gm-8 scenes ([ENGCINE] above).
        // A real non-conversation cutscene leaves the gameplay camera. Journal/outfit-class screens
        // keep that camera and can briefly report gm -1/0; allowing those values alone promoted a
        // narrow mono menu to "VR cutscene" mid-visit. Camera ownership is mandatory.
        // [CINEALLOW 2026-08-14] This was an ALLOW-LIST of game modes (0, -1, then 7, then 8...) and
        // that shape is the bug, not the individual entries: every scene type whose mode was not on
        // the list played FLAT, so each new one had to be discovered in the headset and added by
        // hand. LE3 scenes are not obliged to use a mode the mod has already seen.
        // Inverted to a DENY-list. The positive case is stated directly instead: the camera has left
        // the gameplay system over a live scene, so a cinematic owns the view. Only modes MEASURED to
        // be genuine flat sources are denied:
        //   gm 9  galaxy map      (measured 04:54:43, gameplayCam 0 + live scene + 46 deg)
        //   gm 10 title/orbital   (measured 04:54:37, gameplayCam 0 + live scene + 42 deg)
        //   gm 8  movie, unless [ENGCINE] has earned it - that is the bik/loading guard
        // Everything else cinematic is now VR by default rather than by enumeration.
        // The guards that make this safe are unchanged and all independently measured: gameplay,
        // ADS, cover and every squad/journal/loadout menu run a GAMEPLAY-class camera (which is why
        // the old camera gate never armed), so !gameplayCam excludes them all; sceneAlive excludes
        // biks and loading screens; narrow-FOV `cine` is still required; and a latched menu still
        // vetoes below.
        const bool gmFlatSource = (gm == 9) || (gm == 10) || (gm == 8 && !s_gm8Vr);
        // s_gm8Vr is already a corroborated gm-8 scene: normally live P1 + non-gameplay camera,
        // or [GM8FOV]'s latched animated alternate camera. Do not demand those original proofs a
        // second time here; that made the new path impossible to bootstrap.
        const bool vrEligible = convoLike || (gm == 8 && s_gm8Vr) ||
                                (!gameplayCam && sceneAlive && !gmFlatSource);
        // The presentation controller is authoritative. Any latched menu state cancels inherited
        // VR-cine state as well, including pause-owned menus that do not expose a stable GUI object.
        const bool menuMono = ME2VR::D3DCapture::GetMenuMode();
        const bool renderCineInVr = cine && vrEligible && (!menuMono || fpConvo) &&
                                    (fpConvo || (convoLike ? ME2VR::CalcViewHook::GetCineVrConvo()
                                                          : ME2VR::CalcViewHook::GetCineVrCutscene()));
        ME2VR::CalcViewHook::SetVrCineActive(renderCineInVr);
        ME2VR::CalcViewHook::SetCinematic(cine && !renderCineInVr);
        // [CINEVR] one line per presentation flip: this is what verifies (or corrects) the LE3
        // convo/cutscene mapping from a single play session.
        static int s_prevCineState = -1;
        const int cineState = !cine ? 0 : (renderCineInVr ? (convoLike ? 2 : 3) : 1);
        static const char* kNames[4] = { "none", "FLAT panel", "VR conversation", "VR cutscene" };
        if (cineState != s_prevCineState)
        {
            // [XSTATE] a banter-heavy scene (party, hub crowd) fires this edge once per NPC
            // conversation start/end; count + time it here so a burst shows up in [FRAMETIME]
            // without hand-correlating a hitch against the surrounding log by timestamp.
            LARGE_INTEGER xt0; QueryPerformanceCounter(&xt0);
            s_prevCineState = cineState;
            char b[224] = {};
            sprintf_s(b, "[CINEVR] %s   (gm %d, convoCam %d, gameplayCam %d, sceneAlive %d, paused %d, fov %.1f)",
                      kNames[cineState], gm, convoCam ? 1 : 0, gameplayCam ? 1 : 0, sceneAlive ? 1 : 0,
                      paused ? 1 : 0, static_cast<double>(gh * 2.0f * 57.2957795f));
            XLog(b);
            LARGE_INTEGER xt1; QueryPerformanceCounter(&xt1);
            ME2VR::D3DCapture::NoteCineTransition(xt0, xt1);
        }
        // [CINEHB] heartbeat while any cine classification is active: one line per ~3s. The flip
        // log only shows instantaneous values at the transition; menu-vs-conversation questions
        // need the values DURING the state (a menu shows convoCam stuck 0 and camAge climbing, a
        // real conversation shows camAge resetting at every shot change). This is what verifies
        // LATCHDECAY against a natural play session.
        if (cine || s_convoVrLatch)
        {
            static unsigned long long s_lastHbMs = 0;
            if (nowMs - s_lastHbMs >= 3000ull)
            {
                s_lastHbMs = nowMs;
                char b[224] = {};
                sprintf_s(b, "[CINEHB] %s   (gm %d, convoCam %d, latch %d, camAge %.1fs, menuMode %d, paused %d, fov %.1f)",
                          kNames[cineState], gm, convoCam ? 1 : 0, s_convoVrLatch ? 1 : 0,
                          s_lastConvoCamMs != 0 ? static_cast<double>(nowMs - s_lastConvoCamMs) * 0.001 : -1.0,
                          ME2VR::D3DCapture::GetMenuMode() ? 1 : 0, paused ? 1 : 0,
                          static_cast<double>(gh * 2.0f * 57.2957795f));
                XLog(b);
            }
        }
        // [CINETRACE] PROBE ONLY - reads state and writes a log line, changes NOTHING. The 3s
        // heartbeat and the on-change flip line are both too coarse for the one unsolved case: the
        // mono blip at the START of a conversation, where the game locks input BEFORE the camera
        // hands over, so for a moment the state is identical to a menu opening (input locked +
        // gameplay-class camera + no conversation camera yet) and BOTH the menu controller and the
        // cine controller independently flatten the screen. Fixing that needs the frame-by-frame
        // handoff, so: on any change of presentation (cine state or menu mode) trace every present
        // for ~1.5s. Windows are capped so a long session cannot flood the log.
        // What to look for in the party-conversation trace:
        //   - the first frame where cine=1 while cam=G (gameplay camera still owns it) -> that is
        //     the ambiguous window, and elig/vr/flat/menu on those lines say which owner flattened.
        //   - gui= is the named full-screen menu census: if it stays 0 through the whole handoff
        //     while a real menu shows gui=1, that is the discriminator the fix can key on WITHOUT
        //     touching the menu path (this is the part that must not regress).
        //   - how many presents pass between cine=1 and cam losing G, i.e. how long the window is.
        if (ME2VR::Log::DiagnosticsOn())
        {
            static int s_traceLeft = 0;
            static int s_traceWindows = 0;
            static int s_prevKey = -1;
            // Key on PRESENTATION only. Deliberately not on convoCam/gameplayCam: LE3 flips the
            // camera class per shot, and arming on that would spend every window inside long
            // conversations instead of on the transitions being investigated.
            const int key = cineState | (menuMono ? 16 : 0);
            if (key != s_prevKey)
            {
                s_prevKey = key;
                if (s_traceWindows < 60) { s_traceLeft = 120; ++s_traceWindows; }
            }
            if (s_traceLeft > 0)
            {
                --s_traceLeft;
                unsigned char mv = 0, lk = 0;
                const bool owned = ME2VR::EngineProbe::IsMenuInputOwned(&mv, &lk);
                char t[288] = {};
                sprintf_s(t, "[CINETRACE] gm=%d cam=%c%c alive=%d cine=%d elig=%d vr=%d flat=%d menu=%d "
                             "move=%u look=%u owned=%d gui=%d latch=%d fov=%.1f",
                          gm, gameplayCam ? 'G' : '-', convoCam ? 'C' : '-', sceneAlive ? 1 : 0,
                          cine ? 1 : 0, vrEligible ? 1 : 0, renderCineInVr ? 1 : 0,
                          ME2VR::CalcViewHook::GetCinematic() ? 1 : 0, menuMono ? 1 : 0,
                          static_cast<unsigned>(mv), static_cast<unsigned>(lk), owned ? 1 : 0,
                          ME2VR::EngineProbe::GetNamedGuiActive() ? 1 : 0, s_convoVrLatch ? 1 : 0,
                          static_cast<double>(gh * 2.0f * 57.2957795f));
                XLog(t);
            }
        }
    }

    // One final decision, once per frame. MENU_MONO and flat cinematics are explicit owners. A
    // sustained absence of SFR captures is render-topology evidence that the live source is a
    // loading/movie/photo-result frame even when ME3 reports gm 0 (Citadel transit, 2026-08-17).
    // This is deliberately submit-only: setting the cinematic flag here would stop capture and make
    // the test self-sustaining, which is how an earlier generalized fix stranded whole cutscenes in
    // mono. The renderer keeps running, and a fresh captured pair is the automatic exit condition.
    const bool menuMono = ME2VR::D3DCapture::GetMenuMode();
    const bool flatCine = ME2VR::CalcViewHook::GetCinematic();
    const uint64_t topologyCaptureAge = ME2VR::D3DCapture::SfrCaptureAgePresents();
    const bool captureTopologyMono = sfrMode && !ME2VR::CalcViewHook::GetVrCineActive() &&
                                     topologyCaptureAge > kStaleCapturePresents;
    const bool stereoIntent = !menuMono && !flatCine && !captureTopologyMono;

    // [SOURCEEPISODE 2026-08-16] Once a real flat owner (loading/movie/menu/cinematic) has taken
    // presentation, do not switch back to projection merely because one heuristic dropped. Hold the
    // flat panel until SFR has produced a genuinely fresh pass-0 image; that capture is the terminal
    // proof that a complete stereo gameplay frame exists again. This is the pre-2e58081 behavior that
    // made loading screens stable. It is safe to restore now because [BINDRACE2] fixed the capture
    // detector that previously stranded this gate after photo mode and other render stalls.
    static bool s_sfrNeedsFreshPair = true;
    if (sfrMode)
    {
        if (!stereoIntent) s_sfrNeedsFreshPair = true;
        else if (s_sfrNeedsFreshPair && ME2VR::D3DCapture::SfrCaptureAgePresents() <= 1)
            s_sfrNeedsFreshPair = false;
    }
    const bool stereoSourceReady = sfrMode
        ? (!s_sfrNeedsFreshPair && ME2VR::D3DCapture::GetSfrPass0Texture() != nullptr)
        : (g_framesSinceSplit < kSbsHoldFrames);
    sbsFrame = stereoIntent && stereoSourceReady;
    {
        static int previous = -1;
        const int current = menuMono ? 1 : (flatCine ? 2 : (captureTopologyMono ? 4 : (sbsFrame ? 0 : 3)));
        if (current != previous)
        {
            previous = current;
            static const char* names[] = { "GAMEPLAY_STEREO", "MENU_MONO", "CINEMATIC_MONO", "SOURCE_WAIT", "CAPTURE_STALL_MONO" };
            XLog(std::string("[PRESENTATION] submit=") + names[current]);
        }
    }

    XrCompositionLayerQuad monoQuad = {};
    bool haveMono = false;

    // [MONOPROJ 2026-08-14] Mono GAMEPLAY is presented exactly like stereo - a projection layer with a
    // declared per-eye FOV and a render pose - just with the same image in both eyes. It used to fall
    // into the flat-quad fallback below, and every mono defect traced to that one choice: a quad has
    // no declared FOV, so the deliberately anamorphic fill render was never un-squeezed (the vertical
    // squash); and it is a fixed-size world-locked rectangle, so head tracking could not reproject and
    // the panel drifted off-centre as the camera turned. Patching the quad's shape and feeding it
    // head-look treated symptoms - the quad itself was wrong for a primary presentation.
    // Menus and cinematics in mono still use the quad, exactly as they do in every other mode.
    const bool monoGameplay = (ME2VR::CalcViewHook::GetVrMode() == 0) && stereoIntent;

    if (fs.shouldRender && !sbsFrame && !dibrMode && !monoGameplay)
    {
        // MONO fallback (menu / loading / any non-split frame): show the WHOLE backbuffer to both
        // eyes as one quad -> no halving, no cross-eye.
        EnsureMonoSwapchain();
        ID3D11Texture2D* backbuffer = nullptr;
        IDXGISwapChain* sc = ME2VR::D3DCapture::GetGameSwapChain();
        if (sc) sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer));
        if (g_monoSwap != nullptr && backbuffer != nullptr)
        {
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai = {}; ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
            if (XrSucceeded(g_fn.acquireSwapchainImage(g_monoSwap, &ai, &idx)))
            {
                XrSwapchainImageWaitInfo wi = {}; wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE; wi.timeout = XR_INFINITE_DURATION_VALUE;
                if (XrSucceeded(g_fn.waitSwapchainImage(g_monoSwap, &wi)) && idx < g_monoImages.size())
                    g_ctx->CopyResource(g_monoImages[idx], backbuffer);
                XrSwapchainImageReleaseInfo ri = {}; ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
                g_fn.releaseSwapchainImage(g_monoSwap, &ri);

                // The quad shows BACKBUFFER content, so its shape must come from the backbuffer alone.
                // This mixed g_swapH (the PER-EYE swapchain height) with g_bbW (the backbuffer width) -
                // two different resolutions - so the height came out wrong and every mono menu was
                // squashed vertically.
                const unsigned bbW = ME2VR::D3DCapture::GetBackbufferWidth();
                const unsigned bbH = ME2VR::D3DCapture::GetBackbufferHeight();
                const float aspect = (bbW != 0 && bbH != 0)
                                   ? static_cast<float>(bbH) / static_cast<float>(bbW)
                                   : 0.5625f;
                monoQuad.type = XR_TYPE_COMPOSITION_LAYER_QUAD_VALUE;
                // WORLD-locked (g_localSpace), not head-locked: the flat panel (galaxy map etc.) stays
                // put when you turn your head instead of following the camera. Recenter re-centers it.
                monoQuad.space = g_localSpace;
                monoQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH_VALUE;
                monoQuad.subImage.swapchain = g_monoSwap;
                monoQuad.subImage.imageRect.offset = { 0, 0 };
                monoQuad.subImage.imageRect.extent = { static_cast<int32_t>(g_bbW), static_cast<int32_t>(g_swapH) };
                monoQuad.subImage.imageArrayIndex = 0;
                monoQuad.pose = IdentityPose();
                monoQuad.pose.position.z = -g_monoQuadDistanceM.load(std::memory_order_relaxed);
                float mw = g_monoQuadWidthM.load(std::memory_order_relaxed);
                // [VRCINE] cutscene zoom applies only to the flat CINE screen, never to menus -
                // ME2's model (the panel itself is already world-locked here, matching ME2's
                // cine screen; menus share it by design in ME3).
                if (ME2VR::CalcViewHook::GetCinematic())
                    mw *= g_cineScreenZoom.load(std::memory_order_relaxed);
                monoQuad.size.width = mw;
                monoQuad.size.height = mw * aspect;
                haveMono = true;
            }
        }
        if (backbuffer) backbuffer->Release();
        if (g_modeLogCount.fetch_add(1, std::memory_order_relaxed) < 4) XLog("frame mode = MONO (no split) -> full-screen quad to both eyes");
    }
    else if (fs.shouldRender && monoGameplay)
    {
        // [MONOPROJ] Identical to the SFR submit below in every respect except that both eyes receive
        // the SAME image and convergence is zero - mono has no disparity by definition. Everything
        // that makes stereo correct therefore applies here too: the declared FOV matches what was
        // rendered (so the anamorphic fill is un-squeezed by the compositor), and the pair carries the
        // delayed render pose (so the compositor reprojects and head tracking is stable).
        static bool s_autoRecenteredMono = false;
        if (!s_autoRecenteredMono || g_recenterRequested.exchange(false, std::memory_order_relaxed))
        {
            RecenterAppSpace(fs.predictedDisplayTime);
            s_autoRecenteredMono = true;
        }

        XrViewLocateInfo vli = {};
        vli.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
        vli.viewConfigurationType = kStereo;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = g_localSpace;
        XrViewState vs = {}; vs.type = XR_TYPE_VIEW_STATE_VALUE;
        uint32_t viewCount = 0;
        XrView views[2] = {}; views[0].type = views[1].type = XR_TYPE_VIEW_VALUE;
        const XrResult lr = g_fn.locateViews(g_session, &vli, &vs, 2, &viewCount, views);
        if (XrSucceeded(lr) && viewCount == 2) DriveHeadLook(views[0]);

        ID3D11Texture2D* backbuffer = nullptr;
        IDXGISwapChain* sc = ME2VR::D3DCapture::GetGameSwapChain();
        if (sc) sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer));

        if (XrSucceeded(lr) && viewCount == 2 && backbuffer && EnsureAerSwapchains())
        {
            ApplyFovFill(views);
            const float gh = ME2VR::CalcViewHook::GetGameHalfFovH();
            const float gv = ME2VR::CalcViewHook::GetGameHalfFovV();

            // Same frame to both eyes, no convergence shift: there is no disparity to converge.
            CopyTextureToEyeFullFrame(0, backbuffer, 0);
            CopyTextureToEyeFullFrame(1, backbuffer, 0);

            XrPosef headPose = g_delayedTagPose;
            if (ME2VR::CalcViewHook::GetVrCineActive() && !ME2VR::CalcViewHook::GetCineVrHeadTracking())
            {
                headPose.orientation = RemoveRoll(views[0].pose.orientation);
                headPose.position.x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
                headPose.position.y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
                headPose.position.z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;
            }

            if (g_aerSwap[0] != nullptr && g_aerSwap[1] != nullptr)
            {
                for (int eye = 0; eye < 2; ++eye)
                {
                    XrFovf fov = views[eye].fov;
                    if (gh > 0.01f && gv > 0.01f) { fov.angleLeft = -gh; fov.angleRight = gh; fov.angleUp = gv; fov.angleDown = -gv; }
                    projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE;
                    projViews[eye].pose = headPose;
                    projViews[eye].fov = fov;
                    projViews[eye].subImage.swapchain = g_aerSwap[eye];
                    projViews[eye].subImage.imageRect.offset = { 0, 0 };
                    projViews[eye].subImage.imageRect.extent = { static_cast<int32_t>(g_aerSwapW), static_cast<int32_t>(g_aerSwapH) };
                    projViews[eye].subImage.imageArrayIndex = 0;
                }
                layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE;
                layer.space = g_localSpace;
                layer.viewCount = 2;
                layer.views = projViews;
                rendered = true;
                if (g_aerModeLogCount.fetch_add(1, std::memory_order_relaxed) < 4)
                    XLog("frame mode = MONO (projection) -> same image both eyes, declared FOV + render pose");
            }
        }
        if (backbuffer) backbuffer->Release();
    }
    else if (fs.shouldRender && aerMode)
    {
        // AER: calcview renders ONE laterally-offset eye per present (it arms g_aerRenderEye + stamps which
        // eye landed). The mod captures that into a 2-slot history bank and submit BOTH slots (one fresh, one ~1
        // frame old) under ONE shared latched head pose -- the stereo disparity lives in the PIXELS, never
        // in the pose tag. That shared pose is what stops AER from ghosting when you turn your head.
        static bool s_autoRecenteredAer = false;
        if (!s_autoRecenteredAer || g_recenterRequested.exchange(false, std::memory_order_relaxed))
        {
            RecenterAppSpace(fs.predictedDisplayTime);
            s_autoRecenteredAer = true;
        }

        XrViewLocateInfo vli = {};
        vli.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
        vli.viewConfigurationType = kStereo;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = g_localSpace;
        XrViewState vs = {}; vs.type = XR_TYPE_VIEW_STATE_VALUE;
        uint32_t viewCount = 0;
        XrView views[2] = {}; views[0].type = views[1].type = XR_TYPE_VIEW_VALUE;
        const XrResult lr = g_fn.locateViews(g_session, &vli, &vs, 2, &viewCount, views);

        // Drive free head-look from the located (recentered) head orientation.
        if (XrSucceeded(lr) && viewCount == 2) DriveHeadLook(views[0]);

        ID3D11Texture2D* backbuffer = nullptr;
        IDXGISwapChain* sc = ME2VR::D3DCapture::GetGameSwapChain();
        if (sc) sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer));

        if (XrSucceeded(lr) && viewCount == 2 && backbuffer)
        {
            // FOV-FILL (gameplay): widen the wide gameplay view to fill the headset (same target as stereo).
            ApplyFovFill(views);

            // Game's rendered half-FOV (radians) -> declared per eye (must EXACTLY match what was rendered).
            const float gh = ME2VR::CalcViewHook::GetGameHalfFovH();
            const float gv = ME2VR::CalcViewHook::GetGameHalfFovV();

            // This frame's head pose (roll removed, centered). Position = head CENTER (average of both eye
            // positions), matching ME1's headPos - not eye 0 (which would shift the world half an IPD left).
            // Computed BEFORE submit so the freshly-captured eye is tagged with it.
            XrPosef headPose = views[0].pose;
            headPose.orientation = RemoveRoll(views[0].pose.orientation);
            headPose.position.x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
            headPose.position.y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
            headPose.position.z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;

            // Capture the freshly-rendered offset eye into history (tagged with headPose) + fill BOTH full-size
            // AER eye swapchains (full-frame -> whole eye = the un-squash). Allocates g_aerSwap/g_aerHist first use.
            SubmitAerFrame(backbuffer, headPose);

            if (g_aerSwap[0] != nullptr && g_aerSwap[1] != nullptr)
            {
                for (int eye = 0; eye < 2; ++eye)
                {
                    XrFovf fov = views[eye].fov;
                    if (gh > 0.01f && gv > 0.01f) { fov.angleLeft = -gh; fov.angleRight = gh; fov.angleUp = gv; fov.angleDown = -gv; }

                    // ANTI-GHOST (the sacred pose-tag): submit each eye at the pose it was RENDERED at (per-slot),
                    // NOT the shared current pose. Fresh eye's stored pose == headPose (just captured); the STALE
                    // eye keeps its OLDER capture pose, so the compositor reprojects it to align -> no turn ghost.
                    projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE;
                    projViews[eye].pose = g_aerHistPoseValid[eye] ? g_aerHistPose[eye] : headPose;
                    projViews[eye].fov = fov;
                    projViews[eye].subImage.swapchain = g_aerSwap[eye];
                    projViews[eye].subImage.imageRect.offset = { 0, 0 };
                    projViews[eye].subImage.imageRect.extent = { static_cast<int32_t>(g_aerSwapW), static_cast<int32_t>(g_aerSwapH) };
                    projViews[eye].subImage.imageArrayIndex = 0;
                }
                layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE;
                layer.space = g_localSpace;
                layer.viewCount = 2;
                layer.views = projViews;
                rendered = true;
                if (!g_submitLogged.exchange(true))
                    XLog("=== AER: first alternate-eye projection submitted (gameFovH=" + std::to_string(gh) + " rad) ===");
                if (g_aerModeLogCount.fetch_add(1, std::memory_order_relaxed) < 4) XLog("frame mode = AER (alternate-eye) -> both eyes, one shared pose");
            }
        }
        if (backbuffer) backbuffer->Release();
    }
    else if (fs.shouldRender && sfrMode)
    {
        // [SFR] same-frame stereo: the frame was rendered TWICE this present (the DrawDetour replay).
        // Pass 0 (-halfEye = LEFT) was snapshotted mid-present at the replay's depth clear; pass 1
        // (+halfEye = RIGHT) is the backbuffer now. Submit pass0->left, pass1->right, full-frame.
        // Both eyes are FULL PRIMARY renders from the SAME instant, so every full-screen pass ran for
        // each of them - that is what kills the right-eye bloom loss and the right-eye-only blue line -
        // and one shared head pose is correct (no AER-style staleness between eyes).
        static bool s_autoRecenteredSfr = false;
        if (!s_autoRecenteredSfr || g_recenterRequested.exchange(false, std::memory_order_relaxed))
        {
            RecenterAppSpace(fs.predictedDisplayTime);
            s_autoRecenteredSfr = true;
        }

        XrViewLocateInfo vli = {};
        vli.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
        vli.viewConfigurationType = kStereo;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = g_localSpace;
        XrViewState vs = {}; vs.type = XR_TYPE_VIEW_STATE_VALUE;
        uint32_t viewCount = 0;
        XrView views[2] = {}; views[0].type = views[1].type = XR_TYPE_VIEW_VALUE;
        const XrResult lr = g_fn.locateViews(g_session, &vli, &vs, 2, &viewCount, views);
        if (XrSucceeded(lr) && viewCount == 2) DriveHeadLook(views[0]);

        ID3D11Texture2D* backbuffer = nullptr;
        IDXGISwapChain* sc = ME2VR::D3DCapture::GetGameSwapChain();
        if (sc) sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer));

        if (XrSucceeded(lr) && viewCount == 2 && backbuffer && EnsureAerSwapchains())
        {
            // FOV-FILL: widen the gameplay view to fill the headset (identical to AER/DIBR/stereo).
            ApplyFovFill(views);
            // What was RENDERED is what the mod declares - never reshape the submit FOV (ME2's regression).
            const float gh = ME2VR::CalcViewHook::GetGameHalfFovH();
            const float gv = ME2VR::CalcViewHook::GetGameHalfFovV();

            ID3D11Texture2D* pass0 = ME2VR::D3DCapture::GetSfrPass0Texture();   // left (-halfEye), mid-present
            const bool swap = ME2VR::CalcViewHook::GetSwapEyes();
            // [STALEEYE 2026-08-15] pass0 is a HELD texture: it only changes when a scene render is
            // captured. The right eye is the live backbuffer, so when the scene stops rendering while
            // the presentation is still stereo - a loading screen entered straight from gameplay, the
            // frozen scene behind the Citadel photo - one eye keeps updating and the other shows the
            // last captured frame. Measured as "the right eye freezes" because SwapEyes=1 routes the
            // held image to eye slot 1. The warm-up gate above cannot cover this: it only re-arms when
            // leaving a MONO state, and these stalls never leave stereo.
            // This is deliberately NOT the old capture-age demotion to the mono quad (see the comment
            // at the presentation decision): that made a second presentation owner and left the game
            // flat after menus. Nothing here touches presentation state, latches or modes - it only
            // chooses this frame's image SOURCE, so a stale capture shows both eyes the live frame
            // (zero disparity, like mono gameplay) and the pair self-heals on the next capture.
            const uint64_t capAge = ME2VR::D3DCapture::SfrCaptureAgePresents();
            const bool staleCapture = (pass0 != nullptr) && (capAge > kStaleCapturePresents);
            {
                static bool s_wasStale = false;
                if (staleCapture != s_wasStale)
                {
                    s_wasStale = staleCapture;
                    XLog(staleCapture
                             ? "[STALEEYE] capture stalled (" + std::to_string(capAge) +
                                   " presents) -> both eyes live frame until it resumes"
                             : "[STALEEYE] capture resumed -> stereo pair restored");
                }
            }
            ID3D11Texture2D* leftSrc  = (pass0 != nullptr && !staleCapture) ? pass0 : backbuffer;  // mono until 1st capture / while stalled
            ID3D11Texture2D* rightSrc = backbuffer;                                // right (+halfEye) = pass 1 (now)
            // [SFRCONV] convergence at SUBMIT, on the finished eye IMAGES (world and UI already
            // composited): left content shifts right (+), right content shifts left (-). Doing this at
            // render instead would move the world in one pass but split the HUD by 2*conv, because the
            // HUD is composited identically into both passes. Sign is bolted to the PASS, so swap-eyes
            // inverts the convergence plane exactly as it should. conv=0 -> plain copy, nothing touched.
            const float conv = ME2VR::CalcViewHook::GetSfrConvergence();
            // Identical images shifted apart would FAKE disparity on flat content, so a stale-capture
            // frame converges at 0 - the same choice mono gameplay makes.
            const int convPx = staleCapture
                ? 0
                : static_cast<int>(lroundf(conv * 0.5f * static_cast<float>(g_aerSwapW)));
            CopyTextureToEyeFullFrame(swap ? 1 : 0, leftSrc,  +convPx);
            CopyTextureToEyeFullFrame(swap ? 0 : 1, rightSrc, -convPx);

            // [POSETAG] Tag the captured pair with the pose that RENDERED it - the smoothed head-look
            // armed ~poseTagDelayFrames presents ago - not the pose located this instant. The
            // compositor then reprojects from the true render pose, so the world stops dragging behind
            // the head on turns. Delay 0 falls back to roughly the current smoothed pose.
            XrPosef headPose = g_delayedTagPose;
            const bool lockedCine = ME2VR::CalcViewHook::GetVrCineActive() && !ME2VR::CalcViewHook::GetCineVrHeadTracking();
            // [VRCINE] head-tracking OFF: render was parked (no head rotation), so anchor the pair at
            // the CURRENT head each frame -> a head-locked 3D picture that follows you, no drift.
            if (lockedCine)
            {
                headPose.orientation = RemoveRoll(views[0].pose.orientation);
                headPose.position.x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
                headPose.position.y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
                headPose.position.z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;
            }

            // [FREEZETAG 2026-08-19, ported from ME2] If the engine did not produce a NEW pass-0/pass-1
            // pair this present, the eye textures still hold the previous frame's pixels. Tagging those
            // same pixels with a freshly located pose tells the compositor the old image belongs to the
            // new head position, so its reprojection "corrects" an image that was never wrong - and the
            // world jerks. Hold the pose AND the declared FOV with the pixels instead, and let the
            // runtime reproject the stale pair honestly. This is the case ordinary frame drops hit
            // constantly once the render rate falls below the present rate (the apartment party, hub
            // crowds); [STALEEYE]'s age test above only catches a LONG stall, not a single dropped
            // frame. ME2 has had this since its SFR bring-up; ME3 never got it, which is why head
            // tracking here degrades with frame rate in a way ME1/ME2 do not.
            float ghUse = gh, gvUse = gv;
            {
                static uint64_t s_lastCapSeq = 0;
                static XrPosef  s_freshPose = {};
                static float    s_freshGh = 0.0f, s_freshGv = 0.0f;
                static bool     s_freshInit = false;
                const uint64_t capSeq = ME2VR::D3DCapture::GetSfrPass0CaptureSeq();
                const bool freshSfrPair = (capSeq != s_lastCapSeq);
                s_lastCapSeq = capSeq;
                // A head-locked cine deliberately re-anchors every present, and a stale-capture frame is
                // already showing both eyes the live backbuffer - neither wants the freeze.
                if (freshSfrPair || !s_freshInit || lockedCine || staleCapture)
                {
                    s_freshPose = headPose; s_freshGh = gh; s_freshGv = gv; s_freshInit = true;
                }
                else
                {
                    headPose = s_freshPose; ghUse = s_freshGh; gvUse = s_freshGv;
                    g_poseTagFreezes.fetch_add(1, std::memory_order_relaxed);   // [POSEHB]
                }
            }

            if (g_aerSwap[0] != nullptr && g_aerSwap[1] != nullptr)
            {
                for (int eye = 0; eye < 2; ++eye)
                {
                    XrFovf fov = views[eye].fov;
                    if (ghUse > 0.01f && gvUse > 0.01f) { fov.angleLeft = -ghUse; fov.angleRight = ghUse; fov.angleUp = gvUse; fov.angleDown = -gvUse; }
                    projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE;
                    projViews[eye].pose = headPose;
                    projViews[eye].fov = fov;
                    projViews[eye].subImage.swapchain = g_aerSwap[eye];
                    projViews[eye].subImage.imageRect.offset = { 0, 0 };
                    projViews[eye].subImage.imageRect.extent = { static_cast<int32_t>(g_aerSwapW), static_cast<int32_t>(g_aerSwapH) };
                    projViews[eye].subImage.imageArrayIndex = 0;
                }
                layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE;
                layer.space = g_localSpace;
                layer.viewCount = 2;
                layer.views = projViews;
                rendered = true;
                if (!g_submitLogged.exchange(true))
                    XLog("=== SFR: first same-frame stereo pair submitted (pass0=L pass1=R gameFovH=" +
                         std::to_string(gh) + ") ===");
                if (g_aerModeLogCount.fetch_add(1, std::memory_order_relaxed) < 4)
                    XLog("frame mode = SFR (same-frame) -> pass0->L pass1->R, one shared pose");
            }
        }
        if (backbuffer) backbuffer->Release();
    }
    else if (fs.shouldRender && dibrMode)
    {
        // DIBR: left eye = the real mono frame, right eye = the depth-warped synthesis of that SAME frame.
        // NOTE: this branch owns EVERY DIBR frame (not gated on depth-ready) so DIBR can never fall through to
        // the Stereo SBS split, which would mis-halve the mono frame (= the "squished/crossed" bug). Until depth
        // is ready GetDibrRightEye returns null -> both eyes get the backbuffer = clean MONO, never black/crossed.
        // Both eyes are from one moment -> ONE shared current pose (no staleness, no per-slot tag); the whole
        // stereo disparity lives in the warped pixels. Reuses the full-size AER eye swapchains + the full-frame
        // copy (never the aspect-fit copy, or the DIBR eye squashes vertically).
        static bool s_autoRecenteredDibr = false;
        if (!s_autoRecenteredDibr || g_recenterRequested.exchange(false, std::memory_order_relaxed))
        {
            RecenterAppSpace(fs.predictedDisplayTime);
            s_autoRecenteredDibr = true;
        }

        XrViewLocateInfo vli = {};
        vli.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
        vli.viewConfigurationType = kStereo;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = g_localSpace;
        XrViewState vs = {}; vs.type = XR_TYPE_VIEW_STATE_VALUE;
        uint32_t viewCount = 0;
        XrView views[2] = {}; views[0].type = views[1].type = XR_TYPE_VIEW_VALUE;
        const XrResult lr = g_fn.locateViews(g_session, &vli, &vs, 2, &viewCount, views);
        if (XrSucceeded(lr) && viewCount == 2) DriveHeadLook(views[0]);

        ID3D11Texture2D* backbuffer = nullptr;
        IDXGISwapChain* sc = ME2VR::D3DCapture::GetGameSwapChain();
        if (sc) sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer));

        if (XrSucceeded(lr) && viewCount == 2 && backbuffer && EnsureAerSwapchains())
        {
            // FOV-FILL: widen the game view to fill the headset (identical to AER/stereo). ApplyFov (calcview
            // DIBR branch) widens the render to this target + publishes it as the submit FOV below.
            ApplyFovFill(views);

            // Game's rendered (now widened) half-FOV -> declared per eye (must EXACTLY match what was rendered).
            const float gh = ME2VR::CalcViewHook::GetGameHalfFovH();
            const float gv = ME2VR::CalcViewHook::GetGameHalfFovV();

            // left = the real frame, right = the synth (null -> backbuffer, never black). No swap.
            ID3D11Texture2D* right = ME2VR::D3DCapture::GetDibrRightEye(backbuffer);
            CopyTextureToEyeFullFrame(0, backbuffer);
            CopyTextureToEyeFullFrame(1, right != nullptr ? right : backbuffer);

            // ONE shared current pose for both eyes (same-frame stereo; roll removed, centered position).
            XrPosef headPose = views[0].pose;
            headPose.orientation = RemoveRoll(views[0].pose.orientation);
            headPose.position.x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
            headPose.position.y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
            headPose.position.z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;

            for (int eye = 0; eye < 2; ++eye)
            {
                XrFovf fov = views[eye].fov;
                if (gh > 0.01f && gv > 0.01f) { fov.angleLeft = -gh; fov.angleRight = gh; fov.angleUp = gv; fov.angleDown = -gv; }
                projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE;
                projViews[eye].pose = headPose;
                projViews[eye].fov = fov;
                projViews[eye].subImage.swapchain = g_aerSwap[eye];
                projViews[eye].subImage.imageRect.offset = { 0, 0 };
                projViews[eye].subImage.imageRect.extent = { static_cast<int32_t>(g_aerSwapW), static_cast<int32_t>(g_aerSwapH) };
                projViews[eye].subImage.imageArrayIndex = 0;
            }
            layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE;
            layer.space = g_localSpace;
            layer.viewCount = 2;
            layer.views = projViews;
            rendered = true;
            // Stash this good pose/FOV so a later hiccup frame can hold it instead of going black.
            g_dibrLastHeadPose = headPose;
            g_dibrLastFovL = projViews[0].fov;
            g_dibrLastFovR = projViews[1].fov;
            g_dibrHaveLast = true;
            ++g_dibrFullFrames;
            if (g_aerModeLogCount.fetch_add(1, std::memory_order_relaxed) < 4) XLog("frame mode = DIBR (depth warp) -> left real, right synth");
        }
        else if (backbuffer && EnsureAerSwapchains() && g_dibrHaveLast)
        {
            // NEVER BLACK (ME1 parity): the located-stereo guard failed this frame (locateViews hiccup /
            // transient viewCount!=2). DIBR has no mono-quad fallback, so without this the layer count would
            // hit 0 -> endFrame with 0 layers -> a hard black frame on BOTH eyes = the "flickers badly" flash.
            // Hold the last good pose/FOV and refresh both eyes to the current backbuffer (mono).
            CopyTextureToEyeFullFrame(0, backbuffer);
            CopyTextureToEyeFullFrame(1, backbuffer);
            for (int eye = 0; eye < 2; ++eye)
            {
                projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE;
                projViews[eye].pose = g_dibrLastHeadPose;
                projViews[eye].fov = (eye == 0) ? g_dibrLastFovL : g_dibrLastFovR;
                projViews[eye].subImage.swapchain = g_aerSwap[eye];
                projViews[eye].subImage.imageRect.offset = { 0, 0 };
                projViews[eye].subImage.imageRect.extent = { static_cast<int32_t>(g_aerSwapW), static_cast<int32_t>(g_aerSwapH) };
                projViews[eye].subImage.imageArrayIndex = 0;
            }
            layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE;
            layer.space = g_localSpace;
            layer.viewCount = 2;
            layer.views = projViews;
            rendered = true;
            ++g_dibrFallbackFrames;
        }
        else
        {
            ++g_dibrBlackFrames;   // no backbuffer AND no prior good frame (true startup only) -> unavoidable
        }
        // [DIBRSUB] confirm-the-fix line: after the fix, black=0 in steady gameplay; any flicker left is NOT
        // the 0-layer path. fallback>0 = frames the mod rescued that ME1-parity would have blacked.
        {
            const ULONGLONG nowMs = GetTickCount64();
            if (g_dibrSubStatMs == 0) g_dibrSubStatMs = nowMs;
            if (nowMs - g_dibrSubStatMs >= 2000)
            {
                char line[160] = {};
                sprintf_s(line, "[DIBRSUB] full=%u fallback=%u black=%u (per ~2s; black must be 0)",
                          g_dibrFullFrames, g_dibrFallbackFrames, g_dibrBlackFrames);
                XLog(line);
                g_dibrSubStatMs = nowMs; g_dibrFullFrames = 0; g_dibrFallbackFrames = 0; g_dibrBlackFrames = 0;
            }
        }
        if (backbuffer) backbuffer->Release();
    }
    else if (fs.shouldRender)
    {
        // Recenter the app space (re-origin to the current head) once at startup or on request,
        // BEFORE locating - so the located head AND the submitted pose share the recentered frame.
        static bool s_autoRecentered = false;
        if (!s_autoRecentered || g_recenterRequested.exchange(false, std::memory_order_relaxed))
        {
            RecenterAppSpace(fs.predictedDisplayTime);
            s_autoRecentered = true;
        }

        XrViewLocateInfo vli = {};
        vli.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
        vli.viewConfigurationType = kStereo;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = g_localSpace;
        XrViewState vs = {}; vs.type = XR_TYPE_VIEW_STATE_VALUE;
        uint32_t viewCount = 0;
        XrView views[2] = {}; views[0].type = views[1].type = XR_TYPE_VIEW_VALUE;
        const XrResult lr = g_fn.locateViews(g_session, &vli, &vs, 2, &viewCount, views);

        // Drive free head-look from the located (recentered) head orientation.
        if (XrSucceeded(lr) && viewCount == 2) DriveHeadLook(views[0]);

        ID3D11Texture2D* backbuffer = nullptr;
        IDXGISwapChain* sc = ME2VR::D3DCapture::GetGameSwapChain();
        if (sc) sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer));

        if (XrSucceeded(lr) && viewCount == 2 && backbuffer)
        {
            // Game's rendered half-FOV (radians); fall back to the runtime's if not published yet.
            const float gh = ME2VR::CalcViewHook::GetGameHalfFovH();
            const float gv = ME2VR::CalcViewHook::GetGameHalfFovV();
            // Live FOV readout (compare ME2 wide vs ME3 narrow): hFOV/vFOV in degrees, plus headset's.
            {
                static unsigned s_fovLog = 0;
                if ((s_fovLog++ % 180) == 0)
                {
                    const float r2d = 57.29578f;
                    const float hh = (views[0].fov.angleRight - views[0].fov.angleLeft) * 0.5f * r2d;
                    const float hv = (views[0].fov.angleUp - views[0].fov.angleDown) * 0.5f * r2d;
                    XLog("[XRAPI] FOV game hHalf=" + std::to_string(gh * r2d) + " vHalf=" + std::to_string(gv * r2d) +
                         " (hFOV=" + std::to_string(2 * gh * r2d) + " vFOV=" + std::to_string(2 * gv * r2d) +
                         ")  headset hHalf=" + std::to_string(hh) + " vHalf=" + std::to_string(hv));
                }
            }
            // FOV-FILL: ME3 renders a narrow ~70deg hFOV. Target = fill the headset vertically, with the
            // horizontal computed from the eye-image aspect so there's no distortion. Pushed to the
            // render side (applied next frame); the submit below uses the matching (now widened) gh/gv.
            ApplyFovFill(views);

            for (int eye = 0; eye < 2; ++eye)
            {
                CopyHalfToEye(eye, backbuffer, (eye == 0) ? 0u : g_swapW);   // left half -> eye0, right half -> eye1

                XrFovf fov = views[eye].fov;
                if (gh > 0.01f && gv > 0.01f) { fov.angleLeft = -gh; fov.angleRight = gh; fov.angleUp = gv; fov.angleDown = -gv; }

                projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE;
                projViews[eye].pose = views[eye].pose;
                projViews[eye].pose.orientation = RemoveRoll(views[eye].pose.orientation);  // never tilt with head roll
                projViews[eye].fov = fov;
                projViews[eye].subImage.swapchain = g_swap[eye];
                projViews[eye].subImage.imageRect.offset = { 0, 0 };
                projViews[eye].subImage.imageRect.extent = { static_cast<int32_t>(g_swapW), static_cast<int32_t>(g_swapH) };
                projViews[eye].subImage.imageArrayIndex = 0;
            }
            layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE;
            layer.space = g_localSpace;
            layer.viewCount = 2;
            layer.views = projViews;
            rendered = true;
            if (!g_submitLogged.exchange(true))
                XLog("=== B2b: first STEREO projection submitted (gameFovH=" + std::to_string(gh) + " rad) ===");
            if (g_modeLogCount.fetch_add(1, std::memory_order_relaxed) < 4) XLog("frame mode = SBS (split ran) -> per-eye projection");
        }
        if (backbuffer) backbuffer->Release();
    }

    // Insert menu quad: the ImGui menu texture, head-locked, alpha-blended over the world.
    XrCompositionLayerQuad uiQuad = {};
    bool haveUi = false;
    ID3D11Texture2D* uiTex = ME2VR::Menu::RenderFrame();   // non-null only while the menu is open
    if (uiTex != nullptr)
    {
        D3D11_TEXTURE2D_DESC mdesc = {};
        uiTex->GetDesc(&mdesc);
        EnsureUiSwapchain(mdesc.Width, mdesc.Height);
        if (g_uiSwap != nullptr)
        {
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai = {}; ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
            if (XrSucceeded(g_fn.acquireSwapchainImage(g_uiSwap, &ai, &idx)))
            {
                XrSwapchainImageWaitInfo wi = {}; wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE; wi.timeout = XR_INFINITE_DURATION_VALUE;
                if (XrSucceeded(g_fn.waitSwapchainImage(g_uiSwap, &wi)) && idx < g_uiImages.size())
                    g_ctx->CopyResource(g_uiImages[idx], uiTex);
                XrSwapchainImageReleaseInfo ri = {}; ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
                g_fn.releaseSwapchainImage(g_uiSwap, &ri);

                // Fixed comfortable panel: 1.4 m wide at 1.5 m, correct menu aspect (no FOV stretch).
                const float dist = g_menuQuadDistanceM.load(std::memory_order_relaxed);
                const float width = g_menuQuadWidthM.load(std::memory_order_relaxed);
                const float aspect = (g_uiW != 0) ? static_cast<float>(g_uiH) / static_cast<float>(g_uiW) : 0.8f;
                uiQuad.type = XR_TYPE_COMPOSITION_LAYER_QUAD_VALUE;
                uiQuad.layerFlags = kLayerSrcAlpha;
                uiQuad.space = g_viewSpace;                      // head-locked
                uiQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH_VALUE;
                uiQuad.subImage.swapchain = g_uiSwap;
                uiQuad.subImage.imageRect.offset = { 0, 0 };
                uiQuad.subImage.imageRect.extent = { static_cast<int32_t>(g_uiW), static_cast<int32_t>(g_uiH) };
                uiQuad.subImage.imageArrayIndex = 0;
                uiQuad.pose = IdentityPose();
                uiQuad.pose.position.x = g_menuQuadOffXM.load(std::memory_order_relaxed);
                uiQuad.pose.position.y = g_menuQuadOffYM.load(std::memory_order_relaxed);
                uiQuad.pose.position.z = -dist;
                uiQuad.size.width = width;
                uiQuad.size.height = width * aspect;
                haveUi = true;
                if (!g_submitLogged.load(std::memory_order_acquire)) {}   // (first-stereo log already fired)
            }
        }
    }

    // Game-UI overlay (HUD + menus): one flat layer over the always-stereo world. Sized to the game's
    // FOV so HUD elements land where the game drew them; head-locked so it follows your view.
    XrCompositionLayerQuad gameUiQuad = {};
    bool haveGameUi = false;
    if (ME2VR::D3DCapture::GetUiOverlayMode() && ME2VR::D3DCapture::GetUiOverlayActive())
    {
        ID3D11Texture2D* ov = ME2VR::D3DCapture::GetUiOverlayTexture();
        if (ov != nullptr)
        {
            D3D11_TEXTURE2D_DESC od = {}; ov->GetDesc(&od);
            EnsureGameUiSwapchain(od.Width, od.Height);
            if (g_gameUiSwap != nullptr)
            {
                uint32_t gidx = 0;
                XrSwapchainImageAcquireInfo gai = {}; gai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
                if (XrSucceeded(g_fn.acquireSwapchainImage(g_gameUiSwap, &gai, &gidx)))
                {
                    XrSwapchainImageWaitInfo gwi = {}; gwi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE; gwi.timeout = XR_INFINITE_DURATION_VALUE;
                    if (XrSucceeded(g_fn.waitSwapchainImage(g_gameUiSwap, &gwi)) && gidx < g_gameUiImages.size())
                        g_ctx->CopyResource(g_gameUiImages[gidx], ov);
                    XrSwapchainImageReleaseInfo gri = {}; gri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
                    g_fn.releaseSwapchainImage(g_gameUiSwap, &gri);

                    float gh = ME2VR::CalcViewHook::GetGameHalfFovH();
                    float gv = ME2VR::CalcViewHook::GetGameHalfFovV();
                    if (gh < 0.05f) gh = 0.60f;
                    if (gv < 0.05f) gv = 0.45f;
                    const float gdist = 1.2f;
                    gameUiQuad.type = XR_TYPE_COMPOSITION_LAYER_QUAD_VALUE;
                    gameUiQuad.layerFlags = kLayerSrcAlpha;
                    // WORLD-LOCKED (g_localSpace, re-origined on recenter), NOT head-locked: the HUD
                    // stays put when you turn your head instead of swimming/following it. Recenter
                    // snaps it back in front of you.
                    gameUiQuad.space = g_localSpace;
                    gameUiQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH_VALUE;
                    gameUiQuad.subImage.swapchain = g_gameUiSwap;
                    gameUiQuad.subImage.imageRect.offset = { 0, 0 };
                    gameUiQuad.subImage.imageRect.extent = { static_cast<int32_t>(g_gameUiW), static_cast<int32_t>(g_gameUiH) };
                    gameUiQuad.subImage.imageArrayIndex = 0;
                    gameUiQuad.pose = IdentityPose();
                    gameUiQuad.pose.position.z = -gdist;
                    gameUiQuad.size.width = 2.0f * gdist * tanf(gh);
                    gameUiQuad.size.height = 2.0f * gdist * tanf(gv);
                    haveGameUi = true;
                }
            }
        }
    }

    // [LINKFOV] Runtime FOV reconciliation, applied to whichever mode filled projViews (SFR / AER /
    // DIBR / mono) - one place instead of four. Crop each eye's submitted rect to the intersection of
    // what the mod rendered (the declared window D) and the runtime's own per-eye frustum T, and declare
    // exactly that intersection. Meta's compositor assumes T regardless of what the mod declares, so once
    // declared == T-window == the submitted pixels, its assumption and the mod's agree and the doubling
    // goes away. SteamVR outright voids a view declared wider than T, and the same crop fixes it.
    // Pure rect math in tan space: no extra rendering, and "declared FOV must exactly match what was
    // rendered" stays true because the RECT shrinks along with the angles. When D is already narrower
    // than T (menus, flat cine at game FOV) the intersection IS D and this is a no-op. Any other
    // runtime (VDXR etc.) never enters here - path bit-identical to before.
    if (rendered && ((g_isOculusRuntime && g_questFovMatch.load(std::memory_order_relaxed)) || g_isSteamVrRuntime))
    {
        XrView rtViews[2] = {};
        rtViews[0].type = XR_TYPE_VIEW_VALUE; rtViews[1].type = XR_TYPE_VIEW_VALUE;
        XrViewState rvs = {}; rvs.type = XR_TYPE_VIEW_STATE_VALUE;
        XrViewLocateInfo rli = {};
        rli.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
        rli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE;
        rli.displayTime = fs.predictedDisplayTime;
        rli.space = g_localSpace;
        uint32_t rc = 0;
        if (XrSucceeded(g_fn.locateViews(g_session, &rli, &rvs, 2, &rc, rtViews)) && rc >= 2)
        {
            g_lastRtFov[0] = rtViews[0].fov; g_lastRtFov[1] = rtViews[1].fov; g_lastRtFovValid = true;
            for (int e = 0; e < 2; ++e)
            {
                const XrFovf d = projViews[e].fov;      // what the mod declared/rendered
                const XrFovf& t = rtViews[e].fov;       // the runtime's own frustum
                const float aL = (d.angleLeft  > t.angleLeft)  ? d.angleLeft  : t.angleLeft;
                const float aR = (d.angleRight < t.angleRight) ? d.angleRight : t.angleRight;
                const float aU = (d.angleUp    < t.angleUp)    ? d.angleUp    : t.angleUp;
                const float aD = (d.angleDown  > t.angleDown)  ? d.angleDown  : t.angleDown;
                const float tanDL = tanf(d.angleLeft), tanDR = tanf(d.angleRight);
                const float tanDU = tanf(d.angleUp),   tanDD = tanf(d.angleDown);
                const float hSpan = tanDR - tanDL, vSpan = tanDU - tanDD;
                if (aR - aL <= 0.05f || aU - aD <= 0.05f || hSpan <= 1e-4f || vSpan <= 1e-4f) continue;
                const int32_t w = projViews[e].subImage.imageRect.extent.width;
                const int32_t h = projViews[e].subImage.imageRect.extent.height;
                auto clampI = [](int32_t v, int32_t lo, int32_t hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); };
                const int32_t x0 = clampI(static_cast<int32_t>(lroundf((tanf(aL) - tanDL) / hSpan * w)), 0, w - 1);
                const int32_t x1 = clampI(static_cast<int32_t>(lroundf((tanf(aR) - tanDL) / hSpan * w)), x0 + 1, w);
                const int32_t y0 = clampI(static_cast<int32_t>(lroundf((tanDU - tanf(aU)) / vSpan * h)), 0, h - 1);
                const int32_t y1 = clampI(static_cast<int32_t>(lroundf((tanDU - tanf(aD)) / vSpan * h)), y0 + 1, h);
                projViews[e].subImage.imageRect.offset.x += x0;
                projViews[e].subImage.imageRect.offset.y += y0;
                projViews[e].subImage.imageRect.extent.width  = x1 - x0;
                projViews[e].subImage.imageRect.extent.height = y1 - y0;
                projViews[e].fov.angleLeft = aL; projViews[e].fov.angleRight = aR;
                projViews[e].fov.angleUp = aU;   projViews[e].fov.angleDown = aD;
            }
        }
    }

    // [EYETAG2] telemetry (pure logging, all runtimes): declared submit FOV per eye AFTER any crop,
    // plus the rendering views' located FOV - 1 line/sec, 20 lines/session. ME1's [EYETAG] equivalent.
    if (rendered)
    {
        static ULONGLONG s_etLastMs = 0; static int s_etCount = 0;
        const ULONGLONG now = GetTickCount64();
        if (s_etCount < 20 && now - s_etLastMs > 1000)
        {
            s_etLastMs = now; ++s_etCount;
            char b[352];
            std::snprintf(b, sizeof(b),
                          "[EYETAG2] decl L(%.3f,%.3f,%.3f,%.3f) R(%.3f,%.3f,%.3f,%.3f) "
                          "rt L(%.3f,%.3f,%.3f,%.3f) R(%.3f,%.3f,%.3f,%.3f) rtValid=%d rectL=%ld,%ld %ldx%ld",
                          projViews[0].fov.angleLeft, projViews[0].fov.angleRight, projViews[0].fov.angleUp, projViews[0].fov.angleDown,
                          projViews[1].fov.angleLeft, projViews[1].fov.angleRight, projViews[1].fov.angleUp, projViews[1].fov.angleDown,
                          g_lastRtFov[0].angleLeft, g_lastRtFov[0].angleRight, g_lastRtFov[0].angleUp, g_lastRtFov[0].angleDown,
                          g_lastRtFov[1].angleLeft, g_lastRtFov[1].angleRight, g_lastRtFov[1].angleUp, g_lastRtFov[1].angleDown,
                          g_lastRtFovValid ? 1 : 0,
                          static_cast<long>(projViews[0].subImage.imageRect.offset.x),
                          static_cast<long>(projViews[0].subImage.imageRect.offset.y),
                          static_cast<long>(projViews[0].subImage.imageRect.extent.width),
                          static_cast<long>(projViews[0].subImage.imageRect.extent.height));
            XLog(b);
        }
    }

    // [VRHEALTH] If the stereo path stops producing a pair the picture goes flat or freezes and the
    // user just sees "VR vanished" with nothing in the log to say why (Palaven shuttle, 2026-07-28).
    // Deliberately NOT behind a diagnostic tag: a mode silently degrading is an event, not a trace.
    // Edge-triggered both ways, so a one-frame hiccup during a load is not reported.
    {
        static int  s_noPair = 0;
        static bool s_warned = false;
        if (!rendered && !haveMono) { if (s_noPair < 10000) ++s_noPair; }   // mono cine counts as presented
        else
        {
            if (s_warned) ME2VR::Log::Line("Stereo recovered.");
            s_noPair = 0; s_warned = false;
        }
        if (!s_warned && s_noPair == 120)
        {
            s_warned = true;
            ME2VR::Log::Line("Stereo stopped producing a frame - the view will look flat or frozen. "
                             "Switching render mode and back usually restarts it.");
        }
    }
    const XrCompositionLayerBaseHeader* layers[4] = {};
    uint32_t layerCount = 0;
    if (rendered)   layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer);
    if (haveMono)   layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&monoQuad);
    if (haveGameUi) layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&gameUiQuad);
    if (haveUi)     layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&uiQuad);

    XrFrameEndInfo fei = {};
    fei.type = XR_TYPE_FRAME_END_INFO_VALUE;
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE_VALUE;
    fei.layerCount = layerCount;
    fei.layers = (layerCount > 0) ? layers : nullptr;
    g_fn.endFrame(g_session, &fei);
}
}

namespace ME2VR::Me2Xr
{
// [LINKFOV] Quest Link image fix. No-op on runtimes that do not need it.
// [COMFORT] panel placement (ME2 parity).
float GetMenuScreenDist() noexcept { return g_monoQuadDistanceM.load(std::memory_order_relaxed); }
void  SetMenuScreenDist(float m) noexcept { g_monoQuadDistanceM.store((m < 0.4f) ? 0.4f : (m > 6.0f ? 6.0f : m), std::memory_order_relaxed); }
float GetMenuScreenSize() noexcept { return g_monoQuadWidthM.load(std::memory_order_relaxed); }
void  SetMenuScreenSize(float m) noexcept { g_monoQuadWidthM.store((m < 0.8f) ? 0.8f : (m > 6.0f ? 6.0f : m), std::memory_order_relaxed); }
float GetMenuPanelDist() noexcept { return g_menuQuadDistanceM.load(std::memory_order_relaxed); }
void  SetMenuPanelDist(float m) noexcept { g_menuQuadDistanceM.store((m < 0.8f) ? 0.8f : (m > 4.0f ? 4.0f : m), std::memory_order_relaxed); }
float GetMenuPanelSize() noexcept { return g_menuQuadWidthM.load(std::memory_order_relaxed); }
void  SetMenuPanelSize(float m) noexcept { g_menuQuadWidthM.store((m < 0.5f) ? 0.5f : (m > 3.5f ? 3.5f : m), std::memory_order_relaxed); }
float GetMenuPanelOffX() noexcept { return g_menuQuadOffXM.load(std::memory_order_relaxed); }
void  SetMenuPanelOffX(float m) noexcept { g_menuQuadOffXM.store((m < -1.5f) ? -1.5f : (m > 1.5f ? 1.5f : m), std::memory_order_relaxed); }
float GetMenuPanelOffY() noexcept { return g_menuQuadOffYM.load(std::memory_order_relaxed); }
void  SetMenuPanelOffY(float m) noexcept { g_menuQuadOffYM.store((m < -1.0f) ? -1.0f : (m > 1.0f ? 1.0f : m), std::memory_order_relaxed); }
// [VRFILL] headset fill controls (ME2 parity).
bool GetVrFovFill() noexcept { return g_vrFovFillEnabled.load(std::memory_order_relaxed); }
void SetVrFovFill(bool on) noexcept { g_vrFovFillEnabled.store(on, std::memory_order_relaxed); }
float GetVrFillH() noexcept { return g_vrFillH.load(std::memory_order_relaxed); }
void  SetVrFillH(float v) noexcept { g_vrFillH.store((v < 0.5f) ? 0.5f : (v > 1.25f ? 1.25f : v), std::memory_order_relaxed); }
float GetVrFillV() noexcept { return g_vrFillV.load(std::memory_order_relaxed); }
void  SetVrFillV(float v) noexcept { g_vrFillV.store((v < 0.5f) ? 0.5f : (v > 1.25f ? 1.25f : v), std::memory_order_relaxed); }
void SetQuestFovMatch(bool on) noexcept { g_questFovMatch.store(on, std::memory_order_relaxed); }
bool GetQuestFovMatch() noexcept { return g_questFovMatch.load(std::memory_order_relaxed); }
bool IsOculusRuntime() noexcept { return g_isOculusRuntime; }
void Recenter() noexcept { g_recenterRequested.store(true, std::memory_order_relaxed); }
unsigned LastWaitFrameUs() noexcept { return g_lastWaitFrameUs.exchange(0, std::memory_order_relaxed); }
// [XRSUBMIT] windowed RunFrame stats, read and reset once per [FRAMETIME] window.
void TakeXrSubmitStats(unsigned* count, unsigned* sumUs, unsigned* maxUs) noexcept
{
    if (count) *count = g_xrSubmitCount.exchange(0, std::memory_order_relaxed);
    if (sumUs) *sumUs = static_cast<unsigned>(g_xrSubmitSumUs.exchange(0, std::memory_order_relaxed));
    if (maxUs) *maxUs = g_xrSubmitMaxUs.exchange(0, std::memory_order_relaxed);
}
// [POSEHB] windowed pose-path execution counters, read and reset once per [FRAMETIME] window.
void TakePoseStats(unsigned* pushes, unsigned* seeds, unsigned* freezes) noexcept
{
    if (pushes) *pushes = g_poseTagPushes.exchange(0, std::memory_order_relaxed);
    if (seeds) *seeds = g_poseTagSeeds.exchange(0, std::memory_order_relaxed);
    if (freezes) *freezes = g_poseTagFreezes.exchange(0, std::memory_order_relaxed);
}
float GetCineScreenZoom() noexcept { return g_cineScreenZoom.load(std::memory_order_relaxed); }
void  SetCineScreenZoom(float z) noexcept { g_cineScreenZoom.store((z < 0.5f) ? 0.5f : (z > 3.0f ? 3.0f : z), std::memory_order_relaxed); }

void Tick() noexcept
{
    // [SFR2] discriminator for the flat-stereo bug: calcViews should be ~2x replays (both passes
    // build a view). calcViews == replays means the replay pass never re-enters CalcSceneView, so
    // both eyes render pass 0's camera - identical images, separation just shifts the world.
    if (ME2VR::Log::DiagnosticsOn())
    {
        static ULONGLONG s_last = 0; static uint64_t s_cv = 0, s_rp = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - s_last >= 2000)
        {
            s_last = now;
            const uint64_t cv = ME2VR::CalcViewHook::GetSfrCalcViews();
            const uint64_t rp = ME2VR::CalcViewHook::GetSfrReplays();
            if (cv != s_cv || rp != s_rp)
                ME2VR::Log::Line("[SFR2] calcViews+" + std::to_string(cv - s_cv) +
                                 " replays+" + std::to_string(rp - s_rp) + " per ~2s");
            s_cv = cv; s_rp = rp;
        }
    }
    // LE3 discovery probes. Read-only, and self-gated on Diagnostics=1, so they cost nothing in a
    // normal session. See engine_probe.cpp for what each is hunting.
    ME2VR::EngineProbe::AimProbeTick();
    ME2VR::EngineProbe::ModeProbeTick();
    ME2VR::EngineProbe::CamProbeTick();
    ME2VR::EngineProbe::GuiProbeStart();   // [GUICEN] menu-name discovery census (own thread)
    if (!ME2VR::CalcViewHook::GetVrEnabled()) return;   // VR off (flat FP dev) -> no XR bring-up/submit
    // One-time bring-up once the game device is captured.
    if (!g_tried.load(std::memory_order_acquire))
    {
        ID3D11Device* device = ME2VR::D3DCapture::GetGameDevice();
        if (device == nullptr) return;
        bool expected = false;
        if (!g_tried.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
        // [BUILD] compile stamp, logged once per session at bring-up. A session on 2026-08-19 showed
        // a binary verifiably containing new code producing none of its log lines - so the running
        // build's identity gets asserted in the log itself, not inferred from file hashes on disk.
        XLog(std::string("[BUILD] me2_xr compiled ") + __DATE__ + " " + __TIME__ +
             " (POSETAG frame-indexed rev, FREEZETAG, POSEHB, AERFULL)");
        XLog("A2 bring-up starting");
        BringUp(device);
        return;
    }

    if (!g_initOk) return;
    PumpEvents();
    if (g_begun)
    {
        // [XRSUBMIT] Total wall time of RunFrame - which starts with xrWaitFrame, then does pacing,
        // per-eye texture copies, the swapchain acquire/wait/release cycle and xrEndFrame (the actual
        // handoff to the runtime's compositor). The consumer subtracts LastWaitFrameUs from this, the
        // same way it already subtracts it from "mod", so what's left is everything AFTER the wait -
        // a stall there that isn't per-draw hook cost (HOOKCOST) or a presentation edge (XSTATE)
        // points at the runtime/compositor itself - e.g. Virtual Desktop's encoder or network link
        // stalling on a visually busy frame - not the mod's code.
        LARGE_INTEGER rf0; QueryPerformanceCounter(&rf0);
        RunFrame();
        LARGE_INTEGER rf1; QueryPerformanceCounter(&rf1);
        if (g_paceQpcFreq.QuadPart != 0)
        {
            const uint32_t us = static_cast<uint32_t>((rf1.QuadPart - rf0.QuadPart) * 1000000ll / g_paceQpcFreq.QuadPart);
            g_xrSubmitCount.fetch_add(1, std::memory_order_relaxed);
            g_xrSubmitSumUs.fetch_add(us, std::memory_order_relaxed);
            uint32_t m = g_xrSubmitMaxUs.load(std::memory_order_relaxed);
            while (us > m && !g_xrSubmitMaxUs.compare_exchange_weak(m, us, std::memory_order_relaxed)) {}
        }
    }
}
}
