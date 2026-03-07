#include "WorkspaceSwitcher.h"
#include "compositor/WMCompositor.h"
#include "core/Workspace.h"
#include "core/Window.h"
#include "core/Config.h"
#include "core/TilingEngine.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QFontDatabase>
#include <QPropertyAnimation>
#include <QTimer>
#include <QtMath>
#include <QDebug>
#include <algorithm>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

WorkspaceSwitcher::WorkspaceSwitcher(WMCompositor* compositor, QWidget* parent)
: GlassWidget(parent)
, m_compositor(compositor)
{
    Q_ASSERT(compositor);

    // Window flags: frameless, always on top, doesn't steal focus permanently.
    setWindowFlags(Qt::Window
    | Qt::FramelessWindowHint
    | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating, false); // needs keyboard focus

    // Cover the full primary screen.
    if (QScreen* s = QGuiApplication::primaryScreen()) {
        setGeometry(s->geometry());
    }

    // ── Show/hide animation ────────────────────────────────────────────────
    m_showAnim = new QPropertyAnimation(this, "showProgress", this);
    m_showAnim->setEasingCurve(QEasingCurve::OutCubic);

    // ── Animation tick timer ───────────────────────────────────────────────
    m_animTimer = new QTimer(this);
    m_animTimer->setInterval(16); // ~60 fps
    connect(m_animTimer, &QTimer::timeout, this, &WorkspaceSwitcher::onAnimTick);

    // ── Compositor connections ─────────────────────────────────────────────
    connect(compositor, &WMCompositor::activeWorkspaceChanged,
            this,       &WorkspaceSwitcher::onWorkspaceChanged);
    connect(compositor, &WMCompositor::windowAdded,
            this,       &WorkspaceSwitcher::onWindowAdded);
    connect(compositor, &WMCompositor::windowRemoved,
            this,       &WorkspaceSwitcher::onWindowRemoved);

    hide();
    qDebug() << "[WorkspaceSwitcher] created";
}

WorkspaceSwitcher::~WorkspaceSwitcher() = default;

// ─────────────────────────────────────────────────────────────────────────────
// Show / hide
// ─────────────────────────────────────────────────────────────────────────────

void WorkspaceSwitcher::open(int startId) {
    if (m_open && !m_closing) return;

    m_closing     = false;
    m_open        = true;
    m_originalId  = m_compositor->activeWorkspace()
    ? m_compositor->activeWorkspace()->id() : 1;
    m_selectedId  = (startId > 0) ? startId : m_originalId;

    rebuildThumbs();
    computeThumbRects();

    // Cover the full primary screen in case it changed.
    if (QScreen* s = QGuiApplication::primaryScreen()) {
        setGeometry(s->geometry());
    }

    show();
    raise();
    activateWindow();
    setFocus();

    // Animate in.
    m_showAnim->stop();
    m_showAnim->setDuration(kShowMs);
    m_showAnim->setStartValue(0.f);
    m_showAnim->setEndValue(1.f);
    m_showAnim->start();

    m_animTimer->start();

    qDebug() << "[WorkspaceSwitcher] opened, selected ws:" << m_selectedId;
}

void WorkspaceSwitcher::cancel() {
    if (!m_open) return;
    m_closing = true;

    m_showAnim->stop();
    m_showAnim->setDuration(kHideMs);
    m_showAnim->setStartValue(m_showProgress);
    m_showAnim->setEndValue(0.f);

    connect(m_showAnim, &QPropertyAnimation::finished, this, [this]() {
        hide();
        m_open    = false;
        m_closing = false;
        m_animTimer->stop();
        disconnect(m_showAnim, &QPropertyAnimation::finished, nullptr, nullptr);
    }, Qt::SingleShotConnection);

    m_showAnim->start();

    qDebug() << "[WorkspaceSwitcher] cancelled";
}

void WorkspaceSwitcher::confirm() {
    if (!m_open) return;
    m_closing = true;

    const int target = m_selectedId;

    // Pop animation on selected thumb.
    if (WsThumb* t = thumbForId(target)) {
        t->selectScale = kSelectPop;
    }

    m_showAnim->stop();
    m_showAnim->setDuration(kHideMs);
    m_showAnim->setStartValue(m_showProgress);
    m_showAnim->setEndValue(0.f);

    connect(m_showAnim, &QPropertyAnimation::finished, this, [this, target]() {
        hide();
        m_open    = false;
        m_closing = false;
        m_animTimer->stop();
        disconnect(m_showAnim, &QPropertyAnimation::finished, nullptr, nullptr);

        if (target != m_originalId) {
            qDebug() << "[WorkspaceSwitcher] switching to ws:" << target;
            emit workspaceSwitchRequested(target);
        }
    }, Qt::SingleShotConnection);

    m_showAnim->start();
}

// ─────────────────────────────────────────────────────────────────────────────
// Thumbnail management
// ─────────────────────────────────────────────────────────────────────────────

void WorkspaceSwitcher::rebuildThumbs() {
    const int count = m_compositor->workspaceCount();
    m_thumbs.clear();
    m_thumbs.reserve(count);

    for (int i = 1; i <= count; ++i) {
        WsThumb t;
        t.id           = i;
        t.name         = QString::number(i);
        t.active       = (i == m_compositor->activeWorkspace()->id());
        t.enterProgress = 0.f;
        t.hoverProgress = 0.f;
        t.selectScale   = 1.f;
        t.previewDirty  = true;

        Workspace* ws = m_compositor->workspace(i);
        if (ws) {
            t.name     = ws->name();
            t.occupied = !ws->visibleWindows().isEmpty();
            rebuildThumb(t, ws);
        }

        m_thumbs.append(t);
    }
}

void WorkspaceSwitcher::rebuildThumb(WsThumb& t, Workspace* ws) {
    Q_ASSERT(ws);
    t.tiles.clear();
    t.occupied = false;

    const QList<Window*> wins = ws->visibleWindows();
    if (wins.isEmpty()) {
        t.previewDirty = false;
        return;
    }

    t.occupied = true;

    // Scale factor from real screen coords to thumb-internal coords.
    QScreen* scr = QGuiApplication::primaryScreen();
    if (!scr) return;
    const QRect screen = scr->geometry();

    const int innerW = kThumbW - kThumbPad * 2;
    const int innerH = kThumbH - kThumbPad * 2;
    const float sx   = (float)innerW / screen.width();
    const float sy   = (float)innerH / screen.height();

    for (Window* w : wins) {
        const QRect& g = w->geometry();

        WsThumb::MiniTile tile;
        tile.rect = QRect(
            kThumbPad + (int)((g.x() - screen.x()) * sx),
                          kThumbPad + (int)((g.y() - screen.y()) * sy),
                          qMax(4, (int)(g.width()  * sx)),
                          qMax(4, (int)(g.height() * sy)));
        tile.title    = w->title();
        tile.appId    = w->appId();
        tile.active   = w->isActive();
        tile.floating = w->isFloating();
        t.tiles.append(tile);
    }

    t.previewDirty = false;
}

void WorkspaceSwitcher::markThumbsDirty() {
    for (auto& t : m_thumbs) t.previewDirty = true;
}

WsThumb* WorkspaceSwitcher::thumbForId(int id) {
    for (auto& t : m_thumbs) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

WsThumb* WorkspaceSwitcher::thumbAt(const QPoint& pos) {
    for (auto& t : m_thumbs) {
        if (t.rect.contains(pos)) return &t;
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Navigation
// ─────────────────────────────────────────────────────────────────────────────

void WorkspaceSwitcher::selectNext() {
    const int count = m_thumbs.size();
    if (count == 0) return;

    int idx = 0;
    for (int i = 0; i < count; ++i) {
        if (m_thumbs[i].id == m_selectedId) { idx = i; break; }
    }
    selectId(m_thumbs[(idx + 1) % count].id);
}

void WorkspaceSwitcher::selectPrev() {
    const int count = m_thumbs.size();
    if (count == 0) return;

    int idx = 0;
    for (int i = 0; i < count; ++i) {
        if (m_thumbs[i].id == m_selectedId) { idx = i; break; }
    }
    selectId(m_thumbs[(idx - 1 + count) % count].id);
}

void WorkspaceSwitcher::selectId(int id) {
    if (m_selectedId == id) return;
    m_selectedId = id;
    update();
}

// ─────────────────────────────────────────────────────────────────────────────
// Geometry helpers
// ─────────────────────────────────────────────────────────────────────────────

void WorkspaceSwitcher::computeThumbRects() {
    if (m_thumbs.isEmpty()) return;

    const int n      = m_thumbs.size();
    const int totalW = n * kThumbW + (n - 1) * kThumbSpacing;
    const int startX = (width()  - totalW) / 2;
    const int centreY = (height() - kThumbH - kLabelH - kHintH) / 2;

    for (int i = 0; i < n; ++i) {
        m_thumbs[i].rect = QRect(
            startX + i * (kThumbW + kThumbSpacing),
                                 centreY,
                                 kThumbW,
                                 kThumbH);
    }
}

QRect WorkspaceSwitcher::thumbAreaRect() const {
    if (m_thumbs.isEmpty()) return {};
    return m_thumbs.first().rect
    .united(m_thumbs.last().rect)
    .adjusted(-kThumbSpacing, -kThumbSpacing,
              kThumbSpacing,  kThumbSpacing);
}

QRect WorkspaceSwitcher::titleAreaRect() const {
    QRect ta = thumbAreaRect();
    return QRect(ta.left(), ta.bottom() + 4, ta.width(), kLabelH);
}

// ─────────────────────────────────────────────────────────────────────────────
// Animation tick
// ─────────────────────────────────────────────────────────────────────────────

void WorkspaceSwitcher::onAnimTick() {
    bool needsUpdate = false;

    for (auto& t : m_thumbs) {
        // Enter animation.
        if (t.enterProgress < 1.f) {
            t.enterProgress = qMin(1.f, t.enterProgress + kEnterSpeed);
            needsUpdate = true;
        }

        // Hover fade.
        const bool hovered = (t.id == m_hoveredId);
        if (hovered && t.hoverProgress < 1.f) {
            t.hoverProgress = qMin(1.f, t.hoverProgress + kHoverSpeed);
            needsUpdate = true;
        } else if (!hovered && t.hoverProgress > 0.f) {
            t.hoverProgress = qMax(0.f, t.hoverProgress - kHoverSpeed);
            needsUpdate = true;
        }

        // Select-pop settle back to 1.
        if (t.selectScale > 1.f) {
            t.selectScale = qMax(1.f, t.selectScale - 0.008f);
            needsUpdate = true;
        }
    }

    if (needsUpdate) update();
}

// ─────────────────────────────────────────────────────────────────────────────
// Compositor event reactions
// ─────────────────────────────────────────────────────────────────────────────

void WorkspaceSwitcher::onWorkspaceChanged(int id) {
    for (auto& t : m_thumbs) {
        t.active = (t.id == id);
    }
    if (m_open) update();
}

void WorkspaceSwitcher::onWindowAdded(Window* w) {
    Q_UNUSED(w);
    markThumbsDirty();
}

void WorkspaceSwitcher::onWindowRemoved(Window* w) {
    Q_UNUSED(w);
    markThumbsDirty();
}

void WorkspaceSwitcher::onWindowTitleChanged() {
    markThumbsDirty();
}

// ─────────────────────────────────────────────────────────────────────────────
// Paint
// ─────────────────────────────────────────────────────────────────────────────

void WorkspaceSwitcher::paintEvent(QPaintEvent*) {
    if (m_showProgress <= 0.001f) return;

    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    p.setOpacity(m_showProgress);

    // Rebuild dirty thumbnails before painting.
    for (auto& t : m_thumbs) {
        if (t.previewDirty) {
            Workspace* ws = m_compositor->workspace(t.id);
            if (ws) rebuildThumb(t, ws);
        }
    }

    drawBackground(p);

    for (const auto& t : m_thumbs) {
        drawThumb(p, t);
    }

    drawSelectedTitle(p);
    drawHint(p);
}

// ── drawBackground ────────────────────────────────────────────────────────────

void WorkspaceSwitcher::drawBackground(QPainter& p) {
    const auto& theme = Config::instance().theme;

    // Full-screen dim.
    p.fillRect(rect(), QColor(0, 0, 0, 160));

    // Central frosted-glass card behind the thumb row.
    const QRect area = thumbAreaRect().adjusted(-24, -20, 24, kLabelH + kHintH + 24);
    if (!area.isValid()) return;

    QPainterPath path;
    path.addRoundedRect(area, 20, 20);

    // Glass fill.
    p.fillPath(path, theme.glassBackground);

    // Glass border.
    p.setPen(QPen(theme.glassBorder, 1));
    p.drawPath(path);

    // Inner highlight shimmer at top edge.
    QLinearGradient shimmer(area.topLeft(), area.bottomLeft());
    shimmer.setColorAt(0.0, QColor(255, 255, 255, 18));
    shimmer.setColorAt(0.12, QColor(255, 255, 255, 0));
    p.fillPath(path, shimmer);
}

// ── drawThumb ─────────────────────────────────────────────────────────────────

void WorkspaceSwitcher::drawThumb(QPainter& p, const WsThumb& t) {
    if (t.rect.isEmpty()) return;

    const float ep = easeOutBack(qMin(t.enterProgress, 1.f));
    if (ep <= 0.f) return;

    p.save();

    // Apply enter scale (grows from 0.85 → 1.0) and select pop.
    const float scale = (0.85f + 0.15f * ep) * t.selectScale;
    const QPointF centre = t.rect.center();
    p.translate(centre);
    p.scale(scale, scale);
    p.translate(-centre);
    p.setOpacity(ep * m_showProgress);

    drawThumbFrame(p, t);
    drawThumbTiles(p, t);
    drawThumbLabel(p, t);
    drawThumbDot(p, t);

    p.restore();
}

// ── drawThumbFrame ────────────────────────────────────────────────────────────

void WorkspaceSwitcher::drawThumbFrame(QPainter& p, const WsThumb& t) {
    const auto& theme = Config::instance().theme;
    const bool  sel   = (t.id == m_selectedId);

    QPainterPath path;
    path.addRoundedRect(t.rect, kThumbRadius, kThumbRadius);

    // Background: darker for inactive, slightly lighter for selected.
    QColor bg = sel
    ? theme.glassBackground.lighter(130)
    : theme.glassBackground;
    bg.setAlpha(sel ? 230 : 180);
    p.fillPath(path, bg);

    // Border: accent for selected with hover-brightness boost, muted otherwise.
    QColor border;
    if (sel) {
        border = theme.glassBorderActive;
        // Boost towards accent on hover.
        border = QColor(
            (int)(border.red()   + t.hoverProgress * (theme.accentColor.red()   - border.red())),
                        (int)(border.green() + t.hoverProgress * (theme.accentColor.green() - border.green())),
                        (int)(border.blue()  + t.hoverProgress * (theme.accentColor.blue()  - border.blue())),
                        qMin(255, (int)(border.alpha() + t.hoverProgress * 60)));
    } else {
        border = QColor(
            theme.glassBorder.red(),
                        theme.glassBorder.green(),
                        theme.glassBorder.blue(),
                        (int)(theme.glassBorder.alpha() + t.hoverProgress * 60));
    }

    const float borderW = sel ? 2.f : 1.f;
    p.setPen(QPen(border, borderW));
    p.drawPath(path);

    // Selected: accent glow halo outside the frame.
    if (sel) {
        QColor glow = theme.accentColor;
        glow.setAlpha(40 + (int)(t.hoverProgress * 30));
        p.setPen(QPen(glow, 4));
        QPainterPath halo;
        halo.addRoundedRect(t.rect.adjusted(-2, -2, 2, 2),
                            kThumbRadius + 2, kThumbRadius + 2);
        p.drawPath(halo);
    }
}

// ── drawThumbTiles ────────────────────────────────────────────────────────────

void WorkspaceSwitcher::drawThumbTiles(QPainter& p, const WsThumb& t) {
    if (t.tiles.isEmpty()) {
        // Empty workspace — draw a subtle grid hint.
        const auto& theme = Config::instance().theme;
        p.setPen(QPen(QColor(theme.textMuted.red(),
                             theme.textMuted.green(),
                             theme.textMuted.blue(), 40), 1,
                      Qt::DotLine));

        const QRect inner = t.rect.adjusted(kThumbPad, kThumbPad,
                                            -kThumbPad, -kThumbPad);
        // Two horizontal lines.
        const int third = inner.height() / 3;
        p.drawLine(inner.left(), inner.top() + third,
                   inner.right(), inner.top() + third);
        p.drawLine(inner.left(), inner.top() + 2 * third,
                   inner.right(), inner.top() + 2 * third);
        return;
    }

    const auto& theme = Config::instance().theme;

    for (const auto& tile : t.tiles) {
        // Translate mini tile rect from thumb-local to widget-local.
        const QRect r = tile.rect.translated(t.rect.topLeft());

        QPainterPath path;
        path.addRoundedRect(r, 2, 2);

        // Window fill.
        QColor fill = tile.active
        ? QColor(theme.accentColor.red(),
                 theme.accentColor.green(),
                 theme.accentColor.blue(), 120)
        : QColor(255, 255, 255, 30);
        p.fillPath(path, fill);

        // Window border.
        QColor border = tile.active
        ? QColor(theme.accentColor.red(),
                 theme.accentColor.green(),
                 theme.accentColor.blue(), 200)
        : QColor(255, 255, 255, 60);
        p.setPen(QPen(border, 0.5f));
        p.drawPath(path);

        // Title micro-text if there is enough room.
        if (r.width() > 20 && r.height() > 10) {
            QFont f = uiFont(7);
            p.setFont(f);
            p.setPen(QColor(255, 255, 255, 160));
            p.drawText(r.adjusted(2, 1, -2, -1),
                       Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                       tile.title.isEmpty() ? tile.appId : tile.title);
        }
    }
}

// ── drawThumbLabel ────────────────────────────────────────────────────────────

void WorkspaceSwitcher::drawThumbLabel(QPainter& p, const WsThumb& t) {
    const auto& theme = Config::instance().theme;
    const bool  sel   = (t.id == m_selectedId);

    // Number label below the thumb.
    const QRect labelRect(t.rect.left(),
                          t.rect.bottom() + 4,
                          t.rect.width(), 20);

    QFont f = uiFont(sel ? 13 : 11);
    f.setBold(sel);
    p.setFont(f);

    QColor col = sel ? theme.textPrimary : theme.textSecondary;
    col.setAlpha(sel ? 255 : 160);
    p.setPen(col);
    p.drawText(labelRect, Qt::AlignHCenter | Qt::AlignVCenter, t.name);
}

// ── drawThumbDot ──────────────────────────────────────────────────────────────

void WorkspaceSwitcher::drawThumbDot(QPainter& p, const WsThumb& t) {
    if (!t.occupied) return;

    const auto& theme = Config::instance().theme;
    const bool  sel   = (t.id == m_selectedId);

    // Small dot at bottom-centre of the thumb to indicate occupancy.
    const QPoint dotPos(t.rect.center().x(),
                        t.rect.bottom() - kThumbPad / 2);

    p.setPen(Qt::NoPen);
    p.setBrush(sel ? theme.accentColor : theme.textMuted);
    p.drawEllipse(dotPos, kDotR, kDotR);
}

// ── drawSelectedTitle ─────────────────────────────────────────────────────────

void WorkspaceSwitcher::drawSelectedTitle(QPainter& p) {
    if (m_thumbs.isEmpty()) return;

    const auto& theme = Config::instance().theme;
    const QRect ta    = titleAreaRect();

    // Show the active window's title in the selected workspace.
    QString text;
    Workspace* ws = m_compositor->workspace(m_selectedId);
    if (ws) {
        Window* aw = ws->activeWindow();
        if (aw && !aw->title().isEmpty()) {
            text = aw->title();
            if (!aw->appId().isEmpty()) {
                text = aw->appId() + "  —  " + text;
            }
        } else if (!ws->visibleWindows().isEmpty()) {
            text = QString("%1 window%2")
            .arg(ws->visibleWindows().size())
            .arg(ws->visibleWindows().size() > 1 ? "s" : "");
        } else {
            text = "Empty workspace";
        }
    }

    QFont f = uiFont(13);
    p.setFont(f);
    p.setPen(theme.textSecondary);
    p.drawText(ta, Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextSingleLine,
               text);
}

// ── drawHint ──────────────────────────────────────────────────────────────────

void WorkspaceSwitcher::drawHint(QPainter& p) {
    const auto& theme = Config::instance().theme;
    const QRect ta    = thumbAreaRect();

    const QRect hint(ta.left(),
                     ta.bottom() + kLabelH + 6,
                     ta.width(), kHintH);

    QFont f = uiFont(11);
    p.setFont(f);
    p.setPen(QColor(theme.textMuted.red(),
                    theme.textMuted.green(),
                    theme.textMuted.blue(), 120));

    p.drawText(hint, Qt::AlignHCenter | Qt::AlignVCenter,
               "← → navigate    Enter confirm    Esc cancel");
}

// ─────────────────────────────────────────────────────────────────────────────
// Input events
// ─────────────────────────────────────────────────────────────────────────────

void WorkspaceSwitcher::keyPressEvent(QKeyEvent* e) {
    if (!m_open || m_closing) return;

    switch (e->key()) {
        case Qt::Key_Escape:
            cancel();
            return;

        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Space:
            confirm();
            return;

        case Qt::Key_Left:
        case Qt::Key_H:
            selectPrev();
            return;

        case Qt::Key_Right:
        case Qt::Key_L:
            selectNext();
            return;

        case Qt::Key_Tab:
            if (e->modifiers() & Qt::ShiftModifier)
                selectPrev();
        else
            selectNext();
        return;

        default:
            // Number keys 1–9 jump directly.
            if (e->key() >= Qt::Key_1 && e->key() <= Qt::Key_9) {
                const int target = e->key() - Qt::Key_0;
                if (target <= m_compositor->workspaceCount()) {
                    selectId(target);
                    confirm();
                }
                return;
            }
            break;
    }

    e->ignore();
}

void WorkspaceSwitcher::mousePressEvent(QMouseEvent* e) {
    if (!m_open) return;

    WsThumb* t = thumbAt(e->pos());
    if (!t) {
        // Click outside any thumb — cancel.
        cancel();
        return;
    }

    if (e->button() == Qt::LeftButton) {
        selectId(t->id);
        confirm();
    }
}

void WorkspaceSwitcher::mouseMoveEvent(QMouseEvent* e) {
    if (!m_open) return;

    WsThumb* t = thumbAt(e->pos());
    const int newHov = t ? t->id : -1;

    if (newHov != m_hoveredId) {
        m_hoveredId = newHov;
        if (t) selectId(t->id);
        update();
    }
}

void WorkspaceSwitcher::leaveEvent(QEvent*) {
    m_hoveredId = -1;
    update();
}

void WorkspaceSwitcher::wheelEvent(QWheelEvent* e) {
    if (!m_open) return;

    if (e->angleDelta().y() > 0)
        selectPrev();
    else
        selectNext();
}

// ─────────────────────────────────────────────────────────────────────────────
// Easing functions
// ─────────────────────────────────────────────────────────────────────────────

float WorkspaceSwitcher::easeOutBack(float t) {
    constexpr float c1 = 1.70158f;
    constexpr float c3 = c1 + 1.f;
    const float t1 = t - 1.f;
    return 1.f + c3 * t1 * t1 * t1 + c1 * t1 * t1;
}

float WorkspaceSwitcher::easeInCubic(float t) {
    return t * t * t;
}

float WorkspaceSwitcher::easeOutCubic(float t) {
    const float t1 = t - 1.f;
    return 1.f + t1 * t1 * t1;
}

float WorkspaceSwitcher::spring(float t) {
    if (t >= 1.f) return 1.f;
    constexpr float zeta  = 0.55f;
    constexpr float omega = 9.42f; // 2*pi*1.5
    return 1.f - std::exp(-zeta * omega * t)
    * std::cos(omega * std::sqrt(1.f - zeta * zeta) * t);
}

// ─────────────────────────────────────────────────────────────────────────────
// Font helpers
// ─────────────────────────────────────────────────────────────────────────────

QFont WorkspaceSwitcher::uiFont(int size) const {
    const auto& theme = Config::instance().theme;
    QFont f(theme.fontFamily);
    f.setPointSize(size > 0 ? size : theme.fontSizeUI);
    f.setStyleHint(QFont::SansSerif);
    return f;
}

QFont WorkspaceSwitcher::monoFont(int size) const {
    QFont f("JetBrains Mono, Fira Code, Consolas, monospace");
    f.setPointSize(size > 0 ? size : 11);
    f.setStyleHint(QFont::Monospace);
    return f;
}
