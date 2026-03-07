#include "GlassWidget.h"
#include "core/Config.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QConicalGradient>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QImage>
#include <QColor>
#include <QtMath>
#include <QDebug>

#include <algorithm>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

GlassWidget::GlassWidget(QWidget* parent)
: QWidget(parent, Qt::FramelessWindowHint)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);

    // Pull initial values from the global config so derived widgets look
    // correct without having to call any setter explicitly.
    syncFromConfig();

    // Keep appearance in sync whenever the user hot-reloads the config.
    connect(&Config::instance(), &Config::themeChanged,
            this, [this]() {
                syncFromConfig();
                update();
            });
}

// ─────────────────────────────────────────────────────────────────────────────
// Config sync
// ─────────────────────────────────────────────────────────────────────────────

void GlassWidget::syncFromConfig() {
    const auto& theme = Config::instance().theme;
    m_glassColor   = theme.glassBackground;
    m_borderColor  = theme.glassBorder;
    m_borderRadius = theme.borderRadius;
    m_blurRadius   = theme.blurRadius;
}

// ─────────────────────────────────────────────────────────────────────────────
// paintEvent — orchestrates all draw passes in order
// ─────────────────────────────────────────────────────────────────────────────

void GlassWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing
    | QPainter::TextAntialiasing
    | QPainter::SmoothPixmapTransform);

    // The shadow bleeds outside the logical glass surface, so we shrink the
    // glass rect inward to leave room for it on all sides.
    const QRectF glassRect = QRectF(rect()).adjusted(
        kShadowMargin,  kShadowMargin,
        -kShadowMargin, -kShadowMargin);

    // Build the clipping/fill path once; shared by most draw passes.
    QPainterPath glassPath;
    glassPath.addRoundedRect(glassRect, m_borderRadius, m_borderRadius);

    // ── Pass 1: multi-layer drop shadow ───────────────────────────────────
    drawShadow(p, glassRect);

    // ── Pass 2: blurred wallpaper background (optional) ───────────────────
    if (m_showBlurBackground && !m_backgroundCache.isNull()) {
        p.save();
        p.setClipPath(glassPath);
        p.drawPixmap(glassRect.toRect(), m_backgroundCache);
        p.restore();
    }

    // ── Pass 3: glass fill + subtle depth gradient ────────────────────────
    drawGlassFill(p, glassPath, glassRect);

    // ── Pass 4: top-edge specular shimmer ─────────────────────────────────
    drawShimmer(p, glassPath, glassRect);

    // ── Pass 5: border (active gradient glow or inactive hairline) ────────
    drawBorder(p, glassPath, glassRect);

    // ── Pass 6: inner edge vignette (optional depth darkening) ────────────
    if (m_showInnerVignette) {
        drawInnerVignette(p, glassPath, glassRect);
    }

    // ── Pass 7: subclass content, clipped to the glass shape ──────────────
    p.save();
    p.setClipPath(glassPath);
    paintContent(p);
    p.restore();
}

// ─────────────────────────────────────────────────────────────────────────────
// Pass 1 — multi-layer drop shadow
//
// We render kShadowLayers concentric rounded rectangles at decreasing opacity,
// each one slightly larger and shifted downward.  Together they produce a soft
// penumbra that looks like a physically lifted panel.
// ─────────────────────────────────────────────────────────────────────────────

void GlassWidget::drawShadow(QPainter& p, const QRectF& glassRect) {
    const QColor base = Config::instance().theme.shadowColor;

    p.setPen(Qt::NoPen);

    for (int i = kShadowLayers; i >= 1; --i) {
        // t = 1 at the outermost layer, 0 at the innermost.
        const float t      = float(kShadowLayers - i) / float(kShadowLayers - 1);
        const float spread = float(i) * kShadowSpread;

        // Alpha: strong close to the widget, fading out at the fringe.
        // Using a quadratic falloff rather than linear gives a more
        // natural-looking soft shadow.
        const float alpha  = kShadowAlphaMax * (1.f - t) * (1.f - t);

        // The shadow shifts slightly downward so light appears to come from
        // above — a common convention in modern UI design.
        const float yBias  = spread * 0.35f;

        QColor shadowColor = base;
        shadowColor.setAlphaF(alpha);
        p.setBrush(shadowColor);

        const QRectF shadowRect = glassRect.adjusted(
            -spread,         -(spread * 0.5f),
                                                     spread,           spread + yBias);

        const float r = m_borderRadius + spread * 0.45f;
        p.drawRoundedRect(shadowRect, r, r);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Pass 3 — glass fill
//
// Two layers:
//   a) Flat semi-transparent base colour (m_glassColor).
//   b) Vertical gradient that brightens the top edge and darkens the bottom,
//      giving the appearance of an angled frosted panel.
// ─────────────────────────────────────────────────────────────────────────────

void GlassWidget::drawGlassFill(QPainter&           p,
                                const QPainterPath& path,
                                const QRectF&       rect)
{
    // a) Base flat fill.
    p.fillPath(path, m_glassColor);

    // b) Depth gradient: a very faint brightness ramp top→bottom.
    QLinearGradient depth(rect.topLeft(), rect.bottomLeft());
    depth.setColorAt(0.00, QColor(255, 255, 255, 10));  // slight brightening
    depth.setColorAt(0.45, QColor(255, 255, 255,  0));  // neutral midpoint
    depth.setColorAt(1.00, QColor(  0,   0,   0, 14));  // slight darkening
    p.fillPath(path, depth);
}

// ─────────────────────────────────────────────────────────────────────────────
// Pass 4 — top-edge specular shimmer
//
// A narrow horizontal band at the very top of the widget catches simulated
// light and fades out quickly downward — the same specular effect seen on
// frosted glass or anodised aluminium panels.
// ─────────────────────────────────────────────────────────────────────────────

void GlassWidget::drawShimmer(QPainter&           p,
                              const QPainterPath& fullPath,
                              const QRectF&       rect)
{
    // The shimmer extends down by two border radii so it follows the corner
    // curvature naturally.
    const float shimmerH = float(m_borderRadius) * 2.f + 6.f;

    QLinearGradient shimmer(
        rect.topLeft(),
                            QPointF(rect.left(), rect.top() + shimmerH));
    shimmer.setColorAt(0.0, QColor(255, 255, 255, kShimmerAlpha));
    shimmer.setColorAt(1.0, Qt::transparent);

    // Clip to the intersection of the shimmer band rect and the glass shape
    // so that the gradient does not paint outside the rounded corners.
    QPainterPath shimmerBand;
    shimmerBand.addRect(
        QRectF(rect.left(), rect.top(), rect.width(), shimmerH));

    p.fillPath(shimmerBand.intersected(fullPath), shimmer);
}

// ─────────────────────────────────────────────────────────────────────────────
// Pass 5 — border
// ─────────────────────────────────────────────────────────────────────────────

void GlassWidget::drawBorder(QPainter&           p,
                             const QPainterPath& path,
                             const QRectF&       rect)
{
    if (m_active) {
        drawActiveBorder(p, path, rect);
    } else {
        // Inactive: single muted hairline that reinforces the card edge.
        p.setPen(QPen(m_borderColor, 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Active border — four stacked layers that produce a holographic glow effect:
//
//   Layer 1 — wide outer penumbra glow  (very transparent, 5 px wide)
//   Layer 2 — medium inner glow         (semi-transparent, 2.5 px)
//   Layer 3 — conical gradient hairline (accent→secondary→tertiary→accent)
//   Layer 4 — upper-left quadrant boost (simulates directional light)
// ─────────────────────────────────────────────────────────────────────────────

void GlassWidget::drawActiveBorder(QPainter&           p,
                                   const QPainterPath& path,
                                   const QRectF&       rect)
{
    const auto& theme = Config::instance().theme;
    p.setBrush(Qt::NoBrush);

    // ── Layer 1: outer penumbra ───────────────────────────────────────────
    {
        QColor glow = theme.accentColor;
        glow.setAlpha(28);
        p.setPen(QPen(glow, 5.0));
        p.drawPath(path);
    }

    // ── Layer 2: mid glow ─────────────────────────────────────────────────
    {
        QColor glow = theme.accentColor;
        glow.setAlpha(60);
        p.setPen(QPen(glow, 2.5));
        p.drawPath(path);
    }

    // ── Layer 3: conical colour-shift hairline ────────────────────────────
    // The conical gradient rotates through the three accent colours as it
    // travels around the perimeter, producing a subtle iridescent effect —
    // the "holographic foil" signature of the HackerLand WM style.
    {
        QConicalGradient cg(rect.center(), 90.0);
        cg.setColorAt(0.00, theme.accentColor);
        cg.setColorAt(0.33, theme.accentSecondary);
        cg.setColorAt(0.66, theme.accentTertiary);
        cg.setColorAt(1.00, theme.accentColor);
        p.setPen(QPen(QBrush(cg), 1.5));
        p.drawPath(path);
    }

    // ── Layer 4: upper-left directional boost ─────────────────────────────
    // A linear gradient from the top-left corner to the centre fades from a
    // bright accent tint to transparent, simulating a light source in the
    // upper-left — the conventional "material light" direction.
    {
        QLinearGradient hl(rect.topLeft(), rect.center());
        QColor hiColor = theme.accentColor;
        hiColor.setAlpha(80);
        hl.setColorAt(0.0, hiColor);
        hl.setColorAt(1.0, Qt::transparent);
        p.setPen(QPen(QBrush(hl), 1.0));
        p.drawPath(path);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Pass 6 — inner vignette
//
// A radial gradient, transparent at the centre and dark at the edges, that
// deepens the frosted-glass illusion and makes widgets feel slightly recessed
// within their shadow.
// ─────────────────────────────────────────────────────────────────────────────

void GlassWidget::drawInnerVignette(QPainter&           p,
                                    const QPainterPath& path,
                                    const QRectF&       rect)
{
    const float radius = float(std::max(rect.width(), rect.height())) * 0.72f;

    QRadialGradient vignette(rect.center(), radius);
    vignette.setColorAt(0.0, Qt::transparent);
    vignette.setColorAt(1.0, QColor(0, 0, 0, kVignetteAlpha));
    p.fillPath(path, vignette);
}

// ─────────────────────────────────────────────────────────────────────────────
// Background blur cache
// ─────────────────────────────────────────────────────────────────────────────

void GlassWidget::setBackgroundPixmap(const QPixmap& wallpaperRegion) {
    if (wallpaperRegion.isNull()) {
        m_backgroundCache    = {};
        m_showBlurBackground = false;
        return;
    }

    // Convert to a premultiplied format for correct alpha compositing during
    // the box blur passes.
    QImage src = wallpaperRegion.toImage()
    .convertToFormat(QImage::Format_ARGB32_Premultiplied);

    // Two-pass box blur approximates a Gaussian at the configured radius.
    const int blurR    = qMax(1, static_cast<int>(m_blurRadius));
    QImage blurred     = boxBlur(src,    blurR);
    blurred            = boxBlur(blurred, blurR / 2 + 1);

    // Optional saturation boost for the vivid "frosted glass" look.
    const float sat = Config::instance().theme.blurSaturation;
    if (!qFuzzyCompare(sat, 1.f)) {
        boostSaturation(blurred, sat);
    }

    m_backgroundCache    = QPixmap::fromImage(blurred);
    m_showBlurBackground = true;
    update();
}

void GlassWidget::clearBackgroundPixmap() {
    m_backgroundCache    = {};
    m_showBlurBackground = false;
    update();
}

// ─────────────────────────────────────────────────────────────────────────────
// resizeEvent
// ─────────────────────────────────────────────────────────────────────────────

void GlassWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);

    // The background blur cache was sized to the previous wallpaper crop;
    // after a resize it no longer matches.  The compositor will supply a
    // fresh crop via setBackgroundPixmap() when it redraws.
    m_backgroundCache    = {};
    m_showBlurBackground = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Static helper — boxBlur
//
// Sliding-window 2D box blur implemented as two 1D passes (horizontal then
// vertical).  Each pass runs in O(w×h) time regardless of radius because it
// maintains a running sum and slides it forward one sample at a time.
//
// Caller can invoke this twice at radius r and r/2 to approximate a Gaussian
// at negligible extra cost.
// ─────────────────────────────────────────────────────────────────────────────

QImage GlassWidget::boxBlur(const QImage& src, int radius) {
    if (radius <= 0) return src;

    const int w = src.width();
    const int h = src.height();
    if (w == 0 || h == 0) return src;

    QImage tmp(w, h, QImage::Format_ARGB32_Premultiplied);
    QImage dst(w, h, QImage::Format_ARGB32_Premultiplied);

    const int halfR  = radius;
    const int kCount = 2 * halfR + 1;

    // ── Horizontal pass: src → tmp ────────────────────────────────────────
    for (int y = 0; y < h; ++y) {
        const QRgb* srcRow = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb*       dstRow = reinterpret_cast<QRgb*>(tmp.scanLine(y));

        // Seed the running sum with the first kCount pixels (clamped).
        qint32 r = 0, g = 0, b = 0, a = 0;
        for (int k = -halfR; k <= halfR; ++k) {
            const QRgb px = srcRow[qBound(0, k, w - 1)];
            r += qRed(px);   g += qGreen(px);
            b += qBlue(px);  a += qAlpha(px);
        }

        for (int x = 0; x < w; ++x) {
            dstRow[x] = qRgba(r / kCount, g / kCount,
                              b / kCount, a / kCount);

            // Slide: drop the leftmost sample, add the next right sample.
            const QRgb rem = srcRow[qBound(0, x - halfR,     w - 1)];
            const QRgb add = srcRow[qBound(0, x + halfR + 1, w - 1)];
            r += qRed(add)   - qRed(rem);
            g += qGreen(add) - qGreen(rem);
            b += qBlue(add)  - qBlue(rem);
            a += qAlpha(add) - qAlpha(rem);
        }
    }

    // ── Vertical pass: tmp → dst ──────────────────────────────────────────
    for (int x = 0; x < w; ++x) {
        // Seed from column x in tmp.
        qint32 r = 0, g = 0, b = 0, a = 0;
        for (int k = -halfR; k <= halfR; ++k) {
            const QRgb px = reinterpret_cast<const QRgb*>(
                tmp.constScanLine(qBound(0, k, h - 1)))[x];
                r += qRed(px);   g += qGreen(px);
                b += qBlue(px);  a += qAlpha(px);
        }

        for (int y = 0; y < h; ++y) {
            reinterpret_cast<QRgb*>(dst.scanLine(y))[x] =
            qRgba(r / kCount, g / kCount,
                  b / kCount, a / kCount);

            const QRgb rem = reinterpret_cast<const QRgb*>(
                tmp.constScanLine(qBound(0, y - halfR,     h - 1)))[x];
                const QRgb add = reinterpret_cast<const QRgb*>(
                    tmp.constScanLine(qBound(0, y + halfR + 1, h - 1)))[x];
                    r += qRed(add)   - qRed(rem);
                    g += qGreen(add) - qGreen(rem);
                    b += qBlue(add)  - qBlue(rem);
                    a += qAlpha(add) - qAlpha(rem);
        }
    }

    return dst;
}

// ─────────────────────────────────────────────────────────────────────────────
// Static helper — boostSaturation
//
// Converts every pixel to HSL, multiplies the S component by \p factor, and
// converts back.  Operates in-place.  Factor > 1.0 saturates (more vivid),
// factor < 1.0 desaturates (more washed-out).
// ─────────────────────────────────────────────────────────────────────────────

void GlassWidget::boostSaturation(QImage& img, float factor) {
    if (qFuzzyCompare(factor, 1.f)) return;

    const int w = img.width();
    const int h = img.height();

    for (int y = 0; y < h; ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            QColor c = QColor::fromRgba(line[x]);
            int hue, sat, lightness, alpha;
            c.getHsl(&hue, &sat, &lightness, &alpha);
            sat    = qBound(0, int(float(sat) * factor), 255);
            c.setHsl(hue, sat, lightness, alpha);
            line[x] = c.rgba();
        }
    }
}
