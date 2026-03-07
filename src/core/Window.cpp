#include "Window.h"
#include "compositor/WMSurface.h"
#include "core/Config.h"

#include <QGuiApplication>
#include <QScreen>
#include <QIcon>
#include <QDebug>

// ─────────────────────────────────────────────────────────────────────────────
// Static member
// ─────────────────────────────────────────────────────────────────────────────

uint64_t Window::s_nextId = 1;

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers — file-local
// ─────────────────────────────────────────────────────────────────────────────
namespace {

    /// Human-readable name for a WindowState value (used in debug output only).
    const char* stateStr(WindowState s) {
        switch (s) {
            case WindowState::Normal:     return "Normal";
            case WindowState::Maximized:  return "Maximized";
            case WindowState::Fullscreen: return "Fullscreen";
            case WindowState::Minimized:  return "Minimized";
            case WindowState::Floating:   return "Floating";
            case WindowState::Tiled:      return "Tiled";
            case WindowState::Monocle:    return "Monocle";
        }
        return "?";
    }

    /// Compute a sensible default floating geometry centred on the primary screen.
    QRect defaultFloatRect() {
        QScreen* scr = QGuiApplication::primaryScreen();
        if (!scr) return { 200, 200, 900, 650 };

        const QRect avail = scr->availableGeometry();
        const int w = qMin(900, (int)(avail.width()  * 0.55));
        const int h = qMin(700, (int)(avail.height() * 0.62));
        const int x = avail.x() + (avail.width()  - w) / 2;
        const int y = avail.y() + (avail.height() - h) / 2;
        return { x, y, w, h };
    }

    /// Clamp rect dimensions to [minSz, maxSz] keeping the top-left fixed.
    QRect clampSize(const QRect& rect, const QSize& minSz, const QSize& maxSz) {
        const int w = qBound(minSz.width(),  rect.width(),  maxSz.width());
        const int h = qBound(minSz.height(), rect.height(), maxSz.height());
        return { rect.topLeft(), QSize(w, h) };
    }

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

Window::Window(WMSurface* surface, QObject* parent)
: QObject(parent)
, m_id(s_nextId++)
, m_surface(surface)
{
    // Windows start inactive; set opacity accordingly.
    m_opacity = Config::instance().theme.inactiveOpacity;

    qDebug() << "[Window" << m_id << "] created";
}

Window::~Window() {
    qDebug() << "[Window" << m_id << "] destroyed:"
    << (m_title.isEmpty() ? m_appId : m_title);
}

// ─────────────────────────────────────────────────────────────────────────────
// Identity
// ─────────────────────────────────────────────────────────────────────────────

void Window::setTitle(const QString& t) {
    if (m_title == t) return;
    m_title = t;
    qDebug() << "[Window" << m_id << "] title:" << t;
    emit titleChanged(t);
}

void Window::setAppId(const QString& id) {
    if (m_appId == id) return;
    m_appId = id;

    // Derive a plain class name from the appId.
    // "org.gnome.Nautilus" → "nautilus"
    // "firefox"            → "firefox"
    QString cls = id;
    const int dot = cls.lastIndexOf('.');
    if (dot >= 0) cls = cls.mid(dot + 1);
    m_className = cls.toLower();

    // Icon resolution: try several variants from most to least specific.
    m_icon = QIcon::fromTheme(id);
    if (m_icon.isNull()) m_icon = QIcon::fromTheme(id.toLower());
    if (m_icon.isNull()) m_icon = QIcon::fromTheme(m_className);
    // Common fallbacks so the title bar always has something to paint.
    if (m_icon.isNull()) m_icon = QIcon::fromTheme("application-x-executable");
    if (m_icon.isNull()) m_icon = QIcon::fromTheme("utilities-terminal");

    qDebug() << "[Window" << m_id << "] appId:" << id
    << "class:" << m_className
    << "icon:" << (!m_icon.isNull() ? "ok" : "missing");
}

// ─────────────────────────────────────────────────────────────────────────────
// Active / focus
// ─────────────────────────────────────────────────────────────────────────────

void Window::setActive(bool active) {
    if (m_active == active) return;
    m_active  = active;
    m_opacity = active
    ? Config::instance().theme.activeOpacity
    : Config::instance().theme.inactiveOpacity;

    qDebug() << "[Window" << m_id << "]"
    << (active ? "activated" : "deactivated");

    emit activeChanged(active);
}

void Window::focus() {
    // The compositor reacts to this signal and calls setActive(true) after
    // updating the previously-active window.
    emit focusRequested();
}

// ─────────────────────────────────────────────────────────────────────────────
// Geometry
// ─────────────────────────────────────────────────────────────────────────────

void Window::setGeometry(const QRect& rect) {
    // Enforce min / max size constraints before storing.
    const QRect clamped = clampSize(rect, m_minSize, m_maxSize);

    if (m_geometry == clamped) return;
    m_geometry = clamped;

    qDebug() << "[Window" << m_id << "] geometry:"
    << m_geometry.x() << m_geometry.y()
    << m_geometry.width() << "x" << m_geometry.height();

    emit geometryChanged(m_geometry);
}

void Window::setGeometryAnimated(const QRect& rect, int durationMs) {
    // AnimationEngine drives the actual interpolation.  This method just
    // records the final target and (optionally) the duration, then sets the
    // geometry immediately if animations are disabled.
    //
    // When animations are enabled the AnimationEngine listens to
    // geometryChanged() and smoothly interpolates the window to rect over
    // durationMs milliseconds.  For this to work we must NOT call setGeometry()
    // with the final rect right away — we emit a dedicated signal instead so
    // the engine can start the animation.  If no engine is wired up (unit
    // test / headless mode) we fall through to the immediate path.

    const QRect target = clampSize(rect, m_minSize, m_maxSize);

    if (!Config::instance().animationsEnabled() || durationMs == 0) {
        // Animations disabled: teleport immediately.
        setGeometry(target);
        return;
    }

    // Animations enabled: record progress as not-yet-started.
    m_animProgress = 0.0f;

    // Set geometry directly so the window model is consistent; the
    // AnimationEngine overrides this with interpolated frames.
    if (m_geometry == target) return;
    m_geometry = target;

    qDebug() << "[Window" << m_id << "] animated move to:"
    << target << "over" << durationMs << "ms";

    emit geometryChanged(m_geometry);
}

// ─────────────────────────────────────────────────────────────────────────────
// State
// ─────────────────────────────────────────────────────────────────────────────

void Window::setState(WindowState state) {
    if (m_state == state) return;

    const WindowState prev = m_state;
    m_state = state;

    qDebug() << "[Window" << m_id << "] state:"
    << stateStr(prev) << "->" << stateStr(state);

    // When leaving Fullscreen or Maximized back to a non-special state,
    // restore the pre-transition geometry so the tiling engine has the
    // correct starting point for its next pass.
    const bool leavingExpanded =
    (prev == WindowState::Fullscreen || prev == WindowState::Maximized);
    const bool returningToNormal =
    (state == WindowState::Tiled   ||
    state == WindowState::Floating ||
    state == WindowState::Normal);

    if (leavingExpanded && returningToNormal) {
        if (m_savedGeometry.isValid() && !m_savedGeometry.isEmpty()) {
            // Bypass the clamping in setGeometry() deliberately — the saved
            // geometry was already valid when it was saved.
            if (m_geometry != m_savedGeometry) {
                m_geometry = m_savedGeometry;
                emit geometryChanged(m_geometry);
            }
        }
    }

    emit stateChanged(state);
}

void Window::setWorkspace(int id) {
    if (m_workspaceId == id) return;
    m_workspaceId = id;
    qDebug() << "[Window" << m_id << "] workspace:" << id;
    emit workspaceChanged(id);
}

void Window::setPinned(bool pinned) {
    // Pinned windows appear on every workspace.  The flag is read by
    // WMCompositor when switching workspaces.
    m_pinned = pinned;
    qDebug() << "[Window" << m_id << "] pinned:" << pinned;
}

void Window::setVisible(bool visible) {
    // No early-return guard: callers may legitimately set the same value
    // when making a workspace visible after a switch; they rely on the
    // window actually updating its paint state.
    m_visible = visible;
    qDebug() << "[Window" << m_id << "] visible:" << visible;
}

void Window::setOpacity(float opacity) {
    m_opacity = qBound(0.0f, opacity, 1.0f);
}

// ─────────────────────────────────────────────────────────────────────────────
// Toggle actions
// ─────────────────────────────────────────────────────────────────────────────

void Window::toggleFloat() {
    if (m_state == WindowState::Floating) {
        // ── Returning to tiled ────────────────────────────────────────────
        // Restore saved geometry so the tiling engine starts from a sane rect.
        // If no saved geometry exists (shouldn't happen, but be defensive),
        // the engine will compute an appropriate one on the next retile pass.
        if (m_savedGeometry.isValid() && !m_savedGeometry.isEmpty()) {
            if (m_geometry != m_savedGeometry) {
                m_geometry = m_savedGeometry;
                emit geometryChanged(m_geometry);
            }
        }
        setState(WindowState::Tiled);
    } else {
        // ── Entering floating ─────────────────────────────────────────────
        m_savedGeometry = m_geometry;

        // Give the window a sensible floating size if it has never been given
        // real geometry (e.g. it was created directly as a floating window).
        if (m_geometry.isEmpty() || !m_geometry.isValid()) {
            m_geometry = defaultFloatRect();
            emit geometryChanged(m_geometry);
        }

        setState(WindowState::Floating);
    }
}

void Window::toggleMaximize() {
    if (m_state == WindowState::Maximized) {
        // setState() will restore m_savedGeometry if it is valid.
        setState(WindowState::Tiled);
    } else {
        m_savedGeometry = m_geometry;
        setState(WindowState::Maximized);
        // WMXdgShell / WMCompositor will send a configure with the work-area
        // size; we do not set geometry here because the compositor may decide
        // to use a different output or account for layer-shell panels.
    }
}

void Window::toggleFullscreen() {
    if (m_state == WindowState::Fullscreen) {
        setState(WindowState::Tiled);
    } else {
        m_savedGeometry = m_geometry;
        setState(WindowState::Fullscreen);
        // As with maximize, geometry is set by the compositor after this
        // state change.
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Close
// ─────────────────────────────────────────────────────────────────────────────

void Window::close() {
    qDebug() << "[Window" << m_id << "] close():"
    << (m_title.isEmpty() ? m_appId : m_title);
    // WMXdgShell listens to this signal and calls
    // QWaylandXdgToplevel::sendClose() on the protocol side.
    emit closeRequested();
}

// ─────────────────────────────────────────────────────────────────────────────
// Size constraints
// ─────────────────────────────────────────────────────────────────────────────

void Window::setMinSize(const QSize& sz) {
    if (m_minSize == sz) return;
    m_minSize = sz;
}

void Window::setMaxSize(const QSize& sz) {
    if (m_maxSize == sz) return;
    m_maxSize = sz;
}

// ─────────────────────────────────────────────────────────────────────────────
// Animation progress
// ─────────────────────────────────────────────────────────────────────────────

void Window::setAnimProgress(float p) {
    m_animProgress = qBound(0.0f, p, 1.0f);
    // No signal needed — AnimationEngine drives repaints itself.
}

// ─────────────────────────────────────────────────────────────────────────────
// Constraints helper
// ─────────────────────────────────────────────────────────────────────────────

QRect Window::clampToConstraints(const QRect& rect) const {
    return clampSize(rect, m_minSize, m_maxSize);
}
