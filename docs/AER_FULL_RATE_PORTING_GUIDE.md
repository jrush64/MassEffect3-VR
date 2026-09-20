# Making AER worth using: the full-rate change, and how to port it to ME2

What was wrong with AER, the one-line fix, why the thing it was guarding no longer exists, and the
exact steps to do the same in ME2. Written 2026-08-20 from the ME3 change.

---

## 1. What AER is, and why it should be fast

AER (alternate-eye rendering) renders **one eye per present** and alternates. The other eye is the
previous present's image, held in a two-slot history bank and submitted alongside the fresh one, each
slot tagged with the head pose it was actually rendered at.

That matters because of what was measured on ME3 (see `PERFORMANCE.md`): the Citadel hubs are
**CPU-bound on D3D11 draw submission**, ~7,000 draws a frame, and stereo (SFR) re-issues every one of
them for the second eye. The render thread profile in a 25 ms hub frame was `d3d11.dll 52% / game exe
21% / ntdll 14% / driver 5% / the mod 1%`, with the GPU sitting at ~25%.

So the bottleneck is *the number of draw calls submitted per frame*. AER halves it by construction:
one view built and submitted per present instead of two. That is the same idea as PureDark's **AFW**
(Alternate Frame Warping), which reports 60-80% gains for exactly this reason.

## 2. The bug: AER was hard-clamped to half the display rate

In ME3 (and ME2, verbatim - this came over in the port), the pacing call looked like this:

```cpp
const bool wantPace = (aerMode && GetAerFramePacing()) ||
                      ((stereoMode || sfrMode) && GetStereoFramePacing());
if (wantPace) PaceDisplayLocked(!aerMode);   // <-- AER always passed false
```

and inside:

```cpp
void PaceDisplayLocked(bool allow1x) noexcept
{
    const bool full = allow1x && GetFullRefreshPacing();
    const double target = full ? g_displayPeriodSec : 2.0 * g_displayPeriodSec;
    ...spin until `target` has elapsed since the last present...
}
```

`allow1x = !aerMode` meant AER could **never** reach the `full` branch. It always paced to
`2.0 * displayPeriod`, i.e. half the headset refresh.

**Do the arithmetic on what that costs.** At a 90 Hz headset:

| | presents/sec | each eye refreshes at |
|---|---|---|
| AER, old forced half-rate | 45 | **22.5 Hz** |
| AER, full rate | 90 | **45 Hz** |
| SFR/Stereo (both eyes every present) | engine-bound, ~45 in hubs | 45 Hz |

Half-rate AER gave each eye **22.5 Hz** while costing the engine one view per present. It threw away
the entire performance win and looked terrible doing it - the reported experience was "so slow, and
doesn't run well". At 120 Hz it was 60 presents / 30 Hz per eye, which is why it was merely bad rather
than unusable there.

Full-rate AER at 90 Hz gives each eye 45 Hz - **the same per-eye rate as stereo in the hubs** - while
the engine only pays for one view per frame.

## 3. Why the clamp existed, and why it is now dead code

The clamp was not arbitrary. The comment on `PaceDisplayLocked` records the original reasoning:

> Deliberately a FIXED user choice, not adaptive: ME2 tried choosing automatically from measured
> frame time and it oscillated, because pacing to half lets the game coast (looks fast -> upgrade)
> while pacing to full loads it (looks slow -> downgrade). The measurement is changed by the
> decision, so no threshold fixes that loop. AER always gets half.

That is a real feedback loop, and it was a real bug. **But it was a bug in the refresh-rate/cadence
*learner*, not in AER.** The learner has since been replaced: `UpdatePaceWarmup` now takes the refresh
from `XR_FB_display_refresh_rate` when the runtime offers it, and locks it **once per session**:

```
[PACE] cadence: runtime reports 90Hz -> stable session lock (no frame-delta guess)
```

Once the period is locked from the runtime and never re-derived from measured frame time, the
measurement can no longer be changed by the decision, so the oscillation cannot form. The clamp was
guarding a hazard that no longer exists - it just outlived its reason and sat in a mode nobody was
testing while all the recent work went into the stereo path.

There is a second, weaker justification worth naming and dismissing: "the eye-swap needs half-rate
cadence or it flickers." What the eye swap actually needs is **regularity**, not slowness. At exactly
the display rate the alternating eyes land on alternate vsyncs and each eye gets a steady
`display / 2` - which is regular. `AerFramePacing` (the display-lock itself) is what provides that and
stays on; only the forced `2x` multiplier goes.

## 4. The ME3 change (all of it)

Three edits.

**a) `me2_xr.cpp`, in `RunFrame()`** - stop forcing AER to half:

```cpp
- if (wantPace) PaceDisplayLocked(!aerMode);
+ if (wantPace) PaceDisplayLocked(true);
```

**b) `me2_xr.cpp`** - update the `PaceDisplayLocked` comment so the next reader knows the clamp was
removed deliberately and why (the oscillation hazard is gone, not forgotten).

**c) `me2_menu.cpp`, in the AER block** - expose the knob that now matters for AER, reusing the same
setting stereo already uses so there is no new ini key:

```cpp
bool afr = ME2VR::CalcViewHook::GetFullRefreshPacing();
if (ImGui::Checkbox("Run at full frame rate##aer", &afr))
    ME2VR::CalcViewHook::SetFullRefreshPacing(afr);
```

`FullRefreshPacing` was already loaded/saved from the ini and already clamped, so nothing else needed
touching. Users who had it on for stereo get full-rate AER automatically.

## 5. Porting it to ME2

ME2's code is the origin of this pattern, so the shapes match closely.

1. **Confirm ME2's cadence lock is the session-locked kind.** Look at `UpdatePaceWarmup` in
   `me2_xr.cpp` and check the log for a `[PACE] cadence: runtime reports NNHz -> stable session lock`
   line. If ME2 still re-derives the period from measured frame deltas continuously, **fix that
   first** or the oscillation the clamp was guarding will come back. This is the one real
   prerequisite; everything else is mechanical.
2. **Find the pacing call** in ME2's `RunFrame()`. It is the same `PaceDisplayLockedAer(...)` /
   `PaceDisplayLocked(!aerMode)` shape. Change the AER argument so `allow1x` is true.
3. **Add the "Run at full frame rate" checkbox** to ME2's AER menu block (`me2_menu.cpp`), bound to
   `GetFullRefreshPacing` / `SetFullRefreshPacing`, with an `##aer` id suffix so it does not collide
   with the stereo one.
4. **Do not change** `AerFramePacing` (the display-lock toggle) or `AerFramePacingHz`. Those stay.
5. **Rebuild, then verify from the log, not from the file on disk** (see section 7).

Expected result on ME2: since ME2 is engine-bound at ~60 fps rather than submission-bound the way
ME3's hubs are, the win will be smaller in the places ME2 already runs well, and largest wherever
ME2's draw count spikes. The per-eye rate improvement (halving the eye refresh interval) applies
everywhere regardless.

## 6. What full-rate AER does and does not fix

**Does:** halves per-frame draw submission, which is the measured bottleneck in crowded areas. Raises
head-tracking sample rate to the present rate (tracking feel is tied to present rate, so this alone
is noticeable).

**Does not:** change anything about how a *single* eye is rendered. If a scene is GPU-bound, AER does
not help - check `[FRAMEOWNER]`'s `gpu3d` before assuming.

**Known trade-offs, inherent to the technique** (PureDark documents the same ones for AFW):
- One eye is always one present old. Fast-moving objects can shimmer or show slight
  disparity error. Most of the time it is not noticeable; sometimes it is.
- View-dependent effects (volumetrics, specular, screen-space reflections) are computed for the eye
  that was rendered, so they can disagree between eyes.
- Extra VRAM for the history bank: two full-backbuffer textures. At 6144x3456 that is not trivial;
  watch it if pushing the resolution ladder.

**The next refinement, if wanted:** PureDark's AFW warps the stale eye using depth rather than
submitting it as-is with a pose tag. The machinery for that already exists in this codebase in DIBR
mode (depth capture at `ClearDSV`, the warp shader). Combining them - AER's alternation with DIBR's
depth warp for the held eye - is the strongest version of this idea and is not yet built.

## 7. Verification - read the log, do not trust the file on disk

A session on 2026-08-19 produced **zero** log lines from code that was verifiably present in the
deployed binary (byte-searched, strings confirmed). That has not been explained, and it cost a wasted
test cycle and a wrong conclusion. The rule now: **a build proves itself in the log.**

Check, in order:

1. `[BUILD] me2_xr compiled <date> <time> (... AERFULL)` at bring-up - proves which build is running.
2. `[PACE] cadence: runtime reports NNHz -> stable session lock` - proves the period is locked from
   the runtime, which is the precondition for removing the clamp.
3. `[AERHZ] captureHz=...` - the per-eye capture rate. At 90 Hz full-rate this should approach ~90
   captures/sec total, versus ~45 under the old clamp. Also watch `sameEyeTwice` and `seqGapBad`,
   which should stay at 0; nonzero means the alternation is slipping.
4. `[FRAMETIME] presents=N` over a 5-second window in a heavy area - compare against the same spot in
   stereo. This is the actual answer to "did it help".
5. `[FRAMEOWNER] gpu3d=...` - if the GPU is now the wall instead of the CPU, AER did its job and the
   next lever is render resolution.

## 8. Files touched (ME3, for reference when diffing)

| File | Change |
|---|---|
| `src/M0PassiveDxgiProbe/me2_xr.cpp` | `PaceDisplayLocked(true)` in `RunFrame`; `[PACE1X]` comment rewritten; `[BUILD]` stamp gains `AERFULL` |
| `src/M0PassiveDxgiProbe/me2_menu.cpp` | "Run at full frame rate##aer" checkbox in the AER block |

No ini schema change. No new settings. `FullRefreshPacing` is shared with stereo and was already
persisted.
