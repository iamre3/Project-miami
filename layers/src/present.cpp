// Presentation contract implementation — real-exe path only.
// See include/present.h for the rules (Seth, 2026-11-23):
//  1. Black until the title submits its first genuine frame.
//  2. Game window is 1280x720 windowed, never fullscreen.
#include "present.h"

namespace {

// Window class for the game window.
constexpr wchar_t kGameWindowClass[] = L"ScarlettCompatGameWindow";
HWND g_gameWindow = nullptr;

LRESULT CALLBACK GameWindowProc(HWND hwnd, UINT msg, WPARAM wParam,
                                LPARAM lParam) {
    if (msg == WM_DESTROY) {
        // The title owns its lifetime; quitting the message loop is
        // the window's job, not the layer's. Just clear our handle.
        if (g_gameWindow == hwnd) g_gameWindow = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

extern "C" HWND ScCreateGameWindow(void) {
    if (g_gameWindow) return g_gameWindow;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = GameWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = kGameWindowClass;
    // Re-registering an existing class fails with
    // ERROR_CLASS_ALREADY_EXISTS; that is fine, the class is ours.
    RegisterClassExW(&wc);

    // 1280x720 CLIENT area, windowed (WS_OVERLAPPEDWINDOW — never
    // fullscreen, never borderless, never WS_POPUP).
    RECT rc = {0, 0, SC_GAME_WINDOW_WIDTH, SC_GAME_WINDOW_HEIGHT};
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
    const int winW = rc.right - rc.left;
    const int winH = rc.bottom - rc.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;

    g_gameWindow = CreateWindowExW(
        0, kGameWindowClass, L"ScarlettCompat", WS_OVERLAPPEDWINDOW, x, y,
        winW, winH, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_gameWindow) return nullptr;
    ShowWindow(g_gameWindow, SW_SHOW);
    return g_gameWindow;
}

extern "C" void ScDestroyGameWindow(void) {
    if (g_gameWindow) {
        DestroyWindow(g_gameWindow);
        g_gameWindow = nullptr;
    }
}

extern "C" void ScPresentGate_Init(ScPresentGate* gate) {
    if (gate) gate->firstRealFrame = false;
}

extern "C" void ScPresentGate_NoteGameFrame(ScPresentGate* gate) {
    if (gate) gate->firstRealFrame = true;
}

extern "C" bool ScPresentGate_ShouldShowGame(const ScPresentGate* gate) {
    return gate && gate->firstRealFrame;
}
