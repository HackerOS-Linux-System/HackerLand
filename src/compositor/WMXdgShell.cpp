#include "WMXdgShell.h"
#include "WMCompositor.h"
#include "WMSurface.h"
#include "WMOutput.h"            // full definition needed for ->screen()->geometry()
#include "core/Window.h"
#include "core/Config.h"
#include "core/Workspace.h"

#include <QWaylandCompositor>
#include <QWaylandSurface>
#include <QWaylandOutput>
#include <QWaylandSeat>
#include <QWaylandXdgShell>
#include <QWaylandXdgDecorationManagerV1>
#include <QGuiApplication>
#include <QScreen>
#include <QRegularExpression>
#include <QDebug>

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

WMXdgShell::WMXdgShell(WMCompositor* compositor, QObject* parent)
: QObject(parent)
, m_compositor(compositor)
{
    Q_ASSERT(compositor);
}

WMXdgShell::~WMXdgShell() {
    m_toplevelToWindow.clear();
    m_windowToToplevel.clear();
    m_activePopups.clear();
    m_decorations.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// Initialisation
// ─────────────────────────────────────────────────────────────────────────────

void WMXdgShell::initialize() {
    // ── xdg_wm_base ──────────────────────────────────────────────────────
    m_xdgShell = new QWaylandXdgShell(m_compositor);

    connect(m_xdgShell, &QWaylandXdgShell::toplevelCreated,
            this,       &WMXdgShell::onToplevelCreated);

    connect(m_xdgShell, &QWaylandXdgShell::popupCreated,
            this,       &WMXdgShell::onPopupCreated);

    // ── xdg_decoration_manager_v1 ─────────────────────────────────────────
    // Qt6: constructor takes no args; register via setExtensionContainer then
    // call initialize().  The manager advertises server-side decoration to all
    // new toplevels automatically — no per-toplevel signal needed.
    m_decoration = new QWaylandXdgDecorationManagerV1();
    m_decoration->setExtensionContainer(m_compositor);
    m_decoration->initialize();

    // Note: QWaylandXdgDecorationManagerV1 in this Qt6 build does NOT expose
    // a decorationCreated signal.  Decoration mode is enforced per-toplevel
    // inside onToplevelCreated() via the decoration object stored in the
    // manager's internal map, or simply by the manager's preferred mode.

    qDebug() << "[WMXdgShell] initialized — xdg_wm_base + xdg_decoration ready";
}

// ─────────────────────────────────────────────────────────────────────────────
// xdg_toplevel created
// ─────────────────────────────────────────────────────────────────────────────

void WMXdgShell::onToplevelCreated(QWaylandXdgToplevel* toplevel,
                                   QWaylandXdgSurface*  xdgSurface) {
    Q_ASSERT(toplevel);
    Q_ASSERT(xdgSurface);

    qDebug() << "[WMXdgShell] xdg_toplevel created:"
    << toplevel->title() << "|" << toplevel->appId();

    // ── Property-change signals (Qt6 names) ──────────────────────────────
    connect(toplevel, &QWaylandXdgToplevel::titleChanged,
            this,     &WMXdgShell::onTitleChanged);

    connect(toplevel, &QWaylandXdgToplevel::appIdChanged,
            this,     &WMXdgShell::onAppIdChanged);

    // Qt6: minSizeChanged / maxSizeChanged  (NOT minimumSizeChanged/maximumSizeChanged)
    connect(toplevel, &QWaylandXdgToplevel::minSizeChanged,
            this,     &WMXdgShell::onMinimumSizeChanged);

    connect(toplevel, &QWaylandXdgToplevel::maxSizeChanged,
            this,     &WMXdgShell::onMaximumSizeChanged);

    // ── State-request signals (Qt6 names) ────────────────────────────────
    // Qt6: setFullscreen(QWaylandOutput*) / unsetFullscreen()
    //      setMaximized() / unsetMaximized() / setMinimized()
    connect(toplevel, &QWaylandXdgToplevel::setFullscreen,
            this,     &WMXdgShell::onFullscreenRequested);

    connect(toplevel, &QWaylandXdgToplevel::unsetFullscreen,
            this,     &WMXdgShell::onUnfullscreenRequested);

    connect(toplevel, &QWaylandXdgToplevel::setMaximized,
            this,     &WMXdgShell::onMaximizeRequested);

    connect(toplevel, &QWaylandXdgToplevel::unsetMaximized,
            this,     &WMXdgShell::onUnmaximizeRequested);

    connect(toplevel, &QWaylandXdgToplevel::setMinimized,
            this,     &WMXdgShell::onMinimizeRequested);

    // ── Interactive move / resize ─────────────────────────────────────────
    // Qt6: startMove(QWaylandSeat*)  — NO serial parameter
    connect(toplevel, &QWaylandXdgToplevel::startMove,
            this, [this](QWaylandSeat* seat) {
                onStartMove(seat, 0);
            });

    // Qt6: startResize(QWaylandSeat*, Qt::Edges)  — NO serial parameter
    connect(toplevel, &QWaylandXdgToplevel::startResize,
            this, [this](QWaylandSeat* seat, Qt::Edges edges) {
                onStartResize(seat, 0, edges);
            });

    // ── Destruction ───────────────────────────────────────────────────────
    connect(toplevel, &QWaylandXdgToplevel::destroyed,
            this,     &WMXdgShell::onToplevelDestroyed);

    // ── Create Window model ───────────────────────────────────────────────
    Window* w = createWindowForToplevel(toplevel, xdgSurface);
    doSendConfigure(toplevel, w);

    emit windowCreated(w);
                                   }

                                   // ─────────────────────────────────────────────────────────────────────────────
                                   // Window model creation
                                   // ─────────────────────────────────────────────────────────────────────────────

                                   Window* WMXdgShell::createWindowForToplevel(QWaylandXdgToplevel* toplevel,
                                                                               QWaylandXdgSurface*  xdgSurface) {
                                       QWaylandSurface* qwlSurface = xdgSurface->surface();
                                       WMSurface* wms = surfaceFor(qwlSurface);
                                       if (!wms) {
                                           wms = new WMSurface(qwlSurface, m_compositor, m_compositor);
                                       }
                                       wms->setRole(WMSurface::Role::XdgToplevel);

                                       auto* w = new Window(wms, m_compositor);
                                       w->setTitle(toplevel->title());
                                       w->setAppId(toplevel->appId());
                                       w->setType(classifyWindowType(toplevel));

                                       // Qt6: minSize() / maxSize()  (NOT minimumSize() / maximumSize())
                                       QSize minSz = toplevel->minSize();
                                       QSize maxSz = toplevel->maxSize();
                                       if (minSz.isValid() && !minSz.isEmpty())              w->setMinSize(minSz);
                                       if (maxSz.isValid() && maxSz.width() > 0
                                           && maxSz.height() > 0)            w->setMaxSize(maxSz);

                                       QRect initGeom = initialGeometry(toplevel);
                                       w->setGeometry(initGeom);
                                       applyWindowRules(w, toplevel);
                                       wms->setWindow(w);

                                       m_toplevelToWindow.insert(toplevel, w);
                                       m_windowToToplevel.insert(w, toplevel);

                                       connect(w, &Window::closeRequested, this, [this, toplevel]() {
                                           sendClose(windowForToplevel(toplevel));
                                       });
                                       connect(w, &Window::stateChanged, this, [this, toplevel, w](WindowState) {
                                           doSendConfigure(toplevel, w);
                                       });
                                       connect(w, &Window::geometryChanged, this, [this, toplevel, w](const QRect&) {
                                           doSendConfigure(toplevel, w);
                                       });

                                       qDebug() << "[WMXdgShell] Window created: id=" << w->id()
                                       << "title=" << w->title() << "geom=" << initGeom;
                                       return w;
                                                                               }

                                                                               // ─────────────────────────────────────────────────────────────────────────────
                                                                               // Initial geometry
                                                                               // ─────────────────────────────────────────────────────────────────────────────

                                                                               QRect WMXdgShell::initialGeometry(QWaylandXdgToplevel* toplevel) const {
                                                                                   QRect workArea = m_compositor->workArea();
                                                                                   auto& tileCfg  = Config::instance().tiling;

                                                                                   if (toplevel->parentToplevel()) {
                                                                                       Window* parent = windowForToplevel(toplevel->parentToplevel());
                                                                                       if (parent) {
                                                                                           QRect pr = parent->geometry();
                                                                                           int w = qMax(400, (int)(pr.width()  * 0.65));
                                                                                           int h = qMax(300, (int)(pr.height() * 0.65));
                                                                                           int x = pr.x() + (pr.width()  - w) / 2;
                                                                                           int y = pr.y() + (pr.height() - h) / 2;
                                                                                           return { x, y, w, h };
                                                                                       }
                                                                                   }

                                                                                   // Qt6: minSize()
                                                                                   QSize minSz = toplevel->minSize();
                                                                                   QSize hint;
                                                                                   if (minSz.isValid() && minSz.width() > 100 && minSz.height() > 50)
                                                                                       hint = minSz;

                                                                                   int w = hint.isValid() ? qMax(hint.width(),  500)
                                                                                   : (int)(workArea.width()  * tileCfg.masterRatio);
                                                                                   int h = hint.isValid() ? qMax(hint.height(), 400)
                                                                                   : (int)(workArea.height() * 0.75f);
                                                                                   int x = workArea.x() + (workArea.width()  - w) / 2;
                                                                                   int y = workArea.y() + (workArea.height() - h) / 2;
                                                                                   return { x, y, w, h };
                                                                               }

                                                                               // ─────────────────────────────────────────────────────────────────────────────
                                                                               // Window type classification
                                                                               // ─────────────────────────────────────────────────────────────────────────────

                                                                               WindowType WMXdgShell::classifyWindowType(QWaylandXdgToplevel* toplevel) const {
                                                                                   if (toplevel->parentToplevel()) return WindowType::Dialog;

                                                                                   const QString appId = toplevel->appId().toLower();
                                                                                   static const QStringList dialogAppIds = {
                                                                                       "pavucontrol", "nm-connection-editor", "blueman-manager",
                                                                                       "gnome-calculator", "kcalc", "font-manager",
                                                                                       "lxappearance", "qt5ct", "qt6ct", "kvantum"
                                                                                   };
                                                                                   for (const auto& id : dialogAppIds) {
                                                                                       if (appId.contains(id)) return WindowType::Dialog;
                                                                                   }
                                                                                   return WindowType::Normal;
                                                                               }

                                                                               // ─────────────────────────────────────────────────────────────────────────────
                                                                               // Window rules
                                                                               // ─────────────────────────────────────────────────────────────────────────────

                                                                               void WMXdgShell::applyWindowRules(Window* w,
                                                                                                                 QWaylandXdgToplevel* toplevel) const {
                                                                                                                     const QString appId   = w->appId().toLower();
                                                                                                                     const QRect   workArea = m_compositor->workArea();

                                                                                                                     if (w->type() == WindowType::Dialog) {
                                                                                                                         w->setState(WindowState::Floating);
                                                                                                                         return;
                                                                                                                     }

                                                                                                                     static const QStringList floatApps = {
                                                                                                                         "pavucontrol", "nm-connection-editor", "blueman-manager",
                                                                                                                         "lxappearance", "qt5ct", "qt6ct", "kvantum",
                                                                                                                         "gnome-calculator", "kcalc", "mpv", "feh", "eog", "gimp"
                                                                                                                     };
                                                                                                                     for (const auto& app : floatApps) {
                                                                                                                         if (appId.contains(app)) {
                                                                                                                             w->setState(WindowState::Floating);
                                                                                                                             QSize sz { qMin(900, (int)(workArea.width()  * 0.55)),
                                                                                                                                 qMin(700, (int)(workArea.height() * 0.65)) };
                                                                                                                                 w->setGeometry({
                                                                                                                                     workArea.x() + (workArea.width()  - sz.width())  / 2,
                                                                                                                                                workArea.y() + (workArea.height() - sz.height()) / 2,
                                                                                                                                                sz.width(), sz.height()
                                                                                                                                 });
                                                                                                                                 qDebug() << "[WMXdgShell] rule: float" << appId;
                                                                                                                                 return;
                                                                                                                         }
                                                                                                                     }

                                                                                                                     Q_UNUSED(toplevel);
                                                                                                                 }

                                                                                                                 // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                 // xdg_popup created
                                                                                                                 // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                 void WMXdgShell::onPopupCreated(QWaylandXdgPopup*   popup,
                                                                                                                                                 QWaylandXdgSurface* xdgSurface) {
                                                                                                                     Q_ASSERT(popup);
                                                                                                                     Q_ASSERT(xdgSurface);
                                                                                                                     qDebug() << "[WMXdgShell] xdg_popup created";

                                                                                                                     m_activePopups.append(popup);

                                                                                                                     QWaylandSurface* qwlSurface = xdgSurface->surface();
                                                                                                                     WMSurface* wms = surfaceFor(qwlSurface);
                                                                                                                     if (!wms) wms = new WMSurface(qwlSurface, m_compositor, m_compositor);
                                                                                                                     wms->setRole(WMSurface::Role::XdgPopup);

                                                                                                                     connect(popup, &QWaylandXdgPopup::destroyed,
                                                                                                                             this,  &WMXdgShell::onPopupDestroyed);

                                                                                                                     emit popupCreated(popup, wms);
                                                                                                                                                 }

                                                                                                                                                 // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                                                 // Property-change callbacks
                                                                                                                                                 // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                                                 void WMXdgShell::onTitleChanged() {
                                                                                                                                                     auto* toplevel = qobject_cast<QWaylandXdgToplevel*>(sender());
                                                                                                                                                     if (!toplevel) return;
                                                                                                                                                     Window* w = m_toplevelToWindow.value(toplevel);
                                                                                                                                                     if (w) w->setTitle(toplevel->title());
                                                                                                                                                 }

                                                                                                                                                 void WMXdgShell::onAppIdChanged() {
                                                                                                                                                     auto* toplevel = qobject_cast<QWaylandXdgToplevel*>(sender());
                                                                                                                                                     if (!toplevel) return;
                                                                                                                                                     Window* w = m_toplevelToWindow.value(toplevel);
                                                                                                                                                     if (w) w->setAppId(toplevel->appId());
                                                                                                                                                 }

                                                                                                                                                 void WMXdgShell::onMinimumSizeChanged() {
                                                                                                                                                     auto* toplevel = qobject_cast<QWaylandXdgToplevel*>(sender());
                                                                                                                                                     if (!toplevel) return;
                                                                                                                                                     Window* w = m_toplevelToWindow.value(toplevel);
                                                                                                                                                     if (!w) return;
                                                                                                                                                     // Qt6: minSize()
                                                                                                                                                     QSize minSz = toplevel->minSize();
                                                                                                                                                     if (minSz.isValid() && !minSz.isEmpty()) w->setMinSize(minSz);
                                                                                                                                                 }

                                                                                                                                                 void WMXdgShell::onMaximumSizeChanged() {
                                                                                                                                                     auto* toplevel = qobject_cast<QWaylandXdgToplevel*>(sender());
                                                                                                                                                     if (!toplevel) return;
                                                                                                                                                     Window* w = m_toplevelToWindow.value(toplevel);
                                                                                                                                                     if (!w) return;
                                                                                                                                                     // Qt6: maxSize()
                                                                                                                                                     QSize maxSz = toplevel->maxSize();
                                                                                                                                                     if (maxSz.isValid() && maxSz.width() > 0 && maxSz.height() > 0)
                                                                                                                                                         w->setMaxSize(maxSz);
                                                                                                                                                 }

                                                                                                                                                 // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                                                 // State-request callbacks
                                                                                                                                                 // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                                                 void WMXdgShell::onFullscreenRequested(QWaylandOutput* preferredOutput) {
                                                                                                                                                     auto* toplevel = qobject_cast<QWaylandXdgToplevel*>(sender());
                                                                                                                                                     if (!toplevel) return;
                                                                                                                                                     Window* w = m_toplevelToWindow.value(toplevel);
                                                                                                                                                     if (!w) return;

                                                                                                                                                     QRect fsGeom;
                                                                                                                                                     if (preferredOutput) {
                                                                                                                                                         fsGeom = QRect(QPoint(0, 0), preferredOutput->geometry().size());
                                                                                                                                                     } else if (m_compositor->primaryOutput()) {
                                                                                                                                                         fsGeom = m_compositor->primaryOutput()->screen()->geometry();
                                                                                                                                                     } else {
                                                                                                                                                         fsGeom = QGuiApplication::primaryScreen()->geometry();
                                                                                                                                                     }

                                                                                                                                                     w->setState(WindowState::Fullscreen);
                                                                                                                                                     w->setGeometry(fsGeom);
                                                                                                                                                 }

                                                                                                                                                 void WMXdgShell::onUnfullscreenRequested() {
                                                                                                                                                     auto* toplevel = qobject_cast<QWaylandXdgToplevel*>(sender());
                                                                                                                                                     if (!toplevel) return;
                                                                                                                                                     Window* w = m_toplevelToWindow.value(toplevel);
                                                                                                                                                     if (w) w->setState(WindowState::Tiled);
                                                                                                                                                 }

                                                                                                                                                 void WMXdgShell::onMaximizeRequested() {
                                                                                                                                                     auto* toplevel = qobject_cast<QWaylandXdgToplevel*>(sender());
                                                                                                                                                     if (!toplevel) return;
                                                                                                                                                     Window* w = m_toplevelToWindow.value(toplevel);
                                                                                                                                                     if (!w) return;
                                                                                                                                                     w->setState(WindowState::Maximized);
                                                                                                                                                     w->setGeometry(m_compositor->workArea());
                                                                                                                                                 }

                                                                                                                                                 void WMXdgShell::onUnmaximizeRequested() {
                                                                                                                                                     auto* toplevel = qobject_cast<QWaylandXdgToplevel*>(sender());
                                                                                                                                                     if (!toplevel) return;
                                                                                                                                                     Window* w = m_toplevelToWindow.value(toplevel);
                                                                                                                                                     if (w) w->setState(WindowState::Tiled);
                                                                                                                                                 }

                                                                                                                                                 void WMXdgShell::onMinimizeRequested() {
                                                                                                                                                     auto* toplevel = qobject_cast<QWaylandXdgToplevel*>(sender());
                                                                                                                                                     if (!toplevel) return;
                                                                                                                                                     Window* w = m_toplevelToWindow.value(toplevel);
                                                                                                                                                     if (!w) return;
                                                                                                                                                     w->setState(WindowState::Minimized);
                                                                                                                                                     w->setVisible(false);
                                                                                                                                                 }

                                                                                                                                                 // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                                                 // Interactive move / resize
                                                                                                                                                 // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                                                 void WMXdgShell::onStartMove(QWaylandSeat* seat, uint serial) {
                                                                                                                                                     Q_UNUSED(seat); Q_UNUSED(serial);
                                                                                                                                                     auto* toplevel = qobject_cast<QWaylandXdgToplevel*>(sender());
                                                                                                                                                     if (!toplevel) return;
                                                                                                                                                     Window* w = m_toplevelToWindow.value(toplevel);
                                                                                                                                                     if (!w) return;
                                                                                                                                                     if (!w->isFloating()) {
                                                                                                                                                         w->setState(WindowState::Floating);
                                                                                                                                                         doSendConfigure(toplevel, w);
                                                                                                                                                     }
                                                                                                                                                 }

                                                                                                                                                 void WMXdgShell::onStartResize(QWaylandSeat* seat, uint serial, Qt::Edges edges) {
                                                                                                                                                     Q_UNUSED(seat); Q_UNUSED(serial); Q_UNUSED(edges);
                                                                                                                                                     auto* toplevel = qobject_cast<QWaylandXdgToplevel*>(sender());
                                                                                                                                                     if (!toplevel) return;
                                                                                                                                                     Window* w = m_toplevelToWindow.value(toplevel);
                                                                                                                                                     if (!w) return;
                                                                                                                                                     if (!w->isFloating()) {
                                                                                                                                                         w->setState(WindowState::Floating);
                                                                                                                                                         doSendConfigure(toplevel, w);
                                                                                                                                                     }
                                                                                                                                                 }

                                                                                                                                                 // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                                                 // Configure helpers
                                                                                                                                                 // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                                                 void WMXdgShell::sendConfigure(Window* w) {
                                                                                                                                                     if (!w) return;
                                                                                                                                                     QWaylandXdgToplevel* toplevel = m_windowToToplevel.value(w);
                                                                                                                                                     if (toplevel) doSendConfigure(toplevel, w);
                                                                                                                                                 }

                                                                                                                                                 void WMXdgShell::doSendConfigure(QWaylandXdgToplevel* toplevel,
                                                                                                                                                                                  Window*              w) const {
                                                                                                                                                                                      if (!toplevel || !w) return;

                                                                                                                                                                                      // Qt6: sendConfigure takes QList<QWaylandXdgToplevel::State>
                                                                                                                                                                                      // Enum values: MaximizedState, FullscreenState, ResizingState, ActivatedState
                                                                                                                                                                                      // (NO States typedef/QFlags — it's a plain QList)
                                                                                                                                                                                      QList<QWaylandXdgToplevel::State> states;

                                                                                                                                                                                      switch (w->state()) {
                                                                                                                                                                                          case WindowState::Maximized:
                                                                                                                                                                                              states << QWaylandXdgToplevel::State::MaximizedState;
                                                                                                                                                                                              break;
                                                                                                                                                                                          case WindowState::Fullscreen:
                                                                                                                                                                                              states << QWaylandXdgToplevel::State::FullscreenState;
                                                                                                                                                                                              break;
                                                                                                                                                                                          default:
                                                                                                                                                                                              break;
                                                                                                                                                                                      }

                                                                                                                                                                                      if (w->isActive())
                                                                                                                                                                                          states << QWaylandXdgToplevel::State::ActivatedState;

                                                                                                                                                                                      QSize configSize { 0, 0 };
                                                                                                                                                                                      if (w->isFullscreen() || w->isMaximized())
                                                                                                                                                                                          configSize = w->geometry().size();

                                                                                                                                                                                      toplevel->sendConfigure(configSize, states);

                                                                                                                                                                                      qDebug() << "[WMXdgShell] configure sent to" << w->title()
                                                                                                                                                                                      << "size=" << configSize << "states=" << states.size();
                                                                                                                                                                                  }

                                                                                                                                                                                  void WMXdgShell::sendClose(Window* w) {
                                                                                                                                                                                      if (!w) return;
                                                                                                                                                                                      QWaylandXdgToplevel* toplevel = m_windowToToplevel.value(w);
                                                                                                                                                                                      if (toplevel) toplevel->sendClose();
                                                                                                                                                                                  }

                                                                                                                                                                                  // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                                                                                  // Toplevel / popup destroyed
                                                                                                                                                                                  // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                                                                                  void WMXdgShell::onToplevelDestroyed() {
                                                                                                                                                                                      auto* toplevel = qobject_cast<QWaylandXdgToplevel*>(sender());
                                                                                                                                                                                      if (!toplevel) return;

                                                                                                                                                                                      Window* w = m_toplevelToWindow.take(toplevel);
                                                                                                                                                                                      if (!w) return;

                                                                                                                                                                                      m_windowToToplevel.remove(w);
                                                                                                                                                                                      m_decorations.remove(toplevel);

                                                                                                                                                                                      qDebug() << "[WMXdgShell] toplevel destroyed:" << w->title();
                                                                                                                                                                                      emit windowDestroyed(w);
                                                                                                                                                                                      w->deleteLater();
                                                                                                                                                                                  }

                                                                                                                                                                                  void WMXdgShell::onPopupDestroyed() {
                                                                                                                                                                                      auto* popup = qobject_cast<QWaylandXdgPopup*>(sender());
                                                                                                                                                                                      if (popup) m_activePopups.removeOne(popup);
                                                                                                                                                                                      qDebug() << "[WMXdgShell] popup destroyed";
                                                                                                                                                                                  }

                                                                                                                                                                                  // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                                                                                  // Decoration (template — defined here so the instantiation is in this TU)
                                                                                                                                                                                  // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                                                                                  template<typename DecorationT>
                                                                                                                                                                                  void WMXdgShell::onDecorationCreated(DecorationT* decoration) {
                                                                                                                                                                                      if (!decoration) return;
                                                                                                                                                                                      // ServerSideDecoration = 2 in xdg-decoration-unstable-v1 protocol
                                                                                                                                                                                      decoration->setMode(static_cast<decltype(decoration->mode())>(2));
                                                                                                                                                                                      auto* toplevel = decoration->toplevel();
                                                                                                                                                                                      if (toplevel)
                                                                                                                                                                                          m_decorations.insert(toplevel, static_cast<QObject*>(decoration));
                                                                                                                                                                                      qDebug() << "[WMXdgShell] decoration created — mode: ServerSide";
                                                                                                                                                                                  }

                                                                                                                                                                                  // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                                                                                  // Lookup helpers
                                                                                                                                                                                  // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                                                                                  Window* WMXdgShell::windowForToplevel(QWaylandXdgToplevel* toplevel) const {
                                                                                                                                                                                      return m_toplevelToWindow.value(toplevel, nullptr);
                                                                                                                                                                                  }

                                                                                                                                                                                  QWaylandXdgToplevel* WMXdgShell::toplevelForWindow(Window* w) const {
                                                                                                                                                                                      return m_windowToToplevel.value(w, nullptr);
                                                                                                                                                                                  }

                                                                                                                                                                                  WMSurface* WMXdgShell::surfaceFor(QWaylandSurface* surface) const {
                                                                                                                                                                                      for (auto* w : m_toplevelToWindow.values()) {
                                                                                                                                                                                          if (w && w->surface() && w->surface()->surface() == surface)
                                                                                                                                                                                              return w->surface();
                                                                                                                                                                                      }
                                                                                                                                                                                      return nullptr;
                                                                                                                                                                                  }
