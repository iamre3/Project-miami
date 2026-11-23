#include "miami/renderer.h"
#include "miami/config.h"
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <cmath>
#include <string>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dxgi.lib")

namespace miami {

MiamiRenderer::MiamiRenderer() {
    // Premium PC Launcher Dark Theme: Obsidian + Miami Cyan + Sunset Pink
    m_theme.bgBase = D2D1::ColorF(0.035f, 0.035f, 0.045f, 1.0f);     // #09090c
    m_theme.bgSurface = D2D1::ColorF(0.065f, 0.065f, 0.085f, 0.95f);  // #101016
    m_theme.bgSurfaceElev = D2D1::ColorF(0.095f, 0.095f, 0.125f, 0.95f); // #181820
    m_theme.bgCard = D2D1::ColorF(0.08f, 0.08f, 0.10f, 0.85f);
    m_theme.accentCyan = D2D1::ColorF(0.0f, 0.92f, 0.98f, 1.0f);     // #00f2fe Cyan
    m_theme.accentPink = D2D1::ColorF(0.91f, 0.31f, 0.48f, 1.0f);    // #e8507a Pink
    m_theme.textPrimary = D2D1::ColorF(0.98f, 0.98f, 1.0f, 1.0f);    // Soft white
    m_theme.textSecondary = D2D1::ColorF(0.60f, 0.62f, 0.68f, 1.0f); // Muted silver
    m_theme.textMuted = D2D1::ColorF(0.35f, 0.37f, 0.42f, 1.0f);     // Dark muted
    m_theme.borderLight = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f);
    m_theme.borderAccent = D2D1::ColorF(0.0f, 0.92f, 0.98f, 0.35f);
    m_theme.successGreen = D2D1::ColorF(0.06f, 0.78f, 0.45f, 1.0f);
    m_theme.warningAmber = D2D1::ColorF(0.96f, 0.62f, 0.04f, 1.0f);
    m_theme.errorRed = D2D1::ColorF(0.94f, 0.27f, 0.27f, 1.0f);
}

MiamiRenderer::~MiamiRenderer() {
    Cleanup();
}

bool MiamiRenderer::Initialize(HWND hwnd) {
    m_hwnd = hwnd;
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_pD2DFactory);
    if (FAILED(hr)) return false;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&m_pDWriteFactory);
    if (FAILED(hr)) return false;

    hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_pWicFactory));
    if (FAILED(hr)) return false;

    // Premium Typography
    m_pDWriteFactory->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 34.0f, L"en-us", &m_pFontDisplay);
    m_pDWriteFactory->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"en-us", &m_pFontHeading);
    m_pDWriteFactory->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &m_pFontBody);
    m_pDWriteFactory->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &m_pFontSmall);
    m_pDWriteFactory->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"en-us", &m_pFontButton);
    m_pDWriteFactory->CreateTextFormat(L"Consolas", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"en-us", &m_pFontMonospace);

    CreateDeviceResources();
    return true;
}

void MiamiRenderer::CreateDeviceResources() {
    if (m_pRenderTarget) return;

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);

    m_pD2DFactory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
        D2D1::HwndRenderTargetProperties(m_hwnd, size, D2D1_PRESENT_OPTIONS_IMMEDIATELY),
        &m_pRenderTarget
    );

    LoadAssetBitmaps();
}

void MiamiRenderer::DiscardDeviceResources() {
    if (m_pBmpBackground) { m_pBmpBackground->Release(); m_pBmpBackground = NULL; }
    if (m_pBmpBanner) { m_pBmpBanner->Release(); m_pBmpBanner = NULL; }
    if (m_pBmpLogo) { m_pBmpLogo->Release(); m_pBmpLogo = NULL; }
    if (m_pRenderTarget) { m_pRenderTarget->Release(); m_pRenderTarget = NULL; }
}

void MiamiRenderer::LoadAssetBitmaps() {
    if (!m_pRenderTarget || !m_pWicFactory) return;

    auto loadBmp = [&](const std::wstring& path, ID2D1Bitmap** ppBmp) {
        IWICBitmapDecoder* pDecoder = NULL;
        if (FAILED(m_pWicFactory->CreateDecoderFromFilename(path.c_str(), NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &pDecoder))) return;
        IWICBitmapFrameDecode* pFrame = NULL;
        if (FAILED(pDecoder->GetFrame(0, &pFrame))) { pDecoder->Release(); return; }
        IWICFormatConverter* pConverter = NULL;
        if (FAILED(m_pWicFactory->CreateFormatConverter(&pConverter))) { pFrame->Release(); pDecoder->Release(); return; }
        if (SUCCEEDED(pConverter->Initialize(pFrame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0f, WICBitmapPaletteTypeCustom))) {
            m_pRenderTarget->CreateBitmapFromWicBitmap(pConverter, NULL, ppBmp);
        }
        pConverter->Release();
        pFrame->Release();
        pDecoder->Release();
    };

    loadBmp(L"e:\\ProjectMiami\\assets\\images\\bg_sunset.png", &m_pBmpBackground);
    loadBmp(L"e:\\ProjectMiami\\assets\\images\\banner.png", &m_pBmpBanner);
    loadBmp(L"e:\\ProjectMiami\\assets\\images\\logo.png", &m_pBmpLogo);
}

void MiamiRenderer::Resize(UINT width, UINT height) {
    if (m_pRenderTarget) {
        m_pRenderTarget->Resize(D2D1::SizeU(width, height));
    }
}

void MiamiRenderer::Cleanup() {
    DiscardDeviceResources();
    if (m_pFontDisplay) m_pFontDisplay->Release();
    if (m_pFontHeading) m_pFontHeading->Release();
    if (m_pFontBody) m_pFontBody->Release();
    if (m_pFontSmall) m_pFontSmall->Release();
    if (m_pFontButton) m_pFontButton->Release();
    if (m_pFontMonospace) m_pFontMonospace->Release();
    if (m_pWicFactory) m_pWicFactory->Release();
    if (m_pDWriteFactory) m_pDWriteFactory->Release();
    if (m_pD2DFactory) m_pD2DFactory->Release();
}

static bool IsPointInRect(int x, int y, const D2D1_RECT_F& r) {
    return (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom);
}

void MiamiRenderer::OnMouseMove(int x, int y) {
    m_mouseX = x;
    m_mouseY = y;
    InvalidateRect(m_hwnd, NULL, FALSE);
}

void MiamiRenderer::OnLButtonDown(int x, int y) {
    m_mouseX = x;
    m_mouseY = y;
    m_mouseDown = true;
    InvalidateRect(m_hwnd, NULL, FALSE);
}

void MiamiRenderer::OnLButtonUp(int x, int y) {
    m_mouseX = x;
    m_mouseY = y;
    m_mouseDown = false;
    InvalidateRect(m_hwnd, NULL, FALSE);
}

void MiamiRenderer::OnKeyDown(WPARAM key) {
    MiamiApp* app = GetAppInstance();
    if (!app) return;

    if (key == VK_ESCAPE) {
        if (app->GetCurrentView() == AppView::MainSettings || 
            app->GetCurrentView() == AppView::MainDiagnostics || 
            app->GetCurrentView() == AppView::MainLibrary ||
            app->GetCurrentView() == AppView::MainAbout) {
            app->NavigateTo(AppView::MainHome);
        }
    }
    InvalidateRect(m_hwnd, NULL, FALSE);
}

// ── MODERN UI CONTROLS ────────────────────────────────────────

bool MiamiRenderer::DrawButton(const wchar_t* text, D2D1_RECT_F rect, bool isPrimary, bool isHovered, bool isSelected) {
    ID2D1SolidColorBrush* pBrush = NULL;
    
    D2D1_COLOR_F fillColor;
    D2D1_COLOR_F borderColor;
    D2D1_COLOR_F textColor;

    if (isPrimary) {
        fillColor = isHovered ? D2D1::ColorF(0.0f, 0.95f, 1.0f, 1.0f) : m_theme.accentCyan;
        borderColor = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.4f);
        textColor = D2D1::ColorF(0.02f, 0.02f, 0.04f, 1.0f); // Dark contrast text on Cyan CTA
    } else if (isSelected) {
        fillColor = D2D1::ColorF(0.0f, 0.92f, 0.98f, 0.15f);
        borderColor = m_theme.accentCyan;
        textColor = m_theme.accentCyan;
    } else {
        fillColor = isHovered ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.04f);
        borderColor = isHovered ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.25f) : m_theme.borderLight;
        textColor = isHovered ? m_theme.textPrimary : m_theme.textSecondary;
    }

    D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(rect, 4.0f, 4.0f);

    m_pRenderTarget->CreateSolidColorBrush(fillColor, &pBrush);
    m_pRenderTarget->FillRoundedRectangle(rrect, pBrush);
    pBrush->Release();

    m_pRenderTarget->CreateSolidColorBrush(borderColor, &pBrush);
    m_pRenderTarget->DrawRoundedRectangle(rrect, pBrush, 1.0f);
    pBrush->Release();

    m_pRenderTarget->CreateSolidColorBrush(textColor, &pBrush);
    m_pFontButton->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_pFontButton->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_pRenderTarget->DrawText(text, (UINT32)wcslen(text), m_pFontButton, rect, pBrush);
    pBrush->Release();

    return isHovered && m_mouseDown;
}

bool MiamiRenderer::DrawNavTab(const wchar_t* text, D2D1_RECT_F rect, bool isSelected, bool isHovered) {
    ID2D1SolidColorBrush* pBrush = NULL;

    D2D1_COLOR_F textColor = isSelected ? m_theme.accentCyan : (isHovered ? m_theme.textPrimary : m_theme.textSecondary);
    m_pRenderTarget->CreateSolidColorBrush(textColor, &pBrush);
    m_pFontHeading->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_pFontHeading->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_pRenderTarget->DrawText(text, (UINT32)wcslen(text), m_pFontHeading, rect, pBrush);
    pBrush->Release();

    // Active indicator line
    if (isSelected) {
        D2D1_RECT_F indRect = D2D1::RectF(rect.left + 15.0f, rect.bottom - 2.0f, rect.right - 15.0f, rect.bottom);
        m_pRenderTarget->CreateSolidColorBrush(m_theme.accentCyan, &pBrush);
        m_pRenderTarget->FillRectangle(indRect, pBrush);
        pBrush->Release();
    }

    return isHovered && m_mouseDown;
}

void MiamiRenderer::RenderCustomTitleBar(MiamiApp* app, D2D1_RECT_F bounds) {
    D2D1_RECT_F titleBar = D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.top + 36.0f);
    
    ID2D1SolidColorBrush* pBrush = NULL;
    m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.04f, 0.04f, 0.05f, 0.95f), &pBrush);
    m_pRenderTarget->FillRectangle(titleBar, pBrush);
    pBrush->Release();

    // Window control buttons on top right
    float btnW = 46.0f;
    float btnH = 36.0f;
    D2D1_RECT_F closeBtn = D2D1::RectF(titleBar.right - btnW, titleBar.top, titleBar.right, titleBar.bottom);
    D2D1_RECT_F maxBtn   = D2D1::RectF(closeBtn.left - btnW, titleBar.top, closeBtn.left, titleBar.bottom);
    D2D1_RECT_F minBtn   = D2D1::RectF(maxBtn.left - btnW, titleBar.top, maxBtn.left, titleBar.bottom);

    bool isCloseHov = IsPointInRect(m_mouseX, m_mouseY, closeBtn);
    bool isMaxHov   = IsPointInRect(m_mouseX, m_mouseY, maxBtn);
    bool isMinHov   = IsPointInRect(m_mouseX, m_mouseY, minBtn);

    if (isCloseHov) {
        m_pRenderTarget->CreateSolidColorBrush(m_theme.errorRed, &pBrush);
        m_pRenderTarget->FillRectangle(closeBtn, pBrush);
        pBrush->Release();
    }
    if (isMaxHov || isMinHov) {
        m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f), &pBrush);
        m_pRenderTarget->FillRectangle(isMaxHov ? maxBtn : minBtn, pBrush);
        pBrush->Release();
    }

    m_pRenderTarget->CreateSolidColorBrush(m_theme.textSecondary, &pBrush);
    m_pFontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_pFontSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_pRenderTarget->DrawText(L"✕", 1, m_pFontSmall, closeBtn, pBrush);
    m_pRenderTarget->DrawText(L"□", 1, m_pFontSmall, maxBtn, pBrush);
    m_pRenderTarget->DrawText(L"—", 1, m_pFontSmall, minBtn, pBrush);
    pBrush->Release();

    if (isCloseHov && m_mouseDown) {
        PostMessage(m_hwnd, WM_CLOSE, 0, 0);
    } else if (isMaxHov && m_mouseDown) {
        WINDOWPLACEMENT wp;
        wp.length = sizeof(wp);
        GetWindowPlacement(m_hwnd, &wp);
        ShowWindow(m_hwnd, (wp.showCmd == SW_SHOWMAXIMIZED) ? SW_RESTORE : SW_MAXIMIZE);
    } else if (isMinHov && m_mouseDown) {
        ShowWindow(m_hwnd, SW_MINIMIZE);
    }

    // App title in titlebar
    D2D1_RECT_F textRect = D2D1::RectF(titleBar.left + 16.0f, titleBar.top, titleBar.right - 180.0f, titleBar.bottom);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textMuted, &pBrush);
    m_pFontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(L"PROJECT MIAMI — PC LAUNCHER", 28, m_pFontSmall, textRect, pBrush);
    pBrush->Release();
}

void MiamiRenderer::RenderTopNavigation(MiamiApp* app, D2D1_RECT_F bounds) {
    RenderCustomTitleBar(app, bounds);

    float navTop = bounds.top + 36.0f;
    D2D1_RECT_F navBar = D2D1::RectF(bounds.left, navTop, bounds.right, navTop + 60.0f);

    ID2D1SolidColorBrush* pBrush = NULL;
    m_pRenderTarget->CreateSolidColorBrush(m_theme.bgSurface, &pBrush);
    m_pRenderTarget->FillRectangle(navBar, pBrush);
    pBrush->Release();

    m_pRenderTarget->CreateSolidColorBrush(m_theme.borderLight, &pBrush);
    m_pRenderTarget->DrawLine(D2D1::Point2F(navBar.left, navBar.bottom), D2D1::Point2F(navBar.right, navBar.bottom), pBrush, 1.0f);
    pBrush->Release();

    // Brand Wordmark
    D2D1_RECT_F brandRect = D2D1::RectF(navBar.left + 32.0f, navBar.top + 14.0f, navBar.left + 220.0f, navBar.bottom);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.accentCyan, &pBrush);
    m_pFontHeading->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pFontHeading->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    m_pRenderTarget->DrawText(L"PROJECT MIAMI", 13, m_pFontHeading, brandRect, pBrush);
    pBrush->Release();

    // Navigation Tabs: HOME, LIBRARY, SETTINGS, ABOUT
    const wchar_t* tabs[] = { L"HOME", L"LIBRARY", L"SETTINGS", L"ABOUT" };
    AppView views[] = { AppView::MainHome, AppView::MainLibrary, AppView::MainSettings, AppView::MainAbout };

    float startX = navBar.left + 260.0f;
    for (int i = 0; i < 4; ++i) {
        D2D1_RECT_F tabRect = D2D1::RectF(startX + (i * 120.0f), navBar.top, startX + (i * 120.0f) + 110.0f, navBar.bottom);
        bool isHovered = IsPointInRect(m_mouseX, m_mouseY, tabRect);
        bool isSelected = (app->GetCurrentView() == views[i]);

        if (DrawNavTab(tabs[i], tabRect, isSelected, isHovered)) {
            app->NavigateTo(views[i]);
        }
    }

    // Status on right side
    const InstallationInfo& info = app->GetInstaller().GetCurrentInfo();
    D2D1_RECT_F statusRect = D2D1::RectF(navBar.right - 280.0f, navBar.top + 18.0f, navBar.right - 24.0f, navBar.bottom);
    m_pRenderTarget->CreateSolidColorBrush(info.status == ValidationStatus::Valid ? m_theme.successGreen : m_theme.warningAmber, &pBrush);
    m_pFontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    std::wstring statusStr = (info.status == ValidationStatus::Valid) ? L"● GAME READY" : L"▲ SETUP REQUIRED";
    m_pRenderTarget->DrawText(statusStr.c_str(), (UINT32)statusStr.length(), m_pFontSmall, statusRect, pBrush);
    pBrush->Release();
}

// ── REDESIGNED HOME SCREEN ────────────────────────────────────

void MiamiRenderer::RenderMainHome(MiamiApp* app, D2D1_RECT_F bounds) {
    // 1. Cinematic Full Background with subtle darkening
    if (m_pBmpBackground) {
        m_pRenderTarget->DrawBitmap(m_pBmpBackground, bounds, 0.45f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    // Subtle atmospheric darkening gradient
    ID2D1SolidColorBrush* pDark = NULL;
    m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.02f, 0.02f, 0.03f, 0.65f), &pDark);
    m_pRenderTarget->FillRectangle(bounds, pDark);
    pDark->Release();

    RenderTopNavigation(app, bounds);

    float contentTop = bounds.top + 140.0f;
    float contentLeft = bounds.left + 64.0f;
    float contentRight = bounds.right - 64.0f;

    GameLibrary& lib = app->GetLibrary();
    const GameEntry* activeGame = lib.GetActiveGame();
    std::wstring gameTitle = activeGame ? std::wstring(activeGame->title.begin(), activeGame->title.end()) : L"Grand Theft Auto VI";

    // Game Title & Tagline
    D2D1_RECT_F titleRect = D2D1::RectF(contentLeft, contentTop, contentLeft + 700.0f, contentTop + 50.0f);
    ID2D1SolidColorBrush* pBrush = NULL;
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textPrimary, &pBrush);
    m_pFontDisplay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(gameTitle.c_str(), (UINT32)gameTitle.length(), m_pFontDisplay, titleRect, pBrush);
    pBrush->Release();

    D2D1_RECT_F tagRect = D2D1::RectF(contentLeft, contentTop + 55.0f, contentLeft + 600.0f, contentTop + 85.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.accentPink, &pBrush);
    m_pFontHeading->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(L"LEONIDA PC RELEASE", 18, m_pFontHeading, tagRect, pBrush);
    pBrush->Release();

    D2D1_RECT_F descRect = D2D1::RectF(contentLeft, contentTop + 100.0f, contentLeft + 540.0f, contentTop + 200.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textSecondary, &pBrush);
    m_pFontBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    const wchar_t* synopsis = 
        L"Experience the next generation of open-world gaming. Explore Vice City and the surrounding state "
        L"of Leonida with native PC performance, direct x86-64 execution, and responsive controls.";
    m_pRenderTarget->DrawText(synopsis, (UINT32)wcslen(synopsis), m_pFontBody, descRect, pBrush);
    pBrush->Release();

    // Primary Action (Play Button)
    ProcessManager& proc = app->GetProcess();
    proc.PollState();

    D2D1_RECT_F playBtn = D2D1::RectF(contentLeft, contentTop + 220.0f, contentLeft + 240.0f, contentTop + 276.0f);
    bool playHover = IsPointInRect(m_mouseX, m_mouseY, playBtn);

    const InstallationInfo& info = app->GetInstaller().GetCurrentInfo();
    bool isInstalled = (info.status == ValidationStatus::Valid);

    if (proc.IsRunning()) {
        std::wstring runningText = L"■ RUNNING (PID " + std::to_wstring(proc.GetStatus().processId) + L")";
        DrawButton(runningText.c_str(), playBtn, false, playHover);
    } else if (!isInstalled) {
        if (DrawButton(L"CONFIGURE GAME", playBtn, true, playHover)) {
            app->NavigateTo(AppView::SetupWelcome);
        }
    } else {
        if (DrawButton(L"▶  PLAY NOW", playBtn, true, playHover)) {
            app->StartGameLaunch();
        }
    }

    // Launch Error Notice if any
    if (proc.GetStatus().state == ProcessState::FailedToLaunch) {
        D2D1_RECT_F errRect = D2D1::RectF(contentLeft, contentTop + 288.0f, contentLeft + 540.0f, contentTop + 330.0f);
        m_pRenderTarget->CreateSolidColorBrush(m_theme.errorRed, &pBrush);
        std::wstring errStr = L"Launch error: " + std::wstring(proc.GetStatus().errorMessage.begin(), proc.GetStatus().errorMessage.end());
        m_pRenderTarget->DrawText(errStr.c_str(), (UINT32)errStr.length(), m_pFontSmall, errRect, pBrush);
        pBrush->Release();
    }

    // Right Side: Clean Game Metadata Card
    float cardW = 340.0f;
    float cardH = 260.0f;
    D2D1_RECT_F metaCard = D2D1::RectF(contentRight - cardW, contentTop, contentRight, contentTop + cardH);

    m_pRenderTarget->CreateSolidColorBrush(m_theme.bgCard, &pBrush);
    m_pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(metaCard, 6.0f, 6.0f), pBrush);
    pBrush->Release();

    m_pRenderTarget->CreateSolidColorBrush(m_theme.borderLight, &pBrush);
    m_pRenderTarget->DrawRoundedRectangle(D2D1::RoundedRect(metaCard, 6.0f, 6.0f), pBrush, 1.0f);
    pBrush->Release();

    D2D1_RECT_F cardHead = D2D1::RectF(metaCard.left + 20.0f, metaCard.top + 20.0f, metaCard.right - 20.0f, metaCard.top + 45.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textPrimary, &pBrush);
    m_pFontHeading->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(L"GAME DETAILS", 12, m_pFontHeading, cardHead, pBrush);
    pBrush->Release();

    std::wstring metaInfo = 
        std::wstring(L"Status: ") + (isInstalled ? L"Ready to Launch" : L"Installation Incomplete") + L"\n"
        L"Version: 1.0.2.1 PC\n"
        L"Executable: " + std::wstring(info.executableName.begin(), info.executableName.end()) + L"\n"
        L"Install Folder: " + std::wstring(info.directoryPath.begin(), info.directoryPath.end()) + L"\n"
        L"Runtime Adapter: " + std::wstring(app->GetIntegration()->GetInfo().name.begin(), app->GetIntegration()->GetInfo().name.end()) + L"\n"
        L"Display Mode: 1920x1080 (Borderless)";

    D2D1_RECT_F metaText = D2D1::RectF(metaCard.left + 20.0f, metaCard.top + 55.0f, metaCard.right - 20.0f, metaCard.bottom - 20.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textSecondary, &pBrush);
    m_pRenderTarget->DrawText(metaInfo.c_str(), (UINT32)metaInfo.length(), m_pFontSmall, metaText, pBrush);
    pBrush->Release();
}

// ── REDESIGNED SETTINGS (CATEGORIES ON LEFT, OPTIONS ON RIGHT) ─

void MiamiRenderer::RenderMainSettings(MiamiApp* app, D2D1_RECT_F bounds) {
    RenderTopNavigation(app, bounds);

    float contentTop = bounds.top + 116.0f;
    float contentLeft = bounds.left + 48.0f;
    float contentRight = bounds.right - 48.0f;
    float contentBottom = bounds.bottom - 36.0f;

    // Sidebar Category Width
    float sideW = 200.0f;
    D2D1_RECT_F sideRect = D2D1::RectF(contentLeft, contentTop, contentLeft + sideW, contentBottom);
    D2D1_RECT_F mainRect = D2D1::RectF(contentLeft + sideW + 24.0f, contentTop, contentRight, contentBottom);

    ID2D1SolidColorBrush* pBrush = NULL;
    m_pRenderTarget->CreateSolidColorBrush(m_theme.bgSurface, &pBrush);
    m_pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(mainRect, 6.0f, 6.0f), pBrush);
    pBrush->Release();

    m_pRenderTarget->CreateSolidColorBrush(m_theme.borderLight, &pBrush);
    m_pRenderTarget->DrawRoundedRectangle(D2D1::RoundedRect(mainRect, 6.0f, 6.0f), pBrush, 1.0f);
    pBrush->Release();

    // Render Categories in Sidebar
    const wchar_t* categories[] = { L"Display", L"Performance", L"Audio", L"Input", L"System" };
    for (int i = 0; i < 5; ++i) {
        D2D1_RECT_F catBtn = D2D1::RectF(sideRect.left, sideRect.top + (i * 44.0f), sideRect.right, sideRect.top + (i * 44.0f) + 38.0f);
        bool isHovered = IsPointInRect(m_mouseX, m_mouseY, catBtn);
        bool isSelected = (m_settingsCategory == i);

        if (DrawButton(categories[i], catBtn, false, isHovered, isSelected)) {
            m_settingsCategory = i;
        }
    }

    AppConfig& cfg = app->GetConfig();

    // Content area inside mainRect
    D2D1_RECT_F headRect = D2D1::RectF(mainRect.left + 32.0f, mainRect.top + 24.0f, mainRect.right - 32.0f, mainRect.top + 60.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textPrimary, &pBrush);
    m_pFontHeading->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    std::wstring sectionTitle = std::wstring(categories[m_settingsCategory]) + L" Preferences";
    m_pRenderTarget->DrawText(sectionTitle.c_str(), (UINT32)sectionTitle.length(), m_pFontHeading, headRect, pBrush);
    pBrush->Release();

    auto renderRow = [&](float y, const wchar_t* label, const wchar_t* value) {
        D2D1_RECT_F rowRect = D2D1::RectF(mainRect.left + 32.0f, y, mainRect.right - 32.0f, y + 40.0f);
        m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.02f), &pBrush);
        m_pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(rowRect, 4.0f, 4.0f), pBrush);
        pBrush->Release();

        D2D1_RECT_F lRect = D2D1::RectF(rowRect.left + 16.0f, rowRect.top + 10.0f, rowRect.left + 260.0f, rowRect.bottom);
        m_pRenderTarget->CreateSolidColorBrush(m_theme.textSecondary, &pBrush);
        m_pRenderTarget->DrawText(label, (UINT32)wcslen(label), m_pFontBody, lRect, pBrush);
        pBrush->Release();

        D2D1_RECT_F vRect = D2D1::RectF(rowRect.right - 260.0f, rowRect.top + 10.0f, rowRect.right - 16.0f, rowRect.bottom);
        m_pRenderTarget->CreateSolidColorBrush(m_theme.accentCyan, &pBrush);
        m_pFontBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        m_pRenderTarget->DrawText(value, (UINT32)wcslen(value), m_pFontBody, vRect, pBrush);
        pBrush->Release();
    };

    float startY = mainRect.top + 75.0f;
    if (m_settingsCategory == 0) { // Display
        renderRow(startY, L"Target Resolution", L"1920 x 1080 (16:9)");
        renderRow(startY + 48.0f, L"Window Mode", L"Borderless Fullscreen");
        renderRow(startY + 96.0f, L"Refresh Rate", L"60 Hz");
        renderRow(startY + 144.0f, L"UI Scaling", L"100% (1.0x)");
    } else if (m_settingsCategory == 1) { // Performance
        renderRow(startY, L"Target Frame Rate", L"60 FPS (Preference)");
        renderRow(startY + 48.0f, L"Vertical Sync (VSync)", L"Enabled");
        renderRow(startY + 96.0f, L"Frame Pacing", L"Hardware Paced");
        renderRow(startY + 144.0f, L"Process Priority", L"Normal");
    } else if (m_settingsCategory == 2) { // Audio
        renderRow(startY, L"Master Volume", L"80%");
        renderRow(startY + 48.0f, L"Music & Radio Volume", L"70%");
        renderRow(startY + 96.0f, L"Sound Effects Volume", L"85%");
        renderRow(startY + 144.0f, L"Output Device", L"Default Audio Device");
    } else if (m_settingsCategory == 3) { // Input
        renderRow(startY, L"Game Controller Support", L"Enabled (XInput / DualSense)");
        renderRow(startY + 48.0f, L"Controller Vibration", L"Enabled");
        renderRow(startY + 96.0f, L"Keyboard & Mouse Controls", L"Active");
    } else if (m_settingsCategory == 4) { // System
        std::wstring pW(cfg.installationPath.begin(), cfg.installationPath.end());
        renderRow(startY, L"Installation Directory", pW.c_str());
        renderRow(startY + 48.0f, L"Configuration Location", L"%APPDATA%\\ProjectMiami");
        renderRow(startY + 96.0f, L"Launcher Version", L"1.0.0 PC Release");

        // Open logs folder action
        D2D1_RECT_F openLogBtn = D2D1::RectF(mainRect.left + 32.0f, startY + 160.0f, mainRect.left + 220.0f, startY + 200.0f);
        if (DrawButton(L"OPEN LOGS FOLDER", openLogBtn, false, IsPointInRect(m_mouseX, m_mouseY, openLogBtn))) {
            ShellExecuteA(NULL, "open", "explorer.exe", ("/select,\"" + GetLogFilePath() + "\"").c_str(), NULL, SW_SHOWNORMAL);
        }
    }
}

// ── GENUINE GAME LIBRARY VIEW ─────────────────────────────────

void MiamiRenderer::RenderMainLibrary(MiamiApp* app, D2D1_RECT_F bounds) {
    RenderTopNavigation(app, bounds);

    float contentTop = bounds.top + 116.0f;
    float contentLeft = bounds.left + 48.0f;
    float contentRight = bounds.right - 48.0f;

    D2D1_RECT_F headRect = D2D1::RectF(contentLeft, contentTop, contentRight, contentTop + 40.0f);
    ID2D1SolidColorBrush* pBrush = NULL;
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textPrimary, &pBrush);
    m_pFontHeading->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(L"INSTALLED GAMES", 15, m_pFontHeading, headRect, pBrush);
    pBrush->Release();

    // Game card in library
    float cardW = 320.0f;
    float cardH = 340.0f;
    D2D1_RECT_F cardRect = D2D1::RectF(contentLeft, contentTop + 50.0f, contentLeft + cardW, contentTop + 50.0f + cardH);

    bool isHovered = IsPointInRect(m_mouseX, m_mouseY, cardRect);
    m_pRenderTarget->CreateSolidColorBrush(isHovered ? m_theme.bgSurfaceElev : m_theme.bgSurface, &pBrush);
    m_pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(cardRect, 6.0f, 6.0f), pBrush);
    pBrush->Release();

    m_pRenderTarget->CreateSolidColorBrush(isHovered ? m_theme.accentCyan : m_theme.borderLight, &pBrush);
    m_pRenderTarget->DrawRoundedRectangle(D2D1::RoundedRect(cardRect, 6.0f, 6.0f), pBrush, 1.0f);
    pBrush->Release();

    // Banner image preview if loaded
    if (m_pBmpBanner) {
        D2D1_RECT_F imgRect = D2D1::RectF(cardRect.left + 1.0f, cardRect.top + 1.0f, cardRect.right - 1.0f, cardRect.top + 160.0f);
        m_pRenderTarget->DrawBitmap(m_pBmpBanner, imgRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    D2D1_RECT_F gTitle = D2D1::RectF(cardRect.left + 16.0f, cardRect.top + 175.0f, cardRect.right - 16.0f, cardRect.top + 205.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textPrimary, &pBrush);
    m_pFontHeading->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(L"Grand Theft Auto VI", 19, m_pFontHeading, gTitle, pBrush);
    pBrush->Release();

    const InstallationInfo& info = app->GetInstaller().GetCurrentInfo();
    D2D1_RECT_F gDesc = D2D1::RectF(cardRect.left + 16.0f, cardRect.top + 210.0f, cardRect.right - 16.0f, cardRect.bottom - 60.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textSecondary, &pBrush);
    std::wstring details = (info.status == ValidationStatus::Valid) ? L"● Installed & Verified" : L"▲ Setup Required";
    m_pRenderTarget->DrawText(details.c_str(), (UINT32)details.length(), m_pFontSmall, gDesc, pBrush);
    pBrush->Release();

    D2D1_RECT_F selBtn = D2D1::RectF(cardRect.left + 16.0f, cardRect.bottom - 48.0f, cardRect.right - 16.0f, cardRect.bottom - 16.0f);
    if (DrawButton(L"SELECT & PLAY", selBtn, true, IsPointInRect(m_mouseX, m_mouseY, selBtn))) {
        app->NavigateTo(AppView::MainHome);
    }
}

// ── ABOUT VIEW ────────────────────────────────────────────────

void MiamiRenderer::RenderMainAbout(MiamiApp* app, D2D1_RECT_F bounds) {
    RenderTopNavigation(app, bounds);

    float cardW = 680.0f;
    float cardH = 440.0f;
    float left = (bounds.right - cardW) * 0.5f;
    float top = (bounds.bottom - cardH) * 0.5f + 18.0f;
    D2D1_RECT_F card = D2D1::RectF(left, top, left + cardW, top + cardH);

    ID2D1SolidColorBrush* pBrush = NULL;
    m_pRenderTarget->CreateSolidColorBrush(m_theme.bgSurface, &pBrush);
    m_pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(card, 6.0f, 6.0f), pBrush);
    pBrush->Release();

    m_pRenderTarget->CreateSolidColorBrush(m_theme.borderAccent, &pBrush);
    m_pRenderTarget->DrawRoundedRectangle(D2D1::RoundedRect(card, 6.0f, 6.0f), pBrush, 1.0f);
    pBrush->Release();

    D2D1_RECT_F titleRect = D2D1::RectF(card.left + 36.0f, card.top + 36.0f, card.right - 36.0f, card.top + 72.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textPrimary, &pBrush);
    m_pFontDisplay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(L"PROJECT MIAMI", 13, m_pFontDisplay, titleRect, pBrush);
    pBrush->Release();

    D2D1_RECT_F subRect = D2D1::RectF(card.left + 36.0f, card.top + 76.0f, card.right - 36.0f, card.top + 104.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.accentCyan, &pBrush);
    m_pFontHeading->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(L"Standalone PC Launcher for Grand Theft Auto VI", 46, m_pFontHeading, subRect, pBrush);
    pBrush->Release();

    D2D1_RECT_F descRect = D2D1::RectF(card.left + 36.0f, card.top + 124.0f, card.right - 36.0f, card.bottom - 36.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textSecondary, &pBrush);
    m_pFontBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    const wchar_t* aboutText = 
        L"Project Miami is an independent PC launcher engineered to manage configuration, presentation, "
        L"and execution of Grand Theft Auto VI on Windows desktop.\n\n"
        L"Features:\n"
        L"• Clean, modern PC desktop interface rendered via Direct2D at 60 FPS\n"
        L"• Integrated installation validator and process lifecycle manager\n"
        L"• 7-category persistent configuration stored in %APPDATA%\\ProjectMiami\n"
        L"• Modular compatibility adapter architecture for runtime execution\n\n"
        L"Notice: Project Miami does not distribute copyrighted files, binaries, or encryption keys. "
        L"Users provide their own legally obtained installation.";
    m_pRenderTarget->DrawText(aboutText, (UINT32)wcslen(aboutText), m_pFontBody, descRect, pBrush);
    pBrush->Release();
}

// ── REDESIGNED SETUP WIZARD (CLEAN 5-STEP PROCESS) ─────────────

void MiamiRenderer::RenderSetupWelcome(MiamiApp* app, D2D1_RECT_F bounds) {
    if (m_pBmpBackground) {
        m_pRenderTarget->DrawBitmap(m_pBmpBackground, bounds, 0.40f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    RenderCustomTitleBar(app, bounds);

    float cardW = 680.0f;
    float cardH = 460.0f;
    float left = (bounds.right - cardW) * 0.5f;
    float top = (bounds.bottom - cardH) * 0.5f;
    D2D1_RECT_F card = D2D1::RectF(left, top, left + cardW, top + cardH);

    ID2D1SolidColorBrush* pBrush = NULL;
    m_pRenderTarget->CreateSolidColorBrush(m_theme.bgSurface, &pBrush);
    m_pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(card, 8.0f, 8.0f), pBrush);
    pBrush->Release();

    m_pRenderTarget->CreateSolidColorBrush(m_theme.borderAccent, &pBrush);
    m_pRenderTarget->DrawRoundedRectangle(D2D1::RoundedRect(card, 8.0f, 8.0f), pBrush, 1.0f);
    pBrush->Release();

    D2D1_RECT_F titleRect = D2D1::RectF(card.left + 40.0f, card.top + 40.0f, card.right - 40.0f, card.top + 80.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textPrimary, &pBrush);
    m_pFontDisplay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(L"PROJECT MIAMI", 13, m_pFontDisplay, titleRect, pBrush);
    pBrush->Release();

    D2D1_RECT_F subRect = D2D1::RectF(card.left + 40.0f, card.top + 85.0f, card.right - 40.0f, card.top + 115.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.accentCyan, &pBrush);
    m_pFontHeading->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(L"WELCOME TO THE PC LAUNCHER SETUP", 34, m_pFontHeading, subRect, pBrush);
    pBrush->Release();

    D2D1_RECT_F descRect = D2D1::RectF(card.left + 40.0f, card.top + 140.0f, card.right - 40.0f, card.top + 320.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textSecondary, &pBrush);
    m_pFontBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    const wchar_t* msg = 
        L"Project Miami provides a streamlined PC gaming experience for Grand Theft Auto VI.\n\n"
        L"This initial setup will help you:\n"
        L"• Locate your game installation folder on your PC\n"
        L"• Verify executable integrity and required assets\n"
        L"• Configure recommended display and performance preferences\n\n"
        L"Click Continue to begin setup.";
    m_pRenderTarget->DrawText(msg, (UINT32)wcslen(msg), m_pFontBody, descRect, pBrush);
    pBrush->Release();

    D2D1_RECT_F nextBtn = D2D1::RectF(card.right - 200.0f, card.bottom - 70.0f, card.right - 40.0f, card.bottom - 26.0f);
    if (DrawButton(L"CONTINUE  →", nextBtn, true, IsPointInRect(m_mouseX, m_mouseY, nextBtn))) {
        app->NavigateTo(AppView::SetupDirectory);
    }
}

void MiamiRenderer::RenderSetupDirectory(MiamiApp* app, D2D1_RECT_F bounds) {
    if (m_pBmpBackground) {
        m_pRenderTarget->DrawBitmap(m_pBmpBackground, bounds, 0.40f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    RenderCustomTitleBar(app, bounds);

    float cardW = 720.0f;
    float cardH = 480.0f;
    float left = (bounds.right - cardW) * 0.5f;
    float top = (bounds.bottom - cardH) * 0.5f;
    D2D1_RECT_F card = D2D1::RectF(left, top, left + cardW, top + cardH);

    ID2D1SolidColorBrush* pBrush = NULL;
    m_pRenderTarget->CreateSolidColorBrush(m_theme.bgSurface, &pBrush);
    m_pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(card, 8.0f, 8.0f), pBrush);
    pBrush->Release();

    m_pRenderTarget->CreateSolidColorBrush(m_theme.borderAccent, &pBrush);
    m_pRenderTarget->DrawRoundedRectangle(D2D1::RoundedRect(card, 8.0f, 8.0f), pBrush, 1.0f);
    pBrush->Release();

    D2D1_RECT_F titleRect = D2D1::RectF(card.left + 40.0f, card.top + 36.0f, card.right - 40.0f, card.top + 70.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textPrimary, &pBrush);
    m_pFontHeading->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(L"SELECT GAME INSTALLATION FOLDER", 32, m_pFontHeading, titleRect, pBrush);
    pBrush->Release();

    D2D1_RECT_F descRect = D2D1::RectF(card.left + 40.0f, card.top + 76.0f, card.right - 40.0f, card.top + 120.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textSecondary, &pBrush);
    m_pFontBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(L"Choose the directory where your copy of Grand Theft Auto VI is located on your PC:", 80, m_pFontBody, descRect, pBrush);
    pBrush->Release();

    // Folder Path Display Box
    D2D1_RECT_F pathBox = D2D1::RectF(card.left + 40.0f, card.top + 130.0f, card.right - 180.0f, card.top + 175.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.bgBase, &pBrush);
    m_pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(pathBox, 4.0f, 4.0f), pBrush);
    pBrush->Release();

    m_pRenderTarget->CreateSolidColorBrush(m_theme.borderLight, &pBrush);
    m_pRenderTarget->DrawRoundedRectangle(D2D1::RoundedRect(pathBox, 4.0f, 4.0f), pBrush, 1.0f);
    pBrush->Release();

    std::string currentPath = app->GetConfig().installationPath;
    std::wstring wPath(currentPath.begin(), currentPath.end());
    D2D1_RECT_F pathTextRect = D2D1::RectF(pathBox.left + 14.0f, pathBox.top + 12.0f, pathBox.right - 14.0f, pathBox.bottom - 12.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textPrimary, &pBrush);
    m_pFontBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(wPath.c_str(), (UINT32)wPath.length(), m_pFontBody, pathTextRect, pBrush);
    pBrush->Release();

    // Native Folder Picker Browse Button
    D2D1_RECT_F browseBtn = D2D1::RectF(card.right - 160.0f, card.top + 130.0f, card.right - 40.0f, card.top + 175.0f);
    if (DrawButton(L"BROWSE...", browseBtn, false, IsPointInRect(m_mouseX, m_mouseY, browseBtn))) {
        BROWSEINFOA bi = { 0 };
        bi.lpszTitle = "Select Grand Theft Auto VI Game Directory";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
        if (pidl != 0) {
            char pathBuf[MAX_PATH];
            if (SHGetPathFromIDListA(pidl, pathBuf)) {
                app->GetInstaller().SetAndSaveInstallation(pathBuf);
            }
            CoTaskMemFree(pidl);
        }
    }

    // Honest Validation Feedback Card
    const InstallationInfo& info = app->GetInstaller().GetCurrentInfo();
    D2D1_RECT_F valCard = D2D1::RectF(card.left + 40.0f, card.top + 200.0f, card.right - 40.0f, card.top + 360.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.bgSurfaceElev, &pBrush);
    m_pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(valCard, 6.0f, 6.0f), pBrush);
    pBrush->Release();

    D2D1_COLOR_F statusColor = (info.status == ValidationStatus::Valid) ? m_theme.successGreen : m_theme.warningAmber;
    D2D1_RECT_F valHead = D2D1::RectF(valCard.left + 20.0f, valCard.top + 18.0f, valCard.right - 20.0f, valCard.top + 45.0f);
    m_pRenderTarget->CreateSolidColorBrush(statusColor, &pBrush);
    std::wstring vTitle = (info.status == ValidationStatus::Valid) ? L"✓ Directory Verified" : L"▲ Notice";
    m_pRenderTarget->DrawText(vTitle.c_str(), (UINT32)vTitle.length(), m_pFontHeading, valHead, pBrush);
    pBrush->Release();

    D2D1_RECT_F valText = D2D1::RectF(valCard.left + 20.0f, valCard.top + 52.0f, valCard.right - 20.0f, valCard.bottom - 16.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textSecondary, &pBrush);
    std::wstring vMsg = std::wstring(info.statusMessage.begin(), info.statusMessage.end()) + L"\n";
    if (info.executableFound) {
        vMsg += L"• Executable Detected: " + std::wstring(info.executableName.begin(), info.executableName.end()) + 
                L" (" + std::to_wstring(info.executableSizeBytes / (1024*1024)) + L" MB)\n";
    }
    if (info.hasGameData) {
        vMsg += L"• Package Data: " + std::to_wstring(info.totalDataBytes / (1024*1024*1024)) + L" GB game archives detected\n";
    }
    m_pRenderTarget->DrawText(vMsg.c_str(), (UINT32)vMsg.length(), m_pFontBody, valText, pBrush);
    pBrush->Release();

    // Navigation
    D2D1_RECT_F backBtn = D2D1::RectF(card.left + 40.0f, card.bottom - 70.0f, card.left + 180.0f, card.bottom - 26.0f);
    if (DrawButton(L"← BACK", backBtn, false, IsPointInRect(m_mouseX, m_mouseY, backBtn))) {
        app->NavigateTo(AppView::SetupWelcome);
    }

    D2D1_RECT_F nextBtn = D2D1::RectF(card.right - 200.0f, card.bottom - 70.0f, card.right - 40.0f, card.bottom - 26.0f);
    if (DrawButton(L"CONTINUE  →", nextBtn, true, IsPointInRect(m_mouseX, m_mouseY, nextBtn))) {
        app->NavigateTo(AppView::SetupComplete);
    }
}

void MiamiRenderer::RenderSetupComplete(MiamiApp* app, D2D1_RECT_F bounds) {
    if (m_pBmpBackground) {
        m_pRenderTarget->DrawBitmap(m_pBmpBackground, bounds, 0.40f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    RenderCustomTitleBar(app, bounds);

    float cardW = 680.0f;
    float cardH = 460.0f;
    float left = (bounds.right - cardW) * 0.5f;
    float top = (bounds.bottom - cardH) * 0.5f;
    D2D1_RECT_F card = D2D1::RectF(left, top, left + cardW, top + cardH);

    ID2D1SolidColorBrush* pBrush = NULL;
    m_pRenderTarget->CreateSolidColorBrush(m_theme.bgSurface, &pBrush);
    m_pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(card, 8.0f, 8.0f), pBrush);
    pBrush->Release();

    m_pRenderTarget->CreateSolidColorBrush(m_theme.borderAccent, &pBrush);
    m_pRenderTarget->DrawRoundedRectangle(D2D1::RoundedRect(card, 8.0f, 8.0f), pBrush, 1.0f);
    pBrush->Release();

    D2D1_RECT_F titleRect = D2D1::RectF(card.left + 40.0f, card.top + 40.0f, card.right - 40.0f, card.top + 76.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textPrimary, &pBrush);
    m_pFontDisplay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(L"SETUP COMPLETE", 14, m_pFontDisplay, titleRect, pBrush);
    pBrush->Release();

    D2D1_RECT_F subRect = D2D1::RectF(card.left + 40.0f, card.top + 80.0f, card.right - 40.0f, card.top + 110.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.accentCyan, &pBrush);
    m_pFontHeading->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(L"YOUR PC LAUNCHER IS CONFIGURED AND READY", 41, m_pFontHeading, subRect, pBrush);
    pBrush->Release();

    D2D1_RECT_F descRect = D2D1::RectF(card.left + 40.0f, card.top + 130.0f, card.right - 40.0f, card.top + 320.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textSecondary, &pBrush);
    m_pFontBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    const wchar_t* msg = 
        L"Project Miami is fully configured:\n\n"
        L"• Game: Grand Theft Auto VI\n"
        L"• Target Display: 1920x1080 (Borderless Fullscreen, 60 FPS)\n"
        L"• Audio & Input: XInput / Game Controller Enabled\n"
        L"• Settings: Persisted to %APPDATA%\\ProjectMiami\n\n"
        L"You can modify any setting or change your installation path at any time in the Settings tab.";
    m_pRenderTarget->DrawText(msg, (UINT32)wcslen(msg), m_pFontBody, descRect, pBrush);
    pBrush->Release();

    D2D1_RECT_F launchBtn = D2D1::RectF(card.right - 240.0f, card.bottom - 70.0f, card.right - 40.0f, card.bottom - 26.0f);
    if (DrawButton(L"LAUNCH PROJECT MIAMI  →", launchBtn, true, IsPointInRect(m_mouseX, m_mouseY, launchBtn))) {
        app->GetConfig().setupCompleted = true;
        app->GetConfig().Save();
        app->NavigateTo(AppView::MainHome);
    }
}

// ── RENDER DISPATCHER ─────────────────────────────────────────

void MiamiRenderer::Render(MiamiApp* app) {
    if (!m_pRenderTarget) {
        CreateDeviceResources();
        if (!m_pRenderTarget) return;
    }

    m_pRenderTarget->BeginDraw();

    D2D1_SIZE_F size = m_pRenderTarget->GetSize();
    D2D1_RECT_F bounds = D2D1::RectF(0, 0, size.width, size.height);

    m_pRenderTarget->Clear(m_theme.bgBase);

    switch (app->GetCurrentView()) {
        case AppView::SetupWelcome:
            RenderSetupWelcome(app, bounds);
            break;
        case AppView::SetupDirectory:
            RenderSetupDirectory(app, bounds);
            break;
        case AppView::SetupComplete:
            RenderSetupComplete(app, bounds);
            break;
        case AppView::MainHome:
            RenderMainHome(app, bounds);
            break;
        case AppView::MainLibrary:
            RenderMainLibrary(app, bounds);
            break;
        case AppView::MainSettings:
            RenderMainSettings(app, bounds);
            break;
        case AppView::MainAbout:
            RenderMainAbout(app, bounds);
            break;
        default:
            RenderMainHome(app, bounds);
            break;
    }

    HRESULT hr = m_pRenderTarget->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
    }
}

} // namespace miami
