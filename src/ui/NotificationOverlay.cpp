#include "NotificationOverlay.h"
#include "core/Config.h"

#include <QPainter>
#include <QPainterPath>
#include <QGuiApplication>
#include <QScreen>
#include <QMouseEvent>
#include <QTimer>
#include <QLinearGradient>
#include <QFont>
#include <QFontMetrics>
#include <QDebug>
#include <QtMath>

// Static initialiser — type must match header: uint32_t
uint32_t NotificationOverlay::s_nextId = 1;

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

NotificationOverlay::NotificationOverlay(QScreen* screen, QWidget* parent)
: GlassWidget(parent)                          // constructor matches header
, m_screen(screen)
{
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint |
    Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);

    if (screen) {
        const QRect sg = screen->geometry();
        setGeometry(sg.right() - kCardWidth - kRightMargin,
                    sg.top()   + m_topMargin,
                    kCardWidth, 600);
    }

    m_animTimer = new QTimer(this);
    connect(m_animTimer, &QTimer::timeout, this, &NotificationOverlay::onAnimTick);
    m_animTimer->start(33);

    m_elapsed.start();
    hide();
}

NotificationOverlay::~NotificationOverlay() {}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

uint32_t NotificationOverlay::showNotification(
    const QString&                   summary,
    const QString&                   body,
    const QString&                   iconName,
    int                              timeoutMs,
    NotificationUrgency              urgency,
    const QString&                   appName,
    const QList<NotificationAction>& actions,
    uint32_t                         replaceId)
{
    // Replace in-place if replaceId matches an existing notification
    if (replaceId != 0) {
        for (auto& n : m_notifications) {
            if (n.id == replaceId) {
                n.summary  = summary;
                n.body     = body;
                n.iconName = iconName;
                n.appName  = appName;
                n.actions  = actions;
                n.timeoutMs    = timeoutForUrgency(urgency, timeoutMs);
                n.urgency      = urgency;
                n.accentColor  = accentForUrgency(urgency);
                n.createdAt    = m_elapsed.elapsed();
                n.dismissing   = false;
                n.exitProgress = 0.f;
                n.iconPixmap   = loadIcon(iconName);
                layoutCards();
                update();
                return replaceId;
            }
        }
    }

    // Cap simultaneous cards
    if (m_notifications.size() >= kMaxCards) {
        // Dismiss the oldest (last in list = bottom of stack)
        beginDismiss(m_notifications.last().id, 1);
    }

    Notification n = createNotification(summary, body, iconName, timeoutMs,
                                        urgency, appName, actions);
    m_notifications.prepend(n);   // newest on top

    layoutCards();
    resizeToFit();
    show();
    update();
    return n.id;
}

void NotificationOverlay::closeNotification(uint32_t id) {
    beginDismiss(id, 3);
}

void NotificationOverlay::clearAll() {
    m_notifications.clear();
    hide();
    update();
}

void NotificationOverlay::setScreen(QScreen* screen) {
    m_screen = screen;
    if (screen) {
        const QRect sg = screen->geometry();
        move(sg.right() - kCardWidth - kRightMargin,
             sg.top()   + m_topMargin);
    }
}

void NotificationOverlay::setTopMargin(int px) {
    m_topMargin = px;
    if (m_screen) {
        const QRect sg = m_screen->geometry();
        move(x(), sg.top() + m_topMargin);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Animation tick
// ─────────────────────────────────────────────────────────────────────────────

void NotificationOverlay::onAnimTick() {    // matches header slot name
    bool changed = false;

    for (auto& n : m_notifications) {
        if (n.dismissing) {
            n.exitProgress = qMin(n.exitProgress + kExitSpeed, 1.0f);
            changed = true;
        } else {
            if (n.enterProgress < 1.0f) {
                n.enterProgress = qMin(n.enterProgress + kEnterSpeed, 1.0f);
                changed = true;
            }
            // Timeout countdown (skip if hovered)
            if (!n.hovered && n.timeoutMs > 0) {
                const qint64 age = m_elapsed.elapsed() - n.createdAt;
                if (age >= n.timeoutMs) {
                    beginDismiss(n.id, 1);
                    changed = true;
                }
            }
        }
    }

    pruneFinished();

    if (m_notifications.isEmpty()) hide();
    else if (changed) { layoutCards(); update(); }
}

// ─────────────────────────────────────────────────────────────────────────────
// Notification lifecycle helpers
// ─────────────────────────────────────────────────────────────────────────────

Notification NotificationOverlay::createNotification(
    const QString&                   summary,
    const QString&                   body,
    const QString&                   iconName,
    int                              timeoutMs,
    NotificationUrgency              urgency,
    const QString&                   appName,
    const QList<NotificationAction>& actions) const
    {
        Notification n;
        n.id          = s_nextId++;
        n.summary     = summary;
        n.body        = body;
        n.iconName    = iconName;
        n.appName     = appName;
        n.actions     = actions;
        n.urgency     = urgency;
        n.timeoutMs   = timeoutForUrgency(urgency, timeoutMs);
        n.accentColor = accentForUrgency(urgency);
        n.createdAt   = m_elapsed.elapsed();
        n.iconPixmap  = loadIcon(iconName);
        return n;
    }

    void NotificationOverlay::beginDismiss(uint32_t id, uint32_t reason) {
        for (auto& n : m_notifications) {
            if (n.id == id && !n.dismissing) {
                n.dismissing = true;
                emit notificationClosed(id, reason);
                return;
            }
        }
    }

    void NotificationOverlay::pruneFinished() {
        m_notifications.removeIf([](const Notification& n) {
            return n.dismissing && n.exitProgress >= 1.0f;
        });
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Layout
    // ─────────────────────────────────────────────────────────────────────────────

    int NotificationOverlay::cardHeight(const Notification& n) const {
        int h = kCardPadding * 2 + kIconSize;
        if (!n.body.isEmpty()) h += 20;
        if (!n.actions.isEmpty())
            h += kActionButtonH + kCardPadding;
        return qMax(h, 72);
    }

    void NotificationOverlay::layoutCards() {
        int y = kCardSpacing;
        for (auto& n : m_notifications) {
            const int h   = cardHeight(n);
            const float t = easeOutBack(n.enterProgress);
            const int  xOff = (int)((1.0f - t) * (kCardWidth + 20));

            n.cardRect = QRect(xOff, y, kCardWidth, h);

            // Sub-rects
            n.iconRect    = QRect(n.cardRect.x() + kCardPadding,
                                  n.cardRect.y() + kCardPadding,
                                  kIconSize, kIconSize);
            const int textX = n.iconRect.right() + kCardPadding;
            const int textW = n.cardRect.right() - kCardPadding - textX;
            n.summaryRect = QRect(textX, n.cardRect.y() + kCardPadding,
                                  textW, 18);
            n.bodyRect    = QRect(textX, n.summaryRect.bottom() + 4,
                                  textW, n.cardRect.bottom()
                                  - n.summaryRect.bottom() - 4
                                  - kProgressBarH - kCardPadding);
            n.closeRect   = QRect(n.cardRect.right() - kCloseButtonSize - 6,
                                  n.cardRect.y() + 6,
                                  kCloseButtonSize, kCloseButtonSize);
            n.progressRect = QRect(n.cardRect.x() + kCardRadius,
                                   n.cardRect.bottom() - kProgressBarH - 4,
                                   n.cardRect.width() - 2 * kCardRadius,
                                   kProgressBarH);

            // Apply opacity via exitProgress (will be set in drawCard)
            y += h + kCardSpacing;
        }
    }

    void NotificationOverlay::resizeToFit() {
        if (m_notifications.isEmpty()) { hide(); return; }
        int totalH = kCardSpacing;
        for (const auto& n : m_notifications)
            totalH += cardHeight(n) + kCardSpacing;
        totalH = qMax(totalH, 10);
        if (m_screen) {
            const QRect sg = m_screen->geometry();
            setGeometry(sg.right() - kCardWidth - kRightMargin,
                        sg.top()   + m_topMargin,
                        kCardWidth, totalH);
        } else {
            resize(kCardWidth, totalH);
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Paint
    // ─────────────────────────────────────────────────────────────────────────────

    void NotificationOverlay::paintEvent(QPaintEvent*) {
        QPainter p(this);
        p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
        for (const auto& n : m_notifications)
            drawCard(p, n);
    }

    void NotificationOverlay::drawCard(QPainter& p, const Notification& n) {
        const float opacity = n.dismissing
        ? (1.0f - easeInCubic(n.exitProgress))
        : easeOutBack(n.enterProgress);

        p.save();
        p.setOpacity(qBound(0.0f, opacity, 1.0f));

        drawCardBackground(p, n);
        drawCardIcon(p, n);
        drawCardText(p, n);
        drawCloseButton(p, n);
        if (n.timeoutMs > 0 && !n.dismissing)
            drawProgressBar(p, n);
        if (!n.actions.isEmpty())
            drawActionButtons(p, n);

        p.restore();
    }

    void NotificationOverlay::drawCardBackground(QPainter& p, const Notification& n) {
        const auto& theme = Config::instance().theme;
        const QRect& r = n.cardRect;

        QPainterPath path;
        path.addRoundedRect(r, kCardRadius, kCardRadius);

        // Glass fill
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(12, 18, 35, 210));
        p.drawPath(path);

        // Left accent bar
        QColor ac = n.accentColor;
        QLinearGradient leftBar(r.x(), r.y(), r.x(), r.bottom());
        leftBar.setColorAt(0, ac);
        ac.setAlpha(60);
        leftBar.setColorAt(1, ac);
        p.setPen(Qt::NoPen);
        p.setBrush(leftBar);
        p.drawRect(r.x(), r.y() + 8, 3, r.height() - 16);

        // Border
        p.strokePath(path, QPen(QColor(255,255,255, n.hovered ? 50 : 25), 1));

        // Top shimmer
        QLinearGradient sh(r.topLeft(), QPointF(r.left(), r.top() + 18));
        sh.setColorAt(0, QColor(255,255,255, 22));
        sh.setColorAt(1, Qt::transparent);
        p.fillRect(r.x(), r.y(), r.width(), 18, sh);

        Q_UNUSED(theme);
    }

    void NotificationOverlay::drawCardIcon(QPainter& p, const Notification& n) {
        if (!n.iconPixmap.isNull()) {
            p.drawPixmap(n.iconRect, n.iconPixmap.scaled(
                n.iconRect.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        } else if (!n.iconName.isEmpty()) {
            // Fallback: draw first letter of app name
            const auto& theme = Config::instance().theme;
            p.setBrush(n.accentColor);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(n.iconRect, 6, 6);
            QFont f(theme.fontFamily);
            f.setPixelSize(16);
            f.setWeight(QFont::Bold);
            p.setFont(f);
            p.setPen(Qt::white);
            p.drawText(n.iconRect, Qt::AlignCenter,
                       n.appName.isEmpty() ? "?" : n.appName.left(1).toUpper());
        }
    }

    void NotificationOverlay::drawCardText(QPainter& p, const Notification& n) {
        const auto& theme = Config::instance().theme;

        // Summary
        QFont sf(theme.fontFamily);
        sf.setPixelSize(12);
        sf.setWeight(QFont::DemiBold);  // DemiBold — SemiBold removed in Qt6
        p.setFont(sf);
        p.setPen(theme.textPrimary);
        p.drawText(n.summaryRect, Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(sf).elidedText(n.summary, Qt::ElideRight,
                                               n.summaryRect.width()));

        // Body
        if (!n.body.isEmpty()) {
            QFont bf(theme.fontFamily);
            bf.setPixelSize(11);
            p.setFont(bf);
            p.setPen(theme.textSecondary);
            p.drawText(n.bodyRect,
                       Qt::AlignTop | Qt::TextWordWrap | Qt::AlignLeft,
                       n.body);
        }
    }

    void NotificationOverlay::drawCloseButton(QPainter& p, const Notification& n) {
        const QRect& r = n.closeRect;
        p.setPen(QPen(QColor(255,255,255, 60), 1.5f));
        p.setBrush(Qt::NoBrush);
        const int m = 4;
        p.drawLine(r.x() + m, r.y() + m, r.right() - m, r.bottom() - m);
        p.drawLine(r.right() - m, r.y() + m, r.x() + m, r.bottom() - m);
    }

    void NotificationOverlay::drawProgressBar(QPainter& p, const Notification& n) {
        const QRect& r = n.progressRect;
        // Background track
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255,255,255, 20));
        p.drawRoundedRect(r, 1, 1);

        // Fill — shrinks as time passes
        const qint64 age     = m_elapsed.elapsed() - n.createdAt;
        const float  ratio   = 1.0f - qBound(0.0f, float(age) / float(n.timeoutMs), 1.0f);
        const int    fillW   = (int)(r.width() * ratio);
        if (fillW > 0) {
            p.setBrush(n.accentColor);
            p.drawRoundedRect(r.x(), r.y(), fillW, r.height(), 1, 1);
        }
    }

    void NotificationOverlay::drawActionButtons(QPainter& p, const Notification& n) {
        const auto& theme = Config::instance().theme;
        for (const auto& act : n.actions) {
            p.setBrush(QColor(255,255,255, 15));
            p.setPen(QPen(QColor(255,255,255, 30), 1));
            p.drawRoundedRect(act.rect, 6, 6);

            QFont f(theme.fontFamily);
            f.setPixelSize(11);
            p.setFont(f);
            p.setPen(theme.textSecondary);
            p.drawText(act.rect, Qt::AlignCenter, act.label);
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Input events
    // ─────────────────────────────────────────────────────────────────────────────

    void NotificationOverlay::mousePressEvent(QMouseEvent* e) {
        m_pressedId = 0;
        for (auto& n : m_notifications) {              // m_notifications (not m_notifs)
            if (n.closeRect.contains(e->pos())) {
                beginDismiss(n.id, 2);
                return;
            }
            for (const auto& act : n.actions) {
                if (act.rect.contains(e->pos())) {
                    emit actionInvoked(n.id, act.key);
                    beginDismiss(n.id, 2);
                    return;
                }
            }
            if (n.cardRect.contains(e->pos())) {
                m_pressedId = n.id;
                return;
            }
        }
    }

    void NotificationOverlay::mouseReleaseEvent(QMouseEvent* e) {
        if (m_pressedId == 0) return;
        for (auto& n : m_notifications) {
            if (n.id == m_pressedId && n.cardRect.contains(e->pos())) {
                beginDismiss(n.id, 2);
                break;
            }
        }
        m_pressedId = 0;
    }

    void NotificationOverlay::mouseMoveEvent(QMouseEvent* e) {
        m_lastPos = e->pos();
        for (auto& n : m_notifications)
            n.hovered = n.cardRect.contains(e->pos());
        update();
    }

    void NotificationOverlay::leaveEvent(QEvent*) {
        for (auto& n : m_notifications)
            n.hovered = false;
        update();
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Helpers
    // ─────────────────────────────────────────────────────────────────────────────

    Notification* NotificationOverlay::notificationAt(const QPoint& pos) {
        for (auto& n : m_notifications) {
            if (n.cardRect.contains(pos)) return &n;
        }
        return nullptr;
    }

    const NotificationAction* NotificationOverlay::actionAt(
        const QPoint& pos, const Notification& n) const
        {
            for (const auto& act : n.actions) {
                if (act.rect.contains(pos)) return &act;
            }
            return nullptr;
        }

        QColor NotificationOverlay::accentForUrgency(NotificationUrgency u) const {
            const auto& theme = Config::instance().theme;
            switch (u) {
                case NotificationUrgency::Low:      return theme.textMuted;
                case NotificationUrgency::Critical: return QColor(0xff, 0x44, 0x55);
                default:                            return theme.accentColor;
            }
        }

        int NotificationOverlay::timeoutForUrgency(NotificationUrgency u, int requested) const {
            if (requested > 0) return requested;
            switch (u) {
                case NotificationUrgency::Low:      return kTimeoutLow;
                case NotificationUrgency::Critical: return kTimeoutCritical;
                default:                            return kTimeoutNormal;
            }
        }

        QPixmap NotificationOverlay::loadIcon(const QString& name) const {
            if (name.isEmpty()) return {};
            // Try QIcon from theme
            const QIcon icon = QIcon::fromTheme(name);
            if (!icon.isNull())
                return icon.pixmap(kIconSize, kIconSize);
            return {};
        }

        // ─────────────────────────────────────────────────────────────────────────────
        // Easing
        // ─────────────────────────────────────────────────────────────────────────────

        float NotificationOverlay::easeOutBack(float t) {
            const float c1 = 1.70158f;
            const float c3 = c1 + 1.0f;
            return 1.0f + c3 * qPow(t - 1.0f, 3) + c1 * qPow(t - 1.0f, 2);
        }

        float NotificationOverlay::easeInCubic(float t) {
            return t * t * t;
        }

        float NotificationOverlay::easeOutCubic(float t) {
            const float t2 = t - 1.0f;
            return 1.0f + t2 * t2 * t2;
        }
