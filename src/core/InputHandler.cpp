// ─────────────────────────────────────────────────────────────────────────────
// InputHandler.cpp — HackerLand WM
//
// Handles: keyboard (keybinds + key remapping), mouse (libinput accel),
//          touchpad gestures (3-finger swipe, 2-finger pinch),
//          gamepad passthrough.
//
// KEY FIX: Mouse events no longer call update() directly — they set
//          m_needsRedraw on WMOutput via signal. This breaks the
//          "every mouse move = full render" freeze loop.
// ─────────────────────────────────────────────────────────────────────────────
#include "InputHandler.h"
#include "Config.h"
#include "Window.h"
#include "compositor/WMCompositor.h"
#include "compositor/WMSurface.h"
#include "core/Workspace.h"

#include <QWaylandSeat>
#include <QWaylandSurface>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QTouchEvent>
#include <QPointF>
#include <QDebug>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers — anonymous namespace
// ─────────────────────────────────────────────────────────────────────────────
namespace {

    QString keyName(int key) {
        // Bare modifier keys are never the "trigger" key — they are the prefix.
        if (key == Qt::Key_Shift   || key == Qt::Key_Control ||
            key == Qt::Key_Alt     || key == Qt::Key_Meta    ||
            key == Qt::Key_Super_L || key == Qt::Key_Super_R ||
            key == Qt::Key_Hyper_L || key == Qt::Key_Hyper_R ||
            key == Qt::Key_AltGr   || key == Qt::Key_CapsLock)
            return {};

        switch (key) {
            case Qt::Key_Return:    return "Return";
            case Qt::Key_Enter:     return "Return";
            case Qt::Key_Space:     return "Space";
            case Qt::Key_Tab:       return "Tab";
            case Qt::Key_Backtab:   return "Tab";
            case Qt::Key_Escape:    return "Escape";
            case Qt::Key_Backspace: return "BackSpace";
            case Qt::Key_Delete:    return "Delete";
            case Qt::Key_Insert:    return "Insert";
            case Qt::Key_Home:      return "Home";
            case Qt::Key_End:       return "End";
            case Qt::Key_PageUp:    return "Prior";
            case Qt::Key_PageDown:  return "Next";
            case Qt::Key_Left:      return "Left";
            case Qt::Key_Right:     return "Right";
            case Qt::Key_Up:        return "Up";
            case Qt::Key_Down:      return "Down";
            case Qt::Key_Print:     return "Print";
            case Qt::Key_Pause:     return "Pause";
            default: break;
        }
        if (key >= Qt::Key_F1 && key <= Qt::Key_F35)
            return QString("F%1").arg(key - Qt::Key_F1 + 1);
        if (key >= Qt::Key_0 && key <= Qt::Key_9)
            return QString(QChar(key));
        if (key >= Qt::Key_A && key <= Qt::Key_Z)
            return QString(QChar(key));
        return QKeySequence(key).toString();
    }

    float pointDist(const QPointF& a, const QPointF& b) {
        const float dx = float(a.x() - b.x()), dy = float(a.y() - b.y());
        return std::sqrt(dx*dx + dy*dy);
    }

    QPointF centroid(const QHash<int,QPointF>& pts) {
        QPointF s(0,0);
        for (const auto& p : pts) s += p;
        return pts.isEmpty() ? s : s / pts.size();
    }

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

InputHandler::InputHandler(WMCompositor* compositor, QObject* parent)
: QObject(parent), m_compositor(compositor)
{
    reloadBindings();
    reloadKeyRemaps();
}

InputHandler::~InputHandler() = default;

// ─────────────────────────────────────────────────────────────────────────────
// Config reload
// ─────────────────────────────────────────────────────────────────────────────

void InputHandler::reloadBindings() {
    // Bindings are read live from Config — nothing to cache here
    qDebug() << "[Input] bindings reloaded";
}

void InputHandler::reloadKeyRemaps() {
    // Key remaps: e.g. CapsLock → Super, stored as raw Qt key int pairs
    m_keyRemaps.clear();

    const auto& remaps = Config::instance().keys.remaps;
    // remaps is QMap<QString,QString> e.g. {"CapsLock" → "Super_L"}
    // Convert name → Qt::Key
    static const QHash<QString,int> nameToKey = {
        {"CapsLock",  Qt::Key_CapsLock},
        {"Super_L",   Qt::Key_Meta},
        {"Super_R",   Qt::Key_Meta},
        {"Alt_L",     Qt::Key_Alt},
        {"Alt_R",     Qt::Key_AltGr},
        {"Ctrl_L",    Qt::Key_Control},
        {"Ctrl_R",    Qt::Key_Control},
        {"Shift_L",   Qt::Key_Shift},
        {"Shift_R",   Qt::Key_Shift},
        {"Escape",    Qt::Key_Escape},
        {"Return",    Qt::Key_Return},
    };
    for (auto it = remaps.constBegin(); it != remaps.constEnd(); ++it) {
        const int from = nameToKey.value(it.key(),   0);
        const int to   = nameToKey.value(it.value(), 0);
        if (from && to) {
            m_keyRemaps[from] = to;
            qDebug() << "[Input] remap" << it.key() << "→" << it.value();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Keyboard
// ─────────────────────────────────────────────────────────────────────────────

bool InputHandler::handleKeyPress(QKeyEvent* event) {
    // ── Apply key remap ───────────────────────────────────────────────────
    int key = event->key();
    if (m_keyRemaps.contains(key)) key = m_keyRemaps[key];

    // Track held modifiers
    m_modifiers = event->modifiers();
    m_pressedKeys.insert(key);

    // ── Build binding string ──────────────────────────────────────────────
    const QString combo = buildBindingKey(event, key);
    if (combo.isEmpty()) return false;

    const KeybindMatch match = matchBinding(combo);
    if (match.matched) {
        m_consumedKeys.insert(key);
        emit actionTriggered(match.action);
        return true;
    }

    forwardKeyToSurface(event);
    return false;
}

bool InputHandler::handleKeyRelease(QKeyEvent* event) {
    m_modifiers = event->modifiers();
    const int key = m_keyRemaps.value(event->key(), event->key());
    const bool consumed = m_consumedKeys.remove(key);
    m_pressedKeys.remove(key);
    if (!consumed) forwardKeyToSurface(event);
    return consumed;
}

QString InputHandler::buildBindingKey(QKeyEvent* event, int overrideKey) const {
    const int key = overrideKey ? overrideKey : event->key();
    const QString kn = keyName(key);
    if (kn.isEmpty()) return {};

    const QString& mod = Config::instance().keys.modifier;
    QString prefix;

    const Qt::KeyboardModifiers mods = event->modifiers();

    // Check configured modifier
    bool modHeld = false;
    if      (mod == "Super" || mod == "Meta")
        modHeld = mods & Qt::MetaModifier;
    else if (mod == "Alt")
        modHeld = mods & Qt::AltModifier;
    else if (mod == "Ctrl" || mod == "Control")
        modHeld = mods & Qt::ControlModifier;

    if (modHeld)           prefix += mod + "+";
    if (mods & Qt::ShiftModifier)   prefix += "Shift+";
    if (mods & Qt::ControlModifier && mod != "Ctrl") prefix += "Ctrl+";
    if (mods & Qt::AltModifier     && mod != "Alt")  prefix += "Alt+";

    return prefix + kn;
}

KeybindMatch InputHandler::matchBinding(const QString& key) const {
    const auto& bindings = Config::instance().keys.bindings;
    auto it = bindings.find(key);
    if (it != bindings.end()) return {true, it.value()};
    return {false, {}};
}

void InputHandler::forwardKeyToSurface(QKeyEvent* event) const {
    auto* ws = m_compositor->activeWorkspace();
    if (!ws) return;
    auto* w = ws->activeWindow();
    if (!w) return;
    auto* seat = m_compositor->defaultSeat();
    if (seat) seat->sendFullKeyEvent(event);
}

// ─────────────────────────────────────────────────────────────────────────────
// Mouse — with libinput-style pointer acceleration
// ─────────────────────────────────────────────────────────────────────────────

void InputHandler::handleMouseMove(const QPoint& globalPos, Qt::MouseButtons buttons) {
    // ── Pointer acceleration ──────────────────────────────────────────────
    // Apply simple libinput-style acceleration: fast moves are amplified,
    // slow moves are reduced. This makes fine control easy and fast movement
    // snappy without libinput dependency.
    const QPoint rawDelta   = globalPos - m_cursorPos;
    const float  speed      = std::sqrt(float(rawDelta.x()*rawDelta.x() +
    rawDelta.y()*rawDelta.y()));
    const float  accelFactor = Config::instance().input.pointerAccel; // e.g. 0.5
    const float  gain       = 1.f + accelFactor * (speed / 20.f);
    const QPoint accelDelta = QPoint(int(rawDelta.x() * gain),
                                     int(rawDelta.y() * gain));
    m_cursorPos = (m_cursorPos + accelDelta)
    .expandedTo(QPoint(0,0)); // clamp ≥ 0

    // ── Drag update ───────────────────────────────────────────────────────
    if (m_dragging && m_dragWindow) {
        QRect geom = m_dragWindow->geometry();
        geom.moveTopLeft(m_cursorPos - m_dragOffset);
        m_dragWindow->setGeometry(geom);
        emit dragUpdated(m_dragWindow, geom.topLeft());
    }

    // ── Resize update ─────────────────────────────────────────────────────
    if (m_resizing && m_resizeWindow) {
        updateResize(m_cursorPos);
    }

    // Update hovered window & cursor shape
    m_hoveredWindow = windowAt(m_cursorPos);
    updateCursorShape(m_cursorPos);

    Q_UNUSED(buttons);
}

void InputHandler::handleMousePress(const QPoint& globalPos,
                                    Qt::MouseButton button,
                                    Qt::KeyboardModifiers modifiers) {
    m_cursorPos = globalPos;
    m_heldButtons |= button;
    m_modifiers   = modifiers;

    Window* hit = windowAt(globalPos);
    if (!hit) return;

    emit windowFocusRequested(hit);

    // ── Title bar buttons ─────────────────────────────────────────────────
    const TitleBarButton tb = titleBarButtonAt(globalPos, hit);
    if (tb == TitleBarButton::Close)    { emit closeButtonClicked(hit);    return; }
    if (tb == TitleBarButton::Maximize) { emit maximizeButtonClicked(hit); return; }
    if (tb == TitleBarButton::Minimize) { emit minimizeButtonClicked(hit); return; }

    // ── Resize ────────────────────────────────────────────────────────────
    if (button == Qt::LeftButton) {
        const Qt::Edges edges = resizeEdgeAt(globalPos, hit);
        if (edges) { beginResize(hit, globalPos, edges); return; }
    }

    // ── Drag via title bar ────────────────────────────────────────────────
    if (button == Qt::LeftButton && inTitleBar(globalPos, hit)) {
        beginDrag(hit, globalPos); return;
    }

    // ── Forward to Wayland surface ────────────────────────────────────────
    forwardMouseButtonToSurface(hit, globalPos, button, true);
                                    }

                                    void InputHandler::handleMouseRelease(const QPoint& globalPos,
                                                                          Qt::MouseButton button,
                                                                          Qt::KeyboardModifiers modifiers) {
                                        m_cursorPos   = globalPos;
                                        m_heldButtons &= ~button;
                                        m_modifiers   = modifiers;

                                        if (m_dragging)  endDrag(globalPos);
                                        if (m_resizing)  endResize(globalPos);

                                        Window* hit = windowAt(globalPos);
                                        if (hit) forwardMouseButtonToSurface(hit, globalPos, button, false);
                                                                          }

                                                                          void InputHandler::handleWheel(const QPoint& globalPos,
                                                                                                         const QPointF& angleDelta,
                                                                                                         Qt::KeyboardModifiers modifiers) {
                                                                              // Super+Scroll = workspace switch
                                                                              const QString& mod = Config::instance().keys.modifier;
                                                                              const bool modHeld =
                                                                              ((mod=="Super"||mod=="Meta") && modifiers&Qt::MetaModifier) ||
                                                                              (mod=="Alt"  && modifiers&Qt::AltModifier)  ||
                                                                              (mod=="Ctrl" && modifiers&Qt::ControlModifier);

                                                                              if (modHeld) {
                                                                                  if (angleDelta.y() > 0) emit actionTriggered("workspace:prev");
                                                                                  if (angleDelta.y() < 0) emit actionTriggered("workspace:next");
                                                                                  return;
                                                                              }

                                                                              Window* hit = windowAt(globalPos);
                                                                              if (!hit) return;
                                                                              auto* seat = m_compositor->defaultSeat();
                                                                              if (seat) {
                                                                                  // Forward scroll to surface
                                                                                  seat->sendMouseWheelEvent(Qt::Vertical,
                                                                                                            angleDelta.y() > 0 ? 1 : -1);
                                                                              }
                                                                                                         }

                                                                                                         // ─────────────────────────────────────────────────────────────────────────────
                                                                                                         // Touch / touchpad gestures
                                                                                                         // ─────────────────────────────────────────────────────────────────────────────

                                                                                                         void InputHandler::handleTouch(QTouchEvent* event) {
                                                                                                             if (!event) return;
                                                                                                             const auto& pts = event->points();

                                                                                                             switch (event->type()) {
                                                                                                                 case QEvent::TouchBegin: {
                                                                                                                     m_touch.points.clear();
                                                                                                                     m_touch.active = GestureType::None;
                                                                                                                     m_touch.committed = false;
                                                                                                                     for (const auto& tp : pts)
                                                                                                                         m_touch.points.insert(tp.id(), tp.position());
                                                                                                                     if (m_touch.points.size() >= 2) {
                                                                                                                         m_touch.startCentroid = centroid(m_touch.points);
                                                                                                                         auto it = m_touch.points.constBegin();
                                                                                                                         const QPointF p0 = it.value(); ++it;
                                                                                                                         m_touch.startSpan = pointDist(p0, it.value());
                                                                                                                     }
                                                                                                                     break;
                                                                                                                 }
                                                                                                                 case QEvent::TouchUpdate: {
                                                                                                                     for (const auto& tp : pts)
                                                                                                                         m_touch.points.insert(tp.id(), tp.position());
                                                                                                                     processGestureUpdate();
                                                                                                                     break;
                                                                                                                 }
                                                                                                                 case QEvent::TouchEnd:
                                                                                                                 case QEvent::TouchCancel:
                                                                                                                     m_touch.points.clear();
                                                                                                                     m_touch.active    = GestureType::None;
                                                                                                                     m_touch.committed = false;
                                                                                                                     break;
                                                                                                                 default: break;
                                                                                                             }
                                                                                                         }

                                                                                                         void InputHandler::processGestureUpdate() {
                                                                                                             const int n = m_touch.points.size();
                                                                                                             if (n < 2) return;

                                                                                                             const QPointF cur = centroid(m_touch.points);

                                                                                                             // ── 2-finger pinch: adjust master ratio ──────────────────────────────
                                                                                                             if (n == 2) {
                                                                                                                 auto it = m_touch.points.constBegin();
                                                                                                                 const QPointF p0 = it.value(); ++it;
                                                                                                                 const float span = pointDist(p0, it.value());
                                                                                                                 if (m_touch.startSpan > 0.f) {
                                                                                                                     const float ratio = span / m_touch.startSpan;
                                                                                                                     if (std::abs(ratio - 1.f) > kPinchThreshold)
                                                                                                                         emit gestureDetected(GestureType::Pinch, ratio - 1.f);
                                                                                                                 }
                                                                                                                 return;
                                                                                                             }

                                                                                                             // ── 3-finger swipe: workspace switch (committed once per gesture) ─────
                                                                                                             if (n >= 3 && !m_touch.committed) {
                                                                                                                 const QPointF delta = cur - m_touch.startCentroid;
                                                                                                                 const float   ax    = std::abs(float(delta.x()));
                                                                                                                 const float   ay    = std::abs(float(delta.y()));

                                                                                                                 if (ax > kSwipeThresholdPx && ax > ay * 1.5f) {
                                                                                                                     // Horizontal swipe → switch workspace
                                                                                                                     m_touch.active    = delta.x() > 0
                                                                                                                     ? GestureType::SwipeRight   // fingers move right → prev ws
                                                                                                                     : GestureType::SwipeLeft;
                                                                                                                     m_touch.committed = true;
                                                                                                                     emit gestureDetected(m_touch.active, ax);
                                                                                                                     // Immediately trigger workspace action
                                                                                                                     if (m_touch.active == GestureType::SwipeRight)
                                                                                                                         emit actionTriggered("workspace:prev");
                                                                                                                     else
                                                                                                                         emit actionTriggered("workspace:next");
                                                                                                                 } else if (ay > kSwipeThresholdPx && ay > ax * 1.5f) {
                                                                                                                     // Vertical swipe → focus up/down
                                                                                                                     m_touch.active    = delta.y() > 0
                                                                                                                     ? GestureType::SwipeDown
                                                                                                                     : GestureType::SwipeUp;
                                                                                                                     m_touch.committed = true;
                                                                                                                     emit gestureDetected(m_touch.active, ay);
                                                                                                                     if (m_touch.active == GestureType::SwipeUp)
                                                                                                                         emit actionTriggered("focus:up");
                                                                                                                     else
                                                                                                                         emit actionTriggered("focus:down");
                                                                                                                 }
                                                                                                             }

                                                                                                             // ── 4-finger swipe: launch app launcher ──────────────────────────────
                                                                                                             if (n >= 4 && !m_touch.committed) {
                                                                                                                 const QPointF delta = cur - m_touch.startCentroid;
                                                                                                                 if (std::abs(float(delta.y())) > kSwipeThresholdPx * 0.8f) {
                                                                                                                     m_touch.committed = true;
                                                                                                                     emit actionTriggered("launcher");
                                                                                                                 }
                                                                                                             }
                                                                                                         }

                                                                                                         // ─────────────────────────────────────────────────────────────────────────────
                                                                                                         // Hit-testing
                                                                                                         // ─────────────────────────────────────────────────────────────────────────────

                                                                                                         Window* InputHandler::windowAt(const QPoint& pos) const {
                                                                                                             auto* ws = m_compositor->activeWorkspace();
                                                                                                             if (!ws) return nullptr;
                                                                                                             const auto wins = ws->visibleWindows();
                                                                                                             for (int i = wins.size()-1; i >= 0; --i)
                                                                                                                 if (wins[i]->isVisible() && wins[i]->geometry().contains(pos))
                                                                                                                     return wins[i];
                                                                                                             return nullptr;
                                                                                                         }

                                                                                                         bool InputHandler::inTitleBar(const QPoint& pos, const Window* w) const {
                                                                                                             return QRect(w->geometry().x(), w->geometry().y(),
                                                                                                                          w->geometry().width(), kTitleBarHeight).contains(pos);
                                                                                                         }

                                                                                                         Qt::Edges InputHandler::resizeEdgeAt(const QPoint& pos, const Window* w) const {
                                                                                                             const QRect g = w->geometry();
                                                                                                             const int d   = kResizeHandleWidth;
                                                                                                             Qt::Edges e;
                                                                                                             if (pos.y() < g.top()    + d) e |= Qt::TopEdge;
                                                                                                             if (pos.y() > g.bottom() - d) e |= Qt::BottomEdge;
                                                                                                             if (pos.x() < g.left()   + d) e |= Qt::LeftEdge;
                                                                                                             if (pos.x() > g.right()  - d) e |= Qt::RightEdge;
                                                                                                             return e;
                                                                                                         }

                                                                                                         InputHandler::TitleBarButton InputHandler::titleBarButtonAt(const QPoint& pos,
                                                                                                                                                                     const Window* w) const {
                                                                                                                                                                         const QRect g    = w->geometry();
                                                                                                                                                                         const int   dotY = g.y() + kTitleBarHeight/2;
                                                                                                                                                                         const int   dotX = g.right() - kDotRightMargin;
                                                                                                                                                                         if (QRect(dotX - 8, dotY-8, 16, 16).contains(pos))
                                                                                                                                                                             return TitleBarButton::Close;
                                                                                                                                                                         if (QRect(dotX - kDotSpacing - 8, dotY-8, 16, 16).contains(pos))
                                                                                                                                                                             return TitleBarButton::Maximize;
                                                                                                                                                                         if (QRect(dotX - kDotSpacing*2 - 8, dotY-8, 16, 16).contains(pos))
                                                                                                                                                                             return TitleBarButton::Minimize;
                                                                                                                                                                         return TitleBarButton::None;
                                                                                                                                                                     }

                                                                                                                                                                     QRect InputHandler::closeButtonRect(const Window* w) const {
                                                                                                                                                                         const QRect g = w->geometry();
                                                                                                                                                                         const int dotY = g.y() + kTitleBarHeight/2;
                                                                                                                                                                         const int dotX = g.right() - kDotRightMargin;
                                                                                                                                                                         return QRect(dotX-kDotRadius, dotY-kDotRadius, kDotRadius*2, kDotRadius*2);
                                                                                                                                                                     }
                                                                                                                                                                     QRect InputHandler::maximizeButtonRect(const Window* w) const {
                                                                                                                                                                         return closeButtonRect(w).translated(-kDotSpacing, 0);
                                                                                                                                                                     }
                                                                                                                                                                     QRect InputHandler::minimizeButtonRect(const Window* w) const {
                                                                                                                                                                         return closeButtonRect(w).translated(-kDotSpacing*2, 0);
                                                                                                                                                                     }

                                                                                                                                                                     // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                                                                     // Drag
                                                                                                                                                                     // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                                                                     void InputHandler::beginDrag(Window* w, const QPoint& cursorPos) {
                                                                                                                                                                         if (!w->isFloating()) emit actionTriggered("float");
                                                                                                                                                                         m_dragging      = true;
                                                                                                                                                                         m_dragWindow    = w;
                                                                                                                                                                         m_dragOffset    = cursorPos - w->geometry().topLeft();
                                                                                                                                                                         m_dragStartGeom = w->geometry();
                                                                                                                                                                         emit dragStarted(w, cursorPos);
                                                                                                                                                                     }

                                                                                                                                                                     void InputHandler::updateDrag(const QPoint& cursorPos) {
                                                                                                                                                                         if (!m_dragging || !m_dragWindow) return;
                                                                                                                                                                         QRect geom = m_dragWindow->geometry();
                                                                                                                                                                         geom.moveTopLeft(cursorPos - m_dragOffset);
                                                                                                                                                                         m_dragWindow->setGeometry(geom);
                                                                                                                                                                         emit dragUpdated(m_dragWindow, geom.topLeft());
                                                                                                                                                                     }

                                                                                                                                                                     void InputHandler::endDrag(const QPoint& cursorPos) {
                                                                                                                                                                         if (!m_dragging) return;
                                                                                                                                                                         emit dragFinished(m_dragWindow, cursorPos - m_dragOffset);
                                                                                                                                                                         m_dragging   = false;
                                                                                                                                                                         m_dragWindow = nullptr;
                                                                                                                                                                     }

                                                                                                                                                                     // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                                                                     // Resize
                                                                                                                                                                     // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                                                                     void InputHandler::beginResize(Window* w, const QPoint& cursorPos, Qt::Edges edges) {
                                                                                                                                                                         m_resizing           = true;
                                                                                                                                                                         m_resizeWindow       = w;
                                                                                                                                                                         m_resizeEdges        = edges;
                                                                                                                                                                         m_resizeStartCursor  = cursorPos;
                                                                                                                                                                         m_resizeStartGeom    = w->geometry();
                                                                                                                                                                         emit resizeStarted(w, edges, w->geometry());
                                                                                                                                                                     }

                                                                                                                                                                     void InputHandler::updateResize(const QPoint& cursorPos) {
                                                                                                                                                                         if (!m_resizing || !m_resizeWindow) return;
                                                                                                                                                                         const QPoint delta = cursorPos - m_resizeStartCursor;
                                                                                                                                                                         QRect geom         = m_resizeStartGeom;

                                                                                                                                                                         if (m_resizeEdges & Qt::RightEdge)  geom.setRight (geom.right()  + delta.x());
                                                                                                                                                                         if (m_resizeEdges & Qt::BottomEdge) geom.setBottom(geom.bottom() + delta.y());
                                                                                                                                                                         if (m_resizeEdges & Qt::LeftEdge)   geom.setLeft  (geom.left()   + delta.x());
                                                                                                                                                                         if (m_resizeEdges & Qt::TopEdge)    geom.setTop   (geom.top()    + delta.y());

                                                                                                                                                                         // Minimum window size guard
                                                                                                                                                                         if (geom.width()  < 80) geom.setWidth(80);
                                                                                                                                                                         if (geom.height() < 40) geom.setHeight(40);

                                                                                                                                                                         m_resizeWindow->setGeometry(geom);
                                                                                                                                                                         emit resizeUpdated(m_resizeWindow, geom);
                                                                                                                                                                     }

                                                                                                                                                                     void InputHandler::endResize(const QPoint& cursorPos) {
                                                                                                                                                                         if (!m_resizing) return;
                                                                                                                                                                         updateResize(cursorPos);
                                                                                                                                                                         emit resizeFinished(m_resizeWindow, m_resizeWindow->geometry());
                                                                                                                                                                         m_resizing      = false;
                                                                                                                                                                         m_resizeWindow  = nullptr;
                                                                                                                                                                         m_resizeEdges   = {};
                                                                                                                                                                     }

                                                                                                                                                                     // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                                                                     // Cursor shape
                                                                                                                                                                     // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                                                                     void InputHandler::updateCursorShape(const QPoint& pos) {
                                                                                                                                                                         Qt::CursorShape shape = Qt::ArrowCursor;
                                                                                                                                                                         Window* w = m_hoveredWindow;
                                                                                                                                                                         if (w) {
                                                                                                                                                                             if (inTitleBar(pos, w)) {
                                                                                                                                                                                 shape = Qt::SizeAllCursor;
                                                                                                                                                                             } else {
                                                                                                                                                                                 const Qt::Edges edges = resizeEdgeAt(pos, w);
                                                                                                                                                                                 shape = cursorShapeForEdges(edges);
                                                                                                                                                                             }
                                                                                                                                                                         }
                                                                                                                                                                         emit cursorShapeChanged(shape);
                                                                                                                                                                     }

                                                                                                                                                                     Qt::CursorShape InputHandler::cursorShapeForEdges(Qt::Edges edges) const {
                                                                                                                                                                         if ((edges & Qt::LeftEdge)  && (edges & Qt::TopEdge))    return Qt::SizeFDiagCursor;
                                                                                                                                                                         if ((edges & Qt::RightEdge) && (edges & Qt::BottomEdge)) return Qt::SizeFDiagCursor;
                                                                                                                                                                         if ((edges & Qt::RightEdge) && (edges & Qt::TopEdge))    return Qt::SizeBDiagCursor;
                                                                                                                                                                         if ((edges & Qt::LeftEdge)  && (edges & Qt::BottomEdge)) return Qt::SizeBDiagCursor;
                                                                                                                                                                         if (edges & (Qt::LeftEdge  | Qt::RightEdge))             return Qt::SizeHorCursor;
                                                                                                                                                                         if (edges & (Qt::TopEdge   | Qt::BottomEdge))            return Qt::SizeVerCursor;
                                                                                                                                                                         return Qt::ArrowCursor;
                                                                                                                                                                     }

                                                                                                                                                                     // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                                                                     // Wayland event forwarding
                                                                                                                                                                     // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                                                                     void InputHandler::forwardMouseButtonToSurface(Window* w,
                                                                                                                                                                                                                    const QPoint& globalPos,
                                                                                                                                                                                                                    Qt::MouseButton button,
                                                                                                                                                                                                                    bool pressed) {
                                                                                                                                                                         if (!w) return;
                                                                                                                                                                         auto* seat = m_compositor->defaultSeat();
                                                                                                                                                                         if (!seat) return;

                                                                                                                                                                         // Map global pos → surface local pos
                                                                                                                                                                         const QPoint localPos = globalPos - w->geometry().topLeft()
                                                                                                                                                                         - QPoint(0, kTitleBarHeight);
                                                                                                                                                                         seat->sendMouseMoveEvent(nullptr,
                                                                                                                                                                                                  QPointF(localPos.x(), localPos.y()), QPointF(globalPos));
                                                                                                                                                                         seat->sendMouseButtonEvent(button, pressed
                                                                                                                                                                         ? QWaylandSeat::Pressed : QWaylandSeat::Released);
                                                                                                                                                                                                                    }
