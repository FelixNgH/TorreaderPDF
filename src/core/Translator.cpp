#include "Translator.h"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QStandardPaths>
#include <QDir>
#include <QCryptographicHash>
#include <QSaveFile>
#include <QFile>
#include <QDateTime>
#include <QMetaObject>
#include <QSettings>
#include <QTimer>
#include <algorithm>

static constexpr const char* kTargetLang = "vi";
static constexpr const char* kGeminiModel = "gemini-2.5-flash";
static constexpr const char* kCacheFileName = "translate_cache.json";
static constexpr int kMaxMemoryEntries = 500;
static constexpr int kMaxDiskEntries = 2000;
static constexpr int kCacheSaveDebounceMs = 2000;

static QString makeCacheKey(const QString& lang, const QString& text) {
    return QString::fromLatin1(
        QCryptographicHash::hash(
            (lang + QLatin1String("\x1f") + text).toUtf8(),
            QCryptographicHash::Sha1)
        .toHex());
}

Translator::Translator(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_cacheSaveTimer(new QTimer(this))
{
    m_cacheSaveTimer->setSingleShot(true);
    connect(m_cacheSaveTimer, &QTimer::timeout, this, &Translator::saveCacheToDisk);

    QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(appData);
    m_diskCachePath = appData + QLatin1Char('/') + QLatin1String(kCacheFileName);
}

void Translator::translate(const QString& text) {
    const QString lang = QString::fromLatin1(kTargetLang);
    const QString key = makeCacheKey(lang, text);

    auto emitCached = [this, text](const QString& translation) {
        QMetaObject::invokeMethod(this, [this, text, translation]() {
            emit finished(text, translation);
        }, Qt::QueuedConnection);
    };

    auto it = m_cache.constFind(key);
    if (it != m_cache.constEnd()) {
        emitCached(it->translation);
        return;
    }

    if (!m_cacheLoaded) {
        loadCacheFromDisk();
        it = m_cache.constFind(key);
        if (it != m_cache.constEnd()) {
            emitCached(it->translation);
            return;
        }
    }

    QUrl url(QStringLiteral("https://translate.googleapis.com/translate_a/single"
                            "?client=gtx&sl=auto&tl=%1&dt=t").arg(lang));

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  "application/x-www-form-urlencoded");
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  "Mozilla/5.0 (compatible; TorReaderPDF/2.0.0)");

    QByteArray body = "q=" + QUrl::toPercentEncoding(text);
    QNetworkReply* rep = m_nam->post(req, body);

    connect(rep, &QNetworkReply::finished, [this, rep, text, lang, key]() {
        rep->deleteLater();
        if (rep->error() != QNetworkReply::NoError) {
            tryGeminiFallback(text, lang, key, rep->errorString());
            return;
        }

        QByteArray raw = rep->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(raw);
        QString translation;

        if (!doc.isNull() && doc.isArray()) {
            QJsonArray outer = doc.array();
            if (!outer.isEmpty() && outer[0].isArray()) {
                for (const QJsonValue& seg : outer[0].toArray()) {
                    if (seg.isArray() && !seg.toArray().isEmpty())
                        translation += seg.toArray()[0].toString();
                }
            }
        }

        if (translation.isEmpty()) {
            QString bodyStr = QString::fromUtf8(raw);
            int pos = 0;
            while (true) {
                int start = bodyStr.indexOf("[[[\"", pos);
                if (start == -1) break;
                start += 4;
                int end = start;
                while (end < bodyStr.size()) {
                    if (bodyStr[end] == '"' && (end == 0 || bodyStr[end-1] != '\\')) break;
                    ++end;
                }
                if (end < bodyStr.size())
                    translation += bodyStr.mid(start, end - start);
                pos = end + 1;
                if (bodyStr.indexOf("],[", pos) < bodyStr.indexOf("[[[", pos) ||
                    bodyStr.indexOf("[[[", pos) == -1)
                    break;
            }
            translation.replace("\\n", "\n").replace("\\\"", "\"");
        }

        if (translation.trimmed().isEmpty()) {
            tryGeminiFallback(text, lang, key,
                              QStringLiteral("No translation returned"));
        } else {
            addToCache(key, translation.trimmed());
            emit finished(text, translation.trimmed());
        }
    });
}

void Translator::tryGeminiFallback(const QString& text, const QString& lang,
                                   const QString& key, const QString& primaryError)
{
    if (!m_geminiKeyChecked) {
        m_geminiApiKey = qEnvironmentVariable("GEMINI_API_KEY");
        if (m_geminiApiKey.isEmpty()) {
            QSettings settings;
            m_geminiApiKey = settings.value("translate/geminiApiKey").toString();
        }
        m_geminiKeyChecked = true;
    }

    if (m_geminiApiKey.isEmpty()) {
        emit failed(primaryError +
                    QStringLiteral(" (Gemini fallback: chưa cấu hình GEMINI_API_KEY)"));
        return;
    }

    QUrl url(QStringLiteral(
        "https://generativelanguage.googleapis.com/v1beta/models/%1:generateContent?key=%2")
        .arg(QLatin1String(kGeminiModel), m_geminiApiKey));

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  "Mozilla/5.0 (compatible; TorReaderPDF/2.0.0)");
    req.setTransferTimeout(15000);

    QJsonObject partObj;
    partObj[QStringLiteral("text")] =
        QStringLiteral("Translate the following text to %1. Return ONLY the translation, no explanations:\n\n%2")
            .arg(lang, text);

    QJsonArray parts;
    parts.append(partObj);

    QJsonObject contentObj;
    contentObj[QStringLiteral("parts")] = parts;

    QJsonArray contents;
    contents.append(contentObj);

    QJsonObject bodyObj;
    bodyObj[QStringLiteral("contents")] = contents;

    QByteArray body = QJsonDocument(bodyObj).toJson(QJsonDocument::Compact);
    QNetworkReply* rep = m_nam->post(req, body);

    connect(rep, &QNetworkReply::finished, [this, rep, text, key, primaryError]() {
        rep->deleteLater();
        if (rep->error() != QNetworkReply::NoError) {
            emit failed(primaryError +
                        QStringLiteral(" (Gemini fallback: ") + rep->errorString() +
                        QStringLiteral(")"));
            return;
        }

        QByteArray raw = rep->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(raw);
        QString translation;

        if (!doc.isNull() && doc.isObject()) {
            QJsonObject root = doc.object();
            QJsonArray candidates = root[QStringLiteral("candidates")].toArray();
            if (!candidates.isEmpty()) {
                QJsonObject candidate = candidates[0].toObject();
                QJsonObject content = candidate[QStringLiteral("content")].toObject();
                QJsonArray geminiParts = content[QStringLiteral("parts")].toArray();
                if (!geminiParts.isEmpty()) {
                    translation = geminiParts[0].toObject()[QStringLiteral("text")].toString().trimmed();
                }
            }
        }

        if (translation.isEmpty()) {
            emit failed(primaryError +
                        QStringLiteral(" (Gemini fallback: empty response)"));
        } else {
            addToCache(key, translation);
            emit finished(text, translation);
        }
    });
}

void Translator::addToCache(const QString& key, const QString& translation) {
    if (m_cache.size() >= kMaxMemoryEntries) {
        QList<QPair<qint64, QString>> entries;
        for (auto it = m_cache.begin(); it != m_cache.end(); ++it)
            entries.append({it.value().timestamp, it.key()});
        std::sort(entries.begin(), entries.end());
        int toRemove = entries.size() / 2;
        for (int i = 0; i < toRemove; ++i)
            m_cache.remove(entries[i].second);
    }

    CacheEntry entry;
    entry.translation = translation;
    entry.timestamp = QDateTime::currentSecsSinceEpoch();
    m_cache.insert(key, entry);

    m_cacheSaveTimer->start(kCacheSaveDebounceMs);
}

void Translator::loadCacheFromDisk() {
    m_cacheLoaded = true;

    QFile file(m_diskCachePath);
    if (!file.open(QIODevice::ReadOnly))
        return;

    QByteArray raw = file.readAll();
    file.close();

    QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (doc.isNull() || !doc.isObject())
        return;

    QJsonObject root = doc.object();
    QJsonObject entries = root[QStringLiteral("entries")].toObject();

    for (auto it = entries.begin(); it != entries.end(); ++it) {
        QJsonObject entryObj = it.value().toObject();
        CacheEntry entry;
        entry.translation = entryObj[QStringLiteral("text")].toString();
        entry.timestamp = static_cast<qint64>(entryObj[QStringLiteral("ts")].toDouble());
        m_cache.insert(it.key(), entry);
    }

    if (m_cache.size() > kMaxDiskEntries) {
        QList<QPair<qint64, QString>> sorted;
        for (auto it = m_cache.begin(); it != m_cache.end(); ++it)
            sorted.append({it.value().timestamp, it.key()});
        std::sort(sorted.begin(), sorted.end());
        int toRemove = sorted.size() - kMaxDiskEntries;
        for (int i = 0; i < toRemove; ++i)
            m_cache.remove(sorted[i].second);
    }
}

void Translator::saveCacheToDisk() {
    if (m_cache.size() > kMaxDiskEntries) {
        QList<QPair<qint64, QString>> sorted;
        for (auto it = m_cache.begin(); it != m_cache.end(); ++it)
            sorted.append({it.value().timestamp, it.key()});
        std::sort(sorted.begin(), sorted.end());
        int toRemove = sorted.size() - kMaxDiskEntries;
        for (int i = 0; i < toRemove; ++i)
            m_cache.remove(sorted[i].second);
    }

    QJsonObject entries;
    for (auto it = m_cache.begin(); it != m_cache.end(); ++it) {
        QJsonObject entryObj;
        entryObj[QStringLiteral("text")] = it.value().translation;
        entryObj[QStringLiteral("ts")] = it.value().timestamp;
        entries[it.key()] = entryObj;
    }

    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("entries")] = entries;

    QSaveFile file(m_diskCachePath);
    if (!file.open(QIODevice::WriteOnly))
        return;

    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.commit();
}
