// KHONG duoc build tu 2026-08-14: OCR da dong kem, bo co che tai roi. Giu lai phong khi lam goi ngon ngu.
#include "OcrDialog.h"
#include "core/OcrPackage.h"
#include "core/OcrEngine.h"
#include "core/OcrTextLayer.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QFileDialog>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QSettings>
#include <QDir>
#include <QtConcurrent>
#include <QThread>
#include <QElapsedTimer>

// Rut gon duong dan: giu goc (o dia hoac /), dau 3 cham, roi 2 thanh phan cuoi.
// Khong dung QFontMetrics::elidedText vi no cat theo pixel nen khong dam bao
// thanh phan o giua bi an. Duong dan <=3 thanh phan thi de nguyen.
static QString shortenPath(const QString& path) {
    const QString native = QDir::toNativeSeparators(path);
    const QString sep = QString(QDir::separator());
    const QStringList parts = native.split(sep);
    QStringList keep = parts;
    while (!keep.isEmpty() && keep.first().isEmpty()) keep.removeFirst();  // bo dau "/" tren Linux
    if (keep.size() <= 3) return native;
    QString root = sep;                       // Linux: root la "/"
    if (!parts.first().isEmpty()) root = parts.first() + sep;  // Windows: root la "D:\"
    return root + QStringLiteral("\u2026") + sep
           + keep[keep.size() - 2] + sep + keep[keep.size() - 1];
}

static QString humanSize(qint64 bytes) {
    if (bytes <= 0) return QStringLiteral("0 B");
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024);
    if (bytes < 1024LL * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(QString::number(bytes / 1024.0 / 1024.0, 'f', 1));
    return QStringLiteral("%1 GB").arg(QString::number(bytes / 1024.0 / 1024.0 / 1024.0, 'f', 2));
}

OcrDialog::OcrDialog(PdfDocument* doc, int docCurrentPage, QWidget* parent)
    : QDialog(parent), m_doc(doc), m_docCurrentPage(docCurrentPage) {
    setWindowTitle(QStringLiteral("OCR \u2014 Text recognition"));
    setMinimumWidth(560);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(18, 16, 18, 14);
    lay->setSpacing(10);

    m_statusLabel = new QLabel;
    m_statusLabel->setWordWrap(true);
    lay->addWidget(m_statusLabel);

    m_problemLabel = new QLabel;
    m_problemLabel->setWordWrap(true);
    m_problemLabel->setVisible(false);
    lay->addWidget(m_problemLabel);

    // Dung luong du kien + duong dan that — hien trong moi truong hop
    m_pathLabel = new QLabel;
    m_pathLabel->setWordWrap(true);
    m_pathLabel->setStyleSheet(QStringLiteral("color:#6B7280; font-size:11px;"));
    lay->addWidget(m_pathLabel);

    m_cloudLabel = new QLabel;
    m_cloudLabel->setWordWrap(true);
    m_cloudLabel->setVisible(false);
    lay->addWidget(m_cloudLabel);

    // ── Khoi nhan dang: ghi chu + 2 nut hanh dong + tien do trang ──────────
    auto* ocrNote = new QLabel(
        "Recognized text is kept in memory only \u2014 your file is not modified.");
    ocrNote->setWordWrap(true);
    ocrNote->setStyleSheet(QStringLiteral("color:#6B7280; font-size:11px;"));
    lay->addWidget(ocrNote);

    m_ocrStatusLabel = new QLabel;
    m_ocrStatusLabel->setWordWrap(true);
    lay->addWidget(m_ocrStatusLabel);

    auto* ocrRow = new QHBoxLayout;
    m_ocrPageBtn = new QPushButton(QStringLiteral("Recognize current page"));
    m_ocrWholeBtn = new QPushButton(QStringLiteral("Recognize whole document"));
    ocrRow->addWidget(m_ocrPageBtn);
    ocrRow->addWidget(m_ocrWholeBtn);
    ocrRow->addStretch();
    lay->addLayout(ocrRow);

    auto* ocrProgRow = new QHBoxLayout;
    m_ocrProgress = new QProgressBar;
    m_ocrProgress->setVisible(false);
    ocrProgRow->addWidget(m_ocrProgress, 1);
    m_ocrCancelBtn = new QPushButton(QStringLiteral("Cancel"));
    m_ocrCancelBtn->setVisible(false);
    ocrProgRow->addWidget(m_ocrCancelBtn);
    lay->addLayout(ocrProgRow);

    lay->addSpacing(4);

    // Hanh dong phu: Remove and reinstall (goi hong) / Choose another folder (cloud)
    auto* secondaryRow = new QHBoxLayout;
    m_removeBtn = new QPushButton(QStringLiteral("Remove and reinstall"));
    m_removeBtn->setVisible(false);
    secondaryRow->addWidget(m_removeBtn);
    m_cloudBtn = new QPushButton(QStringLiteral("Choose another folder\u2026"));
    m_cloudBtn->setVisible(false);
    secondaryRow->addWidget(m_cloudBtn);
    secondaryRow->addStretch();
    lay->addLayout(secondaryRow);

    lay->addSpacing(4);

    // Khoi tai xuong: thanh tien do + dong trang thai + nut Cancel (an khi khong tai)
    m_dlStatusLabel = new QLabel;
    m_dlStatusLabel->setWordWrap(true);
    m_dlStatusLabel->setVisible(false);
    lay->addWidget(m_dlStatusLabel);

    auto* dlRow = new QHBoxLayout;
    m_progressBar = new QProgressBar;
    m_progressBar->setVisible(false);
    dlRow->addWidget(m_progressBar, 1);
    m_cancelBtn = new QPushButton(QStringLiteral("Cancel"));
    m_cancelBtn->setVisible(false);
    dlRow->addWidget(m_cancelBtn);
    lay->addLayout(dlRow);

    auto* actionRow = new QHBoxLayout;
    m_downloadBtn = new QPushButton(QStringLiteral("Download OCR package\u2026"));
    actionRow->addWidget(m_downloadBtn);
    m_installBtn = new QPushButton(QStringLiteral("Install from file\u2026"));
    actionRow->addWidget(m_installBtn);
    auto* openFolderBtn = new QPushButton(QStringLiteral("Open folder"));
    actionRow->addWidget(openFolderBtn);
    actionRow->addStretch();
    auto* closeBtn = new QPushButton(QStringLiteral("Close"));
    actionRow->addWidget(closeBtn);
    lay->addLayout(actionRow);

    m_downloader = new OcrDownloader(this);
    connect(m_downloadBtn, &QPushButton::clicked, this, &OcrDialog::onDownload);
    connect(m_cancelBtn, &QPushButton::clicked, m_downloader, &OcrDownloader::cancel);
    connect(m_downloader, &OcrDownloader::releaseInfo, this, &OcrDialog::onReleaseInfo);
    connect(m_downloader, &OcrDownloader::progress, this, &OcrDialog::onProgress);
    connect(m_downloader, &OcrDownloader::stage, m_dlStatusLabel, &QLabel::setText);
    connect(m_downloader, &OcrDownloader::finished, this, &OcrDialog::onDownloadFinished);
    connect(m_installBtn, &QPushButton::clicked, this, &OcrDialog::onInstallFromFile);
    connect(m_removeBtn, &QPushButton::clicked, this, &OcrDialog::onRemovePackage);
    connect(m_cloudBtn, &QPushButton::clicked, this, &OcrDialog::onChooseFolder);
    connect(openFolderBtn, &QPushButton::clicked, this, &OcrDialog::onOpenFolder);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    connect(m_ocrPageBtn, &QPushButton::clicked, this, &OcrDialog::onRecognizePage);
    connect(m_ocrWholeBtn, &QPushButton::clicked, this, &OcrDialog::onRecognizeWhole);
    connect(m_ocrCancelBtn, &QPushButton::clicked, this, [this]() {
        m_ocrCancelled.storeRelaxed(1);
        m_ocrCancelBtn->setEnabled(false);
    });
    connect(&m_ocrWatcher, &QFutureWatcher<void>::finished, this, &OcrDialog::onOcrFinished);
    connect(this, &OcrDialog::ocrProgress, this, &OcrDialog::onOcrProgress);

    refresh();
}

void OcrDialog::refresh() {
    const OcrPackage::Info info = OcrPackage::detect();

    if (!info.installed) {
        if (info.problem.isEmpty()) {
            m_statusLabel->setText(
                "OCR package is not installed (about 200 MB). "
                "It runs entirely on this computer \u2014 no file ever leaves your machine.");
        } else {
            m_statusLabel->setText("The OCR package is damaged or incomplete.");
            m_problemLabel->setText(info.problem);
            m_problemLabel->setVisible(true);
            m_removeBtn->setVisible(true);
        }
    } else {
        m_statusLabel->setText(QStringLiteral("OCR package installed: <b>%1</b> version <b>%2</b> "
                                              "(%3 on disk)")
                                   .arg(info.name).arg(info.version)
                                   .arg(humanSize(info.sizeOnDisk)));
    }

    m_pathLabel->setText(QStringLiteral("Folder: %1\nExpected size: about 200 MB")
                             .arg(shortenPath(info.root)));
    m_pathLabel->setToolTip(info.root);

    const bool cloud = info.inCloudFolder;
    m_cloudLabel->setVisible(cloud);
    m_cloudBtn->setVisible(cloud);
    if (cloud) {
        m_cloudLabel->setText(
            "This folder appears to be synced to cloud storage. The package would be "
            "uploaded as well, which is slow and uses up your storage quota.");
    }

    m_installBtn->setEnabled(info.rootWritable);
    m_downloadBtn->setEnabled(info.rootWritable);
    if (!info.rootWritable) {
        const QString tip = "The package folder is not writable \u2014 use \u201cChoose another folder\u2026\u201d if available.";
        m_installBtn->setToolTip(tip);
        m_downloadBtn->setToolTip(tip);
    } else {
        m_installBtn->setToolTip(QString());
        m_downloadBtn->setToolTip(QString());
    }

    // Nut nhan dang: can doc mo + OCR san sang
    QString whyNot;
    const bool ocrOk = m_doc && m_doc->isOpen() && OcrEngine::available(&whyNot);
    m_ocrPageBtn->setEnabled(ocrOk);
    m_ocrWholeBtn->setEnabled(ocrOk);
    if (m_ocrPageBtn->isEnabled())
        m_ocrStatusLabel->clear();
    else if (m_ocrStatusLabel->text().isEmpty() || !m_ocrStatusLabel->text().startsWith("Cannot"))
        m_ocrStatusLabel->setText(QStringLiteral("Cannot recognize yet. %1")
                                      .arg(m_doc && m_doc->isOpen()
                                               ? whyNot
                                               : QStringLiteral("Open a PDF file first.")));
    if (!ocrOk) {
        QString tip = "Cannot recognize yet.";
        if (m_doc && !m_doc->isOpen()) tip += " Open a PDF file first.";
        else if (!whyNot.isEmpty()) tip += " " + whyNot;
        m_ocrPageBtn->setToolTip(tip);
        m_ocrWholeBtn->setToolTip(tip);
    } else {
        m_ocrPageBtn->setToolTip(QString());
        m_ocrWholeBtn->setToolTip(QString());
    }
}

void OcrDialog::onDownload() {
    m_downloadBtn->setEnabled(false);
    m_installBtn->setEnabled(false);
    m_progressBar->setVisible(true);
    m_progressBar->setRange(0, 0);   // dang xac dinh truoc khi biet dung luong
    m_progressBar->setValue(0);
    m_cancelBtn->setVisible(true);
    m_dlStatusLabel->setVisible(true);
    m_dlStatusLabel->setText("Contacting the update server\u2026");
    m_downloader->fetchReleaseInfo();
}

void OcrDialog::onReleaseInfo(const OcrRelease& r) {
    m_pending = r;
    // Che do chay thu (TORREADER_OCR_RELEASE_URL): bo qua hop xac nhan
    if (!qEnvironmentVariableIsEmpty("TORREADER_OCR_RELEASE_URL")) {
        startDownload();
        return;
    }
    const QMessageBox::StandardButton b = QMessageBox::question(
        this, "Download OCR package",
        QStringLiteral("Found version %1 (%2).\n\nDownload and install now?")
            .arg(r.version, humanSize(r.bytes)),
        QMessageBox::Yes | QMessageBox::No);
    if (b == QMessageBox::Yes)
        startDownload();
    else
        onDownloadFinished(false, QString());
}

void OcrDialog::startDownload() {
    if (m_pending.bytes > 0)
        m_progressBar->setRange(0, static_cast<int>(m_pending.bytes));
    else
        m_progressBar->setRange(0, 0);
    m_progressBar->setValue(0);
    m_dlStatusLabel->setText(QStringLiteral("Downloading version %1 (%2)\u2026")
                                 .arg(m_pending.version, humanSize(m_pending.bytes)));
    m_downloader->start(m_pending, OcrPackage::preferredRoot(nullptr));
}

void OcrDialog::onProgress(qint64 done, qint64 total, double bytesPerSec) {
    if (total > 0) {
        m_progressBar->setRange(0, static_cast<int>(total));
        m_progressBar->setValue(static_cast<int>(qMin<qint64>(done, total)));
    }
    m_dlStatusLabel->setText(QStringLiteral("Downloading\u2026 %1 / %2 \u2014 %3 MB/s")
                                 .arg(humanSize(done), humanSize(total),
                                      QString::number(bytesPerSec / 1024.0 / 1024.0, 'f', 1)));
}

void OcrDialog::onDownloadFinished(bool ok, const QString& err) {
    m_progressBar->setVisible(false);
    m_cancelBtn->setVisible(false);
    m_dlStatusLabel->setVisible(false);
    if (!ok && !err.isEmpty())
        QMessageBox::warning(this, "OCR package", err);
    refresh();
}

void OcrDialog::onInstallFromFile() {
    const QString zip = QFileDialog::getOpenFileName(this, "Select OCR package (.tar.gz)",
                                                     QString(), "Gzip archive (*.tar.gz)");
    if (zip.isEmpty()) return;
    QString err;
    if (OcrPackage::installFromArchive(zip, &err)) {
        QMessageBox::information(this, "OCR package", "The OCR package was installed successfully.");
    } else {
        QMessageBox::warning(this, "OCR package", "Could not install the package.\n\n" + err);
    }
    refresh();
}

void OcrDialog::onRemovePackage() {
    const OcrPackage::Info info = OcrPackage::detect();
    if (QMessageBox::question(this, "Remove OCR package",
                              "Remove the damaged package so it can be reinstalled?\n\n" + shortenPath(info.root),
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;
    QDir dir(info.root);
    if (!dir.removeRecursively()) {
        QMessageBox::warning(this, "Remove OCR package",
                             "Could not remove the package folder:\n" + shortenPath(info.root));
    }
    refresh();
}

void OcrDialog::onChooseFolder() {
    const QString dir = QFileDialog::getExistingDirectory(this, "Choose OCR package folder",
                                                          OcrPackage::preferredRoot(nullptr));
    if (dir.isEmpty()) return;
    QSettings().setValue(QLatin1String("ocr/root"), dir);
    refresh();
}

void OcrDialog::onOpenFolder() {
    const QString root = OcrPackage::preferredRoot(nullptr);
    if (QDir(root).exists()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(root));
    } else {
        QMessageBox::information(this, "Open folder",
                                 "The folder does not exist yet \u2014 it is created when the package is installed.\n\n"
                                 + shortenPath(root));
    }
}

// ── Nhan dang ────────────────────────────────────────────────────────────────

void OcrDialog::onRecognizePage() {
    if (!m_doc || !m_doc->isOpen()) return;
    startRecognize(m_docCurrentPage, m_docCurrentPage);
}

void OcrDialog::onRecognizeWhole() {
    if (!m_doc || !m_doc->isOpen()) return;
    startRecognize(0, m_doc->pageCount() - 1);
}

void OcrDialog::startRecognize(int firstPage, int lastPage) {
    if (firstPage < 0 || lastPage < firstPage) return;

    FPDF_DOCUMENT raw = m_doc->raw();
    const int dpi = OcrEngine::kDefaultDpi;
    const QString langs = QStringLiteral("vie+eng");
    m_ocrCancelled.storeRelaxed(0);
    m_ocrCancelBtn->setEnabled(true);

    setRecognizing(true);
    m_ocrProgress->setRange(0, lastPage - firstPage + 1);
    m_ocrProgress->setValue(0);

    QFuture<OcrResult> fut = QtConcurrent::run([this, raw, dpi, langs, firstPage, lastPage]() {
        OcrResult res;
        QElapsedTimer wTimer; wTimer.start();
        for (int p = firstPage; p <= lastPage && !m_ocrCancelled.loadRelaxed(); ++p) {
            auto words = OcrEngine::recognizePage(raw, p, langs, dpi,
                                                  [this]() { return m_ocrCancelled.loadRelaxed() != 0; });
            if (!words.isEmpty())
                OcrTextLayer::insertPage(raw, p, words);
            res.words += words.size();
            ++res.pages;
            emit ocrProgress(res.pages, lastPage - firstPage + 1);
            QThread::yieldCurrentThread();
        }
        res.elapsedMs = wTimer.elapsed();
        return res;
    });
    m_ocrWatcher.setFuture(fut);
}

void OcrDialog::onOcrProgress(int done, int total) {
    m_ocrProgress->setRange(0, total);
    m_ocrProgress->setValue(done);
}

void OcrDialog::onOcrFinished() {
    setRecognizing(false);
    const OcrResult res = m_ocrWatcher.result();
    m_ocrStatusLabel->setText(
        QStringLiteral("Recognized %1 page(s), %2 word(s) in %3 s.")
            .arg(res.pages).arg(res.words)
            .arg(QString::number(res.elapsedMs / 1000.0, 'f', 1)));
}

void OcrDialog::setRecognizing(bool busy) {
    m_ocrPageBtn->setEnabled(!busy && m_doc && m_doc->isOpen()
                             && OcrEngine::available(nullptr));
    m_ocrWholeBtn->setEnabled(m_ocrPageBtn->isEnabled());
    m_ocrProgress->setVisible(busy);
    m_ocrCancelBtn->setVisible(busy);
    m_downloadBtn->setEnabled(!busy);
    m_installBtn->setEnabled(!busy);
    if (busy) {
        m_ocrStatusLabel->setText("Recognizing\u2026 you can keep using the app.");
    }
}
