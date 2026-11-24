// fixed_timestep.cpp — fixed-timestep physics decoupling (see
// include/fixed_timestep.h and docs/FIXED-TIMESTEP.md).
// Accumulator + fixed-step invocation + interpolation factor, driven
// by QPC. The physics state belongs to the title; this file owns only
// the timing. The pump runs on the game's thread, at the game's frame
// boundary (called from HleWaitFrameEventX when a callback is
// registered, or directly by the title).
#include "fixed_timestep.h"
#include "scarlett.h"  // sc::Log
#include <stdlib.h>

namespace {

void FtLog(const char* fmt, ...) {
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    sc::Log("timestep", "%s", body);
}

// Default physics rate (Hz) and spiral-of-death cap (steps per pump).
const double kDefaultRateHz = 60.0;
const uint32_t kDefaultMaxSteps = 5;

double g_rateHz = kDefaultRateHz;
double g_stepDt = 1.0 / kDefaultRateHz;
uint32_t g_maxSteps = kDefaultMaxSteps;
double g_accumulator = 0.0;      // seconds since the last step, [0, dt)
LARGE_INTEGER g_lastQpc = {};    // QPC at the last pump
bool g_haveBaseline = false;     // false until the first pump after Init
ScPhysicsStepFn g_stepFn = nullptr;
void* g_stepCtx = nullptr;

uint64_t g_totalSteps = 0;
uint64_t g_totalPumps = 0;
uint32_t g_maxStepsInOnePump = 0;
double g_droppedSeconds = 0.0;

// QPC frequency, cached once (QueryPerformanceFrequency never
// changes for a process).
LARGE_INTEGER QpcFreq() {
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    return freq;
}

// Read the launch rate from SCARLETT_PHYSICS_HZ. Returns <= 0 when
// the variable is unset, unparsable, or out of range (1..1000 Hz) —
// the caller falls back to the default.
double RateFromEnv() {
    char value[64] = {};
    DWORD n = GetEnvironmentVariableA("SCARLETT_PHYSICS_HZ", value,
                                      sizeof(value));
    if (n == 0 || n >= sizeof(value)) return 0.0;
    char* end = nullptr;
    double hz = strtod(value, &end);
    if (end == value || *end != '\0' || hz <= 0.0 || hz > 1000.0) {
        FtLog("SCARLETT_PHYSICS_HZ='%s' unparsable/out of range -> 60 Hz "
              "(want a number 1..1000)",
              value);
        return 0.0;
    }
    FtLog("SCARLETT_PHYSICS_HZ='%s' -> %.3f Hz", value, hz);
    return hz;
}

}  // namespace

extern "C" HRESULT ScFixedTimestep_Init(double rateHz,
                                        uint32_t maxStepsPerFrame) {
    if (rateHz <= 0.0) {
        rateHz = RateFromEnv();
        if (rateHz <= 0.0) rateHz = kDefaultRateHz;
    }
    if (maxStepsPerFrame == 0) maxStepsPerFrame = kDefaultMaxSteps;
    g_rateHz = rateHz;
    g_stepDt = 1.0 / rateHz;
    g_maxSteps = maxStepsPerFrame;
    g_accumulator = 0.0;
    g_haveBaseline = false;
    g_totalSteps = 0;
    g_totalPumps = 0;
    g_maxStepsInOnePump = 0;
    g_droppedSeconds = 0.0;
    FtLog("Init rate=%.3f Hz step=%.4f ms maxSteps=%u (callback %s)",
          g_rateHz, g_stepDt * 1000.0, g_maxSteps,
          g_stepFn ? "registered" : "none");
    return S_OK;
}

extern "C" HRESULT ScFixedTimestep_SetStepCallback(ScPhysicsStepFn fn,
                                                   void* ctx) {
    g_stepFn = fn;
    g_stepCtx = ctx;
    FtLog("SetStepCallback fn=%p ctx=%p (%s)", (void*)fn, ctx,
          fn ? "registered" : "unregistered");
    return S_OK;
}

extern "C" HRESULT ScFixedTimestep_Pump(uint32_t* stepsTaken) {
    if (stepsTaken) *stepsTaken = 0;
    // No callback: nothing to step, and no time accumulates — reset
    // the baseline so registering later starts clean. This is also
    // what keeps the WaitFrameEventX hook zero-cost for titles that
    // never register a step function.
    if (!g_stepFn) {
        g_haveBaseline = false;
        return S_OK;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_haveBaseline) {
        g_lastQpc = now;
        g_haveBaseline = true;
        g_totalPumps++;
        return S_OK;
    }
    LARGE_INTEGER freq = QpcFreq();
    double elapsed =
        (double)(now.QuadPart - g_lastQpc.QuadPart) / (double)freq.QuadPart;
    g_lastQpc = now;
    g_totalPumps++;
    if (elapsed <= 0.0) return S_OK;
    g_accumulator += elapsed;
    // How many fixed steps the accumulated time buys.
    uint32_t want = (uint32_t)(g_accumulator / g_stepDt);
    uint32_t take = want;
    if (take > g_maxSteps) {
        // Spiral-of-death guard: after a hitch the simulation does
        // NOT fast-forward to catch up. Take the cap, drop the rest.
        double dropped = (double)(take - g_maxSteps) * g_stepDt;
        g_droppedSeconds += dropped;
        g_accumulator -= dropped;
        take = g_maxSteps;
        FtLog("Pump: hitch, %u steps wanted, took %u, dropped %.1f ms",
              want, take, dropped * 1000.0);
    }
    for (uint32_t i = 0; i < take; i++) {
        g_stepFn(g_stepCtx, g_stepDt);
        g_accumulator -= g_stepDt;
        g_totalSteps++;
    }
    // Floating-point residue can leave the accumulator a hair above
    // dt (or a hair below 0); clamp it into [0, dt).
    if (g_accumulator < 0.0) g_accumulator = 0.0;
    if (g_accumulator >= g_stepDt) g_accumulator = g_stepDt * 0.999999;
    if (take > g_maxStepsInOnePump) g_maxStepsInOnePump = take;
    if (stepsTaken) *stepsTaken = take;
    return S_OK;
}

extern "C" double ScFixedTimestep_Alpha(void) {
    double alpha = g_accumulator / g_stepDt;
    if (alpha < 0.0) return 0.0;
    if (alpha >= 1.0) return 0.999999;
    return alpha;
}

extern "C" HRESULT ScFixedTimestep_GetStats(ScFixedTimestepStats* out) {
    if (!out) return E_INVALIDARG;
    out->rateHz = g_rateHz;
    out->stepDt = g_stepDt;
    out->maxStepsPerFrame = g_maxSteps;
    out->totalSteps = g_totalSteps;
    out->totalPumps = g_totalPumps;
    out->maxStepsInOnePump = g_maxStepsInOnePump;
    out->droppedSeconds = g_droppedSeconds;
    out->alpha = ScFixedTimestep_Alpha();
    return S_OK;
}

extern "C" void ScFixedTimestep_Shutdown(void) {
    g_stepFn = nullptr;
    g_stepCtx = nullptr;
    g_accumulator = 0.0;
    g_haveBaseline = false;
    g_totalSteps = 0;
    g_totalPumps = 0;
    g_maxStepsInOnePump = 0;
    g_droppedSeconds = 0.0;
    FtLog("Shutdown (callback cleared, accumulator reset)");
}
