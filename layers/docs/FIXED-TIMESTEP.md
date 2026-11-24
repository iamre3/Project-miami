# Fixed-timestep physics decoupling

## The problem

A title that steps its physics once per frame event has its physics
rate tied to its frame rate: at 30 FPS physics runs at half speed,
at 120 FPS it runs at double speed. The layer already paces frame
events at 16.7 ms (phase 3), but pacing alone does not decouple
anything — a slow render loop still means fewer physics steps per
second of wall time.

## The mechanism (Gaffer on Games, "Fix Your Timestep")

Three pieces, all in `src/fixed_timestep.cpp` (`include/fixed_timestep.h`):

1. **Accumulator.** Each pump adds the real elapsed time (QPC) since
   the last pump to an accumulator.
2. **Fixed steps.** While the accumulator holds at least one step
   (`dt = 1/rateHz`, default 60 Hz), the registered physics-step
   function is invoked once per step and `dt` is subtracted. A frame
   that took 33.3 ms steps physics twice; a frame that took 8.3 ms
   steps it zero times on most frames and once every other frame.
   Over any wall-clock interval the step count is the interval times
   the rate, independent of the render rate.
3. **Interpolation.** After stepping, `alpha = accumulator / dt` in
   `[0, 1)`. The renderer draws
   `prev + (curr - prev) * alpha`, where prev/curr are the physics
   states before/after the most recent step. Motion stays smooth
   when the render rate is not a multiple of the physics rate.

**Spiral-of-death guard.** If one pump's elapsed time needs more
than `maxStepsPerFrame` (default 5) steps, only 5 are taken and the
rest is dropped (counted in `stats.droppedSeconds`). After a hitch
the simulation does NOT fast-forward to catch up — it resumes at
the fixed rate from the current time. A game that tried to catch up
would take longer and longer per frame and never recover.

**Honest bound.** The rendered state is the interpolation between
the previous and current physics states, which trails wall-clock
time by exactly one fixed step (16.7 ms at 60 Hz). That lag is
constant, so motion is smooth; the alternative (extrapolating one
step ahead) would be exact but unstable. The proof guest asserts
the rendered position stays within one step of the analytic
position.

## How the pump is driven

Two paths — a title uses one per frame, not both:

- **Direct:** the title calls `ScFixedTimestep_Pump()` once per
  render-loop iteration. The proof guest's 30/120 FPS scenarios use
  this path.
- **Frame-event hook:** `HleWaitFrameEventX` calls the pump after
  the wait completes, whenever a step callback is registered. A
  title that just waits on frame events gets fixed-rate physics
  with no code change on its side. The pump does NOT run on the
  early "no such frame scheduled" return — a wait for a frame that
  was never scheduled is not a frame boundary. The proof guest's
  `ft_hook` stage proves this path: physics steps happen inside
  `WaitFrameEventX` with no direct `Pump` call.

While no callback is registered the pump is a no-op that
accumulates no time (the baseline is reset), so the hook is
zero-cost for titles that never register a step function, and all
existing guests are unaffected.

**The first pump after Init sets the QPC baseline and steps 0
times.** A title should prime the pump at loop start (or accept
that the first frame steps 0 times — the accumulator self-corrects
from the second frame on, since every later interval is counted).

## Why no separate physics thread

The physics state is the game's memory. Stepping it from another
thread would race the game's own access to that state, and the
layer cannot know the game's synchronization. The pump therefore
runs at the game's frame boundary, on the game's thread — the
decoupling is in the accumulator, not in threading. A separate
physics thread is the game's own architectural choice, not
something a compat layer can impose.

## Launch setting

`SCARLETT_PHYSICS_HZ` (environment variable, read at Init when the
rate argument is 0): a number in Hz, 1..1000. Unset / unparsable /
out of range → 60 Hz. Same convention as `SCARLETT_RT_MODE`.

## The real-game hook point (exe-dependent)

The mechanism above is built and proven against a synthetic guest.
What it does NOT yet know is the real title's physics-step
function address — that is exe-dependent and unknowable until the
exe lands (noon Nov 23):

- The function to hook is the one the title's main loop calls once
  per frame event — the call that advances the world state by one
  frame's worth of simulation. It is the function reached after a
  `WaitFrameEventX` return in the main loop, before the next
  `ScheduleFrameEventX`.
- How to find it once the exe is in hand: run the title under the
  layer with the frame-event log on, take the return address of the
  first call after `WaitFrameEventX` returns in the main loop, and
  confirm it advances world state (e.g. a character/object position
  changes by one step's worth per call). The layer then routes that
  function through `ScFixedTimestep_SetStepCallback` — either by
  patching its call site to the pump, or by registering it as the
  step function and letting the `WaitFrameEventX` hook drive it.
- Until that address is known, no real-title physics decoupling can
  be verified — the synthetic guest proves the mechanism, not the
  hook. This is the same class of unknown as the real ordinals and
  the true `D3D12XboxCreateDevice` signature: the layer is built to
  work, the title-specific binding waits on the exe.

## Interaction with frame batching

Frame batching (optimization item 9, `docs/OPTIMIZATION.md`) changes
how command lists are submitted to the GPU; the fixed timestep
changes when physics steps. They are independent: batching does not
move the physics clock, and the timestep does not reorder draws.
Both read the same frame pacing, and neither may reorder the
game's draws.

## Verification

Proof guest `tests/guest_physics.cpp`, stages `ft_*`, run by
`build.bat` (RC7) alongside the other six guests. The simulated
physics is a point at constant velocity v = 10 units/s, so the
analytic position at wall time t is x = v * t.

| Stage | What it proves |
|---|---|
| `ft_runtime` | runtime init |
| `ft_env` | `SCARLETT_PHYSICS_HZ=120` is honored (rate 120 Hz, maxSteps 5) |
| `ft_naive_tied` | the problem: one step per render frame at 30 FPS reaches ~half the analytic distance (x ≈ 10 vs 20) — the stage PASSES when the naive path shows the tie |
| `ft_30fps` | render at 30 FPS, physics at fixed 60 Hz: 120 steps in 2 s, final position matches analytic, interpolated render within one step, alpha in [0, 1) |
| `ft_120fps` | render at 120 FPS, physics at fixed 60 Hz: same asserts |
| `ft_hook` | frame-event path drives the pump: ~12 steps across 5 waits, no direct Pump call |
| `ft_hitch` | 200 ms stall takes exactly maxSteps (5), drops ≥50 ms, then recovers (≤2 steps per normal pump) |

Results (first build run, `F:\ScarlettCompat\build_fixed_timestep.log`,
2026-11-23 ~03:36 EST): **guest_physics 7/7 stages PASS, verdict ALL
PASS.** The run happened under full-build load, so the paced frame
times ran long (30 FPS scenario wall 2.288 s instead of 2.0 s, 120 FPS
scenario wall 3.690 s) — the asserts are wall-clock-relative, so the
proof holds under load:

- `ft_naive_tied`: naive 30 FPS reaches x=10.000 after 2.000 s vs
  analytic 20.001 — **50% of analytic**, the tie this work fixes.
- `ft_30fps`: 137 steps in 2.288 s (want 137 ±4), final x=22.833
  (want 22.878 ±0.75), max interp err 0.1668 (bound 0.250), alpha
  [0.000, 0.986).
- `ft_120fps`: 221 steps in 3.690 s (want 221 ±4), final x=36.833
  (want 36.898 ±0.75), max interp err 0.1674 (bound 0.250), alpha
  [0.001, 0.997).
- `ft_hook`: 5 frame events over 0.307 s, 14 steps (want 12 ±2),
  x=2.333 (want 2.0 ±0.5) — physics stepped inside
  `WaitFrameEventX` with no direct `Pump` call.
- `ft_hitch`: 200 ms stall took exactly 5 steps (the cap), dropped
  116.7 ms, recovery pumps 2/2.
- `ft_env`: `SCARLETT_PHYSICS_HZ=120` honored (rate 120.0 Hz,
  maxSteps 5).

Full build verdict: 8 of 9 verify runs ALL PASS (minimal, ordinal,
proof, era, software-rt in all three RT modes, physics). The one
failure is `guest_opt` (7/9 stages) on two optimization-agent items
unrelated to this work: `opt_detile` (256x256 RGBA8 round-trip
MISMATCH; detiles=2 hits=0, want 1/1/0) and `opt_async`
(`AsyncInit FAILED hr=0x80070057` E_INVALIDARG; serial/async both
0.000 ms, checksums 0). Those are the optimization agent's
in-progress items 2 and 4 — see the parent agent, not this doc.

## Files

- `include/fixed_timestep.h` — flat C API (ScFixedTimestep_*)
- `src/fixed_timestep.cpp` — accumulator + pump + alpha + stats
- `tests/guest_physics.cpp` — proof guest
- `defs/d3d12_xs.def` — the six exports
- `src/hle_registry.{cpp,h}` — Hle wrappers + registry entries +
  the pump call in `HleWaitFrameEventX`
- `src/verify.cpp` — the `ft` report block
- `build.bat` — DLL compile line, guest build, RC7 verify run
