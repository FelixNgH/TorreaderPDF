#pragma once
#include <QObject>
#include <QString>

class QNetworkAccessManager;

class UpdateChecker : public QObject {
    Q_OBJECT
public:
    explicit UpdateChecker(QObject* parent = nullptr);
    void checkForUpdates();

    static constexpr const char* kDownloadPageUrl = "https://torreader.cloud";

signals:
    void updateAvailable(const QString& newVersion, const QString& title,
                         const QString& body, bool blocking);
    void checkFailed(const QString& reason);
    void upToDate();

private:
    QNetworkAccessManager* m_nam = nullptr;
};
