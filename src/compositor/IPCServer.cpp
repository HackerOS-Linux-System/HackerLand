#include "IPCServer.h"
#include "WMCompositor.h"
#include "core/Config.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QDebug>

IPCServer::IPCServer(WMCompositor* compositor, QObject* parent)
: QObject(parent), m_compositor(compositor)
{
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    connect(m_server, &QLocalServer::newConnection,
            this, &IPCServer::onNewConnection);
}

IPCServer::~IPCServer() { stop(); }

QString IPCServer::socketPath() {
    const QString rdir = qgetenv("XDG_RUNTIME_DIR");
    return (rdir.isEmpty() ? "/tmp" : rdir) + "/hackerlandwm.sock";
}

bool IPCServer::start() {
    const QString path = socketPath();
    QLocalServer::removeServer(path); // clean up stale socket
    if (!m_server->listen(path)) {
        qWarning() << "[IPC] failed to listen on" << path << m_server->errorString();
        return false;
    }
    qInfo() << "[IPC] listening on" << path;
    return true;
}

void IPCServer::stop() {
    for (auto* c : m_clients) c->disconnectFromServer();
    m_clients.clear();
    m_server->close();
}

bool IPCServer::isRunning() const { return m_server->isListening(); }

void IPCServer::onNewConnection() {
    auto* client = m_server->nextPendingConnection();
    if (!client) return;
    m_clients.append(client);
    connect(client, &QLocalSocket::readyRead,     this, &IPCServer::onClientData);
    connect(client, &QLocalSocket::disconnected,  this, &IPCServer::onClientDisconnected);
}

void IPCServer::onClientData() {
    auto* client = qobject_cast<QLocalSocket*>(sender());
    if (!client) return;
    while (client->canReadLine()) {
        const QString cmd = QString::fromUtf8(client->readLine()).trimmed();
        if (!cmd.isEmpty()) handleCommand(client, cmd);
    }
}

void IPCServer::onClientDisconnected() {
    auto* client = qobject_cast<QLocalSocket*>(sender());
    m_clients.removeOne(client);
    client->deleteLater();
}

void IPCServer::handleCommand(QLocalSocket* client, const QString& cmd) {
    qInfo() << "[IPC] command:" << cmd;
    emit commandReceived(cmd);

    // Parse: "verb [args...]"
    const QStringList parts = cmd.split(' ', Qt::SkipEmptyParts);
    if (parts.isEmpty()) { sendResponse(client, false, "empty command"); return; }

    const QString verb = parts[0].toLower();
    const QString arg  = parts.size() > 1 ? parts.mid(1).join(' ') : QString();

    if (verb == "workspace") {
        bool ok; int n = arg.toInt(&ok);
        if (ok) { m_compositor->switchWorkspace(n); sendResponse(client, true); }
        else sendResponse(client, false, "invalid workspace number");
    } else if (verb == "exec") {
        if (arg.isEmpty()) { sendResponse(client, false, "exec needs a command"); return; }
        m_compositor->launchApp(arg);
        sendResponse(client, true);
    } else if (verb == "close") {
        if (auto* w = m_compositor->activeWindow())
            m_compositor->closeWindow(w);
        sendResponse(client, true);
    } else if (verb == "layout") {
        m_compositor->setLayout(TilingEngine::layoutFromString(arg));
        sendResponse(client, true);
    } else if (verb == "reload") {
        m_compositor->reloadConfig();
        sendResponse(client, true);
    } else if (verb == "quit") {
        sendResponse(client, true);
        QCoreApplication::quit();
    } else if (verb == "fullscreen") {
        m_compositor->toggleFullscreen();
        sendResponse(client, true);
    } else if (verb == "float") {
        m_compositor->toggleFloat();
        sendResponse(client, true);
    } else if (verb == "focus") {
        m_compositor->focusDirection(arg);
        sendResponse(client, true);
    } else if (verb == "status") {
        QJsonObject st;
        st["workspace"] = m_compositor->activeWorkspaceId();
        st["windows"]   = m_compositor->allWindows().size();
        const auto* w   = m_compositor->activeWindow();
        st["active"]    = w ? w->title() : "";
        sendResponse(client, true, QJsonDocument(st).toJson(QJsonDocument::Compact));
    } else {
        sendResponse(client, false, "unknown command: " + verb);
    }
}

void IPCServer::sendResponse(QLocalSocket* client, bool ok, const QString& msg) {
    QJsonObject obj;
    obj["success"] = ok;
    if (!msg.isEmpty()) obj[ok ? "result" : "error"] = msg;
    client->write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n");
    client->flush();
}
