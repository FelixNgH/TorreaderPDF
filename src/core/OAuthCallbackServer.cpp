#include "OAuthCallbackServer.h"
#include <QTcpSocket>
#include <QUrlQuery>

OAuthCallbackServer::OAuthCallbackServer(QObject* parent)
    : QObject(parent), m_server(new QTcpServer(this))
{
    connect(m_server, &QTcpServer::newConnection,
            this, &OAuthCallbackServer::onNewConnection);
    if (m_server->listen(QHostAddress::LocalHost, 0))
        m_port = m_server->serverPort();
}

OAuthCallbackServer::~OAuthCallbackServer() {}

quint16 OAuthCallbackServer::port() const { return m_port; }

void OAuthCallbackServer::onNewConnection() {
    if (m_done) return;
    while (m_server->hasPendingConnections()) {
        QTcpSocket* client = m_server->nextPendingConnection();
        if (client)
            connect(client, &QTcpSocket::readyRead, this, [this, client]() {
                handleClient(client);
            });
    }
}

void OAuthCallbackServer::handleClient(QTcpSocket* client) {
    if (m_done) { client->deleteLater(); return; }

    QByteArray data = client->readAll();
    QString firstLine = QString::fromUtf8(data).section('\n', 0, 0).trimmed();

    QString code, state;
    if (firstLine.startsWith("GET ")) {
        int qpos    = firstLine.indexOf('?');
        int httpPos = firstLine.indexOf(" HTTP/", qpos);
        if (qpos != -1 && httpPos != -1) {
            QUrlQuery q(firstLine.mid(qpos + 1, httpPos - qpos - 1));
            code  = q.queryItemValue("code");
            state = q.queryItemValue("state");
        }
    }

    static const char* html =
        "<html><body style='font-family:sans-serif;text-align:center;padding:40px'>"
        "<h2>&#x2714; Authentication successful</h2>"
        "<p>You can close this tab and return to TorReader.</p>"
        "</body></html>";
    QByteArray htmlBytes(html);
    QByteArray resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: "
                    + QByteArray::number(htmlBytes.size())
                    + "\r\nConnection: close\r\n\r\n" + htmlBytes;
    client->write(resp);
    client->flush();
    client->disconnectFromHost();

    m_done = true;
    m_server->close();

    if (!code.isEmpty())
        emit codeReceived(code, state);
}
