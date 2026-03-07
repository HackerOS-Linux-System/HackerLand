#include "RenderEngine.h"
#include "compositor/WMCompositor.h"
#include "core/Workspace.h"
#include "core/Window.h"
#include "core/Config.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QConicalGradient>
#include <QFile>
#include <QScreen>
#include <QGuiApplication>
#include <QFontDatabase>
#include <QIcon>
#include <QtMath>
#include <QDebug>

#include <cmath>
#include <algorithm>
#include <random>

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

RenderEngine::RenderEngine(WMCompositor* compositor, QObject* parent)
: QObject(parent)
, m_compositor(compositor)
{
    Q_ASSERT(compositor);

    connect(&Config::instance(), &Config::themeChanged,
            this, [this]() {
                m_wallpaperDirty = true;
                invalidateAllBlurCaches();
            });
}

RenderEngine::~RenderEngine() = default;

// ─────────────────────────────────────────────────────────────────────────────
// Frame entry point
// ─────────────────────────────────────────────────────────────────────────────

void RenderEngine::renderFrame(QPainter& p, const QRect& viewport) {
    p.setRenderHints(QPainter::Antialiasing
    | QPainter::TextAntialiasing
    | QPainter::SmoothPixmapTransform);

    if (m_wallpaperDirty || m_wallpaperScaled.size() != viewport.size()) {
        rescaleWallpaper(viewport.size());
    }

    drawWallpaper(p, viewport);
    drawVignette (p, viewport);
    drawWindows  (p, viewport);
    drawCursor   (p);
}

// ─────────────────────────────────────────────────────────────────────────────
// Glow pulse
// ─────────────────────────────────────────────────────────────────────────────

void RenderEngine::advanceGlowPulse() {
    m_glowPulse += kGlowPulseSpeed * m_glowDir;
    if (m_glowPulse >= kGlowPulseMax) { m_glowPulse = kGlowPulseMax; m_glowDir = -1.f; }
    if (m_glowPulse <= kGlowPulseMin) { m_glowPulse = kGlowPulseMin; m_glowDir =  1.f; }

    // Advance per-window phases at a slightly different rate for stagger.
    for (auto it = m_states.begin(); it != m_states.end(); ++it) {
        it->glowPhase += kGlowPulseSpeed * 0.7f;
        if (it->glowPhase > float(2.0 * M_PI)) it->glowPhase -= float(2.0 * M_PI);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Pass 1 — wallpaper
// ─────────────────────────────────────────────────────────────────────────────

void RenderEngine::drawWallpaper(QPainter& p, const QRect& vp) {
    if (m_wallpaperScaled.isNull()) {
        p.fillRect(vp, QColor(8, 10, 20));
        return;
    }
    p.drawPixmap(vp, m_wallpaperScaled);
}

// ─────────────────────────────────────────────────────────────────────────────
// Pass 2 — full-screen vignette
// ─────────────────────────────────────────────────────────────────────────────

void RenderEngine::drawVignette(QPainter& p, const QRect& vp) {
    // Radial gradient: transparent at centre, dark at edges.
    QRadialGradient vg(vp.center(),
                       std::max(vp.width(), vp.height()) * 0.7f);
    vg.setColorAt(0.0, Qt::transparent);
    vg.setColorAt(1.0, QColor(0, 0, 0, kVignetteAlpha));
    p.fillRect(vp, vg);
}

// ─────────────────────────────────────────────────────────────────────────────
// Pass 3 — windows
// ─────────────────────────────────────────────────────────────────────────────

void RenderEngine::drawWindows(QPainter& p, const QRect& /*vp*/) {
    Workspace* ws = m_compositor->activeWorkspace();
    if (!ws) return;

    const QList<Window*> wins = ws->visibleWindows();
    if (wins.isEmpty()) return;

    const Window* active = ws->activeWindow();

    // Draw in Z-order: tiled bottom → top, then floating windows on top.
    // Within each group the list is already in insertion / tile order.
    for (Window* w : wins) {
        if (w->isFloating()) continue;   // second pass below
        if (!w->isVisible()) continue;
        drawWindow(p, w, w == active);
    }
    for (Window* w : wins) {
        if (!w->isFloating()) continue;
        if (!w->isVisible()) continue;
        drawWindow(p, w, w == active);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-window draw — orchestrates the sub-passes
// ─────────────────────────────────────────────────────────────────────────────

void RenderEngine::drawWindow(QPainter& p, Window* w, bool active) {
    Q_ASSERT(w);

    const QRect rect = w->geometry();
    if (rect.isEmpty()) return;

    WindowRenderState& state = stateFor(w);

    // Invalidate blur if the window moved or resized.
    if (state.lastGeometry != rect) {
        state.blurDirty   = true;
        state.lastGeometry = rect;
    }

    // Apply window opacity (for animations and inactive dimming).
    const float opacity = qBound(0.f, w->opacity(), 1.f);
    p.setOpacity(opacity);

    drawWindowShadow        (p, rect, active);
    drawWindowBlurBackground(p, w, rect);
    drawWindowGlassOverlay  (p, rect, active);
    drawWindowBorder        (p, rect, active, state.glowPhase);
    drawTitleBar            (p, w, active);
    drawTitleBarSeparator   (p, rect, active);

    p.setOpacity(1.f);
}

// ─────────────────────────────────────────────────────────────────────────────
// Window shadow — kShadowLayers concentric rounded rects, quadratic falloff
// ─────────────────────────────────────────────────────────────────────────────

void RenderEngine::drawWindowShadow(QPainter& p,
                                    const QRect& rect,
                                    bool active)
{
    const QColor base  = Config::instance().theme.shadowColor;
    const int    br    = Config::instance().theme.borderRadius;

    p.setPen(Qt::NoPen);

    for (int i = kShadowLayers; i >= 1; --i) {
        const float t      = float(kShadowLayers - i) / float(kShadowLayers - 1);
        const float spread = float(i) * kShadowSpread;
        const float alpha  = kShadowAlphaMax
        * (active ? 1.f : 0.55f)
        * (1.f - t) * (1.f - t);
        const float yBias  = spread * 0.4f; // shadow falls downward

        QColor sc = base;
        sc.setAlphaF(alpha);
        p.setBrush(sc);

        const QRectF sr = QRectF(rect).adjusted(
            -spread,         -(spread * 0.3f),
                                                spread,           spread + yBias);
        const float r  = br + spread * 0.5f;
        p.drawRoundedRect(sr, r, r);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Blurred wallpaper background inside the window shape
// ─────────────────────────────────────────────────────────────────────────────

void RenderEngine::drawWindowBlurBackground(QPainter& p,
                                            Window*   w,
                                            const QRect& rect)
{
    const int br = Config::instance().theme.borderRadius;

    // Build a clip path for the full window shape.
    QPainterPath clip;
    clip.addRoundedRect(rect, br, br);
    p.save();
    p.setClipPath(clip);

    const QImage blurred = blurWallpaperRegion(w, rect);
    if (!blurred.isNull()) {
        p.drawImage(rect, blurred);
    }

    p.restore();
}

// ─────────────────────────────────────────────────────────────────────────────
// Frosted glass overlay — tinted fill + shimmer highlight
// ─────────────────────────────────────────────────────────────────────────────

void RenderEngine::drawWindowGlassOverlay(QPainter& p,
                                          const QRect& rect,
                                          bool active)
{
    const auto& theme = Config::instance().theme;
    const int   br    = theme.borderRadius;

    QPainterPath clip;
    clip.addRoundedRect(rect, br, br);
    p.save();
    p.setClipPath(clip);

    // Base glass fill.
    QColor glass = theme.glassBackground;
    if (!active) {
        // Inactive windows are slightly more opaque / darker.
        glass.setAlpha(qMin(255, glass.alpha() + 20));
    }
    p.fillPath(clip, glass);

    // Vertical depth gradient: brighter at top, darker at bottom.
    QLinearGradient depth(rect.topLeft(), rect.bottomLeft());
    depth.setColorAt(0.00, QColor(255, 255, 255, active ? 10 : 6));
    depth.setColorAt(0.45, QColor(255, 255, 255, 0));
    depth.setColorAt(1.00, QColor(  0,   0,   0, active ? 16 : 24));
    p.fillPath(clip, depth);

    // Top-edge specular shimmer (restricted to upper rounded band).
    const float shimH = float(br) * 2.f + 6.f;
    QPainterPath shimBand;
    shimBand.addRect(QRectF(rect.left(), rect.top(), rect.width(), shimH));

    QLinearGradient shim(rect.topLeft(),
                         QPointF(rect.left(), rect.top() + shimH));
    shim.setColorAt(0.0, QColor(255, 255, 255, kShimmerAlpha));
    shim.setColorAt(1.0, Qt::transparent);
    p.fillPath(shimBand.intersected(clip), shim);

    p.restore();
}

// ─────────────────────────────────────────────────────────────────────────────
// Border — four-layer holographic glow (active) or hairline (inactive)
// ─────────────────────────────────────────────────────────────────────────────

void RenderEngine::drawWindowBorder(QPainter& p,
                                    const QRect& rect,
                                    bool  active,
                                    float glowPhase)
{
    const auto& theme = Config::instance().theme;
    const int   br    = theme.borderRadius;

    QPainterPath path;
    path.addRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), br, br);
    p.setBrush(Qt::NoBrush);

    if (!active) {
        // Inactive: single muted hairline.
        p.setPen(QPen(theme.glassBorder, 1.0));
        p.drawPath(path);
        return;
    }

    // Pulsing glow factor from the per-window phase.
    const float pulse = 0.65f + 0.35f * std::sin(glowPhase) * m_glowPulse;

    // ── Layer 1: wide outer penumbra ──────────────────────────────────────
    {
        QColor glow = theme.accentColor;
        glow.setAlpha(int(30.f * pulse));
        p.setPen(QPen(glow, 5.0));
        p.drawPath(path);
    }

    // ── Layer 2: mid glow ─────────────────────────────────────────────────
    {
        QColor glow = theme.accentColor;
        glow.setAlpha(int(65.f * pulse));
        p.setPen(QPen(glow, 2.5));
        p.drawPath(path);
    }

    // ── Layer 3: conical colour-shift hairline ────────────────────────────
    {
        QConicalGradient cg(QRectF(rect).center(), 90.0 + glowPhase * 10.f);
        cg.setColorAt(0.00, theme.accentColor);
        cg.setColorAt(0.33, theme.accentSecondary);
        cg.setColorAt(0.66, theme.accentTertiary);
        cg.setColorAt(1.00, theme.accentColor);
        p.setPen(QPen(QBrush(cg), 1.5));
        p.drawPath(path);
    }

    // ── Layer 4: upper-left directional highlight ─────────────────────────
    {
        QLinearGradient hl(rect.topLeft(), rect.center());
        QColor hiColor = theme.accentColor;
        hiColor.setAlpha(int(75.f * pulse));
        hl.setColorAt(0.0, hiColor);
        hl.setColorAt(1.0, Qt::transparent);
        p.setPen(QPen(QBrush(hl), 1.0));
        p.drawPath(path);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Title bar
// ─────────────────────────────────────────────────────────────────────────────

void RenderEngine::drawTitleBar(QPainter& p, Window* w, bool active) {
    Q_ASSERT(w);

    const auto& theme = Config::instance().theme;
    const QRect& rect = w->geometry();
    const int    br   = theme.borderRadius;

    // Title bar occupies the top kTitleBarHeight pixels of the window.
    const QRect tbRect(rect.left(), rect.top(),
                       rect.width(), kTitleBarHeight);

    // ── Background: semi-transparent stripe ──────────────────────────────
    {
        QPainterPath tbPath;
        // Only round the top two corners.
        tbPath.moveTo(rect.left() + br, rect.top());
        tbPath.lineTo(rect.right() - br, rect.top());
        tbPath.arcTo(QRectF(rect.right() - br * 2, rect.top(), br * 2, br * 2),
                     0, 90);
        tbPath.lineTo(rect.right(), rect.top() + kTitleBarHeight);
        tbPath.lineTo(rect.left(),  rect.top() + kTitleBarHeight);
        tbPath.arcTo(QRectF(rect.left(), rect.top(), br * 2, br * 2),
                     180, 90);
        tbPath.closeSubpath();

        QLinearGradient tbGrad(tbRect.topLeft(), tbRect.bottomLeft());
        if (active) {
            tbGrad.setColorAt(0.0, QColor(255, 255, 255, 18));
            tbGrad.setColorAt(1.0, QColor(255, 255, 255,  4));
        } else {
            tbGrad.setColorAt(0.0, QColor(255, 255, 255,  8));
            tbGrad.setColorAt(1.0, QColor(  0,   0,   0,  8));
        }
        p.fillPath(tbPath, tbGrad);
    }

    drawTrafficLights(p, rect, active);
    drawTitleIcon    (p, w,    rect);
    drawTitleText    (p, w,    rect, active);
}

// ── Traffic-light dots ────────────────────────────────────────────────────────

void RenderEngine::drawTrafficLights(QPainter& p,
                                     const QRect& windowRect,
                                     bool active)
{
    // Three dots aligned to the right edge of the title bar.
    // Order right-to-left: Close (rightmost), Maximize, Minimize.
    struct Dot {
        QColor base;
        QColor active;
    };

    static const Dot dots[3] = {
        { QColor(100,  50,  50), QColor(255,  95,  87) }, // Close   — red
        { QColor( 50, 100,  50), QColor( 40, 200,  64) }, // Maximize — green
        { QColor(100, 100,  50), QColor(255, 189,  46) }, // Minimize — yellow
    };

    const int cy = windowRect.top() + kTitleBarHeight / 2;

    for (int i = 0; i < 3; ++i) {
        const int cx = windowRect.right()
        - kDotRightMargin
        - i * kDotSpacing;

        const QColor& col = active ? dots[i].active : dots[i].base;

        // Glow halo for active dots.
        if (active) {
            QColor glow = col;
            glow.setAlpha(40);
            p.setPen(Qt::NoPen);
            p.setBrush(glow);
            p.drawEllipse(QPoint(cx, cy),
                          kDotRadius + 3, kDotRadius + 3);
        }

        // Dot body.
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        p.drawEllipse(QPoint(cx, cy), kDotRadius, kDotRadius);

        // Inner highlight (tiny bright spot at top-left).
        if (active) {
            p.setBrush(QColor(255, 255, 255, 60));
            p.drawEllipse(QPoint(cx - 1, cy - 1),
                          kDotRadius / 2, kDotRadius / 2);
        }
    }
}

// ── Window icon ───────────────────────────────────────────────────────────────

void RenderEngine::drawTitleIcon(QPainter& p, Window* w,
                                 const QRect& windowRect)
{
    const QIcon& icon = w->icon();
    if (icon.isNull()) return;

    const int cy  = windowRect.top() + kTitleBarHeight / 2;
    const int x   = windowRect.left() + 12;
    const QRect iconRect(x, cy - kTitleIconSize / 2,
                         kTitleIconSize, kTitleIconSize);

    const QPixmap pm = icon.pixmap(kTitleIconSize, kTitleIconSize);
    if (!pm.isNull()) {
        p.drawPixmap(iconRect, pm);
    }
}

// ── Title text ────────────────────────────────────────────────────────────────

void RenderEngine::drawTitleText(QPainter& p, Window* w,
                                 const QRect& windowRect,
                                 bool active)
{
    const auto& theme = Config::instance().theme;

    // Title area sits between the icon and the leftmost traffic-light dot.
    const int iconEnd  = windowRect.left() + 12 + kTitleIconSize + 6;
    const int dotsLeft = windowRect.right()
    - kDotRightMargin
    - 2 * kDotSpacing
    - kDotRadius * 2
    - 6;
    const QRect textRect(iconEnd,
                         windowRect.top(),
                         dotsLeft - iconEnd,
                         kTitleBarHeight);

    const QString title = w->title().isEmpty()
    ? w->appId()
    : w->title();

    p.setFont(titleFont());

    QColor col = active ? theme.textPrimary : theme.textSecondary;
    if (!active) col.setAlpha(160);
    p.setPen(col);

    p.drawText(textRect,
               Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
               title);
}

// ── Title bar separator ───────────────────────────────────────────────────────

void RenderEngine::drawTitleBarSeparator(QPainter& p,
                                         const QRect& rect,
                                         bool active)
{
    const int y = rect.top() + kTitleBarHeight;

    QLinearGradient sep(rect.left(),  y,
                        rect.right(), y);
    const int alpha = active ? 50 : 25;
    sep.setColorAt(0.0, Qt::transparent);
    sep.setColorAt(0.1, QColor(255, 255, 255, alpha));
    sep.setColorAt(0.9, QColor(255, 255, 255, alpha));
    sep.setColorAt(1.0, Qt::transparent);

    p.setPen(QPen(QBrush(sep), 1.0));
    p.drawLine(rect.left(), y, rect.right(), y);
}

// ─────────────────────────────────────────────────────────────────────────────
// Pass 4 — software cursor
// ─────────────────────────────────────────────────────────────────────────────

void RenderEngine::drawCursor(QPainter& p) {
    const QPoint pos  = m_cursorPos;
    const int    half = kCursorSize / 2;

    p.setOpacity(1.f);
    p.setRenderHint(QPainter::Antialiasing);

    switch (m_cursorShape) {
        // ── Arrow cursor ──────────────────────────────────────────────────
        case Qt::ArrowCursor:
        default: {
            // Classic arrow: filled triangle pointing upper-left.
            QPolygonF arrow;
            arrow << QPointF(pos.x(),        pos.y())
            << QPointF(pos.x(),        pos.y() + kCursorSize)
            << QPointF(pos.x() + kCursorSize * 0.35f,
                       pos.y() + kCursorSize * 0.65f)
            << QPointF(pos.x() + kCursorSize * 0.55f,
                       pos.y() + kCursorSize);
            // Drop shadow.
            QPolygonF shadow = arrow;
            for (auto& pt : shadow) pt += QPointF(1.5f, 1.5f);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 80));
            p.drawPolygon(shadow);
            // Body.
            p.setBrush(Qt::white);
            p.setPen(QPen(QColor(30, 30, 30), 1.0));
            p.drawPolygon(arrow);
            break;
        }

        // ── Move / drag ───────────────────────────────────────────────────
        case Qt::SizeAllCursor: {
            p.setPen(QPen(Qt::white, 2.0));
            p.setBrush(Qt::NoBrush);
            // Four outward arrowheads at N/S/E/W.
            const QPoint dirs[4] = {
                {0, -half}, {0, half}, {-half, 0}, {half, 0}
            };
            for (const auto& d : dirs) {
                const QPoint tip  = pos + d;
                p.drawLine(pos, tip);
                // Small arrowhead triangle at the tip.
                QPolygonF head;
                if (d.y() != 0) {
                    const int s = (d.y() < 0) ? -1 : 1;
                    head << QPointF(tip.x() - 4, tip.y() - s * 4)
                    << QPointF(tip.x() + 4, tip.y() - s * 4)
                    << QPointF(tip.x(),      tip.y());
                } else {
                    const int s = (d.x() < 0) ? -1 : 1;
                    head << QPointF(tip.x() - s * 4, tip.y() - 4)
                    << QPointF(tip.x() - s * 4, tip.y() + 4)
                    << QPointF(tip.x(),          tip.y());
                }
                p.setBrush(Qt::white);
                p.setPen(Qt::NoPen);
                p.drawPolygon(head);
            }
            break;
        }

        // ── Resize cursors ────────────────────────────────────────────────
        case Qt::SizeHorCursor: {
            p.setPen(QPen(Qt::white, 2.0));
            p.drawLine(pos.x() - half, pos.y(),
                       pos.x() + half, pos.y());
            break;
        }
        case Qt::SizeVerCursor: {
            p.setPen(QPen(Qt::white, 2.0));
            p.drawLine(pos.x(), pos.y() - half,
                       pos.x(), pos.y() + half);
            break;
        }
        case Qt::SizeFDiagCursor:
        case Qt::SizeBDiagCursor: {
            const int s = (m_cursorShape == Qt::SizeFDiagCursor) ? 1 : -1;
            p.setPen(QPen(Qt::white, 2.0));
            p.drawLine(pos.x() - half, pos.y() - half * s,
                       pos.x() + half, pos.y() + half * s);
            break;
        }

        // ── Pointing hand ─────────────────────────────────────────────────
        case Qt::PointingHandCursor: {
            // Simple circle with a dot.
            p.setPen(QPen(Qt::white, 1.5));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(pos, half, half);
            p.setBrush(Qt::white);
            p.setPen(Qt::NoPen);
            p.drawEllipse(pos, 2, 2);
            break;
        }

        // ── Open hand (drag title bar) ────────────────────────────────────
        case Qt::OpenHandCursor: {
            // Four vertical lines = fingers.
            p.setPen(QPen(Qt::white, 2.0));
            for (int i = -2; i <= 2; i += 1) {
                p.drawLine(pos.x() + i * 3, pos.y() - half / 2,
                           pos.x() + i * 3, pos.y() + half / 2);
            }
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Wallpaper management
// ─────────────────────────────────────────────────────────────────────────────

void RenderEngine::loadWallpaper() {
    const QString path = Config::instance().theme.wallpaperPath;

    if (path == m_loadedWallpaperPath && !m_wallpaper.isNull()) return;

    m_wallpaper = QPixmap(path);
    if (m_wallpaper.isNull()) {
        qWarning() << "[RenderEngine] wallpaper not found:" << path
        << "— using fallback";
        if (QScreen* s = QGuiApplication::primaryScreen()) {
            m_wallpaper = generateFallbackWallpaper(s->size());
        }
    }

    m_loadedWallpaperPath = path;
    m_wallpaperDirty      = true;
    invalidateAllBlurCaches();
}

void RenderEngine::rescaleWallpaper(const QSize& outputSize) {
    if (m_wallpaper.isNull()) {
        loadWallpaper();
    }

    if (m_wallpaper.isNull()) {
        m_wallpaperScaled = {};
        m_wallpaperDirty  = false;
        return;
    }

    const QString mode = Config::instance().theme.wallpaperMode;

    if (mode == "fit") {
        m_wallpaperScaled = m_wallpaper.scaled(
            outputSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    } else if (mode == "center") {
        QPixmap canvas(outputSize);
        canvas.fill(Qt::black);
        QPainter cp(&canvas);
        const QSize s = m_wallpaper.size().scaled(
            outputSize, Qt::KeepAspectRatio);
        cp.drawPixmap((outputSize.width()  - s.width())  / 2,
                      (outputSize.height() - s.height()) / 2,
                      m_wallpaper.scaled(s, Qt::IgnoreAspectRatio,
                                         Qt::SmoothTransformation));
        m_wallpaperScaled = canvas;
    } else if (mode == "tile") {
        QPixmap canvas(outputSize);
        QPainter cp(&canvas);
        const int tw = m_wallpaper.width();
        const int th = m_wallpaper.height();
        for (int y = 0; y < outputSize.height(); y += th)
            for (int x = 0; x < outputSize.width(); x += tw)
                cp.drawPixmap(x, y, m_wallpaper);
        m_wallpaperScaled = canvas;
    } else {
        // "fill" (default): scale to cover the entire output, crop if needed.
        m_wallpaperScaled = m_wallpaper.scaled(
            outputSize,
            Qt::KeepAspectRatioByExpanding,
            Qt::SmoothTransformation);

        // Crop to exact output size if the scaled image is larger.
        if (m_wallpaperScaled.size() != outputSize) {
            const int ox = (m_wallpaperScaled.width()  - outputSize.width())  / 2;
            const int oy = (m_wallpaperScaled.height() - outputSize.height()) / 2;
            m_wallpaperScaled = m_wallpaperScaled.copy(
                ox, oy, outputSize.width(), outputSize.height());
        }
    }

    m_wallpaperDirty = false;
}

QPixmap RenderEngine::generateFallbackWallpaper(const QSize& size) const {
    QPixmap pm(size);
    QPainter p(&pm);

    // Deep-space gradient background.
    QLinearGradient bg(0, 0, 0, size.height());
    bg.setColorAt(0.0, QColor( 2,  4, 18));
    bg.setColorAt(0.5, QColor( 5, 10, 30));
    bg.setColorAt(1.0, QColor( 8, 16, 40));
    p.fillRect(QRect({}, size), bg);

    // Nebula blobs.
    struct Nebula { float x, y, r; QColor c; };
    const Nebula nebulae[] = {
        { 0.25f, 0.35f, 0.22f, QColor(40,  80, 180, 18) },
        { 0.70f, 0.60f, 0.18f, QColor(80,  40, 160, 14) },
        { 0.50f, 0.80f, 0.14f, QColor(30, 120, 200, 10) },
    };
    for (const auto& n : nebulae) {
        QRadialGradient rg(n.x * size.width(), n.y * size.height(),
                           n.r * size.width());
        rg.setColorAt(0.0, n.c);
        rg.setColorAt(1.0, Qt::transparent);
        p.fillRect(QRect({}, size), rg);
    }

    // Stars — deterministic via a fixed seed so the image is stable.
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> distX(0.f, float(size.width()));
    std::uniform_real_distribution<float> distY(0.f, float(size.height()));
    std::uniform_int_distribution<int>    distA(80, 255);
    std::uniform_real_distribution<float> distR(0.4f, 1.6f);

    p.setPen(Qt::NoPen);
    for (int i = 0; i < 800; ++i) {
        const float x = distX(rng);
        const float y = distY(rng);
        const int   a = distA(rng);
        const float r = distR(rng);
        p.setBrush(QColor(255, 255, 255, a));
        p.drawEllipse(QPointF(x, y), r, r);
    }

    return pm;
}

// ─────────────────────────────────────────────────────────────────────────────
// Blur helpers
// ─────────────────────────────────────────────────────────────────────────────

QImage RenderEngine::blurWallpaperRegion(Window* w, const QRect& windowRect) {
    if (m_wallpaperScaled.isNull()) return {};

    WindowRenderState& state = stateFor(w);

    // Re-bake only when dirty or when the crop rect has changed.
    if (!state.blurDirty && state.blurSourceRect == windowRect) {
        return state.blurredBackground;
    }

    // Crop the wallpaper region behind this window.
    const QPixmap crop = m_wallpaperScaled.copy(windowRect);
    if (crop.isNull()) return {};

    QImage src = crop.toImage().convertToFormat(
        QImage::Format_ARGB32_Premultiplied);

    const int r     = static_cast<int>(kBlurRadius);
    QImage blurred  = boxBlur(src,     r);
    blurred         = boxBlur(blurred, r / 2 + 1);

    // Saturation boost for vivid frosted-glass look.
    const float sat = Config::instance().theme.blurSaturation;
    if (!qFuzzyCompare(sat, 1.f)) {
        const int w2 = blurred.width();
        const int h2 = blurred.height();
        for (int y = 0; y < h2; ++y) {
            QRgb* line = reinterpret_cast<QRgb*>(blurred.scanLine(y));
            for (int x = 0; x < w2; ++x) {
                QColor c = QColor::fromRgba(line[x]);
                int hue, s2, lightness, alpha;
                c.getHsl(&hue, &s2, &lightness, &alpha);
                s2    = qBound(0, int(float(s2) * sat), 255);
                c.setHsl(hue, s2, lightness, alpha);
                line[x] = c.rgba();
            }
        }
    }

    state.blurredBackground = blurred;
    state.blurSourceRect    = windowRect;
    state.blurDirty         = false;

    return blurred;
}

// ─────────────────────────────────────────────────────────────────────────────
// Static box blur — 2-pass sliding-window O(w×h)
// ─────────────────────────────────────────────────────────────────────────────

QImage RenderEngine::boxBlur(const QImage& src, int radius) {
    if (radius <= 0) return src;

    const int w      = src.width();
    const int h      = src.height();
    if (w == 0 || h == 0) return src;

    const int halfR  = radius;
    const int kCount = 2 * halfR + 1;

    QImage tmp(w, h, QImage::Format_ARGB32_Premultiplied);
    QImage dst(w, h, QImage::Format_ARGB32_Premultiplied);

    // ── Horizontal pass ───────────────────────────────────────────────────
    for (int y = 0; y < h; ++y) {
        const QRgb* sl = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb*       dl = reinterpret_cast<QRgb*>(tmp.scanLine(y));

        qint32 r = 0, g = 0, b = 0, a = 0;
        for (int k = -halfR; k <= halfR; ++k) {
            const QRgb px = sl[qBound(0, k, w - 1)];
            r += qRed(px); g += qGreen(px); b += qBlue(px); a += qAlpha(px);
        }
        for (int x = 0; x < w; ++x) {
            dl[x] = qRgba(r/kCount, g/kCount, b/kCount, a/kCount);
            const QRgb rem = sl[qBound(0, x - halfR,     w - 1)];
            const QRgb add = sl[qBound(0, x + halfR + 1, w - 1)];
            r += qRed(add)  - qRed(rem);
            g += qGreen(add)- qGreen(rem);
            b += qBlue(add) - qBlue(rem);
            a += qAlpha(add)- qAlpha(rem);
        }
    }

    // ── Vertical pass ─────────────────────────────────────────────────────
    for (int x = 0; x < w; ++x) {
        qint32 r = 0, g = 0, b = 0, a = 0;
        for (int k = -halfR; k <= halfR; ++k) {
            const QRgb px = reinterpret_cast<const QRgb*>(
                tmp.constScanLine(qBound(0, k, h-1)))[x];
                r += qRed(px); g += qGreen(px); b += qBlue(px); a += qAlpha(px);
        }
        for (int y = 0; y < h; ++y) {
            reinterpret_cast<QRgb*>(dst.scanLine(y))[x] =
            qRgba(r/kCount, g/kCount, b/kCount, a/kCount);
            const QRgb rem = reinterpret_cast<const QRgb*>(
                tmp.constScanLine(qBound(0, y - halfR,     h-1)))[x];
                const QRgb add = reinterpret_cast<const QRgb*>(
                    tmp.constScanLine(qBound(0, y + halfR + 1, h-1)))[x];
                    r += qRed(add)  - qRed(rem);
                    g += qGreen(add)- qGreen(rem);
                    b += qBlue(add) - qBlue(rem);
                    a += qAlpha(add)- qAlpha(rem);
        }
    }

    return dst;
}

// ─────────────────────────────────────────────────────────────────────────────
// Cache management
// ─────────────────────────────────────────────────────────────────────────────

WindowRenderState& RenderEngine::stateFor(Window* w) {
    auto it = m_states.find(w);
    if (it == m_states.end()) {
        it = m_states.insert(w, WindowRenderState{});
    }
    return it.value();
}

void RenderEngine::invalidateAllBlurCaches() {
    for (auto& state : m_states) {
        state.blurDirty = true;
    }
}

void RenderEngine::invalidateBlurCache(Window* w) {
    auto it = m_states.find(w);
    if (it != m_states.end()) {
        it->blurDirty = true;
    }
}

void RenderEngine::onWindowRemoved(Window* w) {
    m_states.remove(w);
}

// ─────────────────────────────────────────────────────────────────────────────
// Font helpers
// ─────────────────────────────────────────────────────────────────────────────

QFont RenderEngine::titleFont() const {
    const auto& theme = Config::instance().theme;
    QFont f(theme.fontFamily);
    f.setPointSize(theme.fontSizeUI);
    f.setStyleHint(QFont::SansSerif);
    return f;
}

QFont RenderEngine::uiFont(int size) const {
    const auto& theme = Config::instance().theme;
    QFont f(theme.fontFamily);
    f.setPointSize(size > 0 ? size : theme.fontSizeUI);
    f.setStyleHint(QFont::SansSerif);
    return f;
}

// ─────────────────────────────────────────────────────────────────────────────
// Colour helper
// ─────────────────────────────────────────────────────────────────────────────

QColor RenderEngine::lerpColor(const QColor& a, const QColor& b, float t) {
    return QColor(
        int(a.red()   + (b.red()   - a.red())   * t),
                  int(a.green() + (b.green() - a.green()) * t),
                  int(a.blue()  + (b.blue()  - a.blue())  * t),
                  int(a.alpha() + (b.alpha() - a.alpha()) * t));
}
