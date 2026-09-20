# Performance - what to expect, and how to minimize the cost

Release notes section. Measured on ME3 LE at 4096x2304 render on a high-end desktop GPU.

## The short version

Frame rate depends on **where you are**, not on your GPU.

| Where | Typical frame time | Rate |
|---|---|---|
| Missions, corridors, combat | 7-10 ms | at the display's limit |
| Citadel hubs, crowded social areas | 17-25 ms | 40-55 fps |

If you only change one thing, **set your headset to 90 Hz**. The hubs land near 45 fps, which is
exactly half of 90, so the runtime's reprojection gets a clean 2:1 cadence and the drop reads as
"slightly soft" instead of judder. At 120 Hz the same frames land on an uneven 3:8 pattern and feel
far worse than the numbers suggest. Head tracking is sampled once per rendered frame, so anything
that raises frame rate also makes tracking feel tighter - a hub that "feels laggy" is the frame rate,
not a separate tracking problem.

## Why the hubs are expensive

Stereo means the scene is rendered twice per frame, once per eye. In a hub the game issues around
**7,000 draw calls per frame**, each with its own material and constant setup, and every one of those
has to be re-issued for the second eye.

Measured breakdown of the render thread during a 25 ms hub frame:

| Component | Share |
|---|---|
| D3D11 runtime (per-call cost of issuing draws) | ~52% |
| Game's own render logic | ~21% |
| Locks / memory / OS | ~15% |
| GPU driver | ~5% |
| **The VR mod's own code** | **~1%** |

The GPU sits at roughly 25% utilization the whole time. This is a CPU submission cost, not a
rendering-power cost - which is why a faster graphics card does not help, and why lowering the render
resolution does not either.

**This cannot be fixed inside the mod.** The cost is the graphics API's per-call overhead multiplied
by the game's draw count, and stereo requires the calls twice. What follows are the levers that do
work, in the order worth trying.

## What actually helps

### 1. Headset at 90 Hz (biggest perceived gain, no quality loss)

Set the refresh rate in your streaming/runtime app (Virtual Desktop, Steam Link, Oculus/Meta app)
to **90 Hz**. Missions still run at the cap; hubs get a stable half-rate cadence instead of judder.
120 Hz is only worth it if you spend most of your time in missions.

### 2. Render resolution - only for GPU-limited setups

`Insert` menu → **Render resolution**. Restart to apply.

On a high-end GPU this changes **nothing** in the hubs, because the hubs are CPU-bound. Lower it only
if you are on weaker hardware and see the GPU pegged. Raising it is nearly free in the hubs and costs
you in missions - so if your card has headroom, a higher setting buys sharpness where it matters.

### 3. Game settings that cut draw calls

These reduce the number of things the engine draws, which is the actual bottleneck. Edit
`…/Game/ME3/BioGame/Config/GamerSettings.ini` under `[SystemSettings]` with the game closed. Each is
a visual trade - try one at a time and keep what you don't miss.

| Key | Effect |
|---|---|
| `DynamicShadows=False` | Already off in this mod's setup; largest single saving. |
| `LightEnvironmentShadows=False` | Drops per-character shadow projection. Helps most in crowds. |
| `CompositeDynamicLights=False` | Fewer per-light passes on characters. |
| `DetailMode=1` | Removes low-priority clutter objects. |
| `SkeletalMeshLODBias=1` | Characters drop to a lower LOD sooner. Visible on nearby faces. |
| `ParticleLODBias=1` | Cheaper particle systems. |
| `MaxCharacterCinematicLightingPasses=1` | Fewer lighting passes in dialogue. |

The mod rewrites `ResX`/`ResY` in this file to match your chosen render resolution - leave those
alone, and don't be surprised to see them change between sessions.

### 4. Alternate-eye mode (last resort, has a visual cost)

`Insert` menu → **Render mode → AER (alternate-eye)**. This renders one eye per frame instead of
both, roughly halving the submission work. It is not free: each eye updates at half rate, which some
people see as shimmer or ghosting on moving objects, and depth is reconstructed from the previous
frame. Stereo (the default) is the better experience everywhere the frame rate allows it. Consider
AER only if you spend a lot of time in the hubs and the judder bothers you more than the shimmer.

### 5. Ordinary system hygiene

Close anything else using the GPU or streaming from it. Keep `Diagnostics=0` in `MELE3VR.ini` (the
default) - turning it on enables profiling probes, including one that samples the render thread
250 times a second, and it will cost you frames.

## What does not help

- **A faster GPU or lower resolution in the hubs.** The GPU is at ~25% there.
- **Turning off mod features.** All mod code together measures ~1% of the render thread; there is no
  toggle in the menu that buys back meaningful time.
- **Frame-rate caps or the display-locked pacing toggle.** These change *when* frames are presented,
  not how long they take to build.

## Reading it yourself

The log at `%LOCALAPPDATA%\MELEVR\ME3PassiveDxgiProbe.log` writes two lines
every 5 seconds:

```
[FRAMETIME]  presents=… avg=…ms | game=… wait=… mod=… | draws/present=…
[FRAMEOWNER] rt=…% gt=…% | gpu3d=…% | hooks/present: draw=…ms bind=…ms | …
```

- `avg` is your real frame time; `draws/present` is the draw count driving it.
- `rt=100%` with `gpu3d` well below means CPU-bound submission - the hub case described above.
- `gpu3d` near 100% would mean the opposite: lower the render resolution.
- `mod` and `hooks/present` are the mod's own cost. Expect ~0.1-0.5 ms. If you ever see it in the
  multiple-millisecond range, that is a bug worth reporting with the log.
