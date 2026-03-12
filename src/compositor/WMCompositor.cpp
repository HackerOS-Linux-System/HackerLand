#include "WMCompositor.h"
#include "compositor/LockScreen.h"
#include "ui/AppLauncher.h"
#include "WMOutput.h"
#include "WMSurface.h"
#include "core/Window.h"
#include "core/Workspace.h"
#include "core/Config.h"
#include "ui/BarWidget.h"
#include "ui/AppLauncher.h"
#include "ui/NotificationOverlay.h"

#include <QWaylandOutput>
#include <QWaylandXdgShell>
#include <QWaylandXdgDecorationManagerV1>
#include <QWaylandSeat>
#include <QProcess>
#include <QDebug>
#include <QScreen>
#include <QGuiApplication>

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

WMCompositor::WMCompositor(QObject* parent)
: QWaylandCompositor(parent)
, m_animEngine(this)
{
}

WMCompositor::~WMCompositor() {
    qDeleteAll(m_workspaces);
}

// ─────────────────────────────────────────────────────────────────────────────
// initialize
// ─────────────────────────────────────────────────────────────────────────────

bool WMCompositor::initialize() {
    if (m_initialized) return true;

    QWaylandCompositor::create();

    setupWorkspaces();
    setupOutputs();
    setupShell();
    setupBar();

    m_initialized = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Setup helpers
// ─────────────────────────────────────────────────────────────────────────────

void WMCompositor::setupWorkspaces() {
    int count = Config::instance().workspaceCount();
    for (int i = 1; i <= count; ++i) {
        auto* ws = new Workspace(i, this);
        m_workspaces.append(ws);
        connect(ws, &Workspace::retileRequested, this, [this](const QRect&) {
            emit tiledWindowsChanged();
        });
    }
    m_activeWorkspaceId = 1;
    if (!m_workspaces.isEmpty())
        m_workspaces[0]->setActive(true);
}

void WMCompositor::setupOutputs() {
    const auto screens = QGuiApplication::screens();
    for (auto* screen : screens) {
        auto* output = new QWaylandOutput(this, nullptr);
        output->setManufacturer("HackerOS");
        output->setModel("Virtual");

        QWaylandOutputMode mode(screen->geometry().size(), 60000);
        output->addMode(mode, true);
        output->setCurrentMode(mode);

        if (!m_primaryOutput) {
            m_primaryOutput = new WMOutput(this, screen, this);
        }
        break; // single-screen for now
    }
}

void WMCompositor::setupShell() {
    // ── xdg_wm_base ───────────────────────────────────────────────────────
    m_xdgShell = new QWaylandXdgShell(this);
    connect(m_xdgShell, &QWaylandXdgShell::toplevelCreated,
            this, &WMCompositor::onXdgToplevelCreated);
    connect(m_xdgShell, &QWaylandXdgShell::popupCreated,
            this, &WMCompositor::onXdgPopupCreated);

    // xdg_decoration_manager_v1 is handled entirely by WMXdgShell, which
    // creates its own QWaylandXdgDecorationManagerV1 and sets ServerSideDecoration.
    // We do NOT create a second instance here — doing so would register two
    // conflicting decoration managers with the same compositor.

    connect(this, &QWaylandCompositor::surfaceCreated,
            this, &WMCompositor::onSurfaceCreated);
}

void WMCompositor::setupBar() {
    if (!m_primaryOutput) return;

    m_bar      = new BarWidget(this, nullptr);
    m_launcher = new AppLauncher(nullptr);
    m_notif    = new NotificationOverlay(nullptr);

    connect(m_bar, &BarWidget::workspaceSwitchRequested,
            this, &WMCompositor::switchWorkspace);
    connect(m_bar, &BarWidget::launchRequested,
            this, &WMCompositor::launchApp);
}

// ─────────────────────────────────────────────────────────────────────────────
// show / shutdown
// ─────────────────────────────────────────────────────────────────────────────

void WMCompositor::show() {
    if (m_primaryOutput) m_primaryOutput->show();
    if (m_bar)           m_bar->show();
}

void WMCompositor::shutdown() {
    for (auto* w : allWindows())
        w->close();
}

// ─────────────────────────────────────────────────────────────────────────────
// Surface created
// ─────────────────────────────────────────────────────────────────────────────

void WMCompositor::onSurfaceCreated(QWaylandSurface* surface) {
    auto* wms = new WMSurface(surface, this);
    m_surfaceMap.insert(surface, wms);

    connect(surface, &QWaylandSurface::destroyed, this, [this, surface, wms]() {
        m_surfaceMap.remove(surface);
        wms->deleteLater();
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Toplevel created
// ─────────────────────────────────────────────────────────────────────────────

void WMCompositor::onXdgToplevelCreated(QWaylandXdgToplevel* toplevel,
                                        QWaylandXdgSurface*  xdgSurface) {
    auto* surface = xdgSurface->surface();
    auto* wms = m_surfaceMap.value(surface);
    if (!wms) {
        wms = new WMSurface(surface, this);
        m_surfaceMap.insert(surface, wms);
    }

    auto* w = new Window(wms, this);
    m_toplevelMap.insert(toplevel, w);

    w->setTitle(toplevel->title());
    w->setAppId(toplevel->appId());

    // Property changes
    connect(toplevel, &QWaylandXdgToplevel::titleChanged, w,
            [w, toplevel]() { w->setTitle(toplevel->title()); });
    connect(toplevel, &QWaylandXdgToplevel::appIdChanged, w,
            [w, toplevel]() { w->setAppId(toplevel->appId()); });
    connect(toplevel, &QWaylandXdgToplevel::startMove, this,
            [w]() { w->setState(WindowState::Floating); });

    // Client asks to close
    connect(w, &Window::closeRequested, this, [toplevel]() {
        toplevel->sendClose();
    });

    // Toplevel destroyed — clean up window model
    connect(toplevel, &QObject::destroyed, this, [this, toplevel, w]() {
        removeWindowFromSystem(w);
        m_toplevelMap.remove(toplevel);
        w->deleteLater();
    });

    applyWindowRules(w);
    addWindowToSystem(w);

    emit windowAdded(w);
    qDebug() << "[WMCompositor] window added:" << w->title() << w->appId();
                                        }

                                        void WMCompositor::onXdgPopupCreated(QWaylandXdgPopup*, QWaylandXdgSurface*) {
                                            // Popups render on the surface tree; not tile-managed.
                                        }

                                        // ─────────────────────────────────────────────────────────────────────────────
                                        // Window lifecycle
                                        // ─────────────────────────────────────────────────────────────────────────────

                                        void WMCompositor::addWindowToSystem(Window* w) {
                                            auto* ws = activeWorkspace();
                                            if (!ws) return;

                                            ws->addWindow(w);
                                            retileWorkspace(ws);

                                            // Animate open
                                            const auto tiles = ws->computeTiles(workArea());
                                            for (const auto& t : tiles) {
                                                if (t.window == w) {
                                                    m_animEngine.animateWindowOpen(w, t.targetGeometry);
                                                    break;
                                                }
                                            }

                                            ws->setActiveWindow(w);
                                            emit activeWindowChanged(w);
                                        }

                                        void WMCompositor::removeWindowFromSystem(Window* w) {
                                            for (auto* ws : m_workspaces) {
                                                if (ws->contains(w)) {
                                                    ws->removeWindow(w);
                                                    retileWorkspace(ws);
                                                    break;
                                                }
                                            }
                                            emit windowRemoved(w);
                                            emit activeWindowChanged(activeWindow());
                                        }

                                        // ─────────────────────────────────────────────────────────────────────────────
                                        // Window rules
                                        // ─────────────────────────────────────────────────────────────────────────────

                                        void WMCompositor::applyWindowRules(Window* w) {
                                            const QString appId = w->appId().toLower();

                                            static const QStringList floatApps = {
                                                "pavucontrol", "nm-connection-editor", "thunar", "mpv",
                                                "gimp", "inkscape", "vlc"
                                            };

                                            for (const auto& app : floatApps) {
                                                if (appId.contains(app)) {
                                                    w->setState(WindowState::Floating);
                                                    const QRect area = workArea();
                                                    const QSize sz(800, 600);
                                                    w->setGeometry(QRect(
                                                        area.x() + (area.width()  - sz.width())  / 2,
                                                                         area.y() + (area.height() - sz.height()) / 2,
                                                                         sz.width(), sz.height()));
                                                    return;
                                                }
                                            }
                                        }

                                        // ─────────────────────────────────────────────────────────────────────────────
                                        // Private slots
                                        // ─────────────────────────────────────────────────────────────────────────────

                                        void WMCompositor::onWindowCloseRequested() {
                                            // handled via per-window lambda connections
                                        }

                                        void WMCompositor::onWindowTitleChanged(const QString&) {
                                            emit tiledWindowsChanged();
                                        }

                                        void WMCompositor::retileCurrentWorkspace() {
                                            if (auto* ws = activeWorkspace())
                                                retileWorkspace(ws);
                                        }

                                        // ─────────────────────────────────────────────────────────────────────────────
                                        // Tiling
                                        // ─────────────────────────────────────────────────────────────────────────────

                                        void WMCompositor::retileWorkspace(Workspace* ws) {
                                            if (!ws) return;
                                            const auto tiles = ws->computeTiles(workArea());
                                            for (const auto& t : tiles) {
                                                if (t.window->geometry() != t.targetGeometry
                                                    && !m_animEngine.isAnimating(t.window)) {
                                                    m_animEngine.animateWindowMove(
                                                        t.window, t.window->geometry(), t.targetGeometry);
                                                    }
                                            }
                                            emit tiledWindowsChanged();
                                        }

                                        // ─────────────────────────────────────────────────────────────────────────────
                                        // Lookups / accessors
                                        // ─────────────────────────────────────────────────────────────────────────────

                                        Workspace* WMCompositor::workspace(int id) const {
                                            for (auto* ws : m_workspaces) {
                                                if (ws->id() == id) return ws;
                                            }
                                            return nullptr;
                                        }

                                        Workspace* WMCompositor::activeWorkspace() const {
                                            return workspace(m_activeWorkspaceId);
                                        }

                                        Window* WMCompositor::activeWindow() const {
                                            auto* ws = activeWorkspace();
                                            return ws ? ws->activeWindow() : nullptr;
                                        }

                                        QList<Window*> WMCompositor::allWindows() const {
                                            QList<Window*> all;
                                            for (auto* ws : m_workspaces)
                                                all.append(ws->windows());
                                            return all;
                                        }

                                        Window* WMCompositor::windowFromToplevel(QWaylandXdgToplevel* toplevel) const {
                                            return m_toplevelMap.value(toplevel, nullptr);
                                        }

                                        QRect WMCompositor::workArea() const {
                                            if (!m_primaryOutput) return {0, 0, 1920, 1080};
                                            const auto geom = m_primaryOutput->screen()->geometry();
                                            const int  barH = Config::instance().theme.barHeight;
                                            return {geom.x(), geom.y() + barH, geom.width(), geom.height() - barH};
                                        }

                                        // ─────────────────────────────────────────────────────────────────────────────
                                        // Workspace switching
                                        // ─────────────────────────────────────────────────────────────────────────────

                                        void WMCompositor::switchWorkspace(int id) {
                                            if (id == m_activeWorkspaceId) return;
                                            if (!workspace(id)) return;

                                            auto* oldWs = activeWorkspace();
                                            auto* newWs = workspace(id);

                                            if (oldWs) {
                                                for (auto* w : oldWs->windows())
                                                    w->setVisible(false);
                                                oldWs->setActive(false);
                                            }

                                            m_activeWorkspaceId = id;
                                            newWs->setActive(true);

                                            for (auto* w : newWs->windows())
                                                w->setVisible(true);

                                            retileWorkspace(newWs);
                                            emit activeWorkspaceChanged(id);

                                            if (!newWs->activeWindow() && !newWs->windows().isEmpty())
                                                newWs->setActiveWindow(newWs->windows().last());

                                            emit activeWindowChanged(newWs->activeWindow());
                                        }

                                        void WMCompositor::moveWindowToWorkspace(Window* w, int workspaceId) {
                                            auto* ws = workspace(workspaceId);
                                            if (!ws || !w) return;

                                            for (auto* fromWs : m_workspaces) {
                                                if (fromWs->contains(w)) {
                                                    fromWs->removeWindow(w);
                                                    retileWorkspace(fromWs);
                                                    break;
                                                }
                                            }

                                            ws->addWindow(w);
                                            w->setVisible(workspaceId == m_activeWorkspaceId);
                                            retileWorkspace(ws);
                                        }

                                        // ─────────────────────────────────────────────────────────────────────────────
                                        // Window actions
                                        // ─────────────────────────────────────────────────────────────────────────────

                                        void WMCompositor::closeWindow(Window* w) {
                                            if (!w) return;
                                            m_animEngine.animateWindowClose(w, [w]() { w->close(); });
                                        }

                                        void WMCompositor::focusWindow(Window* w) {
                                            if (!w) return;
                                            for (auto* ws : m_workspaces) {
                                                if (ws->contains(w)) {
                                                    ws->setActiveWindow(w);
                                                    break;
                                                }
                                            }
                                            emit activeWindowChanged(w);
                                        }

                                        void WMCompositor::focusDirection(const QString& dir) {
                                            auto* ws = activeWorkspace();
                                            if (!ws) return;
                                            if (dir == "next" || dir == "right" || dir == "down")
                                                ws->focusNext();
                                            else
                                                ws->focusPrev();
                                            emit activeWindowChanged(ws->activeWindow());
                                        }

                                        void WMCompositor::moveWindowDirection(const QString& dir) {
                                            auto* ws = activeWorkspace();
                                            if (!ws || !ws->activeWindow()) return;

                                            auto windows = ws->windows();
                                            const int idx    = windows.indexOf(ws->activeWindow());
                                            int       newIdx = idx;

                                            if (dir == "right" || dir == "down")
                                                newIdx = qMin(idx + 1, windows.size() - 1);
                                            else
                                                newIdx = qMax(idx - 1, 0);

                                            if (newIdx != idx) {
                                                windows.swapItemsAt(idx, newIdx);
                                                retileWorkspace(ws);
                                            }
                                        }

                                        void WMCompositor::cycleLayout() {
                                            auto* ws = activeWorkspace();
                                            if (!ws) return;
                                            const int l = ((int)ws->layout() + 1) % 6;
                                            ws->setLayout((TilingLayout)l);
                                            retileWorkspace(ws);
                                        }

                                        void WMCompositor::setLayout(TilingLayout l) {
                                            auto* ws = activeWorkspace();
                                            if (!ws) return;
                                            ws->setLayout(l);
                                            retileWorkspace(ws);
                                        }

                                        void WMCompositor::toggleFloat(Window* w) {
                                            auto* win = w ? w : activeWindow();
                                            if (!win) return;
                                            win->toggleFloat();
                                            retileCurrentWorkspace();
                                        }

                                        void WMCompositor::toggleFullscreen(Window* w) {
                                            auto* win = w ? w : activeWindow();
                                            if (!win) return;
                                            win->toggleFullscreen();
                                            if (win->isFullscreen())
                                                win->setGeometry(primaryOutput()->screen()->geometry());
                                            else
                                                retileCurrentWorkspace();
                                        }

                                        void WMCompositor::toggleMaximize(Window* w) {
                                            auto* win = w ? w : activeWindow();
                                            if (!win) return;
                                            win->toggleMaximize();
                                            if (win->isMaximized())
                                                win->setGeometry(workArea());
                                            else
                                                retileCurrentWorkspace();
                                        }

                                        void WMCompositor::launchApp(const QString& cmd) {
                                            QProcess::startDetached("bash", {"-c", cmd});
                                        }

                                        void WMCompositor::reloadConfig() {
                                            Config::instance().load(QString());
                                            retileCurrentWorkspace();
                                            emit tiledWindowsChanged();
                                        }

                                        void WMCompositor::lockScreen() {
                                            if (!m_lockScreen) {
                                                m_lockScreen = new LockScreen(nullptr);
                                                connect(m_lockScreen, &LockScreen::unlocked, this, [this]{
                                                    if (m_primaryOutput) m_primaryOutput->setFocus();
                                                });
                                            }
                                            m_lockScreen->lock();
                                        }

                                        void WMCompositor::showLauncher() {
                                            if (m_launcher) {
                                                m_launcher->show();
                                                m_launcher->raise();
                                                m_launcher->activateWindow();
                                            }
                                        }

                                        void WMCompositor::dispatchAction(const QString& action) {
                                            handleKeybind(action);
                                        }
