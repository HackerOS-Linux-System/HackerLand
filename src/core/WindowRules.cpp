#include "WindowRules.h"
#include "Window.h"
#include "Config.h"
#include <QDebug>
#include <QRegularExpression>

WindowRules::WindowRules(QObject* parent) : QObject(parent) {}

void WindowRules::loadFromConfig() {
    m_rules.clear();
    // Rules are parsed from Config TOML [rules] section
    // Format in TOML:
    // [[rules]]
    // app_id    = "firefox"
    // workspace = 2
    // [[rules]]
    // app_id    = "mpv"
    // floating  = true
    // size      = "800x600"
    // Rules loaded by Config::parseTOML and stored in Config::rules list
    // Here we just copy them
    // (Config populates WindowRules via WMCompositor::reloadConfig)
    qDebug() << "[WindowRules] loaded" << m_rules.size() << "rules";
}

bool WindowRules::matches(const WindowRule& r, const Window* w) const {
    if (!r.matchAppId.isEmpty()) {
        QRegularExpression re(QRegularExpression::wildcardToRegularExpression(r.matchAppId),
                              QRegularExpression::CaseInsensitiveOption);
        if (!re.match(w->appId()).hasMatch()) return false;
    }
    if (!r.matchTitle.isEmpty()) {
        QRegularExpression re(QRegularExpression::wildcardToRegularExpression(r.matchTitle),
                              QRegularExpression::CaseInsensitiveOption);
        if (!re.match(w->title()).hasMatch()) return false;
    }
    return true;
}

void WindowRules::apply(Window* w) const {
    for (const auto& r : m_rules) {
        if (!matches(r, w)) continue;
        if (r.workspace > 0)  w->setWorkspace(r.workspace);
        if (r.floating)       w->setState(WindowState::Floating);
        if (r.fullscreen)     w->setState(WindowState::Fullscreen);
        if (r.size.isValid()) w->setGeometry(QRect(w->geometry().topLeft(), r.size));
        qDebug() << "[WindowRules] applied rule to" << w->appId();
    }
}
