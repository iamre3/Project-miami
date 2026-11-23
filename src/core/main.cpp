#include <windows.h>
#include "miami/app.h"
#include "miami/renderer.h"
#include "miami/config.h"

static miami::MiamiApp g_app;
static miami::MiamiRenderer g_renderer;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE: {
            UINT width = LOWORD(lParam);
            UINT height = HIWORD(lParam);
            g_renderer.Resize(width, height);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            g_renderer.OnMouseMove(x, y);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            SetCapture(hwnd);
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            g_renderer.OnLButtonDown(x, y);
            return 0;
        }
        case WM_LBUTTONUP: {
            ReleaseCapture();
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            g_renderer.OnLButtonUp(x, y);
            return 0;
        }
        case WM_KEYDOWN: {
            g_renderer.OnKeyDown(wParam);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            g_renderer.Render(&g_app);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER: {
            // Repaint for smooth animations and process polling
            g_app.GetProcess().PollState();
            InvalidateRect(hwnd, NULL, FALSE);
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
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    // Initialize application logic
    if (!g_app.Initialize(hInstance, nCmdShow)) {
        MessageBoxA(NULL, "Failed to initialize Project Miami application core.", "Project Miami Error", MB_ICONERROR);
        return 1;
    }

    // Register Window Class
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

    // Create Window
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int winW = 1280;
    int winH = 800;
    int posX = (screenW - winW) / 2;
    int posY = (screenH - winH) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        CLASS_NAME,
        L"Project Miami — Grand Theft Auto VI PC Launcher",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        posX, posY, winW, winH,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd) {
        return 1;
    }

    // Initialize Direct2D Renderer
    if (!g_renderer.Initialize(hwnd)) {
        MessageBoxA(NULL, "Failed to initialize Direct2D / DirectWrite hardware acceleration.", "Project Miami Error", MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Set 60 FPS animation/poll timer (~16ms)
    SetTimer(hwnd, 1, 16, NULL);

    // Main Message Loop
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
