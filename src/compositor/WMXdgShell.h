#pragma once

#include <QObject>
#include <QHash>
#include <QList>
#include <QSize>
#include <QRect>
#include <QPoint>

#include <QWaylandXdgShell>
#include <QWaylandXdgDecorationManagerV1>

#include "core/Window.h"   // WindowType

class WMCompositor;
class WMSurface;

// QWaylandXdgToplevelDecorationV1 is declared inside QWaylandXdgDecorationManagerV1
// but is NOT fully exported on all Qt6 builds — forward-declare it here so
// it can appear in private member storage (as QObject*) and in non-slot
// private methods without MOC ever seeing the real type in a slot signature.
class QWaylandXdgToplevelDecorationV1;

// ─────────────────────────────────────────────────────────────────────────────
// WMXdgShell
// ─────────────────────────────────────────────────────────────────────────────
class WMXdgShell : public QObject {
    Q_OBJECT

public:
    explicit WMXdgShell(WMCompositor* compositor, QObject* parent = nullptr);
    ~WMXdgShell() override;

    void initialize();
    void sendConfigure(Window* w);
    void sendClose(Window* w);

    Window*              windowForToplevel(QWaylandXdgToplevel* toplevel) const;
    QWaylandXdgToplevel* toplevelForWindow(Window* w)                     const;

signals:
    void windowCreated  (Window* w);
    void windowDestroyed(Window* w);
    void popupCreated   (QWaylandXdgPopup* popup, WMSurface* surface);

private slots:
    // ── xdg_wm_base ──────────────────────────────────────────────────────
    void onToplevelCreated(QWaylandXdgToplevel* toplevel,
                           QWaylandXdgSurface*  xdgSurface);
    void onPopupCreated   (QWaylandXdgPopup*    popup,
                           QWaylandXdgSurface*  xdgSurface);

    // ── xdg_toplevel property changes ────────────────────────────────────
    void onTitleChanged();
    void onAppIdChanged();
    void onMinimumSizeChanged();
    void onMaximumSizeChanged();

    // ── xdg_toplevel state requests ───────────────────────────────────────
    void onFullscreenRequested(QWaylandOutput* preferredOutput);
    void onUnfullscreenRequested();
    void onMaximizeRequested();
    void onUnmaximizeRequested();
    void onMinimizeRequested();

    // ── xdg_toplevel interactive operations ───────────────────────────────
    void onStartMove  (QWaylandSeat* seat, uint serial);
    void onStartResize(QWaylandSeat* seat, uint serial, Qt::Edges edges);

    // ── destroy ───────────────────────────────────────────────────────────
    void onToplevelDestroyed();
    void onPopupDestroyed();

    // NOTE: onDecorationCreated is NOT a slot — it's a plain private method
    // called from a lambda in initialize(). This avoids MOC needing the full
    // definition of QWaylandXdgToplevelDecorationV1.

private:
    // Called from lambda in initialize() — NOT a Qt slot.
    // Uses a template parameter to avoid depending on QWaylandXdgToplevelDecorationV1
    // which is not publicly declared in this Qt6 build.
    template<typename DecorationT>
    void onDecorationCreated(DecorationT* decoration);

    // ── Other helpers ──────────────────────────────────────────────────────
    WMSurface* surfaceFor(QWaylandSurface* surface) const;

    Window* createWindowForToplevel(QWaylandXdgToplevel* toplevel,
                                    QWaylandXdgSurface*  xdgSurface);

    QRect      initialGeometry   (QWaylandXdgToplevel* toplevel) const;
    WindowType classifyWindowType(QWaylandXdgToplevel* toplevel) const;
    void       applyWindowRules  (Window* w,
                                  QWaylandXdgToplevel* toplevel) const;
                                  void       doSendConfigure   (QWaylandXdgToplevel* toplevel,
                                                                Window* w) const;

                                                                // ── Members ───────────────────────────────────────────────────────────
                                                                WMCompositor*                   m_compositor = nullptr;
                                                                QWaylandXdgShell*               m_xdgShell   = nullptr;
                                                                QWaylandXdgDecorationManagerV1* m_decoration = nullptr;

                                                                QHash<QWaylandXdgToplevel*, Window*>             m_toplevelToWindow;
                                                                QHash<Window*, QWaylandXdgToplevel*>             m_windowToToplevel;
                                                                QList<QWaylandXdgPopup*>                         m_activePopups;

                                                                // Stored as QObject* so QHash instantiation never exposes the incomplete
                                                                // type to the linker. Cast to QWaylandXdgToplevelDecorationV1* in .cpp.
                                                                QHash<QWaylandXdgToplevel*, QObject*>            m_decorations;
};
