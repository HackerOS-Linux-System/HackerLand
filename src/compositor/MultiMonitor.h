#pragma once
#include <QObject>
#include <QList>
#include <QRect>
#include <QScreen>

class WMCompositor;
class WMOutput;

// ─────────────────────────────────────────────────────────────────────────────
// MultiMonitor — manages multiple QScreens / WMOutputs
// Each screen gets its own WMOutput and independent workspace set.
// ─────────────────────────────────────────────────────────────────────────────
struct MonitorInfo {
    QScreen*  screen  = nullptr;
    WMOutput* output  = nullptr;
    int       primaryWorkspace = 1;
    bool      primary = false;
};

class MultiMonitor : public QObject {
    Q_OBJECT
public:
    explicit MultiMonitor(WMCompositor* compositor, QObject* parent = nullptr);
    ~MultiMonitor() override;

    void setup();
    void teardown();

    WMOutput* primaryOutput()     const;
    WMOutput* outputForScreen(QScreen* s) const;
    int        monitorCount()     const { return m_monitors.size(); }
    QList<MonitorInfo> monitors() const { return m_monitors; }

    // Total bounding rect of all monitors
    QRect totalGeometry() const;

private slots:
    void onScreenAdded  (QScreen* s);
    void onScreenRemoved(QScreen* s);

private:
    void addMonitor(QScreen* s, bool primary);
    void removeMonitor(QScreen* s);

    WMCompositor*      m_compositor = nullptr;
    QList<MonitorInfo> m_monitors;
};
