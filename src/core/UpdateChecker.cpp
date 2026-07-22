#include "UpdateChecker.h"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QDebug>

// Simple version comparison: "1.0.3" > "1.0.2" → true
static bool isNewer(const QString& remote, const QString& local) {
    auto r = remote.split('.');
    auto l = local.split('.');
    int n = qMax(r.size(), l.size());
    for (int i = 0; i < n; ++i) {
        int rv = i < r.size() ? r[i].toInt() : 0;
        int lv = i < l.size() ? l[i].toInt() : 0;
        if (rv > lv) return true;
        if (rv < lv) return false;
    }
    return false;
}

UpdateChecker::UpdateChecker(QObject* parent) : QObject(parent) {
    m_nam = new QNetworkAccessManager(this);
}

void UpdateChecker::checkForUpdates() {
    QNetworkRequest req(QUrl("https://torreader.cloud/version.json"));
    req.setRawHeader("User-Agent", "TorReaderPDF/" FELIXPDF_VERSION);
    auto* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::sslErrors, reply,
            [reply](const QList<QSslError>&) { reply->ignoreSslErrors(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit checkFailed(reply->errorString());
            return;
        }
        auto doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) { emit checkFailed("invalid response"); return; }
        auto obj = doc.object();
        QString remote = obj["version"].toString().trimmed();
        if (remote.isEmpty()) { emit checkFailed("empty version"); return; }
        if (!isNewer(remote, FELIXPDF_VERSION)) { emit upToDate(); return; }
        QString title   = obj["title"].toString().trimmed();
        QString body    = obj["body"].toString().trimmed();
        bool blocking   = obj["blocking"].toBool(false);
        emit updateAvailable(remote, title, body, blocking);
    });
}
