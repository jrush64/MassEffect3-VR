#pragma once

namespace ME2VR::Me2Xr
{
// [LINKFOV] Quest Link image fix (ini QuestFovMatch).
// [COMFORT] panel placement.
float GetMenuScreenDist() noexcept;  void SetMenuScreenDist(float m) noexcept;
float GetMenuScreenSize() noexcept;  void SetMenuScreenSize(float m) noexcept;
float GetCineScreenZoom() noexcept; void SetCineScreenZoom(float z) noexcept;   // [VRCINE]
float GetMenuPanelDist() noexcept;   void SetMenuPanelDist(float m) noexcept;
float GetMenuPanelSize() noexcept;   void SetMenuPanelSize(float m) noexcept;
float GetMenuPanelOffX() noexcept;   void SetMenuPanelOffX(float m) noexcept;
float GetMenuPanelOffY() noexcept;   void SetMenuPanelOffY(float m) noexcept;
bool  GetVrFovFill() noexcept;  void SetVrFovFill(bool on) noexcept;
float GetVrFillH() noexcept;    void SetVrFillH(float v) noexcept;
float GetVrFillV() noexcept;    void SetVrFillV(float v) noexcept;
void SetQuestFovMatch(bool on) noexcept;
bool GetQuestFovMatch() noexcept;
bool IsOculusRuntime() noexcept;
// Milestone A1: bring up OpenXR on the GAME's D3D11 device and create a session (log-only, no
// submit). Answers the one real Milestone-A risk: does ME2's own device bind to xrCreateSession?
// Call once per Present; makes a single attempt once the game device is available, then latches.
void Tick() noexcept;

// Re-origin the head-look reference to the current head pose (call from a future recenter hotkey).
void Recenter() noexcept;

// [FRAMETIME] microseconds the last RunFrame spent blocked inside xrWaitFrame (the runtime's own
// pacing), so the present-hook timing can separate "waiting for the headset" from mod work.
unsigned LastWaitFrameUs() noexcept;
// [XRSUBMIT] windowed RunFrame wall-time stats (xrWaitFrame included - subtract [FRAMETIME]'s
// own windowed wait sum/max to isolate the submit-side cost). Resets on read.
void TakeXrSubmitStats(unsigned* count, unsigned* sumUs, unsigned* maxUs) noexcept;
// [POSEHB] pose-path execution counters (pushes/seeds/freezes since last call). Resets on read.
void TakePoseStats(unsigned* pushes, unsigned* seeds, unsigned* freezes) noexcept;
}
