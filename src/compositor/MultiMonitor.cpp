#include "MultiMonitor.h"
#include "WMCompositor.h"
#include "WMOutput.h"

#include <QGuiApplication>
#include <QDebug>

MultiMonitor::MultiMonitor(WMCompositor* compositor, QObject* parent)
: QObject(parent), m_compositor(compositor)
{
    connect(qApp, &QGuiApplication::screenAdded,
            this, &MultiMonitor::onScreenAdded);
    connect(qApp, &QGuiApplication::screenRemoved,
            this, &MultiMonitor::onScreenRemoved);
}

MultiMonitor::~MultiMonitor() { teardown(); }

void MultiMonitor::setup() {
    const auto screens = QGuiApplication::screens();
    QScreen* primary   = QGuiApplication::primaryScreen();

    for (auto* s : screens)
        addMonitor(s, s == primary);

    qInfo() << "[MultiMonitor]" << m_monitors.size() << "monitor(s) initialized";
}

void MultiMonitor::teardown() {
    for (auto& m : m_monitors) {
        if (m.output) { m.output->hide(); delete m.output; }
    }
    m_monitors.clear();
}

void MultiMonitor::addMonitor(QScreen* s, bool primary) {
    if (outputForScreen(s)) return; // already added

    auto* out = new WMOutput(m_compositor, s, m_compositor);
    MonitorInfo info;
    info.screen  = s;
    info.output  = out;
    info.primary = primary;
    m_monitors.append(info);

    out->show();
    qInfo() << "[MultiMonitor] added" << s->name()
    << s->geometry() << (primary ? "(primary)" : "");
}

void MultiMonitor::removeMonitor(QScreen* s) {
    for (int i = 0; i < m_monitors.size(); ++i) {
        if (m_monitors[i].screen == s) {
            m_monitors[i].output->hide();
            delete m_monitors[i].output;
            m_monitors.removeAt(i);
            qInfo() << "[MultiMonitor] removed" << s->name();
            return;
        }
    }
}

void MultiMonitor::onScreenAdded(QScreen* s) {
    addMonitor(s, false);
}

void MultiMonitor::onScreenRemoved(QScreen* s) {
    removeMonitor(s);
}

WMOutput* MultiMonitor::primaryOutput() const {
    for (const auto& m : m_monitors)
        if (m.primary) return m.output;
        return m_monitors.isEmpty() ? nullptr : m_monitors.first().output;
}

WMOutput* MultiMonitor::outputForScreen(QScreen* s) const {
    for (const auto& m : m_monitors)
        if (m.screen == s) return m.output;
        return nullptr;
}

QRect MultiMonitor::totalGeometry() const {
    QRect total;
    for (const auto& m : m_monitors)
        total = total.united(m.screen->geometry());
    return total;
}
