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
#include <QSslSocket>
#include <algorithm>

static constexpr const char* kGeminiModel = "gemini-2.5-flash";
static constexpr const char* kCacheFileName = "translate_cache.json";
static constexpr int kMaxMemoryEntries = 500;
static constexpr int kMaxDiskEntries = 2000;
static constexpr int kCacheSaveDebounceMs = 2000;
static constexpr int kRequestTimeoutMs = 15000;

static QString makeCacheKey(const QString& srcLang, const QString& dstLang,
                            const QString& text) {
    return QString::fromLatin1(
        QCryptographicHash::hash(
            (srcLang + QLatin1String("\x1f") + dstLang +
             QLatin1String("\x1f") + text).toUtf8(),
            QCryptographicHash::Sha1)
        .toHex());
}

const QList<QPair<QString, QString>>& Translator::languages() {
    static const QList<QPair<QString, QString>> kLangs = {
        { QStringLiteral("auto"), QStringLiteral("Detect language") },
        { QStringLiteral("en"),   QStringLiteral("English") },
        { QStringLiteral("vi"),   QStringLiteral("Vietnamese") },
        { QStringLiteral("zh-CN"),QStringLiteral("Chinese (Simplified)") },
        { QStringLiteral("zh-TW"),QStringLiteral("Chinese (Traditional)") },
        { QStringLiteral("ja"),   QStringLiteral("Japanese") },
        { QStringLiteral("ko"),   QStringLiteral("Korean") },
        { QStringLiteral("fr"),   QStringLiteral("French") },
        { QStringLiteral("de"),   QStringLiteral("German") },
        { QStringLiteral("es"),   QStringLiteral("Spanish") },
        { QStringLiteral("pt"),   QStringLiteral("Portuguese") },
        { QStringLiteral("it"),   QStringLiteral("Italian") },
        { QStringLiteral("ru"),   QStringLiteral("Russian") },
        { QStringLiteral("th"),   QStringLiteral("Thai") },
        { QStringLiteral("id"),   QStringLiteral("Indonesian") },
        { QStringLiteral("ms"),   QStringLiteral("Malay") },
        { QStringLiteral("hi"),   QStringLiteral("Hindi") },
        { QStringLiteral("ar"),   QStringLiteral("Arabic") },
        { QStringLiteral("nl"),   QStringLiteral("Dutch") },
        { QStringLiteral("pl"),   QStringLiteral("Polish") },
        { QStringLiteral("tr"),   QStringLiteral("Turkish") },
    };
    return kLangs;
}

QString Translator::languageName(const QString& code) {
    for (const auto& p : languages())
        if (p.first.compare(code, Qt::CaseInsensitive) == 0)
            return p.second;
    return code;
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

void Translator::translate(const QString& text, const QString& srcLangIn,
                           const QString& dstLangIn) {
    const QString srcLang = srcLangIn.isEmpty() ? QStringLiteral("auto") : srcLangIn;
    const QString dstLang = dstLangIn.isEmpty() ? QStringLiteral("vi")   : dstLangIn;
    const QString key = makeCacheKey(srcLang, dstLang, text);

    auto emitCached = [this, text, srcLang, dstLang](const QString& translation) {
        QMetaObject::invokeMethod(this, [this, text, translation, srcLang, dstLang]() {
            emit finished(text, translation, srcLang, dstLang);
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

    // A Qt build shipped without its TLS backend plugin cannot open *any* https
    // connection, and the resulting reply error is opaque. Say so plainly.
    if (!QSslSocket::supportsSsl()) {
        emit failed(QStringLiteral(
            "No TLS backend available, so https requests cannot be made. "
            "The Qt 'tls' plugin folder is missing next to the executable "
            "(re-run windeployqt on the build)."));
        return;
    }

    sendGoogleRequest(text, srcLang, dstLang, key, /*useGet=*/false);
}

// Google is asked twice before falling back to Gemini: first as the POST the
// endpoint documents, then as a plain GET. Some proxies and captive networks
// drop the POST body and hand back an empty 200, which used to look exactly
// like "no response at all" from the UI.
void Translator::sendGoogleRequest(const QString& text, const QString& srcLang,
                                   const QString& dstLang, const QString& key,
                                   bool useGet)
{
    QString urlStr = QStringLiteral("https://translate.googleapis.com/translate_a/single"
                                    "?client=gtx&sl=%1&tl=%2&dt=t")
                         .arg(QString::fromLatin1(QUrl::toPercentEncoding(srcLang)),
                              QString::fromLatin1(QUrl::toPercentEncoding(dstLang)));
    if (useGet)
        urlStr += QStringLiteral("&q=") +
                  QString::fromLatin1(QUrl::toPercentEncoding(text));

    QNetworkRequest req((QUrl(urlStr)));
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  "application/x-www-form-urlencoded");
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  "Mozilla/5.0 (compatible; TorReaderPDF/2.0.0)");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    // Without a timeout a stalled connection never emits finished(), so the UI
    // would sit on "Translating…" forever with no error.
    req.setTransferTimeout(kRequestTimeoutMs);

    QNetworkReply* rep = useGet
        ? m_nam->get(req)
        : m_nam->post(req, QByteArray("q=") + QUrl::toPercentEncoding(text));

    connect(rep, &QNetworkReply::finished, this,
            [this, rep, text, srcLang, dstLang, key, useGet]() {
        rep->deleteLater();

        const int http = rep->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = rep->readAll();

        qWarning("Translator: %s %s -> http=%d qterr=%d(%s) bytes=%lld",
                 useGet ? "GET" : "POST",
                 qPrintable(QStringLiteral("%1->%2").arg(srcLang, dstLang)),
                 http, int(rep->error()), qPrintable(rep->errorString()),
                 qint64(raw.size()));

        if (rep->error() != QNetworkReply::NoError) {
            const QString why = QStringLiteral("%1 (HTTP %2)")
                                    .arg(rep->errorString()).arg(http);
            if (!useGet)
                sendGoogleRequest(text, srcLang, dstLang, key, /*useGet=*/true);
            else
                tryGeminiFallback(text, srcLang, dstLang, key, why);
            return;
        }

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
            // An empty 200 usually means the body never reached Google; retry
            // as a GET before giving up on the endpoint entirely.
            if (!useGet) {
                sendGoogleRequest(text, srcLang, dstLang, key, /*useGet=*/true);
                return;
            }
            tryGeminiFallback(
                text, srcLang, dstLang, key,
                QStringLiteral("Google returned HTTP %1 with %2 bytes and no "
                               "translation").arg(http).arg(raw.size()));
        } else {
            addToCache(key, translation.trimmed());
            emit finished(text, translation.trimmed(), srcLang, dstLang);
        }
    });
}

void Translator::tryGeminiFallback(const QString& text, const QString& srcLang,
                                   const QString& dstLang, const QString& key,
                                   const QString& primaryError)
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
                    QStringLiteral(" (Gemini fallback unavailable: GEMINI_API_KEY is not configured)"));
        return;
    }

    QUrl url(QStringLiteral(
        "https://generativelanguage.googleapis.com/v1beta/models/%1:generateContent?key=%2")
        .arg(QLatin1String(kGeminiModel), m_geminiApiKey));

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  "Mozilla/5.0 (compatible; TorReaderPDF/2.0.0)");
    req.setTransferTimeout(kRequestTimeoutMs);

    const QString fromPart = (srcLang == QLatin1String("auto"))
        ? QString()
        : QStringLiteral("from %1 ").arg(languageName(srcLang));

    QJsonObject partObj;
    partObj[QStringLiteral("text")] =
        QStringLiteral("Translate the following text %1to %2. "
                       "Return ONLY the translation, no explanations:\n\n%3")
            .arg(fromPart, languageName(dstLang), text);

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

    connect(rep, &QNetworkReply::finished, this,
            [this, rep, text, srcLang, dstLang, key, primaryError]() {
        rep->deleteLater();
        const int http = rep->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QByteArray raw = rep->readAll();
        qWarning("Translator: Gemini -> http=%d qterr=%d(%s) bytes=%lld",
                 http, int(rep->error()), qPrintable(rep->errorString()),
                 qint64(raw.size()));

        if (rep->error() != QNetworkReply::NoError) {
            emit failed(primaryError +
                        QStringLiteral(" (Gemini fallback: ") + rep->errorString() +
                        QStringLiteral(")"));
            return;
        }

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
            emit finished(text, translation, srcLang, dstLang);
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
