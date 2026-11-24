// fixed_timestep.h — fixed-timestep physics decoupling for the
// ScarlettCompat layer.
//
// The problem: a title steps its physics once per frame event, so the
// physics rate IS the frame rate — at 30 FPS physics runs at half
// speed, at 120 FPS it runs at double speed. The fix is the standard
// one (Gaffer on Games, "Fix Your Timestep"):
//   * an accumulator collects real elapsed time (QPC),
//   * physics steps in fixed increments (1/60 s by default) as many
//     times as needed per frame,
//   * the renderer interpolates between the last two physics states
//     with alpha = accumulator / dt, so motion stays smooth when the
//     render rate differs from the physics rate.
//
// These are ScarlettCompat-defined entry points, NOT Xbox APIs. The
// physics state itself belongs to the title — the layer owns only the
// accumulator, the step invocation, and the interpolation factor. The
// title registers its physics-step function with
// ScFixedTimestep_SetStepCallback; the layer calls it from the
// frame-event path (WaitFrameEventX pumps automatically when a
// callback is registered) and the title may also pump directly with
// ScFixedTimestep_Pump. Use ONE path per frame, not both — a second
// pump in the same frame sees ~0 elapsed time and steps 0 times, but
// it still counts in the stats.
//
// Why the pump runs on the game's thread (and not on a separate
// physics thread): the physics state is the game's memory. Stepping
// it from another thread would race the game's own access to that
// state, and the layer cannot know the game's synchronization. The
// pump therefore runs at the game's frame boundary, on the game's
// thread — the decoupling is in the accumulator, not in threading.
//
// Honest bound: the rendered state is the interpolation between the
// previous and current physics states, which trails wall-clock time
// by exactly one fixed step (16.7 ms at 60 Hz). That lag is constant,
// so motion is smooth; the alternative (extrapolating one step ahead)
// would be exact but unstable. The proof guest asserts the rendered
// position stays within one step of the analytic position.
//
// The real-game hook point is exe-dependent (see
// docs/FIXED-TIMESTEP.md): the layer must find the title's physics
// step — the function its main loop calls once per frame event — and
// route it through ScFixedTimestep_SetStepCallback. The mechanism
// below is built and proven against a synthetic guest; the address
// to hook in the real exe is unknown until the exe lands.
#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The title's physics-step function: advance the title's physics
// state by exactly dt seconds. Called on the game's thread, from the
// pump, once per fixed step. dt is always the configured step
// (1/rateHz), never a variable frame time.
typedef void (CALLBACK *ScPhysicsStepFn)(void* ctx, double dt);

// Snapshot of the accumulator state, for the harness / diagnostics.
typedef struct ScFixedTimestepStats {
    double rateHz;              // configured physics rate (default 60)
    double stepDt;              // 1 / rateHz, seconds per step
    uint32_t maxStepsPerFrame;  // spiral-of-death cap (default 5)
    uint64_t totalSteps;        // physics steps taken since Init
    uint64_t totalPumps;        // Pump calls since Init
    uint32_t maxStepsInOnePump; // most steps any single Pump took
    double droppedSeconds;      // wall time discarded by the step cap
    double alpha;               // current interpolation factor [0, 1)
} ScFixedTimestepStats;

// Initialize (or re-initialize) the accumulator. Resets the baseline,
// the accumulator, and the stats; keeps the registered callback.
//   rateHz <= 0  -> read SCARLETT_PHYSICS_HZ (a number in Hz);
//                   unset / unparsable / out of range (1..1000) -> 60.
//   maxSteps == 0 -> 5.
// A re-Init is safe to call between scenarios (the proof guest does).
HRESULT ScFixedTimestep_Init(double rateHz, uint32_t maxStepsPerFrame);

// Register the title's physics-step function (NULL unregisters it).
// The pump does nothing while no callback is registered, and no time
// accumulates — the baseline is reset, so registering later starts
// clean.
HRESULT ScFixedTimestep_SetStepCallback(ScPhysicsStepFn fn, void* ctx);

// Advance the accumulator by the real elapsed time since the last
// pump and invoke the step callback once per fixed step. stepsTaken
// receives how many steps this pump took (0 when no callback is
// registered). If the elapsed time needs more steps than
// maxStepsPerFrame, only maxStepsPerFrame are taken and the rest is
// dropped (counted in stats.droppedSeconds) — the spiral-of-death
// guard: after a hitch the simulation does NOT fast-forward to catch
// up, it resumes at the fixed rate from the current time.
HRESULT ScFixedTimestep_Pump(uint32_t* stepsTaken);

// Current interpolation factor: accumulator / stepDt, in [0, 1).
// The renderer draws prev + (curr - prev) * alpha, where prev/curr
// are the physics states before/after the most recent step.
double ScFixedTimestep_Alpha(void);

// Copy the current stats into out (NULL -> E_INVALIDARG).
HRESULT ScFixedTimestep_GetStats(ScFixedTimestepStats* out);

// Clear the callback and reset the accumulator/stats.
void ScFixedTimestep_Shutdown(void);

#ifdef __cplusplus
}
#endif
