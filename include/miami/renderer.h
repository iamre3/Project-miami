#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <string>
#include "miami/app.h"

namespace miami {

struct UiTheme {
    D2D1_COLOR_F bgBase;         // #07070a Deep obsidian / near black
    D2D1_COLOR_F bgSurface;      // #101016 Dark charcoal surface
    D2D1_COLOR_F bgSurfaceElev;  // #181822 Elevated surface
    D2D1_COLOR_F bgCard;         // #13131c Card surface
    D2D1_COLOR_F accentCyan;     // #00f2fe Miami Electric Cyan (Primary Accent)
    D2D1_COLOR_F accentPink;     // #e8507a Miami Sunset Pink (Secondary Accent)
    D2D1_COLOR_F textPrimary;    // #ffffff Soft white
    D2D1_COLOR_F textSecondary;  // #9ca3af Muted silver gray
    D2D1_COLOR_F textMuted;      // #525866 Deep muted
    D2D1_COLOR_F borderLight;    // rgba(255,255,255,0.08)
    D2D1_COLOR_F borderAccent;   // rgba(0,242,254,0.4)
    D2D1_COLOR_F successGreen;   // #10b981
    D2D1_COLOR_F warningAmber;   // #f59e0b
    D2D1_COLOR_F errorRed;       // #ef4444
};

class MiamiRenderer {
public:
    MiamiRenderer();
    ~MiamiRenderer();

    bool Initialize(HWND hwnd);
    void Resize(UINT width, UINT height);
    void Render(MiamiApp* app);
    void Cleanup();

    // Input handlers
    void OnMouseMove(int x, int y);
    void OnLButtonDown(int x, int y);
    void OnLButtonUp(int x, int y);
    void OnKeyDown(WPARAM key);

private:
    HWND m_hwnd = NULL;
    ID2D1Factory* m_pD2DFactory = NULL;
    ID2D1HwndRenderTarget* m_pRenderTarget = NULL;
    IDWriteFactory* m_pDWriteFactory = NULL;
    IWICImagingFactory* m_pWicFactory = NULL;

    // Premium Typography
    IDWriteTextFormat* m_pFontDisplay = NULL;   // 36pt Bold for Hero titles
    IDWriteTextFormat* m_pFontHeading = NULL;   // 20pt Semi-Bold for section titles
    IDWriteTextFormat* m_pFontBody = NULL;      // 14pt Regular for interface text
    IDWriteTextFormat* m_pFontSmall = NULL;     // 12pt Regular for captions/meta
    IDWriteTextFormat* m_pFontButton = NULL;    // 15pt Bold for primary CTA
    IDWriteTextFormat* m_pFontMonospace = NULL; // 11pt Consolas for logs/diagnostics

    // Assets
    ID2D1Bitmap* m_pBmpBackground = NULL;
    ID2D1Bitmap* m_pBmpBanner = NULL;
    ID2D1Bitmap* m_pBmpLogo = NULL;

    UiTheme m_theme;
    float m_animTime = 0.0f;
    int m_mouseX = 0;
    int m_mouseY = 0;
    bool m_mouseDown = false;
    int m_settingsCategory = 0; // 0=Display, 1=Performance, 2=Audio, 3=Input, 4=System

    // Helpers
    void CreateDeviceResources();
    void DiscardDeviceResources();
    void LoadAssetBitmaps();

    // View Renderers
    void RenderCustomTitleBar(MiamiApp* app, D2D1_RECT_F bounds);
    void RenderTopNavigation(MiamiApp* app, D2D1_RECT_F bounds);
    void RenderMainHome(MiamiApp* app, D2D1_RECT_F bounds);
    void RenderMainLibrary(MiamiApp* app, D2D1_RECT_F bounds);
    void RenderMainSettings(MiamiApp* app, D2D1_RECT_F bounds);
    void RenderMainDiagnostics(MiamiApp* app, D2D1_RECT_F bounds);
    void RenderMainAbout(MiamiApp* app, D2D1_RECT_F bounds);

    // Setup Wizard Views
    void RenderSetupWelcome(MiamiApp* app, D2D1_RECT_F bounds);
    void RenderSetupDirectory(MiamiApp* app, D2D1_RECT_F bounds);
    void RenderSetupValidation(MiamiApp* app, D2D1_RECT_F bounds);
    void RenderSetupConfiguration(MiamiApp* app, D2D1_RECT_F bounds);
    void RenderSetupComplete(MiamiApp* app, D2D1_RECT_F bounds);

    // UI Controls
    bool DrawButton(const wchar_t* text, D2D1_RECT_F rect, bool isPrimary = false, bool isHovered = false, bool isSelected = false);
    bool DrawNavTab(const wchar_t* text, D2D1_RECT_F rect, bool isSelected, bool isHovered);
};

} // namespace miami
