#include "Workspace.h"
#include "Window.h"
#include "Config.h"
#include "TilingEngine.h"

#include <QDebug>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

Workspace::Workspace(int id, QObject* parent)
: QObject(parent)
, m_id(id)
, m_name(QString::number(id))
{
    const auto& cfg = Config::instance();

    // Inherit layout and tiling parameters from the global config.
    m_layout = TilingEngine::layoutFromString(cfg.tiling.layout);

    m_engine.setMasterRatio(cfg.tiling.masterRatio);
    m_engine.setGaps(cfg.theme.gapInner, cfg.theme.gapOuter);
    m_engine.setSmartGaps(cfg.tiling.smartGaps);
    m_engine.setSmartBorders(cfg.tiling.smartBorders);
    m_engine.setMaxColumns(cfg.tiling.maxColumns);

    // Re-apply engine settings whenever the user reloads the config.
    connect(&Config::instance(), &Config::configReloaded,
            this, &Workspace::onConfigReloaded);

    qDebug() << "[Workspace" << m_id << "] created, layout:"
    << TilingEngine::layoutToString(m_layout);
}

Workspace::~Workspace() {
    // Windows are owned by WMCompositor, not by us.  Just clear the reference.
    // Any active-window signal would fire into dead objects if we emitted here,
    // so we suppress it during destruction.
    m_activeWindow = nullptr;
    m_windows.clear();
    m_focusHistory.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// Config reload
// ─────────────────────────────────────────────────────────────────────────────

void Workspace::onConfigReloaded() {
    const auto& cfg = Config::instance();
    m_engine.setMasterRatio(cfg.tiling.masterRatio);
    m_engine.setGaps(cfg.theme.gapInner, cfg.theme.gapOuter);
    m_engine.setSmartGaps(cfg.tiling.smartGaps);
    m_engine.setSmartBorders(cfg.tiling.smartBorders);
    m_engine.setMaxColumns(cfg.tiling.maxColumns);
    qDebug() << "[Workspace" << m_id << "] engine settings reloaded from config";
}

// ─────────────────────────────────────────────────────────────────────────────
// Layout
// ─────────────────────────────────────────────────────────────────────────────

void Workspace::setLayout(TilingLayout l) {
    if (m_layout == l) return;
    m_layout = l;
    qDebug() << "[Workspace" << m_id << "] layout changed to:"
    << TilingEngine::layoutToString(l);
    emit layoutChanged(l);
}

void Workspace::cycleLayoutForward() {
    // Cycle through all nine layouts in a logical progression.
    static const TilingLayout cycle[] = {
        TilingLayout::Spiral,
        TilingLayout::Tall,
        TilingLayout::Wide,
        TilingLayout::ThreeColumn,
        TilingLayout::Grid,
        TilingLayout::Dwindle,
        TilingLayout::BSP,
        TilingLayout::Monocle,
        TilingLayout::Centered
    };
    static constexpr int kCount = static_cast<int>(
        sizeof(cycle) / sizeof(cycle[0]));

    int cur = 0;
    for (int i = 0; i < kCount; ++i) {
        if (cycle[i] == m_layout) { cur = i; break; }
    }
    setLayout(cycle[(cur + 1) % kCount]);
}

void Workspace::cycleLayoutBackward() {
    static const TilingLayout cycle[] = {
        TilingLayout::Spiral,
        TilingLayout::Tall,
        TilingLayout::Wide,
        TilingLayout::ThreeColumn,
        TilingLayout::Grid,
        TilingLayout::Dwindle,
        TilingLayout::BSP,
        TilingLayout::Monocle,
        TilingLayout::Centered
    };
    static constexpr int kCount = static_cast<int>(
        sizeof(cycle) / sizeof(cycle[0]));

    int cur = 0;
    for (int i = 0; i < kCount; ++i) {
        if (cycle[i] == m_layout) { cur = i; break; }
    }
    setLayout(cycle[(cur - 1 + kCount) % kCount]);
}

// ─────────────────────────────────────────────────────────────────────────────
// Window management — add / remove
// ─────────────────────────────────────────────────────────────────────────────

void Workspace::addWindow(Window* w) {
    Q_ASSERT(w);
    if (m_windows.contains(w)) return;

    // Connect window signals so the workspace can react to state changes.
    connectWindowSignals(w);

    m_windows.append(w);
    w->setWorkspace(m_id);

    // Update tile slots for all tiled windows.
    refreshTileSlots();

    qDebug() << "[Workspace" << m_id << "] window added:"
    << w->title() << "(id=" << w->id() << ") total=" << m_windows.size();

    emit windowAdded(w);
}

void Workspace::removeWindow(Window* w) {
    Q_ASSERT(w);
    if (!m_windows.contains(w)) return;

    // Disconnect all signals we previously connected.
    disconnectWindowSignals(w);

    m_windows.removeOne(w);
    m_focusHistory.removeAll(w);

    // If the removed window was active, promote the most-recently-focused
    // window that is still on this workspace.
    if (m_activeWindow == w) {
        m_activeWindow = nullptr;
        Window* next = mostRecentlyFocused();
        if (!next && !m_windows.isEmpty()) {
            next = m_windows.last();
        }
        // Set active without going through setActiveWindow() so we don't
        // deactivate the already-removed window.
        m_activeWindow = next;
        if (m_activeWindow) m_activeWindow->setActive(true);
        emit activeWindowChanged(m_activeWindow);
    }

    refreshTileSlots();

    qDebug() << "[Workspace" << m_id << "] window removed:"
    << w->title() << "remaining=" << m_windows.size();

    emit windowRemoved(w);
}

bool Workspace::contains(Window* w) const {
    return m_windows.contains(w);
}

// ─────────────────────────────────────────────────────────────────────────────
// Focus management
// ─────────────────────────────────────────────────────────────────────────────

void Workspace::setActiveWindow(Window* w) {
    // Allow nullptr to explicitly clear focus.
    if (m_activeWindow == w) return;

    // Deactivate previous.
    if (m_activeWindow) {
        m_activeWindow->setActive(false);
    }

    m_activeWindow = w;

    if (w) {
        // Only manage focus for windows that actually live here.
        if (!m_windows.contains(w)) {
            qWarning() << "[Workspace" << m_id << "] setActiveWindow called with"
            << "window not in this workspace:" << w->title();
            m_activeWindow = nullptr;
            return;
        }

        w->setActive(true);

        // Push to focus history, keeping the list compact.
        m_focusHistory.removeAll(w);
        m_focusHistory.prepend(w);

        // Cap history size to avoid unbounded growth.
        while (m_focusHistory.size() > kMaxFocusHistory) {
            m_focusHistory.removeLast();
        }
    }

    qDebug() << "[Workspace" << m_id << "] active window:"
    << (w ? w->title() : "<none>");

    emit activeWindowChanged(w);
}

void Workspace::focusNext() {
    QList<Window*> focusable = focusableWindows();
    if (focusable.isEmpty()) return;

    int idx = m_activeWindow ? focusable.indexOf(m_activeWindow) : -1;
    int next = (idx + 1) % focusable.size();
    setActiveWindow(focusable[next]);
}

void Workspace::focusPrev() {
    QList<Window*> focusable = focusableWindows();
    if (focusable.isEmpty()) return;

    int idx = m_activeWindow ? focusable.indexOf(m_activeWindow) : 0;
    int prev = (idx - 1 + focusable.size()) % focusable.size();
    setActiveWindow(focusable[prev]);
}

void Workspace::focusDirection(FocusDirection dir) {
    if (!m_activeWindow || m_windows.size() < 2) return;

    QList<Window*> candidates = focusableWindows();
    if (candidates.size() < 2) return;

    const QRect cur = m_activeWindow->geometry();
    const QPoint curCenter = cur.center();

    Window* best   = nullptr;
    int     bestScore = INT_MAX;

    for (auto* w : candidates) {
        if (w == m_activeWindow) continue;

        const QPoint wc = w->geometry().center();
        const int dx = wc.x() - curCenter.x();
        const int dy = wc.y() - curCenter.y();

        // Determine if the candidate lies in the requested direction.
        // We use a 45-degree sector test so diagonal neighbours are reachable.
        bool inSector = false;
        int  primary  = 0; // smaller = closer along primary axis
        int  secondary = 0;

        switch (dir) {
            case FocusDirection::Left:
                inSector  = (dx < 0) && (qAbs(dx) >= qAbs(dy));
                primary   = -dx;
                secondary = qAbs(dy);
                break;
            case FocusDirection::Right:
                inSector  = (dx > 0) && (qAbs(dx) >= qAbs(dy));
                primary   = dx;
                secondary = qAbs(dy);
                break;
            case FocusDirection::Up:
                inSector  = (dy < 0) && (qAbs(dy) >= qAbs(dx));
                primary   = -dy;
                secondary = qAbs(dx);
                break;
            case FocusDirection::Down:
                inSector  = (dy > 0) && (qAbs(dy) >= qAbs(dx));
                primary   = dy;
                secondary = qAbs(dx);
                break;
        }

        if (!inSector) continue;

        // Score: prefer windows close on the primary axis, then secondary.
        int score = primary * 4 + secondary;
        if (score < bestScore) {
            bestScore = score;
            best      = w;
        }
    }

    if (best) {
        setActiveWindow(best);
    } else {
        // No window in that direction — wrap around to the other side.
        focusNext();
    }
}

void Workspace::focusMaster() {
    QList<Window*> tiled = tiledWindows();
    if (tiled.isEmpty()) return;
    setActiveWindow(tiled.first());
}

void Workspace::focusLast() {
    // Re-focus the window that had focus before the current one.
    for (auto* w : m_focusHistory) {
        if (w != m_activeWindow && m_windows.contains(w) && w->isVisible()) {
            setActiveWindow(w);
            return;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Window ordering / swap
// ─────────────────────────────────────────────────────────────────────────────

void Workspace::swapWindows(Window* a, Window* b) {
    if (!a || !b || a == b) return;
    int ia = m_windows.indexOf(a);
    int ib = m_windows.indexOf(b);
    if (ia < 0 || ib < 0) return;
    m_windows.swapItemsAt(ia, ib);
    refreshTileSlots();
    qDebug() << "[Workspace" << m_id << "] swapped:"
    << a->title() << "<->" << b->title();
}

void Workspace::swapWithMaster() {
    QList<Window*> tiled = tiledWindows();
    if (tiled.isEmpty() || !m_activeWindow) return;
    if (m_activeWindow == tiled.first()) return;
    swapWindows(m_activeWindow, tiled.first());
}

void Workspace::moveWindowUp(Window* w) {
    if (!w) w = m_activeWindow;
    if (!w) return;
    int idx = m_windows.indexOf(w);
    if (idx <= 0) return;
    m_windows.swapItemsAt(idx, idx - 1);
    refreshTileSlots();
}

void Workspace::moveWindowDown(Window* w) {
    if (!w) w = m_activeWindow;
    if (!w) return;
    int idx = m_windows.indexOf(w);
    if (idx < 0 || idx >= m_windows.size() - 1) return;
    m_windows.swapItemsAt(idx, idx + 1);
    refreshTileSlots();
}

void Workspace::moveWindowToTop(Window* w) {
    if (!w) w = m_activeWindow;
    if (!w) return;
    if (!m_windows.removeOne(w)) return;
    m_windows.prepend(w);
    refreshTileSlots();
}

// ─────────────────────────────────────────────────────────────────────────────
// Master ratio
// ─────────────────────────────────────────────────────────────────────────────

void Workspace::setMasterRatio(float ratio) {
    m_engine.setMasterRatio(ratio);
    emit masterRatioChanged(m_engine.masterRatio());
}

void Workspace::adjustMasterRatio(float delta) {
    m_engine.adjustMasterRatio(delta);
    emit masterRatioChanged(m_engine.masterRatio());
}

// ─────────────────────────────────────────────────────────────────────────────
// Tiling computation
// ─────────────────────────────────────────────────────────────────────────────

QList<TileResult> Workspace::computeTiles(const QRect& area) const {
    return m_engine.tile(m_windows, area, m_layout);
}

void Workspace::retile(const QRect& area) {
    if (area.isEmpty()) return;

    m_lastArea = area;
    auto tiles = computeTiles(area);

    for (const auto& t : tiles) {
        if (t.window && t.window->geometry() != t.targetGeometry) {
            t.window->setGeometry(t.targetGeometry);
        }
    }

    emit retileRequested(area);
}

void Workspace::retileWithAnimation(const QRect& area) {
    // Identical to retile() but uses setGeometryAnimated() so the
    // AnimationEngine gets to interpolate each window to its new position.
    if (area.isEmpty()) return;

    m_lastArea = area;
    auto tiles = computeTiles(area);

    for (const auto& t : tiles) {
        if (t.window && t.window->geometry() != t.targetGeometry) {
            t.window->setGeometryAnimated(t.targetGeometry);
        }
    }

    emit retileRequested(area);
}

// ─────────────────────────────────────────────────────────────────────────────
// Window visibility helpers
// ─────────────────────────────────────────────────────────────────────────────

void Workspace::showAllWindows() {
    for (auto* w : m_windows) {
        w->setVisible(true);
    }
}

void Workspace::hideAllWindows() {
    for (auto* w : m_windows) {
        w->setVisible(false);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Filtered window lists
// ─────────────────────────────────────────────────────────────────────────────

QList<Window*> Workspace::tiledWindows() const {
    QList<Window*> result;
    for (auto* w : m_windows) {
        if (w->isVisible() && !w->isFloating()
            && !w->isFullscreen() && !w->isMaximized())
        {
            result.append(w);
        }
    }
    return result;
}

QList<Window*> Workspace::floatingWindows() const {
    QList<Window*> result;
    for (auto* w : m_windows) {
        if (w->isVisible() && w->isFloating()) result.append(w);
    }
    return result;
}

QList<Window*> Workspace::visibleWindows() const {
    QList<Window*> result;
    for (auto* w : m_windows) {
        if (w->isVisible()) result.append(w);
    }
    return result;
}

QList<Window*> Workspace::focusableWindows() const {
    QList<Window*> result;
    for (auto* w : m_windows) {
        if (w->isVisible()) result.append(w);
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Private helpers
// ─────────────────────────────────────────────────────────────────────────────

void Workspace::connectWindowSignals(Window* w) {
    // When a window changes state (e.g. client requests float/fullscreen) the
    // workspace must retile so the layout reflects the new window count.
    connect(w, &Window::stateChanged, this, [this](WindowState) {
        if (!m_lastArea.isEmpty()) {
            retileWithAnimation(m_lastArea);
        }
    });

    // If visibility changes (minimise / restore) retile immediately.
    connect(w, &Window::geometryChanged, this, [this, w](const QRect& newRect) {
        // Only care about geometry changes we didn't initiate (e.g. client
        // resizing itself).  Update m_lastArea if it covers the new rect.
        Q_UNUSED(w);
        Q_UNUSED(newRect);
        // No action needed here — WMCompositor drives retiling.
    });
}

void Workspace::disconnectWindowSignals(Window* w) {
    disconnect(w, nullptr, this, nullptr);
}

void Workspace::refreshTileSlots() {
    // Assign consecutive slot indices to tiled windows so the BSP layout
    // and other slot-aware algorithms have stable references.
    int slot = 0;
    for (auto* w : m_windows) {
        if (!w->isFloating() && !w->isFullscreen()) {
            w->setTileSlot(slot++);
        } else {
            w->setTileSlot(-1);
        }
    }
}

Window* Workspace::mostRecentlyFocused() const {
    for (auto* w : m_focusHistory) {
        if (m_windows.contains(w) && w->isVisible()) return w;
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Window signal reactions (connected in connectWindowSignals)
// ─────────────────────────────────────────────────────────────────────────────

void Workspace::onWindowStateChanged(WindowState state) {
    auto* w = qobject_cast<Window*>(sender());
    if (!w || !m_windows.contains(w)) return;

    qDebug() << "[Workspace" << m_id << "] window state changed:"
    << w->title() << (int)state;

    refreshTileSlots();

    // When a window becomes fullscreen or maximized, push it to the top of
    // the visual stack by making sure it is last in the window list.
    if (state == WindowState::Fullscreen || state == WindowState::Maximized) {
        if (m_windows.last() != w) {
            m_windows.removeOne(w);
            m_windows.append(w);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Debug helpers
// ─────────────────────────────────────────────────────────────────────────────

QString Workspace::debugString() const {
    QString s = QString("[Workspace %1 \"%2\" layout=%3 windows=%4")
    .arg(m_id)
    .arg(m_name)
    .arg(TilingEngine::layoutToString(m_layout))
    .arg(m_windows.size());

    if (m_activeWindow) {
        s += QString(" active=\"%1\"").arg(m_activeWindow->title());
    }
    s += "]";
    return s;
}
