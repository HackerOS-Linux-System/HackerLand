#pragma once

#include <QObject>
#include <QRect>
#include <QSize>
#include <QPoint>
#include <QColor>
#include <QImage>
#include <QPixmap>
#include <QHash>
#include <QList>
#include <QFont>

#include "compositor/WindowRenderState.h"   // ← shared, no redefinition

class WMCompositor;
class Window;
class Workspace;
class QPainter;

// ─────────────────────────────────────────────────────────────────────────────
// RenderEngine
//
// Centralises all QPainter-based drawing that WMOutput previously had inline.
// WMOutput creates one RenderEngine and forwards its paintGL() call here.
// RenderEngine owns no Qt widget — it accepts a QPainter& and viewport QRect.
//
// Draw pipeline (one full frame):
//   1.  drawWallpaper()            fill / fit / center / tile modes
//   2.  drawVignette()             full-screen edge-darkening overlay
//   3.  drawWindows()              for each visible window, bottom → top:
//         drawWindowShadow()
//         drawWindowBlurBackground()
//         drawWindowGlassOverlay()
//         drawWindowBorder()
//         drawTitleBar()
//         drawTitleBarSeparator()
//   4.  drawCursor()               software cursor drawn above everything
// ─────────────────────────────────────────────────────────────────────────────
class RenderEngine : public QObject {
    Q_OBJECT

public:
    explicit RenderEngine(WMCompositor* compositor, QObject* parent = nullptr);
    ~RenderEngine() override;

    // ── Frame entry point ─────────────────────────────────────────────────
    void renderFrame(QPainter& painter, const QRect& viewport);

    // ── Wallpaper management ──────────────────────────────────────────────
    void loadWallpaper();
    void invalidateWallpaper()        { m_wallpaperDirty = true; }
    void invalidateAllBlurCaches();
    void invalidateBlurCache(Window* w);

    // ── Per-window state ──────────────────────────────────────────────────
    void onWindowRemoved(Window* w);

    // ── Cursor ────────────────────────────────────────────────────────────
    void setCursorPos  (const QPoint& p)    { m_cursorPos   = p; }
    void setCursorShape(Qt::CursorShape s)  { m_cursorShape = s; }

    // ── Glow pulse ────────────────────────────────────────────────────────
    void advanceGlowPulse();

private:
    // ── Top-level draw passes ─────────────────────────────────────────────
    void drawWallpaper            (QPainter& p, const QRect& vp);
    void drawVignette             (QPainter& p, const QRect& vp);
    void drawWindows              (QPainter& p, const QRect& vp);
    void drawCursor               (QPainter& p);

    // ── Per-window draw passes ────────────────────────────────────────────
    void drawWindow               (QPainter& p, Window* w, bool active);
    void drawWindowShadow         (QPainter& p, const QRect& rect, bool active);
    void drawWindowBlurBackground (QPainter& p, Window* w, const QRect& rect);
    void drawWindowGlassOverlay   (QPainter& p, const QRect& rect, bool active);
    void drawWindowBorder         (QPainter& p, const QRect& rect, bool active,
                                   float glowPhase);
    void drawTitleBar             (QPainter& p, Window* w, bool active);
    void drawTitleBarSeparator    (QPainter& p, const QRect& rect, bool active);

    // ── Title bar sub-components ──────────────────────────────────────────
    void drawTrafficLights        (QPainter& p, const QRect& windowRect,
                                   bool active);
    void drawTitleIcon            (QPainter& p, Window* w,
                                   const QRect& windowRect);
    void drawTitleText            (QPainter& p, Window* w,
                                   const QRect& windowRect,
                                   bool active);

    // ── Wallpaper helpers ─────────────────────────────────────────────────
    void    rescaleWallpaper      (const QSize& outputSize);
    QPixmap generateFallbackWallpaper(const QSize& size) const;

    // ── Blur helpers ──────────────────────────────────────────────────────
    QImage        blurWallpaperRegion(Window* w, const QRect& windowRect);
    static QImage boxBlur            (const QImage& src, int radius);

    // ── Render state cache ────────────────────────────────────────────────
    WindowRenderState& stateFor(Window* w);

    // ── Font / colour helpers ─────────────────────────────────────────────
    QFont  titleFont()                        const;
    QFont  uiFont(int size = -1)              const;
    static QColor lerpColor(const QColor& a, const QColor& b, float t);

    // ── Members ───────────────────────────────────────────────────────────
    WMCompositor* m_compositor        = nullptr;

    QPixmap       m_wallpaper;
    QPixmap       m_wallpaperScaled;
    bool          m_wallpaperDirty    = true;
    QString       m_loadedWallpaperPath;

    QHash<Window*, WindowRenderState> m_states;

    float         m_glowPulse         = 0.f;
    float         m_glowDir           = 1.f;

    QPoint        m_cursorPos;
    Qt::CursorShape m_cursorShape     = Qt::ArrowCursor;

    // ── Draw constants ────────────────────────────────────────────────────
    static constexpr int   kTitleBarHeight    = 28;
    static constexpr int   kResizeHandleWidth =  6;
    static constexpr int   kDotRadius         =  5;
    static constexpr int   kDotSpacing        = 16;
    static constexpr int   kDotRightMargin    = 12;
    static constexpr int   kTitleIconSize     = 16;
    static constexpr int   kShadowLayers      =  7;
    static constexpr float kShadowSpread      =  2.2f;
    static constexpr float kShadowAlphaMax    =  0.55f;
    static constexpr float kBlurRadius        = 14.f;
    static constexpr float kGlowPulseSpeed    =  0.012f;
    static constexpr float kGlowPulseMin      =  0.35f;
    static constexpr float kGlowPulseMax      =  1.0f;
    static constexpr int   kShimmerAlpha      = 22;
    static constexpr int   kVignetteAlpha     = 60;
    static constexpr int   kCursorSize        = 16;
};
