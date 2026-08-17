// KHONG duoc build tu 2026-08-14: OCR da dong kem, bo co che tai roi. Giu lai phong khi lam goi ngon ngu.
#include "OcrDownloader.h"
#include "OcrPackage.h"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QTimer>

namespace {
constexpr int kMaxRetries = 3;
constexpr int kRetryDelays[3] = { 2000, 5000, 10000 };
}

OcrDownloader::OcrDownloader(QObject* parent) : QObject(parent) {
    m_nam = new QNetworkAccessManager(this);
}

// Tai JSON mo ta ban phat hanh. Loi mang -> finished(false, "Could not reach...").
void OcrDownloader::fetchReleaseInfo() {
    QString url = qEnvironmentVariable("TORREADER_OCR_RELEASE_URL");
    if (url.isEmpty()) url = QString::fromLatin1(OcrPackage::kOcrReleaseInfoUrl);
    QNetworkRequest req{ QUrl(url) };
    req.setRawHeader("User-Agent", "TorReaderPDF/" FELIXPDF_VERSION);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setMaximumRedirectsAllowed(5);
    auto* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (m_cancelled) return;
        if (reply->error() != QNetworkReply::NoError) {
            finish(false, QStringLiteral("Could not reach the update server. Please try again later."));
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonObject obj = doc.object();
        OcrRelease r;
        r.version = obj.value(QLatin1String("version")).toString();
        r.url     = obj.value(QLatin1String("url")).toString();
        r.sha256  = obj.value(QLatin1String("sha256")).toString().toLower();
        r.bytes   = static_cast<qint64>(obj.value(QLatin1String("bytes")).toDouble());
        if (r.url.isEmpty() || r.version.isEmpty() || r.sha256.isEmpty()) {
            finish(false, QStringLiteral("Could not reach the update server. Please try again later."));
            return;
        }
        emit releaseInfo(r);
    });
}

void OcrDownloader::start(const OcrRelease& r, const QString& destRoot) {
    m_release = r;
    m_partPath = destRoot + QLatin1String("/package.tar.gz.part");
    m_resumeOffset = QFileInfo(m_partPath).size();   // tai tiep tu so byte da co
    m_downloaded = 0;
    m_totalBytes = r.bytes;
    m_attempt = 0;
    m_cancelled = false;
    QDir().mkpath(destRoot);
    emit stage(QStringLiteral("Downloading"));
    beginDownload();
}

void OcrDownloader::beginDownload() {
    m_timer.restart();
    m_lastEmitMs = 0;
    QNetworkRequest req{ QUrl(m_release.url) };
    req.setRawHeader("User-Agent", "TorReaderPDF/" FELIXPDF_VERSION);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setMaximumRedirectsAllowed(5);
    if (m_resumeOffset > 0)
        req.setRawHeader("Range", QByteArray("bytes=") + QByteArray::number(m_resumeOffset) + "-");
    auto* reply = m_nam->get(req);
    m_reply = reply;    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply]() {
        if (reply == m_reply) onMetaChanged(reply);
    });
    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        if (reply == m_reply) drain(reply);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (reply == m_reply) onFinished(reply);
    });
}

void OcrDownloader::onMetaChanged(QNetworkReply* reply) {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (m_resumeOffset > 0 && status == 200) {
        // May chu khong ho tro Range (tra 200 thay vi 206): khong noi them vao
        // file cu (se ra file hong) -> xoa .part va tai lai tu dau.
        closePart();
        QFile::remove(m_partPath);
        m_resumeOffset = 0;
        m_downloaded = 0;
        reply->abort();
        beginDownload();
        return;
    }
    openPart();
}

void OcrDownloader::openPart() {
    closePart();
    m_part = new QFile(m_partPath);
    if (m_resumeOffset > 0)
        m_part->open(QIODevice::Append);
    else
        m_part->open(QIODevice::WriteOnly | QIODevice::Truncate);
    if (!m_part->isOpen()) {
        delete m_part;
        m_part = nullptr;
        finish(false, QStringLiteral("Cannot write the download file."));
    }
}

void OcrDownloader::closePart() {
    if (m_part) { m_part->close(); delete m_part; m_part = nullptr; }
}

void OcrDownloader::drain(QNetworkReply* reply) {
    if (!m_part || !m_part->isOpen()) return;
    const QByteArray chunk = reply->readAll();
    if (chunk.isEmpty()) return;
    if (m_part->write(chunk) != chunk.size()) {
        finish(false, QStringLiteral("Cannot write the download file."));
        return;
    }
    m_downloaded += chunk.size();
    emitProgress();
}

// Gioi han progress toi da 4 lan/giay, nhung luon phat lan cuoi.
void OcrDownloader::emitProgress() {
    const qint64 nowMs = m_timer.elapsed();
    const qint64 done = m_resumeOffset + m_downloaded;
    if (done < m_totalBytes && nowMs - m_lastEmitMs < 250) return;
    m_lastEmitMs = nowMs;
    const double rate = (m_downloaded > 0 && nowMs > 0) ? m_downloaded * 1000.0 / nowMs : 0.0;
    emit progress(done, m_totalBytes, rate);
}

void OcrDownloader::onFinished(QNetworkReply* reply) {
    if (m_cancelled) {
        // Huy: dung ngay, giu nguyen .part de lan sau tai tiep
        finish(false, QString());
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool http4xx = status >= 400 && status < 500;
        closePart();
        if (http4xx) {
            finish(false, QStringLiteral("Server returned HTTP %1").arg(status));
            return;
        }
        if (m_attempt < kMaxRetries) {
            retryBackoff();
            return;
        }
        finish(false, QStringLiteral("Network error: ") + reply->errorString());
        return;
    }
    drain(reply);
    closePart();
    if (m_totalBytes > 0 && m_resumeOffset + m_downloaded != m_totalBytes) {
        finish(false, QStringLiteral("Downloaded size mismatch (%1 of %2 bytes)")
                            .arg(m_resumeOffset + m_downloaded).arg(m_totalBytes));
        return;
    }
    verifyAndInstall();
}

void OcrDownloader::retryBackoff() {
    ++m_attempt;
    // Giu lai nhung gi da tai duoc: lan sau tai tiep tu day
    m_resumeOffset += m_downloaded;
    m_downloaded = 0;
    emit stage(QStringLiteral("Retrying (%1/3)").arg(m_attempt));
    const int ms = kRetryDelays[(m_attempt - 1) % kMaxRetries];
    QTimer::singleShot(ms, this, [this]() {
        if (!m_cancelled) beginDownload();
    });
}

void OcrDownloader::verifyAndInstall() {
    emit stage(QStringLiteral("Verifying"));
    // sha256 doc theo khoi 1MB, khong nap ca file vao RAM
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QFile f(m_partPath);
    if (!f.open(QIODevice::ReadOnly)) {
        finish(false, QStringLiteral("Cannot read the downloaded file."));
        return;
    }
    char buf[1024 * 1024];
    while (!f.atEnd()) {
        const qint64 n = f.read(buf, sizeof(buf));
        if (n <= 0) break;
        hash.addData(QByteArrayView(buf, static_cast<qsizetype>(n)));
    }
    f.close();
    const QString actual = QString::fromLatin1(hash.result().toHex());
    if (actual != m_release.sha256) {
        // Sai sha256: xoa .part, khong cai
        closePart();
        QFile::remove(m_partPath);
        finish(false, QStringLiteral("Checksum mismatch: expected %1, got %2")
                            .arg(m_release.sha256, actual));
        return;
    }
    emit stage(QStringLiteral("Extracting"));
    QString err;
    if (!OcrPackage::installFromArchive(m_partPath, &err)) {
        // .part da hop le nhung goi hong -> xoa di cho sach trang thai
        closePart();
        QFile::remove(m_partPath);
        finish(false, QStringLiteral("Install failed: ") + err);
        return;
    }
    closePart();
    QFile::remove(m_partPath);
    finish(true, QString());
}

void OcrDownloader::finish(bool ok, const QString& err) {
    closePart();
    if (m_reply) { m_reply->deleteLater(); m_reply = nullptr; }
    emit stage(ok ? QStringLiteral("Done") : QStringLiteral("Failed"));
    emit finished(ok, err);
}

void OcrDownloader::cancel() {
    m_cancelled = true;
    if (m_reply) m_reply->abort();
}
