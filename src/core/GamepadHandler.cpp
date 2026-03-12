#include "GamepadHandler.h"

#include <QDebug>
#include <fcntl.h>
#include <unistd.h>
#include <linux/joystick.h>

// Standard Linux joystick button map (PS/Xbox layout)
// BTN 0=A/Cross  1=B/Circle  2=X/Square  3=Y/Tri
// BTN 4=L1  5=R1  6=L2  7=R2  8=Select  9=Start
const QHash<int,QString> GamepadHandler::s_buttonMap = {
    {4, "workspace:prev"},  // L1
    {5, "workspace:next"},  // R1
    {8, "close"},           // Select
    {9, "exec:alacritty"},  // Start → terminal
    {0, "focus:right"},     // A
    {3, "focus:left"},      // Y
    {1, "focus:down"},      // B
    {2, "focus:up"},        // X
};

GamepadHandler::GamepadHandler(QObject* parent) : QObject(parent) {
    m_timer = new QTimer(this);
    m_timer->setInterval(16); // poll at ~60Hz
    connect(m_timer, &QTimer::timeout, this, &GamepadHandler::readEvents);
}

GamepadHandler::~GamepadHandler() { stop(); }

bool GamepadHandler::start() {
    // Try js0 through js3
    for (int i = 0; i < 4; ++i) {
        const QString path = QString("/dev/input/js%1").arg(i);
        m_fd = ::open(path.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK);
        if (m_fd >= 0) {
            qInfo() << "[Gamepad] opened" << path;
            m_timer->start();
            return true;
        }
    }
    qDebug() << "[Gamepad] no joystick device found";
    return false;
}

void GamepadHandler::stop() {
    m_timer->stop();
    if (m_fd >= 0) { ::close(m_fd); m_fd = -1; }
}

void GamepadHandler::readEvents() {
    if (m_fd < 0) return;
    struct js_event ev;
    while (::read(m_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        const int type = ev.type & ~JS_EVENT_INIT;
        if (type == JS_EVENT_BUTTON) handleButton(ev.number, ev.value != 0);
        if (type == JS_EVENT_AXIS)   handleAxis(ev.number,   ev.value);
    }
}

void GamepadHandler::handleButton(int button, bool pressed) {
    if (button == 8) { m_selectHeld = pressed; return; } // Select held

    if (!pressed) return;

    // Select+A = close window
    if (m_selectHeld && button == 0) {
        emit actionTriggered("close"); return;
    }

    const QString action = s_buttonMap.value(button);
    if (!action.isEmpty()) emit actionTriggered(action);
}

void GamepadHandler::handleAxis(int axis, int value) {
    // D-pad: axis 6 = LR, axis 7 = UD  (value -32767/0/32767)
    const int thresh = 16000;
    if (axis == 6) {
        if (value < -thresh) emit actionTriggered("focus:left");
        if (value >  thresh) emit actionTriggered("focus:right");
    } else if (axis == 7) {
        if (value < -thresh) emit actionTriggered("focus:up");
        if (value >  thresh) emit actionTriggered("focus:down");
    }
}
