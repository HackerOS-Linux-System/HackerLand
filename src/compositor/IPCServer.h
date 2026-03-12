#pragma once
#include <QObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QList>

class WMCompositor;

// ─────────────────────────────────────────────────────────────────────────────
// IPCServer — Unix socket IPC, compatible with i3/Sway CLI format
//
// Socket path: $XDG_RUNTIME_DIR/hackerlandwm.sock
// Usage:  hackerlandwm-msg workspace 3
//         hackerlandwm-msg exec alacritty
//         hackerlandwm-msg reload
//         hackerlandwm-msg layout spiral
//         hackerlandwm-msg close
//
// Protocol: newline-terminated plain text commands
// Response: JSON {"success":true} or {"success":false,"error":"..."}
// ─────────────────────────────────────────────────────────────────────────────
class IPCServer : public QObject {
    Q_OBJECT
public:
    explicit IPCServer(WMCompositor* compositor, QObject* parent = nullptr);
    ~IPCServer() override;

    bool start();
    void stop();
    bool isRunning() const;

    static QString socketPath();

signals:
    void commandReceived(const QString& cmd);

private slots:
    void onNewConnection();
    void onClientData();
    void onClientDisconnected();

private:
    void handleCommand(QLocalSocket* client, const QString& cmd);
    void sendResponse(QLocalSocket* client, bool ok, const QString& msg = {});

    WMCompositor*       m_compositor = nullptr;
    QLocalServer*       m_server     = nullptr;
    QList<QLocalSocket*> m_clients;
};
