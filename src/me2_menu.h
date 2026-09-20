#pragma once

struct IDXGISwapChain;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

// In-headset Insert menu (ImGui -> texture -> head-locked quad), ported from ME1's M8 menu.
// Settings persist to MELE2VR.ini next to MassEffect2.exe. Options are filled in over time.
namespace ME2VR::Menu
{
void Init(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height) noexcept;
void OnPresent(IDXGISwapChain* swapChain) noexcept;   // poll Insert toggle
ID3D11Texture2D* RenderFrame() noexcept;              // returns the menu texture while open, else null
void RenderToBackbuffer(IDXGISwapChain* swapChain) noexcept;  // flat (VR-off) path: draw straight to backbuffer
bool IsOpen() noexcept;
// True only when the Insert overlay itself introduced both PlayerController input locks.
// False when it was opened over a native game menu that already owned those locks.
bool IsSoleInputLockOwner() noexcept;
int  LeftStickMagnitude() noexcept;   // [MOVEFIX] live left-stick push, drives the sprint latch
bool PadSprintHeld() noexcept;        // [FPSTORM] A held = storming in LE3
bool PadLeftShoulderHeld() noexcept;  // [WHEELVR] freshly sampled L1/LB hold = native weapon wheel
int  GetRecenterKey() noexcept;      // rebindable; the poll sites read these instead of constants
int  GetFpToggleKey() noexcept;
bool IsRebindingFpToggle() noexcept;
bool GetMoveFollowsHead() noexcept;   void SetMoveFollowsHead(bool on) noexcept;
bool GetDecoupledPitch() noexcept;    void SetDecoupledPitch(bool on) noexcept;
bool GetDecoupledYaw() noexcept;      void SetDecoupledYaw(bool on) noexcept;

}
