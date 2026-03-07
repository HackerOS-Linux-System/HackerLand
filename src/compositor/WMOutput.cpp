#include "WMOutput.h"
#include "WMCompositor.h"
#include "core/Window.h"
#include "core/Workspace.h"      // ← full definition needed for ws->windows()
#include "core/Config.h"
#include "core/TilingEngine.h"

#include <QPainter>
#include <QPainterPath>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QTimer>
#include <QDebug>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QConicalGradient>
#include <QScreen>
#include <QtMath>
#include <QCoreApplication>

#include <cstdlib>   // std::srand / std::rand  (replaces removed qsrand/qrand)
#include <ctime>

// ─────────────────────────────────────────────────────────────────────────────
// Internal box-blur helper
// ─────────────────────────────────────────────────────────────────────────────
static QImage blurImage(const QImage& src, int radius) {
    if (radius < 1) return src;
    QImage result = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int w = result.width(), h = result.height();

    for (int pass = 0; pass < 3; ++pass) {
        QImage tmp = result;
        // Horizontal pass
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int r=0, g=0, b=0, a=0, cnt=0;
                for (int dx = -radius; dx <= radius; ++dx) {
                    int nx = qBound(0, x+dx, w-1);
                    QRgb px = tmp.pixel(nx, y);
                    r += qRed(px); g += qGreen(px);
                    b += qBlue(px); a += qAlpha(px); ++cnt;
                }
                result.setPixel(x, y, qRgba(r/cnt, g/cnt, b/cnt, a/cnt));
            }
        }
        // Vertical pass
        tmp = result;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int r=0, g=0, b=0, a=0, cnt=0;
                for (int dy = -radius; dy <= radius; ++dy) {
                    int ny = qBound(0, y+dy, h-1);
                    QRgb px = tmp.pixel(x, ny);
                    r += qRed(px); g += qGreen(px);
                    b += qBlue(px); a += qAlpha(px); ++cnt;
                }
                result.setPixel(x, y, qRgba(r/cnt, g/cnt, b/cnt, a/cnt));
            }
        }
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Procedural fallback wallpaper
// ─────────────────────────────────────────────────────────────────────────────
static QPixmap makeFallbackWallpaper(int w, int h) {
    QPixmap wp(w, h);
    QPainter p(&wp);

    // Deep-space gradient
    QLinearGradient grad(0, 0, w, h);
    grad.setColorAt(0.0, QColor(5,  8,  20));
    grad.setColorAt(0.3, QColor(10, 18, 45));
    grad.setColorAt(0.6, QColor(15, 25, 60));
    grad.setColorAt(1.0, QColor(8,  14, 35));
    p.fillRect(wp.rect(), grad);

    // Nebula glows
    p.setCompositionMode(QPainter::CompositionMode_Screen);
    const struct { float x, y, r; QColor c; } nebulae[] = {
        { 0.25f, 0.28f, 0.21f, QColor(40,  80, 180, 60) },
        { 0.73f, 0.65f, 0.18f, QColor(80,  30, 160, 50) },
        { 0.50f, 0.83f, 0.14f, QColor(20, 120, 140, 40) },
    };
    for (const auto& n : nebulae) {
        QRadialGradient neb(n.x * w, n.y * h, n.r * w);
        neb.setColorAt(0, n.c);
        neb.setColorAt(1, Qt::transparent);
        p.fillRect(wp.rect(), neb);
    }

    // Stars — deterministic via fixed seed
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    std::srand(42);
    for (int i = 0; i < 350; ++i) {
        const int   sx = std::rand() % w;
        const int   sy = std::rand() % h;
        const int   sr = std::rand() % 3;
        const int   sa = 80 + std::rand() % 160;
        const float sz = (sr == 0) ? 1.5f : 0.8f;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, sa));
        p.drawEllipse(QPointF(sx, sy), sz, sz);
    }
    p.end();
    return wp;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / destructor
// ─────────────────────────────────────────────────────────────────────────────

WMOutput::WMOutput(WMCompositor* compositor, QScreen* screen, QObject* parent)
: QOpenGLWidget(nullptr)
, m_compositor(compositor)
, m_screen(screen)
{
    Q_UNUSED(parent);

    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMouseTracking(true);

    // Load wallpaper from config
    auto& cfg = Config::instance();
    m_wallpaper.load(cfg.theme.wallpaperPath);
    if (m_wallpaper.isNull()) {
        m_wallpaper = makeFallbackWallpaper(1920, 1080);
    }

    // Render timer at ~60 fps
    m_renderTimer = new QTimer(this);
    m_renderTimer->setInterval(16);
    connect(m_renderTimer, &QTimer::timeout, this, &WMOutput::onRenderTick);

    // Redraw on compositor state changes
    connect(compositor, &WMCompositor::tiledWindowsChanged,    this, [this]{ update(); });
    connect(compositor, &WMCompositor::windowAdded,            this, [this](Window*){ update(); });
    connect(compositor, &WMCompositor::windowRemoved,          this, [this](Window*){ update(); });
    connect(compositor, &WMCompositor::activeWindowChanged,    this, [this](Window*){ update(); });
    connect(compositor, &WMCompositor::activeWorkspaceChanged, this, [this](int){ update(); });
}

WMOutput::~WMOutput() = default;

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void WMOutput::show() {
    if (m_screen) setGeometry(m_screen->geometry());
    QOpenGLWidget::show();
    m_renderTimer->start();
}

void WMOutput::hide() {
    m_renderTimer->stop();
    QOpenGLWidget::hide();
}

// ─────────────────────────────────────────────────────────────────────────────
// Qt GL overrides (minimal — we do everything in paintEvent via QPainter)
// ─────────────────────────────────────────────────────────────────────────────

void WMOutput::initializeGL()          {}
void WMOutput::resizeGL(int, int)      {}
void WMOutput::paintGL()               { paintEvent(nullptr); }

// ─────────────────────────────────────────────────────────────────────────────
// Render tick slot
// ─────────────────────────────────────────────────────────────────────────────

void WMOutput::onRenderTick() {
    // Advance glow pulse
    m_glowPulse += kGlowPulseSpeed * m_glowDir;
    if (m_glowPulse >= 1.0f) { m_glowPulse = 1.0f; m_glowDir = -1.0f; }
    if (m_glowPulse <= 0.3f) { m_glowPulse = 0.3f; m_glowDir =  1.0f; }
    update();
}

// ─────────────────────────────────────────────────────────────────────────────
// Resize
// ─────────────────────────────────────────────────────────────────────────────

void WMOutput::resizeEvent(QResizeEvent* e) {
    QOpenGLWidget::resizeEvent(e);
    if (!m_wallpaper.isNull()) {
        m_wallpaperScaled = m_wallpaper.scaled(
            size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    }
    m_blurCache = QPixmap();   // invalidate wallpaper blur cache
}

// ─────────────────────────────────────────────────────────────────────────────
// Paint
// ─────────────────────────────────────────────────────────────────────────────

void WMOutput::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing |
    QPainter::SmoothPixmapTransform |
    QPainter::TextAntialiasing);

    drawWallpaper(p);
    drawWindows(p);
    drawCursor(p);
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw passes
// ─────────────────────────────────────────────────────────────────────────────

void WMOutput::drawWallpaper(QPainter& p) {
    if (!m_wallpaperScaled.isNull()) {
        const int xOff = (m_wallpaperScaled.width()  - width())  / 2;
        const int yOff = (m_wallpaperScaled.height() - height()) / 2;
        p.drawPixmap(0, 0, m_wallpaperScaled, xOff, yOff, width(), height());
    } else {
        QLinearGradient grad(0, 0, width(), height());
        grad.setColorAt(0, QColor(5,  8,  20));
        grad.setColorAt(1, QColor(15, 25, 60));
        p.fillRect(rect(), grad);
    }

    // Subtle edge vignette
    QRadialGradient vignette(QPointF(width() / 2.0, height() / 2.0),
                             qMax(width(), height()) * 0.72);
    vignette.setColorAt(0.55, Qt::transparent);
    vignette.setColorAt(1.0,  QColor(0, 0, 0, 80));
    p.fillRect(rect(), vignette);
}

void WMOutput::drawVignette(QPainter& p) {
    // Already included in drawWallpaper; kept for API completeness.
    Q_UNUSED(p);
}

void WMOutput::drawWindows(QPainter& p) {
    auto* ws = m_compositor->activeWorkspace();
    if (!ws) return;

    // Pull full Workspace definition via #include "core/Workspace.h"
    const QList<Window*> windows = ws->visibleWindows();
    Window* const active = ws->activeWindow();

    // Z-order: tiled first, then floating, active always on top
    QList<Window*> tiled, floating;
    for (auto* w : windows) {
        if (w->isFloating()) floating.append(w);
        else                 tiled.append(w);
    }
    // Move active to end of its group
    auto moveActiveToEnd = [&](QList<Window*>& list) {
        if (active && list.contains(active)) {
            list.removeOne(active);
            list.append(active);
        }
    };
    moveActiveToEnd(tiled);
    moveActiveToEnd(floating);

    for (auto* w : tiled)    drawWindow(p, w, w == active);
    for (auto* w : floating) drawWindow(p, w, w == active);
}

void WMOutput::drawWindow(QPainter& p, Window* w, bool isActive) {
    const QRect  geom  = w->geometry();
    if (geom.isEmpty()) return;

    p.save();
    p.setOpacity(qBound(0.f, w->opacity(), 1.f));

    drawWindowShadow       (p, geom, isActive);
    drawWindowBlurBackground(p, w,   geom);
    drawWindowGlassOverlay (p, geom, isActive);
    drawWindowBorder       (p, geom, isActive);
    drawWindowContent      (p, w);

    p.restore();
}

void WMOutput::drawWindowShadow(QPainter& p, const QRect& rect, bool active) {
    const auto& theme  = Config::instance().theme;
    const int   spread = active ? 24 : 14;
    const float alpha  = active ? 0.55f : 0.35f;

    p.setPen(Qt::NoPen);
    for (int i = spread; i > 0; i -= 2) {
        const float a = alpha * float(i) / float(spread) * 0.4f;
        QColor sc = theme.shadowColor;
        sc.setAlphaF(a);
        QPainterPath shadow;
        const float sr = theme.borderRadius + i * 0.5f;
        shadow.addRoundedRect(
            rect.adjusted(-i, -i/2, i, i + i/2), sr, sr);
        p.fillPath(shadow, sc);
    }
}

void WMOutput::drawWindowBlurBackground(QPainter& p, Window* w, const QRect& rect) {
    if (m_wallpaperScaled.isNull()) return;
    const auto& theme = Config::instance().theme;

    const int xOff = (m_wallpaperScaled.width()  - width())  / 2;
    const int yOff = (m_wallpaperScaled.height() - height()) / 2;
    QRect srcRect  = rect.translated(xOff, yOff)
    .intersected(m_wallpaperScaled.rect());
    if (srcRect.isEmpty()) return;

    // Cache blur per window
    WindowRenderState& state = renderStateFor(w);
    if (state.blurDirty || state.blurSourceRect != rect) {
        QPixmap slice  = m_wallpaperScaled.copy(srcRect);
        QImage blurred = blurImage(slice.toImage(), 12);
        state.blurredBackground = blurred;
        state.blurSourceRect    = rect;
        state.blurDirty         = false;
    }

    QPainterPath clip;
    clip.addRoundedRect(rect, theme.borderRadius, theme.borderRadius);
    p.save();
    p.setClipPath(clip);
    p.drawImage(rect.topLeft(), state.blurredBackground);
    p.restore();
}

void WMOutput::drawWindowGlassOverlay(QPainter& p, const QRect& rect, bool active) {
    const auto& theme = Config::instance().theme;
    const int   r     = theme.borderRadius;

    QPainterPath path;
    path.addRoundedRect(rect, r, r);
    p.save();
    p.setClipPath(path);

    // Base glass fill
    QColor bg = theme.glassBackground;
    if (!active) bg.setAlpha(qMin(255, bg.alpha() + 20));
    p.fillPath(path, bg);

    // Vertical depth gradient
    QLinearGradient depth(rect.topLeft(), rect.bottomLeft());
    depth.setColorAt(0.0,  QColor(255, 255, 255, active ? 12 : 6));
    depth.setColorAt(0.45, Qt::transparent);
    depth.setColorAt(1.0,  QColor(0, 0, 0, active ? 18 : 28));
    p.fillPath(path, depth);

    // Top-edge shimmer
    QPainterPath shimmer;
    shimmer.addRect(QRectF(rect.left(), rect.top(), rect.width(), r * 2.0));
    QLinearGradient shim(rect.topLeft(), QPointF(rect.left(), rect.top() + r * 2));
    shim.setColorAt(0.0, QColor(255, 255, 255, 30));
    shim.setColorAt(1.0, Qt::transparent);
    p.fillPath(shimmer.intersected(path), shim);

    p.restore();
}

void WMOutput::drawWindowBorder(QPainter& p, const QRect& rect, bool active) {
    const auto& theme = Config::instance().theme;
    const int   r     = theme.borderRadius;

    QPainterPath border;
    border.addRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5),
                          r - 0.5, r - 0.5);
    p.setBrush(Qt::NoBrush);

    if (!active) {
        p.setPen(QPen(theme.glassBorder, theme.borderWidth));
        p.drawPath(border);
        return;
    }

    // Outer glow layers
    for (int glow = 3; glow > 0; --glow) {
        QColor gc = theme.glassBorderActive;
        gc.setAlpha(gc.alpha() / (glow + 1));
        QPen pen(gc, theme.borderWidth + glow * 2.0);
        p.strokePath(border, pen);
    }

    // Main conical gradient border
    QLinearGradient borderGrad(rect.topLeft(), rect.bottomRight());
    borderGrad.setColorAt(0.0, theme.accentColor);
    borderGrad.setColorAt(0.5, theme.accentSecondary);
    borderGrad.setColorAt(1.0, theme.accentTertiary);
    p.strokePath(border, QPen(QBrush(borderGrad), theme.borderWidth));
}

void WMOutput::drawTitleBar(QPainter& p, Window* w, bool active) {
    drawWindowContent(p, w);
    Q_UNUSED(active);
}

void WMOutput::drawTitleBarSeparator(QPainter& p, const QRect& rect, bool active) {
    const int y = rect.top() + kTitleBarHeight;
    QLinearGradient sep(rect.left(), y, rect.right(), y);
    const int a = active ? 40 : 20;
    sep.setColorAt(0.0, Qt::transparent);
    sep.setColorAt(0.1, QColor(255, 255, 255, a));
    sep.setColorAt(0.9, QColor(255, 255, 255, a));
    sep.setColorAt(1.0, Qt::transparent);
    p.setPen(QPen(QBrush(sep), 1.0));
    p.drawLine(rect.left(), y, rect.right(), y);
}

void WMOutput::drawWindowContent(QPainter& p, Window* w) {
    const auto& theme   = Config::instance().theme;
    const QRect geom    = w->geometry();
    const bool  isActive = w->isActive();
    const int   r       = theme.borderRadius;

    // ── Title bar background ──────────────────────────────────────────────
    const QRect titleRect(geom.x(), geom.y(), geom.width(), kTitleBarHeight);
    QPainterPath titlePath;
    titlePath.addRoundedRect(titleRect, r, r);
    titlePath.addRect(geom.x(), geom.y() + kTitleBarHeight / 2,
                      geom.width(), kTitleBarHeight / 2);
    QLinearGradient tg(titleRect.topLeft(), titleRect.bottomLeft());
    tg.setColorAt(0, isActive ? QColor(255,255,255,18) : QColor(255,255,255,8));
    tg.setColorAt(1, Qt::transparent);
    p.fillPath(titlePath, tg);

    // ── Traffic-light dots ────────────────────────────────────────────────
    const int dotY  = geom.y() + kTitleBarHeight / 2;
    const int dotX  = geom.right() - kDotRightMargin;

    struct DotColor { QColor inactive, active; };
    static const DotColor dots[3] = {
        { QColor(120,  50,  50, 180), QColor(255,  95,  86) }, // close
        { QColor(110, 110,  50, 180), QColor(255, 189,  46) }, // maximise
        { QColor( 50, 110,  50, 180), QColor( 39, 201,  63) }, // minimise
    };
    p.setPen(Qt::NoPen);
    for (int i = 0; i < 3; ++i) {
        const QColor& col = isActive ? dots[i].active : dots[i].inactive;
        p.setBrush(col);
        p.drawEllipse(QPoint(dotX - i * kDotSpacing, dotY),
                      kDotRadius, kDotRadius);
    }

    // ── App icon ──────────────────────────────────────────────────────────
    if (!w->icon().isNull()) {
        QPixmap icon = w->icon().pixmap(16, 16);
        p.drawPixmap(geom.x() + 8, dotY - 8, 16, 16, icon);
    }

    // ── Title text ────────────────────────────────────────────────────────
    QFont font(theme.fontFamily);
    font.setPixelSize(theme.fontSizeUI);
    p.setFont(font);
    p.setPen(isActive ? theme.textPrimary : theme.textSecondary);

    QString title = w->title();
    if (title.isEmpty()) title = w->appId();
    if (title.isEmpty()) title = "Window";

    const int iconEnd  = geom.x() + 28;
    const int dotsLeft = dotX - 2 * kDotSpacing - kDotRadius * 2 - 4;
    const QRect textRect(iconEnd, geom.y(), dotsLeft - iconEnd, kTitleBarHeight);
    p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
               p.fontMetrics().elidedText(title, Qt::ElideRight, textRect.width()));

    // ── Separator ─────────────────────────────────────────────────────────
    if (isActive) {
        p.setPen(QPen(QColor(255, 255, 255, 22), 1));
        p.drawLine(geom.x() + r, geom.y() + kTitleBarHeight,
                   geom.right() - r, geom.y() + kTitleBarHeight);
    }
}

void WMOutput::drawCursor(QPainter& p) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(m_cursorPos);

    // Outer ring
    p.setPen(QPen(QColor(255, 255, 255, 200), 1.5));
    p.setBrush(QColor(100, 180, 255, 30));
    p.drawEllipse(QPoint(0, 0), 6, 6);

    // Inner dot
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 240));
    p.drawEllipse(QPoint(0, 0), 2, 2);

    p.restore();
}

// ─────────────────────────────────────────────────────────────────────────────
// Wallpaper / blur helpers
// ─────────────────────────────────────────────────────────────────────────────

void WMOutput::loadWallpaper() {
    m_wallpaper.load(Config::instance().theme.wallpaperPath);
    if (m_wallpaper.isNull())
        m_wallpaper = makeFallbackWallpaper(1920, 1080);
    m_wallpaperDirty = true;
    invalidateAllBlurCaches();
}

void WMOutput::rescaleWallpaper() {
    if (m_wallpaper.isNull()) return;
    m_wallpaperScaled = m_wallpaper.scaled(
        size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    m_wallpaperDirty = false;
}

QPixmap WMOutput::generateFallbackWallpaper(const QSize& size) const {
    return makeFallbackWallpaper(size.width(), size.height());
}

QImage WMOutput::blurWallpaperRegion(Window* /*w*/, const QRect& /*rect*/) {
    return {};   // handled inline in drawWindowBlurBackground
}

QImage WMOutput::boxBlur(const QImage& src, int radius) {
    return blurImage(src, radius);
}

WindowRenderState& WMOutput::renderStateFor(Window* w) {
    return m_renderStates[w];
}

void WMOutput::invalidateAllBlurCaches() {
    for (auto& s : m_renderStates) s.blurDirty = true;
    m_blurCache = QPixmap();
}

// ─────────────────────────────────────────────────────────────────────────────
// Input
// ─────────────────────────────────────────────────────────────────────────────

void WMOutput::keyPressEvent(QKeyEvent* e) {
    const auto& bindings = Config::instance().keys.bindings;

    QString mod;
    if (e->modifiers() & Qt::MetaModifier)    mod += "Super+";
    if (e->modifiers() & Qt::AltModifier)     mod += "Alt+";
    if (e->modifiers() & Qt::ControlModifier) mod += "Ctrl+";
    if (e->modifiers() & Qt::ShiftModifier)   mod += "Shift+";

    const QString combo = mod + QKeySequence(e->key()).toString();

    if (bindings.contains(combo)) {
        handleAction(bindings[combo]);
        e->accept();
        return;
    }
    QOpenGLWidget::keyPressEvent(e);
}

void WMOutput::keyReleaseEvent(QKeyEvent* e) {
    QOpenGLWidget::keyReleaseEvent(e);
}

void WMOutput::handleAction(const QString& action) {
    if      (action.startsWith("exec:"))
        m_compositor->launchApp(action.mid(5));
    else if (action.startsWith("workspace:"))
        m_compositor->switchWorkspace(action.mid(10).toInt());
    else if (action.startsWith("move_to_workspace:")) {
        if (auto* w = m_compositor->activeWindow())
            m_compositor->moveWindowToWorkspace(w, action.mid(18).toInt());
    }
    else if (action.startsWith("focus:"))
        m_compositor->focusDirection(action.mid(6));
    else if (action.startsWith("move:"))
        m_compositor->moveWindowDirection(action.mid(5));
    else if (action.startsWith("layout:"))
        m_compositor->setLayout(TilingEngine::layoutFromString(action.mid(7)));
    else if (action == "close") {
        if (auto* w = m_compositor->activeWindow())
            m_compositor->closeWindow(w);
    }
    else if (action == "fullscreen")    m_compositor->toggleFullscreen();
    else if (action == "float_toggle")  m_compositor->toggleFloat();
    else if (action == "reload_config") m_compositor->reloadConfig();
    else if (action == "quit")          QCoreApplication::quit();
}

void WMOutput::mousePressEvent(QMouseEvent* e) {
    m_cursorPos = e->pos();

    auto* ws = m_compositor->activeWorkspace();
    if (!ws) return;

    const QList<Window*> windows = ws->visibleWindows();
    Window* clicked = nullptr;
    for (int i = windows.size() - 1; i >= 0; --i) {
        if (windows[i]->isVisible() && windows[i]->geometry().contains(e->pos())) {
            clicked = windows[i];
            break;
        }
    }

    if (clicked) {
        m_compositor->focusWindow(clicked);

        const QRect titleBar(clicked->geometry().x(),
                             clicked->geometry().y(),
                             clicked->geometry().width(), kTitleBarHeight);

        // Close button hit-test
        const int dotY   = clicked->geometry().y() + kTitleBarHeight / 2;
        const int closeX = clicked->geometry().right() - kDotRightMargin;
        if (QRect(closeX - 8, dotY - 8, 16, 16).contains(e->pos())) {
            m_compositor->closeWindow(clicked);
            return;
        }

        // Drag via title bar
        if (titleBar.contains(e->pos()) && e->button() == Qt::LeftButton) {
            m_dragging   = true;
            m_dragWindow = clicked;
            m_dragOffset = e->pos() - clicked->geometry().topLeft();
            if (!clicked->isFloating()) clicked->toggleFloat();
        }
    }

    update();
}

void WMOutput::mouseReleaseEvent(QMouseEvent* e) {
    Q_UNUSED(e);
    m_dragging   = false;
    m_dragWindow = nullptr;
    update();
}

void WMOutput::mouseMoveEvent(QMouseEvent* e) {
    m_cursorPos = e->pos();

    if (m_dragging && m_dragWindow) {
        QRect geom = m_dragWindow->geometry();
        geom.moveTopLeft(e->pos() - m_dragOffset);
        m_dragWindow->setGeometry(geom);
        renderStateFor(m_dragWindow).blurDirty = true;
    }

    update();
}

void WMOutput::wheelEvent(QWheelEvent* e) {
    QOpenGLWidget::wheelEvent(e);
}

void WMOutput::enterEvent(QEnterEvent* e) {
    QOpenGLWidget::enterEvent(e);
}

void WMOutput::leaveEvent(QEvent* e) {
    QOpenGLWidget::leaveEvent(e);
}

// ─────────────────────────────────────────────────────────────────────────────
// Hit-testing helpers
// ─────────────────────────────────────────────────────────────────────────────

Window* WMOutput::windowAt(const QPoint& pos) const {
    auto* ws = m_compositor->activeWorkspace();
    if (!ws) return nullptr;
    const auto wins = ws->visibleWindows();
    for (int i = wins.size() - 1; i >= 0; --i) {
        if (wins[i]->isVisible() && wins[i]->geometry().contains(pos))
            return wins[i];
    }
    return nullptr;
}

ResizeEdge WMOutput::resizeEdgeAt(const QPoint& pos, Window* w) const {
    const QRect g = w->geometry();
    const int   d = kResizeHandleWidth;
    int edge = (int)ResizeEdge::None;
    if (pos.y() < g.top()    + d) edge |= (int)ResizeEdge::Top;
    if (pos.y() > g.bottom() - d) edge |= (int)ResizeEdge::Bottom;
    if (pos.x() < g.left()   + d) edge |= (int)ResizeEdge::Left;
    if (pos.x() > g.right()  - d) edge |= (int)ResizeEdge::Right;
    return static_cast<ResizeEdge>(edge);
}

void WMOutput::updateCursorShape() { /* TODO */ }

WMOutput::TitleBarButton WMOutput::titleBarButtonAt(const QPoint& pos,
                                                    Window* w) const {
                                                        const QRect g    = w->geometry();
                                                        const int   dotY = g.y() + kTitleBarHeight / 2;
                                                        const int   dotX = g.right() - kDotRightMargin;
                                                        if (QRect(dotX - 8,            dotY - 8, 16, 16).contains(pos)) return TitleBarButton::Close;
                                                        if (QRect(dotX - kDotSpacing - 8, dotY - 8, 16, 16).contains(pos)) return TitleBarButton::Maximize;
                                                        if (QRect(dotX - kDotSpacing * 2 - 8, dotY - 8, 16, 16).contains(pos)) return TitleBarButton::Minimize;
                                                        return TitleBarButton::None;
                                                    }

                                                    QRect WMOutput::closeButtonRect(const QRect& r) const {
                                                        return QRect(r.right() - kDotRightMargin - kDotRadius,
                                                                     r.top() + kTitleBarHeight / 2 - kDotRadius,
                                                                     kDotRadius * 2, kDotRadius * 2);
                                                    }

                                                    QRect WMOutput::maximizeButtonRect(const QRect& r) const {
                                                        return closeButtonRect(r).translated(-kDotSpacing, 0);
                                                    }

                                                    QRect WMOutput::minimizeButtonRect(const QRect& r) const {
                                                        return closeButtonRect(r).translated(-kDotSpacing * 2, 0);
                                                    }

                                                    void WMOutput::dispatchKeybind(QKeyEvent* e) { keyPressEvent(e); }
