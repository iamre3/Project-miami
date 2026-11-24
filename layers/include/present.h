// Presentation contract — real-exe path only (Seth, 2026-11-23).
//
// Two rules, both hard:
//  1. BLACK UNTIL THE TITLE LOADS. The screen stays BLACK unless the title
//     itself is trying to load. No test pattern, no red triangle, no
//     placeholder output in the real-exe path. Default presentation
//     is a black clear; only once the title has actually submitted a
//     real frame does anything else show. The proof guest's red
//     triangle and any test patterns live in the SYNTHETIC verify
//     guests only, never in the real-exe boot path.
//  2. 720p WINDOWED, NEVER FULLSCREEN. When the title's presentation
//     path creates its window/swapchain, it is 1280x720 windowed.
//
// The PresentX handler (COM wrapper milestone) consults the gate in
// this module: while ScPresentGate_ShouldShowGame is false it clears
// the back buffer to black (0,0,0,1) and presents that; once the
// title has submitted a real frame (ScPresentGate_NoteGameFrame), it
// presents the game's frame. The window the presenter uses comes
// from ScCreateGameWindow.
//
// What shows on screen during a real-exe boot TODAY: nothing. Every
// boot so far dies in static/global init before any window is
// created, before any device, before any present. This module is the
// contract for the presentation milestone so that when a window and
// a present path DO exist, they default to black + 720p windowed
// instead of ever showing a test pattern.
#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// Game window size — 720p, windowed. Never fullscreen.
#define SC_GAME_WINDOW_WIDTH 1280
#define SC_GAME_WINDOW_HEIGHT 720

// Creates the game window: SC_GAME_WINDOW_WIDTH x SC_GAME_WINDOW_HEIGHT
// client area, WS_OVERLAPPEDWINDOW (windowed, never fullscreen),
// centered on the primary monitor, shown. Returns the HWND, or NULL
// on failure. Safe to call once; a second call returns the same HWND.
HWND ScCreateGameWindow(void);

// Destroys the game window created by ScCreateGameWindow (if any).
// Safe to call when no window exists.
void ScDestroyGameWindow(void);

// Black-until-first-frame gate. Starts closed (show black); opens
// the first time the title submits a genuine frame.
typedef struct ScPresentGate {
    bool firstRealFrame;
} ScPresentGate;

// Initialise a gate to the closed state (show black).
void ScPresentGate_Init(ScPresentGate* gate);

// Call when the title submits a genuine frame (first PresentX with
// real plane resources / first ExecuteCommandLists carrying draw or
// dispatch work). Opens the gate permanently.
void ScPresentGate_NoteGameFrame(ScPresentGate* gate);

// False until the first genuine game frame has been submitted — while
// false, the presenter must clear to black and present black. True
// once a real frame exists — the presenter may then show the game's
// frame.
bool ScPresentGate_ShouldShowGame(const ScPresentGate* gate);

#ifdef __cplusplus
}
#endif
