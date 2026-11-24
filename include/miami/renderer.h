#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <string>
#include "miami/app.h"

namespace miami {

struct UiTheme {
    D2D1_COLOR_F bgBase;         // #07070c Deep obsidian
    D2D1_COLOR_F bgSurface;      // #0e0e16 Translucent dark slate
    D2D1_COLOR_F bgSurfaceElev;  // #161622 Elevated glass panel
    D2D1_COLOR_F bgCard;         // #11111a Soft glass card
    D2D1_COLOR_F accentCyan;     // #00f2fe Miami Electric Cyan
    D2D1_COLOR_F accentPink;     // #e8507a Miami Sunset Pink
    D2D1_COLOR_F accentGold;     // #ffb347 Warm Sunset Amber
    D2D1_COLOR_F textPrimary;    // #ffffff Crisp white
    D2D1_COLOR_F textSecondary;  // #a1a8b8 Muted silver
    D2D1_COLOR_F textMuted;      // #565d6e Deep muted
    D2D1_COLOR_F borderLight;    // rgba(255,255,255,0.09)
    D2D1_COLOR_F borderAccent;   // rgba(0,242,254,0.45)
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
    IDWriteTextFormat* m_pFontHero = NULL;      // 44pt Heavy Display for Hero Title
    IDWriteTextFormat* m_pFontDisplay = NULL;   // 30pt Bold for Page Titles
    IDWriteTextFormat* m_pFontHeading = NULL;   // 16pt Semi-Bold for Section Headers
    IDWriteTextFormat* m_pFontBody = NULL;      // 13.5pt Regular for Body Text
    IDWriteTextFormat* m_pFontSmall = NULL;     // 11.5pt Regular for Metadata & Badges
    IDWriteTextFormat* m_pFontButton = NULL;    // 14pt Bold for Buttons
    IDWriteTextFormat* m_pFontMonospace = NULL; // 11.5pt Consolas for paths & versions

    // Assets
    ID2D1Bitmap* m_pBmpBackground = NULL;
    ID2D1Bitmap* m_pBmpBanner = NULL;
    ID2D1Bitmap* m_pBmpLogo = NULL;
    ID2D1Bitmap* m_pBmpLogoMark = NULL;
    ID2D1Bitmap* m_pBmpCharacters = NULL;
    ID2D1Bitmap* m_pBmpSkyline = NULL;

    // Cached Brushes (eliminates millions of allocations per minute)
    ID2D1SolidColorBrush* m_pBrushBgBase = NULL;
    ID2D1SolidColorBrush* m_pBrushBgSurface = NULL;
    ID2D1SolidColorBrush* m_pBrushBgCard = NULL;
    ID2D1SolidColorBrush* m_pBrushTextPrimary = NULL;
    ID2D1SolidColorBrush* m_pBrushTextSecondary = NULL;
    ID2D1SolidColorBrush* m_pBrushTextMuted = NULL;
    ID2D1SolidColorBrush* m_pBrushBorderLight = NULL;
    ID2D1SolidColorBrush* m_pBrushBorderAccent = NULL;
    ID2D1SolidColorBrush* m_pBrushAccentCyan = NULL;
    ID2D1SolidColorBrush* m_pBrushAccentPink = NULL;
    ID2D1SolidColorBrush* m_pBrushSuccessGreen = NULL;
    ID2D1SolidColorBrush* m_pBrushWarningAmber = NULL;

    UiTheme m_theme;
    float m_animTime = 0.0f;
    int m_mouseX = 0;
    int m_mouseY = 0;
    bool m_mouseDown = false;
    bool m_clickConsumed = false;
    int m_settingsCategory = 0; // 0=Display, 1=Performance, 2=Audio, 3=Input, 4=System

    // Helpers
    void CreateDeviceResources();
    void DiscardDeviceResources();
    void LoadAssetBitmaps();
    void RenderCinematicBackdrop(D2D1_RECT_F bounds, float imageOpacity = 0.55f);
    void RenderRoundedWindowFrame(D2D1_RECT_F bounds);

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
    void RenderSetupComplete(MiamiApp* app, D2D1_RECT_F bounds);

    // UI Controls
    bool DrawButton(const wchar_t* text, D2D1_RECT_F rect, bool isPrimary = false, bool isHovered = false, bool isSelected = false, float radius = 8.0f);
    bool DrawNavTab(const wchar_t* text, D2D1_RECT_F rect, bool isSelected, bool isHovered);
    void DrawPillBadge(const wchar_t* text, D2D1_RECT_F rect, D2D1_COLOR_F accentColor);
    void DrawGlassPanel(D2D1_RECT_F rect, float radius = 12.0f, bool highlightBorder = false);
};

} // namespace miami
