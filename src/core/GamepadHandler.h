#pragma once
#include <QObject>
#include <QTimer>
#include <QString>
#include <QHash>

// ─────────────────────────────────────────────────────────────────────────────
// GamepadHandler — reads /dev/input/js* or evdev gamepad events
// and emits actionTriggered() matching WM keybind format.
//
// Supports: workspace switching (L1/R1), window focus (dpad),
//           launch terminal (Start), close window (Select+A)
// ─────────────────────────────────────────────────────────────────────────────
class GamepadHandler : public QObject {
    Q_OBJECT
public:
    explicit GamepadHandler(QObject* parent = nullptr);
    ~GamepadHandler() override;

    bool start();   // opens /dev/input/js0, returns false if not found
    void stop();
    bool isActive() const { return m_fd >= 0; }

signals:
    void actionTriggered(const QString& action);

private slots:
    void readEvents();

private:
    void handleButton(int button, bool pressed);
    void handleAxis(int axis, int value);

    int     m_fd    = -1;
    QTimer* m_timer = nullptr;

    bool m_selectHeld = false;

    // Button → action mapping
    static const QHash<int,QString> s_buttonMap;
};
