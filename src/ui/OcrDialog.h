// KHONG duoc build tu 2026-08-14: OCR da dong kem, bo co che tai roi. Giu lai phong khi lam goi ngon ngu.
#pragma once
#include <QDialog>
#include <QAtomicInt>
#include <QFuture>
#include <QFutureWatcher>
#include "core/OcrDownloader.h"
#include "core/PdfDocument.h"

class QLabel;
class QPushButton;
class QProgressBar;

// Hộp thoại OCR: vừa quản lý gói OCR (tải/cài) vừa chạy nhận dạng trên tài liệu
// đang mở. Phần nhận dạng chạy ở luồng nền, có thanh tiến độ theo trang và
// nút Cancel. Kết quả giữ trong bộ nhớ — không ghi file.
class OcrDialog : public QDialog {
    Q_OBJECT
public:
    // doc: tài liệu PDF đang mở (có thể null nếu chưa mở file).
    // docCurrentPage: trang hiện tại (0-based) để nút "current page" biết đích.
    explicit OcrDialog(PdfDocument* doc, int docCurrentPage,
                       QWidget* parent = nullptr);
    void refresh();   // goi lai detect() de cap nhat trang thai sau khi dong/install

private slots:
    void onInstallFromFile();
    void onRemovePackage();
    void onChooseFolder();
    void onOpenFolder();
    void onDownload();
    void onReleaseInfo(const OcrRelease& r);
    void startDownload();
    void onProgress(qint64 done, qint64 total, double bytesPerSec);
    void onDownloadFinished(bool ok, const QString& err);

    void onRecognizePage();
    void onRecognizeWhole();
    void onOcrProgress(int done, int total);
    void onOcrFinished();

signals:
    void ocrProgress(int done, int total);

private:
    void startRecognize(int firstPage, int lastPage);
    void setRecognizing(bool busy);
    struct OcrResult { int pages = 0; int words = 0; qint64 elapsedMs = 0; };

    QLabel* m_statusLabel = nullptr;
    QLabel* m_problemLabel = nullptr;
    QLabel* m_cloudLabel = nullptr;
    QLabel* m_pathLabel = nullptr;
    QLabel* m_dlStatusLabel = nullptr;
    QLabel* m_ocrStatusLabel = nullptr;
    QPushButton* m_removeBtn = nullptr;
    QPushButton* m_cloudBtn = nullptr;
    QPushButton* m_installBtn = nullptr;
    QPushButton* m_downloadBtn = nullptr;
    QPushButton* m_cancelBtn = nullptr;
    QPushButton* m_ocrPageBtn = nullptr;
    QPushButton* m_ocrWholeBtn = nullptr;
    QPushButton* m_ocrCancelBtn = nullptr;
    QProgressBar* m_progressBar = nullptr;
    QProgressBar* m_ocrProgress = nullptr;
    OcrDownloader* m_downloader = nullptr;
    OcrRelease m_pending;

    PdfDocument* m_doc = nullptr;
    int m_docCurrentPage = 0;

    QFutureWatcher<OcrResult> m_ocrWatcher;
    QAtomicInt m_ocrCancelled{0};
};
