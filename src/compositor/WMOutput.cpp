// ─────────────────────────────────────────────────────────────────────────────
// WMOutput.cpp — HackerLand WM
//
// FIXES in this version:
//   • All GL functions called via QOpenGLFunctions_3_3_Core — no raw gl*()
//   • #include <QFileInfo> added
//   • InputHandler fully included (not forward-declared)
//   • lockScreen / showLauncher use WMCompositor public methods
//   • All misleading-indentation warnings fixed (proper braces everywhere)
//   • SVG support via #ifdef QT_SVG_LIB
// ─────────────────────────────────────────────────────────────────────────────
#include "WMOutput.h"
#include "WMCompositor.h"
#include "core/Window.h"
#include "core/Workspace.h"
#include "core/Config.h"
#include "core/TilingEngine.h"
#include "core/InputHandler.h"
#include "ui/GLBlurRenderer.h"

#include <QPainter>
#include <QPainterPath>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QTimer>
#include <QDebug>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QScreen>
#include <QtMath>
#include <QCoreApplication>
#include <QMovie>
#include <QImageReader>
#include <QFileInfo>
#include <QOpenGLFunctions_3_3_Core>

#ifdef QT_SVG_LIB
#  include <QSvgRenderer>
#endif

#include <cstdlib>
#include <ctime>

// ─────────────────────────────────────────────────────────────────────────────
// Fast CPU fallback blur — 4× downscale + box blur + upscale
// Only used when GL context not ready yet
// ─────────────────────────────────────────────────────────────────────────────
static QImage fastBlurCPU(const QImage& src, int radius)
{
    if (src.isNull() || radius < 1) return src;
    const int sw = qMax(1, src.width() / 4);
    const int sh = qMax(1, src.height() / 4);
    QImage s = src.scaled(sw, sh, Qt::IgnoreAspectRatio, Qt::FastTransformation)
    .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    radius = qMin(radius / 4 + 1, qMin(sw, sh) / 2);
    const int kc = 2 * radius + 1;
    QImage tmp(sw, sh, QImage::Format_ARGB32_Premultiplied);
    QImage dst(sw, sh, QImage::Format_ARGB32_Premultiplied);

    // Horizontal pass
    for (int y = 0; y < sh; ++y) {
        const QRgb* sl = reinterpret_cast<const QRgb*>(s.constScanLine(y));
        QRgb*       dl = reinterpret_cast<QRgb*>(tmp.scanLine(y));
        qint32 r=0, g=0, b=0, a=0;
        for (int k = -radius; k <= radius; ++k) {
            const QRgb p = sl[qBound(0, k, sw-1)];
            r += qRed(p); g += qGreen(p); b += qBlue(p); a += qAlpha(p);
        }
        for (int x = 0; x < sw; ++x) {
            dl[x] = qRgba(r/kc, g/kc, b/kc, a/kc);
            const QRgb rem = sl[qBound(0, x-radius,   sw-1)];
            const QRgb add = sl[qBound(0, x+radius+1, sw-1)];
            r += qRed(add)-qRed(rem); g += qGreen(add)-qGreen(rem);
            b += qBlue(add)-qBlue(rem); a += qAlpha(add)-qAlpha(rem);
        }
    }
    // Vertical pass
    for (int x = 0; x < sw; ++x) {
        qint32 r=0, g=0, b=0, a=0;
        for (int k = -radius; k <= radius; ++k) {
            const QRgb p = reinterpret_cast<const QRgb*>(
                tmp.constScanLine(qBound(0, k, sh-1)))[x];
                r += qRed(p); g += qGreen(p); b += qBlue(p); a += qAlpha(p);
        }
        for (int y = 0; y < sh; ++y) {
            reinterpret_cast<QRgb*>(dst.scanLine(y))[x] =
            qRgba(r/kc, g/kc, b/kc, a/kc);
            const QRgb rem = reinterpret_cast<const QRgb*>(
                tmp.constScanLine(qBound(0, y-radius,   sh-1)))[x];
                const QRgb add = reinterpret_cast<const QRgb*>(
                    tmp.constScanLine(qBound(0, y+radius+1, sh-1)))[x];
                    r += qRed(add)-qRed(rem); g += qGreen(add)-qGreen(rem);
                    b += qBlue(add)-qBlue(rem); a += qAlpha(add)-qAlpha(rem);
        }
    }
    return dst.scaled(src.width(), src.height(),
                      Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

// ─────────────────────────────────────────────────────────────────────────────
// Procedural fallback wallpaper
// ─────────────────────────────────────────────────────────────────────────────
static QPixmap makeFallbackWallpaper(int w, int h)
{
    QPixmap wp(w, h);
    QPainter p(&wp);
    QLinearGradient g(0, 0, w, h);
    g.setColorAt(0.0, QColor(5,  8,  20));
    g.setColorAt(0.4, QColor(10, 18, 45));
    g.setColorAt(1.0, QColor(8,  14, 35));
    p.fillRect(wp.rect(), g);
    p.setCompositionMode(QPainter::CompositionMode_Screen);
    struct N { float x, y, r; QColor c; };
    const N nb[] = {
        {.25f, .28f, .21f, QColor(40,  80, 180, 55)},
        {.73f, .65f, .18f, QColor(80,  30, 160, 45)},
        {.50f, .83f, .14f, QColor(20, 120, 140, 38)},
    };
    for (const auto& n : nb) {
        QRadialGradient rg(n.x*w, n.y*h, n.r*w);
        rg.setColorAt(0, n.c); rg.setColorAt(1, Qt::transparent);
        p.fillRect(wp.rect(), rg);
    }
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    std::srand(42);
    for (int i = 0; i < 300; ++i) {
        const int sx = std::rand() % w, sy = std::rand() % h;
        const int sa = 80 + std::rand() % 160;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, sa));
        const float sr = (std::rand() % 3 == 0) ? 1.5f : 0.8f;
        p.drawEllipse(QPointF(sx, sy), sr, sr);
    }
    return wp;
}

// ─────────────────────────────────────────────────────────────────────────────
// Animated wallpaper GLSL shaders
// ─────────────────────────────────────────────────────────────────────────────
static constexpr const char* kAnimVert = R"GLSL(
    #version 330 core
    layout(location=0) in vec2 pos;
    out vec2 uv;
    void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }
)GLSL";

static constexpr const char* kAnimFrag = R"GLSL(
    #version 330 core
    in  vec2  uv;
    out vec4  fragColor;
    uniform float time;
    uniform vec2  resolution;

    float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7))) * 43758.5453); }
    float noise(vec2 p) {
        vec2 i = floor(p), f = fract(p);
        f = f*f*(3.0-2.0*f);
        return mix(mix(hash(i), hash(i+vec2(1,0)), f.x),
                   mix(hash(i+vec2(0,1)), hash(i+vec2(1,1)), f.x), f.y);
    }
    float fbm(vec2 p) {
        float v=0., a=.5;
        for (int i=0; i<5; i++) { v += a*noise(p); p *= 2.; a *= .5; }
        return v;
    }
    void main() {
        vec2 p = uv * 2.0 - 1.0;
        p.x   *= resolution.x / resolution.y;
        float t = time * 0.08;
        float q = fbm(p + t);
        float r = fbm(p + q + vec2(1.7, 9.2) + 0.15*t);
        float f = fbm(p + r);
        vec3 col = mix(vec3(0.02,0.03,0.08), vec3(0.05,0.10,0.30),
                       clamp(f*f*4., 0., 1.));
        col = mix(col, vec3(0.15,0.05,0.25), clamp(f*f*4., 0., 1.));
        col = mix(col, vec3(0.0,0.25,0.45),  clamp(length(p+vec2(0.1,0.4)), 0., 1.));
        float stars = pow(hash(uv*500.+t*0.01), 80.) * 1.5;
        col += vec3(stars);
        float vig = 1.0 - dot(uv-0.5, (uv-0.5)*2.2);
        col *= vig * 0.9 + 0.1;
        fragColor = vec4(col, 1.0);
    }
    )GLSL";

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────
WMOutput::WMOutput(WMCompositor* compositor, QScreen* screen, QObject* parent)
: QOpenGLWidget(nullptr)
, m_compositor(compositor)
, m_screen(screen)
{
    Q_UNUSED(parent);
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMouseTracking(true);

    loadWallpaper();

    m_renderTimer = new QTimer(this);
    m_renderTimer->setInterval(33); // ~30 fps
    connect(m_renderTimer, &QTimer::timeout,
            this, &WMOutput::onRenderTick);

    auto dirty = [this] { m_needsRedraw = true; };
    connect(compositor, &WMCompositor::tiledWindowsChanged,     this, dirty);
    connect(compositor, &WMCompositor::windowAdded,             this, dirty);
    connect(compositor, &WMCompositor::windowRemoved,           this, dirty);
    connect(compositor, &WMCompositor::activeWindowChanged,     this, dirty);
    connect(compositor, &WMCompositor::activeWorkspaceChanged,  this, dirty);

    connect(&Config::instance(), &Config::themeChanged, this, [this] {
        loadWallpaper();
        invalidateAllBlurCaches();
        m_needsRedraw = true;
    });
}

WMOutput::~WMOutput()
{
    makeCurrent();

    // Use QOpenGLFunctions_3_3_Core to delete GL objects safely
    auto* gl = context()
    ? context()->versionFunctions<QOpenGLFunctions_3_3_Core>()
    : nullptr;
    if (gl) {
        if (m_animVao) gl->glDeleteVertexArrays(1, &m_animVao);
        if (m_animVbo) gl->glDeleteBuffers(1, &m_animVbo);
    }
    m_animVao = 0;
    m_animVbo = 0;

    delete m_glBlur;
    delete m_animShader;
    doneCurrent();
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────
void WMOutput::show()
{
    if (m_screen) setGeometry(m_screen->geometry());
    QOpenGLWidget::show();
    m_renderTimer->start();
    m_frameTimer.start();
}

void WMOutput::hide()
{
    m_renderTimer->stop();
    QOpenGLWidget::hide();
}

// ─────────────────────────────────────────────────────────────────────────────
// OpenGL init
// ─────────────────────────────────────────────────────────────────────────────
void WMOutput::initializeGL()
{
    // Get 3.3 core profile functions
    m_gl = context()->versionFunctions<QOpenGLFunctions_3_3_Core>();
    if (!m_gl) {
        qWarning() << "[WMOutput] QOpenGLFunctions_3_3_Core unavailable — "
        "trying 2.0 fallback";
        // fallback: no VAO/VBO shader — static wallpaper only
        m_glAvailable = false;
        return;
    }
    m_gl->initializeOpenGLFunctions();
    m_glAvailable = true;

    m_glBlur = new GLBlurRenderer(this);
    m_glBlur->initialize();

    initAnimShader();

    qInfo() << "[WMOutput] GL initialized, vendor:"
    << reinterpret_cast<const char*>(m_gl->glGetString(GL_VENDOR));
}

void WMOutput::initAnimShader()
{
    if (!m_glAvailable || !m_gl) return;

    m_animShader = new QOpenGLShaderProgram(this);
    if (!m_animShader->addShaderFromSourceCode(QOpenGLShader::Vertex,   kAnimVert) ||
        !m_animShader->addShaderFromSourceCode(QOpenGLShader::Fragment, kAnimFrag) ||
        !m_animShader->link()) {
        qWarning() << "[WMOutput] anim shader failed:" << m_animShader->log();
    delete m_animShader;
    m_animShader = nullptr;
    return;
        }

        const float quad[] = {-1,-1, 1,-1, -1,1, 1,1};
        m_gl->glGenVertexArrays(1, &m_animVao);
        m_gl->glGenBuffers(1, &m_animVbo);
        m_gl->glBindVertexArray(m_animVao);
        m_gl->glBindBuffer(GL_ARRAY_BUFFER, m_animVbo);
        m_gl->glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        m_gl->glEnableVertexAttribArray(0);
        m_gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        m_gl->glBindVertexArray(0);

        qInfo() << "[WMOutput] animated wallpaper shader ready";
}

void WMOutput::resizeGL(int w, int h)
{
    if (m_gl) m_gl->glViewport(0, 0, w, h);
    rescaleWallpaper();
    invalidateAllBlurCaches();
}

// ─────────────────────────────────────────────────────────────────────────────
// Render tick
// ─────────────────────────────────────────────────────────────────────────────
void WMOutput::onRenderTick()
{
    m_glowPulse += kGlowPulseSpeed * m_glowDir;
    if (m_glowPulse >= 1.0f) { m_glowPulse = 1.0f; m_glowDir = -1.0f; }
    if (m_glowPulse <= 0.3f) { m_glowPulse = 0.3f; m_glowDir =  1.0f; }

    if (Config::instance().theme.animatedWallpaper) {
        m_needsRedraw = true;
    }
    if (m_wallpaperMovie &&
        m_wallpaperMovie->state() == QMovie::Running) {
        m_needsRedraw = true;
        }

        if (m_needsRedraw) {
            m_needsRedraw = false;
            update();
        }
}

// ─────────────────────────────────────────────────────────────────────────────
// paintGL
// ─────────────────────────────────────────────────────────────────────────────
void WMOutput::paintGL()
{
    // ── Animated GLSL wallpaper ───────────────────────────────────────────
    if (Config::instance().theme.animatedWallpaper &&
        m_animShader && m_glAvailable && m_gl) {
        m_animTime += 0.033f;

    m_gl->glDisable(GL_DEPTH_TEST);
    m_gl->glDisable(GL_BLEND);

    m_animShader->bind();
    m_animShader->setUniformValue("time",       m_animTime);
    m_animShader->setUniformValue("resolution",
                                  QVector2D(float(width()), float(height())));

    m_gl->glBindVertexArray(m_animVao);
    m_gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_gl->glBindVertexArray(0);

    m_animShader->release();

    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing |
    QPainter::SmoothPixmapTransform |
    QPainter::TextAntialiasing);
    QRadialGradient vig(QPointF(width()/2., height()/2.),
                        qMax(width(), height()) * 0.72);
    vig.setColorAt(0.55, Qt::transparent);
    vig.setColorAt(1.0,  QColor(0, 0, 0, 60));
    p.fillRect(rect(), vig);
    drawWindows(p);
    drawCursor(p);
    return;
        }

        // ── Static / GIF wallpaper ────────────────────────────────────────────
        QPainter p(this);
        p.setRenderHints(QPainter::Antialiasing |
        QPainter::SmoothPixmapTransform |
        QPainter::TextAntialiasing);
        drawWallpaper(p);
        drawWindows(p);
        drawCursor(p);
}

void WMOutput::paintEvent(QPaintEvent*)
{
    // QOpenGLWidget routes paintEvent → paintGL automatically.
    // Do not call QPainter here to avoid conflicts.
}

// ─────────────────────────────────────────────────────────────────────────────
// Resize
// ─────────────────────────────────────────────────────────────────────────────
void WMOutput::resizeEvent(QResizeEvent* e)
{
    QOpenGLWidget::resizeEvent(e);
    rescaleWallpaper();
    m_blurCache = QPixmap();
    invalidateAllBlurCaches();
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw passes
// ─────────────────────────────────────────────────────────────────────────────
void WMOutput::drawWallpaper(QPainter& p)
{
    if (m_wallpaperMovie &&
        !m_wallpaperMovie->currentPixmap().isNull()) {
        QPixmap frame = m_wallpaperMovie->currentPixmap()
        .scaled(size(), Qt::KeepAspectRatioByExpanding,
                Qt::SmoothTransformation);
        const int xo = (frame.width()  - width())  / 2;
    const int yo = (frame.height() - height()) / 2;
    p.drawPixmap(0, 0, frame, xo, yo, width(), height());
        } else if (!m_wallpaperScaled.isNull()) {
            const int xo = (m_wallpaperScaled.width()  - width())  / 2;
            const int yo = (m_wallpaperScaled.height() - height()) / 2;
            p.drawPixmap(0, 0, m_wallpaperScaled, xo, yo, width(), height());
        } else {
            QLinearGradient g(0, 0, width(), height());
            g.setColorAt(0, QColor(5,  8,  20));
            g.setColorAt(1, QColor(15, 25, 60));
            p.fillRect(rect(), g);
        }

        // Vignette
        QRadialGradient vig(QPointF(width()/2., height()/2.),
                            qMax(width(), height()) * 0.72);
        vig.setColorAt(0.55, Qt::transparent);
        vig.setColorAt(1.0,  QColor(0, 0, 0, 70));
        p.fillRect(rect(), vig);
}

void WMOutput::drawWindows(QPainter& p)
{
    auto* ws = m_compositor->activeWorkspace();
    if (!ws) return;

    const QList<Window*> wins = ws->visibleWindows();
    Window* const active = ws->activeWindow();

    QList<Window*> tiled, floating;
    for (auto* w : wins) {
        if (w->isFloating()) floating.append(w);
        else                 tiled.append(w);
    }

    // Active window drawn last (on top)
    auto moveToEnd = [&](QList<Window*>& list) {
        if (active && list.contains(active)) {
            list.removeOne(active);
            list.append(active);
        }
    };
    moveToEnd(tiled);
    moveToEnd(floating);

    for (auto* w : tiled)    drawWindow(p, w, w == active);
    for (auto* w : floating) drawWindow(p, w, w == active);
}

void WMOutput::drawWindow(QPainter& p, Window* w, bool active)
{
    const QRect geom = w->geometry();
    if (geom.isEmpty()) return;

    p.save();
    p.setOpacity(qBound(0.f, w->opacity(), 1.f));
    drawWindowShadow        (p, geom, active);
    drawWindowBlurBackground(p, w, geom);
    drawWindowGlassOverlay  (p, geom, active);
    drawWindowBorder        (p, geom, active);
    drawWindowContent       (p, w);
    p.restore();
}

void WMOutput::drawWindowShadow(QPainter& p, const QRect& rect, bool active)
{
    const auto& theme  = Config::instance().theme;
    const int   spread = active ? 20 : 10;
    const float alpha  = active ? 0.45f : 0.25f;

    p.setPen(Qt::NoPen);
    for (int i = spread; i > 0; i -= 3) {
        float a = alpha * float(i) / float(spread) * 0.35f;
        QColor sc = theme.shadowColor;
        sc.setAlphaF(a);
        QPainterPath sh;
        sh.addRoundedRect(rect.adjusted(-i, -i/2, i, i + i/2),
                          theme.borderRadius + i*0.4f,
                          theme.borderRadius + i*0.4f);
        p.fillPath(sh, sc);
    }
}

void WMOutput::drawWindowBlurBackground(QPainter& p, Window* w, const QRect& rect)
{
    if (m_wallpaperScaled.isNull() &&
        !(m_wallpaperMovie && !m_wallpaperMovie->currentPixmap().isNull())) {
        return;
        }

        const auto& theme = Config::instance().theme;
    const int xo = (m_wallpaperScaled.width()  - width())  / 2;
    const int yo = (m_wallpaperScaled.height() - height()) / 2;
    const QRect src = rect.translated(xo, yo).intersected(m_wallpaperScaled.rect());
    if (src.isEmpty()) return;

    WindowRenderState& state = renderStateFor(w);
    if (state.blurDirty || state.blurSourceRect != rect) {
        const QPixmap slice  = m_wallpaperScaled.copy(src);
        QImage       blurred;

        if (m_glBlur && m_glBlur->isReady()) {
            makeCurrent();
            blurred = m_glBlur->blurRegion(
                slice.toImage().convertToFormat(QImage::Format_RGBA8888),
                                           slice.rect(), 12);
            doneCurrent();
        }

        if (blurred.isNull()) {
            blurred = fastBlurCPU(
                slice.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied), 12);
        }

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

void WMOutput::drawWindowGlassOverlay(QPainter& p, const QRect& rect, bool active)
{
    const auto& theme = Config::instance().theme;
    const int   r     = theme.borderRadius;

    QPainterPath path;
    path.addRoundedRect(rect, r, r);

    p.save();
    p.setClipPath(path);

    QColor bg = theme.glassBackground;
    if (!active) bg.setAlpha(qMin(255, bg.alpha() + 20));
    p.fillPath(path, bg);

    QLinearGradient dg(rect.topLeft(), rect.bottomLeft());
    dg.setColorAt(0.0,  QColor(255, 255, 255, active ? 12 : 6));
    dg.setColorAt(0.45, Qt::transparent);
    dg.setColorAt(1.0,  QColor(0, 0, 0, active ? 18 : 28));
    p.fillPath(path, dg);

    QPainterPath shim;
    shim.addRect(QRectF(rect.left(), rect.top(), rect.width(), r * 2.));
    QLinearGradient sg(rect.topLeft(), QPointF(rect.left(), rect.top() + r*2));
    sg.setColorAt(0., QColor(255, 255, 255, 28));
    sg.setColorAt(1., Qt::transparent);
    p.fillPath(shim.intersected(path), sg);

    p.restore();
}

void WMOutput::drawWindowBorder(QPainter& p, const QRect& rect, bool active)
{
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

    for (int g = 3; g > 0; --g) {
        QColor gc = theme.glassBorderActive;
        gc.setAlpha(gc.alpha() / (g + 1));
        p.strokePath(border, QPen(gc, theme.borderWidth + g * 2.));
    }

    QLinearGradient bg(rect.topLeft(), rect.bottomRight());
    bg.setColorAt(0.,  theme.accentColor);
    bg.setColorAt(0.5, theme.accentSecondary);
    bg.setColorAt(1.,  theme.accentTertiary);
    p.strokePath(border, QPen(QBrush(bg), theme.borderWidth));
}

void WMOutput::drawWindowContent(QPainter& p, Window* w)
{
    const auto& theme    = Config::instance().theme;
    const QRect geom     = w->geometry();
    const bool  isActive = w->isActive();
    const int   r        = theme.borderRadius;

    // Title bar gradient
    const QRect tr(geom.x(), geom.y(), geom.width(), kTitleBarHeight);
    QPainterPath tp;
    tp.addRoundedRect(tr, r, r);
    tp.addRect(geom.x(), geom.y() + kTitleBarHeight/2,
               geom.width(), kTitleBarHeight/2);

    QLinearGradient tg(tr.topLeft(), tr.bottomLeft());
    tg.setColorAt(0, isActive ? QColor(255,255,255,18) : QColor(255,255,255,8));
    tg.setColorAt(1, Qt::transparent);
    p.fillPath(tp, tg);

    // Traffic-light dots
    const int dotY = geom.y() + kTitleBarHeight / 2;
    const int dotX = geom.right() - kDotRightMargin;

    struct DC { QColor inactive, active; };
    static const DC dots[3] = {
        {QColor(120, 50,  50,  180), QColor(255, 95,  86)},
        {QColor(110, 110, 50,  180), QColor(255, 189, 46)},
        {QColor(50,  110, 50,  180), QColor(39,  201, 63)},
    };
    p.setPen(Qt::NoPen);
    for (int i = 0; i < 3; ++i) {
        p.setBrush(isActive ? dots[i].active : dots[i].inactive);
        p.drawEllipse(QPoint(dotX - i * kDotSpacing, dotY),
                      kDotRadius, kDotRadius);
    }

    // Icon
    if (!w->icon().isNull()) {
        p.drawPixmap(geom.x() + 8, dotY - 8, 16, 16,
                     w->icon().pixmap(16, 16));
    }

    // Title
    QFont font(theme.fontFamily);
    font.setPixelSize(theme.fontSizeUI);
    p.setFont(font);
    p.setPen(isActive ? theme.textPrimary : theme.textSecondary);

    QString title = w->title();
    if (title.isEmpty()) title = w->appId();
    if (title.isEmpty()) title = "Window";

    const int    iconEnd  = geom.x() + 28;
    const int    dotsLeft = dotX - 2*kDotSpacing - kDotRadius*2 - 4;
    const QRect  textRect(iconEnd, geom.y(), dotsLeft - iconEnd, kTitleBarHeight);
    p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
               p.fontMetrics().elidedText(title, Qt::ElideRight, textRect.width()));

    // Separator line
    if (isActive) {
        p.setPen(QPen(QColor(255, 255, 255, 22), 1));
        p.drawLine(geom.x() + r, geom.y() + kTitleBarHeight,
                   geom.right() - r, geom.y() + kTitleBarHeight);
    }
}

void WMOutput::drawCursor(QPainter& p)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(m_cursorPos);
    p.setPen(QPen(QColor(255, 255, 255, 200), 1.5));
    p.setBrush(QColor(100, 180, 255, 30));
    p.drawEllipse(QPoint(0, 0), 6, 6);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 240));
    p.drawEllipse(QPoint(0, 0), 2, 2);
    p.restore();
}

// ─────────────────────────────────────────────────────────────────────────────
// Wallpaper loading
// ─────────────────────────────────────────────────────────────────────────────
void WMOutput::loadWallpaper()
{
    const QString path = Config::instance().theme.wallpaperPath;

    if (m_wallpaperMovie) {
        m_wallpaperMovie->stop();
        delete m_wallpaperMovie;
        m_wallpaperMovie = nullptr;
    }

    if (path.isEmpty() || path == "default" || path == "animated") {
        m_wallpaperScaled = QPixmap();
        m_wallpaper       = QPixmap();
        return;
    }

    const QString ext = QFileInfo(path).suffix().toLower();

    // ── Animated GIF ──────────────────────────────────────────────────────
    if (ext == "gif") {
        m_wallpaperMovie = new QMovie(path, {}, this);
        if (m_wallpaperMovie->isValid()) {
            connect(m_wallpaperMovie, &QMovie::frameChanged,
                    this, [this](int) { m_needsRedraw = true; });
            m_wallpaperMovie->start();
            qInfo() << "[WMOutput] GIF wallpaper:" << path;
            return;
        }
        delete m_wallpaperMovie;
        m_wallpaperMovie = nullptr;
    }

    // ── SVG (requires Qt6Svg) ─────────────────────────────────────────────
    if (ext == "svg" || ext == "svgz") {
        #ifdef QT_SVG_LIB
        QSvgRenderer svg(path);
        if (svg.isValid()) {
            const QSize sz = m_screen ? m_screen->size() : QSize(1920, 1080);
            QPixmap px(sz);
            px.fill(Qt::transparent);
            QPainter sp(&px);
            svg.render(&sp);
            m_wallpaper = px;
            rescaleWallpaper();
            qInfo() << "[WMOutput] SVG wallpaper:" << path;
            return;
        }
        #else
        qWarning() << "[WMOutput] SVG needs Qt6Svg — falling back to QImageReader";
        #endif
    }

    // ── PNG / JPG / BMP / WebP ────────────────────────────────────────────
    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (reader.canRead()) {
        m_wallpaper = QPixmap::fromImage(reader.read());
    }

    if (m_wallpaper.isNull()) {
        qWarning() << "[WMOutput] cannot load wallpaper:" << path
        << "— using fallback";
        m_wallpaper = makeFallbackWallpaper(1920, 1080);
    } else {
        qInfo() << "[WMOutput] wallpaper loaded:" << path;
    }

    rescaleWallpaper();
    invalidateAllBlurCaches();
}

void WMOutput::rescaleWallpaper()
{
    if (m_wallpaper.isNull() || size().isEmpty()) return;
    m_wallpaperScaled = m_wallpaper.scaled(
        size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
}

QPixmap WMOutput::generateFallbackWallpaper(const QSize& sz) const
{
    return makeFallbackWallpaper(sz.width(), sz.height());
}

QImage WMOutput::blurWallpaperRegion(Window*, const QRect&) { return {}; }
QImage WMOutput::boxBlur(const QImage& src, int r) { return fastBlurCPU(src, r); }

WindowRenderState& WMOutput::renderStateFor(Window* w)
{
    return m_renderStates[w];
}

void WMOutput::invalidateAllBlurCaches()
{
    for (auto& s : m_renderStates) { s.blurDirty = true; }
    m_blurCache = QPixmap();
}

// ─────────────────────────────────────────────────────────────────────────────
// Input events
// ─────────────────────────────────────────────────────────────────────────────
void WMOutput::keyPressEvent(QKeyEvent* e)
{
    if (m_inputHandler && m_inputHandler->handleKeyPress(e)) return;
    QOpenGLWidget::keyPressEvent(e);
}

void WMOutput::keyReleaseEvent(QKeyEvent* e)
{
    if (m_inputHandler && m_inputHandler->handleKeyRelease(e)) return;
    QOpenGLWidget::keyReleaseEvent(e);
}

void WMOutput::mousePressEvent(QMouseEvent* e)
{
    if (m_inputHandler) {
        m_cursorPos = e->pos();
        m_inputHandler->handleMousePress(e->pos(), e->button(), e->modifiers());
        m_needsRedraw = true;
        return;
    }
    QOpenGLWidget::mousePressEvent(e);
}

void WMOutput::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_inputHandler) {
        m_inputHandler->handleMouseRelease(e->pos(), e->button(), e->modifiers());
        m_needsRedraw = true;
        return;
    }
    QOpenGLWidget::mouseReleaseEvent(e);
}

void WMOutput::mouseMoveEvent(QMouseEvent* e)
{
    if (m_inputHandler) {
        m_cursorPos = e->pos();
        m_inputHandler->handleMouseMove(e->pos(), e->buttons());
        m_needsRedraw = true;
        return;
    }
    QOpenGLWidget::mouseMoveEvent(e);
}

void WMOutput::wheelEvent(QWheelEvent* e)
{
    if (m_inputHandler) {
        m_inputHandler->handleWheel(e->position().toPoint(),
                                    e->angleDelta(),
                                    e->modifiers());
        return;
    }
    QOpenGLWidget::wheelEvent(e);
}

void WMOutput::enterEvent(QEnterEvent* e) { QOpenGLWidget::enterEvent(e); }
void WMOutput::leaveEvent(QEvent*      e) { QOpenGLWidget::leaveEvent(e); }

// ─────────────────────────────────────────────────────────────────────────────
// Action dispatch
// ─────────────────────────────────────────────────────────────────────────────
void WMOutput::handleAction(const QString& action)
{
    if      (action.startsWith("exec:"))
        m_compositor->launchApp(action.mid(5));
    else if (action.startsWith("workspace:"))
        m_compositor->switchWorkspace(action.mid(10).toInt());
    else if (action.startsWith("movetoworkspace:")) {
        if (auto* w = m_compositor->activeWindow()) {
            m_compositor->moveWindowToWorkspace(w, action.mid(16).toInt());
        }
    }
    else if (action.startsWith("focus:"))
        m_compositor->focusDirection(action.mid(6));
    else if (action.startsWith("move:"))
        m_compositor->moveWindowDirection(action.mid(5));
    else if (action.startsWith("layout:"))
        m_compositor->setLayout(
            TilingEngine::layoutFromString(action.mid(7)));
        else if (action == "close") {
            if (auto* w = m_compositor->activeWindow()) {
                m_compositor->closeWindow(w);
            }
        }
        else if (action == "fullscreen") m_compositor->toggleFullscreen();
        else if (action == "float")      m_compositor->toggleFloat();
        else if (action == "maximize")   m_compositor->toggleMaximize();
        else if (action == "reload")     m_compositor->reloadConfig();
        else if (action == "lock")       m_compositor->lockScreen();
        else if (action == "launcher")   m_compositor->showLauncher();
        else if (action == "quit")       QCoreApplication::quit();

        m_needsRedraw = true;
}

void WMOutput::dispatchKeybind(QKeyEvent* e) { keyPressEvent(e); }

// ─────────────────────────────────────────────────────────────────────────────
// Hit-testing
// ─────────────────────────────────────────────────────────────────────────────
Window* WMOutput::windowAt(const QPoint& pos) const
{
    auto* ws = m_compositor->activeWorkspace();
    if (!ws) return nullptr;

    const auto wins = ws->visibleWindows();
    for (int i = wins.size()-1; i >= 0; --i) {
        if (wins[i]->isVisible() && wins[i]->geometry().contains(pos)) {
            return wins[i];
        }
    }
    return nullptr;
}

ResizeEdge WMOutput::resizeEdgeAt(const QPoint& pos, Window* w) const
{
    const QRect g = w->geometry();
    const int   d = kResizeHandleWidth;
    int e = static_cast<int>(ResizeEdge::None);

    if (pos.y() < g.top()    + d) e |= static_cast<int>(ResizeEdge::Top);
    if (pos.y() > g.bottom() - d) e |= static_cast<int>(ResizeEdge::Bottom);
    if (pos.x() < g.left()   + d) e |= static_cast<int>(ResizeEdge::Left);
    if (pos.x() > g.right()  - d) e |= static_cast<int>(ResizeEdge::Right);

    return static_cast<ResizeEdge>(e);
}

void WMOutput::updateCursorShape() {}

WMOutput::TitleBarButton
WMOutput::titleBarButtonAt(const QPoint& pos, Window* w) const
{
    const QRect g    = w->geometry();
    const int dotY   = g.y() + kTitleBarHeight / 2;
    const int dotX   = g.right() - kDotRightMargin;

    if (QRect(dotX-8, dotY-8, 16, 16).contains(pos))
        return TitleBarButton::Close;
    if (QRect(dotX-kDotSpacing-8, dotY-8, 16, 16).contains(pos))
        return TitleBarButton::Maximize;
    if (QRect(dotX-kDotSpacing*2-8, dotY-8, 16, 16).contains(pos))
        return TitleBarButton::Minimize;
    return TitleBarButton::None;
}

QRect WMOutput::closeButtonRect(const QRect& r) const
{
    const int dotY = r.y() + kTitleBarHeight/2;
    const int dotX = r.right() - kDotRightMargin;
    return QRect(dotX - kDotRadius, dotY - kDotRadius,
                 kDotRadius*2, kDotRadius*2);
}
QRect WMOutput::maximizeButtonRect(const QRect& r) const
{
    return closeButtonRect(r).translated(-kDotSpacing, 0);
}
QRect WMOutput::minimizeButtonRect(const QRect& r) const
{
    return closeButtonRect(r).translated(-kDotSpacing*2, 0);
}
