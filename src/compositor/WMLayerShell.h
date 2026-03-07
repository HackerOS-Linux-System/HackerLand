#pragma once

#include <QObject>
#include <QHash>
#include <QList>
#include <QRect>
#include <QSize>
#include <QMargins>
#include <QPointer>

// Qt6 Wayland does not ship a first-party wlr-layer-shell extension class, so
// we implement the protocol binding ourselves via QWaylandCompositorExtension.
// The actual wl_global advertisement and request dispatch are handled through
// Qt's generic extension mechanism; this header defines our high-level wrapper.

class QWaylandSurface;
class QWaylandOutput;
class QWaylandSeat;
class WMCompositor;
class WMSurface;

// ─────────────────────────────────────────────────────────────────────────────
// LayerSurfaceState
//
// Mirrors the fields a client can set via zwlr_layer_surface_v1 requests.
// Stored per surface so the compositor can read them when computing layout.
// ─────────────────────────────────────────────────────────────────────────────
struct LayerSurfaceState {
    // ── Protocol-level fields ─────────────────────────────────────────────

    /// Which z-layer the surface lives on.
    enum class Layer : uint32_t {
        Background = 0,  ///< Desktop wallpaper / iconsets
        Bottom     = 1,  ///< Below regular windows (docks that want to be behind)
        Top        = 2,  ///< Above regular windows (panels, bars)
        Overlay    = 3   ///< Screen-wide overlays, lock screens, OSD
    } layer = Layer::Top;

    /// Which output edges the surface is anchored to.
    /// Bitmask: 1=Top 2=Bottom 4=Left 8=Right
    uint32_t anchor = 0;

    /// Requested size in logical pixels.  A dimension of 0 means the surface
    /// wants to stretch to fill the anchor edge.
    QSize desiredSize { 0, 0 };

    /// Exclusive zone: how many pixels the surface carves out of the work area.
    ///  > 0  →  reserve that many pixels on the anchored edge
    ///   0   →  do not affect work area
    ///  -1   →  ignore work area (don't be displaced by other panels)
    int32_t exclusiveZone = 0;

    /// Per-edge margin (in logical pixels).
    QMargins margin { 0, 0, 0, 0 };

    /// Whether the surface should take keyboard input.
    enum class KeyboardInteractivity : uint32_t {
        None        = 0,  ///< Never receives keyboard focus
        Exclusive   = 1,  ///< Grabs keyboard exclusively (lock screens)
        OnDemand    = 2   ///< Receives keyboard focus when clicked
    } keyboardInteractivity = KeyboardInteractivity::None;

    // ── Compositor-assigned fields ─────────────────────────────────────────

    /// Final geometry in output (screen) coordinates, computed by
    /// WMLayerShell::computeGeometry().
    QRect geometry;

    /// Configure serial most recently sent to the client.
    uint32_t configureSent = 0;

    /// Configure serial most recently acknowledged by the client.
    uint32_t configureAcked = 0;

    // ── Helper predicates ──────────────────────────────────────────────────
    bool anchoredTop()    const { return anchor & 1; }
    bool anchoredBottom() const { return anchor & 2; }
    bool anchoredLeft()   const { return anchor & 4; }
    bool anchoredRight()  const { return anchor & 8; }
    bool anchoredFull()   const { return anchor == 15; } // all four edges

    bool isPanel()    const {
        return (layer == Layer::Top || layer == Layer::Bottom)
        && exclusiveZone > 0;
    }
    bool isOverlay()  const { return layer == Layer::Overlay; }
    bool isBackground()const{ return layer == Layer::Background; }
};

// ─────────────────────────────────────────────────────────────────────────────
// LayerSurfaceRecord
//
// Everything the compositor needs to know about one active layer surface.
// ─────────────────────────────────────────────────────────────────────────────
struct LayerSurfaceRecord {
    WMSurface*         surface  = nullptr;
    QWaylandOutput*    output   = nullptr;
    LayerSurfaceState  state;
    QString            nameSpace;          ///< Client-supplied namespace string
    bool               mapped   = false;
    bool               closed   = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// WMLayerShell
//
// Implements the wlr-layer-shell-unstable-v1 protocol for HackerLand WM.
//
// Responsibilities
//   • Advertise zwlr_layer_shell_v1 to Wayland clients.
//   • Accept get_layer_surface requests and create LayerSurfaceRecord entries.
//   • Process per-surface set_* requests (anchor, size, exclusive zone, margin,
//     keyboard interactivity, layer).
//   • Compute the final screen-space geometry for each surface and send
//     configure events.
//   • Maintain the work-area exclusion list so WMCompositor::workArea() can
//     account for panels (bars, docks).
//   • Render layer surfaces in the correct z-order around regular windows:
//         Background → Bottom → [windows] → Top → Overlay
//   • Handle keyboard grab for exclusive surfaces (lock screens).
//   • Tear down records cleanly when clients close or disconnect.
//
// Qt6 note:
//   Qt6 WaylandCompositor does not ship a zwlr_layer_shell_v1 class.  We
//   implement the wl_global / request dispatch ourselves using the low-level
//   QWaylandCompositorExtension API and a manually-parsed wayland protocol
//   object.  The actual Wayland protocol XML is at:
//       protocols/wlr-layer-shell-unstable-v1.xml
//
// ─────────────────────────────────────────────────────────────────────────────
class WMLayerShell : public QObject {
    Q_OBJECT

public:
    // ── Anchor bitmask constants (mirrors protocol enum) ──────────────────
    static constexpr uint32_t AnchorTop    = 1u;
    static constexpr uint32_t AnchorBottom = 2u;
    static constexpr uint32_t AnchorLeft   = 4u;
    static constexpr uint32_t AnchorRight  = 8u;

    // ── Construction ──────────────────────────────────────────────────────
    explicit WMLayerShell(WMCompositor* compositor, QObject* parent = nullptr);
    ~WMLayerShell() override;

    // ── Initialisation ────────────────────────────────────────────────────
    /// Register the zwlr_layer_shell_v1 global with the compositor.
    /// Must be called after QWaylandCompositor::create().
    void initialize();

    // ── Work-area query ───────────────────────────────────────────────────
    /// Returns the rectangle of the given output that is NOT reserved by any
    /// exclusive-zone layer surface.  WMCompositor::workArea() calls this.
    QRect exclusionAdjustedWorkArea(QWaylandOutput* output,
                                    const QRect& fullArea) const;

                                    /// Convenience overload using the primary output.
                                    QRect workArea() const;

                                    // ── Geometry reflow ───────────────────────────────────────────────────
                                    /// Recompute geometry for every layer surface on \p output and send
                                    /// configure events where the size changed.  Call whenever the output
                                    /// geometry changes or a surface joins / leaves.
                                    void reflowOutput(QWaylandOutput* output);

                                    // ── Surface queries ───────────────────────────────────────────────────
                                    /// All currently mapped layer surfaces on \p output, ordered by
                                    /// z-layer (background first, overlay last).
                                    QList<LayerSurfaceRecord*> surfacesForOutput(
                                        QWaylandOutput* output,
                                        LayerSurfaceState::Layer layer) const;

                                        /// Every mapped layer surface across all outputs (for the renderer).
                                        QList<LayerSurfaceRecord*> allMappedSurfaces() const;

                                        // ── Frame callbacks ───────────────────────────────────────────────────
                                        /// Forward frame callbacks to every visible layer surface.
                                        /// Called once per rendered frame by WMOutput.
                                        void sendFrameCallbacks();

signals:
    /// A new layer surface has been created and had its initial state set.
    void layerSurfaceCreated(LayerSurfaceRecord* record);

    /// A layer surface has been unmapped or explicitly closed.
    void layerSurfaceDestroyed(LayerSurfaceRecord* record);

    /// The effective work area changed (a panel appeared / disappeared /
    /// changed its exclusive zone).  WMCompositor should retile.
    void workAreaChanged();

private slots:
    void onSurfaceDestroyed();

private:
    // ── Internal protocol simulation ─────────────────────────────────────
    // Because Qt6 doesn't expose zwlr_layer_shell_v1 natively, we simulate
    // what a real wayland-scanner-generated binding would do when a client
    // calls get_layer_surface and the subsequent set_* requests.

    LayerSurfaceRecord* createRecord(WMSurface*      surface,
                                     QWaylandOutput* output,
                                     LayerSurfaceState::Layer layer,
                                     const QString&  nameSpace);

    void cleanupRecord         (LayerSurfaceRecord* rec);

    void handleSetAnchor       (LayerSurfaceRecord* rec, uint32_t anchor);
    void handleSetSize         (LayerSurfaceRecord* rec, uint32_t w, uint32_t h);
    void handleSetExclusiveZone(LayerSurfaceRecord* rec, int32_t  zone);
    void handleSetMargin       (LayerSurfaceRecord* rec,
                                int32_t top, int32_t right,
                                int32_t bottom, int32_t left);
    void handleSetKeyboardInteractivity(
        LayerSurfaceRecord* rec,
        LayerSurfaceState::KeyboardInteractivity mode);
    void handleSetLayer        (LayerSurfaceRecord* rec,
                                LayerSurfaceState::Layer layer);
    void handleGetPopup        (LayerSurfaceRecord* rec,
                                WMSurface*          popupSurface);
    void handleAckConfigure    (LayerSurfaceRecord* rec, uint32_t serial);
    void handleDestroy         (LayerSurfaceRecord* rec);

    // ── Geometry helpers ──────────────────────────────────────────────────

    /// Compute the final QRect for one surface given the output geometry and
    /// the current exclusion margins already accumulated by earlier panels.
    QRect computeGeometry(const LayerSurfaceRecord* rec,
                          const QRect& outputRect,
                          const QMargins& currentExclusion) const;

                          /// Derive QMargins that this record contributes to the work-area exclusion.
                          QMargins exclusionFor(const LayerSurfaceRecord* rec) const;

                          /// Send a zwlr_layer_surface_v1.configure(serial, width, height) to the
                          /// client and record the serial in the state.
                          void sendConfigure(LayerSurfaceRecord* rec, const QSize& size);

                          /// Send zwlr_layer_surface_v1.closed() and mark the record as closed.
                          void sendClosed(LayerSurfaceRecord* rec);

                          // ── Keyboard grab helpers ─────────────────────────────────────────────

                          /// Grant exclusive keyboard focus to an overlay surface.
                          void grabKeyboard(LayerSurfaceRecord* rec);

                          /// Release a keyboard grab previously granted to \p rec.
                          void ungrabKeyboard(LayerSurfaceRecord* rec);

                          // ── Utility ───────────────────────────────────────────────────────────
                          QWaylandOutput* outputForSurface(WMSurface* surface) const;

                          LayerSurfaceRecord* recordForSurface(WMSurface* surface) const;

                          // Sort predicate: layer ascending, then insertion order.
                          static bool layerLessThan(const LayerSurfaceRecord* a,
                                                    const LayerSurfaceRecord* b);

                          // ── Members ───────────────────────────────────────────────────────────
                          WMCompositor*                m_compositor = nullptr;

                          /// All records ever created (including destroyed ones until cleaned up).
                          QList<LayerSurfaceRecord*>   m_records;

                          /// Surface currently holding an exclusive keyboard grab (if any).
                          LayerSurfaceRecord*          m_keyboardGrabOwner = nullptr;

                          /// Running configure serial counter.
                          uint32_t                     m_nextSerial = 1;
};
