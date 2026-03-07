#include "WMSurface.h"
#include "WMCompositor.h"
#include "core/Window.h"

#include <QWaylandSurface>
#include <QWaylandView>
#include <QWaylandBufferRef>
#include <QWaylandSeat>
#include <QWaylandCompositor>
#include <QImage>
#include <QPixmap>
#include <QDebug>
#include <QRegion>

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

WMSurface::WMSurface(QWaylandSurface* surface,
                     WMCompositor*    compositor,
                     QObject*         parent)
: QObject(parent)
, m_surface(surface)
, m_compositor(compositor)
{
    Q_ASSERT(surface);
    Q_ASSERT(compositor);

    // Create the single view that lets us read back the committed buffer.
    // Passing the compositor as the output means Qt will use our primary
    // QWaylandOutput for buffer-scale / transform lookups.
    m_view = new QWaylandView(compositor, this);
    m_view->setSurface(surface);
    m_view->setBufferLocked(false);

    connectSurfaceSignals();

    qDebug() << "[WMSurface] created for surface" << surface;
}

WMSurface::~WMSurface() {
    // m_view is a child QObject, deleted automatically.
    // Notify subsurfaces that we are gone.
    for (auto* child : m_subsurfaces) {
        if (child) child->setParentSurface(nullptr);
    }
    qDebug() << "[WMSurface] destroyed";
}

// ─────────────────────────────────────────────────────────────────────────────
// Signal wiring
// ─────────────────────────────────────────────────────────────────────────────

void WMSurface::connectSurfaceSignals() {
    // Mapped / unmapped ── emitted by QWaylandSurface when the buffer
    // attaches or detaches.
    connect(m_surface, &QWaylandSurface::hasContentChanged,
            this, [this] { updateMappedState(); });

    // Redraw ── emitted every time the client commits a new buffer.
    connect(m_surface, &QWaylandSurface::redraw,
            this, &WMSurface::onRedraw);

    // Damage ── collect regions the client marks as dirty.
    // damaged() is a signal (not a getter) — accumulate into m_damage here.
    connect(m_surface, &QWaylandSurface::damaged,
            this, [this](const QRegion& region) {
                m_damage += region;
            });

    // Surface destroyed ── the client disconnected or called wl_surface.destroy.
    connect(m_surface, &QWaylandSurface::destroyed,
            this, &WMSurface::onSurfaceDestroyed);

    // Buffer committed through the view.
    connect(m_view, &QWaylandView::bufferCommitted,
            this, &WMSurface::onBufferCommitted);
}

// ─────────────────────────────────────────────────────────────────────────────
// Role management
// ─────────────────────────────────────────────────────────────────────────────

void WMSurface::setRole(Role r) {
    if (m_role == r) return;
    m_role = r;
    updateMappedState();
    emit roleChanged(r);
    qDebug() << "[WMSurface] role set to" << (int)r;
}

// ─────────────────────────────────────────────────────────────────────────────
// Window linkage
// ─────────────────────────────────────────────────────────────────────────────

void WMSurface::setWindow(Window* w) {
    if (m_window == w) return;
    m_window = w;
    emit windowAssigned(w);
}

// ─────────────────────────────────────────────────────────────────────────────
// Geometry
// ─────────────────────────────────────────────────────────────────────────────

QSize WMSurface::size() const {
    if (!m_surface) return {};
    // Qt6: QWaylandSurface::size() was removed.
    // destinationSize() returns the logical size (buffer size / bufferScale).
    return m_surface->destinationSize();
}

void WMSurface::setPosition(const QPoint& p) {
    if (m_position == p) return;
    m_position = p;
    emit positionChanged(p);
}

// ─────────────────────────────────────────────────────────────────────────────
// Buffer readback
// ─────────────────────────────────────────────────────────────────────────────

QImage WMSurface::toImage() const {
    if (!m_view) return {};

    // currentBuffer() gives us the latest committed buffer ref.
    QWaylandBufferRef buf = m_view->currentBuffer();
    if (!buf.hasBuffer()) return {};

    // For SHM buffers Qt can hand us a QImage directly from shared memory.
    // For DMA-BUF / OpenGL textures we would need a different path; for
    // now we handle the common SHM case that covers most Wayland clients.
    QImage img = buf.image();
    if (!img.isNull()) return img;

    // Fallback: if the buffer is a non-SHM type (e.g. wl_drm / DMA-BUF),
    // return a solid placeholder so the window still renders as a coloured
    // rectangle rather than nothing.  A production compositor would upload
    // the buffer to an OpenGL texture and blit it via a shader.
    QImage placeholder(size(), QImage::Format_ARGB32_Premultiplied);
    placeholder.fill(QColor(30, 35, 55, 220));
    return placeholder;
}

QPixmap WMSurface::toPixmap() const {
    QImage img = toImage();
    if (img.isNull()) return {};
    return QPixmap::fromImage(img);
}

// ─────────────────────────────────────────────────────────────────────────────
// Buffer scale
// ─────────────────────────────────────────────────────────────────────────────

int WMSurface::bufferScale() const {
    if (!m_surface) return 1;
    return qMax(1, m_surface->bufferScale());
}

// ─────────────────────────────────────────────────────────────────────────────
// Content / damage tracking
// ─────────────────────────────────────────────────────────────────────────────

void WMSurface::markContentPresented() {
    m_contentPending = false;
    // Do NOT clear damage here — caller does that separately via clearDamage()
    // so damage can be used for partial re-renders.
}

void WMSurface::clearDamage() {
    m_damage = QRegion();
}

// ─────────────────────────────────────────────────────────────────────────────
// Frame callbacks
// ─────────────────────────────────────────────────────────────────────────────

void WMSurface::sendFrameCallbacks() {
    if (!m_surface || m_destroyed) return;
    // Sending frame callbacks paces the client: it will only produce a new
    // buffer after receiving this callback, preventing buffer overflow.
    m_surface->frameStarted();
    m_surface->sendFrameCallbacks();
}

// ─────────────────────────────────────────────────────────────────────────────
// Keyboard focus
// ─────────────────────────────────────────────────────────────────────────────

void WMSurface::setKeyboardFocus(bool focused) {
    if (m_hasKeyboardFocus == focused) return;
    m_hasKeyboardFocus = focused;

    if (!m_surface || !m_compositor) return;

    // Notify the Wayland seat so the client receives keyboard enter/leave.
    QWaylandSeat* seat = m_compositor->defaultSeat();
    if (!seat) return;

    if (focused) {
        seat->setKeyboardFocus(m_surface);
    } else {
        // Only clear if we actually own the focus.
        if (seat->keyboardFocus() == m_surface) {
            seat->setKeyboardFocus(nullptr);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Subsurface management
// ─────────────────────────────────────────────────────────────────────────────

void WMSurface::addSubsurface(WMSurface* child) {
    if (!child || m_subsurfaces.contains(child)) return;
    m_subsurfaces.append(child);
    child->setParentSurface(this);
    qDebug() << "[WMSurface] subsurface added";
}

void WMSurface::removeSubsurface(WMSurface* child) {
    if (!m_subsurfaces.removeOne(child)) return;
    if (child->parentSurface() == this) {
        child->setParentSurface(nullptr);
    }
    qDebug() << "[WMSurface] subsurface removed";
}

// ─────────────────────────────────────────────────────────────────────────────
// Mapped state helper
// ─────────────────────────────────────────────────────────────────────────────

void WMSurface::updateMappedState() {
    if (!m_surface) return;

    // A surface is considered "mapped" when:
    //   1. It has an attached buffer with pixel content, AND
    //   2. It has been assigned a role (so the compositor knows what to do
    //      with it).
    bool shouldBeMapped = m_surface->hasContent() && (m_role != Role::None);

    if (shouldBeMapped == m_mapped) return;

    m_mapped = shouldBeMapped;

    if (m_mapped) {
        qDebug() << "[WMSurface] mapped, size =" << size();
        emit mapped();
    } else {
        qDebug() << "[WMSurface] unmapped";
        emit unmapped();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// QWaylandSurface slots
// ─────────────────────────────────────────────────────────────────────────────

void WMSurface::onSurfaceMapped() {
    // hasContentChanged already covers this; kept as a named slot for clarity
    // in case subclasses or tests connect to it directly.
    updateMappedState();
}

void WMSurface::onSurfaceUnmapped() {
    updateMappedState();
}

void WMSurface::onRedraw() {
    if (!m_surface || m_destroyed) return;

    // m_damage is accumulated via the damaged() signal connection in
    // connectSurfaceSignals(). If no damage was reported, mark the whole
    // surface dirty so the renderer doesn't skip it.
    if (m_damage.isEmpty()) {
        m_damage += QRect(QPoint(0, 0), size());
    }

    m_contentPending = true;
    emit contentChanged(m_damage);
}

// ─────────────────────────────────────────────────────────────────────────────
// QWaylandView slots
// ─────────────────────────────────────────────────────────────────────────────

void WMSurface::onBufferCommitted() {
    // A new buffer was committed by the client and is now available via
    // m_view->currentBuffer().  Let interested parties (primarily WMOutput)
    // know that fresh pixel data is ready.
    if (m_destroyed) return;

    QSize newSize = size();
    // Notify if buffer dimensions changed (e.g. window was resized).
    static QSize lastSize;
    if (newSize != lastSize) {
        lastSize = newSize;
        emit sizeChanged(newSize);
    }

    // onRedraw() is the canonical "new content" signal; buffer committed just
    // ensures the view bookkeeping is up to date.  We do NOT set
    // m_contentPending here — that happens in onRedraw() which Qt calls next.
}

// ─────────────────────────────────────────────────────────────────────────────
// Surface destroyed
// ─────────────────────────────────────────────────────────────────────────────

void WMSurface::onSurfaceDestroyed() {
    qDebug() << "[WMSurface] underlying wl_surface destroyed";
    m_destroyed = true;
    m_mapped    = false;
    m_surface   = nullptr;

    // Detach the view so it no longer tries to read a dead buffer.
    if (m_view) {
        m_view->setSurface(nullptr);
    }

    emit unmapped();
}
