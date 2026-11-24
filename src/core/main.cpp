#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include "miami/app.h"
#include "miami/renderer.h"
#include "miami/config.h"

#pragma comment(lib, "dwmapi.lib")

static miami::MiamiApp g_app;
static miami::MiamiRenderer g_renderer;

static void ApplyRoundedWindowRegion(HWND hwnd, UINT width, UINT height) {
    if (width == 0 || height == 0) return;

    WINDOWPLACEMENT wp = {};
    wp.length = sizeof(wp);
    GetWindowPlacement(hwnd, &wp);

    // Enable Windows 11 native DWM rounded corners & dark frame where supported
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkMode, sizeof(darkMode));

    if (wp.showCmd == SW_SHOWMAXIMIZED) {
        SetWindowRgn(hwnd, NULL, TRUE);
    } else {
        // Generous 28px rounded window region for borderless glass aesthetic
        HRGN hRgn = CreateRoundRectRgn(0, 0, static_cast<int>(width) + 1, static_cast<int>(height) + 1, 28, 28);
        SetWindowRgn(hwnd, hRgn, TRUE);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCCALCSIZE: {
            // Remove standard Windows titlebar and non-client frame so our rounded canvas fills 100% of the window
            if (wParam == TRUE) {
                return 0;
            }
            break;
        }
        case WM_NCHITTEST: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);

            RECT rc;
            GetClientRect(hwnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;

            WINDOWPLACEMENT wp = {};
            wp.length = sizeof(wp);
            GetWindowPlacement(hwnd, &wp);
            bool isMaximized = (wp.showCmd == SW_SHOWMAXIMIZED);

            if (!isMaximized) {
                const int kBorder = 6;
                bool left   = pt.x < kBorder;
                bool right  = pt.x >= w - kBorder;
                bool top    = pt.y < kBorder;
                bool bottom = pt.y >= h - kBorder;

                if (top && left)     return HTTOPLEFT;
                if (top && right)    return HTTOPRIGHT;
                if (bottom && left)  return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (left)            return HTLEFT;
                if (right)           return HTRIGHT;
                if (top)             return HTTOP;
                if (bottom)          return HTBOTTOM;
            }

            // Drag window when clicking the top title bar (excluding right-side window buttons)
            if (pt.y >= 0 && pt.y < 40 && pt.x < w - 150) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 1024;
            mmi->ptMinTrackSize.y = 680;
            return 0;
        }
        case WM_SIZE: {
            UINT width = LOWORD(lParam);
            UINT height = HIWORD(lParam);
            ApplyRoundedWindowRegion(hwnd, width, height);
            g_renderer.Resize(width, height);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            g_renderer.OnMouseMove(x, y);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            SetCapture(hwnd);
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            g_renderer.OnLButtonDown(x, y);
            return 0;
        }
        case WM_LBUTTONUP: {
            ReleaseCapture();
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            g_renderer.OnLButtonUp(x, y);
            return 0;
        }
        case WM_KEYDOWN: {
            g_renderer.OnKeyDown(wParam);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            g_renderer.Render(&g_app);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER: {
            bool wasRunning = g_app.GetProcess().IsRunning();
            g_app.GetProcess().PollState();
            bool isRunning = g_app.GetProcess().IsRunning();

            // When a game process is running or state transitions, keep UI responsive.
            // When completely idle with no running process or state change, avoid waking the GPU every 16ms.
            static int idleTick = 0;
            idleTick++;
            if (wasRunning != isRunning || isRunning || (idleTick % 30 == 0)) {
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        case WM_DESTROY: {
            KillTimer(hwnd, 1);
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    SetProcessDPIAware();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    if (!g_app.Initialize(hInstance, nCmdShow)) {
        MessageBoxA(NULL, "Failed to initialize Project Miami application core.", "Project Miami Error", MB_ICONERROR);
        return 1;
    }

    const wchar_t CLASS_NAME[] = L"ProjectMiamiLauncherWindow";
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.style = CS_HREDRAW | CS_VREDRAW;

    RegisterClassExW(&wc);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int winW = 1320;
    int winH = 820;
    int posX = (screenW - winW) / 2;
    int posY = (screenH - winH) / 2;

    // Borderless popup + thickframe for smooth resizing and rounded region clipping
    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        CLASS_NAME,
        L"Project Miami — Grand Theft Auto VI PC Launcher",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_VISIBLE,
        posX, posY, winW, winH,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd) {
        return 1;
    }

    ApplyRoundedWindowRegion(hwnd, winW, winH);

    if (!g_renderer.Initialize(hwnd)) {
        MessageBoxA(NULL, "Failed to initialize Direct2D / DirectWrite hardware acceleration.", "Project Miami Error", MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    SetTimer(hwnd, 1, 16, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_renderer.Cleanup();
    g_app.Shutdown();
    CoUninitialize();

    return (int)msg.wParam;
}
