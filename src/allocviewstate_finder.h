#pragma once

namespace ME2VR::AllocViewStateFinder
{
// Static, read-only xref scan (no hooking): find `mov [reg+0x46C], rax` (a store into
// ULocalPlayer::ViewState) and report the CALL immediately preceding it -- that target is
// ULocalPlayer::AllocateViewState (`ViewState = AllocateViewState()`). Self-latches after one scan.
void TryFindOnce() noexcept;
}
