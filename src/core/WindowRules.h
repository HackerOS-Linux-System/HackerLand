#pragma once
#include <QObject>
#include <QString>
#include <QList>
#include <QSize>

class Window;

// ─────────────────────────────────────────────────────────────────────────────
// WindowRule — one rule loaded from config [rules] section
// ─────────────────────────────────────────────────────────────────────────────
struct WindowRule {
    // Match criteria (empty = wildcard)
    QString matchAppId;     // e.g. "firefox", "org.kde.*"
    QString matchTitle;     // substring / glob

    // Actions
    int     workspace   = -1;       // -1 = don't change
    bool    floating    = false;
    bool    fullscreen  = false;
    bool    noDecor     = false;
    QSize   size        = {};       // 0,0 = don't change
    QString opacity;                // "" = don't change, "0.9" etc
    int     xwayland    = -1;       // -1 = auto, 1 = force x11
};

class WindowRules : public QObject {
    Q_OBJECT
public:
    explicit WindowRules(QObject* parent = nullptr);

    void loadFromConfig();
    void apply(Window* w) const;
    void addRule(const WindowRule& r) { m_rules.append(r); }
    void clear() { m_rules.clear(); }

private:
    bool matches(const WindowRule& r, const Window* w) const;
    QList<WindowRule> m_rules;
};
