#include "BarWidget.h"
#include "compositor/WMCompositor.h"
#include "core/Window.h"
#include "core/Workspace.h"
#include "core/Config.h"
#include "core/TilingEngine.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QEnterEvent>
#include <QDateTime>
#include <QLinearGradient>
#include <QGuiApplication>
#include <QScreen>
#include <QFile>
#include <QDebug>
#include <QtMath>

BarWidget::BarWidget(WMCompositor* compositor, QWidget* parent)
: QWidget(parent, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
, m_compositor(compositor)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);

    const int barH = Config::instance().theme.barHeight;
    setFixedHeight(barH);
    if (auto* screen = QGuiApplication::primaryScreen()) {
        const QRect sg = screen->geometry();
        setGeometry(sg.x(), sg.y(), sg.width(), barH);
    }

    // Clock (1 s)
    m_clockTimer = new QTimer(this);
    connect(m_clockTimer, &QTimer::timeout, this, &BarWidget::updateClock);
    m_clockTimer->start(1000);
    updateClock();

    // Animation tick (~30 fps)
    m_animTimer = new QTimer(this);
    connect(m_animTimer, &QTimer::timeout, this, &BarWidget::onAnimTick);  // onAnimTick
    m_animTimer->start(33);

    // System stats (2 s)
    m_statsTimer = new QTimer(this);                                        // m_statsTimer
    connect(m_statsTimer, &QTimer::timeout, this, &BarWidget::updateSysStats);
    m_statsTimer->start(2000);
    updateSysStats();

    // Compositor connections
    if (compositor) {
        connect(compositor, &WMCompositor::activeWorkspaceChanged,
                this, &BarWidget::onWorkspaceChanged);

        // onActiveWindowChanged(Window*) — pass w directly
        connect(compositor, &WMCompositor::activeWindowChanged,
                this, &BarWidget::onActiveWindowChanged);

        connect(compositor, &WMCompositor::windowAdded,
                this, &BarWidget::onWindowAdded);
        connect(compositor, &WMCompositor::windowRemoved,
                this, &BarWidget::onWindowRemoved);
        connect(compositor, &WMCompositor::tiledWindowsChanged,
                this, [this] { rebuildWorkspaceIndicators(); update(); });
    }

    rebuildWorkspaceIndicators();
}

BarWidget::~BarWidget() {}

// ─────────────────────────────────────────────────────────────────────────────
// Timer slots
// ─────────────────────────────────────────────────────────────────────────────

void BarWidget::updateClock() {
    const QDateTime now = QDateTime::currentDateTime();
    m_timeStr = now.toString("HH:mm");
    m_dateStr = now.toString("ddd, dd MMM");
    update();
}

void BarWidget::updateSysStats() {            // header name
    m_cpuUsage = readCpuUsage();              // readCpuUsage
    m_ramUsage = readRamUsage();
    m_ramStr   = readRamString();             // m_ramStr
    update();
}

void BarWidget::onAnimTick() {                // header name
    // Glow pulse
    m_glowPulse += m_glowDir * kGlowPulseSpeed;
    if (m_glowPulse >= 1.0f) { m_glowPulse = 1.0f; m_glowDir = -1.0f; }
    if (m_glowPulse <= 0.0f) { m_glowPulse = 0.0f; m_glowDir = +1.0f; }

    // Hover per workspace pill
    for (auto& ws : m_wsIndicators) {         // m_wsIndicators
        const bool hov = (ws.id == m_hoveredWsId); // m_hoveredWsId
        ws.hoverProgress = hov
        ? qMin(ws.hoverProgress + kHoverAnimSpeed, 1.0f)
        : qMax(ws.hoverProgress - kHoverAnimSpeed, 0.0f);
    }

    update();
}

// ─────────────────────────────────────────────────────────────────────────────
// Compositor slots
// ─────────────────────────────────────────────────────────────────────────────

void BarWidget::onWorkspaceChanged(int) {
    rebuildWorkspaceIndicators();
    update();
}

void BarWidget::onWindowAdded(Window*) {
    rebuildWorkspaceIndicators();
    update();
}

void BarWidget::onWindowRemoved(Window*) {
    rebuildWorkspaceIndicators();
    update();
}

void BarWidget::onActiveWindowChanged(Window* w) {  // Window* parameter
    if (w) {
        m_activeTitle = w->title();
        m_activeAppId = w->appId();
    } else {
        m_activeTitle.clear();
        m_activeAppId.clear();
    }
    update();
}

void BarWidget::onLayoutChanged(TilingLayout layout) {
    m_currentLayout = layout;
    update();
}

void BarWidget::onConfigReloaded() {
    rebuildWorkspaceIndicators();
    update();
}

// ─────────────────────────────────────────────────────────────────────────────
// Workspace indicators
// ─────────────────────────────────────────────────────────────────────────────

void BarWidget::rebuildWorkspaceIndicators() {
    m_wsIndicators.clear();                    // m_wsIndicators
    const int count  = Config::instance().workspaceCount();
    const int active = (m_compositor && m_compositor->activeWorkspace())
    ? m_compositor->activeWorkspace()->id() : 1;

    const int wsW  = 32;
    const int wsH  = kWsPillH;
    const int gap  = kWsPillSpacing;
    const int barH = Config::instance().theme.barHeight;
    const int y    = (barH - wsH) / 2;
    int       x    = kLogoW + kPaddingH;

    for (int i = 1; i <= count; ++i) {
        WorkspaceIndicator ws;
        ws.id     = i;
        ws.name   = QString::number(i);
        ws.active = (i == active);
        if (m_compositor) {
            auto* w = m_compositor->workspace(i);
            ws.occupied = w && !w->isEmpty();
        }
        ws.rect = QRect(x, y, wsW, wsH);
        m_wsIndicators.append(ws);
        x += wsW + gap;
    }
    m_wsIndicatorsDirty = false;
}

WorkspaceIndicator* BarWidget::indicatorAt(const QPoint& pos) {
    for (auto& ws : m_wsIndicators) {
        if (ws.rect.contains(pos)) return &ws;
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Paint
// ─────────────────────────────────────────────────────────────────────────────

void BarWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
    QPainter::SmoothPixmapTransform);

    drawBackground(p);
    drawLogo(p);
    drawWorkspaces(p);
    drawLayoutIndicator(p);
    drawActiveWindow(p);
    drawStatusSection(p);   // drawStatusSection
    drawClock(p);
}

void BarWidget::drawBackground(QPainter& p) {
    const auto& theme = Config::instance().theme;
    const QRect r = rect();

    p.setPen(Qt::NoPen);
    p.setBrush(theme.barBackground);
    p.drawRect(r);

    // Top highlight
    QLinearGradient top(r.left(), r.top(), r.right(), r.top());
    top.setColorAt(0.0, Qt::transparent);
    top.setColorAt(0.2, QColor(255,255,255, 50));
    top.setColorAt(0.5, QColor(255,255,255, 80));
    top.setColorAt(0.8, QColor(255,255,255, 50));
    top.setColorAt(1.0, Qt::transparent);
    p.setBrush(top);
    p.drawRect(r.left(), r.top(), r.width(), 1);

    // Accent glow
    const float pulse = 0.5f + m_glowPulse * 0.5f;
    QColor ac = theme.accentColor;
    ac.setAlpha((int)(25 * pulse));
    QLinearGradient ag(r.left(), 0, r.right(), 0);
    ag.setColorAt(0.0,  Qt::transparent);
    ag.setColorAt(0.35, ac);
    ag.setColorAt(0.65, ac);
    ag.setColorAt(1.0,  Qt::transparent);
    p.setBrush(ag);
    p.drawRect(r.left(), r.top(), r.width(), 2);

    // Bottom border
    QLinearGradient btm(r.left(), r.bottom(), r.right(), r.bottom());
    btm.setColorAt(0.0,  Qt::transparent);
    btm.setColorAt(0.15, theme.barBorder);
    btm.setColorAt(0.85, theme.barBorder);
    btm.setColorAt(1.0,  Qt::transparent);
    p.setBrush(btm);
    p.drawRect(r.left(), r.bottom() - 1, r.width(), 1);
}

void BarWidget::drawBorder(QPainter&) {} // handled in drawBackground

void BarWidget::drawLogo(QPainter& p) {
    const auto& theme = Config::instance().theme;
    const int size = 20;
    const int x    = 12;
    const int y    = (height() - size) / 2;

    QPainterPath hex;
    const float cx = x + size / 2.0f;
    const float cy = y + size / 2.0f;
    const float r  = size / 2.0f;
    for (int i = 0; i < 6; ++i) {
        const float a = float(M_PI) / 6.0f + i * float(M_PI) / 3.0f;
        const float px = cx + r * qCos(a);
        const float py = cy + r * qSin(a);
        if (i == 0) hex.moveTo(px, py); else hex.lineTo(px, py);
    }
    hex.closeSubpath();

    const float glow = 0.6f + m_glowPulse * 0.4f;
    QColor gc = theme.accentColor; gc.setAlpha((int)(60 * glow));
    p.setBrush(gc); p.setPen(Qt::NoPen);
    p.drawPath(hex);

    QLinearGradient hg(x, y, x + size, y + size);
    hg.setColorAt(0, theme.accentColor);
    hg.setColorAt(1, theme.accentSecondary);
    p.setPen(QPen(QBrush(hg), 1.5));
    p.setBrush(QColor(255,255,255, 8));
    p.drawPath(hex);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255,255,255, (int)(180 * glow)));
    p.drawEllipse(QPointF(cx, cy), 3, 3);
}

void BarWidget::drawWorkspaces(QPainter& p) {
    const auto& theme = Config::instance().theme;

    for (const auto& ws : m_wsIndicators) {   // m_wsIndicators
        const float hover = ws.hoverProgress; // stored per-indicator
        const float pulse = ws.active ? (0.8f + m_glowPulse * 0.2f) : 1.0f;
        const QRect r = ws.rect;

        if (ws.active) {
            for (int gi = 3; gi > 0; --gi) {
                QColor gc = theme.accentColor;
                gc.setAlpha((int)(20 * pulse * gi / 3));
                p.setBrush(gc); p.setPen(Qt::NoPen);
                p.drawRoundedRect(r.adjusted(-gi,-gi,gi,gi), 6+gi, 6+gi);
            }
            QLinearGradient bg(r.topLeft(), r.bottomLeft());
            QColor c1 = theme.accentColor;     c1.setAlpha(180);
            QColor c2 = theme.accentSecondary; c2.setAlpha(140);
            bg.setColorAt(0, c1); bg.setColorAt(1, c2);
            p.setBrush(bg); p.setPen(Qt::NoPen);
            p.drawRoundedRect(r, 6, 6);

            QLinearGradient sh(r.topLeft(),
                               QPointF(r.left(), r.top() + r.height() * 0.5));
            sh.setColorAt(0, QColor(255,255,255,60));
            sh.setColorAt(1, Qt::transparent);
            p.setBrush(sh); p.drawRoundedRect(r, 6, 6);

        } else if (hover > 0.01f) {
            QColor hc = theme.glassBackground;
            hc.setAlpha((int)(120 * hover));
            p.setBrush(hc);
            p.setPen(QPen(QColor(255,255,255, (int)(40*hover)), 1));
            p.drawRoundedRect(r, 6, 6);
        }

        QFont font = barFont(11);
        font.setWeight(ws.active ? QFont::Bold : QFont::Normal);
        p.setFont(font);
        QColor textColor = ws.active ? Qt::white
        : (ws.occupied ? theme.textSecondary : theme.textMuted);
        textColor.setAlphaF(textColor.alphaF() * (0.7f + hover * 0.3f));
        p.setPen(textColor);
        p.drawText(r, Qt::AlignCenter, ws.name);

        if (ws.occupied && !ws.active) {
            QColor dot = theme.accentColor; dot.setAlpha(160);
            p.setPen(Qt::NoPen); p.setBrush(dot);
            p.drawEllipse(QPoint(r.center().x(), r.bottom() - 3), 2, 2);
        }
    }
}

void BarWidget::drawActiveWindow(QPainter& p) {
    const auto& theme = Config::instance().theme;
    if (m_activeTitle.isEmpty() && m_activeAppId.isEmpty()) return;

    const QString display = m_activeTitle.isEmpty() ? m_activeAppId : m_activeTitle;
    QFont font = barFont(12);
    font.setWeight(QFont::Medium);
    p.setFont(font);
    QFontMetrics fm(font);
    const int     maxW = width() / 3;
    const QString el   = fm.elidedText(display, Qt::ElideMiddle, maxW);
    const int     x    = (width() - fm.horizontalAdvance(el)) / 2;
    const int     y    = height() / 2 + fm.ascent() / 2;

    p.setPen(QColor(0,0,0,80));
    p.drawText(x+1, y+1, el);
    p.setPen(theme.textPrimary);
    p.drawText(x, y, el);
}

void BarWidget::drawLayoutIndicator(QPainter& p) {
    const auto& theme = Config::instance().theme;
    if (!m_compositor || !m_compositor->activeWorkspace()) return;

    auto* ws = m_compositor->activeWorkspace();
    const QString icon = TilingEngine::layoutIcon(ws->layout());

    const int afterWs = m_wsIndicators.isEmpty()          // m_wsIndicators
    ? kLogoW + kPaddingH
    : m_wsIndicators.last().rect.right() + 12;

    p.setFont(barFont(13));
    p.setPen(theme.textMuted);
    p.drawText(QRect(afterWs, 0, kLayoutIndicatorW, height()),
               Qt::AlignCenter, icon);
}

void BarWidget::drawStatusSection(QPainter& p) {          // drawStatusSection
    const auto& theme = Config::instance().theme;
    const int right = width() - kClockW - kPaddingH;

    const int bW = 36, bH = 4;
    const int x  = right - bW - 32;
    const int y  = height() / 2 - 6;

    p.setFont(barFont(9));
    p.setPen(theme.textMuted);
    p.drawText(x - 24, y + bH, "CPU");

    p.setBrush(QColor(255,255,255,15)); p.setPen(Qt::NoPen);
    p.drawRoundedRect(x, y, bW, bH, 2, 2);

    const int fillW = (int)(bW * m_cpuUsage);
    if (fillW > 0) {
        QLinearGradient cg(x, y, x + bW, y);
        cg.setColorAt(0, theme.accentTertiary);
        cg.setColorAt(1, theme.accentColor);
        p.setBrush(cg);
        p.drawRoundedRect(x, y, fillW, bH, 2, 2);
    }

    p.setFont(barFont(9));
    p.setPen(theme.textMuted);
    p.drawText(x - 24, y + bH + 16, "RAM");
    p.setPen(theme.textSecondary);
    p.drawText(x, y + bH + 16, m_ramStr);                 // m_ramStr
}

void BarWidget::drawClock(QPainter& p) {
    const auto& theme = Config::instance().theme;
    const int right = width() - kPaddingH;

    QFont df = barFont(10);
    p.setFont(df);
    p.setPen(theme.textMuted);
    QFontMetrics dfm(df);
    p.drawText(right - dfm.horizontalAdvance(m_dateStr),
               height() / 2 + 8, m_dateStr);

    QFont tf = barFont(13);
    tf.setWeight(QFont::DemiBold);                         // DemiBold (SemiBold removed Qt6)
    p.setFont(tf);
    p.setPen(theme.textPrimary);
    QFontMetrics tfm(tf);
    p.drawText(right - tfm.horizontalAdvance(m_timeStr),
               height() / 2 - 2, m_timeStr);
}

void BarWidget::drawSeparator(QPainter& p, int x) {
    const auto& theme = Config::instance().theme;
    p.setPen(QPen(theme.glassBorder, 1));
    p.drawLine(x, kPaddingV, x, height() - kPaddingV);
}

// Sub-draws (stubs — content handled by drawStatusSection)
void BarWidget::drawCpuItem    (QPainter&, const QRect&) {}
void BarWidget::drawRamItem    (QPainter&, const QRect&) {}
void BarWidget::drawVolumeItem (QPainter&, const QRect&) {}
void BarWidget::drawBatteryItem(QPainter&, const QRect&) {}
void BarWidget::drawWorkspacePill(QPainter&, const WorkspaceIndicator&) {}

// ─────────────────────────────────────────────────────────────────────────────
// Section geometry
// ─────────────────────────────────────────────────────────────────────────────

QRect BarWidget::leftSectionRect()   const { return {0, 0, width()/3, height()}; }
QRect BarWidget::centreSectionRect() const { return {width()/3, 0, width()/3, height()}; }
QRect BarWidget::rightSectionRect()  const { return {2*width()/3, 0, width()/3, height()}; }

QRect BarWidget::layoutIndicatorRect() const {
    const int x = m_wsIndicators.isEmpty()
    ? kLogoW + kPaddingH
    : m_wsIndicators.last().rect.right() + 4;
    return {x, 0, kLayoutIndicatorW, height()};
}

QRect BarWidget::clockRect() const {
    return {width() - kClockW - kPaddingH, 0, kClockW, height()};
}

// ─────────────────────────────────────────────────────────────────────────────
// Input
// ─────────────────────────────────────────────────────────────────────────────

void BarWidget::mousePressEvent(QMouseEvent* e) {
    for (const auto& ws : m_wsIndicators) {               // m_wsIndicators
        if (ws.rect.contains(e->pos())) {
            if (e->button() == Qt::LeftButton)
                emit workspaceSwitchRequested(ws.id);
            else if (e->button() == Qt::MiddleButton)
                emit moveWindowToWorkspaceRequested(ws.id);
            return;
        }
    }
    if (layoutIndicatorRect().contains(e->pos())) {
        if (e->button() == Qt::LeftButton)  emit layoutCycleForwardRequested();
        if (e->button() == Qt::RightButton) emit layoutCycleBackwardRequested();
    }
}

void BarWidget::mouseReleaseEvent(QMouseEvent*) {}

void BarWidget::mouseMoveEvent(QMouseEvent* e) {
    m_lastMousePos = e->pos();
    m_hoveredWsId  = -1;                                   // m_hoveredWsId
    for (const auto& ws : m_wsIndicators) {                // m_wsIndicators
        if (ws.rect.contains(e->pos())) {
            m_hoveredWsId = ws.id;
            break;
        }
    }
    m_hoverLayout = layoutIndicatorRect().contains(e->pos());
    m_hoverClock  = clockRect().contains(e->pos());
}

void BarWidget::wheelEvent(QWheelEvent* e) {
    if (e->angleDelta().y() > 0) emit layoutCycleForwardRequested();
    else                          emit layoutCycleBackwardRequested();
}

void BarWidget::enterEvent(QEnterEvent*) {}

void BarWidget::leaveEvent(QEvent*) {
    m_hoveredWsId = -1;                                    // m_hoveredWsId
    m_hoverLayout = false;
    m_hoverClock  = false;
}

void BarWidget::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    m_sectionsDirty = true;
    m_bgCacheDirty  = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// System stats
// ─────────────────────────────────────────────────────────────────────────────

float BarWidget::readCpuUsage() {                          // readCpuUsage
    QFile f("/proc/stat");
    if (!f.open(QIODevice::ReadOnly)) return m_cpuUsage;

    const QString   line  = f.readLine();
    f.close();
    const QStringList pts = line.simplified().split(' ');
    if (pts.size() < 5) return m_cpuUsage;

    const qint64 user  = pts[1].toLongLong();
    const qint64 nice  = pts[2].toLongLong();
    const qint64 sys   = pts[3].toLongLong();
    const qint64 idle  = pts[4].toLongLong();
    const qint64 total = user + nice + sys + idle;

    const qint64 dTotal = total - m_prevCpuTotal;
    const qint64 dIdle  = idle  - m_prevCpuIdle;
    m_prevCpuTotal = total;
    m_prevCpuIdle  = idle;

    if (dTotal == 0) return m_cpuUsage;
    return float(dTotal - dIdle) / float(dTotal);
}

float BarWidget::readRamUsage() {                          // readRamUsage
    QFile f("/proc/meminfo");
    if (!f.open(QIODevice::ReadOnly)) return m_ramUsage;
    qint64 total = 0, avail = 0;
    while (!f.atEnd()) {
        const QString line = f.readLine().simplified();
        if (line.startsWith("MemTotal:"))
            total = line.split(' ')[1].toLongLong();
        else if (line.startsWith("MemAvailable:"))
            avail = line.split(' ')[1].toLongLong();
    }
    f.close();
    return total ? float(total - avail) / float(total) : m_ramUsage;
}

QString BarWidget::readRamString() {                       // readRamString
    QFile f("/proc/meminfo");
    if (!f.open(QIODevice::ReadOnly)) return m_ramStr;
    qint64 total = 0, avail = 0;
    while (!f.atEnd()) {
        const QString line = f.readLine().simplified();
        if (line.startsWith("MemTotal:"))
            total = line.split(' ')[1].toLongLong();
        else if (line.startsWith("MemAvailable:"))
            avail = line.split(' ')[1].toLongLong();
    }
    f.close();
    if (!total) return "?";
    return QString("%1 GB").arg(float(total - avail) / (1024.0f * 1024.0f), 0, 'f', 1);
}

float BarWidget::readVolume()  { return -1.f; }
float BarWidget::readBattery() { return -1.f; }

// ─────────────────────────────────────────────────────────────────────────────
// Background cache (stubs)
// ─────────────────────────────────────────────────────────────────────────────

void BarWidget::rebuildBackgroundCache() { m_bgCacheDirty = false; }
QImage BarWidget::boxBlur(const QImage& src, int) { return src; }

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void BarWidget::setScreenGeometry(const QRect& sg) {
    setGeometry(sg.x(), sg.y(), sg.width(), height());
}

int BarWidget::barHeight() const {
    return Config::instance().theme.barHeight;
}

void BarWidget::invalidateWorkspaces() {
    m_wsIndicatorsDirty = true;
    rebuildWorkspaceIndicators();
    update();
}

void BarWidget::invalidateBackground() {
    m_bgCacheDirty = true;
    update();
}

// ─────────────────────────────────────────────────────────────────────────────
// Font helpers
// ─────────────────────────────────────────────────────────────────────────────

QFont BarWidget::barFont(int size) const {
    const auto& theme = Config::instance().theme;
    QFont f(theme.fontFamily);
    f.setPixelSize(size > 0 ? size : theme.fontSizeUI);
    return f;
}

QFont BarWidget::monoFont(int size) const {
    QFont f("monospace");
    f.setStyleHint(QFont::Monospace);
    if (size > 0) f.setPixelSize(size);
    return f;
}

QFont BarWidget::iconFont(int size) const {
    QFont f("Symbols Nerd Font");
    if (size > 0) f.setPixelSize(size);
    return f;
}
