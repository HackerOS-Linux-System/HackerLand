#include "InputHandler.h"
#include "Config.h"
#include "Window.h"
#include "compositor/WMCompositor.h"
#include "compositor/WMSurface.h"    // full definition needed for ->view()
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
// Internal helpers — anonymous namespace
// ─────────────────────────────────────────────────────────────────────────────
namespace {

    /// Convert a Qt::Key value to the canonical name used in the keybind config.
    /// Returns an empty string for keys that cannot appear in bindings (e.g. bare
    /// modifier keys).
    QString keyName(int key) {
        // Modifier-only keys are never part of a binding — they are the prefix.
        if (key == Qt::Key_Shift    || key == Qt::Key_Control ||
            key == Qt::Key_Alt      || key == Qt::Key_Meta    ||
            key == Qt::Key_Super_L  || key == Qt::Key_Super_R ||
            key == Qt::Key_Hyper_L  || key == Qt::Key_Hyper_R ||
            key == Qt::Key_AltGr    || key == Qt::Key_CapsLock)
        {
            return {};
        }

        // Named special keys.
        switch (key) {
            case Qt::Key_Return:       return "Return";
            case Qt::Key_Enter:        return "Return";
            case Qt::Key_Space:        return "Space";
            case Qt::Key_Tab:          return "Tab";
            case Qt::Key_Backtab:      return "Tab";      // Shift+Tab
            case Qt::Key_Escape:       return "Escape";
            case Qt::Key_Backspace:    return "BackSpace";
            case Qt::Key_Delete:       return "Delete";
            case Qt::Key_Insert:       return "Insert";
            case Qt::Key_Home:         return "Home";
            case Qt::Key_End:          return "End";
            case Qt::Key_PageUp:       return "Prior";
            case Qt::Key_PageDown:     return "Next";
            case Qt::Key_Left:         return "Left";
            case Qt::Key_Right:        return "Right";
            case Qt::Key_Up:           return "Up";
            case Qt::Key_Down:         return "Down";
            case Qt::Key_Print:        return "Print";
            case Qt::Key_Pause:        return "Pause";
            case Qt::Key_F1:           return "F1";
            case Qt::Key_F2:           return "F2";
            case Qt::Key_F3:           return "F3";
            case Qt::Key_F4:           return "F4";
            case Qt::Key_F5:           return "F5";
            case Qt::Key_F6:           return "F6";
            case Qt::Key_F7:           return "F7";
            case Qt::Key_F8:           return "F8";
            case Qt::Key_F9:           return "F9";
            case Qt::Key_F10:          return "F10";
            case Qt::Key_F11:          return "F11";
            case Qt::Key_F12:          return "F12";
            case Qt::Key_Plus:         return "plus";
            case Qt::Key_Minus:        return "minus";
            case Qt::Key_Equal:        return "equal";
            case Qt::Key_BracketLeft:  return "bracketleft";
            case Qt::Key_BracketRight: return "bracketright";
            case Qt::Key_Semicolon:    return "semicolon";
            case Qt::Key_Apostrophe:   return "apostrophe";
            case Qt::Key_Comma:        return "comma";
            case Qt::Key_Period:       return "period";
            case Qt::Key_Slash:        return "slash";
            case Qt::Key_Backslash:    return "backslash";
            case Qt::Key_QuoteLeft:    return "grave";
            default: break;
        }

        // Single printable character — lower-case letter or digit.
        if (key >= Qt::Key_A && key <= Qt::Key_Z) {
            return QString(QChar(key)).toLower();
        }
        if (key >= Qt::Key_0 && key <= Qt::Key_9) {
            return QString(QChar(key));
        }

        return {};
    }

    /// Build a sorted, canonical modifier prefix string from Qt modifier flags.
    /// Canonical order: Super > Ctrl > Alt > Shift
    /// e.g. Qt::ControlModifier | Qt::ShiftModifier → "Ctrl+Shift+"
    QString modPrefix(Qt::KeyboardModifiers mods) {
        QString s;
        if (mods & Qt::MetaModifier)    s += "Super+";
        if (mods & Qt::ControlModifier) s += "Ctrl+";
        if (mods & Qt::AltModifier)     s += "Alt+";
        if (mods & Qt::ShiftModifier)   s += "Shift+";
        return s;
    }

    /// Euclidean distance between two QPointF values.
    float pointDist(const QPointF& a, const QPointF& b) {
        const float dx = a.x() - b.x();
        const float dy = a.y() - b.y();
        return std::sqrt(dx * dx + dy * dy);
    }

    /// Centroid of a set of points.
    QPointF centroid(const QHash<int, QPointF>& pts) {
        if (pts.isEmpty()) return {};
        QPointF sum;
        for (const auto& p : pts) sum += p;
        return sum / pts.size();
    }

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

InputHandler::InputHandler(WMCompositor* compositor, QObject* parent)
: QObject(parent)
, m_compositor(compositor)
{
    Q_ASSERT(compositor);

    // Connect to config reload so keybindings stay in sync.
    connect(&Config::instance(), &Config::configReloaded,
            this, &InputHandler::reloadBindings);

    qDebug() << "[InputHandler] created";
}

InputHandler::~InputHandler() = default;

void InputHandler::reloadBindings() {
    // Bindings are read on-demand from Config::instance().keys.bindings, so
    // there is nothing to cache.  Just log the reload.
    qDebug() << "[InputHandler] keybindings reloaded ("
    << Config::instance().keys.bindings.size() << "entries)";
}

// ─────────────────────────────────────────────────────────────────────────────
// Keyboard — key press
// ─────────────────────────────────────────────────────────────────────────────

bool InputHandler::handleKeyPress(QKeyEvent* event) {
    Q_ASSERT(event);

    // Update modifier tracking.
    m_modifiers = event->modifiers();
    m_pressedKeys.insert(event->key());

    // Ignore auto-repeat for keybinds to avoid action spam.
    if (event->isAutoRepeat()) {
        // Forward repeating key to surface for things like held cursor keys.
        forwardKeyToSurface(event);
        return false;
    }

    // ── Try to match a keybind ────────────────────────────────────────────
    const QString bindKey = buildBindingKey(event);
    if (!bindKey.isEmpty()) {
        KeybindMatch match = matchBinding(bindKey);
        if (match.matched) {
            m_consumedKeys.insert(event->key());
            qDebug() << "[InputHandler] keybind matched:"
            << bindKey << "->" << match.action;
            emit actionTriggered(match.action);
            return true; // consumed — do not forward to surface
        }
    }

    // ── Not a keybind: forward to focused Wayland surface ────────────────
    forwardKeyToSurface(event);
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Keyboard — key release
// ─────────────────────────────────────────────────────────────────────────────

bool InputHandler::handleKeyRelease(QKeyEvent* event) {
    Q_ASSERT(event);

    m_modifiers = event->modifiers();

    // If the key was consumed on press, swallow the release too so the
    // focused application doesn't see a dangling key-up.
    bool consumed = m_consumedKeys.remove(event->key());
    m_pressedKeys.remove(event->key());

    if (!consumed && !event->isAutoRepeat()) {
        forwardKeyToSurface(event);
    }

    return consumed;
}

// ─────────────────────────────────────────────────────────────────────────────
// Keyboard — build binding key / match
// ─────────────────────────────────────────────────────────────────────────────

QString InputHandler::buildBindingKey(QKeyEvent* event) const {
    const QString name = keyName(event->key());
    if (name.isEmpty()) return {};

    // Derive modifiers from the event rather than m_modifiers because the
    // event carries the snapshot at the moment of press, which is what the
    // user configured in their keybind file.
    Qt::KeyboardModifiers mods = event->modifiers();

    // Special-case: if the key itself is a numpad digit or a shifted symbol,
    // strip Shift from the modifiers so "Shift+1" and "exclam" can coexist.
    if (event->key() >= Qt::Key_Exclam && event->key() <= Qt::Key_At) {
        mods &= ~Qt::ShiftModifier;
    }

    return modPrefix(mods) + name;
}

KeybindMatch InputHandler::matchBinding(const QString& bindingKey) const {
    const auto& bindings = Config::instance().keys.bindings;

    auto it = bindings.constFind(bindingKey);
    if (it != bindings.constEnd()) {
        return { true, it.value() };
    }

    return { false, {} };
}

void InputHandler::forwardKeyToSurface(QKeyEvent* event) const {
    if (!m_compositor) return;

    QWaylandSeat* seat = m_compositor->defaultSeat();
    if (!seat) return;

    QWaylandSurface* focused = seat->keyboardFocus();
    if (!focused) return;

    // QWaylandSeat handles the actual Wayland keyboard event protocol.
    if (event->type() == QEvent::KeyPress) {
        seat->sendKeyPressEvent(event->nativeScanCode());
    } else {
        seat->sendKeyReleaseEvent(event->nativeScanCode());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Pointer — mouse move
// ─────────────────────────────────────────────────────────────────────────────

void InputHandler::handleMouseMove(const QPoint& globalPos,
                                   Qt::MouseButtons buttons)
{
    m_cursorPos   = globalPos;
    m_heldButtons = buttons;

    // ── Update drag ───────────────────────────────────────────────────────
    if (m_dragging) {
        updateDrag(globalPos);
        updateCursorShape(globalPos);
        return;
    }

    // ── Update resize ─────────────────────────────────────────────────────
    if (m_resizing) {
        updateResize(globalPos);
        updateCursorShape(globalPos);
        return;
    }

    // ── Update hover and cursor shape ─────────────────────────────────────
    Window* prevHovered = m_hoveredWindow;
    m_hoveredWindow = windowAt(globalPos);
    if (m_hoveredWindow != prevHovered) {
        // Could emit a hoverChanged signal here if needed.
    }

    updateCursorShape(globalPos);

    // ── Forward to Wayland surface under cursor ───────────────────────────
    if (m_hoveredWindow && m_hoveredWindow->surface()) {
        QWaylandSeat* seat = m_compositor
        ? m_compositor->defaultSeat() : nullptr;
        if (seat) {
            // Qt6: sendMouseMoveEvent(QWaylandView*, QPointF local, QPointF global)
            const QPoint local = globalPos - m_hoveredWindow->geometry().topLeft();
            seat->sendMouseMoveEvent(m_hoveredWindow->surface()->view(),
                                     QPointF(local), QPointF(globalPos));
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Pointer — mouse press
// ─────────────────────────────────────────────────────────────────────────────

void InputHandler::handleMousePress(const QPoint& globalPos,
                                    Qt::MouseButton button,
                                    Qt::KeyboardModifiers modifiers)
{
    m_modifiers = modifiers;

    Window* w = windowAt(globalPos);

    // ── Focus on click ────────────────────────────────────────────────────
    if (w) {
        emit windowFocusRequested(w);
    }

    if (!w || button != Qt::LeftButton) {
        // Right-click on desktop or non-left button: forward to surface.
        if (w && w->surface()) {
            forwardMouseButtonToSurface(w, globalPos, button, true);
        }
        return;
    }

    // ── Title-bar button hit-test ─────────────────────────────────────────
    if (inTitleBar(globalPos, w)) {
        TitleBarButton btn = titleBarButtonAt(globalPos, w);
        switch (btn) {
            case TitleBarButton::Close:
                emit closeButtonClicked(w);
                return;
            case TitleBarButton::Maximize:
                emit maximizeButtonClicked(w);
                return;
            case TitleBarButton::Minimize:
                emit minimizeButtonClicked(w);
                return;
            case TitleBarButton::None:
                // Click in title bar but not on a button — start drag if
                // the window can be moved (floating or will become floating).
                beginDrag(w, globalPos);
                return;
        }
    }

    // ── Edge resize hit-test ──────────────────────────────────────────────
    Qt::Edges edges = resizeEdgeAt(globalPos, w);
    if (edges) {
        beginResize(w, globalPos, edges);
        return;
    }

    // ── Client area click — forward to surface ────────────────────────────
    forwardMouseButtonToSurface(w, globalPos, button, true);
}

// ─────────────────────────────────────────────────────────────────────────────
// Pointer — mouse release
// ─────────────────────────────────────────────────────────────────────────────

void InputHandler::handleMouseRelease(const QPoint& globalPos,
                                      Qt::MouseButton button,
                                      Qt::KeyboardModifiers modifiers)
{
    m_modifiers = modifiers;

    if (button == Qt::LeftButton) {
        if (m_dragging) {
            endDrag(globalPos);
            return;
        }
        if (m_resizing) {
            endResize(globalPos);
            return;
        }
    }

    // Forward release to the surface that received the press.
    Window* w = windowAt(globalPos);
    if (w && w->surface()) {
        forwardMouseButtonToSurface(w, globalPos, button, false);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Pointer — wheel
// ─────────────────────────────────────────────────────────────────────────────

void InputHandler::handleWheel(const QPoint& globalPos,
                               const QPointF& angleDelta,
                               Qt::KeyboardModifiers modifiers)
{
    m_modifiers = modifiers;

    // Super + scroll → cycle workspaces.
    if (modifiers & Qt::MetaModifier) {
        if (angleDelta.y() > 0) {
            emit actionTriggered("workspace:prev");
        } else if (angleDelta.y() < 0) {
            emit actionTriggered("workspace:next");
        }
        return;
    }

    // Super + horizontal scroll → adjust master ratio.
    if ((modifiers & Qt::MetaModifier) && angleDelta.x() != 0) {
        const float delta = (angleDelta.x() > 0) ? 0.03f : -0.03f;
        emit actionTriggered(QString("master_ratio:%1").arg(delta));
        return;
    }

    // Otherwise forward scroll to the window under the cursor.
    Window* w = windowAt(globalPos);
    if (!w || !w->surface()) return;

    QWaylandSeat* seat = m_compositor ? m_compositor->defaultSeat() : nullptr;
    if (!seat) return;

    // Qt6: sendMouseWheelEvent(Qt::Orientation, int) — no view argument.
    // The seat already knows which surface has pointer focus.
    if (angleDelta.y() != 0)
        seat->sendMouseWheelEvent(Qt::Vertical,   static_cast<int>(angleDelta.y()));
    if (angleDelta.x() != 0)
        seat->sendMouseWheelEvent(Qt::Horizontal, static_cast<int>(angleDelta.x()));
}

// ─────────────────────────────────────────────────────────────────────────────
// Touch / gesture (stub)
// ─────────────────────────────────────────────────────────────────────────────

void InputHandler::handleTouch(QTouchEvent* event) {
    if (!event) return;

    const auto& points = event->points();

    switch (event->type()) {
        case QEvent::TouchBegin: {
            m_touch.points.clear();
            m_touch.active = GestureType::None;
            for (const auto& tp : points) {
                m_touch.points.insert(tp.id(), tp.position());
            }
            if (m_touch.points.size() >= 2) {
                m_touch.startCentroid = centroid(m_touch.points);
                // Compute initial span (distance between first two points).
                auto it = m_touch.points.constBegin();
                const QPointF p0 = it.value(); ++it;
                const QPointF p1 = it.value();
                m_touch.startSpan = pointDist(p0, p1);
            }
            break;
        }
        case QEvent::TouchUpdate: {
            for (const auto& tp : points) {
                m_touch.points.insert(tp.id(), tp.position());
            }
            processGestureUpdate();
            break;
        }
        case QEvent::TouchEnd:
        case QEvent::TouchCancel: {
            m_touch.points.clear();
            m_touch.active = GestureType::None;
            break;
        }
        default: break;
    }
}

void InputHandler::processGestureUpdate() {
    const int nPoints = m_touch.points.size();
    if (nPoints < 2) return;

    const QPointF curCentroid = centroid(m_touch.points);

    if (nPoints == 2) {
        // ── 2-finger pinch ────────────────────────────────────────────────
        auto it = m_touch.points.constBegin();
        const QPointF p0 = it.value(); ++it;
        const QPointF p1 = it.value();
        const float curSpan = pointDist(p0, p1);

        if (m_touch.startSpan > 0.f) {
            const float ratio = curSpan / m_touch.startSpan;
            if (std::abs(ratio - 1.f) > kPinchThreshold) {
                emit gestureDetected(GestureType::Pinch, ratio - 1.f);
            }
        }
    }

    if (nPoints >= 3) {
        // ── 3-finger swipe ────────────────────────────────────────────────
        const QPointF delta = curCentroid - m_touch.startCentroid;

        if (m_touch.active == GestureType::None) {
            if (std::abs(delta.x()) > kSwipeThresholdPx) {
                m_touch.active = delta.x() > 0
                ? GestureType::SwipeRight
                : GestureType::SwipeLeft;
                emit gestureDetected(m_touch.active,
                                     static_cast<float>(std::abs(delta.x())));
            } else if (std::abs(delta.y()) > kSwipeThresholdPx) {
                m_touch.active = delta.y() > 0
                ? GestureType::SwipeDown
                : GestureType::SwipeUp;
                emit gestureDetected(m_touch.active,
                                     static_cast<float>(std::abs(delta.y())));
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Drag
// ─────────────────────────────────────────────────────────────────────────────

void InputHandler::beginDrag(Window* w, const QPoint& cursorPos) {
    Q_ASSERT(w);

    // Only tiled windows need to be promoted to floating before dragging.
    // Floating windows can be dragged immediately.
    if (!w->isFloating()) {
        emit actionTriggered(QString("float_window:%1").arg(w->id()));
    }

    m_dragging       = true;
    m_dragWindow     = w;
    m_dragOffset     = cursorPos - w->geometry().topLeft();
    m_dragStartGeom  = w->geometry();

    qDebug() << "[InputHandler] drag started:" << w->title()
    << "offset:" << m_dragOffset;

    emit dragStarted(w, cursorPos);
}

void InputHandler::updateDrag(const QPoint& cursorPos) {
    if (!m_dragWindow) return;

    const QPoint newTopLeft = cursorPos - m_dragOffset;
    emit dragUpdated(m_dragWindow, newTopLeft);
}

void InputHandler::endDrag(const QPoint& cursorPos) {
    if (!m_dragWindow) return;

    const QPoint finalTopLeft = cursorPos - m_dragOffset;

    qDebug() << "[InputHandler] drag finished:" << m_dragWindow->title()
    << "at:" << finalTopLeft;

    emit dragFinished(m_dragWindow, finalTopLeft);

    m_dragging   = false;
    m_dragWindow = nullptr;
    m_dragOffset = {};
}

// ─────────────────────────────────────────────────────────────────────────────
// Resize
// ─────────────────────────────────────────────────────────────────────────────

void InputHandler::beginResize(Window* w, const QPoint& cursorPos,
                               Qt::Edges edges)
{
    Q_ASSERT(w);

    // Same float-promotion logic as drag.
    if (!w->isFloating() && !w->isMaximized()) {
        emit actionTriggered(QString("float_window:%1").arg(w->id()));
    }

    m_resizing           = true;
    m_resizeWindow       = w;
    m_resizeEdges        = edges;
    m_resizeStartCursor  = cursorPos;
    m_resizeStartGeom    = w->geometry();

    qDebug() << "[InputHandler] resize started:" << w->title()
    << "edges:" << (int)edges;

    emit resizeStarted(w, edges, m_resizeStartGeom);
}

void InputHandler::updateResize(const QPoint& cursorPos) {
    if (!m_resizeWindow) return;

    const QPoint delta = cursorPos - m_resizeStartCursor;
    QRect geom = m_resizeStartGeom;

    // Adjust the appropriate edges based on which ones are being dragged.
    if (m_resizeEdges & Qt::RightEdge) {
        geom.setRight(m_resizeStartGeom.right() + delta.x());
    }
    if (m_resizeEdges & Qt::BottomEdge) {
        geom.setBottom(m_resizeStartGeom.bottom() + delta.y());
    }
    if (m_resizeEdges & Qt::LeftEdge) {
        geom.setLeft(m_resizeStartGeom.left() + delta.x());
    }
    if (m_resizeEdges & Qt::TopEdge) {
        geom.setTop(m_resizeStartGeom.top() + delta.y());
    }

    // Enforce minimum size so the window can never be dragged inside-out.
    const QSize minSz = m_resizeWindow->minSize();
    if (geom.width()  < minSz.width())  geom.setWidth(minSz.width());
    if (geom.height() < minSz.height()) geom.setHeight(minSz.height());

    emit resizeUpdated(m_resizeWindow, geom);
}

void InputHandler::endResize(const QPoint& cursorPos) {
    if (!m_resizeWindow) return;

    // Run one final update to make sure the geometry is consistent.
    updateResize(cursorPos);

    qDebug() << "[InputHandler] resize finished:" << m_resizeWindow->title()
    << "geom:" << m_resizeWindow->geometry();

    emit resizeFinished(m_resizeWindow, m_resizeWindow->geometry());

    m_resizing      = false;
    m_resizeWindow  = nullptr;
    m_resizeEdges   = {};
}

// ─────────────────────────────────────────────────────────────────────────────
// Hit-testing
// ─────────────────────────────────────────────────────────────────────────────

Window* InputHandler::windowAt(const QPoint& pos) const {
    if (!m_compositor) return nullptr;

    // Iterate windows in reverse paint order (topmost first).
    // WMCompositor exposes allWindows() in bottom-to-top order.
    const QList<Window*> wins = m_compositor->allWindows();
    for (int i = wins.size() - 1; i >= 0; --i) {
        Window* w = wins[i];
        if (!w->isVisible()) continue;
        if (w->geometry().contains(pos)) return w;
    }
    return nullptr;
}

bool InputHandler::inTitleBar(const QPoint& pos, const Window* w) const {
    Q_ASSERT(w);
    const QRect& geom = w->geometry();
    const QRect titleBarRect(geom.x(), geom.y(), geom.width(), kTitleBarHeight);
    return titleBarRect.contains(pos);
}

Qt::Edges InputHandler::resizeEdgeAt(const QPoint& pos, const Window* w) const {
    Q_ASSERT(w);
    if (!w->isFloating() && !w->isMaximized()) return {};

    const QRect& g = w->geometry();
    const int hw = kResizeHandleWidth;

    Qt::Edges edges;

    const bool onLeft   = pos.x() >= g.left()  && pos.x() < g.left()  + hw;
    const bool onRight  = pos.x() >  g.right()  - hw && pos.x() <= g.right();
    const bool onTop    = pos.y() >= g.top()    && pos.y() < g.top()   + hw;
    const bool onBottom = pos.y() >  g.bottom() - hw && pos.y() <= g.bottom();

    if (onLeft)   edges |= Qt::LeftEdge;
    if (onRight)  edges |= Qt::RightEdge;
    if (onTop)    edges |= Qt::TopEdge;
    if (onBottom) edges |= Qt::BottomEdge;

    return edges;
}

// ─────────────────────────────────────────────────────────────────────────────
// Title-bar button geometry
// ─────────────────────────────────────────────────────────────────────────────

InputHandler::TitleBarButton
InputHandler::titleBarButtonAt(const QPoint& pos, const Window* w) const {
    if (closeButtonRect(w).contains(pos))    return TitleBarButton::Close;
    if (maximizeButtonRect(w).contains(pos)) return TitleBarButton::Maximize;
    if (minimizeButtonRect(w).contains(pos)) return TitleBarButton::Minimize;
    return TitleBarButton::None;
}

QRect InputHandler::closeButtonRect(const Window* w) const {
    // Three traffic-light dots, right-aligned in the title bar.
    // Close = rightmost, Maximize = middle, Minimize = leftmost.
    const QRect& g = w->geometry();
    const int cy = g.top() + kTitleBarHeight / 2;
    const int cx = g.right() - kDotRightMargin;
    return QRect(cx - kDotRadius, cy - kDotRadius,
                 kDotRadius * 2, kDotRadius * 2);
}

QRect InputHandler::maximizeButtonRect(const Window* w) const {
    const QRect& g = w->geometry();
    const int cy = g.top() + kTitleBarHeight / 2;
    const int cx = g.right() - kDotRightMargin - kDotSpacing;
    return QRect(cx - kDotRadius, cy - kDotRadius,
                 kDotRadius * 2, kDotRadius * 2);
}

QRect InputHandler::minimizeButtonRect(const Window* w) const {
    const QRect& g = w->geometry();
    const int cy = g.top() + kTitleBarHeight / 2;
    const int cx = g.right() - kDotRightMargin - kDotSpacing * 2;
    return QRect(cx - kDotRadius, cy - kDotRadius,
                 kDotRadius * 2, kDotRadius * 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Cursor shape
// ─────────────────────────────────────────────────────────────────────────────

void InputHandler::updateCursorShape(const QPoint& pos) {
    Qt::CursorShape shape = Qt::ArrowCursor;

    if (m_dragging) {
        shape = Qt::SizeAllCursor;
    } else if (m_resizing) {
        shape = cursorShapeForEdges(m_resizeEdges);
    } else if (m_hoveredWindow) {
        const Qt::Edges edges = resizeEdgeAt(pos, m_hoveredWindow);
        if (edges) {
            shape = cursorShapeForEdges(edges);
        } else if (inTitleBar(pos, m_hoveredWindow)) {
            TitleBarButton btn = titleBarButtonAt(pos, m_hoveredWindow);
            shape = (btn == TitleBarButton::None)
            ? Qt::OpenHandCursor
            : Qt::PointingHandCursor;
        }
    }

    emit cursorShapeChanged(shape);
}

Qt::CursorShape InputHandler::cursorShapeForEdges(Qt::Edges edges) const {
    // Diagonal corners.
    if ((edges & Qt::TopEdge)    && (edges & Qt::LeftEdge))  return Qt::SizeFDiagCursor;
    if ((edges & Qt::TopEdge)    && (edges & Qt::RightEdge)) return Qt::SizeBDiagCursor;
    if ((edges & Qt::BottomEdge) && (edges & Qt::LeftEdge))  return Qt::SizeBDiagCursor;
    if ((edges & Qt::BottomEdge) && (edges & Qt::RightEdge)) return Qt::SizeFDiagCursor;
    // Single edges.
    if (edges & (Qt::LeftEdge | Qt::RightEdge)) return Qt::SizeHorCursor;
    if (edges & (Qt::TopEdge  | Qt::BottomEdge)) return Qt::SizeVerCursor;
    return Qt::ArrowCursor;
}

// ─────────────────────────────────────────────────────────────────────────────
// Forward mouse button to Wayland surface
// ─────────────────────────────────────────────────────────────────────────────

void InputHandler::forwardMouseButtonToSurface(Window*          w,
                                               const QPoint&   globalPos,
                                               Qt::MouseButton button,
                                               bool            pressed)
{
    if (!w || !w->surface() || !m_compositor) return;

    QWaylandSeat* seat = m_compositor->defaultSeat();
    if (!seat) return;

    const QPoint local = globalPos - w->geometry().topLeft();

    if (pressed) {
        seat->sendMousePressEvent(button);
    } else {
        seat->sendMouseReleaseEvent(button);
    }
    Q_UNUSED(local);
}
