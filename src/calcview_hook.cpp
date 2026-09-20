#include "calcview_hook.h"

#include "convo_fp.h"

#include "d3d_capture.h"
#include "engine_probe.h"
#include "logger.h"
#include "me2_xr.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include <MinHook.h>

namespace
{
constexpr std::uintptr_t kCalcSceneViewRva = 0x6CFFD0;   // ME3: confirmed by the finder (p1Calls=7/8), 2026-06-27

// FSceneView offsets (LE1/LE3 identical per the ME3Tweaks SDK; confirming live here).
constexpr std::uintptr_t kFsvViewMatrix = 0x90;
constexpr std::uintptr_t kFsvProjectionMatrix = 0xD0;
constexpr std::uintptr_t kFsvTranslatedViewMatrix = 0x190;
constexpr std::uintptr_t kFsvTranslatedViewProjMatrix = 0x1D0;
constexpr std::uintptr_t kFsvPreViewTranslation = 0x250;
constexpr std::uintptr_t kFsvViewProjectionMatrix = 0x260;
constexpr std::uintptr_t kFsvViewOrigin = 0x320;
constexpr std::uintptr_t kFsvState = 0x8;        // FSceneView::State (FSceneViewStateInterface*)
constexpr std::uintptr_t kFsvFloatRect = 0x64;   // FSceneView X/Y/SizeX/SizeY (4 floats)

// B2b stereo separation. 50 uu = 1 m (LE1); ~1.6 uu half-eye ~ human IPD => ~1:1 scale.
// Runtime-adjustable from the Insert menu.
std::atomic<float> g_halfEyeUU{1.6f};
std::atomic_bool g_swapEyes{false};
std::atomic_bool g_stereoFramePacing{true};   // display-locked pacing (60fps->120Hz judder fix)
std::atomic<int> g_stereoFramePacingHz{0};    // 0 = auto-learn headset Hz
std::atomic<float> g_gameHalfFovH{0.0f};
std::atomic<float> g_gameHalfFovV{0.0f};
// FOV-fill: ME3 renders a narrow ~70deg hFOV; widen each eye's projection to fill the headset (target
// half-FOV pushed from the XR side). Submitted FOV is set to match so there's no distortion.
std::atomic_bool g_fovFillOn{false};
std::atomic<float> g_fovFillH{0.0f};   // target half-FOV horizontal (radians)
std::atomic<float> g_fovFillV{0.0f};   // target half-FOV vertical (radians)
// [FILLSRC] source-side FOV pre-write (ME2's glass/refraction-smear fix, ported 2026-07-30).
// Bisection traced the smear to the FOV fill in ME2: WidenProjection edits proj AFTER the engine
// has built the view, so distortion-class passes keep screen-space constants derived for the
// narrow FOV while the scene renders wide - the ghost rides the screen. The fix writes the fill
// FOV into the CAMERA before the build (everything derives consistently), restores it on return,
// and substitutes the true camera FOV into the published raw so cine detection stays intact.
std::atomic_bool   g_srcRawValid{false};
std::atomic<float> g_srcRawFovH{0.0f};     // camera's true half-FOV read at the source
std::atomic<float> g_srcWrittenFovH{0.0f}; // half-FOV the mod asked the camera for
std::atomic<float> g_srcBuiltFovH{0.0f};   // what the engine actually built (proof telemetry)
std::atomic<float> g_gameRawFovH{0.0f};   // RAW game half-FOV (never widened) -> cinematic detection
std::atomic<float> g_gameRawFovV{0.0f};
// [MARKERVR] The compact FSFXGUISceneView is built before the mod's late head/FOV edits. Retain the raw
// projection that produced it so the exact marker native hook can convert only its call-local VP
// into the rendered clip space. Atomic elements + sequence give a coherent cross-thread snapshot.
std::atomic<float> g_markerRawProjection[16] = {};
std::atomic<unsigned> g_markerProjectionSeq{0};
std::atomic_bool g_markerProjectionReady{false};
// Conversation/cutscene = narrow cinematic FOV -> render MONO (flat panel) instead of stereo split.
std::atomic_bool g_cinematic{false};

// 6DOF free head-look (orientation): yaw/pitch in UE units (65536 = 360 deg), set from the XR side.
std::atomic_bool g_headLookEnabled{false};
std::atomic<int32_t> g_headYawUU{0};
std::atomic<int32_t> g_headPitchUU{0};
// Positional 6DOF (lean): head translation from recenter in XR meters, fed from DriveHeadLook.
// Applied to the camera origin scaled to the same world-scale as the stereo. Toggle = g_headPosOn.
std::atomic_bool g_headPosOn{true};
std::atomic<float> g_headPosScale{1.0f};   // ME1 leanGain (1.0 = true 1:1; higher = more dramatic)
std::atomic<float> g_headPosX{0.0f};
std::atomic<float> g_headPosY{0.0f};
std::atomic<float> g_headPosZ{0.0f};
std::atomic_bool g_leanInvertFwd{false};   // [LEANFWD] one-click forward-sign fix (see ApplyHeadPosition)
// ULocalPlayer viewport rect (validated live).
constexpr std::uintptr_t kLpOrigin = 0x59C;      // FVector2D (OriginX, OriginY)
constexpr std::uintptr_t kLpSize = 0x5A4;        // FVector2D (SizeX, SizeY)

constexpr std::uintptr_t kAllocateViewStateRva = 0x3440F0;   // ME3: found via ViewState(+0x46C) xref 2026-06-27

using CalcFn = void*(__fastcall*)(void*, void*, void*, void*, void*, void*);
CalcFn g_orig = nullptr;
using AllocViewStateFn = void*(__fastcall*)(uint32_t);   // LE1 passes 0
AllocViewStateFn g_allocViewState = nullptr;
void* g_eyeState = nullptr;                       // standalone FSceneViewState for the right eye
void* g_eyeStateLeft = nullptr;                   // standalone FSceneViewState for the LEFT eye (temporal-ghost fix)
std::atomic_bool g_eyeStateLogged{false};
std::atomic_bool g_eyeStateLeftLogged{false};

std::atomic_bool g_started{false};
std::atomic_bool g_installed{false};
std::atomic<int> g_logged{0};
std::atomic<int> g_splitLogged{0};
std::atomic_bool g_stereo{true};                 // Milestone B: P1-layout same-frame split (superseded by g_vrMode)
// Master VR switch. Defaulted OFF during first-person development, which meant the game booted FLAT
// every launch and needed an F4 press to enter VR. ME2 has been on-by-default and persisted for a
// while; match it. F4 still toggles, and the choice now survives a restart via VR/Enabled.
std::atomic_bool g_vrEnabled{true};
std::atomic<uint64_t> g_splitSeq{0};             // bumped each time the split (or AER single view) runs
// --- VR mode selector (Mono/Stereo/AER/DIBR) - ported from ME2 2026-07-12 (AER+DIBR port). ---
enum class VrMode : int { Mono = 0, Stereo = 1, Aer = 2, Dibr = 3, Sfr = 4 };
std::atomic<int> g_vrMode{ (int)VrMode::Sfr };   // default SFR - the same-frame fix (both eyes get bloom/FX)

// --- SFR (Same-Frame stereo), ported from ME2 2026-07-22 (ME2 got it from ME1). Render the whole
// frame TWICE per present, one full PRIMARY render per eye, so bloom/lighting/full-screen FX are
// correct in BOTH eyes. This is the fix for ME3's confirmed SBS bugs: the right eye losing bloom, and
// the blue full-screen "space line" appearing ONLY in the right eye - both are per-view passes running
// inconsistently across a primary+secondary pair. With SFR there is no secondary view, so every pass
// runs fully for each eye by construction.
//
// The double render comes from an FViewportClient::Draw replay (DrawDetour below): pass 0 = the game's
// own Draw (-halfEye), pass 1 = the replay (+halfEye). t_replay distinguishes them; it is thread_local
// because CalcSceneView runs synchronously inside Draw on the SAME game thread.
constexpr std::uintptr_t kDrawRva = 0x6D1E30;    // ME3 FViewportClient::Draw
using DrawFn = void(__fastcall*)(void*, void*, void*);   // Draw(FViewport*, FCanvas*) - vtable slot 2
DrawFn g_origDraw = nullptr;
thread_local bool t_replay = false;              // true = the pass-1 (replay) render
std::atomic<uint64_t> g_sfrReplays{0};           // replay Draws fired (log throttle)
std::atomic<uint64_t> g_sfrCalcViews{0};         // [SFR2] SFR-branch CalcSceneView builds
std::atomic<uint64_t> g_p1CalcSeq{0};            // [ENGCINE] EVERY P1 CalcSceneView call, any branch
std::atomic<uint64_t> g_sfrReplayFaults{0};      // SEH-caught faults in the replay Draw
// [SFR] convergence: an opposite per-eye horizontal shift of each finished eye IMAGE at submit, which
// pulls the zero-disparity (fusion) plane in from infinity so distant isolated elements fuse without
// lowering the IPD. Applied at SUBMIT, never at render: a per-pass projection shift moves the world
// but splits the HUD by 2*conv, because the HUD is composited identically into both passes.
// Default 0.03 = the value ME1 ships baked in. Starting at 0 makes the control look broken: distant
// prompts stay unfused until you happen to find the slider.
std::atomic<float> g_sfrConvergence{0.03f};
// [SFR-UI] HUD trims on top of the automatic [UIRATIO] projection match below.
std::atomic<float> g_sfrUiScaleX{1.0f};
std::atomic<float> g_sfrUiScaleY{1.0f};
std::atomic<float> g_sfrUiOffX{0.0f};            // fraction of viewport width  (-0.5..0.5)
std::atomic<float> g_sfrUiOffY{0.0f};            // fraction of viewport height (-0.5..0.5)
// [POSETAG] head-tracking smoothness. Smoothing is the adaptive low-pass strength when the head is
// nearly still; the tag delay says how many presents back the pose that actually rendered the current
// backbuffer was armed. The delay is MACHINE-SPECIFIC (GPU + queue depth) and must be dialled in the
// headset: raise it until the world stops dragging WITH the head, lower it if it leads ahead.
std::atomic_bool  g_headLookUserEnabled{true};
std::atomic<float> g_headLookSmoothing{0.4f};
std::atomic<float> g_lookSensitivity{1.0f};
std::atomic_bool  g_invertLookYaw{false};
std::atomic_bool  g_invertLookPitch{false};
std::atomic<float> g_poseTagDelayFrames{2.0f};
// [FILL] What this view ACTUALLY rendered (ME2 parity). The submitted FOV is the headset target; the
// render FOV is that target scaled by the game's zoom, so the UI ratio needs the render value.
std::atomic<float> g_renderHalfFovH{0.0f};
std::atomic<float> g_renderHalfFovV{0.0f};
std::atomic<float> g_restHalfFov{0.61f};   // gameplay "rest" half-FOV; ADS narrows below it
std::atomic<float> g_restHalfFovV{0.370f}; // matching 16:9 vertical half-FOV until gameplay is observed
// --- AER (alternate-eye rendering) config, ported from ME2 (which ported it from ME1). ---
std::atomic<float> g_aerHalfEyeUU{3.4f};    // IPD/scale (baked 2026-08-21, tuned value)
// [AERSHAKE bake] FALSE now. TRUE only ever compensated the single-slot stamp's constant pipeline
// mislabel (label one build ahead of pixels = a fixed eye swap). The FIFO stamp removed the
// mislabel, so the compensation would now INVERT depth. Confirmed in an ME2 headset test.
std::atomic_bool   g_aerSwapEyes{false};
std::atomic_bool   g_aerFramePacing{true};  // display-locked pacing (the flicker fix)
std::atomic<int>   g_aerFramePacingHz{0};   // 0 = auto-learn headset Hz
// --- DIBR (depth-image-based rendering) live config, ported from ME2. Pushed to the d3d_capture warp
// shader each frame via SetDibrWarp. ---
std::atomic<float> g_depthWarpGain{1.20f};   // depth strength (gentle default, ME2 parity)
std::atomic<float> g_depthWarpConv{0.985f};  // manual convergence plane (used when auto off)
std::atomic_bool   g_depthWarpFlip{true};    // LE is FORWARD-Z (clears depth to 1.0) - ME2-validated default
std::atomic_bool   g_dibrAutoConverge{true}; // track subject depth for convergence
// --- AER render<->present eye handshake (drift-free; NOT present-count parity, which drifts). ---
// BYTE-IDENTICAL to ME1/ME2: a DEDICATED seq (not the shared g_splitSeq), an ARMED flag (so a stale eye
// from a prior AER session can't read as valid), and a seq-FIRST acquire read in GetAerStamp.
std::atomic<int>      g_aerRenderEye{0};    // legacy arm slot (no longer read by the detour - see [AERSHAKE])
std::atomic<int>      g_aerStampEye{-1};    // eye the detour actually built this frame (-1 = none yet)
std::atomic<uint64_t> g_aerStampSeq{0};     // dedicated AER stamp seq - NOT g_splitSeq
std::atomic_bool      g_aerStampArmed{false}; // set once the detour has stamped a real AER eye
// [AERSHAKE 2026-08-21, ported from ME2 after field confirm] The single-slot stamp was the full-rate
// AER shake: the stamp is written at BUILD time (game thread) but the pixels land at PRESENT one
// frame later (UE3 pipelines), so latest-read labels the on-screen frame with the NEXT build's eye
// whenever the game thread runs ahead. At display/2 the mislabel was constant (a fixed eye swap -
// why AerSwapEyes defaulted TRUE); at full rate the pipeline depth flaps and the world visibly
// vibrates between the two eye positions. Fix: the detour derives the eye from its own seq parity
// (L,R,L,R by construction) and queues {seq, eye}; the present consumes OLDEST-FIRST, one per
// present, matching swapchain delivery order. Label can no longer disagree with pixels.
constexpr int kAerRingN = 8;
struct AerRingSlot { std::atomic<uint64_t> seq{0}; std::atomic<int> eye{0}; };
AerRingSlot g_aerRing[kAerRingN];
std::uintptr_t g_base = 0;

// Write the ULocalPlayer viewport rect (Origin/Size) the engine reads in CalcSceneView. SEH-guarded.
void WriteRect(std::uintptr_t lp, float ox, float oy, float sx, float sy) noexcept
{
    __try
    {
        volatile float* o = reinterpret_cast<volatile float*>(lp + kLpOrigin);
        volatile float* s = reinterpret_cast<volatile float*>(lp + kLpSize);
        o[0] = ox; o[1] = oy; s[0] = sx; s[1] = sy;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SetViewState(std::uintptr_t sv, void* st) noexcept
{
    __try { *reinterpret_cast<void* volatile*>(sv + kFsvState) = st; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void* CallAllocViewState() noexcept
{
    __try { return g_allocViewState ? g_allocViewState(0) : nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool Readable(void* p, size_t n) noexcept
{
    return p != nullptr && !IsBadReadPtr(p, n);
}

// Move this view's camera by signedUU along the render-camera RIGHT axis (ViewMatrix col0),
// editing PreViewTranslation + ViewOrigin. A real camera move -> real binocular parallax (AER method).
void Mul4x4(float* out, const float* a, const float* b) noexcept;   // defined below
void EnsureConvoFpOrigin(std::uintptr_t sv) noexcept;

void ApplyEyeOffset(std::uintptr_t sv, float signedUU) noexcept
{
    EnsureConvoFpOrigin(sv);
    __try
    {
        auto* view = reinterpret_cast<float*>(sv + kFsvViewMatrix);
        const float rx = view[0], ry = view[4], rz = view[8];   // world-space right = column 0
        const float wx = signedUU * rx, wy = signedUU * ry, wz = signedUU * rz;
        volatile float* pvt = reinterpret_cast<volatile float*>(sv + kFsvPreViewTranslation);
        volatile float* vo = reinterpret_cast<volatile float*>(sv + kFsvViewOrigin);
        pvt[0] -= wx; pvt[1] -= wy; pvt[2] -= wz;
        vo[0] += wx;  vo[1] += wy;  vo[2] += wz;
        // [EYEVPM] Bake the shift into ViewMatrix too (translation row = -C.basis; the shift is along
        // camera-right so only the right-dot changes) and rebuild the NON-translated view-projection.
        // The world renders through TranslatedViewMatrix+PreViewTranslation (shifted above), but
        // screen-projected sprites (selection markers, lens flares, light shafts) go through
        // ViewMatrix/ViewProjectionMatrix - without this they project from the HEAD-CENTER camera in
        // BOTH SFR passes, giving a fixed-disparity layer that reads as "two of each, fusing only at
        // one distance". One float does it: VM[12] -= signedUU.
        view[12] -= signedUU;
        const auto* proj = reinterpret_cast<const float*>(sv + kFsvProjectionMatrix);
        const auto* tview = reinterpret_cast<const float*>(sv + kFsvTranslatedViewMatrix);
        auto* vproj = reinterpret_cast<float*>(sv + kFsvViewProjectionMatrix);
        auto* tvproj = reinterpret_cast<float*>(sv + kFsvTranslatedViewProjMatrix);
        Mul4x4(vproj, view, proj);
        Mul4x4(tvproj, tview, proj);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void Mul4x4(float* out, const float* a, const float* b) noexcept
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            float v = 0.0f;
            for (int k = 0; k < 4; ++k) v += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = v;
        }
}

// ---- [INVMAT] stale-inverse-matrix fix, ported from LE1 via LE2 (2026-07-12; solved LE1's head-locked
// dark panel). The eye-offset/head-look/FOV edits below rebuild only the FORWARD matrix products;
// FSceneView also caches INVERSE matrices (LE1: inv(TVPM)@0x210, inv(PM)@0x2A0, inv(VPM)@0x2E0) that keep
// describing the pre-edit camera. Height fog / light shafts reconstruct world from screen+depth through
// those inverses -> injected head PITCH makes a hard screen-locked dark fog band. Fix: one-shot scan of a
// pristine view finds which 64-byte blocks are numerically pure inverses; RefreshInverseSlots recomputes
// exactly those after all edits, every view.
// [2026-07-13 HARDENING, ported from LE1 e12b559] "Self-validating: never writes an unproven slot" was
// proven WRONG in LE1: the scan is per-boot non-deterministic and false-positives extra slots on degenerate
// frames (LE1 logged a 12-match boot validating OVERLAPPING 16-byte-apart blocks, then crashed the game the
// same millisecond the refresh first wrote them; the 2026-07-12-evening "random crash" family - heap /
// nvwgf2umx / VD-fastfail / game-code - was this one corruption). Note the scan-window comment below claims
// "ME1 only ever matched the 3 real slots" - that aged badly within hours. The 0x340 window bound blocks the
// out-of-struct neighbor case; WRITES are additionally hard-bounded to the canonical proven trio
// (IsCanonicalInvSlot); every other scan match is log-only.
constexpr int kInvMaxSlots = 24;
struct InvSlot { std::uint32_t offset; int target; float scanErr; };
InvSlot g_invSlots[kInvMaxSlots] = {};
int g_invSlotCount = 0;
std::atomic_bool g_invScanDone{false};
std::atomic_bool g_invRecheckDone{false};
std::atomic_bool g_invFixEnabled{true};   // default ON (root cause proven in LE1; scan self-limits)
std::atomic<std::uint64_t> g_invScanViews{0};
std::atomic<std::uint64_t> g_invFixLogs{0};

struct InvTarget { const char* name; std::uintptr_t off; };
constexpr InvTarget kInvTargets[] = {
    {"ViewMatrix", kFsvViewMatrix},
    {"ProjectionMatrix", kFsvProjectionMatrix},
    {"TranslatedViewMatrix", kFsvTranslatedViewMatrix},
    {"TranslatedViewProjMatrix", kFsvTranslatedViewProjMatrix},
    {"ViewProjectionMatrix", kFsvViewProjectionMatrix},
};
constexpr int kInvTargetCount = static_cast<int>(sizeof(kInvTargets) / sizeof(kInvTargets[0]));
// SCAN WINDOW - bounded to the real FSceneView struct (2026-07-12, ME3 load-crash fix). The last
// documented field is ViewOrigin @ 0x320; the three genuine cached inverses sit at 0x210/0x2A0/0x2E0,
// all below it. A 0x800 window reached PAST the struct into an adjacent allocation, where ME3 had two
// perfect inverse-matches at 0x720/0x7B0 (a pooled neighbor scene view). Those passed the readable check
// but weren't THE MOD'S memory, so RefreshInverseSlots corrupted the neighbor every frame -> crash on load.
// 0x340 covers through 0x2E0+0x40=0x320 with margin and can never reach a neighbor. (ME1 only ever
// matched the 3 real slots, so this is a behavioral no-op there; ME2/ME3 share the LE2/3 layout that has
// the phantoms.)
constexpr std::uintptr_t kInvScanBytes = 0x340;

// General 4x4 inverse (cofactor expansion; projection-bearing matrices too). False on near-singular.
bool Inverse4x4(const float* m, float* out) noexcept
{
    float inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (!std::isfinite(det) || std::fabs(det) < 1e-25f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * det;
    return true;
}

// Max deviation of P from identity; translation row normalized by camera-translation magnitude (float32
// residue of inv(M)*M on a 1e5-uu translation is legitimately ~1e-2, not a mismatch).
float IdentityErr(const float* P, float transMag) noexcept
{
    float tDiv = transMag * 5e-5f;
    if (tDiv < 1.0f) tDiv = 1.0f;
    float err = 0.0f;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
        {
            float dv = P[i * 4 + j] - ((i == j) ? 1.0f : 0.0f);
            if (dv < 0.0f) dv = -dv;
            if (i == 3 && j < 3) dv /= tDiv;
            if (dv > err) err = dv;
        }
    return err;
}

float InvPairErr(const float* B, const float* M, float transMag) noexcept
{
    float P[16];
    Mul4x4(P, B, M);
    float err = IdentityErr(P, transMag);
    Mul4x4(P, M, B);
    const float err2 = IdentityErr(P, transMag);
    return (err2 < err) ? err2 : err;
}

// SEH leaves (no C++ objects - C2712). Scan returns match count, or <0 to retry on a later view.
int InvMatScanSEH(std::uintptr_t sv, InvSlot* outSlots, int maxSlots, float* outTransMag) noexcept
{
    __try
    {
        if (!Readable(reinterpret_cast<void*>(sv), kInvScanBytes)) return -1;
        const float* vm = reinterpret_cast<const float*>(sv + kFsvViewMatrix);
        float transMag = 0.0f;
        for (int i = 12; i < 15; ++i)
        {
            const float a = (vm[i] < 0.0f) ? -vm[i] : vm[i];
            if (a > transMag) transMag = a;
        }
        if (outTransMag != nullptr) *outTransMag = transMag;
        if (transMag < 50.0f) return -2;
        int n = 0;
        for (std::uint32_t off = 0; off + 64 <= kInvScanBytes && n < maxSlots; off += 0x10)
        {
            const float* B = reinterpret_cast<const float*>(sv + off);
            for (int t = 0; t < kInvTargetCount; ++t)
            {
                if (off == kInvTargets[t].off) continue;
                const float* M = reinterpret_cast<const float*>(sv + kInvTargets[t].off);
                const float err = InvPairErr(B, M, transMag);
                if (err < 0.05f)
                {
                    outSlots[n].offset = off;
                    outSlots[n].target = t;
                    outSlots[n].scanErr = err;
                    ++n;
                    break;
                }
            }
        }
        return n;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

bool InvMatErrsSEH(std::uintptr_t sv, const InvSlot* slots, int count, float* outErrs) noexcept
{
    __try
    {
        for (int i = 0; i < count; ++i)
        {
            const float* B = reinterpret_cast<const float*>(sv + slots[i].offset);
            const float* M = reinterpret_cast<const float*>(sv + kInvTargets[slots[i].target].off);
            outErrs[i] = InvPairErr(B, M, 100000.0f);
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// [LE1 e12b559] Canonical WRITE whitelist: only the clean-room-proven trio may ever be written, and each
// only if the scan confirmed it at that exact offset+target (struct moves degrade to no-op, not corruption).
// Everything else the scan matches is diagnostic-only.
bool IsCanonicalInvSlot(std::uint32_t offset, int target) noexcept
{
    const std::uintptr_t targetOff = kInvTargets[target].off;
    return (offset == 0x210 && targetOff == kFsvTranslatedViewProjMatrix) ||
           (offset == 0x2A0 && targetOff == kFsvProjectionMatrix) ||
           (offset == 0x2E0 && targetOff == kFsvViewProjectionMatrix);
}

int InvMatRefreshSEH(std::uintptr_t sv, const InvSlot* slots, int count) noexcept
{
    __try
    {
        int fixedCount = 0;
        for (int i = 0; i < count; ++i)
        {
            if (!IsCanonicalInvSlot(slots[i].offset, slots[i].target)) continue;   // log-only candidate
            const float* M = reinterpret_cast<const float*>(sv + kInvTargets[slots[i].target].off);
            float invM[16];
            if (!Inverse4x4(M, invM)) continue;
            float* B = reinterpret_cast<float*>(sv + slots[i].offset);
            for (int k = 0; k < 16; ++k) B[k] = invM[k];
            ++fixedCount;
        }
        return fixedCount;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// One-shot scan of a PRISTINE view (call right after g_orig returns, before any edit). Retries until it
// sees a real scene view (camera >50uu from origin - identity-ish views match everything).
void InvMatScanTick(std::uintptr_t sv) noexcept
{
    if (g_invScanDone.load(std::memory_order_acquire)) return;
    const std::uint64_t views = g_invScanViews.fetch_add(1, std::memory_order_relaxed) + 1;
    if (views < 300) return;
    float transMag = 0.0f;
    InvSlot slots[kInvMaxSlots] = {};
    const int n = InvMatScanSEH(sv, slots, kInvMaxSlots, &transMag);
    if (n < 0) return;   // unreadable / near-origin view: retry later
    for (int i = 0; i < n; ++i) g_invSlots[i] = slots[i];
    g_invSlotCount = n;
    g_invScanDone.store(true, std::memory_order_release);
    for (int i = 0; i < n; ++i)
    {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "[INVMAT] scan: slot off=0x%X = inverse(%s) err=%.6f %s",
                      slots[i].offset, kInvTargets[slots[i].target].name, slots[i].scanErr,
                      IsCanonicalInvSlot(slots[i].offset, slots[i].target)
                          ? "[write-enabled]" : "[LOG-ONLY: non-canonical, never written]");
        ME2VR::Log::Line(buf);
    }
    char done[192];
    std::snprintf(done, sizeof(done), "[INVMAT] scan complete matches=%d viewTransMag=%.1f%s",
                  n, transMag, n == 0 ? " -> NO cached inverses found; fix will no-op" : "");
    ME2VR::Log::Line(done);
}

// Refresh every scan-validated inverse slot from its (now edited) target. Call after the LAST edit of each
// view (ApplyFov). Also emits the one-shot staleness proof the first time a real pitch is injected.
void RefreshInverseSlots(std::uintptr_t sv) noexcept
{
    if (!g_invScanDone.load(std::memory_order_acquire) || g_invSlotCount <= 0) return;

    const std::int32_t pitchUU = g_headPitchUU.load(std::memory_order_relaxed);
    const bool bigPitch = (pitchUU > 910) || (pitchUU < -910);   // >5 deg (65536 = 360 deg)
    if (bigPitch && !g_invRecheckDone.load(std::memory_order_acquire))
    {
        float errs[kInvMaxSlots] = {};
        if (InvMatErrsSEH(sv, g_invSlots, g_invSlotCount, errs))
        {
            g_invRecheckDone.store(true, std::memory_order_release);
            for (int i = 0; i < g_invSlotCount; ++i)
            {
                char buf[192];
                std::snprintf(buf, sizeof(buf), "[INVMAT] post-edit off=0x%X target=%s err=%.6f pitchUU=%d -> %s",
                              g_invSlots[i].offset, kInvTargets[g_invSlots[i].target].name, errs[i],
                              static_cast<int>(pitchUU),
                              errs[i] > 0.05f ? "STALE (pre-edit camera)" : "consistent");
                ME2VR::Log::Line(buf);
            }
        }
    }

    if (!g_invFixEnabled.load(std::memory_order_acquire)) return;
    const int fixedCount = InvMatRefreshSEH(sv, g_invSlots, g_invSlotCount);
    const std::uint64_t k = g_invFixLogs.fetch_add(1, std::memory_order_relaxed);
    if (k < 4 || (k % 3600) == 0)
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "[INVMAT] fix active: refreshed %d/%d inverse slots", fixedCount, g_invSlotCount);
        ME2VR::Log::Line(buf);
    }
}

// Rotate the rendered view by head yaw (about up) + pitch (about right): post-multiply ViewMatrix and
// TranslatedViewMatrix by the delta, then recompute the (translated) view-projection. Ported from LE1.
void ApplyHeadRotation(std::uintptr_t sv, float yawRad, float pitchRad) noexcept
{
    __try
    {
        auto* view = reinterpret_cast<float*>(sv + kFsvViewMatrix);
        auto* tview = reinterpret_cast<float*>(sv + kFsvTranslatedViewMatrix);
        const auto* proj = reinterpret_cast<const float*>(sv + kFsvProjectionMatrix);
        auto* vproj = reinterpret_cast<float*>(sv + kFsvViewProjectionMatrix);
        auto* tvproj = reinterpret_cast<float*>(sv + kFsvTranslatedViewProjMatrix);

        const float cy = cosf(yawRad), sy = sinf(yawRad), cp = cosf(pitchRad), sp = sinf(pitchRad);
        const float yaw[16] = { cy,0,-sy,0,  0,1,0,0,  sy,0,cy,0,  0,0,0,1 };
        const float pitch[16] = { 1,0,0,0,  0,cp,sp,0,  0,-sp,cp,0,  0,0,0,1 };
        // yaw then pitch (yaw about world-up, pitch about body-right): keeps pitch consistent at ALL
        // headings. pitch then yaw inverts pitch once you turn ~180 deg (the LE1 bug).
        float delta[16] = {}; Mul4x4(delta, yaw, pitch);
        float nv[16] = {}, ntv[16] = {};
        Mul4x4(nv, view, delta); Mul4x4(ntv, tview, delta);
        for (int i = 0; i < 16; ++i) { view[i] = nv[i]; tview[i] = ntv[i]; }
        Mul4x4(vproj, view, proj); Mul4x4(tvproj, tview, proj);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Positional 6DOF: translate the camera by the head's physical movement (XR meters, relative to
// recenter), mapped through the game-camera basis (ViewMatrix columns) into world units. Edits
// ViewOrigin + PreViewTranslation (same mechanism as the eye offset). Call BEFORE the head rotation
// so the basis is the player's facing. Ported from ME2 2026-08-13.
std::atomic_bool g_convoFpInvertFacing{false};
std::atomic<unsigned long long> g_convoFpApplies{0}, g_convoFpSkips{0};

void EnsureConvoFpOrigin(std::uintptr_t sv) noexcept
{
    if (!ME2VR::ConvoFp::OwnsCamera()) { g_convoFpSkips.fetch_add(1, std::memory_order_relaxed); return; }
    ME2VR::ConvoFp::EyePose pose{};
    if (!ME2VR::ConvoFp::GetEyePose(&pose)) { g_convoFpSkips.fetch_add(1, std::memory_order_relaxed); return; }
    float cam[9] = {};
    __try
    {
        auto* view = reinterpret_cast<float*>(sv + kFsvViewMatrix);
        cam[0]=view[0]; cam[1]=view[4]; cam[2]=view[8]; cam[3]=view[1]; cam[4]=view[5]; cam[5]=view[9]; cam[6]=view[2]; cam[7]=view[6]; cam[8]=view[10];
        volatile float* vo = reinterpret_cast<volatile float*>(sv + kFsvViewOrigin);
        const float dx=pose.x-vo[0], dy=pose.y-vo[1], dz=pose.z-vo[2];
        if (!(dx*dx+dy*dy+dz*dz < 4.0e8f)) return;
        volatile float* pvt = reinterpret_cast<volatile float*>(sv + kFsvPreViewTranslation);
        pvt[0]-=dx; pvt[1]-=dy; pvt[2]-=dz; vo[0]+=dx; vo[1]+=dy; vo[2]+=dz;
        view[12]-=dx*cam[0]+dy*cam[1]+dz*cam[2]; view[13]-=dx*cam[3]+dy*cam[4]+dz*cam[5]; view[14]-=dx*cam[6]+dy*cam[7]+dz*cam[8];
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    float yaw=pose.baseYawDeg*0.01745329252f; if(g_convoFpInvertFacing.load(std::memory_order_relaxed))yaw+=3.14159265359f;
    const float cy=cosf(yaw),sy=sinf(yaw),des[9]={-sy,cy,0, 0,0,1, cy,sy,0}; float d[16]={};
    for(int k=0;k<3;++k)for(int j=0;j<3;++j)d[k*4+j]=cam[k*3]*des[j*3]+cam[k*3+1]*des[j*3+1]+cam[k*3+2]*des[j*3+2]; d[15]=1;
    __try
    {
        auto* view=reinterpret_cast<float*>(sv+kFsvViewMatrix); auto* tview=reinterpret_cast<float*>(sv+kFsvTranslatedViewMatrix);
        const auto* proj=reinterpret_cast<const float*>(sv+kFsvProjectionMatrix); auto* vp=reinterpret_cast<float*>(sv+kFsvViewProjectionMatrix); auto* tvp=reinterpret_cast<float*>(sv+kFsvTranslatedViewProjMatrix);
        float nv[16]{},ntv[16]{};Mul4x4(nv,view,d);Mul4x4(ntv,tview,d);for(int i=0;i<16;++i){view[i]=nv[i];tview[i]=ntv[i];}Mul4x4(vp,view,proj);Mul4x4(tvp,tview,proj);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    g_convoFpApplies.fetch_add(1,std::memory_order_relaxed);
}
void ApplyHeadPosition(std::uintptr_t sv) noexcept
{
    if (!g_headPosOn.load(std::memory_order_acquire)) return;
    // [LEANFP 2026-08-14] The first-person early-out was ported verbatim from ME2 and it silently
    // disabled 6DOF for the mode this player actually uses - lean had never run once. ME2's rationale
    // ("the camera is already at the head") does not hold here: LE3 first-person is a fixed eye
    // OFFSET applied to the pawn's camera, not a head-mounted camera, so the player's real head
    // translation is exactly the parallax that offset is missing. In third person it gives the
    // camera genuine positional parallax around its boom. PosEnabled remains the master switch.
    float hx = g_headPosX.load(std::memory_order_relaxed);
    float hy = g_headPosY.load(std::memory_order_relaxed);
    float hz = g_headPosZ.load(std::memory_order_relaxed);
    if (hx < -2.0f) hx = -2.0f; else if (hx > 2.0f) hx = 2.0f;   // clamp to sane head movement (ignore garbage)
    if (hy < -2.0f) hy = -2.0f; else if (hy > 2.0f) hy = 2.0f;
    if (hz < -2.0f) hz = -2.0f; else if (hz > 2.0f) hz = 2.0f;
    // ME1-PARITY 6DOF: fixed 50 uu/m (WORLD_TO_METERS), NOT coupled to the stereo eye slider, and
    // forward/back emphasized 3x - matches ME1/ME2 exactly (kMetersToUU=50, kFwdLeanEmphasis=3).
    constexpr float kMetersToUU = 50.0f;
    constexpr float kFwdLeanEmphasis = 3.0f;
    const float w2m = kMetersToUU * g_headPosScale.load(std::memory_order_relaxed);   // uu/m * gain (ME1 leanGain)
    __try
    {
        const volatile float* VM = reinterpret_cast<const volatile float*>(sv + kFsvViewMatrix);
        const float rX = VM[0], rY = VM[4], rZ = VM[8];    // camera right   (col0)
        const float uX = VM[1], uY = VM[5], uZ = VM[9];    // camera up      (col1)
        const float fX = VM[2], fY = VM[6], fZ = VM[10];   // camera forward (col2)
        // [LEANFWD] XR: +x right, +y up, -z forward. The basis above IS correct (for a world->view
        // matrix in row-vector form the COLUMNS are the camera axes), so -hz is the mathematically
        // right forward term. ME2 settled this with a toggle: whichever way it reads wrong in the
        // headset, one click fixes it instead of another build.
        const float fwdSign = g_leanInvertFwd.load(std::memory_order_relaxed) ? 1.0f : -1.0f;
        const float dR = hx, dU = hy, dF = fwdSign * hz * kFwdLeanEmphasis;
        const float wx = w2m * (dR * rX + dU * uX + dF * fX);
        const float wy = w2m * (dR * rY + dU * uY + dF * fY);
        const float wz = w2m * (dR * rZ + dU * uZ + dF * fZ);
        volatile float* pvt = reinterpret_cast<volatile float*>(sv + kFsvPreViewTranslation);
        volatile float* vo = reinterpret_cast<volatile float*>(sv + kFsvViewOrigin);
        pvt[0] -= wx; pvt[1] -= wy; pvt[2] -= wz;
        vo[0] += wx;  vo[1] += wy;  vo[2] += wz;
        // [EYEVPM] bake the lean into ViewMatrix's translation row too (see ApplyEyeOffset) so the
        // sprite/VPM path tracks 6DOF; ApplyHeadRotation runs right after and rebuilds the VPMs.
        volatile float* vmw = reinterpret_cast<volatile float*>(sv + kFsvViewMatrix);
        vmw[12] -= (wx * rX + wy * rY + wz * rZ);
        vmw[13] -= (wx * uX + wy * uY + wz * uZ);
        vmw[14] -= (wx * fX + wy * fY + wz * fZ);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void ApplyHeadLook(std::uintptr_t sv) noexcept
{
    // Translation has its own PosEnabled master and must not depend on who currently owns ROTATION.
    // Head aim deliberately parks render-side yaw/pitch; the old ordering returned here first and
    // silently killed positional lean whenever aim was active (or rotational head look was disabled).
    ApplyHeadPosition(sv);   // positional 6DOF first (uses the game-camera basis, pre-rotation)
    if (!g_headLookEnabled.load(std::memory_order_acquire)) return;
    constexpr float kUUToRad = 6.28318530718f / 65536.0f;
    ApplyHeadRotation(sv,
                      static_cast<float>(g_headYawUU.load(std::memory_order_relaxed)) * kUUToRad,
                      static_cast<float>(g_headPitchUU.load(std::memory_order_relaxed)) * kUUToRad);
}

// Publish the game's per-eye half-FOV (radians) from the projection matrix: proj[0]=1/tan(halfH), proj[5]=1/tan(halfV).
void PublishGameFov(std::uintptr_t sv) noexcept
{
    __try
    {
        const volatile float* P = reinterpret_cast<const volatile float*>(sv + kFsvProjectionMatrix);
        const float p0 = P[0], p5 = P[5], p15 = P[15];
        // Publish before WidenProjection edits this view.
        g_markerProjectionSeq.fetch_add(1, std::memory_order_acq_rel);   // odd = writer active
        for (int i = 0; i < 16; ++i) g_markerRawProjection[i].store(P[i], std::memory_order_relaxed);
        g_markerProjectionSeq.fetch_add(1, std::memory_order_release);   // even = complete
        g_markerProjectionReady.store(true, std::memory_order_release);

        if (p0 > 0.0001f && p5 > 0.0001f && p15 > -0.01f && p15 < 0.01f)   // perspective only
        {
            float h = atanf(1.0f / p0), v = atanf(1.0f / p5);
            g_srcBuiltFovH.store(h, std::memory_order_relaxed);   // [FILLSRC] what the engine built
            if (g_srcRawValid.load(std::memory_order_relaxed))
            {
                // [FILLSRC] the build already contains the fill FOV; raw detection uses the camera's
                // true FOV read at the source, vertical derived through the SAME built aspect.
                const float sh = g_srcRawFovH.load(std::memory_order_relaxed);
                if (sh > 0.01f) { h = sh; v = atanf(tanf(sh) * (p0 / p5)); }
            }
            g_gameRawFovH.store(h, std::memory_order_relaxed);   // raw -> detection
            g_gameRawFovV.store(v, std::memory_order_relaxed);
            g_gameHalfFovH.store(h, std::memory_order_relaxed);  // submit default = raw (fill may widen)
            g_gameHalfFovV.store(v, std::memory_order_relaxed);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// FOV-fill: overwrite this view's projection half-FOV (proj[0]=1/tan(hHalf), proj[5]=1/tan(vHalf)) to
// the wider target so the engine renders + culls to the wider frustum, then rebuild the (translated)
// view-projection. Must run AFTER eye-offset + head-look so it preserves them.
void WidenProjection(std::uintptr_t sv, float hHalf, float vHalf) noexcept
{
    __try
    {
        auto* proj = reinterpret_cast<float*>(sv + kFsvProjectionMatrix);
        const float th = tanf(hHalf), tv = tanf(vHalf);
        if (th > 0.0001f) proj[0] = 1.0f / th;
        if (tv > 0.0001f) proj[5] = 1.0f / tv;
        const auto* view = reinterpret_cast<const float*>(sv + kFsvViewMatrix);
        const auto* tview = reinterpret_cast<const float*>(sv + kFsvTranslatedViewMatrix);
        auto* vproj = reinterpret_cast<float*>(sv + kFsvViewProjectionMatrix);
        auto* tvproj = reinterpret_cast<float*>(sv + kFsvTranslatedViewProjMatrix);
        Mul4x4(vproj, view, proj);
        Mul4x4(tvproj, tview, proj);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Per-eye FOV: either widen to the fill target (and publish that as the submitted FOV), or just publish
// the game's own FOV (fallback when fill is off).
void ApplyFov(std::uintptr_t sv) noexcept
{
    PublishGameFov(sv);
    // [FILLSRC] mechanism proof, one-shot: built == written means the engine consumed the
    // pre-written camera FOV (distortion-class constants consistent by construction); built ==
    // raw means the write never reached the build and the smear fix is NOT active - that line is
    // the next clue, not a guess.
    if (g_srcRawValid.load(std::memory_order_relaxed))
    {
        static std::atomic<int> s_fillSrcLogged{0};
        const int n = s_fillSrcLogged.load(std::memory_order_relaxed);
        if (n < 4)
        {
            s_fillSrcLogged.store(n + 1, std::memory_order_relaxed);
            char b[192] = {};
            sprintf_s(b, "[FILLSRC] engine-built hHalf=%.4f written=%.4f raw(src)=%.4f (built==written => fix active)",
                      g_srcBuiltFovH.load(std::memory_order_relaxed),
                      g_srcWrittenFovH.load(std::memory_order_relaxed),
                      g_srcRawFovH.load(std::memory_order_relaxed));
            ME2VR::Log::Line(b);
        }
    }
    // Default: rendered == raw game FOV (any early return below leaves it that way).
    g_renderHalfFovH.store(g_gameRawFovH.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_renderHalfFovV.store(g_gameRawFovV.load(std::memory_order_relaxed), std::memory_order_relaxed);
    if (!g_fovFillOn.load(std::memory_order_acquire)) return;
    const float th = g_fovFillH.load(std::memory_order_relaxed);   // headset fill target (half-FOV, aspect-corr)
    const float tv = g_fovFillV.load(std::memory_order_relaxed);
    if (th < 0.01f || tv < 0.01f) return;
    const float gh = g_gameRawFovH.load(std::memory_order_relaxed);   // RAW game half-FOV this view
    const float gv = g_gameRawFovV.load(std::memory_order_relaxed);
    if (gh < 0.01f || gv < 0.01f) return;

    // First-person conversations own a constant headset window. Zoom narrows only the rendered
    // projection while the submitted FOV remains unchanged, matching ME2 without warping the UI.
    if (ME2VR::ConvoFp::OwnsCamera())
    {
        const float z = ME2VR::ConvoFp::GetZoom();
        const float rH = (z > 1.0f) ? atanf(tanf(th) / z) : th;
        const float rV = (z > 1.0f) ? atanf(tanf(tv) / z) : tv;
        WidenProjection(sv, rH, rV);
        g_renderHalfFovH.store(rH, std::memory_order_relaxed);
        g_renderHalfFovV.store(rV, std::memory_order_relaxed);
        g_gameHalfFovH.store(th, std::memory_order_relaxed);
        g_gameHalfFovV.store(tv, std::memory_order_relaxed);
        return;
    }

    // Capture the gameplay "rest" FOV (hipfire/explore = wide). ADS/sniper narrow it; this holds the
    // last wide value, so (gh/rest) measures how far the game has zoomed in.
    if (gh >= 0.55f && ME2VR::EngineProbe::IsGameplayCamera() &&
        !ME2VR::D3DCapture::GetMenuMode())
    {
        g_restHalfFov.store(gh, std::memory_order_relaxed);
        g_restHalfFovV.store(gv, std::memory_order_relaxed);
    }
    float rest = g_restHalfFov.load(std::memory_order_relaxed);
    if (rest < 0.45f) rest = 0.61f;   // sane default until captured

    // [VRCINE FILL] In-VR conversations/cutscenes (CineVrConvo/CineVrCutscene on). ME2's contract,
    // ported: the director's per-shot framing FOV is not the mod's to honour in a headset -- honouring it
    // put the whole scene in a ~20-deg window inside a ~100-deg view, and the gameplay branch below
    // applies the zoom term (gh/rest with a tiny cine gh = a huge magnifier, ME1's "nearby faces fill
    // the lens" bug). Render AND declare the constant headset window instead, so the scene fills the
    // view and shot cuts change the picture, not the screen. The zoom slider renders NARROWER while
    // declaring the same window: uniform magnification, UI bit-identical (ME1's proven model). Raw FOV
    // keeps publishing the director's value, so the cine detector/hysteresis in me2_xr is untouched.
    // MENU_MONO is the single authoritative owner. It must win even if a stale VR-cine latch survives
    // for one frame while a menu opens, otherwise ApplyFov visibly fights the flat presentation.
    if (ME2VR::D3DCapture::GetMenuMode()) return;

    if (ME2VR::CalcViewHook::GetVrCineActive())
    {
        const float z = ME2VR::Me2Xr::GetCineScreenZoom();
        const float rH = (z > 0.01f) ? atanf(tanf(th) / z) : th;
        const float rV = (z > 0.01f) ? atanf(tanf(tv) / z) : tv;
        WidenProjection(sv, rH, rV);
        g_renderHalfFovH.store(rH, std::memory_order_relaxed);
        g_renderHalfFovV.store(rV, std::memory_order_relaxed);
        g_gameHalfFovH.store(th, std::memory_order_relaxed);   // declare the headset window, always
        g_gameHalfFovV.store(tv, std::memory_order_relaxed);
        return;
    }

    // Conversation/cutscene -> leave raw so it is detected and goes MONO (flat panel). ME2 gates this
    // on "narrow AND no weapon out"; ME3 has no weapon-out signal, so it uses ME3's own cinematic flag.
    if (g_cinematic.load(std::memory_order_acquire)) return;

    // Menus -> leave raw too. The squad menu renders Shepard's 3D model through this same hook, and
    // widening its projection to the headset's near-square shape while the menu is shown on a 16:9
    // flat quad stretched the model vertically - the 2D UI around it never passes through here, which
    // is why only the model distorted.
    if (ME2VR::D3DCapture::GetMenuMode()) return;

    // Continuous fill + zoom - this is ME2's working behaviour, ported as-is. The mod always SUBMITS the
    // headset target (th/tv) so the image fills the headset; the RENDER FOV is that same target at
    // gameplay rest (=> 1:1, gameplay untouched) and shrinks proportionally as the game zooms in below
    // rest (ADS/sniper), so the world magnifies into the fill with no pop crossing into ADS.
    //
    // NOTE the crucial difference from what was here before: the projection is set to the HEADSET's
    // shape outright, NOT to the game's FOV scaled uniformly. A uniform scale preserves the game's
    // 16:9 aspect, which can never match a near-square headset FOV - that mismatch was the letterbox,
    // and no resolution change could ever have fixed it.
    const float frac = (gh < rest) ? (gh / rest) : 1.0f;   // 1.0 at rest, <1 zoomed in
    const float renderH = th * frac;
    const float renderV = tv * frac;
    WidenProjection(sv, renderH, renderV);
    g_renderHalfFovH.store(renderH, std::memory_order_relaxed);   // what this view really rendered
    g_renderHalfFovV.store(renderV, std::memory_order_relaxed);
    g_gameHalfFovH.store(th, std::memory_order_relaxed);   // SUBMIT headset target; render narrower => zoom-fill
    g_gameHalfFovV.store(tv, std::memory_order_relaxed);
}

bool ConvoFpWriteSourcePose(void* loc, void* rot, float x, float y, float z, float yawDeg) noexcept
{
    __try
    {
        auto* l=reinterpret_cast<volatile float*>(loc);l[0]=x;l[1]=y;l[2]=z;
        auto* r=reinterpret_cast<volatile std::int32_t*>(rot);r[0]=0;r[1]=static_cast<std::int32_t>(yawDeg*(65536.0f/360.0f));r[2]=0; return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadRect(std::uintptr_t sv, float* r4) noexcept
{
    __try { const volatile float* R = reinterpret_cast<const volatile float*>(sv + kFsvFloatRect);
            r4[0]=R[0]; r4[1]=R[1]; r4[2]=R[2]; r4[3]=R[3]; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::string Hex(std::uintptr_t v)
{
    char b[32] = {};
    sprintf_s(b, "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

// SEH-guarded read of the FSceneView fields into plain locals (no C++ objects in the __try).
bool ReadView(std::uintptr_t sv, float* proj16, float* view16, float* origin3) noexcept
{
    __try
    {
        const volatile float* P = reinterpret_cast<const volatile float*>(sv + kFsvProjectionMatrix);
        const volatile float* V = reinterpret_cast<const volatile float*>(sv + kFsvViewMatrix);
        const volatile float* O = reinterpret_cast<const volatile float*>(sv + kFsvViewOrigin);
        for (int i = 0; i < 16; ++i) { proj16[i] = P[i]; view16[i] = V[i]; }
        origin3[0] = O[0]; origin3[1] = O[1]; origin3[2] = O[2];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// [VIEWCEN] scene-view census: the squad/outfit menus composite a 3D character pane that comes out
// vertically stretched, and no classifier catches those screens (not gm 7/8, FOV not narrow, so the
// gameplay fill stays live). The stretch means SOME view is rendered near-square and shown 16:9 -
// this census fingerprints every perspective view built (P1 or not, every VR path), keyed by
// (isP1, fov, tan-space aspect, rect), and dumps counts every ~3s. The near-square aspect (~1.1)
// vs the game's 16:9 (~1.78) makes the widened view unmistakable, and the pane view's own
// signature is the discriminator the real fix gates on.
namespace
{
    struct VcEntry { int isP1; float fov, aspect, rx, ry, rw, rh; unsigned count; };
    VcEntry g_vc[10];
    int g_vcN = 0;
    unsigned g_vcDropped = 0;
    unsigned long long g_vcLastDumpMs = 0;
}

void ViewCensusNote(std::uintptr_t svu, bool isP1) noexcept
{
    if (!g_vrEnabled.load(std::memory_order_acquire)) return;
    float proj[16] = {}, view[16] = {}, origin[3] = {};
    if (!ReadView(svu, proj, view, origin)) return;
    const bool perspective = (proj[11] > 0.99f && proj[11] < 1.01f) &&
                             (proj[15] > -0.01f && proj[15] < 0.01f) &&
                             proj[0] > 0.0001f && proj[5] > 0.0001f;
    if (!perspective) return;
    const float fovDeg = 2.0f * atanf(1.0f / proj[0]) * 57.2957795f;
    const float aspect = proj[5] / proj[0];   // tan-space W/H: 16:9 -> ~1.78, headset fill -> ~1.0-1.2
    float r4[4] = {};
    const bool haveRect = ReadRect(svu, r4);

    for (int i = 0; i < g_vcN; ++i)
    {
        VcEntry& e = g_vc[i];
        if (e.isP1 == (isP1 ? 1 : 0) &&
            fabsf(e.fov - fovDeg) < 0.25f && fabsf(e.aspect - aspect) < 0.02f &&
            (!haveRect || (fabsf(e.rw - r4[2]) < 1.0f && fabsf(e.rh - r4[3]) < 1.0f)))
        { ++e.count; goto dumpCheck; }
    }
    if (g_vcN < 10)
    {
        VcEntry& e = g_vc[g_vcN++];
        e.isP1 = isP1 ? 1 : 0; e.fov = fovDeg; e.aspect = aspect;
        e.rx = haveRect ? r4[0] : -1.0f; e.ry = haveRect ? r4[1] : -1.0f;
        e.rw = haveRect ? r4[2] : -1.0f; e.rh = haveRect ? r4[3] : -1.0f;
        e.count = 1;
    }
    else ++g_vcDropped;

dumpCheck:
    {
        const unsigned long long nowMs = GetTickCount64();
        if (g_vcLastDumpMs == 0) { g_vcLastDumpMs = nowMs; return; }
        if (nowMs - g_vcLastDumpMs < 3000ull) return;
        g_vcLastDumpMs = nowMs;
        char b[640] = {};
        int off = sprintf_s(b, "[VIEWCEN] gm=%d gcam=%d menu=%d cine=%d vrcine=%d |",
                            ME2VR::D3DCapture::GetAutoGameMode(),
                            ME2VR::EngineProbe::IsGameplayCamera() ? 1 : 0,
                            ME2VR::D3DCapture::GetMenuMode() ? 1 : 0,
                            g_cinematic.load(std::memory_order_acquire) ? 1 : 0,
                            ME2VR::CalcViewHook::GetVrCineActive() ? 1 : 0);
        for (int i = 0; i < g_vcN && off > 0 && off < 560; ++i)
        {
            const VcEntry& e = g_vc[i];
            off += sprintf_s(b + off, sizeof(b) - off, " %sP1 fov=%.1f a=%.2f rect=%.0fx%.0f@%.0f,%.0f n=%u |",
                             e.isP1 ? "" : "non", static_cast<double>(e.fov), static_cast<double>(e.aspect),
                             static_cast<double>(e.rw), static_cast<double>(e.rh),
                             static_cast<double>(e.rx), static_cast<double>(e.ry), e.count);
        }
        if (g_vcDropped != 0 && off > 0 && off < 600)
            sprintf_s(b + off, sizeof(b) - off, " dropped=%u", g_vcDropped);
        ME2VR::Log::Line(b);
        g_vcN = 0; g_vcDropped = 0;
    }
}

// [FRAMEOWNER] game-thread identity and per-pass wall time, read and reset by the present-hook
// telemetry once per window. Written only by DrawDetour (one thread), read by the present thread.
std::atomic<unsigned long> g_drawThreadId{0};
std::atomic<uint64_t> g_dpP0SumUs{0}, g_dpP1SumUs{0};
std::atomic<unsigned> g_dpP0MaxUs{0}, g_dpP1MaxUs{0}, g_dpCount{0};
LARGE_INTEGER g_dpFreq = {};

void DrawPassNote(const LARGE_INTEGER& a, const LARGE_INTEGER& b, const LARGE_INTEGER& c) noexcept
{
    if (g_dpFreq.QuadPart == 0) QueryPerformanceFrequency(&g_dpFreq);
    const double f = static_cast<double>(g_dpFreq.QuadPart);
    const unsigned p0 = static_cast<unsigned>(static_cast<double>(b.QuadPart - a.QuadPart) * 1e6 / f);
    const unsigned p1 = static_cast<unsigned>(static_cast<double>(c.QuadPart - b.QuadPart) * 1e6 / f);
    g_dpP0SumUs.fetch_add(p0, std::memory_order_relaxed);
    g_dpP1SumUs.fetch_add(p1, std::memory_order_relaxed);
    unsigned m = g_dpP0MaxUs.load(std::memory_order_relaxed);
    while (p0 > m && !g_dpP0MaxUs.compare_exchange_weak(m, p0, std::memory_order_relaxed)) {}
    m = g_dpP1MaxUs.load(std::memory_order_relaxed);
    while (p1 > m && !g_dpP1MaxUs.compare_exchange_weak(m, p1, std::memory_order_relaxed)) {}
    g_dpCount.fetch_add(1, std::memory_order_relaxed);
}

// [SFR] POD-only SEH wrapper for the replay Draw (a function using __try may hold no C++ objects with
// destructors - keep this frame POD). Returns false on fault.
bool SfrReplayDrawSEH(void* self, void* viewport, void* canvas) noexcept
{
    __try { g_origDraw(self, viewport, canvas); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// [SFR] FViewportClient::Draw detour. Pass 0 = the game's own Draw (renders the frame once, -halfEye
// via t_replay=false). In SFR gameplay, run Draw a SECOND time (pass 1, +halfEye) = the whole frame
// re-rendered as a full primary view for the other eye. SEH-guarded; skipped in menu/cinematic (the
// engine's re-entrant-Draw crash territory, per the ME1/ME2 lessons). No D3D work here: pass-0 capture
// is driven entirely on the RENDER thread (d3d_capture's clear hook). Touching D3D from this thread
// raced the render thread on ME2 and produced the VR-load crash.
void __fastcall DrawDetour(void* self, void* viewport, void* canvas) noexcept
{
    if (g_origDraw == nullptr) return;
    const bool convoFp = ME2VR::ConvoFp::OwnsCamera();
    const bool sfrGameplay =
        g_vrEnabled.load(std::memory_order_acquire) &&
        g_vrMode.load(std::memory_order_acquire) == (int)VrMode::Sfr &&
        (!ME2VR::D3DCapture::GetMenuMode() || convoFp) &&
        (!g_cinematic.load(std::memory_order_acquire) || convoFp);

    // [CINEMAP] probe: name the thread Draw runs on, once. The capture's clear hook logs its own
    // thread id; if they differ, no game-thread signal (t_replay included) can order the capture,
    // which is the documented reason the replay flag is invisible to it.
    {
        static std::atomic_bool s_tidLogged{false};
        if (!s_tidLogged.exchange(true))
            ME2VR::Log::Line("[CINEMAP] DrawDetour thread=" + std::to_string(GetCurrentThreadId()));
    }

    // [FRAMEOWNER] this is the game thread; publish its id so the present-hook telemetry can read
    // its CPU time, and time both passes as the game thread sees them (a pass that waits on the
    // render thread shows up here as wall time without CPU time).
    g_drawThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    LARGE_INTEGER q0, q1, q2;
    QueryPerformanceCounter(&q0);

    t_replay = false;
    g_origDraw(self, viewport, canvas);          // PASS 0 = the game's normal render (-halfEye)
    QueryPerformanceCounter(&q1);
    if (!sfrGameplay) { DrawPassNote(q0, q1, q1); return; }

    t_replay = true;
    const bool ok = SfrReplayDrawSEH(self, viewport, canvas);   // PASS 1 = replay (+halfEye)
    t_replay = false;
    QueryPerformanceCounter(&q2);
    DrawPassNote(q0, q1, q2);

    const uint64_t n = g_sfrReplays.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!ok) g_sfrReplayFaults.fetch_add(1, std::memory_order_relaxed);
    if (n <= 4 || (n % 600) == 0)
    {
        char b[128] = {};
        sprintf_s(b, "[SFR] Draw replay #%llu ok=%d faults=%llu",
                  static_cast<unsigned long long>(n), ok ? 1 : 0,
                  static_cast<unsigned long long>(g_sfrReplayFaults.load(std::memory_order_relaxed)));
        ME2VR::Log::Line(b);
    }
}

void* __fastcall CalcViewDetour(void* lp, void* family, void* loc, void* rot, void* vp, void* drawer) noexcept
{
    if (g_orig == nullptr) return nullptr;

    const std::uintptr_t lpu = reinterpret_cast<std::uintptr_t>(lp);
    const bool isP1 = (lpu == ME2VR::EngineProbe::GetPrimaryLocalPlayer());

    // [ENGCINE] bump on EVERY P1 view build, whatever branch runs below. A prerendered bik movie
    // does not build scene views at all (calcViews ~0/s observed during one), while an in-engine
    // cutscene builds them every frame - this seq is how the XR side tells the two apart, so it
    // must not stall when the mono/cinematic branch is taken.
    if (isP1) g_p1CalcSeq.fetch_add(1, std::memory_order_relaxed);

    // ---- Milestone B1: P1-layout same-frame split ----
    // Rewrite P1's viewport to the LEFT half, build the view; rewrite to the RIGHT half, build a
    // second view into the same family (NULL state for B1 -> renders minus occlusion, never shares
    // P1's state). No eye offset yet (B2). The engine then renders both into an SBS backbuffer.
    // World is ALWAYS stereo now (UI is a separate flat overlay) - no mode switch, no snap.
    // Flat/mono: galaxy map / manual F2 (GetMenuMode) OR a conversation/cutscene (g_cinematic, narrow
    // FOV) -> render ONE full view (skip the split) = clean flat panel, not a squashed/tiny stereo split.
    // [FILLSRC] cleared every P1 build; re-armed below when the source pre-write lands, so a
    // menu/cinematic view entered right after gameplay publishes its own raw FOV untouched.
    if (isP1) g_srcRawValid.store(false, std::memory_order_relaxed);

    // Feed the staged-eye pose into the engine before it builds frustum/culling. The render-side
    // relocation below remains required because LE3, like LE2, can replace the final view origin.
    if (isP1 && ME2VR::ConvoFp::OwnsCamera() && loc != nullptr && rot != nullptr)
    {
        ME2VR::ConvoFp::EyePose pose{};
        if (ME2VR::ConvoFp::GetEyePose(&pose))
        {
            float yaw = pose.baseYawDeg + (g_convoFpInvertFacing.load(std::memory_order_relaxed) ? 180.0f : 0.0f);
            while (yaw > 180.0f) yaw -= 360.0f; while (yaw < -180.0f) yaw += 360.0f;
            ConvoFpWriteSourcePose(loc, rot, pose.x, pose.y, pose.z, yaw);
        }
    }

    if (g_vrEnabled.load(std::memory_order_acquire) && isP1 &&
        !ME2VR::ConvoFp::OwnsCamera() &&
        (ME2VR::D3DCapture::GetMenuMode() || g_cinematic.load(std::memory_order_acquire)))
    {
        void* sv = g_orig(lp, family, loc, rot, vp, drawer);
        if (sv != nullptr) PublishGameFov(reinterpret_cast<std::uintptr_t>(sv));   // keep FOV live so XR detects cinematic END
        if (sv != nullptr) ViewCensusNote(reinterpret_cast<std::uintptr_t>(sv), true);   // [VIEWCEN]
        return sv;
    }

    // ---- [FILLSRC] gameplay fill at the SOURCE (ME2's glass-smear fix) -------------------------
    // Write the fill FOV into the CAMERA before the engine builds this view, so distortion-class
    // shader constants (glass/refraction) derive from the FOV the scene actually renders with.
    // The camera FOV is RESTORED when this call returns (guard below): the game's own HUD
    // projection samples the camera later in the frame and must keep seeing the raw FOV - exactly
    // what [UIRATIO]'s correction assumes. ApplyFov still runs after the build, recomputes the
    // same numbers (delta ~0) and stays the only writer of the published render/submit FOVs; its
    // zoom/rest/VRCINE logic is mirrored here on the SOURCE-read FOV so behaviour is unchanged -
    // only WHO builds the projection moved. Offsets come from [CAMFOV] runtime discovery.
    struct FovAtSourceGuard
    {
        bool  active = false;
        float prevDeg = 0.0f;
        ~FovAtSourceGuard() { if (active) ME2VR::EngineProbe::SetCameraFovDeg(prevDeg, nullptr); }
    } fovSrc;
    if (g_vrEnabled.load(std::memory_order_acquire) && isP1 &&
        g_fovFillOn.load(std::memory_order_acquire) && ME2VR::EngineProbe::CamFovReady())
    {
        const float th = g_fovFillH.load(std::memory_order_relaxed);
        float camDeg = 0.0f;
        if (th > 0.05f && ME2VR::EngineProbe::GetCameraFovDeg(&camDeg))
        {
            const float gh = 0.5f * camDeg * (3.14159265358979f / 180.0f);   // camera FOV = horizontal degrees
            if (gh >= 0.55f && ME2VR::EngineProbe::IsGameplayCamera() &&
                !ME2VR::D3DCapture::GetMenuMode())
            {
                g_restHalfFov.store(gh, std::memory_order_relaxed);
                const float rawV = g_gameRawFovV.load(std::memory_order_relaxed);
                if (rawV > 0.01f) g_restHalfFovV.store(rawV, std::memory_order_relaxed);
            }
            float rest = g_restHalfFov.load(std::memory_order_relaxed);
            if (rest < 0.45f) rest = 0.61f;
            float renderH = 0.0f;
            if (ME2VR::ConvoFp::OwnsCamera())
            {
                const float z = ME2VR::ConvoFp::GetZoom();
                renderH = (z > 1.0f) ? atanf(tanf(th) / z) : th;
            }
            else if (ME2VR::CalcViewHook::GetVrCineActive())
            {
                const float z = ME2VR::Me2Xr::GetCineScreenZoom();
                renderH = (z > 0.01f) ? atanf(tanf(th) / z) : th;
            }
            else if (gh >= 0.01f)
            {
                const float frac = (gh < rest) ? (gh / rest) : 1.0f;   // mirror gameplay zoom-fill
                renderH = th * frac;
            }
            if (renderH > 0.01f)
            {
                float deg = 2.0f * renderH * (180.0f / 3.14159265358979f);
                if (deg > 170.0f) deg = 170.0f; else if (deg < 10.0f) deg = 10.0f;
                if (ME2VR::EngineProbe::SetCameraFovDeg(deg, &fovSrc.prevDeg))
                {
                    fovSrc.active = true;
                    g_srcRawFovH.store(gh, std::memory_order_relaxed);
                    g_srcWrittenFovH.store(renderH, std::memory_order_relaxed);
                    g_srcRawValid.store(true, std::memory_order_relaxed);
                }
            }
        }
    }

    // ---- SFR (same-frame stereo): render ONE full view here (no SBS split). The DrawDetour replay
    // renders the whole frame a SECOND time this present, so the mod gets two full PRIMARY renders (one per
    // eye) with correct bloom/lighting in BOTH eyes. Pass 0 (game's Draw) = -halfEye, pass 1 (replay)
    // = +halfEye, distinguished by the thread-local t_replay. Same edit order as AER. ----
    if (g_vrEnabled.load(std::memory_order_acquire) &&
        g_vrMode.load(std::memory_order_acquire) == (int)VrMode::Sfr && isP1)
    {
        void* sv = g_orig(lp, family, loc, rot, vp, drawer);
        if (sv != nullptr)
        {
            const std::uintptr_t svu = reinterpret_cast<std::uintptr_t>(sv);
            // PER-EYE VIEW STATE: each pass gets its OWN persistent FSceneViewState. Both passes go
            // through the game's normal Draw, which uses P1's shared state; without this, pass 1
            // inherits the temporal history pass 0 just wrote (different camera), so TAA/motion-blur
            // smear and shake on one eye. Reuses the two eye states the SBS path already allocates.
            {
                void*& eyeSt = t_replay ? g_eyeState : g_eyeStateLeft;   // pass1=right state, pass0=left state
                if (eyeSt == nullptr && g_allocViewState != nullptr)
                {
                    void* st = CallAllocViewState();
                    if (Readable(st, 0x40)) eyeSt = st;
                }
                if (eyeSt != nullptr) SetViewState(svu, eyeSt);   // null-safe; never force NULL (that smears too)
            }
            InvMatScanTick(svu);             // [INVMAT] pristine-view scan (one-shot; must precede any edit)
            float he = g_halfEyeUU.load(std::memory_order_relaxed);
            if (g_swapEyes.load(std::memory_order_relaxed)) he = -he;
            g_sfrCalcViews.fetch_add(1, std::memory_order_relaxed);   // [SFR2]
            const float signedUU = t_replay ? +he : -he;   // pass1 = right (+), pass0 = left (-)
            ApplyEyeOffset(svu, signedUU);   // IPD only (lean comes from ApplyHeadLook)
            ApplyHeadLook(svu);              // offset BEFORE head rotation (same order as stereo/AER)
            ApplyFov(svu);
            RefreshInverseSlots(svu);        // [INVMAT] keep fog/shaft screen->world matching the edit
            g_splitSeq.fetch_add(1, std::memory_order_release);   // keep SBS hysteresis satisfied
            ViewCensusNote(svu, true);       // [VIEWCEN]
        }
        return sv;
    }

    // ---- DIBR: render ONE full mono view with head-look + FOV-fill so it fills the headset exactly like
    // AER/stereo (stereo comes from the depth WARP, not a camera offset). Depth is captured off this render. ----
    if (g_vrEnabled.load(std::memory_order_acquire) &&
        g_vrMode.load(std::memory_order_acquire) == (int)VrMode::Dibr && isP1)
    {
        void* sv = g_orig(lp, family, loc, rot, vp, drawer);
        if (sv != nullptr)
        {
            const std::uintptr_t svu = reinterpret_cast<std::uintptr_t>(sv);
            InvMatScanTick(svu);  // [INVMAT] pristine-view scan (one-shot; must precede any edit)
            EnsureConvoFpOrigin(svu);
            ApplyHeadLook(svu);   // head tracking (NO eye offset - DIBR stereo is in the warped pixels)
            ApplyFov(svu);        // widen the render to fill the headset + publish the submit FOV
            RefreshInverseSlots(svu);   // [INVMAT] keep fog/shaft screen->world matching the edited camera
            ViewCensusNote(svu, true);  // [VIEWCEN]
        }
        return sv;
    }

    // ---- AER: render ONE offset eye per present (mono panel, disparity across TIME, not space). ----
    // Ported from ME2/ME1: build a single full-viewport view (no SBS split), shift the camera by
    // +/-aerHalfEyeUU along camera-right for the currently-armed eye, apply head-look + FOV, then STAMP
    // {eye, seq}. The present loop reads the stamp to capture the fresh eye into a 2-slot history and
    // arm the next eye. NO WriteRect split, NO per-eye view states (that's a stereo-only need).
    if (g_vrEnabled.load(std::memory_order_acquire) &&
        g_vrMode.load(std::memory_order_acquire) == (int)VrMode::Aer && isP1)
    {
        void* sv = g_orig(lp, family, loc, rot, vp, drawer);
        if (sv != nullptr)
        {
            const std::uintptr_t svu = reinterpret_cast<std::uintptr_t>(sv);
            InvMatScanTick(svu);             // [INVMAT] pristine-view scan (one-shot; must precede any edit)
            // [AERSHAKE] eye from THIS build's own seq parity - no cross-thread arm feedback to flap
            const uint64_t mySeq = g_aerStampSeq.load(std::memory_order_relaxed) + 1;
            const int   eye  = static_cast<int>(mySeq & 1ull);                   // 0=L, 1=R
            const float he   = g_aerHalfEyeUU.load(std::memory_order_relaxed);
            const float base = (eye == 0) ? -he : +he;                           // 0=L, 1=R
            const float signedUU = g_aerSwapEyes.load(std::memory_order_relaxed) ? -base : base;
            ApplyEyeOffset(svu, signedUU);
            ApplyHeadLook(svu);              // offset BEFORE head rotation (same order as stereo below)
            ApplyFov(svu);
            RefreshInverseSlots(svu);        // [INVMAT] keep fog/shaft screen->world matching the edited camera
            AerRingSlot& slot = g_aerRing[mySeq % kAerRingN];   // [AERSHAKE] publish: eye first, seq RELEASE
            slot.eye.store(eye, std::memory_order_relaxed);
            slot.seq.store(mySeq, std::memory_order_release);
            g_aerStampEye.store(eye, std::memory_order_relaxed);   // legacy single-slot stamp kept live
            g_aerStampArmed.store(true, std::memory_order_relaxed);
            g_aerStampSeq.store(mySeq, std::memory_order_release); // publish newest (single writer thread)
            g_splitSeq.fetch_add(1, std::memory_order_release);    // keep the shared seq live too (SBS hysteresis compat)
            ViewCensusNote(svu, true);   // [VIEWCEN]
        }
        return sv;
    }

    // ---- MONO: one full view presented flat to both eyes, but the GAME CAMERA still gets head
    // tracking, 6DOF lean and FOV fill exactly like every other mode. Until now Mono (mode 0) had NO
    // branch here at all: it matched none of the four gates above and fell straight through to the
    // bare g_orig() at the bottom, so the picture appeared via the mono-quad presentation while the
    // camera was never touched. Everything that lives in these calls was dead in mono -
    // head tracking, lean, the [INVMAT] stale-inverse fix, and (because PublishGameFov is called
    // INSIDE ApplyFov) the raw-FOV publish that cinematic detection and the XR submit both read.
    // That last one is the harmful part rather than merely missing: [FILLSRC] is not mode-gated, so
    // the engine already built the view WIDE, while nothing published a matching FOV to the submit -
    // mono rendered at one FOV and was declared at another.
    // ME2 hit this exact hole and fixed it the same way on 2026-07-26; ME3 never got the port.
    // No ApplyEyeOffset by definition: mono has no eye separation.
    // [2026-08-14] InvMatScanTick was missing here even after the branch landed, so the [INVMAT] fix
    // was STILL dead in mono: the scan is one-shot and gated on its own view counter (>=300 views),
    // and that counter only advances from branches that call it. In mono it never advanced, the scan
    // never completed, and RefreshInverseSlots early-returned on !g_invScanDone every single frame.
    // Symptom: light shafts / lens flares stay locked to the head forever (they reconstruct world
    // from screen+depth through the stale inverses). In SFR the same thing happens for ~1s at boot -
    // that IS the scan window, two views per present - and then stops. Mono had no such window.
    if (g_vrEnabled.load(std::memory_order_acquire) &&
        g_vrMode.load(std::memory_order_acquire) == (int)VrMode::Mono && isP1)
    {
        void* sv = g_orig(lp, family, loc, rot, vp, drawer);
        if (sv != nullptr)
        {
            const std::uintptr_t svu = reinterpret_cast<std::uintptr_t>(sv);
            InvMatScanTick(svu);         // [INVMAT] pristine-view scan (one-shot; MUST precede any edit)
            EnsureConvoFpOrigin(svu);
            ApplyHeadLook(svu);          // head rotation + positional lean (no eye offset in mono)
            ApplyFov(svu);               // publishes raw/render/submit FOV and widens to fill
            RefreshInverseSlots(svu);    // [INVMAT] keep fog/shaft screen->world matching the edit
            ViewCensusNote(svu, true);   // [VIEWCEN]
        }
        return sv;
    }

    if (g_vrEnabled.load(std::memory_order_acquire) &&
        g_vrMode.load(std::memory_order_acquire) == (int)VrMode::Stereo && isP1)
    {
        WriteRect(lpu, 0.0f, 0.0f, 0.5f, 1.0f);                 // left half
        void* left = g_orig(lp, family, loc, rot, vp, drawer);
        WriteRect(lpu, 0.5f, 0.0f, 0.5f, 1.0f);                 // right half
        void* right = g_orig(lp, family, loc, rot, vp, nullptr);
        WriteRect(lpu, 0.0f, 0.0f, 1.0f, 1.0f);                 // restore full

        // B2a: give EACH eye its OWN persistent FSceneViewState (allocate once, reuse) so motion-blur/
        // temporal effects have valid, private per-eye history. Sharing P1's state corrupts occlusion AND
        // smears temporally: the game keeps repurposing P1's state for its own (mono/menu/HUD) rendering, so
        // an eye that borrows it inherits stale temporal history -> a moving ghost of the level (LE2 left-eye
        // ghost. Both eyes now get dedicated states.
        if (right != nullptr)
        {
            if (g_eyeState == nullptr && g_allocViewState != nullptr)
            {
                void* st = CallAllocViewState();
                if (Readable(st, 0x40)) g_eyeState = st;
                if (!g_eyeStateLogged.exchange(true))
                    ME2VR::Log::Line("[ME3DISC] B2 AllocateViewState(+0x329750)(0) -> " +
                                     Hex(reinterpret_cast<std::uintptr_t>(st)) +
                                     (g_eyeState ? " (eye state ready)" : " (unreadable -> NULL fallback)"));
            }
            SetViewState(reinterpret_cast<std::uintptr_t>(right), g_eyeState);   // null-safe
        }
        if (left != nullptr)
        {
            if (g_eyeStateLeft == nullptr && g_allocViewState != nullptr)
            {
                void* st = CallAllocViewState();
                if (Readable(st, 0x40)) g_eyeStateLeft = st;
                if (!g_eyeStateLeftLogged.exchange(true))
                    ME2VR::Log::Line("[ME3DISC] B2 AllocateViewState LEFT -> " +
                                     Hex(reinterpret_cast<std::uintptr_t>(st)) +
                                     (g_eyeStateLeft ? " (left eye state ready)" : " (unreadable -> NULL fallback)"));
            }
            // Only override once the mod actually has a dedicated state; never force NULL (that itself smears).
            if (g_eyeStateLeft != nullptr) SetViewState(reinterpret_cast<std::uintptr_t>(left), g_eyeStateLeft);
        }

        // B2b: real parallax - shift each view's camera half-IPD along camera-right (left -, right +).
        // Eye offset BEFORE head rotation (body stays stable, doesn't swim with head-look).
        float he = g_halfEyeUU.load(std::memory_order_relaxed);
        if (g_swapEyes.load(std::memory_order_relaxed)) he = -he;
        if (left != nullptr)
        {
            InvMatScanTick(reinterpret_cast<std::uintptr_t>(left));   // [INVMAT] pristine scan (pre-edit)
            ApplyEyeOffset(reinterpret_cast<std::uintptr_t>(left), -he);
            ApplyHeadLook(reinterpret_cast<std::uintptr_t>(left));
            ApplyFov(reinterpret_cast<std::uintptr_t>(left));    // widen-to-fill (or publish game FOV)
            RefreshInverseSlots(reinterpret_cast<std::uintptr_t>(left));   // [INVMAT] after ALL edits
            ViewCensusNote(reinterpret_cast<std::uintptr_t>(left), true);  // [VIEWCEN]
        }
        if (right != nullptr)
        {
            ApplyEyeOffset(reinterpret_cast<std::uintptr_t>(right), +he);
            ApplyHeadLook(reinterpret_cast<std::uintptr_t>(right));
            ApplyFov(reinterpret_cast<std::uintptr_t>(right));
            RefreshInverseSlots(reinterpret_cast<std::uintptr_t>(right));  // [INVMAT] after ALL edits
            ViewCensusNote(reinterpret_cast<std::uintptr_t>(right), true); // [VIEWCEN]
        }

        if (g_splitLogged.load(std::memory_order_acquire) < 3)
        {
            g_splitLogged.fetch_add(1, std::memory_order_acq_rel);
            char buf[256] = {};
            sprintf_s(buf, "[ME3DISC] B split: left=%p right=%p twoDistinct=%d eyeState=%p",
                      left, right, (left && right && left != right) ? 1 : 0, g_eyeState);
            ME2VR::Log::Line(buf);
        }
        g_splitSeq.fetch_add(1, std::memory_order_release);   // mark: this frame produced an SBS pair
        return left;   // hand the engine the left view as the "primary"
    }

    void* sv = g_orig(lp, family, loc, rot, vp, drawer);
    if (sv != nullptr && isP1 && ME2VR::ConvoFp::OwnsCamera())
    {
        const auto svu = reinterpret_cast<std::uintptr_t>(sv);
        EnsureConvoFpOrigin(svu); ApplyHeadLook(svu); ApplyFov(svu); RefreshInverseSlots(svu);
    }
    if (sv != nullptr) ViewCensusNote(reinterpret_cast<std::uintptr_t>(sv), isP1);   // [VIEWCEN] non-P1 + unclassified views

    if (sv != nullptr && g_logged.load(std::memory_order_acquire) < 4)
    {
        float proj[16] = {}, view[16] = {}, origin[3] = {};
        if (ReadView(reinterpret_cast<std::uintptr_t>(sv), proj, view, origin))
        {
            const bool perspective = (proj[11] > 0.99f && proj[11] < 1.01f) &&
                                     (proj[15] > -0.01f && proj[15] < 0.01f) &&
                                     proj[0] > 0.0001f && proj[5] > 0.0001f;
            g_logged.fetch_add(1, std::memory_order_acq_rel);
            char buf[512] = {};
            sprintf_s(buf,
                      "[ME3DISC] CalcSceneView HOOK: this=%p isP1=%d sv=%p perspective=%d "
                      "proj[0]=%.4f proj[5]=%.4f proj[10]=%.4f proj[11]=%.2f proj[14]=%.3f proj[15]=%.2f",
                      lp, isP1 ? 1 : 0, sv, perspective ? 1 : 0,
                      proj[0], proj[5], proj[10], proj[11], proj[14], proj[15]);
            ME2VR::Log::Line(buf);
            char buf2[512] = {};
            sprintf_s(buf2,
                      "[ME3DISC]   ViewOrigin=(%.1f, %.1f, %.1f)  viewRow0=(%.3f %.3f %.3f) [camera-right]",
                      origin[0], origin[1], origin[2], view[0], view[4], view[8]);
            ME2VR::Log::Line(buf2);
            if (g_logged.load() == 1)
            {
                ME2VR::Log::Line(std::string("[ME3DISC]   VERDICT: ") +
                                 (perspective && isP1
                                      ? "CONFIRMED CalcSceneView + FSceneView offsets VALID (Proj@0xD0/View@0x90/Origin@0x320). Stage 4 done."
                                      : "unexpected -- review values above"));
            }
        }
    }
    return sv;
}
}

namespace ME2VR::CalcViewHook
{
void SetFovFill(float hHalfRad, float vHalfRad, bool on) noexcept
{
    g_fovFillH.store(hHalfRad, std::memory_order_relaxed);
    g_fovFillV.store(vHalfRad, std::memory_order_relaxed);
    g_fovFillOn.store(on, std::memory_order_release);
}
void SetCinematic(bool on) noexcept { g_cinematic.store(on, std::memory_order_release); }
// [VRCINE] ME2's VR conversations/cutscenes, ported. Default OFF - opt-in, exactly as ME2 ships.
std::atomic_bool g_cineVrConvo{false};
std::atomic_bool g_cineVrCutscene{false};
std::atomic_bool g_cineVrHeadTracking{true};
std::atomic_bool g_vrCineActive{false};   // derived per frame: a cine is being RENDERED in VR now
bool GetCineVrConvo() noexcept { return g_cineVrConvo.load(std::memory_order_acquire); }
void SetCineVrConvo(bool on) noexcept { g_cineVrConvo.store(on, std::memory_order_release); }
bool GetCineVrCutscene() noexcept { return g_cineVrCutscene.load(std::memory_order_acquire); }
void SetCineVrCutscene(bool on) noexcept { g_cineVrCutscene.store(on, std::memory_order_release); }
bool GetCineVrHeadTracking() noexcept { return g_cineVrHeadTracking.load(std::memory_order_acquire); }
void SetCineVrHeadTracking(bool on) noexcept { g_cineVrHeadTracking.store(on, std::memory_order_release); }
bool GetVrCineActive() noexcept { return g_vrCineActive.load(std::memory_order_acquire); }
bool CorrectWorldMarkerSceneView(void* sceneView) noexcept
{
    // Objective markers are a gameplay-world overlay. Never touch flat menu/cinematic presentation.
    if (sceneView == nullptr || !g_vrEnabled.load(std::memory_order_acquire) ||
        ME2VR::D3DCapture::GetMenuMode() || g_cinematic.load(std::memory_order_acquire) ||
        g_vrCineActive.load(std::memory_order_acquire) ||
        !g_markerProjectionReady.load(std::memory_order_acquire))
        return false;

    float projectionTemplate[16] = {};
    bool coherent = false;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const unsigned a = g_markerProjectionSeq.load(std::memory_order_acquire);
        if ((a & 1u) != 0) continue;
        for (int i = 0; i < 16; ++i)
            projectionTemplate[i] = g_markerRawProjection[i].load(std::memory_order_relaxed);
        const unsigned b = g_markerProjectionSeq.load(std::memory_order_acquire);
        if (a == b && (b & 1u) == 0) { coherent = true; break; }
    }
    if (!coherent) return false;

    const float rawH = g_gameRawFovH.load(std::memory_order_relaxed);
    const float rawV = g_gameRawFovV.load(std::memory_order_relaxed);
    if (rawH < 0.05f || rawV < 0.05f) return false;

    // [MARKERSPACE] FSFXGUISceneView and Scaleform both live in the raw HUD projection. The later
    // [UIRATIO] viewport scales that entire HUD by tan(rawFov)/tan(renderFov), placing raw-projected
    // elements over the widened world. Converting this marker to Prender here applies that ratio
    // twice and leaves it partially head-driven. Only inject headDelta here; keep the result in Praw:
    // VPgui * inv(Praw) * headDelta * Praw. UIRATIO performs the one required raw->render conversion.
    float guiProj[16] = {}, invGuiProj[16] = {};
    for (int i = 0; i < 16; ++i) guiProj[i] = projectionTemplate[i];
    guiProj[0] = 1.0f / tanf(rawH);
    guiProj[5] = 1.0f / tanf(rawV);
    if (!Inverse4x4(guiProj, invGuiProj)) return false;

    __try
    {
        volatile float* dst = reinterpret_cast<volatile float*>(sceneView);
        float guiVp[16] = {}, view[16] = {}, delta[16] = {}, tmp[16] = {}, corrected[16] = {};
        for (int i = 0; i < 16; ++i) guiVp[i] = dst[i];
        Mul4x4(view, guiVp, invGuiProj);

        // Match ApplyHeadPosition with a centre-eye camera. The renderer's per-eye offset must not
        // leak into this zero-disparity HUD marker shared by both eyes.
        if (g_headPosOn.load(std::memory_order_acquire))
        {
            float hx = g_headPosX.load(std::memory_order_relaxed);
            float hy = g_headPosY.load(std::memory_order_relaxed);
            float hz = g_headPosZ.load(std::memory_order_relaxed);
            if (hx < -2.0f) hx = -2.0f; else if (hx > 2.0f) hx = 2.0f;
            if (hy < -2.0f) hy = -2.0f; else if (hy > 2.0f) hy = 2.0f;
            if (hz < -2.0f) hz = -2.0f; else if (hz > 2.0f) hz = 2.0f;
            constexpr float kMetersToUU = 50.0f;
            constexpr float kFwdLeanEmphasis = 3.0f;
            const float w2m = kMetersToUU * g_headPosScale.load(std::memory_order_relaxed);
            const float fwdSign = g_leanInvertFwd.load(std::memory_order_relaxed) ? 1.0f : -1.0f;
            const float dR = hx, dU = hy, dF = fwdSign * hz * kFwdLeanEmphasis;
            const float wx = w2m * (dR * view[0] + dU * view[1] + dF * view[2]);
            const float wy = w2m * (dR * view[4] + dU * view[5] + dF * view[6]);
            const float wz = w2m * (dR * view[8] + dU * view[9] + dF * view[10]);
            view[12] -= wx * view[0] + wy * view[4] + wz * view[8];
            view[13] -= wx * view[1] + wy * view[5] + wz * view[9];
            view[14] -= wx * view[2] + wy * view[6] + wz * view[10];
            dst[16] += wx; dst[17] += wy; dst[18] += wz;
        }

        constexpr float kUUToRad = 6.28318530718f / 65536.0f;
        const float yaw = static_cast<float>(g_headYawUU.load(std::memory_order_relaxed)) * kUUToRad;
        const float pitch = static_cast<float>(g_headPitchUU.load(std::memory_order_relaxed)) * kUUToRad;
        const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
        const float yawM[16] = { cy,0,-sy,0, 0,1,0,0, sy,0,cy,0, 0,0,0,1 };
        const float pitchM[16] = { 1,0,0,0, 0,cp,sp,0, 0,-sp,cp,0, 0,0,0,1 };
        Mul4x4(delta, yawM, pitchM);
        Mul4x4(tmp, view, delta);
        Mul4x4(corrected, tmp, guiProj);
        for (int i = 0; i < 16; ++i) dst[i] = corrected[i];
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

void SetVrCineActive(bool on) noexcept { g_vrCineActive.store(on, std::memory_order_release); }
bool GetCinematic() noexcept { return g_cinematic.load(std::memory_order_acquire); }
void SetConvoFpInvertFacing(bool on) noexcept { g_convoFpInvertFacing.store(on, std::memory_order_relaxed); }
bool GetConvoFpInvertFacing() noexcept { return g_convoFpInvertFacing.load(std::memory_order_relaxed); }
void GetConvoFpCounts(unsigned long long* applies, unsigned long long* skips) noexcept
{
    if (applies) *applies = g_convoFpApplies.load(std::memory_order_relaxed);
    if (skips) *skips = g_convoFpSkips.load(std::memory_order_relaxed);
}
void SetVrEnabled(bool on) noexcept { g_vrEnabled.store(on, std::memory_order_release); }
bool GetVrEnabled() noexcept { return g_vrEnabled.load(std::memory_order_acquire); }
float GetGameRawFovH() noexcept { return g_gameRawFovH.load(std::memory_order_relaxed); }
float GetGameRawFovV() noexcept { return g_gameRawFovV.load(std::memory_order_relaxed); }
float GetGameHalfFovH() noexcept { return g_gameHalfFovH.load(std::memory_order_relaxed); }
float GetGameHalfFovV() noexcept { return g_gameHalfFovV.load(std::memory_order_relaxed); }

uint64_t GetSplitSeq() noexcept { return g_splitSeq.load(std::memory_order_acquire); }

// [HEADAIM] the last head-look angles, for the aim seed ramp (ME2 parity).
int32_t HeadLookYawUU() noexcept { return g_headYawUU.load(std::memory_order_relaxed); }
int32_t HeadLookPitchUU() noexcept { return g_headPitchUU.load(std::memory_order_relaxed); }
// [SFR2] how many times the SFR calcview branch built a view, vs how many replays ran. If the
// replay does not re-enter CalcSceneView, pass 1 renders with pass 0's camera: both eyes
// identical, and raising eye separation just shifts the world sideways instead of adding depth.
// [SFRCAP] true while the current thread is recording the REPLAY pass (pass 1). The pass-0 capture
// keys on this: at pass 1 recording time the backbuffer still holds pass 0's finished image.
bool InReplayPass() noexcept { return t_replay; }
uint64_t GetSfrCalcViews() noexcept { return g_sfrCalcViews.load(std::memory_order_relaxed); }
uint64_t GetP1CalcSeq() noexcept { return g_p1CalcSeq.load(std::memory_order_relaxed); }   // [ENGCINE]
uint64_t GetSfrReplays() noexcept { return g_sfrReplays.load(std::memory_order_relaxed); }
// [FRAMEOWNER]
unsigned long GetDrawThreadId() noexcept { return g_drawThreadId.load(std::memory_order_relaxed); }
void TakeDrawPassStats(unsigned* count, unsigned* p0AvgUs, unsigned* p0MaxUs,
                       unsigned* p1AvgUs, unsigned* p1MaxUs) noexcept
{
    const unsigned n = g_dpCount.exchange(0, std::memory_order_relaxed);
    const uint64_t s0 = g_dpP0SumUs.exchange(0, std::memory_order_relaxed);
    const uint64_t s1 = g_dpP1SumUs.exchange(0, std::memory_order_relaxed);
    if (count) *count = n;
    if (p0AvgUs) *p0AvgUs = n ? static_cast<unsigned>(s0 / n) : 0;
    if (p1AvgUs) *p1AvgUs = n ? static_cast<unsigned>(s1 / n) : 0;
    if (p0MaxUs) *p0MaxUs = g_dpP0MaxUs.exchange(0, std::memory_order_relaxed);
    if (p1MaxUs) *p1MaxUs = g_dpP1MaxUs.exchange(0, std::memory_order_relaxed);
}
void SetHeadLook(int32_t yawUU, int32_t pitchUU, bool enabled) noexcept
{
    g_headYawUU.store(yawUU, std::memory_order_relaxed);
    g_headPitchUU.store(pitchUU, std::memory_order_relaxed);
    g_headLookEnabled.store(enabled, std::memory_order_release);
}
void SetHeadPos(float x, float y, float z) noexcept
{
    g_headPosX.store(x, std::memory_order_relaxed);
    g_headPosY.store(y, std::memory_order_relaxed);
    g_headPosZ.store(z, std::memory_order_relaxed);
}
void SetHeadPosEnabled(bool on) noexcept { g_headPosOn.store(on, std::memory_order_release); }
bool GetHeadPosEnabled() noexcept { return g_headPosOn.load(std::memory_order_acquire); }
void SetHeadPosScale(float s) noexcept { g_headPosScale.store(s, std::memory_order_relaxed); }
float GetHeadPosScale() noexcept { return g_headPosScale.load(std::memory_order_relaxed); }
void SetLeanInvertFwd(bool on) noexcept { g_leanInvertFwd.store(on, std::memory_order_relaxed); }
bool GetLeanInvertFwd() noexcept { return g_leanInvertFwd.load(std::memory_order_relaxed); }

float GetHalfEyeUU() noexcept { return g_halfEyeUU.load(std::memory_order_relaxed); }
void  SetHalfEyeUU(float v) noexcept { g_halfEyeUU.store(v, std::memory_order_relaxed); }
bool  GetSwapEyes() noexcept { return g_swapEyes.load(std::memory_order_relaxed); }
void  SetSwapEyes(bool v) noexcept { g_swapEyes.store(v, std::memory_order_relaxed); }
// [PACE1X] Present cadence for stereo/SFR. Default full: those modes put BOTH eyes in one present, so
// they never needed the half-rate eye-swap cadence AER requires. Ported from ME2 2026-07-28, where the
// half-rate default was capping the game at 60fps while it was natively feeding ~180.
std::atomic_bool g_fullRefreshPacing{true};
bool  GetFullRefreshPacing() noexcept { return g_fullRefreshPacing.load(std::memory_order_relaxed); }
void  SetFullRefreshPacing(bool v) noexcept { g_fullRefreshPacing.store(v, std::memory_order_relaxed); }
bool  GetStereoFramePacing() noexcept { return g_stereoFramePacing.load(std::memory_order_relaxed); }
void  SetStereoFramePacing(bool v) noexcept { g_stereoFramePacing.store(v, std::memory_order_relaxed); }
int   GetStereoFramePacingHz() noexcept { return g_stereoFramePacingHz.load(std::memory_order_relaxed); }
void  SetStereoFramePacingHz(int v) noexcept { g_stereoFramePacingHz.store(v, std::memory_order_relaxed); }
bool  GetInvMatrixFix() noexcept { return g_invFixEnabled.load(std::memory_order_relaxed); }
void  SetInvMatrixFix(bool v) noexcept { g_invFixEnabled.store(v, std::memory_order_relaxed); }

// --- VR mode + AER + DIBR (ported from ME2 2026-07-12) ---
int   GetVrMode() noexcept { return g_vrMode.load(std::memory_order_acquire); }
void  SetVrMode(int m) noexcept
{
    // Log every mode change. The per-mode "frame mode = ..." lines are one-shot (capped at 4), so
    // they only ever record the mode active at startup - a mid-session switch from the Insert menu
    // left no trace at all, which made "was mono actually running?" unanswerable from a log.
    const int prev = g_vrMode.exchange(m, std::memory_order_acq_rel);
    if (prev != m)
    {
        static const char* kNames[] = { "Mono", "Stereo(SBS)", "AER", "DIBR", "SFR" };
        const char* pn = (prev >= 0 && prev <= 4) ? kNames[prev] : "?";
        const char* nn = (m >= 0 && m <= 4) ? kNames[m] : "?";
        ME2VR::Log::Line(std::string("[VRMODE] ") + pn + " -> " + nn +
                         " (mode " + std::to_string(m) + ")");
    }
}

// [SFR] convergence + HUD trims.
float GetSfrConvergence() noexcept { return g_sfrConvergence.load(std::memory_order_relaxed); }
void  SetSfrConvergence(float v) noexcept
{ g_sfrConvergence.store((v < -0.2f) ? -0.2f : (v > 0.2f ? 0.2f : v), std::memory_order_relaxed); }
namespace { inline float ClampScale(float v) noexcept { return (v < 0.2f) ? 0.2f : (v > 1.5f ? 1.5f : v); }
            inline float ClampOff(float v) noexcept { return (v < -0.5f) ? -0.5f : (v > 0.5f ? 0.5f : v); } }
float GetSfrUiScaleX() noexcept { return g_sfrUiScaleX.load(std::memory_order_relaxed); }
void  SetSfrUiScaleX(float v) noexcept { g_sfrUiScaleX.store(ClampScale(v), std::memory_order_relaxed); }
float GetSfrUiScaleY() noexcept { return g_sfrUiScaleY.load(std::memory_order_relaxed); }
void  SetSfrUiScaleY(float v) noexcept { g_sfrUiScaleY.store(ClampScale(v), std::memory_order_relaxed); }
float GetSfrUiOffX() noexcept { return g_sfrUiOffX.load(std::memory_order_relaxed); }
void  SetSfrUiOffX(float v) noexcept { g_sfrUiOffX.store(ClampOff(v), std::memory_order_relaxed); }
float GetSfrUiOffY() noexcept { return g_sfrUiOffY.load(std::memory_order_relaxed); }
void  SetSfrUiOffY(float v) noexcept { g_sfrUiOffY.store(ClampOff(v), std::memory_order_relaxed); }

// [FILL] what this view actually rendered (the UI ratio must compare against THIS, not the submitted FOV).
float GetRenderHalfFovH() noexcept { return g_renderHalfFovH.load(std::memory_order_relaxed); }
float GetRestHalfFovH() noexcept { return g_restHalfFov.load(std::memory_order_relaxed); }
float GetRestHalfFovV() noexcept { return g_restHalfFovV.load(std::memory_order_relaxed); }
float GetRenderHalfFovV() noexcept { return g_renderHalfFovV.load(std::memory_order_relaxed); }
float GetFovFillTargetH() noexcept { return g_fovFillH.load(std::memory_order_relaxed); }
float GetFovFillTargetV() noexcept { return g_fovFillV.load(std::memory_order_relaxed); }

// [POSETAG] / ME2 view-head-look tuning parity.
bool  GetHeadLookUserEnabled() noexcept { return g_headLookUserEnabled.load(std::memory_order_relaxed); }
void  SetHeadLookUserEnabled(bool v) noexcept { g_headLookUserEnabled.store(v, std::memory_order_relaxed); }
float GetLookSensitivity() noexcept { return g_lookSensitivity.load(std::memory_order_relaxed); }
void  SetLookSensitivity(float v) noexcept
{ g_lookSensitivity.store((v < 0.1f) ? 0.1f : (v > 3.0f ? 3.0f : v), std::memory_order_relaxed); }
bool  GetInvertLookYaw() noexcept { return g_invertLookYaw.load(std::memory_order_relaxed); }
void  SetInvertLookYaw(bool v) noexcept { g_invertLookYaw.store(v, std::memory_order_relaxed); }
bool  GetInvertLookPitch() noexcept { return g_invertLookPitch.load(std::memory_order_relaxed); }
void  SetInvertLookPitch(bool v) noexcept { g_invertLookPitch.store(v, std::memory_order_relaxed); }
float GetHeadLookSmoothing() noexcept { return g_headLookSmoothing.load(std::memory_order_relaxed); }
void  SetHeadLookSmoothing(float v) noexcept
{ g_headLookSmoothing.store((v < 0.0f) ? 0.0f : (v > 0.95f ? 0.95f : v), std::memory_order_relaxed); }
float GetPoseTagDelayFrames() noexcept { return g_poseTagDelayFrames.load(std::memory_order_relaxed); }
void  SetPoseTagDelayFrames(float v) noexcept
{ g_poseTagDelayFrames.store((v < 0.0f) ? 0.0f : (v > 3.0f ? 3.0f : v), std::memory_order_relaxed); }
float GetAerHalfEyeUU() noexcept { return g_aerHalfEyeUU.load(std::memory_order_relaxed); }
void  SetAerHalfEyeUU(float v) noexcept { g_aerHalfEyeUU.store(v, std::memory_order_relaxed); }
bool  GetAerSwapEyes() noexcept { return g_aerSwapEyes.load(std::memory_order_relaxed); }
void  SetAerSwapEyes(bool v) noexcept { g_aerSwapEyes.store(v, std::memory_order_relaxed); }
bool  GetAerFramePacing() noexcept { return g_aerFramePacing.load(std::memory_order_relaxed); }
void  SetAerFramePacing(bool v) noexcept { g_aerFramePacing.store(v, std::memory_order_relaxed); }
int   GetAerFramePacingHz() noexcept { return g_aerFramePacingHz.load(std::memory_order_relaxed); }
void  SetAerFramePacingHz(int v) noexcept { g_aerFramePacingHz.store(v, std::memory_order_relaxed); }
float GetDepthWarpGain() noexcept { return g_depthWarpGain.load(std::memory_order_relaxed); }
void  SetDepthWarpGain(float v) noexcept { g_depthWarpGain.store(v, std::memory_order_relaxed); }
float GetDepthWarpConv() noexcept { return g_depthWarpConv.load(std::memory_order_relaxed); }
void  SetDepthWarpConv(float v) noexcept { g_depthWarpConv.store(v, std::memory_order_relaxed); }
bool  GetDepthWarpFlip() noexcept { return g_depthWarpFlip.load(std::memory_order_relaxed); }
void  SetDepthWarpFlip(bool v) noexcept { g_depthWarpFlip.store(v, std::memory_order_relaxed); }
bool  GetDibrAutoConverge() noexcept { return g_dibrAutoConverge.load(std::memory_order_relaxed); }
void  SetDibrAutoConverge(bool v) noexcept { g_dibrAutoConverge.store(v, std::memory_order_relaxed); }
int   GetAerRenderEye() noexcept { return g_aerRenderEye.load(std::memory_order_acquire); }
void  SetAerRenderEye(int e) noexcept { g_aerRenderEye.store(e, std::memory_order_release); }
// [AERSHAKE] Consume the build AFTER lastSeq, oldest-first - one per present, matching swapchain
// delivery order, so the eye label always belongs to the pixels on screen. False = nothing new.
// If the wanted slot was overwritten (consumer fell a full ring behind: mode switch, menu, load),
// resync to the newest build and report it via *resynced.
bool ConsumeAerStamp(unsigned long long lastSeq, unsigned long long* outSeq, int* outEye, bool* resynced) noexcept
{
    *resynced = false;
    const uint64_t newest = g_aerStampSeq.load(std::memory_order_acquire);
    if (newest <= lastSeq) return false;
    uint64_t want = lastSeq + 1;
    const AerRingSlot* slot = &g_aerRing[want % kAerRingN];
    if (slot->seq.load(std::memory_order_acquire) != want)
    {
        want = newest;
        *resynced = true;
        slot = &g_aerRing[want % kAerRingN];
        if (slot->seq.load(std::memory_order_acquire) != want) return false;   // slot mid-write; next present
    }
    *outSeq = want;
    *outEye = slot->eye.load(std::memory_order_relaxed);
    return true;
}
// Combined stamp read - BYTE-IDENTICAL to ME1/ME2: read the dedicated AER seq FIRST with ACQUIRE
// (synchronizes-with the detour's release bump), THEN the eye with RELAXED - the eye you read is the one
// published before that seq generation, so the present loop can never capture into the wrong history slot.
bool GetAerStamp(unsigned long long* seq, int* eye) noexcept
{
    if (!g_aerStampArmed.load(std::memory_order_acquire)) return false;
    const uint64_t s = g_aerStampSeq.load(std::memory_order_acquire);
    const int e = g_aerStampEye.load(std::memory_order_relaxed);
    if (seq) *seq = s;
    if (eye) *eye = e;
    return e >= 0 && e <= 1;
}

void Tick() noexcept
{
    if (g_installed.load(std::memory_order_acquire)) return;

    // Only install once a local player exists (engine is up).
    if (ME2VR::EngineProbe::GetPrimaryLocalPlayer() == 0) return;

    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

    g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (g_base == 0) return;

    g_allocViewState = reinterpret_cast<AllocViewStateFn>(g_base + kAllocateViewStateRva);

    MH_Initialize();   // idempotent; harmless if already initialized
    void* target = reinterpret_cast<void*>(g_base + kCalcSceneViewRva);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&CalcViewDetour),
                      reinterpret_cast<void**>(&g_orig)) == MH_OK &&
        MH_EnableHook(target) == MH_OK)
    {
        g_installed.store(true, std::memory_order_release);
        ME2VR::Log::Line("[ME3DISC] CalcViewHook: installed at MassEffect2.exe+" + Hex(kCalcSceneViewRva));
    }
    else
    {
        ME2VR::Log::Line("[ME3DISC] CalcViewHook: MH_CreateHook/EnableHook FAILED at +" + Hex(kCalcSceneViewRva));
    }

    // [SFR] FViewportClient::Draw hook (the double-render replay). MinHook is already initialized
    // above; this detour is inert unless the SFR mode is selected, so it is always safe to install.
    void* drawTarget = reinterpret_cast<void*>(g_base + kDrawRva);
    if (MH_CreateHook(drawTarget, reinterpret_cast<void*>(&DrawDetour),
                      reinterpret_cast<void**>(&g_origDraw)) == MH_OK &&
        MH_EnableHook(drawTarget) == MH_OK)
    {
        ME2VR::Log::Line("[SFR] Draw hook installed at MassEffect3.exe+" + Hex(kDrawRva));
    }
    else
    {
        g_origDraw = nullptr;
        ME2VR::Log::Line("[SFR] Draw hook MH_CreateHook/EnableHook FAILED at +" + Hex(kDrawRva));
    }
}
}
