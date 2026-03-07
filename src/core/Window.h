#pragma once

#include <QObject>
#include <QRect>
#include <QSize>
#include <QPoint>
#include <QString>
#include <QIcon>

class WMSurface;

// ─────────────────────────────────────────────────────────────────────────────
// WindowState
//
// Lifecycle and layout state of a Window.  Only one state is active at a time.
// Transitions are managed exclusively by Window::setState() and the toggle*()
// helpers so that m_savedGeometry is always kept consistent.
// ─────────────────────────────────────────────────────────────────────────────
enum class WindowState {
    Normal,      ///< Default state before a role is confirmed (transient).
    Maximized,   ///< Covers the compositor work area; no title bar gaps.
    Fullscreen,  ///< Covers the full output (no bar, no gaps, no borders).
    Minimized,   ///< Hidden from view; client is still running.
    Floating,    ///< Not managed by the tiling engine; free to be dragged.
    Tiled,       ///< Managed by the tiling engine; geometry set by TilingEngine.
    Monocle      ///< Tiled but stacked — only the active window is visible.
};

// ─────────────────────────────────────────────────────────────────────────────
// WindowType
//
// Semantic classification used by WMXdgShell to decide how to tile / float a
// new window and whether to draw a title bar.
// ─────────────────────────────────────────────────────────────────────────────
enum class WindowType {
    Normal,        ///< Standard application window — subject to tiling rules.
    Dialog,        ///< Transient modal / non-modal dialog — always floated.
    Popup,         ///< Short-lived context menu or tooltip overlay.
    Tooltip,       ///< Hover tooltip — no decoration, no focus.
    Notification,  ///< System notification bubble — layer-shell managed.
    Desktop,       ///< Background / iconset surface (layer Background).
    DockBar        ///< Panel / dock (layer Top or Bottom, exclusive zone).
};

// ─────────────────────────────────────────────────────────────────────────────
// Window
//
// The compositor's internal model for one mapped toplevel surface.
// Created by WMXdgShell when an xdg_toplevel is committed for the first time
// and destroyed when the toplevel's underlying wl_surface is destroyed.
//
// Responsibilities
//   • Store identity (title, appId, className, icon).
//   • Track geometry with min/max constraints; emit geometryChanged on change.
//   • Maintain WindowState and transition logic (save/restore geometry).
//   • Expose opacity and animation-progress fields used by WMOutput rendering.
//   • Provide toggle actions (float, maximize, fullscreen, close) that emit
//     protocol-level signals WMXdgShell can forward to the client.
//
// Ownership
//   WMCompositor owns all Window instances; Workspace holds non-owning ptrs.
//   WMSurface is owned by WMCompositor and outlives the Window it is linked to
//   only briefly — always null-check surface() before use.
//
// Thread safety
//   All methods must be called on the compositor (main GUI) thread.
// ─────────────────────────────────────────────────────────────────────────────
class Window : public QObject {
    Q_OBJECT

    // Qt property bindings used by QML / animation engine.
    Q_PROPERTY(bool  active   READ isActive  WRITE setActive  NOTIFY activeChanged)
    Q_PROPERTY(QRect geometry READ geometry  WRITE setGeometry NOTIFY geometryChanged)
    Q_PROPERTY(float opacity  READ opacity   WRITE setOpacity)

public:
    // ── Construction ──────────────────────────────────────────────────────
    explicit Window(WMSurface* surface, QObject* parent = nullptr);
    ~Window() override;

    // ── Identity ──────────────────────────────────────────────────────────

    /// Unique monotonically increasing compositor-assigned identifier.
    uint64_t   id()        const { return m_id;        }

    /// Window title from the most recent xdg_toplevel.set_title request.
    QString    title()     const { return m_title;     }

    /// Application identifier from xdg_toplevel.set_app_id
    /// (e.g. "org.gnome.Nautilus", "firefox").
    QString    appId()     const { return m_appId;     }

    /// Lower-cased leaf component of appId, used for icon / rule lookups
    /// (e.g. "nautilus", "firefox").
    QString    className() const { return m_className; }

    /// Best-effort icon resolved from the theme using appId / className.
    /// May be null — callers should check isNull() before painting.
    QIcon      icon()      const { return m_icon;      }

    /// Semantic window category (used by WMXdgShell for tiling decisions).
    WindowType type()      const { return m_type;      }

    /// The Wayland surface wrapper that carries the pixel content.
    /// May be nullptr briefly during construction or after surface destruction.
    WMSurface* surface()   const { return m_surface;   }

    // ── State predicates ──────────────────────────────────────────────────

    WindowState state()        const { return m_state;  }
    bool isActive()            const { return m_active; }
    bool isFloating()          const { return m_state == WindowState::Floating;    }
    bool isFullscreen()        const { return m_state == WindowState::Fullscreen;  }
    bool isMaximized()         const { return m_state == WindowState::Maximized;   }
    bool isMinimized()         const { return m_state == WindowState::Minimized;   }
    bool isTiled()             const { return m_state == WindowState::Tiled;       }
    bool isMonocle()           const { return m_state == WindowState::Monocle;     }

    /// Pinned windows are shown on every workspace (like i3's sticky windows).
    bool isPinned()            const { return m_pinned;  }

    /// Visibility flag — false while minimized or on an inactive workspace.
    bool isVisible()           const { return m_visible; }

    /// Index of the workspace this window currently belongs to (1-based).
    int  workspaceId()         const { return m_workspaceId; }

    // ── Geometry ──────────────────────────────────────────────────────────

    /// Current geometry in compositor (screen) coordinates.
    QRect geometry()           const { return m_geometry;      }

    /// Geometry saved before the last fullscreen / maximise / float transition.
    /// Used to restore position when the state is reversed.
    QRect savedGeometry()      const { return m_savedGeometry; }

    /// Client-reported minimum acceptable size (clamped to kMinWindowWidth /
    /// kMinWindowHeight at the compositor level).
    QSize minSize()            const { return m_minSize; }

    /// Client-reported maximum acceptable size (kUnboundedMax × kUnboundedMax
    /// when the client has no upper limit).
    QSize maxSize()            const { return m_maxSize; }

    /// Convenience: geometry().center().
    QPoint center()            const { return m_geometry.center(); }

    // ── Animation ─────────────────────────────────────────────────────────

    /// Linear progress of the current open / move / close animation in [0, 1].
    /// 1.0 = animation complete / not running.
    float animProgress()       const { return m_animProgress; }

    /// Render opacity in [0, 1].  Set automatically from config on
    /// setActive(); can be overridden for custom fade effects.
    float opacity()            const { return m_opacity; }

    // ── Tiling slot ───────────────────────────────────────────────────────

    /// Index of this window in the workspace's ordered tile list.
    /// -1 = floating / fullscreen (not in tile order).
    int  tileSlot()            const { return m_tileSlot; }

    // ── Identity setters ──────────────────────────────────────────────────

    void setTitle(const QString& t);
    void setAppId(const QString& id);
    void setType(WindowType t)       { m_type = t; }

    /// Override the min-size hint (clamped to compositor minimum).
    void setMinSize(const QSize& sz);

    /// Override the max-size hint.  Pass a zero or negative size to remove
    /// the constraint (maps to kUnboundedMax internally).
    void setMaxSize(const QSize& sz);

    // ── State setters ─────────────────────────────────────────────────────

    /// Activate or deactivate the window.
    /// Automatically updates m_opacity from config and emits activeChanged().
    void setActive(bool active);

    /// Transition to \p state.
    /// • Leaving Fullscreen / Maximized → restores m_savedGeometry.
    /// • Emits stateChanged().
    void setState(WindowState state);

    /// Assign this window to workspace \p id and emit workspaceChanged().
    void setWorkspace(int id);

    /// Set the pinned flag (shown on all workspaces when true).
    void setPinned(bool pinned);

    /// Show or hide the window without changing its state.
    /// Called by the compositor on workspace switch.
    void setVisible(bool visible);

    /// Override render opacity; clamped to [0, 1].
    void setOpacity(float opacity);

    /// Update animation progress; clamped to [0, 1].
    void setAnimProgress(float p);

    /// Assign tile slot index; -1 = not in tile order.
    void setTileSlot(int s) { m_tileSlot = s; }

    // ── Geometry setters ──────────────────────────────────────────────────

    /// Set geometry immediately (no animation).
    /// Clamps to [minSize, maxSize] before storing.
    /// Emits geometryChanged() if the value changes.
    void setGeometry(const QRect& rect);

    /// Request an animated move to \p rect.
    /// When animations are disabled this is equivalent to setGeometry().
    /// When enabled, sets m_animProgress = 0 and emits geometryChanged() so
    /// AnimationEngine can drive the interpolation.
    /// \p durationMs  Duration override in milliseconds;
    ///                -1 = use Config::anim.tileRearrangeMs.
    void setGeometryAnimated(const QRect& rect, int durationMs = -1);

    // ── Actions ───────────────────────────────────────────────────────────

    /// Request the client to close the window by emitting closeRequested().
    /// Does NOT destroy the Window object — the compositor removes it when the
    /// xdg_toplevel.destroy event arrives from the client.
    void close();

    /// Toggle between Floating and Tiled.
    /// Saves / restores geometry across the transition.
    void toggleFloat();

    /// Toggle between Maximized and Tiled.
    /// Saves geometry on entry; restores it on exit via setState().
    void toggleMaximize();

    /// Toggle between Fullscreen and Tiled.
    /// Saves geometry on entry; restores it on exit via setState().
    void toggleFullscreen();

    /// Emit focusRequested() so the compositor can call setActive(true) after
    /// updating the previously-active window.
    void focus();

signals:
    // ── Model signals (emitted by this class) ─────────────────────────────
    void activeChanged(bool active);
    void geometryChanged(const QRect& rect);
    void stateChanged(WindowState state);
    void titleChanged(const QString& title);
    void visibilityChanged(bool visible);
    void workspaceChanged(int workspaceId);

    // ── Request signals (consumed by WMCompositor / WMXdgShell) ──────────
    /// WMXdgShell reacts by sending xdg_toplevel.close to the client.
    void closeRequested();

    /// WMCompositor reacts by calling setActive(true) on this window and
    /// setActive(false) on the previously-active window.
    void focusRequested();

    /// Emitted by setGeometryAnimated() when animations are enabled.
    /// AnimationEngine listens to this and drives the m_animProgress / geometry
    /// interpolation each frame.
    void animatedMoveRequested(const QRect& target, int durationMs);

private:
    // ── Internal helpers ──────────────────────────────────────────────────

    /// Attempt to load a theme icon for the current appId / className.
    void resolveIcon();

    /// Clamp \p rect to [m_minSize, m_maxSize] keeping topLeft fixed.
    QRect clampToConstraints(const QRect& rect) const;

    /// Compute a sensible default floating geometry centred on the primary
    /// screen.  Used when the window has no previous geometry on float-enter.
    QRect defaultFloatGeometry() const;

    // ── Static ────────────────────────────────────────────────────────────
    static uint64_t s_nextId;

    // ── Constants ─────────────────────────────────────────────────────────
    static constexpr int kMinWindowWidth   =   60; ///< Absolute compositor floor
    static constexpr int kMinWindowHeight  =   30;
    static constexpr int kUnboundedMax     = 16384; ///< "No limit" sentinel

    // ── Members ───────────────────────────────────────────────────────────

    // Identity
    uint64_t    m_id;
    WMSurface*  m_surface   = nullptr;
    QString     m_title;
    QString     m_appId;
    QString     m_className;
    QIcon       m_icon;
    WindowType  m_type      = WindowType::Normal;

    // State
    WindowState m_state     = WindowState::Tiled;
    bool        m_active    = false;
    bool        m_pinned    = false;
    bool        m_visible   = true;
    int         m_workspaceId = 1;
    int         m_tileSlot  = -1;

    // Geometry
    QRect       m_geometry;
    QRect       m_savedGeometry;      ///< Pre-transition snapshot
    QSize       m_minSize   = { kMinWindowWidth,  kMinWindowHeight  };
    QSize       m_maxSize   = { kUnboundedMax,    kUnboundedMax     };

    // Rendering
    float       m_animProgress = 1.0f; ///< 0=start  1=complete
    float       m_opacity      = 1.0f;

    // Animation target (set by setGeometryAnimated, read by AnimationEngine)
    QRect       m_animTarget;
    int         m_animDuration = 0;   ///< ms; 0 = not animating
};
