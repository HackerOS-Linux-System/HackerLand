#pragma once

#include <QObject>
#include <QList>
#include <QRect>
#include <QString>
#include <climits>

#include "TilingEngine.h"

class Window;
enum class WindowState;

// ─────────────────────────────────────────────────────────────────────────────
// FocusDirection
//
// Used by Workspace::focusDirection() to move keyboard focus towards the
// window closest to the active window in the given screen direction.
// ─────────────────────────────────────────────────────────────────────────────
enum class FocusDirection { Left, Right, Up, Down };

// ─────────────────────────────────────────────────────────────────────────────
// Workspace
//
// A named, numbered virtual desktop.  Owns an ordered list of Window pointers
// (windows are owned by WMCompositor), manages keyboard focus, and delegates
// geometry computation to its private TilingEngine instance.
//
// Invariants
//   • m_windows contains every window assigned to this workspace, in the order
//     they were added (modified by moveWindow* and swap* operations).
//   • m_activeWindow is either nullptr or a pointer in m_windows.
//   • m_focusHistory is a prefix-ordered MRU list; all entries are in
//     m_windows (stale entries are removed when windows are removed).
//   • Tile slots (Window::tileSlot) are kept in sync with m_windows order by
//     refreshTileSlots().
// ─────────────────────────────────────────────────────────────────────────────
class Workspace : public QObject {
    Q_OBJECT

public:
    // ── Construction ──────────────────────────────────────────────────────
    explicit Workspace(int id, QObject* parent = nullptr);
    ~Workspace() override;

    // ── Identity ──────────────────────────────────────────────────────────
    int     id()       const { return m_id;     }
    QString name()     const { return m_name;   }
    bool    isActive() const { return m_active; }

    void setName(const QString& n) { m_name = n; }
    void setActive(bool a)         { m_active = a; }

    // ── Layout ────────────────────────────────────────────────────────────
    TilingLayout layout() const { return m_layout; }

    /// Change the layout and emit layoutChanged().
    void setLayout(TilingLayout l);

    /// Advance to the next layout in the cycle order.
    void cycleLayoutForward();

    /// Retreat to the previous layout in the cycle order.
    void cycleLayoutBackward();

    // ── Window management ─────────────────────────────────────────────────

    /// Add \p w to this workspace.  No-op if already present.
    /// Connects window signals and refreshes tile slots.
    void addWindow(Window* w);

    /// Remove \p w from this workspace.  Promotes MRU window as new focus.
    /// Disconnects window signals and refreshes tile slots.
    void removeWindow(Window* w);

    bool contains(Window* w) const;

    QList<Window*> windows()   const { return m_windows; }
    int  windowCount()         const { return m_windows.size(); }
    bool isEmpty()             const { return m_windows.isEmpty(); }

    // ── Focus management ──────────────────────────────────────────────────

    Window* activeWindow()     const { return m_activeWindow; }

    /// Set focus to \p w (must be in this workspace, or nullptr).
    /// Updates the MRU focus history and emits activeWindowChanged().
    void setActiveWindow(Window* w);

    /// Focus the next window in insertion order (wraps around).
    void focusNext();

    /// Focus the previous window in insertion order (wraps around).
    void focusPrev();

    /// Focus the nearest tiled window in the given screen direction.
    /// Falls back to focusNext() if no window lies in that direction.
    void focusDirection(FocusDirection dir);

    /// Focus the first tiled window (i.e. the master in Tall/Wide layouts).
    void focusMaster();

    /// Re-focus the window that had focus before the current one (Alt+Tab style).
    void focusLast();

    // ── Window ordering / swap ────────────────────────────────────────────

    /// Swap the positions of two windows in the tile order.
    void swapWindows(Window* a, Window* b);

    /// Swap the active window with the master (first tiled) window.
    void swapWithMaster();

    /// Move \p w (or the active window) one position earlier in tile order.
    void moveWindowUp(Window* w = nullptr);

    /// Move \p w (or the active window) one position later in tile order.
    void moveWindowDown(Window* w = nullptr);

    /// Move \p w (or the active window) to the front of the tile order
    /// (making it the new master in Tall/Wide/ThreeColumn layouts).
    void moveWindowToTop(Window* w = nullptr);

    // ── Master ratio ──────────────────────────────────────────────────────
    float masterRatio()             const { return m_engine.masterRatio(); }
    void  setMasterRatio(float r);
    void  adjustMasterRatio(float delta);

    // ── Tiling ────────────────────────────────────────────────────────────

    /// Compute tile positions without applying them.
    QList<TileResult> computeTiles(const QRect& area) const;

    /// Compute and immediately apply tile positions via Window::setGeometry().
    void retile(const QRect& area);

    /// Like retile() but uses Window::setGeometryAnimated() so the
    /// AnimationEngine can interpolate each window to its new position.
    void retileWithAnimation(const QRect& area);

    /// Last area passed to retile() — used to re-run layout on state changes.
    QRect lastArea() const { return m_lastArea; }

    // ── Visibility helpers ────────────────────────────────────────────────

    /// Show all windows (called when switching TO this workspace).
    void showAllWindows();

    /// Hide all windows (called when switching AWAY from this workspace).
    void hideAllWindows();

    // ── Filtered window lists ─────────────────────────────────────────────

    /// All tiled (non-floating, non-fullscreen, visible) windows.
    QList<Window*> tiledWindows()    const;

    /// All floating visible windows.
    QList<Window*> floatingWindows() const;

    /// All visible windows (any state).
    QList<Window*> visibleWindows()  const;

    /// All windows that can receive keyboard focus (visible).
    QList<Window*> focusableWindows()const;

    // ── Debug ─────────────────────────────────────────────────────────────
    QString debugString() const;

signals:
    void windowAdded(Window* w);
    void windowRemoved(Window* w);
    void activeWindowChanged(Window* w);
    void layoutChanged(TilingLayout l);
    void masterRatioChanged(float ratio);
    void retileRequested(const QRect& area);

private slots:
    void onConfigReloaded();
    void onWindowStateChanged(WindowState state);

private:
    // ── Signal connection helpers ─────────────────────────────────────────
    void connectWindowSignals(Window* w);
    void disconnectWindowSignals(Window* w);

    // ── Internal helpers ──────────────────────────────────────────────────

    /// Assign consecutive tileSlot indices to all non-floating windows.
    void refreshTileSlots();

    /// Return the first visible window in the MRU focus history that is
    /// still present in m_windows.
    Window* mostRecentlyFocused() const;

    // ── Members ───────────────────────────────────────────────────────────
    int          m_id;
    QString      m_name;
    bool         m_active = false;

    TilingLayout m_layout = TilingLayout::Spiral;
    TilingEngine m_engine;

    QList<Window*> m_windows;        ///< All windows, in tile order.
    Window*        m_activeWindow = nullptr;

    /// Most-recently-used focus history (newest first).
    QList<Window*> m_focusHistory;

    /// Work area from the last retile() call; used to re-tile on state change.
    QRect          m_lastArea;

    static constexpr int kMaxFocusHistory = 32;
};
