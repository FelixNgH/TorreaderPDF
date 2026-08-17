// KHONG duoc build tu 2026-08-14: OCR da dong kem, bo co che tai roi. Giu lai phong khi lam goi ngon ngu.
#pragma once
#include <QObject>
#include <QString>
#include <QElapsedTimer>

struct OcrRelease {
    QString version;
    QString url;
    QString sha256;
    qint64  bytes = 0;
};

class QNetworkAccessManager;
class QNetworkReply;
class QFile;

// Tai goi OCR: fetchReleaseInfo() lay mo ta ban phat hanh tu OcrPackage::
// kOcrReleaseInfoUrl (co the tro sang file cuc bo bang bien moi truong
// TORREADER_OCR_RELEASE_URL), start() tai .tar.gz (tai tiep sau khi dut),
// kiem sha256 roi giai nen + cai qua OcrPackage::installFromArchive().
class OcrDownloader : public QObject {
    Q_OBJECT
public:
    explicit OcrDownloader(QObject* parent = nullptr);

    void fetchReleaseInfo();            // tai JSON mo ta ban phat hanh
    void start(const OcrRelease& r, const QString& destRoot);
    void cancel();                      // dung ngay, giu nguyen .part de tai tiep

signals:
    void releaseInfo(const OcrRelease&);
    void progress(qint64 done, qint64 total, double bytesPerSec);
    void finished(bool ok, const QString& error);   // ok=false + error rong = bi huy
    void stage(const QString& text);    // "Downloading" / "Verifying" / "Extracting"

private:
    void beginDownload();
    void onMetaChanged(QNetworkReply* reply);
    void drain(QNetworkReply* reply);
    void onFinished(QNetworkReply* reply);
    void openPart();
    void closePart();
    void emitProgress();
    void verifyAndInstall();
    void finish(bool ok, const QString& err);
    void retryBackoff();

    QNetworkAccessManager* m_nam = nullptr;
    QNetworkReply* m_reply = nullptr;
    QFile* m_part = nullptr;
    OcrRelease m_release;
    QString m_partPath;
    qint64 m_resumeOffset = 0;   // so byte da co trong .part = diem tai tiep
    qint64 m_downloaded = 0;     // byte nhan them trong lan chay nay
    qint64 m_totalBytes = 0;
    int m_attempt = 0;
    bool m_cancelled = false;
    qint64 m_lastEmitMs = 0;
    QElapsedTimer m_timer;
};
