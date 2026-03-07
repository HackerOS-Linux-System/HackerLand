#pragma once

#include <QObject>
#include <QPoint>
#include <QRect>
#include <QHash>
#include <QSet>
#include <QString>
#include <QElapsedTimer>

#include <Qt>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QTabletEvent>
#include <QTouchEvent>

class WMCompositor;
class Window;
class Workspace;

// ─────────────────────────────────────────────────────────────────────────────
// PointerButton
//
// Logical pointer button identifiers independent of the Qt enum, so protocol
// code can reference them without pulling in QMouseEvent everywhere.
// ─────────────────────────────────────────────────────────────────────────────
enum class PointerButton : uint32_t {
    Left   = 0x110,   // BTN_LEFT  (Linux evdev)
    Right  = 0x111,   // BTN_RIGHT
    Middle = 0x112,   // BTN_MIDDLE
    Side   = 0x113,
    Extra  = 0x114
};

// ─────────────────────────────────────────────────────────────────────────────
// KeybindMatch
//
// Result of trying to match a key event against the configured bindings table.
// ─────────────────────────────────────────────────────────────────────────────
struct KeybindMatch {
    bool    matched = false;
    QString action;          ///< e.g. "exec:alacritty", "workspace:3"
};

// ─────────────────────────────────────────────────────────────────────────────
// GestureType
// ─────────────────────────────────────────────────────────────────────────────
enum class GestureType {
    None,
    SwipeLeft,
    SwipeRight,
    SwipeUp,
    SwipeDown,
    Pinch
};

// ─────────────────────────────────────────────────────────────────────────────
// InputHandler
//
// Centralises all input processing for HackerLand WM so WMOutput stays focused
// on rendering and the various protocol handlers (WMXdgShell, WMLayerShell)
// don't need to duplicate hit-testing and keybind logic.
//
// Responsibilities
//   Keyboard
//     • Decode raw Qt key events into modifier+key strings.
//     • Look up the action string in Config::keys.bindings.
//     • Emit actionTriggered() for the compositor to dispatch.
//     • Forward events not matched by a binding to the focused Wayland surface
//       via QWaylandSeat.
//     • Track held modifiers for chord detection (e.g. Super held = tiling mode).
//
//   Pointer / mouse
//     • Track cursor position across the output surface.
//     • Hit-test the window stack: title bar, resize edges, close/max/min dots,
//       client area.
//     • Manage drag state: initiate / update / finish floating-window drags.
//     • Manage resize state: initiate / update / finish edge resize.
//     • Forward pointer events to the correct Wayland surface.
//     • Emit scroll events for workspace switching (Super+Scroll).
//
//   Touch / gestures (future-proof stub)
//     • Detect 3-finger swipe for workspace switching.
//     • Detect 2-finger pinch for master ratio adjustment.
//
// Usage
//   WMOutput creates one InputHandler and routes all Qt input events through
//   it.  The handler emits signals; WMCompositor connects to them.
// ─────────────────────────────────────────────────────────────────────────────
class InputHandler : public QObject {
    Q_OBJECT

public:
    // ── Construction ──────────────────────────────────────────────────────
    explicit InputHandler(WMCompositor* compositor, QObject* parent = nullptr);
    ~InputHandler() override;

    // ── Event entry points (called by WMOutput) ───────────────────────────

    /// Process a Qt key-press event.
    /// Returns true if the event was consumed by a keybind, false if it should
    /// be forwarded to the focused Wayland surface.
    bool handleKeyPress(QKeyEvent* event);

    /// Process a Qt key-release event.
    /// Returns true if consumed (e.g. the matching press was a keybind).
    bool handleKeyRelease(QKeyEvent* event);

    /// Process pointer movement.  \p globalPos is in compositor coordinates.
    void handleMouseMove(const QPoint& globalPos,
                         Qt::MouseButtons buttons);

    /// Process a pointer button press.
    void handleMousePress(const QPoint& globalPos,
                          Qt::MouseButton button,
                          Qt::KeyboardModifiers modifiers);

    /// Process a pointer button release.
    void handleMouseRelease(const QPoint& globalPos,
                            Qt::MouseButton button,
                            Qt::KeyboardModifiers modifiers);

    /// Process a scroll-wheel event.
    void handleWheel(const QPoint& globalPos,
                     const QPointF& angleDelta,
                     Qt::KeyboardModifiers modifiers);

    /// Process a touch event (stub for future gesture support).
    void handleTouch(QTouchEvent* event);

    // ── State queries ─────────────────────────────────────────────────────

    /// Current pointer position in compositor coordinates.
    QPoint cursorPos()         const { return m_cursorPos; }

    /// True while a floating window is being dragged.
    bool isDragging()          const { return m_dragging;  }

    /// True while a window edge resize is in progress.
    bool isResizing()          const { return m_resizing;  }

    /// Window under the pointer at the last mouse-move event.
    Window* hoveredWindow()    const { return m_hoveredWindow; }

    /// Window currently being dragged (nullptr if not dragging).
    Window* dragWindow()       const { return m_dragWindow; }

    /// Window currently being resized (nullptr if not resizing).
    Window* resizeWindow()     const { return m_resizeWindow; }

    /// Currently held keyboard modifiers.
    Qt::KeyboardModifiers currentModifiers() const { return m_modifiers; }

    // ── Configuration reload ──────────────────────────────────────────────

    /// Re-read keybindings from Config::instance().keys after a config reload.
    void reloadBindings();

signals:
    // ── Keybind actions ───────────────────────────────────────────────────

    /// A keybind was matched and its action string decoded.
    /// WMCompositor connects to this and dispatches the action.
    void actionTriggered(const QString& action);

    // ── Focus change requests ─────────────────────────────────────────────

    /// Pointer clicked in the client area of \p w; compositor should focus it.
    void windowFocusRequested(Window* w);

    // ── Drag signals ──────────────────────────────────────────────────────

    /// A floating-window drag has started.
    void dragStarted(Window* w, const QPoint& startPos);

    /// Drag in progress; compositor should update window geometry.
    void dragUpdated(Window* w, const QPoint& newWindowTopLeft);

    /// Drag finished; compositor may snap window to edges or tiling zones.
    void dragFinished(Window* w, const QPoint& finalWindowTopLeft);

    // ── Resize signals ────────────────────────────────────────────────────

    /// Edge resize started.
    void resizeStarted(Window* w, Qt::Edges edges, const QRect& startGeom);

    /// Resize in progress; compositor should apply the new geometry.
    void resizeUpdated(Window* w, const QRect& newGeom);

    /// Resize finished.
    void resizeFinished(Window* w, const QRect& finalGeom);

    // ── Title bar button signals ──────────────────────────────────────────
    void closeButtonClicked(Window* w);
    void maximizeButtonClicked(Window* w);
    void minimizeButtonClicked(Window* w);

    // ── Cursor shape ──────────────────────────────────────────────────────

    /// Compositor / output should display this cursor shape.
    void cursorShapeChanged(Qt::CursorShape shape);

    // ── Gesture signals (touch / trackpad) ───────────────────────────────
    void gestureDetected(GestureType type, float magnitude);

private:
    // ── Keybind helpers ───────────────────────────────────────────────────

    /// Build the canonical binding key string for a Qt key event,
    /// e.g. "Super+Return", "Super+Shift+Q", "Alt+F4".
    QString buildBindingKey(QKeyEvent* event) const;

    /// Look up \p bindingKey in the bindings table.
    KeybindMatch matchBinding(const QString& bindingKey) const;

    /// Forward a key event to the currently-focused Wayland surface via the
    /// compositor's default seat.
    void forwardKeyToSurface(QKeyEvent* event) const;

    // ── Hit-testing ───────────────────────────────────────────────────────

    /// Return the topmost window whose geometry contains \p pos.
    Window* windowAt(const QPoint& pos) const;

    /// True if \p pos lands inside the title bar of \p w.
    bool inTitleBar(const QPoint& pos, const Window* w) const;

    /// Which resize edge (combination) \p pos hits on \p w's border.
    /// Returns Qt::Edges{} (no edge) if not on a resize border.
    Qt::Edges resizeEdgeAt(const QPoint& pos, const Window* w) const;

    // ── Title-bar button rects ────────────────────────────────────────────
    enum class TitleBarButton { None, Close, Maximize, Minimize };

    TitleBarButton titleBarButtonAt(const QPoint& pos, const Window* w) const;
    QRect closeButtonRect   (const Window* w) const;
    QRect maximizeButtonRect(const Window* w) const;
    QRect minimizeButtonRect(const Window* w) const;

    // ── Wayland event forwarding ──────────────────────────────────────────
    void forwardMouseButtonToSurface(Window* w, const QPoint& globalPos,
                                     Qt::MouseButton button, bool pressed);

    // ── Drag helpers ──────────────────────────────────────────────────────
    void beginDrag(Window* w, const QPoint& cursorPos);
    void updateDrag(const QPoint& cursorPos);
    void endDrag(const QPoint& cursorPos);

    // ── Resize helpers ────────────────────────────────────────────────────
    void beginResize(Window* w, const QPoint& cursorPos, Qt::Edges edges);
    void updateResize(const QPoint& cursorPos);
    void endResize(const QPoint& cursorPos);

    // ── Cursor shape ──────────────────────────────────────────────────────
    void updateCursorShape(const QPoint& pos);
    Qt::CursorShape cursorShapeForEdges(Qt::Edges edges) const;

    // ── Gesture helpers ───────────────────────────────────────────────────
    void processGestureUpdate();

    // ── Members ───────────────────────────────────────────────────────────

    WMCompositor*         m_compositor     = nullptr;

    // Keyboard
    Qt::KeyboardModifiers m_modifiers      = Qt::NoModifier;
    QSet<int>             m_pressedKeys;   ///< Qt::Key values currently held
    QSet<int>             m_consumedKeys;  ///< Keys consumed by a keybind

    // Pointer
    QPoint                m_cursorPos;
    Window*               m_hoveredWindow  = nullptr;
    Qt::MouseButtons      m_heldButtons    = Qt::NoButton;

    // Drag state
    bool                  m_dragging       = false;
    Window*               m_dragWindow     = nullptr;
    QPoint                m_dragOffset;          ///< cursor − window.topLeft
    QRect                 m_dragStartGeom;

    // Resize state
    bool                  m_resizing       = false;
    Window*               m_resizeWindow   = nullptr;
    Qt::Edges             m_resizeEdges    = {};
    QPoint                m_resizeStartCursor;
    QRect                 m_resizeStartGeom;

    // Touch / gesture state
    struct TouchState {
        QHash<int, QPointF> points;    ///< touchpoint id → current position
        QPointF             startCentroid;
        float               startSpan  = 0.f;
        GestureType         active     = GestureType::None;
    } m_touch;

    // Layout constants (must match WMOutput drawing constants)
    static constexpr int kTitleBarHeight    = 28;
    static constexpr int kResizeHandleWidth =  6;
    static constexpr int kDotRadius         =  5;
    static constexpr int kDotSpacing        = 16;
    static constexpr int kDotRightMargin    = 12;

    // Gesture thresholds
    static constexpr float kSwipeThresholdPx  = 80.f;
    static constexpr float kPinchThreshold    = 0.15f;
};
