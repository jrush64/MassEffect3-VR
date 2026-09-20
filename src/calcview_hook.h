#pragma once

#include <cstdint>

namespace ME2VR::CalcViewHook
{
// Hook the confirmed ULocalPlayer::CalcSceneView (MassEffect2.exe+0x6CEB70) with the correct
// 6-arg signature (pointer args only -> register-safe), and log the returned FSceneView's
// Proj/View/ViewOrigin to confirm identity + validate the LE1-identical offsets live.
// Call once per Present; installs once, logs a few frames, then stays quiet. Pure observe+forward.
void Tick() noexcept;

// Game's per-eye rendered half-FOV (radians), published from CalcSceneView's projection matrix.
// The OpenXR bridge declares THESE (not the headset FOV) so the stereo image isn't stretched/zoomed.
// Returns 0 until a perspective view has been seen.
float GetGameHalfFovH() noexcept;
float GetGameHalfFovV() noexcept;

// FOV-fill: widen each eye's rendered projection to this target half-FOV (radians) so the stereo image
// fills the headset instead of a narrow center box. Pushed from the XR side each frame; matched on submit.
void SetFovFill(float hHalfRad, float vHalfRad, bool on) noexcept;

// Conversation/cutscene = narrow cinematic FOV -> render mono. Detected XR-side off the RAW FOV.
void SetCinematic(bool on) noexcept;
bool GetCineVrConvo() noexcept;         void SetCineVrConvo(bool on) noexcept;      // [VRCINE]
bool GetCineVrCutscene() noexcept;      void SetCineVrCutscene(bool on) noexcept;
bool GetCineVrHeadTracking() noexcept;  void SetCineVrHeadTracking(bool on) noexcept;
bool GetVrCineActive() noexcept;        void SetVrCineActive(bool on) noexcept;
bool GetCinematic() noexcept;
void SetConvoFpInvertFacing(bool on) noexcept;
bool GetConvoFpInvertFacing() noexcept;
void GetConvoFpCounts(unsigned long long* applies, unsigned long long* skips) noexcept;
void SetVrEnabled(bool on) noexcept;   // master VR switch (OFF = vanilla flat, for FP dev); F4 toggles
bool GetVrEnabled() noexcept;
// RAW game half-FOV (never widened by fill) -> reliable cinematic detection.
float GetGameRawFovH() noexcept;
float GetGameRawFovV() noexcept;
// World-space Scaleform markers project through FSFXGUISceneView, which the game builds before the mod's
// late FSceneView head/FOV edits. Apply the matching clip-space correction to that 0x50-byte scene
// view so objective markers remain attached to their actors instead of following the headset.
bool CorrectWorldMarkerSceneView(void* sceneView) noexcept;


// Drive free head-look: yaw/pitch as UE rotation units (65536 = 360 deg). enabled=false clears it.
void SetHeadLook(int yawUU, int pitchUU, bool enabled) noexcept;
int32_t HeadLookYawUU() noexcept;
int32_t HeadLookPitchUU() noexcept;
// Positional 6DOF (lean): head translation from recenter (XR meters), applied to the camera
// origin scaled to the stereo world-scale. Toggle with SetHeadPosEnabled (default on).
void SetHeadPos(float x, float y, float z) noexcept;
void SetHeadPosEnabled(bool on) noexcept;
bool GetHeadPosEnabled() noexcept;
void SetHeadPosScale(float s) noexcept;   // amplify head movement (1.0 = true 1:1)
float GetHeadPosScale() noexcept;
void SetLeanInvertFwd(bool on) noexcept;
bool GetLeanInvertFwd() noexcept;
// [SFR2] discriminator counters - see calcview_hook.cpp.
bool InReplayPass() noexcept;   // [SFRCAP] recording pass 1 on this thread
uint64_t GetSfrCalcViews() noexcept;
uint64_t GetSfrReplays() noexcept;
uint64_t GetP1CalcSeq() noexcept;   // [ENGCINE] every P1 CalcSceneView call, any branch (bik discriminator)
// [FRAMEOWNER] the thread FViewportClient::Draw runs on (0 until seen), and the per-window wall time
// of pass 0 / pass 1 as that thread sees them (microseconds; resets on read).
unsigned long GetDrawThreadId() noexcept;
void TakeDrawPassStats(unsigned* count, unsigned* p0AvgUs, unsigned* p0MaxUs,
                       unsigned* p1AvgUs, unsigned* p1MaxUs) noexcept;

// Monotonic counter, bumped every frame the SBS split runs. Lets the bridge tell stereo frames
// (gameplay) from mono frames (menus/loading) and present those full-screen to both eyes.
unsigned long long GetSplitSeq() noexcept;

// Stereo knobs (Insert menu): half-eye separation in UE units (~1.6 = 1:1), and eye swap.
float GetHalfEyeUU() noexcept;
void  SetHalfEyeUU(float v) noexcept;
bool  GetSwapEyes() noexcept;
void  SetSwapEyes(bool v) noexcept;

// Display-locked frame pacing (kills the ~60fps-into-120Hz stereo micro-shake). Hz 0 = auto-learn.
bool  GetStereoFramePacing() noexcept;   void SetStereoFramePacing(bool v) noexcept;
int   GetStereoFramePacingHz() noexcept; void SetStereoFramePacingHz(int v) noexcept;
// [PACE1X] true = present every display frame (up to headset refresh). false = every other (half rate).
bool  GetFullRefreshPacing() noexcept;   void SetFullRefreshPacing(bool v) noexcept;

// [INVMAT] stale-inverse-matrix fix (LE1 dark-panel root cause; default ON, self-validating scan).
bool  GetInvMatrixFix() noexcept;        void SetInvMatrixFix(bool v) noexcept;

// --- VR mode selector + AER + DIBR (ported from ME2 2026-07-12) ---
// Mode: 0=Mono, 1=Stereo (default), 2=AER (alternate-eye), 3=DIBR (depth-warp).
int   GetVrMode() noexcept;              void SetVrMode(int m) noexcept;
// [SFR] convergence (applied to each eye IMAGE at submit) + HUD trims on top of the [UIRATIO] match.
float GetSfrConvergence() noexcept;      void SetSfrConvergence(float v) noexcept;
float GetSfrUiScaleX() noexcept;         void SetSfrUiScaleX(float v) noexcept;
float GetSfrUiScaleY() noexcept;         void SetSfrUiScaleY(float v) noexcept;
float GetSfrUiOffX() noexcept;           void SetSfrUiOffX(float v) noexcept;
float GetSfrUiOffY() noexcept;           void SetSfrUiOffY(float v) noexcept;
float GetRenderHalfFovH() noexcept;      // [FILL] what was actually rendered (for the UI ratio)
float GetRenderHalfFovV() noexcept;
float GetRestHalfFovH() noexcept;        // gameplay rest projection, available without a HUD draw
float GetRestHalfFovV() noexcept;
float GetFovFillTargetH() noexcept;      // headset fill target, independent of current cine/zoom submit
float GetFovFillTargetV() noexcept;
// View head-look controls and pose-tag smoothness (ME2 parity).
bool  GetHeadLookUserEnabled() noexcept;   void SetHeadLookUserEnabled(bool v) noexcept;
float GetLookSensitivity() noexcept;       void SetLookSensitivity(float v) noexcept;
bool  GetInvertLookYaw() noexcept;         void SetInvertLookYaw(bool v) noexcept;
bool  GetInvertLookPitch() noexcept;       void SetInvertLookPitch(bool v) noexcept;
float GetHeadLookSmoothing() noexcept;   void SetHeadLookSmoothing(float v) noexcept;
float GetPoseTagDelayFrames() noexcept;  void SetPoseTagDelayFrames(float v) noexcept;
float GetAerHalfEyeUU() noexcept;        void SetAerHalfEyeUU(float v) noexcept;   // AER IPD/scale (uu)
bool  GetAerSwapEyes() noexcept;         void SetAerSwapEyes(bool v) noexcept;     // invert AER depth
bool  GetAerFramePacing() noexcept;      void SetAerFramePacing(bool v) noexcept;  // display-locked pacing
int   GetAerFramePacingHz() noexcept;    void SetAerFramePacingHz(int v) noexcept; // 0 = auto-learn
// DIBR warp tunables (pushed to the d3d_capture warp shader each frame).
float GetDepthWarpGain() noexcept;       void SetDepthWarpGain(float v) noexcept;
float GetDepthWarpConv() noexcept;       void SetDepthWarpConv(float v) noexcept;
bool  GetDepthWarpFlip() noexcept;       void SetDepthWarpFlip(bool v) noexcept;
bool  GetDibrAutoConverge() noexcept;    void SetDibrAutoConverge(bool v) noexcept;
// AER render<->present eye handshake: present arms the next eye; the detour stamps the eye it built.
int  GetAerRenderEye() noexcept;         void SetAerRenderEye(int e) noexcept;
bool GetAerStamp(unsigned long long* seq, int* eye) noexcept;
// [AERSHAKE] FIFO stamp consume: oldest build after lastSeq, one per present - label matches pixels.
bool ConsumeAerStamp(unsigned long long lastSeq, unsigned long long* outSeq, int* outEye, bool* resynced) noexcept;
}
