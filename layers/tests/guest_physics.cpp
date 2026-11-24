// guest_physics.cpp — fixed-timestep physics proof guest.
// A synthetic Game Core title that exercises the layer's
// fixed-timestep entry points (docs/FIXED-TIMESTEP.md) through the
// normal loader path:
//   XGameRuntimeInitialize -> ScFixedTimestep_Init ->
//   ScFixedTimestep_SetStepCallback -> Pump / Alpha / GetStats.
//
// The simulated physics is a point moving at constant velocity
// v = 10 units/s, x += v * dt per fixed step, so the analytic
// position at wall time t is x = v * t. Every stage compares the
// simulation against that analytic answer.
//
// Stages (ft_* in guest_report.txt):
//   ft_runtime   runtime init
//   ft_env       SCARLETT_PHYSICS_HZ override (120 Hz) is honored
//   ft_naive_tied  the problem: one physics step per render frame at
//                  30 FPS reaches only ~half the analytic distance —
//                  the stage PASSES when the naive path shows the tie
//   ft_30fps     render at 30 FPS, physics must still run at 60 Hz:
//                  120 steps in 2 s, final position matches analytic,
//                  interpolated render stays within one step
//   ft_120fps    render at 120 FPS, physics must still run at 60 Hz:
//                  same asserts as ft_30fps
//   ft_hook      the frame-event path drives the pump: physics steps
//                  happen inside WaitFrameEventX with no direct Pump
//                  call from the guest
//   ft_hitch     spiral-of-death guard: a 200 ms stall takes at most
//                  maxSteps (5) steps and drops the rest, then recovers
// This file contains NO game code — it is a test program written for
// the layer, and it is never run through the Windows loader directly
// (its Xbox imports only resolve inside the layer).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <mmsystem.h>  // timeBeginPeriod (guest pacing; games do this)
#include "xbox_shims.h"
#include "fixed_timestep.h"

// Xbox build-info section, like the shipped DLLs carry.
#pragma section(".xbld", read)
__declspec(allocate(".xbld")) static const char kXbld[] =
    "_xbld_edition_name=GXDK,October 2026 GXDK\0"
    "_xbld_edition_full_productbuild=GXDK,10.0.26100.9605\0"
    "_xbld_edition_sdktype=GXDK,261000\0"
    "_xbld_edition_mscver=GXDK,195237025.0\0"
    "_xbld_edition_build=GXDK,0x65F42585\0";

static FILE* g_report = nullptr;
static int g_stagesTotal = 0;
static int g_stagesPassed = 0;

static void Report(const char* name, bool pass, const char* detail) {
    printf("[guest] %-16s %s %s\n", name, pass ? "PASS" : "FAIL",
           detail ? detail : "");
    if (g_report) {
        fprintf(g_report, "%s=%s\n", name, pass ? "PASS" : "FAIL");
        if (detail) fprintf(g_report, "%s_detail=%s\n", name, detail);
        fflush(g_report);
    }
    if (strncmp(name, "ft", 2) == 0) {
        g_stagesTotal++;
        if (pass) g_stagesPassed++;
    }
}

// ---- simulated physics: a point at constant velocity ----
// prev = state before the most recent step, curr = state after it.
// The render position is prev + (curr - prev) * alpha.
static const double kVelocity = 10.0;  // units per second
static double g_prevX = 0.0;
static double g_currX = 0.0;

static void CALLBACK PhysicsStep(void* ctx, double dt) {
    (void)ctx;
    g_prevX = g_currX;
    g_currX += kVelocity * dt;
}

static void ResetSim() {
    g_prevX = 0.0;
    g_currX = 0.0;
}

// ---- timing helpers (QPC; two-phase pace like guest_era) ----
static LARGE_INTEGER g_qpcFreq = {};

static double WallSeconds(LARGE_INTEGER start) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - start.QuadPart) /
           (double)g_qpcFreq.QuadPart;
}

// Pace the current frame to targetMs since frameStart: Sleep the
// bulk, QPC-spin the last ~1.5 ms (the same shape as the layer's
// two-phase wait and the era guest's frame pacing).
static void PaceToMs(LARGE_INTEGER frameStart, double targetMs) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsedMs = (double)(now.QuadPart - frameStart.QuadPart) *
                       1000.0 / (double)g_qpcFreq.QuadPart;
    double remainMs = targetMs - elapsedMs;
    if (remainMs > 1.5) Sleep((DWORD)(remainMs - 1.5));
    LARGE_INTEGER deadline;
    deadline.QuadPart = frameStart.QuadPart +
                        (LONGLONG)(targetMs * (double)g_qpcFreq.QuadPart /
                                   1000.0);
    do {
        QueryPerformanceCounter(&now);
    } while (now.QuadPart < deadline.QuadPart);
}

// Run one render-rate scenario: render at frameMs per iteration for
// iterations frames, pumping the fixed timestep each iteration and
// rendering the interpolated position. Returns false on any assert
// failure; fills detail with the measured numbers.
static bool RunRateScenario(double frameMs, int iterations, char* detail,
                            size_t n) {
    ResetSim();
    ScFixedTimestep_Init(60.0, 5);
    ScFixedTimestep_SetStepCallback(PhysicsStep, nullptr);
    // Prime the pump: the first Pump after Init sets the QPC baseline
    // and steps 0 times. Priming here means every paced iteration
    // contributes its full interval to the accumulator.
    uint32_t steps = 0;
    ScFixedTimestep_Pump(&steps);

    LARGE_INTEGER scenarioStart;
    QueryPerformanceCounter(&scenarioStart);
    double maxInterpErr = 0.0;
    double alphaMin = 1.0, alphaMax = 0.0;
    for (int i = 0; i < iterations; i++) {
        LARGE_INTEGER frameStart;
        QueryPerformanceCounter(&frameStart);
        PaceToMs(frameStart, frameMs);
        HRESULT hr = ScFixedTimestep_Pump(&steps);
        if (FAILED(hr)) {
            snprintf(detail, n, "Pump hr=0x%08x at frame %d",
                     (unsigned)hr, i);
            return false;
        }
        double alpha = ScFixedTimestep_Alpha();
        if (alpha < alphaMin) alphaMin = alpha;
        if (alpha > alphaMax) alphaMax = alpha;
        // Render the interpolated position and compare it to the
        // analytic position at this wall time.
        double renderX = g_prevX + (g_currX - g_prevX) * alpha;
        double analyticX = kVelocity * WallSeconds(scenarioStart);
        double err = fabs(renderX - analyticX);
        if (err > maxInterpErr) maxInterpErr = err;
    }
    double wallS = WallSeconds(scenarioStart);
    ScFixedTimestepStats stats = {};
    ScFixedTimestep_GetStats(&stats);
    double analyticFinal = kVelocity * wallS;
    double expectedSteps = wallS * 60.0;
    bool stepsOk = fabs((double)stats.totalSteps - expectedSteps) <= 4.0;
    // Position error is the step-count error times one step's motion
    // (0.167 units), so the bound follows the step tolerance: 4 steps
    // = 0.67 units, plus slack.
    bool posOk = fabs(g_currX - analyticFinal) <= 0.75;
    // The interpolated render trails wall time by at most one fixed
    // step (16.7 ms = 0.167 units at v=10); 1.5 steps of slack covers
    // timing jitter. Alpha must stay in [0, 1).
    bool interpOk = maxInterpErr <= kVelocity * (1.0 / 60.0) * 1.5;
    bool alphaOk = alphaMin >= 0.0 && alphaMax < 1.0;
    bool ok = stepsOk && posOk && interpOk && alphaOk;
    snprintf(detail, n,
             "render %.2f ms x %d frames: wall %.3f s, steps %llu "
             "(want %.0f +/-4), final x %.3f (want %.3f +/-0.75), "
             "max interp err %.4f (bound %.3f), alpha [%.3f, %.3f)",
             frameMs, iterations, wallS,
             (unsigned long long)stats.totalSteps, expectedSteps,
             g_currX, analyticFinal, maxInterpErr,
             kVelocity * (1.0 / 60.0) * 1.5, alphaMin, alphaMax);
    return ok;
}

int main() {
    fopen_s(&g_report, "guest_report.txt", "w");
    bool allOk = true;
    char detail[1024] = {};
    HRESULT hr = S_OK;

    QueryPerformanceFrequency(&g_qpcFreq);
    timeBeginPeriod(1);  // test guest pacing; games do this routinely

    // 1. Runtime init.
    hr = XGameRuntimeInitialize();
    Report("ft_runtime", SUCCEEDED(hr), "XGameRuntimeInitialize");
    allOk &= SUCCEEDED(hr);

    // 2. Env override: SCARLETT_PHYSICS_HZ=120 must win over a 0 rate.
    _putenv_s("SCARLETT_PHYSICS_HZ", "120");
    hr = ScFixedTimestep_Init(0.0, 0);
    ScFixedTimestepStats stats = {};
    bool envOk = false;
    if (SUCCEEDED(hr)) {
        ScFixedTimestep_GetStats(&stats);
        envOk = stats.rateHz == 120.0 && stats.maxStepsPerFrame == 5;
        snprintf(detail, sizeof(detail),
                 "hr=0x%08x rate=%.1f Hz maxSteps=%u (want 120 Hz / 5)",
                 (unsigned)hr, stats.rateHz, stats.maxStepsPerFrame);
    } else {
        snprintf(detail, sizeof(detail), "Init hr=0x%08x", (unsigned)hr);
    }
    Report("ft_env", envOk, detail);
    allOk &= envOk;
    _putenv_s("SCARLETT_PHYSICS_HZ", "");  // back to default (60 Hz)

    // 3. The problem: one physics step per render frame at 30 FPS.
    // 60 frames in 2 s, one step each: x = 60 * (10/60) = 10 units,
    // half the analytic 20. The stage PASSES when the naive path
    // shows the tie (x in [9, 11]) — that is the bug this work fixes.
    ResetSim();
    {
        LARGE_INTEGER start;
        QueryPerformanceCounter(&start);
        for (int i = 0; i < 60; i++) {
            LARGE_INTEGER frameStart;
            QueryPerformanceCounter(&frameStart);
            PaceToMs(frameStart, 1000.0 / 30.0);
            PhysicsStep(nullptr, 1.0 / 60.0);  // one step per frame
        }
        double wallS = WallSeconds(start);
        double analytic = kVelocity * wallS;
        bool tied = g_currX >= 9.0 && g_currX <= 11.0;
        snprintf(detail, sizeof(detail),
                 "naive 30 FPS: x=%.3f after %.3f s, analytic %.3f "
                 "(naive is %.0f%% of analytic — physics tied to FPS)",
                 g_currX, wallS, analytic,
                 analytic > 0.0 ? g_currX / analytic * 100.0 : 0.0);
        Report("ft_naive_tied", tied, detail);
        allOk &= tied;
    }

    // 4. Fixed timestep, render at 30 FPS: physics must run at 60 Hz.
    bool ok30 = RunRateScenario(1000.0 / 30.0, 60, detail, sizeof(detail));
    Report("ft_30fps", ok30, detail);
    allOk &= ok30;

    // 5. Fixed timestep, render at 120 FPS: physics must run at 60 Hz.
    bool ok120 = RunRateScenario(1000.0 / 120.0, 240, detail,
                                 sizeof(detail));
    Report("ft_120fps", ok120, detail);
    allOk &= ok120;

    // 6. The frame-event path drives the pump: no direct Pump call
    // from the guest — physics steps happen inside WaitFrameEventX.
    // 5 waits, ~50 ms apart: the first wait's pump sets the baseline
    // (0 steps), the next 4 pump ~3 steps each = ~12 steps total.
    bool hookOk = false;
    {
        ResetSim();
        ScFixedTimestep_Init(60.0, 5);
        ScFixedTimestep_SetStepCallback(PhysicsStep, nullptr);
        SetFrameIntervalX(1, 0);
        LARGE_INTEGER start;
        QueryPerformanceCounter(&start);
        for (int i = 0; i < 5; i++) {
            ScheduleFrameEventX((UINT64)(i + 1));
            Sleep(50);  // frame is 50 ms past its 16.7 ms deadline,
                        // so the wait returns at once and the pump
                        // runs with ~50 ms of elapsed time
            WaitFrameEventX((UINT64)(i + 1), 1000);
        }
        double wallS = WallSeconds(start);
        ScFixedTimestep_GetStats(&stats);
        hookOk = stats.totalSteps >= 10 && stats.totalSteps <= 14 &&
                 fabs(g_currX - 2.0) <= 0.5;
        snprintf(detail, sizeof(detail),
                 "5 frame events over %.3f s: steps %llu (want 12 "
                 "+/-2), x=%.3f (want 2.0 +/-0.5), no direct Pump call",
                 wallS, (unsigned long long)stats.totalSteps, g_currX);
        Report("ft_hook", hookOk, detail);
        allOk &= hookOk;
    }

    // 7. Spiral-of-death guard: a 200 ms stall must take at most
    // maxSteps (5) steps and drop the rest, then recover.
    bool hitchOk = false;
    {
        ResetSim();
        ScFixedTimestep_Init(60.0, 5);
        ScFixedTimestep_SetStepCallback(PhysicsStep, nullptr);
        uint32_t steps = 0;
        ScFixedTimestep_Pump(&steps);  // prime the baseline
        Sleep(200);
        ScFixedTimestep_Pump(&steps);
        ScFixedTimestep_GetStats(&stats);
        // 200 ms wants 12 steps; the cap takes 5 and drops ~117 ms.
        bool capped = steps == 5 && stats.droppedSeconds >= 0.05;
        // Recovery: two normal 16.7 ms pumps take at most 2 steps each.
        uint32_t s1 = 0, s2 = 0;
        Sleep(17);
        ScFixedTimestep_Pump(&s1);
        Sleep(17);
        ScFixedTimestep_Pump(&s2);
        bool recovered = s1 <= 2 && s2 <= 2;
        hitchOk = capped && recovered;
        snprintf(detail, sizeof(detail),
                 "200 ms stall: steps=%u (want 5), dropped %.1f ms "
                 "(want >=50), recovery pumps %u/%u (want <=2 each)",
                 steps, stats.droppedSeconds * 1000.0, s1, s2);
        Report("ft_hitch", hitchOk, detail);
        allOk &= hitchOk;
    }

    ScFixedTimestep_Shutdown();
    timeEndPeriod(1);
    snprintf(detail, sizeof(detail),
             "%d/%d fixed-timestep stages pass: physics at fixed 60 Hz "
             "while rendering at 30 and 120 FPS",
             g_stagesPassed, g_stagesTotal);
    Report("ft", allOk, detail);
    printf("[guest] fixed-timestep %s\n", allOk ? "ALL PASS" : "FAIL");
    if (g_report) fclose(g_report);
    return allOk ? 0 : 1;
}
