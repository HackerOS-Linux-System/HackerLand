#pragma once

#include <QObject>
#include <QWaylandSurface>
#include <QWaylandView>
#include <QWaylandBufferRef>
#include <QSize>
#include <QPoint>
#include <QRect>
#include <QPixmap>
#include <QImage>
#include <QRegion>
#include <QTimer>

class WMCompositor;
class Window;

// ─────────────────────────────────────────────────────────────────────────────
// WMSurface
//
// Wraps a QWaylandSurface and owns the single QWaylandView that maps it onto
// the compositor's output.  It is the bridge between the Wayland protocol
// layer (buffers, damage, subsurfaces, roles) and the compositor's internal
// Window model.
//
// Lifetime:
//   Created by WMCompositor::onSurfaceCreated().
//   Destroyed when the underlying QWaylandSurface emits destroyed().
//
// Rendering:
//   Maintains a QWaylandView so the compositor can call
//   view()->currentBuffer() to get the latest SHM / DMA-BUF frame.
//   Also exposes a convenience toImage() / toPixmap() that converts the
//   current buffer into a QImage for QPainter-based rendering.
//
// Roles:
//   A Wayland surface can have at most one role at a time.
//   WMSurface tracks which role is currently active (XdgToplevel, XdgPopup,
//   LayerSurface, Subsurface, Cursor, None) so the compositor can dispatch
//   events correctly without dynamic_cast soup.
// ─────────────────────────────────────────────────────────────────────────────
class WMSurface : public QObject {
    Q_OBJECT

public:
    // ── Surface role ──────────────────────────────────────────────────────
    enum class Role {
        None,           ///< No role assigned yet (wl_surface just created)
        XdgToplevel,    ///< Regular application window
        XdgPopup,       ///< Transient popup / context-menu
        LayerSurface,   ///< wlr-layer-shell panel / overlay
        Subsurface,     ///< wl_subsurface child
        Cursor          ///< Pointer cursor image
    };

    // ── Construction ──────────────────────────────────────────────────────
    explicit WMSurface(QWaylandSurface* surface,
                       WMCompositor*    compositor,
                       QObject*         parent = nullptr);
    ~WMSurface() override;

    // ── Core accessors ────────────────────────────────────────────────────
    QWaylandSurface* surface()    const { return m_surface;    }
    QWaylandView*    view()       const { return m_view;       }
    WMCompositor*    compositor() const { return m_compositor; }

    /// The Window that owns this surface (set by WMCompositor after the role
    /// is assigned; nullptr until then, and nullptr for popups/cursors).
    Window*          window()     const { return m_window;     }
    void             setWindow(Window* w);

    // ── Role management ───────────────────────────────────────────────────
    Role  role() const { return m_role; }
    void  setRole(Role r);

    bool  isToplevel()    const { return m_role == Role::XdgToplevel;  }
    bool  isPopup()       const { return m_role == Role::XdgPopup;     }
    bool  isLayerSurface()const { return m_role == Role::LayerSurface; }
    bool  isSubsurface()  const { return m_role == Role::Subsurface;   }
    bool  isCursor()      const { return m_role == Role::Cursor;       }

    // ── Geometry ──────────────────────────────────────────────────────────
    /// Buffer size in logical pixels (respects buffer scale).
    QSize  size()         const;

    /// Position in compositor (output) coordinates — set by the compositor.
    QPoint position()     const { return m_position; }
    void   setPosition(const QPoint& p);

    /// Bounding rect in compositor coordinates.
    QRect  rect()         const { return { m_position, size() }; }

    // ── Buffer / content ──────────────────────────────────────────────────
    /// True if the surface has a committed buffer that has not yet been
    /// presented (i.e. there is new pixel content to display).
    bool   hasPendingContent() const { return m_contentPending; }

    /// Mark pending content as consumed (called after the frame is rendered).
    void   markContentPresented();

    /// Convert the current Wayland buffer to a QImage.
    /// Returns a null QImage if no buffer is attached.
    /// The returned image shares memory with the SHM buffer — do not store
    /// it beyond the current frame without calling .copy().
    QImage toImage() const;

    /// Convenience wrapper — returns toImage() as a QPixmap.
    /// Slightly more expensive (copies into GPU-friendly storage).
    QPixmap toPixmap() const;

    // ── Damage tracking ───────────────────────────────────────────────────
    /// Accumulated damage region since the last markContentPresented() call.
    QRegion accumulatedDamage() const { return m_damage; }

    /// Clear the accumulated damage (call after re-rendering the surface).
    void    clearDamage();

    // ── Frame callbacks ───────────────────────────────────────────────────
    /// Send a wl_surface.frame callback to the client so it knows the
    /// compositor is ready for the next frame.  Should be called once per
    /// rendered frame that contained this surface.
    void    sendFrameCallbacks();

    // ── Input ─────────────────────────────────────────────────────────────
    /// True if this surface accepts pointer / keyboard input.
    bool    acceptsInput()  const { return m_acceptsInput; }
    void    setAcceptsInput(bool v) { m_acceptsInput = v; }

    /// True if the keyboard focus is currently on this surface.
    bool    hasKeyboardFocus() const { return m_hasKeyboardFocus; }
    void    setKeyboardFocus(bool v);

    // ── Buffer scale ──────────────────────────────────────────────────────
    /// HiDPI scale factor reported by the client (usually 1 or 2).
    int     bufferScale() const;

    // ── Subsurfaces ───────────────────────────────────────────────────────
    void    addSubsurface(WMSurface* child);
    void    removeSubsurface(WMSurface* child);
    const QList<WMSurface*>& subsurfaces() const { return m_subsurfaces; }

    WMSurface* parentSurface() const { return m_parent; }
    void       setParentSurface(WMSurface* p) { m_parent = p; }

    // ── Mapped state ──────────────────────────────────────────────────────
    /// A surface is "mapped" when it has a buffer and a role, and is ready
    /// to be displayed.  Unmapped surfaces are skipped during rendering.
    bool    isMapped() const { return m_mapped; }

signals:
    void mapped();                          ///< Surface became ready to display
    void unmapped();                        ///< Surface lost its buffer / role
    void contentChanged(const QRegion& dmg);///< New buffer committed with damage
    void positionChanged(const QPoint& p);  ///< setPosition() called
    void roleChanged(Role r);               ///< Role assigned or cleared
    void windowAssigned(Window* w);         ///< Window model linked
    void sizeChanged(const QSize& s);       ///< Buffer dimensions changed

private slots:
    // Connected to QWaylandSurface signals
    void onSurfaceMapped();
    void onSurfaceUnmapped();
    void onRedraw();
    void onSurfaceDestroyed();

    // Connected to QWaylandView signals
    void onBufferCommitted();

private:
    // ── Helpers ───────────────────────────────────────────────────────────
    void connectSurfaceSignals();
    void updateMappedState();

    // ── Members ───────────────────────────────────────────────────────────
    QWaylandSurface* m_surface      = nullptr;
    QWaylandView*    m_view         = nullptr;
    WMCompositor*    m_compositor   = nullptr;
    Window*          m_window       = nullptr;
    WMSurface*       m_parent       = nullptr;

    Role             m_role         = Role::None;
    QPoint           m_position;
    QRegion          m_damage;

    QList<WMSurface*> m_subsurfaces;

    bool             m_mapped          = false;
    bool             m_contentPending  = false;
    bool             m_acceptsInput    = true;
    bool             m_hasKeyboardFocus = false;
    bool             m_destroyed       = false;
};
