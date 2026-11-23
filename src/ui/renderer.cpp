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
    m_theme.bgBase        = D2D1::ColorF(0.031f, 0.031f, 0.047f, 1.0f);   // #08080c
    m_theme.bgSurface     = D2D1::ColorF(0.055f, 0.055f, 0.082f, 0.92f);  // #0e0e15
    m_theme.bgSurfaceElev = D2D1::ColorF(0.086f, 0.086f, 0.125f, 0.94f);  // #161620
    m_theme.bgCard        = D2D1::ColorF(0.067f, 0.067f, 0.098f, 0.88f);  // #111119
    m_theme.accentCyan    = D2D1::ColorF(0.0f, 0.93f, 0.99f, 1.0f);       // #00edfc
    m_theme.accentPink    = D2D1::ColorF(0.91f, 0.31f, 0.48f, 1.0f);      // #e8507a
    m_theme.accentGold    = D2D1::ColorF(1.0f, 0.70f, 0.28f, 1.0f);       // #ffb347
    m_theme.textPrimary   = D2D1::ColorF(0.98f, 0.98f, 1.0f, 1.0f);
    m_theme.textSecondary = D2D1::ColorF(0.64f, 0.67f, 0.74f, 1.0f);
    m_theme.textMuted     = D2D1::ColorF(0.38f, 0.41f, 0.48f, 1.0f);
    m_theme.borderLight   = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f);
    m_theme.borderAccent  = D2D1::ColorF(0.0f, 0.93f, 0.99f, 0.40f);
    m_theme.successGreen  = D2D1::ColorF(0.06f, 0.78f, 0.51f, 1.0f);
    m_theme.warningAmber  = D2D1::ColorF(0.96f, 0.62f, 0.04f, 1.0f);
    m_theme.errorRed      = D2D1::ColorF(0.94f, 0.27f, 0.27f, 1.0f);
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

    // Typography hierarchy
    m_pDWriteFactory->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_BLACK,     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 42.0f, L"en-us", &m_pFontHero);
    m_pDWriteFactory->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_BOLD,      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 28.0f, L"en-us", &m_pFontDisplay);
    m_pDWriteFactory->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15.5f, L"en-us", &m_pFontHeading);
    m_pDWriteFactory->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL,    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.5f, L"en-us", &m_pFontBody);
    m_pDWriteFactory->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_MEDIUM,    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"en-us", &m_pFontSmall);
    m_pDWriteFactory->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_BOLD,      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.5f, L"en-us", &m_pFontButton);
    m_pDWriteFactory->CreateTextFormat(L"Consolas", NULL, DWRITE_FONT_WEIGHT_NORMAL,    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"en-us", &m_pFontMonospace);

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

    if (m_pRenderTarget) {
        m_pRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }

    LoadAssetBitmaps();
}

void MiamiRenderer::DiscardDeviceResources() {
    if (m_pBmpBackground) { m_pBmpBackground->Release(); m_pBmpBackground = NULL; }
    if (m_pBmpBanner)     { m_pBmpBanner->Release();     m_pBmpBanner = NULL; }
    if (m_pBmpLogo)       { m_pBmpLogo->Release();       m_pBmpLogo = NULL; }
    if (m_pBmpCharacters) { m_pBmpCharacters->Release(); m_pBmpCharacters = NULL; }
    if (m_pBmpSkyline)    { m_pBmpSkyline->Release();    m_pBmpSkyline = NULL; }
    if (m_pRenderTarget)  { m_pRenderTarget->Release();  m_pRenderTarget = NULL; }
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

    loadBmp(L"e:\\ProjectMiami\\assets\\images\\bg_sunset.png",  &m_pBmpBackground);
    loadBmp(L"e:\\ProjectMiami\\assets\\images\\banner.png",     &m_pBmpBanner);
    loadBmp(L"e:\\ProjectMiami\\assets\\images\\logo.png",       &m_pBmpLogo);
    loadBmp(L"e:\\ProjectMiami\\assets\\images\\luciajason.png", &m_pBmpCharacters);
    loadBmp(L"e:\\ProjectMiami\\assets\\images\\skyline.png",    &m_pBmpSkyline);
}

void MiamiRenderer::Resize(UINT width, UINT height) {
    if (m_pRenderTarget) {
        m_pRenderTarget->Resize(D2D1::SizeU(width, height));
    }
}

void MiamiRenderer::Cleanup() {
    DiscardDeviceResources();
    if (m_pFontHero)      m_pFontHero->Release();
    if (m_pFontDisplay)   m_pFontDisplay->Release();
    if (m_pFontHeading)   m_pFontHeading->Release();
    if (m_pFontBody)      m_pFontBody->Release();
    if (m_pFontSmall)     m_pFontSmall->Release();
    if (m_pFontButton)    m_pFontButton->Release();
    if (m_pFontMonospace) m_pFontMonospace->Release();
    if (m_pWicFactory)    m_pWicFactory->Release();
    if (m_pDWriteFactory) m_pDWriteFactory->Release();
    if (m_pD2DFactory)    m_pD2DFactory->Release();
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
    m_clickConsumed = false;
    InvalidateRect(m_hwnd, NULL, FALSE);
}

void MiamiRenderer::OnLButtonUp(int x, int y) {
    m_mouseX = x;
    m_mouseY = y;
    m_mouseDown = false;
    m_clickConsumed = false;
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

// ── CINEMATIC BACKDROP & ROUNDED WINDOW FRAME ─────────────────

void MiamiRenderer::RenderCinematicBackdrop(D2D1_RECT_F bounds, float imageOpacity) {
    // 1. Aspect-preserving cover background
    if (m_pBmpBackground) {
        D2D1_SIZE_F bmpSize = m_pBmpBackground->GetSize();
        float winW = bounds.right - bounds.left;
        float winH = bounds.bottom - bounds.top;
        float scaleX = winW / bmpSize.width;
        float scaleY = winH / bmpSize.height;
        float scale = (scaleX > scaleY) ? scaleX : scaleY;

        float drawW = bmpSize.width * scale;
        float drawH = bmpSize.height * scale;
        float offsetX = (winW - drawW) * 0.5f;
        float offsetY = (winH - drawH) * 0.5f;

        D2D1_RECT_F destRect = D2D1::RectF(offsetX, offsetY, offsetX + drawW, offsetY + drawH);
        m_pRenderTarget->DrawBitmap(m_pBmpBackground, destRect, imageOpacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    // 2. Smooth Left-to-Right Cinematic Vignette Gradient
    ID2D1LinearGradientBrush* pHorizGrad = NULL;
    ID2D1GradientStopCollection* pStopsH = NULL;
    D2D1_GRADIENT_STOP stopsH[3] = {
        { 0.0f,  D2D1::ColorF(0.025f, 0.025f, 0.040f, 0.94f) },
        { 0.55f, D2D1::ColorF(0.025f, 0.025f, 0.040f, 0.68f) },
        { 1.0f,  D2D1::ColorF(0.025f, 0.025f, 0.040f, 0.85f) }
    };
    m_pRenderTarget->CreateGradientStopCollection(stopsH, 3, &pStopsH);
    if (pStopsH) {
        m_pRenderTarget->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(bounds.right, 0)),
            pStopsH, &pHorizGrad);
        if (pHorizGrad) {
            m_pRenderTarget->FillRectangle(bounds, pHorizGrad);
            pHorizGrad->Release();
        }
        pStopsH->Release();
    }

    // 3. Top-to-Bottom Depth Vignette Gradient
    ID2D1LinearGradientBrush* pVertGrad = NULL;
    ID2D1GradientStopCollection* pStopsV = NULL;
    D2D1_GRADIENT_STOP stopsV[3] = {
        { 0.0f,  D2D1::ColorF(0.02f, 0.02f, 0.035f, 0.70f) },
        { 0.45f, D2D1::ColorF(0.02f, 0.02f, 0.035f, 0.15f) },
        { 1.0f,  D2D1::ColorF(0.02f, 0.02f, 0.035f, 0.96f) }
    };
    m_pRenderTarget->CreateGradientStopCollection(stopsV, 3, &pStopsV);
    if (pStopsV) {
        m_pRenderTarget->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, bounds.bottom)),
            pStopsV, &pVertGrad);
        if (pVertGrad) {
            m_pRenderTarget->FillRectangle(bounds, pVertGrad);
            pVertGrad->Release();
        }
        pStopsV->Release();
    }

    // 4. Subtle Miami Sunset Ambient Radial Accent
    m_animTime += 0.016f;
    float pulse = 0.07f + 0.02f * sinf(m_animTime * 1.2f);
    ID2D1RadialGradientBrush* pRadBrush = NULL;
    ID2D1GradientStopCollection* pRadStops = NULL;
    D2D1_GRADIENT_STOP rStops[2] = {
        { 0.0f, D2D1::ColorF(0.91f, 0.31f, 0.48f, pulse) },
        { 1.0f, D2D1::ColorF(0.02f, 0.02f, 0.03f, 0.0f) }
    };
    m_pRenderTarget->CreateGradientStopCollection(rStops, 2, &pRadStops);
    if (pRadStops) {
        m_pRenderTarget->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(
                D2D1::Point2F(bounds.right * 0.78f, bounds.bottom * 0.25f),
                D2D1::Point2F(0, 0), 520.0f, 420.0f),
            pRadStops, &pRadBrush);
        if (pRadBrush) {
            m_pRenderTarget->FillRectangle(bounds, pRadBrush);
            pRadBrush->Release();
        }
        pRadStops->Release();
    }
}

void MiamiRenderer::RenderRoundedWindowFrame(D2D1_RECT_F bounds) {
    WINDOWPLACEMENT wp = {};
    wp.length = sizeof(wp);
    GetWindowPlacement(m_hwnd, &wp);
    if (wp.showCmd == SW_SHOWMAXIMIZED) return;

    // Anti-aliased 12px radius outer border matching the 24px SetWindowRgn ellipse
    D2D1_RECT_F frameRect = D2D1::RectF(bounds.left + 1.0f, bounds.top + 1.0f, bounds.right - 1.0f, bounds.bottom - 1.0f);
    D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(frameRect, 12.0f, 12.0f);

    ID2D1LinearGradientBrush* pBorderGrad = NULL;
    ID2D1GradientStopCollection* pStops = NULL;
    D2D1_GRADIENT_STOP stops[3] = {
        { 0.0f, D2D1::ColorF(0.0f, 0.93f, 0.99f, 0.35f) },
        { 0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f) },
        { 1.0f, D2D1::ColorF(0.91f, 0.31f, 0.48f, 0.35f) }
    };
    m_pRenderTarget->CreateGradientStopCollection(stops, 3, &pStops);
    if (pStops) {
        m_pRenderTarget->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(bounds.right, bounds.bottom)),
            pStops, &pBorderGrad);
        if (pBorderGrad) {
            m_pRenderTarget->DrawRoundedRectangle(rrect, pBorderGrad, 1.5f);
            pBorderGrad->Release();
        }
        pStops->Release();
    }
}

// ── MODERN UI CONTROLS ────────────────────────────────────────

void MiamiRenderer::DrawGlassPanel(D2D1_RECT_F rect, float radius, bool highlightBorder) {
    ID2D1SolidColorBrush* pBrush = NULL;
    D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(rect, radius, radius);

    m_pRenderTarget->CreateSolidColorBrush(m_theme.bgCard, &pBrush);
    m_pRenderTarget->FillRoundedRectangle(rrect, pBrush);
    pBrush->Release();

    m_pRenderTarget->CreateSolidColorBrush(highlightBorder ? m_theme.borderAccent : m_theme.borderLight, &pBrush);
    m_pRenderTarget->DrawRoundedRectangle(rrect, pBrush, 1.0f);
    pBrush->Release();
}

void MiamiRenderer::DrawPillBadge(const wchar_t* text, D2D1_RECT_F rect, D2D1_COLOR_F accentColor) {
    ID2D1SolidColorBrush* pBrush = NULL;
    float radius = (rect.bottom - rect.top) * 0.5f;
    D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(rect, radius, radius);

    D2D1_COLOR_F bgCol = D2D1::ColorF(accentColor.r, accentColor.g, accentColor.b, 0.14f);
    D2D1_COLOR_F borderCol = D2D1::ColorF(accentColor.r, accentColor.g, accentColor.b, 0.45f);

    m_pRenderTarget->CreateSolidColorBrush(bgCol, &pBrush);
    m_pRenderTarget->FillRoundedRectangle(pill, pBrush);
    pBrush->Release();

    m_pRenderTarget->CreateSolidColorBrush(borderCol, &pBrush);
    m_pRenderTarget->DrawRoundedRectangle(pill, pBrush, 1.0f);
    pBrush->Release();

    m_pRenderTarget->CreateSolidColorBrush(accentColor, &pBrush);
    m_pFontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_pFontSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_pRenderTarget->DrawText(text, (UINT32)wcslen(text), m_pFontSmall, rect, pBrush);
    pBrush->Release();
}

bool MiamiRenderer::DrawButton(const wchar_t* text, D2D1_RECT_F rect, bool isPrimary, bool isHovered, bool isSelected, float radius) {
    ID2D1SolidColorBrush* pBrush = NULL;
    D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(rect, radius, radius);

    if (isPrimary) {
        // Gradient CTA button (Cyan -> Pink subtle blend or bright Miami Cyan)
        ID2D1LinearGradientBrush* pGrad = NULL;
        ID2D1GradientStopCollection* pStops = NULL;
        D2D1_GRADIENT_STOP stops[2] = {
            { 0.0f, isHovered ? D2D1::ColorF(0.15f, 0.97f, 1.0f, 1.0f) : D2D1::ColorF(0.0f, 0.90f, 0.98f, 1.0f) },
            { 1.0f, isHovered ? D2D1::ColorF(0.0f, 0.82f, 0.95f, 1.0f) : D2D1::ColorF(0.0f, 0.72f, 0.88f, 1.0f) }
        };
        m_pRenderTarget->CreateGradientStopCollection(stops, 2, &pStops);
        if (pStops) {
            m_pRenderTarget->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(rect.left, rect.top), D2D1::Point2F(rect.right, rect.bottom)),
                pStops, &pGrad);
            if (pGrad) {
                m_pRenderTarget->FillRoundedRectangle(rrect, pGrad);
                pGrad->Release();
            }
            pStops->Release();
        }

        m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, isHovered ? 0.55f : 0.25f), &pBrush);
        m_pRenderTarget->DrawRoundedRectangle(rrect, pBrush, 1.2f);
        pBrush->Release();

        m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.02f, 0.03f, 0.05f, 1.0f), &pBrush);
        m_pFontButton->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_pFontButton->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_pRenderTarget->DrawText(text, (UINT32)wcslen(text), m_pFontButton, rect, pBrush);
        pBrush->Release();
    } else {
        D2D1_COLOR_F fillColor;
        D2D1_COLOR_F borderColor;
        D2D1_COLOR_F textColor;

        if (isSelected) {
            fillColor   = D2D1::ColorF(0.0f, 0.93f, 0.99f, 0.16f);
            borderColor = m_theme.accentCyan;
            textColor   = m_theme.accentCyan;
        } else {
            fillColor   = isHovered ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.11f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.045f);
            borderColor = isHovered ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f) : m_theme.borderLight;
            textColor   = isHovered ? m_theme.textPrimary : m_theme.textSecondary;
        }

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
    }

    if (isHovered && m_mouseDown && !m_clickConsumed) {
        m_clickConsumed = true;
        return true;
    }
    return false;
}

bool MiamiRenderer::DrawNavTab(const wchar_t* text, D2D1_RECT_F rect, bool isSelected, bool isHovered) {
    ID2D1SolidColorBrush* pBrush = NULL;

    if (isSelected || isHovered) {
        D2D1_RECT_F pillBg = D2D1::RectF(rect.left + 6.0f, rect.top + 12.0f, rect.right - 6.0f, rect.bottom - 12.0f);
        m_pRenderTarget->CreateSolidColorBrush(
            isSelected ? D2D1::ColorF(0.0f, 0.93f, 0.99f, 0.12f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f),
            &pBrush);
        m_pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(pillBg, 6.0f, 6.0f), pBrush);
        pBrush->Release();
    }

    D2D1_COLOR_F textColor = isSelected ? m_theme.accentCyan : (isHovered ? m_theme.textPrimary : m_theme.textSecondary);
    m_pRenderTarget->CreateSolidColorBrush(textColor, &pBrush);
    m_pFontButton->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_pFontButton->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_pRenderTarget->DrawText(text, (UINT32)wcslen(text), m_pFontButton, rect, pBrush);
    pBrush->Release();

    if (isSelected) {
        D2D1_RECT_F indRect = D2D1::RectF(rect.left + 22.0f, rect.bottom - 3.0f, rect.right - 22.0f, rect.bottom);
        m_pRenderTarget->CreateSolidColorBrush(m_theme.accentCyan, &pBrush);
        m_pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(indRect, 1.5f, 1.5f), pBrush);
        pBrush->Release();
    }

    if (isHovered && m_mouseDown && !m_clickConsumed) {
        m_clickConsumed = true;
        return true;
    }
    return false;
}

void MiamiRenderer::RenderCustomTitleBar(MiamiApp* app, D2D1_RECT_F bounds) {
    (void)app;
    D2D1_RECT_F titleBar = D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.top + 38.0f);
    
    ID2D1SolidColorBrush* pBrush = NULL;
    m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.03f, 0.03f, 0.045f, 0.88f), &pBrush);
    m_pRenderTarget->FillRectangle(titleBar, pBrush);
    pBrush->Release();

    // Window control pill buttons on top right
    float btnW = 38.0f;
    float btnH = 26.0f;
    float topPad = titleBar.top + 6.0f;
    D2D1_RECT_F closeBtn = D2D1::RectF(titleBar.right - btnW - 10.0f, topPad, titleBar.right - 10.0f, topPad + btnH);
    D2D1_RECT_F maxBtn   = D2D1::RectF(closeBtn.left - btnW - 4.0f, topPad, closeBtn.left - 4.0f, topPad + btnH);
    D2D1_RECT_F minBtn   = D2D1::RectF(maxBtn.left - btnW - 4.0f, topPad, maxBtn.left - 4.0f, topPad + btnH);

    bool isCloseHov = IsPointInRect(m_mouseX, m_mouseY, closeBtn);
    bool isMaxHov   = IsPointInRect(m_mouseX, m_mouseY, maxBtn);
    bool isMinHov   = IsPointInRect(m_mouseX, m_mouseY, minBtn);

    if (isCloseHov) {
        m_pRenderTarget->CreateSolidColorBrush(m_theme.errorRed, &pBrush);
        m_pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(closeBtn, 6.0f, 6.0f), pBrush);
        pBrush->Release();
    }
    if (isMaxHov || isMinHov) {
        m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f), &pBrush);
        m_pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(isMaxHov ? maxBtn : minBtn, 6.0f, 6.0f), pBrush);
        pBrush->Release();
    }

    m_pRenderTarget->CreateSolidColorBrush(isCloseHov ? m_theme.textPrimary : m_theme.textSecondary, &pBrush);
    m_pFontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_pFontSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_pRenderTarget->DrawText(L"✕", 1, m_pFontSmall, closeBtn, pBrush);
    pBrush->Release();

    m_pRenderTarget->CreateSolidColorBrush(m_theme.textSecondary, &pBrush);
    m_pRenderTarget->DrawText(L"□", 1, m_pFontSmall, maxBtn, pBrush);
    m_pRenderTarget->DrawText(L"—", 1, m_pFontSmall, minBtn, pBrush);
    pBrush->Release();

    if (isCloseHov && m_mouseDown && !m_clickConsumed) {
        m_clickConsumed = true;
        PostMessage(m_hwnd, WM_CLOSE, 0, 0);
    } else if (isMaxHov && m_mouseDown && !m_clickConsumed) {
        m_clickConsumed = true;
        WINDOWPLACEMENT wp;
        wp.length = sizeof(wp);
        GetWindowPlacement(m_hwnd, &wp);
        ShowWindow(m_hwnd, (wp.showCmd == SW_SHOWMAXIMIZED) ? SW_RESTORE : SW_MAXIMIZE);
    } else if (isMinHov && m_mouseDown && !m_clickConsumed) {
        m_clickConsumed = true;
        ShowWindow(m_hwnd, SW_MINIMIZE);
    }

    // Accent indicator dot + Title
    D2D1_ELLIPSE dot = D2D1::Ellipse(D2D1::Point2F(titleBar.left + 20.0f, titleBar.top + 19.0f), 4.0f, 4.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.accentPink, &pBrush);
    m_pRenderTarget->FillEllipse(dot, pBrush);
    pBrush->Release();

    D2D1_RECT_F textRect = D2D1::RectF(titleBar.left + 32.0f, titleBar.top, titleBar.right - 180.0f, titleBar.bottom);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textSecondary, &pBrush);
    m_pFontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(L"PROJECT MIAMI  •  DESKTOP GAME LAUNCHER", 39, m_pFontSmall, textRect, pBrush);
    pBrush->Release();
}

void MiamiRenderer::RenderTopNavigation(MiamiApp* app, D2D1_RECT_F bounds) {
    RenderCustomTitleBar(app, bounds);

    float navTop = bounds.top + 38.0f;
    D2D1_RECT_F navBar = D2D1::RectF(bounds.left, navTop, bounds.right, navTop + 58.0f);

    ID2D1SolidColorBrush* pBrush = NULL;
    m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.045f, 0.045f, 0.068f, 0.85f), &pBrush);
    m_pRenderTarget->FillRectangle(navBar, pBrush);
    pBrush->Release();

    m_pRenderTarget->CreateSolidColorBrush(m_theme.borderLight, &pBrush);
    m_pRenderTarget->DrawLine(D2D1::Point2F(navBar.left + 24.0f, navBar.bottom), D2D1::Point2F(navBar.right - 24.0f, navBar.bottom), pBrush, 1.0f);
    pBrush->Release();

    // Brand Wordmark
    D2D1_RECT_F brandRect = D2D1::RectF(navBar.left + 36.0f, navBar.top, navBar.left + 220.0f, navBar.bottom);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textPrimary, &pBrush);
    m_pFontHeading->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pFontHeading->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_pRenderTarget->DrawText(L"PROJECT MIAMI", 13, m_pFontHeading, brandRect, pBrush);
    pBrush->Release();

    // Navigation Tabs: HOME, LIBRARY, SETTINGS, ABOUT
    const wchar_t* tabs[] = { L"HOME", L"LIBRARY", L"SETTINGS", L"ABOUT" };
    AppView views[] = { AppView::MainHome, AppView::MainLibrary, AppView::MainSettings, AppView::MainAbout };

    float startX = navBar.left + 230.0f;
    for (int i = 0; i < 4; ++i) {
        D2D1_RECT_F tabRect = D2D1::RectF(startX + (i * 116.0f), navBar.top, startX + (i * 116.0f) + 110.0f, navBar.bottom);
        bool isHovered = IsPointInRect(m_mouseX, m_mouseY, tabRect);
        bool isSelected = (app->GetCurrentView() == views[i]);

        if (DrawNavTab(tabs[i], tabRect, isSelected, isHovered)) {
            app->NavigateTo(views[i]);
        }
    }

    // Status Pill on Right Side
    const InstallationInfo& info = app->GetInstaller().GetCurrentInfo();
    bool isReady = (info.status == ValidationStatus::Valid);
    D2D1_RECT_F pillRect = D2D1::RectF(navBar.right - 190.0f, navBar.top + 15.0f, navBar.right - 36.0f, navBar.bottom - 15.0f);
    DrawPillBadge(isReady ? L"●  READY TO PLAY" : L"▲  FILES REQUIRED",
                  pillRect,
                  isReady ? m_theme.successGreen : m_theme.warningAmber);
}

// ── REDESIGNED HOME SCREEN ────────────────────────────────────

void MiamiRenderer::RenderMainHome(MiamiApp* app, D2D1_RECT_F bounds) {
    RenderCinematicBackdrop(bounds, 0.58f);

    // Subtle character artwork on right side of hero canvas
    if (m_pBmpCharacters) {
        D2D1_SIZE_F cSize = m_pBmpCharacters->GetSize();
        float targetH = (bounds.bottom - bounds.top) * 0.78f;
        float targetW = targetH * (cSize.width / cSize.height);
        D2D1_RECT_F charRect = D2D1::RectF(
            bounds.right - targetW - 20.0f,
            bounds.bottom - targetH,
            bounds.right - 20.0f,
            bounds.bottom
        );
        m_pRenderTarget->DrawBitmap(m_pBmpCharacters, charRect, 0.35f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    RenderTopNavigation(app, bounds);

    float contentTop   = bounds.top + 135.0f;
    float contentLeft  = bounds.left + 64.0f;
    float contentRight = bounds.right - 64.0f;

    // 1. Official Logo Artwork or Hero Display Text
    float heroCursorY = contentTop;
    if (m_pBmpLogo) {
        D2D1_SIZE_F logoSz = m_pBmpLogo->GetSize();
        float logoH = 140.0f;
        float logoW = logoH * (logoSz.width / logoSz.height);
        D2D1_RECT_F logoRect = D2D1::RectF(contentLeft, heroCursorY, contentLeft + logoW, heroCursorY + logoH);
        m_pRenderTarget->DrawBitmap(m_pBmpLogo, logoRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        heroCursorY += logoH + 14.0f;
    }

    // Edition Pill Badge
    D2D1_RECT_F editionPill = D2D1::RectF(contentLeft, heroCursorY, contentLeft + 210.0f, heroCursorY + 26.0f);
    DrawPillBadge(L"LEONIDA PC EDITION  •  v1.0.2.1", editionPill, m_theme.accentPink);
    heroCursorY += 38.0f;

    // Hero Title
    GameLibrary& lib = app->GetLibrary();
    const GameEntry* activeGame = lib.GetActiveGame();
    std::wstring gameTitle = activeGame ? std::wstring(activeGame->title.begin(), activeGame->title.end()) : L"Grand Theft Auto VI";

    ID2D1SolidColorBrush* pBrush = NULL;
    D2D1_RECT_F titleRect = D2D1::RectF(contentLeft, heroCursorY, contentLeft + 680.0f, heroCursorY + 54.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textPrimary, &pBrush);
    m_pFontHero->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pFontHero->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    m_pRenderTarget->DrawText(gameTitle.c_str(), (UINT32)gameTitle.length(), m_pFontHero, titleRect, pBrush);
    pBrush->Release();
    heroCursorY += 62.0f;

    // Synopsis
    D2D1_RECT_F descRect = D2D1::RectF(contentLeft, heroCursorY, contentLeft + 520.0f, heroCursorY + 80.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textSecondary, &pBrush);
    m_pFontBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pFontBody->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    const wchar_t* synopsis = 
        L"Explore the sun-soaked streets of Vice City and the vast state of Leonida. "
        L"Configured for native Windows desktop execution with Direct3D 12 presentation and low-latency input.";
    m_pRenderTarget->DrawText(synopsis, (UINT32)wcslen(synopsis), m_pFontBody, descRect, pBrush);
    pBrush->Release();
    heroCursorY += 92.0f;

    // Primary Action Row (Play CTA + Quick Settings Button)
    ProcessManager& proc = app->GetProcess();
    proc.PollState();

    D2D1_RECT_F playBtn = D2D1::RectF(contentLeft, heroCursorY, contentLeft + 230.0f, heroCursorY + 54.0f);
    bool playHover = IsPointInRect(m_mouseX, m_mouseY, playBtn);

    const InstallationInfo& info = app->GetInstaller().GetCurrentInfo();
    bool isInstalled = (info.status == ValidationStatus::Valid);

    if (proc.IsRunning()) {
        std::wstring runningText = L"■  RUNNING (PID " + std::to_wstring(proc.GetStatus().processId) + L")";
        DrawButton(runningText.c_str(), playBtn, false, playHover, true, 10.0f);
    } else if (!isInstalled) {
        if (DrawButton(L"LOCATE GTA 6 FILES", playBtn, true, playHover, false, 10.0f)) {
            app->NavigateTo(AppView::SetupDirectory);
        }
    } else {
        if (DrawButton(L"▶   PLAY NOW", playBtn, true, playHover, false, 10.0f)) {
            app->StartGameLaunch();
        }
    }

    D2D1_RECT_F quickCfgBtn = D2D1::RectF(playBtn.right + 14.0f, heroCursorY, playBtn.right + 164.0f, heroCursorY + 54.0f);
    if (DrawButton(L"SETTINGS", quickCfgBtn, false, IsPointInRect(m_mouseX, m_mouseY, quickCfgBtn), false, 10.0f)) {
        app->NavigateTo(AppView::MainSettings);
    }

    if (proc.GetStatus().state == ProcessState::FailedToLaunch) {
        D2D1_RECT_F errRect = D2D1::RectF(contentLeft, heroCursorY + 64.0f, contentLeft + 540.0f, heroCursorY + 100.0f);
        m_pRenderTarget->CreateSolidColorBrush(m_theme.errorRed, &pBrush);
        std::wstring errStr = L"Launch error: " + std::wstring(proc.GetStatus().errorMessage.begin(), proc.GetStatus().errorMessage.end());
        m_pRenderTarget->DrawText(errStr.c_str(), (UINT32)errStr.length(), m_pFontSmall, errRect, pBrush);
        pBrush->Release();
    }

    // Right Side: Floating Glass Details Card
    float cardW = 360.0f;
    float cardH = 310.0f;
    D2D1_RECT_F metaCard = D2D1::RectF(contentRight - cardW, contentTop + 20.0f, contentRight, contentTop + 20.0f + cardH);
    DrawGlassPanel(metaCard, 12.0f, true);

    if (m_pBmpBanner) {
        D2D1_RECT_F bannerRect = D2D1::RectF(metaCard.left + 12.0f, metaCard.top + 12.0f, metaCard.right - 12.0f, metaCard.top + 115.0f);
        m_pRenderTarget->DrawBitmap(m_pBmpBanner, bannerRect, 0.92f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    D2D1_RECT_F cardHead = D2D1::RectF(metaCard.left + 20.0f, metaCard.top + 128.0f, metaCard.right - 20.0f, metaCard.top + 152.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textPrimary, &pBrush);
    m_pFontHeading->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pFontHeading->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    m_pRenderTarget->DrawText(L"INSTALLATION SUMMARY", 20, m_pFontHeading, cardHead, pBrush);
    pBrush->Release();

    AppConfig& cfg = app->GetConfig();
    std::wstring resStr = std::to_wstring(cfg.display.resolutionWidth) + L"x" + std::to_wstring(cfg.display.resolutionHeight) +
                          (cfg.display.windowMode == 1 ? L" (Borderless)" : (cfg.display.windowMode == 2 ? L" (Fullscreen)" : L" (Windowed)"));

    std::wstring exeNameW = info.executableName.empty() ? L"game_scarlett_master.exe" : std::wstring(info.executableName.begin(), info.executableName.end());
    std::wstring metaInfo = 
        std::wstring(L"Status: ") + (isInstalled ? L"Verified & Ready" : L"Action Required") + L"\n"
        L"Binary: " + exeNameW + L"\n"
        L"Location: " + std::wstring(info.directoryPath.begin(), info.directoryPath.end()) + L"\n"
        L"Display: " + resStr + L"\n"
        L"Target Rate: " + (cfg.performance.targetFps == 0 ? L"Unlimited" : (std::to_wstring(cfg.performance.targetFps) + L" FPS")) + L"\n"
        L"Runtime: XENITH + Leonida + ScarlettCompat";

    D2D1_RECT_F metaText = D2D1::RectF(metaCard.left + 20.0f, metaCard.top + 158.0f, metaCard.right - 20.0f, metaCard.bottom - 16.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textSecondary, &pBrush);
    m_pFontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pFontSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    m_pRenderTarget->DrawText(metaInfo.c_str(), (UINT32)metaInfo.length(), m_pFontSmall, metaText, pBrush);
    pBrush->Release();

    // Bottom Dock Bar
    D2D1_RECT_F bottomBar = D2D1::RectF(contentLeft, bounds.bottom - 58.0f, contentRight, bounds.bottom - 20.0f);
    DrawGlassPanel(bottomBar, 8.0f, false);

    const SystemHardwareInfo& hw = app->GetHardwareInfo();
    std::wstring hwLine = L"HOST PC:  " + std::wstring(hw.cpuModel.begin(), hw.cpuModel.end()) +
                          L"   •   GPU: " + std::wstring(hw.gpuModel.begin(), hw.gpuModel.end()) +
                          L"   •   RAM: " + std::to_wstring(hw.totalRamMb / 1024) + L" GB";
    D2D1_RECT_F hwRect = D2D1::RectF(bottomBar.left + 18.0f, bottomBar.top, bottomBar.right - 18.0f, bottomBar.bottom);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textMuted, &pBrush);
    m_pFontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pFontSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_pRenderTarget->DrawText(hwLine.c_str(), (UINT32)hwLine.length(), m_pFontSmall, hwRect, pBrush);
    pBrush->Release();
}

// ── INTERACTIVE SETTINGS SCREEN ───────────────────────────────

void MiamiRenderer::RenderMainSettings(MiamiApp* app, D2D1_RECT_F bounds) {
    RenderCinematicBackdrop(bounds, 0.25f);
    RenderTopNavigation(app, bounds);

    float contentTop    = bounds.top + 116.0f;
    float contentLeft   = bounds.left + 48.0f;
    float contentRight  = bounds.right - 48.0f;
    float contentBottom = bounds.bottom - 36.0f;

    float sideW = 210.0f;
    D2D1_RECT_F sideRect = D2D1::RectF(contentLeft, contentTop, contentLeft + sideW, contentBottom);
    D2D1_RECT_F mainRect = D2D1::RectF(contentLeft + sideW + 24.0f, contentTop, contentRight, contentBottom);

    DrawGlassPanel(mainRect, 12.0f, false);

    // Sidebar Categories
    const wchar_t* categories[] = { L"Display", L"Performance", L"Audio", L"Input", L"System" };
    for (int i = 0; i < 5; ++i) {
        D2D1_RECT_F catBtn = D2D1::RectF(sideRect.left, sideRect.top + (i * 48.0f), sideRect.right, sideRect.top + (i * 48.0f) + 40.0f);
        bool isHovered = IsPointInRect(m_mouseX, m_mouseY, catBtn);
        bool isSelected = (m_settingsCategory == i);

        if (DrawButton(categories[i], catBtn, false, isHovered, isSelected, 8.0f)) {
            m_settingsCategory = i;
        }
    }

    AppConfig& cfg = app->GetConfig();
    ID2D1SolidColorBrush* pBrush = NULL;

    D2D1_RECT_F headRect = D2D1::RectF(mainRect.left + 32.0f, mainRect.top + 24.0f, mainRect.right - 32.0f, mainRect.top + 58.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textPrimary, &pBrush);
    m_pFontDisplay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pFontDisplay->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    std::wstring sectionTitle = std::wstring(categories[m_settingsCategory]) + L" Settings";
    m_pRenderTarget->DrawText(sectionTitle.c_str(), (UINT32)sectionTitle.length(), m_pFontDisplay, headRect, pBrush);
    pBrush->Release();

    // Interactive setting row helper (click row button to cycle/toggle and auto-save)
    auto renderInteractiveRow = [&](float y, const wchar_t* label, const wchar_t* desc, const std::wstring& valueStr) -> bool {
        D2D1_RECT_F rowRect = D2D1::RectF(mainRect.left + 32.0f, y, mainRect.right - 32.0f, y + 58.0f);
        m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.03f), &pBrush);
        m_pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(rowRect, 8.0f, 8.0f), pBrush);
        pBrush->Release();

        D2D1_RECT_F lRect = D2D1::RectF(rowRect.left + 18.0f, rowRect.top + 10.0f, rowRect.right - 240.0f, rowRect.top + 30.0f);
        m_pRenderTarget->CreateSolidColorBrush(m_theme.textPrimary, &pBrush);
        m_pFontHeading->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        m_pFontHeading->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        m_pRenderTarget->DrawText(label, (UINT32)wcslen(label), m_pFontHeading, lRect, pBrush);
        pBrush->Release();

        D2D1_RECT_F dRect = D2D1::RectF(rowRect.left + 18.0f, rowRect.top + 32.0f, rowRect.right - 240.0f, rowRect.bottom - 6.0f);
        m_pRenderTarget->CreateSolidColorBrush(m_theme.textSecondary, &pBrush);
        m_pFontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        m_pFontSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        m_pRenderTarget->DrawText(desc, (UINT32)wcslen(desc), m_pFontSmall, dRect, pBrush);
        pBrush->Release();

        D2D1_RECT_F valBtn = D2D1::RectF(rowRect.right - 210.0f, rowRect.top + 11.0f, rowRect.right - 16.0f, rowRect.bottom - 11.0f);
        return DrawButton(valueStr.c_str(), valBtn, false, IsPointInRect(m_mouseX, m_mouseY, valBtn), true, 6.0f);
    };

    float startY = mainRect.top + 80.0f;
    if (m_settingsCategory == 0) { // Display
        std::wstring resLabel = std::to_wstring(cfg.display.resolutionWidth) + L" x " + std::to_wstring(cfg.display.resolutionHeight);
        if (renderInteractiveRow(startY, L"Display Resolution", L"Target rendering resolution for game presentation (Click to cycle)", resLabel)) {
            if (cfg.display.resolutionWidth == 1920)      { cfg.display.resolutionWidth = 2560; cfg.display.resolutionHeight = 1440; }
            else if (cfg.display.resolutionWidth == 2560) { cfg.display.resolutionWidth = 3840; cfg.display.resolutionHeight = 2160; }
            else                                          { cfg.display.resolutionWidth = 1920; cfg.display.resolutionHeight = 1080; }
            cfg.Save();
        }

        std::wstring modeLabel = (cfg.display.windowMode == 0) ? L"Windowed" : ((cfg.display.windowMode == 1) ? L"Borderless" : L"Fullscreen");
        if (renderInteractiveRow(startY + 68.0f, L"Window Mode", L"Choose between windowed, borderless fullscreen, or exclusive mode", modeLabel)) {
            cfg.display.windowMode = (cfg.display.windowMode + 1) % 3;
            cfg.Save();
        }

        std::wstring hzLabel = std::to_wstring(cfg.display.refreshRate) + L" Hz";
        if (renderInteractiveRow(startY + 136.0f, L"Refresh Rate", L"Monitor refresh rate preference", hzLabel)) {
            if (cfg.display.refreshRate == 60)       cfg.display.refreshRate = 120;
            else if (cfg.display.refreshRate == 120) cfg.display.refreshRate = 144;
            else                                     cfg.display.refreshRate = 60;
            cfg.Save();
        }
    } else if (m_settingsCategory == 1) { // Performance
        std::wstring fpsLabel = (cfg.performance.targetFps == 0) ? L"Unlimited" : (std::to_wstring(cfg.performance.targetFps) + L" FPS");
        if (renderInteractiveRow(startY, L"Target Frame Rate", L"Preferred frame rate cap (target preference, not guaranteed)", fpsLabel)) {
            if (cfg.performance.targetFps == 30)       cfg.performance.targetFps = 60;
            else if (cfg.performance.targetFps == 60)  cfg.performance.targetFps = 120;
            else if (cfg.performance.targetFps == 120) cfg.performance.targetFps = 0;
            else                                       cfg.performance.targetFps = 30;
            cfg.Save();
        }

        if (renderInteractiveRow(startY + 68.0f, L"Vertical Sync (VSync)", L"Synchronize frame presentation with display refresh", cfg.performance.vsync ? L"Enabled" : L"Disabled")) {
            cfg.performance.vsync = !cfg.performance.vsync;
            cfg.Save();
        }

        if (renderInteractiveRow(startY + 136.0f, L"Hardware Frame Pacing", L"Smooth frame delivery intervals using high-resolution timers", cfg.performance.framePacing ? L"Enabled" : L"Disabled")) {
            cfg.performance.framePacing = !cfg.performance.framePacing;
            cfg.Save();
        }
    } else if (m_settingsCategory == 2) { // Audio
        std::wstring masterStr = std::to_wstring(cfg.audio.masterVolume) + L"%";
        if (renderInteractiveRow(startY, L"Master Volume", L"Overall audio output level (Click to adjust)", masterStr)) {
            cfg.audio.masterVolume = (cfg.audio.masterVolume + 10) > 100 ? 0 : (cfg.audio.masterVolume + 10);
            cfg.Save();
        }

        std::wstring musicStr = std::to_wstring(cfg.audio.musicVolume) + L"%";
        if (renderInteractiveRow(startY + 68.0f, L"Music & Radio Volume", L"In-game radio stations and background score volume", musicStr)) {
            cfg.audio.musicVolume = (cfg.audio.musicVolume + 10) > 100 ? 0 : (cfg.audio.musicVolume + 10);
            cfg.Save();
        }

        if (renderInteractiveRow(startY + 136.0f, L"Mute All Audio", L"Quickly silence all audio output", cfg.audio.mute ? L"Muted" : L"Active")) {
            cfg.audio.mute = !cfg.audio.mute;
            cfg.Save();
        }
    } else if (m_settingsCategory == 3) { // Input
        if (renderInteractiveRow(startY, L"Controller Support", L"Enable XInput and GameInput controller detection", cfg.input.enableController ? L"Enabled" : L"Disabled")) {
            cfg.input.enableController = !cfg.input.enableController;
            cfg.Save();
        }

        if (renderInteractiveRow(startY + 68.0f, L"Vibration & Haptics", L"Controller rumble and trigger feedback", cfg.input.vibration ? L"Enabled" : L"Disabled")) {
            cfg.input.vibration = !cfg.input.vibration;
            cfg.Save();
        }
    } else if (m_settingsCategory == 4) { // System
        std::wstring pW(cfg.installationPath.begin(), cfg.installationPath.end());
        if (renderInteractiveRow(startY, L"Game Installation Folder", L"Click to re-open the installation folder selector", pW)) {
            app->NavigateTo(AppView::SetupDirectory);
        }

        D2D1_RECT_F openLogBtn = D2D1::RectF(mainRect.left + 32.0f, startY + 84.0f, mainRect.left + 230.0f, startY + 128.0f);
        if (DrawButton(L"OPEN LOGS FOLDER", openLogBtn, false, IsPointInRect(m_mouseX, m_mouseY, openLogBtn))) {
            ShellExecuteA(NULL, "open", "explorer.exe", ("/select,\"" + GetLogFilePath() + "\"").c_str(), NULL, SW_SHOWNORMAL);
        }

        D2D1_RECT_F resetBtn = D2D1::RectF(openLogBtn.right + 16.0f, startY + 84.0f, openLogBtn.right + 216.0f, startY + 128.0f);
        if (DrawButton(L"RESET DEFAULTS", resetBtn, false, IsPointInRect(m_mouseX, m_mouseY, resetBtn))) {
            cfg.ResetDefaults();
            cfg.Save();
        }
    }
}

// ── GENUINE GAME LIBRARY VIEW ─────────────────────────────────

void MiamiRenderer::RenderMainLibrary(MiamiApp* app, D2D1_RECT_F bounds) {
    RenderCinematicBackdrop(bounds, 0.25f);
    RenderTopNavigation(app, bounds);

    float contentTop  = bounds.top + 120.0f;
    float contentLeft = bounds.left + 56.0f;

    ID2D1SolidColorBrush* pBrush = NULL;
    D2D1_RECT_F headRect = D2D1::RectF(contentLeft, contentTop, bounds.right - 56.0f, contentTop + 40.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textPrimary, &pBrush);
    m_pFontDisplay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(L"GAME LIBRARY", 12, m_pFontDisplay, headRect, pBrush);
    pBrush->Release();

    float cardW = 340.0f;
    float cardH = 360.0f;
    D2D1_RECT_F cardRect = D2D1::RectF(contentLeft, contentTop + 54.0f, contentLeft + cardW, contentTop + 54.0f + cardH);
    bool isHovered = IsPointInRect(m_mouseX, m_mouseY, cardRect);

    DrawGlassPanel(cardRect, 12.0f, isHovered);

    if (m_pBmpBanner) {
        D2D1_RECT_F imgRect = D2D1::RectF(cardRect.left + 10.0f, cardRect.top + 10.0f, cardRect.right - 10.0f, cardRect.top + 170.0f);
        m_pRenderTarget->DrawBitmap(m_pBmpBanner, imgRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    D2D1_RECT_F gTitle = D2D1::RectF(cardRect.left + 20.0f, cardRect.top + 185.0f, cardRect.right - 20.0f, cardRect.top + 215.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textPrimary, &pBrush);
    m_pFontHeading->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(L"Grand Theft Auto VI", 19, m_pFontHeading, gTitle, pBrush);
    pBrush->Release();

    const InstallationInfo& info = app->GetInstaller().GetCurrentInfo();
    bool isValid = (info.status == ValidationStatus::Valid);

    D2D1_RECT_F badgeRect = D2D1::RectF(cardRect.left + 20.0f, cardRect.top + 222.0f, cardRect.left + 190.0f, cardRect.top + 246.0f);
    DrawPillBadge(isValid ? L"●  VERIFIED BUILD" : L"▲  SETUP REQUIRED", badgeRect, isValid ? m_theme.successGreen : m_theme.warningAmber);

    D2D1_RECT_F selBtn = D2D1::RectF(cardRect.left + 20.0f, cardRect.bottom - 56.0f, cardRect.right - 20.0f, cardRect.bottom - 16.0f);
    if (DrawButton(L"OPEN GAME PAGE", selBtn, true, IsPointInRect(m_mouseX, m_mouseY, selBtn), false, 8.0f)) {
        app->NavigateTo(AppView::MainHome);
    }
}

// ── ABOUT VIEW ────────────────────────────────────────────────

void MiamiRenderer::RenderMainAbout(MiamiApp* app, D2D1_RECT_F bounds) {
    RenderCinematicBackdrop(bounds, 0.30f);
    RenderTopNavigation(app, bounds);

    float cardW = 720.0f;
    float cardH = 460.0f;
    float left = (bounds.right - cardW) * 0.5f;
    float top = (bounds.bottom - cardH) * 0.5f + 24.0f;
    D2D1_RECT_F card = D2D1::RectF(left, top, left + cardW, top + cardH);

    DrawGlassPanel(card, 14.0f, true);

    ID2D1SolidColorBrush* pBrush = NULL;
    D2D1_RECT_F titleRect = D2D1::RectF(card.left + 40.0f, card.top + 36.0f, card.right - 40.0f, card.top + 72.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textPrimary, &pBrush);
    m_pFontDisplay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(L"PROJECT MIAMI", 13, m_pFontDisplay, titleRect, pBrush);
    pBrush->Release();

    D2D1_RECT_F subRect = D2D1::RectF(card.left + 40.0f, card.top + 76.0f, card.right - 40.0f, card.top + 104.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.accentCyan, &pBrush);
    m_pFontHeading->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(L"Standalone PC Game Launcher  •  v1.0.0", 38, m_pFontHeading, subRect, pBrush);
    pBrush->Release();

    D2D1_RECT_F descRect = D2D1::RectF(card.left + 40.0f, card.top + 124.0f, card.right - 40.0f, card.bottom - 36.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textSecondary, &pBrush);
    m_pFontBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    const wchar_t* aboutText = 
        L"Project Miami is a native Windows PC desktop launcher engineered for Grand Theft Auto VI.\n\n"
        L"Integrated Runtime Engines (Compiled & Linked In-Process):\n"
        L"• XENITH Compatibility Core: 6-Layer Backend, PE32+ Loader, GDK HLE Shims & VFS Mounts\n"
        L"• Leonida Recompilation Runtime: User-SGPR Root Signature Packer, D3D12_XS Flag Sanitizer & GFX10 Detiler\n"
        L"• GTA6-COMPAT-LAYER (ScarlettCompat.dll): 13 HLE Export Bindings & 6 Shim Import Libraries\n\n"
        L"Notice: Project Miami does not distribute copyrighted files, binaries, or encryption keys. "
        L"Users supply their own legally obtained game installation.";
    m_pRenderTarget->DrawText(aboutText, (UINT32)wcslen(aboutText), m_pFontBody, descRect, pBrush);
    pBrush->Release();
}

// ── SETUP WIZARD ──────────────────────────────────────────────

void MiamiRenderer::RenderSetupWelcome(MiamiApp* app, D2D1_RECT_F bounds) {
    RenderCinematicBackdrop(bounds, 0.45f);
    RenderCustomTitleBar(app, bounds);

    float cardW = 680.0f;
    float cardH = 460.0f;
    float left = (bounds.right - cardW) * 0.5f;
    float top = (bounds.bottom - cardH) * 0.5f + 10.0f;
    D2D1_RECT_F card = D2D1::RectF(left, top, left + cardW, top + cardH);

    DrawGlassPanel(card, 14.0f, true);

    ID2D1SolidColorBrush* pBrush = NULL;
    D2D1_RECT_F pillRect = D2D1::RectF(card.left + 40.0f, card.top + 36.0f, card.left + 190.0f, card.top + 60.0f);
    DrawPillBadge(L"STEP 1 OF 3  •  WELCOME", pillRect, m_theme.accentCyan);

    D2D1_RECT_F titleRect = D2D1::RectF(card.left + 40.0f, card.top + 74.0f, card.right - 40.0f, card.top + 114.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textPrimary, &pBrush);
    m_pFontDisplay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(L"WELCOME TO PROJECT MIAMI", 24, m_pFontDisplay, titleRect, pBrush);
    pBrush->Release();

    D2D1_RECT_F descRect = D2D1::RectF(card.left + 40.0f, card.top + 135.0f, card.right - 40.0f, card.top + 330.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textSecondary, &pBrush);
    m_pFontBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    const wchar_t* msg = 
        L"Project Miami configures and launches your copy of Grand Theft Auto VI on Windows PC.\n\n"
        L"Before entering the launcher, this setup wizard will:\n"
        L"• Verify your Grand Theft Auto VI installation directory\n"
        L"• Validate the master executable and game package archives\n"
        L"• Initialize XENITH, Leonida, and ScarlettCompat runtime bindings\n\n"
        L"Click Continue to select your game folder.";
    m_pRenderTarget->DrawText(msg, (UINT32)wcslen(msg), m_pFontBody, descRect, pBrush);
    pBrush->Release();

    D2D1_RECT_F nextBtn = D2D1::RectF(card.right - 210.0f, card.bottom - 72.0f, card.right - 40.0f, card.bottom - 26.0f);
    if (DrawButton(L"CONTINUE   →", nextBtn, true, IsPointInRect(m_mouseX, m_mouseY, nextBtn), false, 8.0f)) {
        app->NavigateTo(AppView::SetupDirectory);
    }
}

void MiamiRenderer::RenderSetupDirectory(MiamiApp* app, D2D1_RECT_F bounds) {
    RenderCinematicBackdrop(bounds, 0.45f);
    RenderCustomTitleBar(app, bounds);

    float cardW = 740.0f;
    float cardH = 500.0f;
    float left = (bounds.right - cardW) * 0.5f;
    float top = (bounds.bottom - cardH) * 0.5f + 10.0f;
    D2D1_RECT_F card = D2D1::RectF(left, top, left + cardW, top + cardH);

    DrawGlassPanel(card, 14.0f, true);

    ID2D1SolidColorBrush* pBrush = NULL;
    D2D1_RECT_F pillRect = D2D1::RectF(card.left + 40.0f, card.top + 32.0f, card.left + 230.0f, card.top + 56.0f);
    DrawPillBadge(L"STEP 2 OF 3  •  GAME FILES", pillRect, m_theme.accentCyan);

    D2D1_RECT_F titleRect = D2D1::RectF(card.left + 40.0f, card.top + 68.0f, card.right - 40.0f, card.top + 104.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textPrimary, &pBrush);
    m_pFontDisplay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(L"LOCATE GRAND THEFT AUTO VI", 26, m_pFontDisplay, titleRect, pBrush);
    pBrush->Release();

    D2D1_RECT_F pathBox = D2D1::RectF(card.left + 40.0f, card.top + 125.0f, card.right - 180.0f, card.top + 170.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.bgBase, &pBrush);
    m_pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(pathBox, 8.0f, 8.0f), pBrush);
    pBrush->Release();

    m_pRenderTarget->CreateSolidColorBrush(m_theme.borderLight, &pBrush);
    m_pRenderTarget->DrawRoundedRectangle(D2D1::RoundedRect(pathBox, 8.0f, 8.0f), pBrush, 1.0f);
    pBrush->Release();

    std::string currentPath = app->GetConfig().installationPath;
    std::wstring wPath(currentPath.begin(), currentPath.end());
    D2D1_RECT_F pathTextRect = D2D1::RectF(pathBox.left + 16.0f, pathBox.top + 12.0f, pathBox.right - 16.0f, pathBox.bottom - 12.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textPrimary, &pBrush);
    m_pFontMonospace->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(wPath.c_str(), (UINT32)wPath.length(), m_pFontMonospace, pathTextRect, pBrush);
    pBrush->Release();

    D2D1_RECT_F browseBtn = D2D1::RectF(card.right - 165.0f, card.top + 125.0f, card.right - 40.0f, card.top + 170.0f);
    if (DrawButton(L"BROWSE...", browseBtn, false, IsPointInRect(m_mouseX, m_mouseY, browseBtn), false, 8.0f)) {
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

    const InstallationInfo& info = app->GetInstaller().GetCurrentInfo();
    D2D1_RECT_F valCard = D2D1::RectF(card.left + 40.0f, card.top + 195.0f, card.right - 40.0f, card.top + 380.0f);
    DrawGlassPanel(valCard, 10.0f, info.status == ValidationStatus::Valid);

    D2D1_COLOR_F statusColor = (info.status == ValidationStatus::Valid) ? m_theme.successGreen : m_theme.warningAmber;
    D2D1_RECT_F valHead = D2D1::RectF(valCard.left + 20.0f, valCard.top + 18.0f, valCard.right - 20.0f, valCard.top + 45.0f);
    m_pRenderTarget->CreateSolidColorBrush(statusColor, &pBrush);
    std::wstring vTitle = (info.status == ValidationStatus::Valid) ? L"✓  Authentic GTA 6 Files Verified" : L"▲  Required Game Files Not Found";
    m_pRenderTarget->DrawText(vTitle.c_str(), (UINT32)vTitle.length(), m_pFontHeading, valHead, pBrush);
    pBrush->Release();

    D2D1_RECT_F valText = D2D1::RectF(valCard.left + 20.0f, valCard.top + 52.0f, valCard.right - 20.0f, valCard.bottom - 16.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textSecondary, &pBrush);
    std::wstring vMsg = std::wstring(info.statusMessage.begin(), info.statusMessage.end()) + L"\n";
    if (info.executableFound) {
        vMsg += L"• Master Binary: " + std::wstring(info.executableName.begin(), info.executableName.end()) + 
                L" (" + std::to_wstring(info.executableSizeBytes / (1024*1024)) + L" MB, PE64 + .xbld verified)\n";
    }
    if (info.hasGameData) {
        vMsg += L"• Data Archives: " + std::to_wstring(info.totalDataBytes / (1024*1024*1024)) + L" GB package data detected\n";
    }
    m_pRenderTarget->DrawText(vMsg.c_str(), (UINT32)vMsg.length(), m_pFontBody, valText, pBrush);
    pBrush->Release();

    D2D1_RECT_F backBtn = D2D1::RectF(card.left + 40.0f, card.bottom - 72.0f, card.left + 180.0f, card.bottom - 26.0f);
    if (DrawButton(L"←  BACK", backBtn, false, IsPointInRect(m_mouseX, m_mouseY, backBtn), false, 8.0f)) {
        app->NavigateTo(AppView::SetupWelcome);
    }

    D2D1_RECT_F nextBtn = D2D1::RectF(card.right - 210.0f, card.bottom - 72.0f, card.right - 40.0f, card.bottom - 26.0f);
    bool canProceed = (info.status == ValidationStatus::Valid);
    if (canProceed) {
        if (DrawButton(L"CONTINUE   →", nextBtn, true, IsPointInRect(m_mouseX, m_mouseY, nextBtn), false, 8.0f)) {
            app->NavigateTo(AppView::SetupComplete);
        }
    } else {
        DrawButton(L"FILES REQUIRED", nextBtn, false, false, false, 8.0f);
    }
}

void MiamiRenderer::RenderSetupComplete(MiamiApp* app, D2D1_RECT_F bounds) {
    RenderCinematicBackdrop(bounds, 0.45f);
    RenderCustomTitleBar(app, bounds);

    float cardW = 680.0f;
    float cardH = 460.0f;
    float left = (bounds.right - cardW) * 0.5f;
    float top = (bounds.bottom - cardH) * 0.5f + 10.0f;
    D2D1_RECT_F card = D2D1::RectF(left, top, left + cardW, top + cardH);

    DrawGlassPanel(card, 14.0f, true);

    ID2D1SolidColorBrush* pBrush = NULL;
    D2D1_RECT_F pillRect = D2D1::RectF(card.left + 40.0f, card.top + 36.0f, card.left + 210.0f, card.top + 60.0f);
    DrawPillBadge(L"STEP 3 OF 3  •  READY", pillRect, m_theme.successGreen);

    D2D1_RECT_F titleRect = D2D1::RectF(card.left + 40.0f, card.top + 74.0f, card.right - 40.0f, card.top + 114.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textPrimary, &pBrush);
    m_pFontDisplay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pRenderTarget->DrawText(L"SETUP COMPLETE", 14, m_pFontDisplay, titleRect, pBrush);
    pBrush->Release();

    D2D1_RECT_F descRect = D2D1::RectF(card.left + 40.0f, card.top + 130.0f, card.right - 40.0f, card.top + 330.0f);
    m_pRenderTarget->CreateSolidColorBrush(m_theme.textSecondary, &pBrush);
    m_pFontBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    const wchar_t* msg = 
        L"Project Miami is ready to launch Grand Theft Auto VI:\n\n"
        L"• Game Files: Verified PE64 Master Binary & Package Archives\n"
        L"• Runtime Engines: XENITH + Leonida + ScarlettCompat Linked In-Process\n"
        L"• Target Display: 1920x1080 (Borderless, 60 FPS VSync)\n"
        L"• Audio & Input: XInput / GameInput Bridge Enabled\n\n"
        L"Click Enter Launcher to open the main home screen.";
    m_pRenderTarget->DrawText(msg, (UINT32)wcslen(msg), m_pFontBody, descRect, pBrush);
    pBrush->Release();

    D2D1_RECT_F launchBtn = D2D1::RectF(card.right - 240.0f, card.bottom - 72.0f, card.right - 40.0f, card.bottom - 26.0f);
    if (DrawButton(L"ENTER LAUNCHER   →", launchBtn, true, IsPointInRect(m_mouseX, m_mouseY, launchBtn), false, 8.0f)) {
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

    RenderRoundedWindowFrame(bounds);

    HRESULT hr = m_pRenderTarget->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
    }
}

} // namespace miami
