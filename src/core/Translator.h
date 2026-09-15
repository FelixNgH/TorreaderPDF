#pragma once
#include <QObject>
#include <QString>
#include <QHash>
#include <QList>
#include <QPair>

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

    // srcLang may be "auto"; dstLang must be a concrete language code.
    void translate(const QString& text,
                   const QString& srcLang = QStringLiteral("en"),
                   const QString& dstLang = QStringLiteral("vi"));

    // Shared language table: <code, display name>. "auto" is the first entry.
    static const QList<QPair<QString, QString>>& languages();
    static QString languageName(const QString& code);

signals:
    void finished(const QString& original, const QString& translation,
                  const QString& srcLang, const QString& dstLang);
    void failed(const QString& error);

private:
    void sendGoogleRequest(const QString& text, const QString& srcLang,
                           const QString& dstLang, const QString& key,
                           bool useGet);
    void addToCache(const QString& key, const QString& translation);
    void loadCacheFromDisk();
    void saveCacheToDisk();
    void tryGeminiFallback(const QString& text, const QString& srcLang,
                           const QString& dstLang, const QString& key,
                           const QString& primaryError);

    QNetworkAccessManager* m_nam;
    QHash<QString, CacheEntry> m_cache;
    QTimer* m_cacheSaveTimer;
    QString m_diskCachePath;
    QString m_geminiApiKey;
    bool m_cacheLoaded = false;
    bool m_geminiKeyChecked = false;
};
