#pragma once

#include <QWidget>
#include <QTimer>
#include <QElapsedTimer>
#include <QList>
#include <QHash>
#include <QRect>
#include <QPixmap>
#include <QImage>
#include <QFont>
#include <QColor>
#include <QString>
#include <QPropertyAnimation>

#include "core/TilingEngine.h"   // TilingLayout

class WMCompositor;
class Window;
class Workspace;
class QPainter;
class QPainterPath;

// ─────────────────────────────────────────────────────────────────────────────
// WorkspaceIndicator
//
// Snapshot of one workspace's display state, rebuilt whenever the workspace
// list or active workspace changes.  Stored in m_wsIndicators and used by
// drawWorkspaces() to position and paint each pill.
// ─────────────────────────────────────────────────────────────────────────────
struct WorkspaceIndicator {
    int     id       = 0;
    QString name;            ///< Display name (number or user label)
    bool    active   = false;///< This workspace is currently shown
    bool    occupied = false;///< Has at least one window
    bool    urgent   = false;///< A window on this workspace demands attention
    QRect   rect;            ///< Hit-test rect in bar-local coordinates
    float   hoverProgress = 0.f; ///< 0–1 hover fade-in; driven by m_animTimer
};

// ─────────────────────────────────────────────────────────────────────────────
// StatusItem
//
// One entry in the right-hand status section of the bar (e.g. CPU, RAM, clock,
// battery).  Drawn by drawStatusSection() in left-to-right order.
// ─────────────────────────────────────────────────────────────────────────────
struct StatusItem {
    enum class Kind {
        Text,        ///< Plain label — m_text is rendered directly
        Icon,        ///< QPixmap icon with optional label
        Progress,    ///< Horizontal bar (e.g. CPU / RAM percentage)
        Separator    ///< Thin vertical divider, no text
    };

    Kind    kind  = Kind::Text;
    QString icon;        ///< XDG icon name (loaded lazily)
    QString text;        ///< Rendered label (may include icon glyph)
    float   value = 0.f; ///< Normalised value in [0, 1] for Kind::Progress
    QColor  color;       ///< Override colour; transparent = use theme default
    int     width = 0;   ///< Cached pixel width after last layout pass
};

// ─────────────────────────────────────────────────────────────────────────────
// BarSection
//
// Logical horizontal sections of the bar, laid out left → centre → right.
// ─────────────────────────────────────────────────────────────────────────────
enum class BarSection {
    Left,     ///< Logo + workspace indicators
    Centre,   ///< Active window title
    Right     ///< Layout indicator + status items + clock
};

// ─────────────────────────────────────────────────────────────────────────────
// BarWidget
//
// The HackerLand WM top panel.  A fullscreen-width, fixed-height QWidget drawn
// with QPainter to achieve the glassmorphism look:
//
//   ┌──────────────────────────────────────────────────────────────────────┐
//   │  ⬡  [1] [2●] [3] [4]   │  Firefox — HackerOS Dev   │  ⠿ ▲ 42%  10:24│
//   └──────────────────────────────────────────────────────────────────────┘
//      Logo  Workspaces          Active title                Status   Clock
//
// Rendering
//   • Translucent frosted-glass background (blur of wallpaper below).
//   • Workspace pills: inactive = dim outline, occupied = accent dot,
//     active = gradient fill with glow border, urgent = pulsing red.
//   • Active window title with app icon, truncated with ellipsis.
//   • Layout indicator glyph that cycles layouts on click.
//   • Status section: CPU bar, RAM label, volume icon, battery icon.
//   • Digital clock with date tooltip.
//
// Interactions
//   • Left-click on workspace pill  → emit workspaceSwitchRequested(id).
//   • Middle-click on workspace pill → move active window there.
//   • Right-click on workspace pill → context menu (future).
//   • Left-click on layout indicator → cycle layout forward.
//   • Right-click on layout indicator → cycle layout backward.
//   • Left-click on title section → focus active window.
//   • Scroll wheel over workspaces  → cycle workspaces.
//
// Layer shell
//   The bar is registered with WMLayerShell as a Top-layer surface with an
//   exclusive zone equal to its height, so the tiling engine never places
//   windows underneath it.  (See WMCompositor::setupBar().)
// ─────────────────────────────────────────────────────────────────────────────
class BarWidget : public QWidget {
    Q_OBJECT

    // Q_PROPERTY so QPropertyAnimation can drive the glow pulse.
    Q_PROPERTY(float glowPulse READ glowPulse WRITE setGlowPulse)

public:
    // ── Construction ──────────────────────────────────────────────────────
    explicit BarWidget(WMCompositor* compositor, QWidget* parent = nullptr);
    ~BarWidget() override;

    // ── Geometry ──────────────────────────────────────────────────────────

    /// Re-anchor the bar to the top of \p screenGeometry and resize to fit.
    /// Called by WMCompositor::setupBar() and on screen geometry change.
    void setScreenGeometry(const QRect& screenGeometry);

    /// Height in logical pixels (read from Config::theme.barHeight).
    int barHeight() const;

    // ── Glow pulse property (driven by QPropertyAnimation) ───────────────
    float glowPulse() const        { return m_glowPulse; }
    void  setGlowPulse(float v)    { m_glowPulse = v; update(); }

    // ── Data refresh ──────────────────────────────────────────────────────

    /// Force a full rebuild of workspace indicators on the next paint.
    void invalidateWorkspaces();

    /// Force a redraw of the blur-cache background on the next paint.
    void invalidateBackground();

signals:
    // ── User-action signals (consumed by WMCompositor) ────────────────────

    /// User clicked on workspace pill \p id.
    void workspaceSwitchRequested(int id);

    /// User middle-clicked on workspace pill — move active window there.
    void moveWindowToWorkspaceRequested(int id);

    /// User clicked on the active-window title section.
    void activeWindowFocusRequested();

    /// User left-clicked on the layout indicator.
    void layoutCycleForwardRequested();

    /// User right-clicked on the layout indicator.
    void layoutCycleBackwardRequested();

    /// Compositor should launch \p cmd (e.g. from a clickable status icon).
    void launchRequested(const QString& cmd);

protected:
    // ── Qt event overrides ────────────────────────────────────────────────
    void paintEvent     (QPaintEvent*  event) override;
    void mousePressEvent(QMouseEvent*  event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent (QMouseEvent*  event) override;
    void wheelEvent     (QWheelEvent*  event) override;
    void enterEvent     (QEnterEvent*  event) override;
    void leaveEvent     (QEvent*       event) override;
    void resizeEvent    (QResizeEvent* event) override;

private slots:
    // ── Periodic updates ──────────────────────────────────────────────────
    void updateClock();           ///< Refresh m_timeStr / m_dateStr (1 s)
    void updateSysStats();        ///< Refresh CPU / RAM / battery (2 s)
    void onAnimTick();            ///< Drive hover / glow animations (~30 fps)

    // ── Compositor event reactions ────────────────────────────────────────
    void onWorkspaceChanged(int id);
    void onWindowAdded(Window* w);
    void onWindowRemoved(Window* w);
    void onActiveWindowChanged(Window* w);
    void onLayoutChanged(TilingLayout layout);
    void onConfigReloaded();

private:
    // ── Paint passes (called in order from paintEvent) ────────────────────

    /// Frosted glass background: blurred wallpaper slice + tinted overlay.
    void drawBackground(QPainter& p);

    /// Bottom border line with gradient glow.
    void drawBorder(QPainter& p);

    /// Left section: logo glyph.
    void drawLogo(QPainter& p);

    /// Workspace pills in the left section.
    void drawWorkspaces(QPainter& p);

    /// Active window icon + title in the centre section.
    void drawActiveWindow(QPainter& p);

    /// Layout indicator glyph between centre and right sections.
    void drawLayoutIndicator(QPainter& p);

    /// CPU / RAM / volume / battery indicators.
    void drawStatusSection(QPainter& p);

    /// Clock (time + date) in the right section.
    void drawClock(QPainter& p);

    /// Thin vertical separator between sections.
    void drawSeparator(QPainter& p, int x);

    // ── Individual status-item draw helpers ───────────────────────────────
    void drawCpuItem    (QPainter& p, const QRect& rect);
    void drawRamItem    (QPainter& p, const QRect& rect);
    void drawVolumeItem (QPainter& p, const QRect& rect);
    void drawBatteryItem(QPainter& p, const QRect& rect);

    // ── Workspace pill helpers ────────────────────────────────────────────

    /// Recompute WorkspaceIndicator rects for the current workspace list.
    void rebuildWorkspaceIndicators();

    /// Return the indicator at bar-local \p pos, or nullptr.
    WorkspaceIndicator* indicatorAt(const QPoint& pos);

    /// Draw one workspace pill at its rect.
    void drawWorkspacePill(QPainter& p, const WorkspaceIndicator& ws);

    // ── Layout section geometry helpers ──────────────────────────────────

    /// Rectangle of the left section (logo + workspaces).
    QRect leftSectionRect()   const;

    /// Rectangle of the centre section (active window title).
    QRect centreSectionRect() const;

    /// Rectangle of the right section (layout + status + clock).
    QRect rightSectionRect()  const;

    /// Rectangle occupied by the layout indicator glyph.
    QRect layoutIndicatorRect() const;

    /// Rectangle occupied by the clock text.
    QRect clockRect() const;

    // ── System-stat helpers ───────────────────────────────────────────────

    /// Read /proc/stat and return CPU usage as a fraction in [0, 1].
    float readCpuUsage();

    /// Read /proc/meminfo and return used / total as a fraction in [0, 1].
    float readRamUsage();

    /// Return used RAM as a human-readable string, e.g. "3.2 GB".
    QString readRamString();

    /// Return current volume as a fraction in [0, 1] via wpctl / PulseAudio.
    float readVolume();

    /// Return battery charge as a fraction in [0, 1], or -1 if unavailable.
    float readBattery();

    // ── Background blur helpers ───────────────────────────────────────────

    /// Capture a screenshot of the wallpaper area behind the bar,
    /// blur it, and cache the result in m_bgCache.
    void rebuildBackgroundCache();

    /// Cheap 3-pass box blur on an ARGB32_Premultiplied image.
    static QImage boxBlur(const QImage& src, int radius);

    // ── Font helpers ──────────────────────────────────────────────────────

    QFont barFont(int size = -1) const;    ///< Primary UI font from config
    QFont monoFont(int size = -1) const;   ///< Monospace font for clock / stats
    QFont iconFont(int size = -1) const;   ///< Symbol/icon glyph font

    // ── Members ───────────────────────────────────────────────────────────

    WMCompositor*    m_compositor = nullptr;

    // ── Timers ────────────────────────────────────────────────────────────
    QTimer*          m_clockTimer  = nullptr;  ///< 1 s — clock refresh
    QTimer*          m_animTimer   = nullptr;  ///< ~33 ms — hover / glow
    QTimer*          m_statsTimer  = nullptr;  ///< 2 s — sys stats

    // ── Clock ─────────────────────────────────────────────────────────────
    QString          m_timeStr;    ///< e.g. "10:24"
    QString          m_dateStr;    ///< e.g. "Thu 5 Mar"

    // ── System stats (updated by m_statsTimer) ────────────────────────────
    float            m_cpuUsage   = 0.f;  ///< [0, 1]
    float            m_ramUsage   = 0.f;  ///< [0, 1]
    QString          m_ramStr;             ///< "3.2 GB"
    float            m_volume     = -1.f; ///< [0, 1] or -1 if unknown
    float            m_battery    = -1.f; ///< [0, 1] or -1 if no battery

    // /proc/stat state for incremental CPU calculation
    qint64           m_prevCpuTotal = 0;
    qint64           m_prevCpuIdle  = 0;

    // ── Workspace indicators ──────────────────────────────────────────────
    QList<WorkspaceIndicator> m_wsIndicators;
    bool             m_wsIndicatorsDirty = true;

    // ── Active window ─────────────────────────────────────────────────────
    QString          m_activeTitle;   ///< Cached title to avoid per-frame lookup
    QString          m_activeAppId;   ///< For icon lookup

    // ── Layout indicator ──────────────────────────────────────────────────
    TilingLayout     m_currentLayout = TilingLayout::Spiral;

    // ── Hover state ───────────────────────────────────────────────────────
    int              m_hoveredWsId   = -1;  ///< Workspace pill under cursor
    bool             m_hoverLayout   = false;
    bool             m_hoverClock    = false;
    QPoint           m_lastMousePos;

    // ── Glow pulse (driven by QPropertyAnimation) ────────────────────────
    float            m_glowPulse     = 0.f;
    float            m_glowDir       = 1.f;
    QPropertyAnimation* m_pulseAnim  = nullptr;

    // ── Background blur cache ─────────────────────────────────────────────
    QPixmap          m_bgCache;
    bool             m_bgCacheDirty  = true;

    // ── Section geometry cache (invalidated on resize) ────────────────────
    mutable QRect    m_leftRect;
    mutable QRect    m_centreRect;
    mutable QRect    m_rightRect;
    mutable bool     m_sectionsDirty = true;

    // ── Layout constants ──────────────────────────────────────────────────
    static constexpr int kPaddingH          = 10;  ///< Horizontal outer padding
    static constexpr int kPaddingV          =  4;  ///< Vertical outer padding
    static constexpr int kSeparatorW        =  1;  ///< Separator line width
    static constexpr int kWsPillMinW        = 24;  ///< Minimum workspace pill width
    static constexpr int kWsPillH           = 20;  ///< Workspace pill height
    static constexpr int kWsPillRadius      =  6;  ///< Corner radius of pill
    static constexpr int kWsPillSpacing     =  4;  ///< Gap between pills
    static constexpr int kLogoW             = 28;  ///< Logo glyph width
    static constexpr int kLayoutIndicatorW  = 26;  ///< Layout glyph width
    static constexpr int kClockW            = 72;  ///< Clock text reserved width
    static constexpr int kStatusItemSpacing =  8;  ///< Gap between status items
    static constexpr float kHoverAnimSpeed  = 0.12f; ///< Per-tick hover progress step
    static constexpr float kGlowPulseSpeed  = 0.018f;///< Per-tick glow pulse step
    static constexpr int   kBlurRadius      = 18;  ///< Background blur strength
};
