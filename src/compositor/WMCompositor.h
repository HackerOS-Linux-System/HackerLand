#pragma once

#include <QWaylandCompositor>
#include <QWaylandSeat>
#include <QWaylandOutput>
#include <QWaylandXdgShell>
#include <QList>
#include <QHash>
#include <memory>

#include "core/TilingEngine.h"
#include "core/AnimationEngine.h"
#include "core/Config.h"

class Window;
class Workspace;
class WMOutput;
class WMSurface;
class BarWidget;
class AppLauncher;
class LockScreen;
class NotificationOverlay;

class WMCompositor : public QWaylandCompositor {
    Q_OBJECT

public:
    explicit WMCompositor(QObject* parent = nullptr);
    ~WMCompositor() override;

    bool initialize();
    void show();
    void shutdown();

    // ── Workspace management ──────────────────────────────────────────────
    Workspace* workspace(int id) const;
    Workspace* activeWorkspace() const;
    void switchWorkspace(int id);
    void moveWindowToWorkspace(Window* w, int workspaceId);
    int  workspaceCount() const { return m_workspaces.size(); }

    // ── Window management ─────────────────────────────────────────────────
    Window*        activeWindow() const;
    void           closeWindow(Window* w);
    void           focusWindow(Window* w);
    QList<Window*> allWindows() const;

    // ── Actions ───────────────────────────────────────────────────────────
    void focusDirection(const QString& dir);
    void moveWindowDirection(const QString& dir);
    void cycleLayout();
    void setLayout(TilingLayout l);
    void toggleFloat(Window* w = nullptr);
    void toggleFullscreen(Window* w = nullptr);
    void toggleMaximize(Window* w = nullptr);
    void launchApp(const QString& cmd);
    void reloadConfig();
    void lockScreen();
    void showLauncher();

    // ── Geometry ──────────────────────────────────────────────────────────
    QRect     workArea() const;
    WMOutput* primaryOutput() const { return m_primaryOutput; }

    // ── Accessors for UI ──────────────────────────────────────────────────
    AnimationEngine* animEngine() { return &m_animEngine; }
    BarWidget*       bar()        { return m_bar; }

signals:
    void windowAdded           (Window* w);
    void windowRemoved         (Window* w);
    void activeWorkspaceChanged(int id);
    void activeWindowChanged   (Window* w);
    void tiledWindowsChanged   ();

public slots:
    /// Public entry point for external action dispatch (IPC, gamepad, etc.)
    void dispatchAction(const QString& action);

    void onXdgToplevelCreated(QWaylandXdgToplevel* toplevel,
                              QWaylandXdgSurface*  xdgSurface);
    void onXdgPopupCreated   (QWaylandXdgPopup*    popup,
                              QWaylandXdgSurface*  xdgSurface);
    void onSurfaceCreated    (QWaylandSurface*     surface);

private slots:
    void onWindowCloseRequested();
    void onWindowTitleChanged(const QString& title);
    void retileCurrentWorkspace();

private:
    // ── Setup ─────────────────────────────────────────────────────────────
    void setupWorkspaces();
    void setupOutputs();
    void setupShell();
    void setupBar();

    // ── Internal window lifecycle ─────────────────────────────────────────
    void addWindowToSystem    (Window* w);
    void removeWindowFromSystem(Window* w);

    // ── Helpers ───────────────────────────────────────────────────────────
    void     retileWorkspace(Workspace* ws);
    Window*  windowFromToplevel(QWaylandXdgToplevel* toplevel) const;
    void     handleKeybind(const QString& action);
    void     applyWindowRules(Window* w);

    // ── Protocol objects ──────────────────────────────────────────────────
    QWaylandXdgShell* m_xdgShell = nullptr;

    // ── Workspaces ────────────────────────────────────────────────────────
    QList<Workspace*> m_workspaces;
    int               m_activeWorkspaceId = 1;

    // ── Surface / window maps ─────────────────────────────────────────────
    QHash<QWaylandXdgToplevel*, Window*>    m_toplevelMap;
    QHash<QWaylandSurface*,     WMSurface*> m_surfaceMap;

    // ── UI objects ────────────────────────────────────────────────────────
    WMOutput*            m_primaryOutput = nullptr;
    BarWidget*           m_bar           = nullptr;
    AppLauncher*         m_launcher      = nullptr;
    LockScreen*          m_lockScreen    = nullptr;
    NotificationOverlay* m_notif         = nullptr;

    AnimationEngine m_animEngine;
    bool            m_initialized = false;
};
