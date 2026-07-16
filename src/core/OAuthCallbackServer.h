#pragma once
#include <QObject>
#include <QTcpServer>

class QTcpSocket;

class OAuthCallbackServer : public QObject {
    Q_OBJECT
public:
    explicit OAuthCallbackServer(QObject* parent = nullptr);
    ~OAuthCallbackServer();
    quint16 port() const;

signals:
    void codeReceived(const QString& code, const QString& state);

private slots:
    void onNewConnection();

private:
    void handleClient(QTcpSocket* client);

    QTcpServer* m_server;
    quint16     m_port = 0;
    bool        m_done = false;
};
