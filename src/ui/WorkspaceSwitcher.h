#pragma once

#include "GlassWidget.h"
#include "core/TilingEngine.h"

#include <QList>
#include <QHash>
#include <QRect>
#include <QPixmap>
#include <QTimer>
#include <QElapsedTimer>
#include <QPropertyAnimation>

class WMCompositor;
class Workspace;
class Window;
class QPainter;
class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

// ─────────────────────────────────────────────────────────────────────────────
// WsThumb
//
// Per-workspace thumbnail data rebuilt when the switcher opens or when a
// workspace's window list changes.
// ─────────────────────────────────────────────────────────────────────────────
struct WsThumb {
    int      id       = 0;
    QString  name;
    bool     active   = false;     ///< This is the currently-shown workspace
    bool     occupied = false;     ///< Has at least one visible window
    QRect    rect;                 ///< Thumb rect in switcher-local coords
    QPixmap  preview;              ///< Scaled-down window layout preview
    bool     previewDirty = true;  ///< Needs to be repainted

    // ── Window mini-tiles for the preview ─────────────────────────────────
    struct MiniTile {
        QRect   rect;              ///< Scaled rect inside thumb
        QString title;
        QString appId;
        bool    active  = false;
        bool    floating = false;
    };
    QList<MiniTile> tiles;

    // ── Animation ─────────────────────────────────────────────────────────
    float    enterProgress = 0.f;  ///< 0→1 on first appearance
    float    hoverProgress = 0.f;  ///< 0→1 hover glow fade-in
    float    selectScale   = 1.f;  ///< pops on selection (spring overshoot)
};

// ─────────────────────────────────────────────────────────────────────────────
// WorkspaceSwitcher
//
// A full-screen glass overlay shown via Super+Tab (or Super+grave).
// Displays a row of workspace thumbnails with scaled window previews;
// the user can navigate with arrow keys, number keys, or mouse click.
//
// Layout
//   ┌────────────────────────────────────────────────────────────────┐
//   │                                                                │
//   │   ┌──────┐   ┌══════╗   ┌──────┐   ┌──────┐   ┌──────┐      │
//   │   │  1   │   ║  2 ● ║   │  3   │   │  4   │   │  5   │      │
//   │   │ [::] │   ║ [::] ║   │      │   │      │   │      │      │
//   │   └──────┘   ╚══════╝   └──────┘   └──────┘   └──────┘      │
//   │                Firefox — HackerOS Dev                         │
//   │                                                               │
//   └────────────────────────────────────────────────────────────────┘
//
// Behaviour
//   • Show:  animate in (fade + scale) over kShowMs milliseconds.
//   • Close: animate out (fade + scale) over kHideMs milliseconds, then
//            hide the widget and switch workspace.
//   • Navigation: Left/Right arrow or H/L cycle selection.
//                 Number 1–9 jump directly.
//                 Return/Space confirm and close.
//                 Escape cancel (return to original workspace).
//                 Mouse click on a thumb selects and confirms.
//                 Scroll wheel cycles selection.
//   • Live updates: WMCompositor signals cause thumbnail rebuilds so the
//                   content is current when the overlay is shown.
// ─────────────────────────────────────────────────────────────────────────────
class WorkspaceSwitcher : public GlassWidget {
    Q_OBJECT

    Q_PROPERTY(float showProgress READ showProgress WRITE setShowProgress)

public:
    explicit WorkspaceSwitcher(WMCompositor* compositor,
                               QWidget*      parent = nullptr);
    ~WorkspaceSwitcher() override;

    // ── Show / hide ───────────────────────────────────────────────────────

    /// Open the switcher and optionally pre-select workspace \p startId.
    /// If startId is -1 the currently active workspace is pre-selected.
    void open(int startId = -1);

    /// Close the switcher without switching (Escape).
    void cancel();

    /// True while the switcher is open (including animating in/out).
    bool isOpen() const { return m_open; }

    // ── Show-progress property ────────────────────────────────────────────
    float showProgress()         const { return m_showProgress; }
    void  setShowProgress(float v)     { m_showProgress = v; update(); }

signals:
    /// Emitted when the user confirms a workspace selection.
    void workspaceSwitchRequested(int id);

protected:
    void paintEvent      (QPaintEvent*  e) override;
    void keyPressEvent   (QKeyEvent*    e) override;
    void mousePressEvent (QMouseEvent*  e) override;
    void mouseMoveEvent  (QMouseEvent*  e) override;
    void leaveEvent      (QEvent*       e) override;
    void wheelEvent      (QWheelEvent*  e) override;

private slots:
    void onAnimTick();
    void onWorkspaceChanged(int id);
    void onWindowAdded(Window* w);
    void onWindowRemoved(Window* w);
    void onWindowTitleChanged();

private:
    // ── Thumbnail management ──────────────────────────────────────────────
    void rebuildThumbs();
    void rebuildThumb(WsThumb& t, Workspace* ws);
    void markThumbsDirty();
    WsThumb* thumbForId(int id);
    WsThumb* thumbAt(const QPoint& pos);

    // ── Navigation ────────────────────────────────────────────────────────
    void selectNext();
    void selectPrev();
    void selectId(int id);
    void confirm();           ///< Switch to m_selectedId and close

    // ── Paint passes ──────────────────────────────────────────────────────
    void drawBackground   (QPainter& p);
    void drawThumb        (QPainter& p, const WsThumb& t);
    void drawThumbFrame   (QPainter& p, const WsThumb& t);
    void drawThumbTiles   (QPainter& p, const WsThumb& t);
    void drawThumbLabel   (QPainter& p, const WsThumb& t);
    void drawThumbDot     (QPainter& p, const WsThumb& t);
    void drawSelectedTitle(QPainter& p);
    void drawHint         (QPainter& p);

    // ── Geometry helpers ──────────────────────────────────────────────────
    void computeThumbRects();        ///< Place thumb rects across the overlay
    QRect thumbAreaRect() const;     ///< Central row where thumbs live
    QRect titleAreaRect() const;     ///< Label below thumbs

    // ── Easing ────────────────────────────────────────────────────────────
    static float easeOutBack  (float t);
    static float easeInCubic  (float t);
    static float easeOutCubic (float t);
    static float spring       (float t);

    // ── Utility ───────────────────────────────────────────────────────────
    QFont  uiFont   (int size = -1) const;
    QFont  monoFont (int size = -1) const;

    // ── Members ───────────────────────────────────────────────────────────
    WMCompositor*    m_compositor   = nullptr;

    QList<WsThumb>   m_thumbs;
    int              m_selectedId   = -1;  ///< Currently highlighted thumb
    int              m_originalId   = -1;  ///< Workspace active when opened

    bool             m_open         = false;
    bool             m_closing      = false;

    // Overlay show/hide animation (0=hidden → 1=visible)
    float            m_showProgress = 0.f;
    QPropertyAnimation* m_showAnim  = nullptr;

    QTimer*          m_animTimer    = nullptr;

    // Hover
    int              m_hoveredId    = -1;

    // Layout constants
    static constexpr int   kThumbW         = 200;  ///< Base thumb width  px
    static constexpr int   kThumbH         = 130;  ///< Base thumb height px
    static constexpr int   kThumbSpacing   =  16;  ///< Gap between thumbs
    static constexpr int   kThumbRadius    =  10;  ///< Corner radius
    static constexpr int   kThumbPad       =   6;  ///< Inner padding
    static constexpr int   kDotR           =   4;  ///< Occupied-dot radius
    static constexpr int   kLabelH         =  32;  ///< Height below thumbs
    static constexpr int   kHintH          =  20;  ///< Key-hint row height

    // Animation constants
    static constexpr int   kShowMs         = 180;
    static constexpr int   kHideMs         = 140;
    static constexpr float kEnterSpeed     = 0.11f;
    static constexpr float kHoverSpeed     = 0.13f;
    static constexpr float kSelectPop      = 1.06f; ///< Scale overshoot
};
