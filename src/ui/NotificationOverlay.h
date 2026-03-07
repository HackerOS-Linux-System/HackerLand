#pragma once

#include "GlassWidget.h"

#include <QObject>
#include <QList>
#include <QHash>
#include <QRect>
#include <QColor>
#include <QPixmap>
#include <QString>
#include <QTimer>
#include <QElapsedTimer>
#include <QPropertyAnimation>
#include <functional>

class QPainter;
class QMouseEvent;
class QScreen;

// ─────────────────────────────────────────────────────────────────────────────
// NotificationUrgency
//
// Maps to the freedesktop.org Desktop Notifications spec urgency levels.
// Affects accent colour, timeout duration, and whether the overlay pulses.
// ─────────────────────────────────────────────────────────────────────────────
enum class NotificationUrgency {
    Low,      ///< Informational — muted accent, longer timeout (8 s)
    Normal,   ///< Default — theme accent, standard timeout (5 s)
    Critical  ///< Alert — red accent, no auto-dismiss, border pulses
};

// ─────────────────────────────────────────────────────────────────────────────
// NotificationAction
//
// An optional button shown at the bottom of a notification card.
// Matches the freedesktop.org "actions" list in the Notify D-Bus call.
// ─────────────────────────────────────────────────────────────────────────────
struct NotificationAction {
    QString key;     ///< Action identifier sent back to the client
    QString label;   ///< Text rendered on the button
    QRect   rect;    ///< Hit-test rect; filled by layoutCard()
};

// ─────────────────────────────────────────────────────────────────────────────
// Notification
//
// All state for one notification card.  Owned by NotificationOverlay.
// ─────────────────────────────────────────────────────────────────────────────
struct Notification {
    // ── Identity ──────────────────────────────────────────────────────────
    uint32_t id          = 0;     ///< Unique ID; also used for D-Bus replaceId
    QString  appName;             ///< Sending application name
    QString  summary;             ///< Short title line (always shown)
    QString  body;                ///< Rich-text body (may be empty)
    QString  iconName;            ///< XDG icon name (e.g. "dialog-error")
    QPixmap  iconPixmap;          ///< Pre-loaded icon (loaded once on creation)

    // ── Category / urgency ────────────────────────────────────────────────
    NotificationUrgency urgency   = NotificationUrgency::Normal;
    QString  category;            ///< freedesktop category hint (e.g. "email")

    // ── Actions ───────────────────────────────────────────────────────────
    QList<NotificationAction> actions;

    // ── Timing ────────────────────────────────────────────────────────────
    int     timeoutMs    = 5000;  ///< Auto-dismiss after this many ms; -1=never
    qint64  createdAt    = 0;     ///< QElapsedTimer ms at creation

    // ── Theme ─────────────────────────────────────────────────────────────
    QColor  accentColor;          ///< Resolved from urgency + Config on creation

    // ── Animation state (driven by NotificationOverlay::onAnimTick) ───────
    float   enterProgress = 0.f;  ///< 0→1 slide-in from the right
    float   exitProgress  = 0.f;  ///< 0→1 fade/slide-out (starts when dismissed)
    bool    dismissing    = false;///< true = exit animation in progress
    bool    hovered       = false;///< Cursor is over this card

    // ── Layout (computed each frame by layoutCard()) ───────────────────────
    QRect   cardRect;             ///< Current card rect in overlay-local coords
    QRect   iconRect;             ///< Icon area inside card
    QRect   summaryRect;          ///< Summary text area
    QRect   bodyRect;             ///< Body text area
    QRect   closeRect;            ///< ✕ button hit-test rect
    QRect   progressRect;         ///< Timeout progress bar rect
};

// ─────────────────────────────────────────────────────────────────────────────
// NotificationOverlay
//
// A translucent, always-on-top overlay that stacks notification cards in the
// top-right corner of the primary screen (below the bar).
//
// Rendering
//   Each card is drawn with the glassmorphism style inherited from GlassWidget:
//   frosted background, rounded corners, gradient border.
//   Urgency-specific accent colours:
//     Low      →  muted blue-grey
//     Normal   →  theme accentColor  (electric blue)
//     Critical →  red (#ff4455), pulsing border
//
//   A slim progress bar at the bottom of each card depletes over timeoutMs,
//   giving the user a visual countdown.  Critical notifications have no bar
//   (they never auto-dismiss).
//
// Animation
//   • Enter: card slides in from the right edge over ~300 ms (spring easing).
//   • Exit:  card fades and slides out to the right over ~200 ms (ease-in).
//   • Stack: when a card is removed the cards below slide up smoothly.
//   • Hover: hovering a card pauses its timeout countdown.
//
// D-Bus integration (future)
//   This class exposes showNotification() which WMCompositor can call from a
//   freedesktop.org org.freedesktop.Notifications D-Bus service implementation.
//   The signals closeNotification() and actionInvoked() allow the service to
//   forward results back to the originating application.
//
// Usage
//   NotificationOverlay* notif = new NotificationOverlay(screen, compositor);
//   notif->show();
//   notif->showNotification("Firefox", "Download complete", "download", 5000,
//                            NotificationUrgency::Normal);
// ─────────────────────────────────────────────────────────────────────────────
class NotificationOverlay : public GlassWidget {
    Q_OBJECT

    Q_PROPERTY(float stackShift READ stackShift WRITE setStackShift)

public:
    // ── Construction ──────────────────────────────────────────────────────
    explicit NotificationOverlay(QScreen*  screen,
                                 QWidget*  parent = nullptr);
    ~NotificationOverlay() override;

    // ── Public API — show / manage notifications ──────────────────────────

    /// Show a new notification card.
    /// If \p replaceId is non-zero and matches an existing notification, that
    /// card is updated in place (replaces title / body / icon / timeout).
    /// Returns the assigned notification ID.
    uint32_t showNotification(const QString&           summary,
                              const QString&           body        = {},
                              const QString&           iconName    = {},
                              int                      timeoutMs   = 5000,
                              NotificationUrgency      urgency     = NotificationUrgency::Normal,
                              const QString&           appName     = {},
                              const QList<NotificationAction>& actions = {},
                              uint32_t                 replaceId   = 0);

    /// Dismiss a specific notification by ID, triggering its exit animation.
    void closeNotification(uint32_t id);

    /// Dismiss all visible notifications immediately (no animation).
    void clearAll();

    /// Number of cards currently on screen (including ones being dismissed).
    int count() const { return m_notifications.size(); }

    // ── Screen repositioning ──────────────────────────────────────────────

    /// Reposition the overlay on \p screen (call when the screen geometry or
    /// bar height changes).
    void setScreen(QScreen* screen);

    /// Y offset from the top of the screen (bar height + margin).
    void setTopMargin(int px);

    // ── Stack-shift property (driven by QPropertyAnimation on card removal) ─
    float stackShift()      const { return m_stackShift; }
    void  setStackShift(float v)  { m_stackShift = v; update(); }

signals:
    // ── D-Bus / protocol signals ──────────────────────────────────────────

    /// Emitted when a card is closed (by timeout, user dismiss, or closeNotification()).
    /// \p reason: 1=expired, 2=dismissed by user, 3=closeNotification(), 4=undefined
    void notificationClosed(uint32_t id, uint32_t reason);

    /// Emitted when the user clicks an action button on a card.
    void actionInvoked(uint32_t id, const QString& actionKey);

protected:
    // ── Qt event overrides ────────────────────────────────────────────────
    void paintEvent       (QPaintEvent*   event) override;
    void mousePressEvent  (QMouseEvent*   event) override;
    void mouseReleaseEvent(QMouseEvent*   event) override;
    void mouseMoveEvent   (QMouseEvent*   event) override;
    void leaveEvent       (QEvent*        event) override;

private slots:
    void onAnimTick();       ///< ~33 ms — drives all animations and timeouts

private:
    // ── Notification lifecycle ────────────────────────────────────────────

    /// Create a fresh Notification from parameters and prepend to m_notifications.
    Notification createNotification(const QString&      summary,
                                    const QString&      body,
                                    const QString&      iconName,
                                    int                 timeoutMs,
                                    NotificationUrgency urgency,
                                    const QString&      appName,
                                    const QList<NotificationAction>& actions) const;

                                    /// Begin the exit animation for the card with \p id.
                                    /// \p reason is forwarded to notificationClosed().
                                    void beginDismiss(uint32_t id, uint32_t reason);

                                    /// Remove fully-exited cards from m_notifications.
                                    void pruneFinished();

                                    // ── Layout ────────────────────────────────────────────────────────────

                                    /// Recompute cardRect, iconRect, summaryRect, bodyRect, closeRect and
                                    /// progressRect for every notification.  Called when the card list changes
                                    /// or the overlay is resized.
                                    void layoutCards();

                                    /// Compute the pixel height of one card given its content.
                                    int cardHeight(const Notification& n) const;

                                    /// Resize and reposition the overlay widget to tightly wrap all cards.
                                    void resizeToFit();

                                    // ── Paint helpers ─────────────────────────────────────────────────────

                                    /// Draw one notification card at its current cardRect.
                                    void drawCard(QPainter& p, const Notification& n);

                                    /// Draw the frosted glass background + border for one card.
                                    void drawCardBackground(QPainter& p, const Notification& n);

                                    /// Draw the icon (QPixmap or fallback glyph) in n.iconRect.
                                    void drawCardIcon(QPainter& p, const Notification& n);

                                    /// Draw summary (bold) and body text in their respective rects.
                                    void drawCardText(QPainter& p, const Notification& n);

                                    /// Draw the ✕ close button in n.closeRect.
                                    void drawCloseButton(QPainter& p, const Notification& n);

                                    /// Draw the timeout progress bar in n.progressRect.
                                    void drawProgressBar(QPainter& p, const Notification& n);

                                    /// Draw action buttons at the bottom of the card.
                                    void drawActionButtons(QPainter& p, const Notification& n);

                                    // ── Easing helpers ────────────────────────────────────────────────────
                                    static float easeOutBack  (float t);   ///< Slight overshoot — enter
                                    static float easeInCubic  (float t);   ///< Accelerate — exit
                                    static float easeOutCubic (float t);   ///< Decelerate — stack reflow

                                    // ── Icon loading ──────────────────────────────────────────────────────

                                    /// Load and scale an XDG icon to kIconSize × kIconSize.
                                    /// Returns a null pixmap if the icon cannot be found.
                                    QPixmap loadIcon(const QString& name) const;

                                    // ── Urgency helpers ───────────────────────────────────────────────────
                                    QColor accentForUrgency(NotificationUrgency u) const;
                                    int    timeoutForUrgency(NotificationUrgency u, int requested) const;

                                    // ── Utility ───────────────────────────────────────────────────────────

                                    /// Return the notification at overlay-local \p pos, or nullptr.
                                    Notification* notificationAt(const QPoint& pos);

                                    /// Return the action at overlay-local \p pos within \p n, or nullptr.
                                    const NotificationAction* actionAt(const QPoint& pos,
                                                                       const Notification& n) const;

                                                                       // ── Members ───────────────────────────────────────────────────────────

                                                                       QScreen*              m_screen     = nullptr;
                                                                       int                   m_topMargin  = 50;   ///< Y offset from screen top

                                                                       QList<Notification>   m_notifications;     ///< Front = newest (top of stack)
                                                                       QTimer*               m_animTimer  = nullptr;
                                                                       QElapsedTimer         m_elapsed;            ///< For per-notification timeout

                                                                       // Stack-shift for smooth reflow when a card is removed
                                                                       float                 m_stackShift = 0.f;
                                                                       QPropertyAnimation*   m_shiftAnim  = nullptr;

                                                                       // Mouse tracking
                                                                       QPoint                m_lastPos;
                                                                       uint32_t              m_pressedId  = 0;    ///< Card under mouse-press

                                                                       // ID counter
                                                                       static uint32_t       s_nextId;

                                                                       // ── Layout constants ──────────────────────────────────────────────────
                                                                       static constexpr int   kCardWidth       = 360;  ///< Fixed card width px
                                                                       static constexpr int   kCardPadding     =  14;  ///< Inner padding
                                                                       static constexpr int   kCardSpacing     =   8;  ///< Gap between stacked cards
                                                                       static constexpr int   kCardRadius      =  14;  ///< Corner radius
                                                                       static constexpr int   kIconSize        =  32;  ///< Icon square size px
                                                                       static constexpr int   kCloseButtonSize =  16;  ///< ✕ button size px
                                                                       static constexpr int   kProgressBarH    =   3;  ///< Progress bar height px
                                                                       static constexpr int   kActionButtonH   =  28;  ///< Action button height px
                                                                       static constexpr int   kRightMargin     =  16;  ///< Distance from right edge
                                                                       static constexpr int   kMaxCards        =   5;  ///< Maximum simultaneous cards

                                                                       // ── Animation constants ───────────────────────────────────────────────
                                                                       static constexpr float kEnterSpeed      = 0.09f; ///< Per-tick enter progress
                                                                       static constexpr float kExitSpeed       = 0.10f; ///< Per-tick exit progress
                                                                       static constexpr float kGlowPulseSpeed  = 0.025f;///< Per-tick glow for Critical

                                                                       // ── Urgency default timeouts ──────────────────────────────────────────
                                                                       static constexpr int   kTimeoutLow      = 8000;  ///< ms
                                                                       static constexpr int   kTimeoutNormal   = 5000;  ///< ms
                                                                       static constexpr int   kTimeoutCritical = -1;    ///< no auto-dismiss
};
