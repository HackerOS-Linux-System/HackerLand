#pragma once

#include <QWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QImage>
#include <QColor>
#include <QResizeEvent>
#include <QPaintEvent>

// ─────────────────────────────────────────────────────────────────────────────
// GlassWidget
//
// Base class for every HackerLand WM glassmorphism widget (BarWidget,
// WorkspaceSwitcher, NotificationOverlay, AppLauncher, …).
//
// Each paint cycle runs six ordered passes:
//   1. Multi-layer drop shadow         drawShadow()
//   2. Blurred wallpaper background    (from m_backgroundCache, optional)
//   3. Glass fill + depth gradient     drawGlassFill()
//   4. Top-edge shimmer highlight      drawShimmer()
//   5. Border: static or active glow   drawBorder() / drawActiveBorder()
//   6. Inner vignette                  drawInnerVignette() (optional)
//   7. Subclass content                paintContent()   ← override this
//
// Background blur
//   The compositor crops the wallpaper region behind this widget and calls
//   setBackgroundPixmap().  GlassWidget blurs and caches it.  When the
//   widget moves or resizes the cache is invalidated automatically.
// ─────────────────────────────────────────────────────────────────────────────
class GlassWidget : public QWidget {
    Q_OBJECT

public:
    explicit GlassWidget(QWidget* parent = nullptr);

    // ── Appearance setters ────────────────────────────────────────────────
    void setBlurRadius  (float r) { m_blurRadius   = r;  update(); }
    void setBorderRadius(int   r) { m_borderRadius = r;  update(); }
    void setGlassColor  (QColor c){ m_glassColor   = c;  update(); }
    void setBorderColor (QColor c){ m_borderColor  = c;  update(); }

    /// Activate the accent glow border (used when a child widget is "focused").
    void setAccentActive(bool a)  { m_active = a; update(); }

    /// Enable or disable the inner radial vignette darkening.
    void setVignetteEnabled(bool v){ m_showInnerVignette = v; update(); }

    // ── Background blur ───────────────────────────────────────────────────

    /// Supply a pre-cropped slice of the wallpaper behind this widget.
    /// GlassWidget blurs it and composites it as the background layer.
    void setBackgroundPixmap(const QPixmap& wallpaperRegion);

    /// Remove the background pixmap and fall back to a flat glass fill.
    void clearBackgroundPixmap();

    // ── Static image helpers (used by WMOutput and BarWidget too) ─────────

    /// 2-pass sliding-window box blur on an ARGB32_Premultiplied image.
    /// Caller can invoke this twice to approximate a Gaussian.
    static QImage boxBlur(const QImage& src, int radius);

    /// Boost the HSL saturation of every pixel by \p factor (1.0 = no change).
    static void   boostSaturation(QImage& img, float factor);

protected:
    // ── Qt overrides ──────────────────────────────────────────────────────
    void paintEvent  (QPaintEvent*   event) override;
    void resizeEvent (QResizeEvent*  event) override;

    /// Subclasses override this to draw widget-specific content.
    /// The painter is already clipped to the rounded glass shape.
    virtual void paintContent(QPainter& p) { Q_UNUSED(p) }

    // ── Draw passes (callable from subclass paintContent if needed) ────────
    void drawShadow         (QPainter& p, const QRectF& glassRect);
    void drawGlassFill      (QPainter& p, const QPainterPath& path,
                             const QRectF& rect);
    void drawShimmer        (QPainter& p, const QPainterPath& path,
                             const QRectF& rect);
    void drawBorder         (QPainter& p, const QPainterPath& path,
                             const QRectF& rect);
    void drawActiveBorder   (QPainter& p, const QPainterPath& path,
                             const QRectF& rect);
    void drawInnerVignette  (QPainter& p, const QPainterPath& path,
                             const QRectF& rect);

    // ── Config sync ───────────────────────────────────────────────────────
    void syncFromConfig();

    // ── Members accessible to subclasses ──────────────────────────────────
    float  m_blurRadius         = 20.0f;
    int    m_borderRadius       = 12;
    QColor m_glassColor         = QColor( 10,  15,  30, 160);
    QColor m_borderColor        = QColor(255, 255, 255,  40);
    bool   m_active             = false;
    bool   m_showInnerVignette  = true;
    bool   m_showBlurBackground = false;
    QPixmap m_backgroundCache;

private:
    // ── Layout / render constants ─────────────────────────────────────────
    static constexpr int   kShadowMargin   = 10;  ///< px reserved for shadow
    static constexpr int   kShadowLayers   =  8;  ///< number of shadow rings
    static constexpr float kShadowSpread   = 1.4f;///< px growth per layer
    static constexpr float kShadowAlphaMax = 0.55f;
    static constexpr int   kShimmerAlpha   = 32;  ///< top-edge highlight
    static constexpr int   kVignetteAlpha  = 18;  ///< inner edge darkening
};
