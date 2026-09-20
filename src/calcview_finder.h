#pragma once

namespace ME2VR::CalcViewFinder
{
// Find ULocalPlayer::CalcSceneView by enumerating the CALL targets inside FViewportClient::Draw
// (and one level into its callees), then cycle-hooking each candidate and validating it:
//   - called with arg1 (rcx) == the live P1 ULocalPlayer, AND
//   - returns a pointer whose +0xD0 looks like a perspective projection matrix.
// Drive it once per Present. Self-latches once CalcSceneView is identified. SEH/guarded.
void Tick() noexcept;
}
