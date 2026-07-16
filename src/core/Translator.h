#pragma once
#include <QObject>
#include <QString>
#include <QHash>

class QNetworkAccessManager;
class QTimer;

struct CacheEntry {
    QString translation;
    qint64 timestamp;
};

class Translator : public QObject {
    Q_OBJECT
public:
    explicit Translator(QObject* parent = nullptr);
    void translate(const QString& text);

signals:
    void finished(const QString& original, const QString& translation);
    void failed(const QString& error);

private:
    void addToCache(const QString& key, const QString& translation);
    void loadCacheFromDisk();
    void saveCacheToDisk();
    void tryGeminiFallback(const QString& text, const QString& lang,
                           const QString& key, const QString& primaryError);

    QNetworkAccessManager* m_nam;
    QHash<QString, CacheEntry> m_cache;
    QTimer* m_cacheSaveTimer;
    QString m_diskCachePath;
    QString m_geminiApiKey;
    bool m_cacheLoaded = false;
    bool m_geminiKeyChecked = false;
};
