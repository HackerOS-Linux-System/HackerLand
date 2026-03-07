#include "WMLayerShell.h"
#include "WMCompositor.h"
#include "WMSurface.h"
#include "compositor/WMOutput.h"
#include "core/Config.h"

#include <QWaylandCompositor>
#include <QWaylandOutput>
#include <QWaylandSurface>
#include <QWaylandSeat>
#include <QGuiApplication>
#include <QScreen>
#include <QDebug>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers — anonymous namespace
// ─────────────────────────────────────────────────────────────────────────────
namespace {

    /// Human-readable name for a layer value (debug only).
    const char* layerName(LayerSurfaceState::Layer l) {
        switch (l) {
            case LayerSurfaceState::Layer::Background: return "background";
            case LayerSurfaceState::Layer::Bottom:     return "bottom";
            case LayerSurfaceState::Layer::Top:        return "top";
            case LayerSurfaceState::Layer::Overlay:    return "overlay";
        }
        return "unknown";
    }

    /// Convert a uint32_t layer enum value coming from the protocol wire into our
    /// typed enum.  Unknown values default to Top.
    [[maybe_unused]]
    static LayerSurfaceState::Layer layerFromUint(uint32_t v) {
        switch (v) {
            case 0:  return LayerSurfaceState::Layer::Background;
            case 1:  return LayerSurfaceState::Layer::Bottom;
            case 2:  return LayerSurfaceState::Layer::Top;
            case 3:  return LayerSurfaceState::Layer::Overlay;
            default: return LayerSurfaceState::Layer::Top;
        }
    }

    /// Clamp a margin value to a sane range (protocol allows arbitrary int32_t).
    int clampMargin(int32_t v) {
        return qBound(-4096, (int)v, 4096);
    }

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

WMLayerShell::WMLayerShell(WMCompositor* compositor, QObject* parent)
: QObject(parent)
, m_compositor(compositor)
{
    Q_ASSERT(compositor);
}

WMLayerShell::~WMLayerShell() {
    // Release any keyboard grab so the seat does not hold a dangling pointer.
    if (m_keyboardGrabOwner) {
        ungrabKeyboard(m_keyboardGrabOwner);
    }
    qDeleteAll(m_records);
    m_records.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// Initialisation
// ─────────────────────────────────────────────────────────────────────────────

void WMLayerShell::initialize() {
    // Qt6 WaylandCompositor has no built-in zwlr_layer_shell_v1 support.
    // In a production compositor you would:
    //   1. Parse wlr-layer-shell-unstable-v1.xml with wayland-scanner.
    //   2. Implement the generated *_interface vtable.
    //   3. Call wl_global_create() (accessible via QWaylandCompositor::display()).
    //
    // For HackerLand WM we expose a simulated version: our own BarWidget and
    // any third-party clients that understand a compatible ABI can talk to us
    // through the normal WMCompositor::createLayerSurface() call path, which
    // creates LayerSurfaceRecords directly.
    //
    // The code below sets up everything needed to manage those records once
    // they arrive.

    qDebug() << "[WMLayerShell] initialized"
    << "(zwlr_layer_shell_v1 simulation — ready for layer surfaces)";
}

// ─────────────────────────────────────────────────────────────────────────────
// Record creation
// ─────────────────────────────────────────────────────────────────────────────

LayerSurfaceRecord* WMLayerShell::createRecord(
    WMSurface*               surface,
    QWaylandOutput*          output,
    LayerSurfaceState::Layer layer,
    const QString&           nameSpace)
{
    Q_ASSERT(surface);

    // Ensure the WMSurface is marked with the correct role.
    surface->setRole(WMSurface::Role::LayerSurface);

    auto* rec       = new LayerSurfaceRecord;
    rec->surface    = surface;
    rec->output     = output;
    rec->state.layer = layer;
    rec->nameSpace  = nameSpace;

    m_records.append(rec);

    // Observe surface destruction so we can clean up automatically.
    connect(surface, &QObject::destroyed, this, &WMLayerShell::onSurfaceDestroyed);

    qDebug() << "[WMLayerShell] layer surface created:"
    << nameSpace
    << "layer:" << layerName(layer);

    return rec;
}

// ─────────────────────────────────────────────────────────────────────────────
// Protocol request handlers
// These would be called from the wl_resource dispatch callback in a real
// wayland-scanner-generated binding.  In our simulation they are called
// directly by the compositor integration code.
// ─────────────────────────────────────────────────────────────────────────────

void WMLayerShell::handleSetAnchor(LayerSurfaceRecord* rec, uint32_t anchor) {
    Q_ASSERT(rec);
    if (rec->closed) return;

    // Validate: only bits 0–3 are defined in the protocol.
    rec->state.anchor = anchor & 0x0Fu;

    qDebug() << "[WMLayerShell]" << rec->nameSpace
    << "set_anchor:" << rec->state.anchor
    << "(T=" << rec->state.anchoredTop()
    << "B="  << rec->state.anchoredBottom()
    << "L="  << rec->state.anchoredLeft()
    << "R="  << rec->state.anchoredRight() << ")";
}

void WMLayerShell::handleSetSize(LayerSurfaceRecord* rec,
                                 uint32_t w, uint32_t h) {
    Q_ASSERT(rec);
    if (rec->closed) return;

    // Protocol says 0 means "stretch to fill the anchor edge".
    rec->state.desiredSize = QSize((int)w, (int)h);

    qDebug() << "[WMLayerShell]" << rec->nameSpace
    << "set_size:" << rec->state.desiredSize;
                                 }

                                 void WMLayerShell::handleSetExclusiveZone(LayerSurfaceRecord* rec,
                                                                           int32_t zone) {
                                     Q_ASSERT(rec);
                                     if (rec->closed) return;

                                     bool wasPanel = rec->state.isPanel();
                                     rec->state.exclusiveZone = zone;
                                     bool isPanel  = rec->state.isPanel();

                                     qDebug() << "[WMLayerShell]" << rec->nameSpace
                                     << "set_exclusive_zone:" << zone;

                                     // If the exclusive-zone status changed, the work area must be recomputed.
                                     if (wasPanel != isPanel) {
                                         emit workAreaChanged();
                                     }
                                                                           }

                                                                           void WMLayerShell::handleSetMargin(LayerSurfaceRecord* rec,
                                                                                                              int32_t top, int32_t right,
                                                                                                              int32_t bottom, int32_t left) {
                                                                               Q_ASSERT(rec);
                                                                               if (rec->closed) return;

                                                                               rec->state.margin = QMargins(clampMargin(left),  clampMargin(top),
                                                                                                            clampMargin(right), clampMargin(bottom));

                                                                               qDebug() << "[WMLayerShell]" << rec->nameSpace
                                                                               << "set_margin: T" << top << "R" << right
                                                                               << "B" << bottom << "L" << left;
                                                                                                              }

                                                                                                              void WMLayerShell::handleSetKeyboardInteractivity(
                                                                                                                  LayerSurfaceRecord* rec,
                                                                                                                  LayerSurfaceState::KeyboardInteractivity mode)
                                                                                                              {
                                                                                                                  Q_ASSERT(rec);
                                                                                                                  if (rec->closed) return;

                                                                                                                  auto prev = rec->state.keyboardInteractivity;
                                                                                                                  rec->state.keyboardInteractivity = mode;

                                                                                                                  qDebug() << "[WMLayerShell]" << rec->nameSpace
                                                                                                                  << "set_keyboard_interactivity:" << (int)mode;

                                                                                                                  // If changing to exclusive, grab the keyboard immediately (if mapped).
                                                                                                                  if (mode == LayerSurfaceState::KeyboardInteractivity::Exclusive
                                                                                                                      && rec->mapped)
                                                                                                                  {
                                                                                                                      grabKeyboard(rec);
                                                                                                                  }
                                                                                                                  else if (prev == LayerSurfaceState::KeyboardInteractivity::Exclusive
                                                                                                                      && mode != LayerSurfaceState::KeyboardInteractivity::Exclusive)
                                                                                                                  {
                                                                                                                      if (m_keyboardGrabOwner == rec) {
                                                                                                                          ungrabKeyboard(rec);
                                                                                                                      }
                                                                                                                  }
                                                                                                              }

                                                                                                              void WMLayerShell::handleSetLayer(LayerSurfaceRecord* rec,
                                                                                                                                                LayerSurfaceState::Layer layer) {
                                                                                                                  Q_ASSERT(rec);
                                                                                                                  if (rec->closed) return;

                                                                                                                  bool wasPanel = rec->state.isPanel();
                                                                                                                  rec->state.layer = layer;
                                                                                                                  bool isPanel  = rec->state.isPanel();

                                                                                                                  qDebug() << "[WMLayerShell]" << rec->nameSpace
                                                                                                                  << "set_layer:" << layerName(layer);

                                                                                                                  if (wasPanel != isPanel) {
                                                                                                                      emit workAreaChanged();
                                                                                                                  }

                                                                                                                  // Re-sort and re-layout; we do a full reflow on the relevant output.
                                                                                                                  if (rec->output) {
                                                                                                                      reflowOutput(rec->output);
                                                                                                                  }
                                                                                                                                                }

                                                                                                                                                void WMLayerShell::handleGetPopup(LayerSurfaceRecord* rec,
                                                                                                                                                                                  WMSurface*          popupSurface) {
                                                                                                                                                    Q_ASSERT(rec);
                                                                                                                                                    Q_ASSERT(popupSurface);
                                                                                                                                                    // Reparent the popup surface to be a child of the layer surface in the
                                                                                                                                                    // surface tree, so it inherits the correct z-layer.
                                                                                                                                                    popupSurface->setParentSurface(rec->surface);
                                                                                                                                                    qDebug() << "[WMLayerShell]" << rec->nameSpace << "get_popup attached";
                                                                                                                                                                                  }

                                                                                                                                                                                  void WMLayerShell::handleAckConfigure(LayerSurfaceRecord* rec,
                                                                                                                                                                                                                        uint32_t serial) {
                                                                                                                                                                                      Q_ASSERT(rec);
                                                                                                                                                                                      if (rec->closed) return;

                                                                                                                                                                                      if (serial != rec->state.configureSent) {
                                                                                                                                                                                          qWarning() << "[WMLayerShell]" << rec->nameSpace
                                                                                                                                                                                          << "ack_configure serial mismatch: got" << serial
                                                                                                                                                                                          << "expected" << rec->state.configureSent;
                                                                                                                                                                                          // We tolerate mismatched serials — the client may ack an older
                                                                                                                                                                                          // configure if it sent several requests in quick succession.
                                                                                                                                                                                      }

                                                                                                                                                                                      rec->state.configureAcked = serial;

                                                                                                                                                                                      // Surface is now properly configured; mark it as mapped if it has a buffer.
                                                                                                                                                                                      if (!rec->mapped && rec->surface && rec->surface->isMapped()) {
                                                                                                                                                                                          rec->mapped = true;
                                                                                                                                                                                          emit layerSurfaceCreated(rec);

                                                                                                                                                                                          // Apply exclusive zone to the work area.
                                                                                                                                                                                          if (rec->state.isPanel()) {
                                                                                                                                                                                              emit workAreaChanged();
                                                                                                                                                                                          }

                                                                                                                                                                                          // Grant keyboard focus if the surface requested exclusive interactivity.
                                                                                                                                                                                          if (rec->state.keyboardInteractivity
                                                                                                                                                                                              == LayerSurfaceState::KeyboardInteractivity::Exclusive)
                                                                                                                                                                                          {
                                                                                                                                                                                              grabKeyboard(rec);
                                                                                                                                                                                          }

                                                                                                                                                                                          qDebug() << "[WMLayerShell]" << rec->nameSpace << "mapped";
                                                                                                                                                                                      }
                                                                                                                                                                                                                        }

                                                                                                                                                                                                                        void WMLayerShell::handleDestroy(LayerSurfaceRecord* rec) {
                                                                                                                                                                                                                            Q_ASSERT(rec);
                                                                                                                                                                                                                            if (rec->closed) return;

                                                                                                                                                                                                                            qDebug() << "[WMLayerShell]" << rec->nameSpace << "destroy requested";

                                                                                                                                                                                                                            sendClosed(rec);
                                                                                                                                                                                                                            cleanupRecord(rec);
                                                                                                                                                                                                                        }

                                                                                                                                                                                                                        // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                                                                                                                        // Geometry computation
                                                                                                                                                                                                                        // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                                                                                                                        void WMLayerShell::reflowOutput(QWaylandOutput* output) {
                                                                                                                                                                                                                            if (!output) return;

                                                                                                                                                                                                                            // Determine the full output rectangle in logical pixels.
                                                                                                                                                                                                                            QRect fullRect;
                                                                                                                                                                                                                            if (m_compositor->primaryOutput()
                                                                                                                                                                                                                                && m_compositor->primaryOutput()->screen())
                                                                                                                                                                                                                            {
                                                                                                                                                                                                                                fullRect = m_compositor->primaryOutput()->screen()->geometry();
                                                                                                                                                                                                                            } else {
                                                                                                                                                                                                                                // Fallback: assume 1920×1080 at origin.
                                                                                                                                                                                                                                fullRect = { 0, 0, 1920, 1080 };
                                                                                                                                                                                                                            }

                                                                                                                                                                                                                            // We process layers from background → overlay so that panels on the Top
                                                                                                                                                                                                                            // layer correctly shrink the space available to Background/Bottom panels.
                                                                                                                                                                                                                            const LayerSurfaceState::Layer layerOrder[] = {
                                                                                                                                                                                                                                LayerSurfaceState::Layer::Background,
                                                                                                                                                                                                                                LayerSurfaceState::Layer::Bottom,
                                                                                                                                                                                                                                LayerSurfaceState::Layer::Top,
                                                                                                                                                                                                                                LayerSurfaceState::Layer::Overlay
                                                                                                                                                                                                                            };

                                                                                                                                                                                                                            // Accumulate exclusion margins as we process each layer.
                                                                                                                                                                                                                            // Top-layer panels (our own BarWidget) run first and eat into the edges;
                                                                                                                                                                                                                            // Bottom-layer docks then eat from what's left.
                                                                                                                                                                                                                            QMargins accumulated { 0, 0, 0, 0 };

                                                                                                                                                                                                                            for (auto layer : layerOrder) {
                                                                                                                                                                                                                                // Collect every record on this output and layer, sorted by insertion.
                                                                                                                                                                                                                                QList<LayerSurfaceRecord*> layerRecs;
                                                                                                                                                                                                                                for (auto* rec : m_records) {
                                                                                                                                                                                                                                    if (!rec->closed && rec->output == output
                                                                                                                                                                                                                                        && rec->state.layer == layer)
                                                                                                                                                                                                                                    {
                                                                                                                                                                                                                                        layerRecs.append(rec);
                                                                                                                                                                                                                                    }
                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                for (auto* rec : layerRecs) {
                                                                                                                                                                                                                                    QRect newGeom = computeGeometry(rec, fullRect, accumulated);

                                                                                                                                                                                                                                    bool changed = (newGeom != rec->state.geometry);
                                                                                                                                                                                                                                    rec->state.geometry = newGeom;

                                                                                                                                                                                                                                    if (changed) {
                                                                                                                                                                                                                                        sendConfigure(rec, newGeom.size());
                                                                                                                                                                                                                                        // Tell the WMSurface its new compositor-space position.
                                                                                                                                                                                                                                        if (rec->surface) {
                                                                                                                                                                                                                                            rec->surface->setPosition(newGeom.topLeft());
                                                                                                                                                                                                                                        }
                                                                                                                                                                                                                                    }

                                                                                                                                                                                                                                    // If this panel has an exclusive zone, grow the accumulated
                                                                                                                                                                                                                                    // margin so subsequent panels and the tiling work area account
                                                                                                                                                                                                                                    // for it.
                                                                                                                                                                                                                                    if (rec->state.isPanel() || rec->state.exclusiveZone > 0) {
                                                                                                                                                                                                                                        QMargins contrib = exclusionFor(rec);
                                                                                                                                                                                                                                        accumulated.setTop(   accumulated.top()    + contrib.top());
                                                                                                                                                                                                                                        accumulated.setBottom(accumulated.bottom() + contrib.bottom());
                                                                                                                                                                                                                                        accumulated.setLeft(  accumulated.left()   + contrib.left());
                                                                                                                                                                                                                                        accumulated.setRight( accumulated.right()  + contrib.right());
                                                                                                                                                                                                                                    }
                                                                                                                                                                                                                                }
                                                                                                                                                                                                                            }

                                                                                                                                                                                                                            qDebug() << "[WMLayerShell] reflow complete for output; work area:"
                                                                                                                                                                                                                            << exclusionAdjustedWorkArea(output, fullRect);
                                                                                                                                                                                                                        }

                                                                                                                                                                                                                        QRect WMLayerShell::computeGeometry(const LayerSurfaceRecord* rec,
                                                                                                                                                                                                                                                            const QRect&   outputRect,
                                                                                                                                                                                                                                                            const QMargins& currentExclusion) const
                                                                                                                                                                                                                                                            {
                                                                                                                                                                                                                                                                Q_ASSERT(rec);

                                                                                                                                                                                                                                                                const auto& s = rec->state;
                                                                                                                                                                                                                                                                const QMargins& m = s.margin;

                                                                                                                                                                                                                                                                // Start from the available rectangle (output minus accumulated exclusions).
                                                                                                                                                                                                                                                                QRect avail = outputRect.marginsRemoved(currentExclusion);

                                                                                                                                                                                                                                                                // ── Horizontal extent ─────────────────────────────────────────────────
                                                                                                                                                                                                                                                                int x, w;
                                                                                                                                                                                                                                                                if (s.anchoredLeft() && s.anchoredRight()) {
                                                                                                                                                                                                                                                                // Stretch between left and right edges.
                                                                                                                                                                                                                                                                x = avail.left()  + m.left();
                                                                                                                                                                                                                                                                w = avail.width() - m.left() - m.right();
                                                                                                                                                                                                                                                                } else if (s.anchoredLeft()) {
                                                                                                                                                                                                                                                                x = avail.left() + m.left();
                                                                                                                                                                                                                                                                w = (s.desiredSize.width() > 0) ? s.desiredSize.width()
                                                                                                                                                                                                                                                                : avail.width() / 2;
                                                                                                                                                                                                                                                                } else if (s.anchoredRight()) {
                                                                                                                                                                                                                                                                w = (s.desiredSize.width() > 0) ? s.desiredSize.width()
                                                                                                                                                                                                                                                                : avail.width() / 2;
                                                                                                                                                                                                                                                                x = avail.right() - w - m.right();
                                                                                                                                                                                                                                                                } else {
                                                                                                                                                                                                                                                                // Neither edge: centre horizontally.
                                                                                                                                                                                                                                                                w = (s.desiredSize.width() > 0) ? s.desiredSize.width()
                                                                                                                                                                                                                                                                : qMin(800, avail.width());
                                                                                                                                                                                                                                                                x = avail.left() + (avail.width() - w) / 2;
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                // ── Vertical extent ───────────────────────────────────────────────────
                                                                                                                                                                                                                                                                int y, h;
                                                                                                                                                                                                                                                                if (s.anchoredTop() && s.anchoredBottom()) {
                                                                                                                                                                                                                                                                // Stretch between top and bottom edges.
                                                                                                                                                                                                                                                                y = avail.top()    + m.top();
                                                                                                                                                                                                                                                                h = avail.height() - m.top() - m.bottom();
                                                                                                                                                                                                                                                                } else if (s.anchoredTop()) {
                                                                                                                                                                                                                                                                y = avail.top() + m.top();
                                                                                                                                                                                                                                                                h = (s.desiredSize.height() > 0) ? s.desiredSize.height()
                                                                                                                                                                                                                                                                : Config::instance().theme.barHeight;
                                                                                                                                                                                                                                                                } else if (s.anchoredBottom()) {
                                                                                                                                                                                                                                                                h = (s.desiredSize.height() > 0) ? s.desiredSize.height()
                                                                                                                                                                                                                                                                : Config::instance().theme.barHeight;
                                                                                                                                                                                                                                                                y = avail.bottom() - h - m.bottom();
                                                                                                                                                                                                                                                                } else {
                                                                                                                                                                                                                                                                // Neither edge: centre vertically.
                                                                                                                                                                                                                                                                h = (s.desiredSize.height() > 0) ? s.desiredSize.height()
                                                                                                                                                                                                                                                                : qMin(600, avail.height());
                                                                                                                                                                                                                                                                y = avail.top() + (avail.height() - h) / 2;
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                // Clamp to sane minimum size.
                                                                                                                                                                                                                                                                w = qMax(w, 1);
                                                                                                                                                                                                                                                                h = qMax(h, 1);

                                                                                                                                                                                                                                                                return { x, y, w, h };
                                                                                                                                                                                                                                                            }

                                                                                                                                                                                                                                                            QMargins WMLayerShell::exclusionFor(const LayerSurfaceRecord* rec) const {
                                                                                                                                                                                                                                                                Q_ASSERT(rec);
                                                                                                                                                                                                                                                                QMargins result { 0, 0, 0, 0 };

                                                                                                                                                                                                                                                                if (rec->state.exclusiveZone <= 0) return result;
                                                                                                                                                                                                                                                                int zone = rec->state.exclusiveZone;

                                                                                                                                                                                                                                                                const auto& s = rec->state;

                                                                                                                                                                                                                                                                // Determine which edge the exclusive zone belongs to.
                                                                                                                                                                                                                                                                // The protocol rule: if the surface is anchored to exactly ONE edge
                                                                                                                                                                                                                                                                // (and optionally to the perpendicular pair), the exclusive zone applies
                                                                                                                                                                                                                                                                // to that single edge.
                                                                                                                                                                                                                                                                if (s.anchoredTop() && !s.anchoredBottom()) {
                                                                                                                                                                                                                                                                result.setTop(zone);
                                                                                                                                                                                                                                                                } else if (s.anchoredBottom() && !s.anchoredTop()) {
                                                                                                                                                                                                                                                                result.setBottom(zone);
                                                                                                                                                                                                                                                                } else if (s.anchoredLeft() && !s.anchoredRight()) {
                                                                                                                                                                                                                                                                result.setLeft(zone);
                                                                                                                                                                                                                                                                } else if (s.anchoredRight() && !s.anchoredLeft()) {
                                                                                                                                                                                                                                                                result.setRight(zone);
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                // For fully-anchored surfaces (e.g. full-screen overlays) no edge is
                                                                                                                                                                                                                                                                // reserved; the protocol does not define exclusion for them.

                                                                                                                                                                                                                                                                return result;
                                                                                                                                                                                                                                                            }

                                                                                                                                                                                                                                                            // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                                                                                                                                                            // Work-area queries
                                                                                                                                                                                                                                                            // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                                                                                                                                                            QRect WMLayerShell::exclusionAdjustedWorkArea(QWaylandOutput* output,
                                                                                                                                                                                                                                                                const QRect& fullArea) const
                                                                                                                                                                                                                                                                {
                                                                                                                                                                                                                                                                QMargins total { 0, 0, 0, 0 };

                                                                                                                                                                                                                                                                for (auto* rec : m_records) {
                                                                                                                                                                                                                                                                if (rec->closed || !rec->mapped) continue;
                                                                                                                                                                                                                                                                if (rec->output != output) continue;
                                                                                                                                                                                                                                                                if (rec->state.exclusiveZone <= 0) continue;

                                                                                                                                                                                                                                                                QMargins contrib = exclusionFor(rec);
                                                                                                                                                                                                                                                                total.setTop(   total.top()    + contrib.top());
                                                                                                                                                                                                                                                                total.setBottom(total.bottom() + contrib.bottom());
                                                                                                                                                                                                                                                                total.setLeft(  total.left()   + contrib.left());
                                                                                                                                                                                                                                                                total.setRight( total.right()  + contrib.right());
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                return fullArea.marginsRemoved(total);
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                QRect WMLayerShell::workArea() const {
                                                                                                                                                                                                                                                                QScreen* screen = nullptr;
                                                                                                                                                                                                                                                                if (m_compositor->primaryOutput()) {
                                                                                                                                                                                                                                                                screen = m_compositor->primaryOutput()->screen();
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                if (!screen) screen = QGuiApplication::primaryScreen();
                                                                                                                                                                                                                                                                if (!screen) return { 0, 0, 1920, 1080 };

                                                                                                                                                                                                                                                                QRect fullArea = screen->geometry();

                                                                                                                                                                                                                                                                // Determine which QWaylandOutput corresponds to the primary screen.
                                                                                                                                                                                                                                                                // For now we pass nullptr and only match records with a null output
                                                                                                                                                                                                                                                                // (those mapped to "any / primary output").
                                                                                                                                                                                                                                                                return exclusionAdjustedWorkArea(nullptr, fullArea);
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                                                                                                                                                                // Surface queries
                                                                                                                                                                                                                                                                // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                                                                                                                                                                QList<LayerSurfaceRecord*> WMLayerShell::surfacesForOutput(
                                                                                                                                                                                                                                                                QWaylandOutput* output,
                                                                                                                                                                                                                                                                LayerSurfaceState::Layer layer) const
                                                                                                                                                                                                                                                                {
                                                                                                                                                                                                                                                                QList<LayerSurfaceRecord*> result;
                                                                                                                                                                                                                                                                for (auto* rec : m_records) {
                                                                                                                                                                                                                                                                if (rec->closed)          continue;
                                                                                                                                                                                                                                                                if (!rec->mapped)         continue;
                                                                                                                                                                                                                                                                if (rec->output != output) continue;
                                                                                                                                                                                                                                                                if (rec->state.layer != layer) continue;
                                                                                                                                                                                                                                                                result.append(rec);
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                return result;
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                QList<LayerSurfaceRecord*> WMLayerShell::allMappedSurfaces() const {
                                                                                                                                                                                                                                                                QList<LayerSurfaceRecord*> result;
                                                                                                                                                                                                                                                                for (auto* rec : m_records) {
                                                                                                                                                                                                                                                                if (!rec->closed && rec->mapped) {
                                                                                                                                                                                                                                                                result.append(rec);
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                // Sort by layer so callers can iterate in render order.
                                                                                                                                                                                                                                                                std::sort(result.begin(), result.end(), layerLessThan);
                                                                                                                                                                                                                                                                return result;
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                                                                                                                                                                // Frame callbacks
                                                                                                                                                                                                                                                                // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                                                                                                                                                                void WMLayerShell::sendFrameCallbacks() {
                                                                                                                                                                                                                                                                for (auto* rec : m_records) {
                                                                                                                                                                                                                                                                if (!rec->closed && rec->mapped && rec->surface) {
                                                                                                                                                                                                                                                                rec->surface->sendFrameCallbacks();
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                                                                                                                                                                // Configure / close protocol messages
                                                                                                                                                                                                                                                                // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                                                                                                                                                                void WMLayerShell::sendConfigure(LayerSurfaceRecord* rec, const QSize& size) {
                                                                                                                                                                                                                                                                Q_ASSERT(rec);
                                                                                                                                                                                                                                                                if (rec->closed) return;

                                                                                                                                                                                                                                                                uint32_t serial = m_nextSerial++;
                                                                                                                                                                                                                                                                rec->state.configureSent = serial;

                                                                                                                                                                                                                                                                // In a real binding we would call:
                                                                                                                                                                                                                                                                //   zwlr_layer_surface_v1_send_configure(resource, serial, w, h);
                                                                                                                                                                                                                                                                // Here we log the action and update our internal state.  The real send
                                                                                                                                                                                                                                                                // happens in the protocol binding layer that wraps this class.

                                                                                                                                                                                                                                                                qDebug() << "[WMLayerShell] configure sent to" << rec->nameSpace
                                                                                                                                                                                                                                                                << "serial=" << serial
                                                                                                                                                                                                                                                                << "size="   << size;
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                void WMLayerShell::sendClosed(LayerSurfaceRecord* rec) {
                                                                                                                                                                                                                                                                Q_ASSERT(rec);
                                                                                                                                                                                                                                                                if (rec->closed) return;

                                                                                                                                                                                                                                                                rec->closed = true;

                                                                                                                                                                                                                                                                // In a real binding: zwlr_layer_surface_v1_send_closed(resource);
                                                                                                                                                                                                                                                                qDebug() << "[WMLayerShell] closed sent to" << rec->nameSpace;
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                                                                                                                                                                // Keyboard grab
                                                                                                                                                                                                                                                                // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                                                                                                                                                                void WMLayerShell::grabKeyboard(LayerSurfaceRecord* rec) {
                                                                                                                                                                                                                                                                Q_ASSERT(rec);
                                                                                                                                                                                                                                                                if (!rec->surface || !m_compositor) return;

                                                                                                                                                                                                                                                                // Release any previous grab first.
                                                                                                                                                                                                                                                                if (m_keyboardGrabOwner && m_keyboardGrabOwner != rec) {
                                                                                                                                                                                                                                                                ungrabKeyboard(m_keyboardGrabOwner);
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                QWaylandSeat* seat = m_compositor->defaultSeat();
                                                                                                                                                                                                                                                                if (!seat) return;

                                                                                                                                                                                                                                                                if (rec->surface->surface()) {
                                                                                                                                                                                                                                                                seat->setKeyboardFocus(rec->surface->surface());
                                                                                                                                                                                                                                                                m_keyboardGrabOwner = rec;
                                                                                                                                                                                                                                                                qDebug() << "[WMLayerShell] keyboard grabbed by" << rec->nameSpace;
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                void WMLayerShell::ungrabKeyboard(LayerSurfaceRecord* rec) {
                                                                                                                                                                                                                                                                Q_ASSERT(rec);
                                                                                                                                                                                                                                                                if (m_keyboardGrabOwner != rec) return;

                                                                                                                                                                                                                                                                QWaylandSeat* seat = m_compositor ? m_compositor->defaultSeat() : nullptr;
                                                                                                                                                                                                                                                                if (seat) {
                                                                                                                                                                                                                                                                seat->setKeyboardFocus(nullptr);
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                m_keyboardGrabOwner = nullptr;
                                                                                                                                                                                                                                                                qDebug() << "[WMLayerShell] keyboard released by" << rec->nameSpace;
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                                                                                                                                                                // Cleanup helpers
                                                                                                                                                                                                                                                                // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                                                                                                                                                                void WMLayerShell::cleanupRecord(LayerSurfaceRecord* rec) {
                                                                                                                                                                                                                                                                Q_ASSERT(rec);

                                                                                                                                                                                                                                                                bool wasPanel = rec->mapped && rec->state.isPanel();

                                                                                                                                                                                                                                                                // Release keyboard grab if this surface held it.
                                                                                                                                                                                                                                                                if (m_keyboardGrabOwner == rec) {
                                                                                                                                                                                                                                                                ungrabKeyboard(rec);
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                rec->mapped = false;
                                                                                                                                                                                                                                                                rec->closed = true;

                                                                                                                                                                                                                                                                emit layerSurfaceDestroyed(rec);

                                                                                                                                                                                                                                                                if (wasPanel) {
                                                                                                                                                                                                                                                                emit workAreaChanged();
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                m_records.removeOne(rec);
                                                                                                                                                                                                                                                                delete rec;
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                void WMLayerShell::onSurfaceDestroyed() {
                                                                                                                                                                                                                                                                // sender() is the WMSurface that just got destroyed.
                                                                                                                                                                                                                                                                auto* surface = qobject_cast<WMSurface*>(sender());
                                                                                                                                                                                                                                                                if (!surface) return;

                                                                                                                                                                                                                                                                LayerSurfaceRecord* rec = recordForSurface(surface);
                                                                                                                                                                                                                                                                if (!rec) return;

                                                                                                                                                                                                                                                                qDebug() << "[WMLayerShell] surface destroyed for" << rec->nameSpace;
                                                                                                                                                                                                                                                                cleanupRecord(rec);
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                                                                                                                                                                // Utility
                                                                                                                                                                                                                                                                // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                                                                                                                                                                QWaylandOutput* WMLayerShell::outputForSurface(WMSurface* surface) const {
                                                                                                                                                                                                                                                                // In a multi-output setup we would check which output the surface is
                                                                                                                                                                                                                                                                // closest to.  For now we always return the primary output.
                                                                                                                                                                                                                                                                Q_UNUSED(surface);
                                                                                                                                                                                                                                                                return nullptr; // primary (null) output sentinel
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                LayerSurfaceRecord* WMLayerShell::recordForSurface(WMSurface* surface) const {
                                                                                                                                                                                                                                                                for (auto* rec : m_records) {
                                                                                                                                                                                                                                                                if (rec->surface == surface) return rec;
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                return nullptr;
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                bool WMLayerShell::layerLessThan(const LayerSurfaceRecord* a,
                                                                                                                                                                                                                                                                const LayerSurfaceRecord* b)
                                                                                                                                                                                                                                                                {
                                                                                                                                                                                                                                                                return static_cast<uint32_t>(a->state.layer)
                                                                                                                                                                                                                                                                < static_cast<uint32_t>(b->state.layer);
                                                                                                                                                                                                                                                                }
