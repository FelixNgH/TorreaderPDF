#pragma once
#include <QObject>
#include <QString>

class QNetworkAccessManager;

class UpdateChecker : public QObject {
    Q_OBJECT
public:
    explicit UpdateChecker(QObject* parent = nullptr);
    void checkForUpdates();

signals:
    void updateAvailable(const QString& newVersion, const QString& downloadUrl);
    void checkFailed(const QString& reason);

private:
    QNetworkAccessManager* m_nam = nullptr;
};
