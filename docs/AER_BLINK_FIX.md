# AER Blink Fix ([AERBLINK], 2026-08-21)

Fixes the intermittent blink/hitch in AER mode. Written for porting to ME2
(and ME1 if its AER shows the same symptom).

## The symptom

AER ran well after [AERFULL] (full display-rate pacing), but the headset showed brief
blinks or hitches at random moments, roughly once every second or two. Not motion shimmer,
not whole-view flashing, not mode flapping: single short blinks.

## How it was diagnosed

Telemetry first, theory second. Three log blocks lined up:

1. `[AERHZ]` showed the eye alternation itself was healthy: captureHz ~118-120 against a
   120Hz display, `sameEyeTwice=0` in every window. The AER handshake was NOT the problem,
   so anything touching the stamp/eye-toggle logic would have been a wasted swing.
2. `[PACE2]` showed presents arriving late a few times per second: `maxMs` spikes of 29,
   30, 41, 75ms against an 8.33ms target. A 75ms stall is nine missed display frames. Each
   of those stalls is one visible blink: the compositor reprojects the last pair (so
   rotation stays locked), then the world jumps the missed time on resume.
3. `[XRSUBMIT]` (RunFrame wall time minus xrWaitFrame) had max spikes of 18-40ms. That
   placed a share of the stall INSIDE the mod's own submit path, not just in the game.

## The root cause

`SubmitAerFrame` did three full-size (5120x2880) copies and two OpenXR swapchain
acquire/wait/release cycles on every present:

- one `CopyResource` capturing the backbuffer into the fresh eye's history slot,
- one acquire/wait/copy/release filling the LEFT eye swapchain from history,
- one acquire/wait/copy/release filling the RIGHT eye swapchain from history.

Two of those three copies re-uploaded content that had not changed since the previous
present - the stale eye's history is by definition untouched between its own captures.

The copies themselves are cheap. The waits are not: `xrWaitSwapchainImage` hands control
to the runtime's compositor, and on VDXR (Virtual Desktop, streaming over the network)
that call can block for tens of milliseconds when the encoder or network hiccups. The
present hook runs ON the game's render thread, so every blocked wait stalls the engine
itself - the game misses vsyncs it would otherwise have made. Two wait points per present
at 120Hz is 240 chances per second for a compositor hiccup to become an engine stall.

## The fix

The OpenXR spec guarantees that a composition layer references a swapchain's most recently
RELEASED image; a swapchain that is not re-acquired keeps presenting what it last held.
Almost everything AER was doing per present was therefore unnecessary:

- The STALE eye needs no acquire, no wait, and no copy. Its swapchain already holds the
  right image; only its pose tag (already stored per slot) matters at submit.
- A present with NO fresh render (stamp seq unchanged) needs no swapchain work at all.
- Only the freshly captured eye gets one acquire/copy/release per present.

Common path: 3 copies + 2 waits -> 2 copies + 1 wait. Idle path: -> zero.

Two safeguards make this unable to present garbage:

1. A per-eye `g_aerEyeFilled` flag records whether that eye's SWAPCHAIN has ever held real
   stereo content. It is cleared by `ResetAerHistory` (mode leave, resolution change). Any
   present where an eye's history is valid but its swapchain is unfilled does a catch-up
   copy. This covers bootstrap (mono frames deliberately do not set the flag, so the first
   real stereo pair overwrites them), AER re-entry after SFR/mono used the same swapchains,
   and history recreation.
2. `CopyTextureToEyeFullFrame` now returns whether the copy actually landed. It used to
   return true even when `xrWaitSwapchainImage` failed and nothing was copied, which would
   have marked an eye "filled" on a copy that never happened.

## What this does NOT fix

- The game's own streaming/loading hitches (`game=` max spikes in `[FRAMETIME]`). Those
  stall every mode and every renderer; no submit-path change can remove them.
- The occasional `seqGapBad` event (engine built two views between two presents, one
  render lost). Harmless one-interval staleness; counted in `[AERHZ]`, left alone.

## How to judge it in the field

Same-length AER session, then compare:

- Blinks gone or much rarer in the headset.
- `[XRSUBMIT] max` should drop well below the old 18-40ms spikes.
- `[PACE2] maxMs` spikes should shrink toward the target (stalls that remain should
  correlate with `game=` spikes in `[FRAMETIME]`, i.e. the game's own hitches).

If blinks persist while `[XRSUBMIT] max` is now small, the remaining stall is the game's
own frame drops, not the submit path - stop looking here.

## Porting notes (ME2)

ME2's `SubmitAerFrame` is the same shape (ME3's was ported from ME1 via ME2). The port is:

1. Add `g_aerEyeFilled[2]`, clear it in `ResetAerHistory`.
2. Make the eye-copy helper return whether the copy landed.
3. Replace the both-eyes-every-present block with: fresh eye only, catch-up for unfilled
   eyes, bootstrap unchanged and never setting the filled flag.

ME2 also submits through the same swapchains from other modes - confirm its equivalent of
the mode-leave reset clears the filled flags, or re-entry will trust stale content.
