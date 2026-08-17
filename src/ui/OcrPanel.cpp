#include "OcrPanel.h"
#include "ThemeTokens.h"
#include "../core/PdfDocument.h"
#include "../core/OcrTextLayer.h"
#include "../core/OcrEngine.h"

#include <QSettings>
#include <QFileInfo>
#include <QStandardItemModel>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QComboBox>
#include <QLabel>
#include <QProgressBar>
#include <QFontMetrics>
#include <QDir>
#include <QDesktopServices>
#include <QUrl>
#include <QMutex>
#include <fpdfview.h>
#include <fpdf_text.h>

extern QMutex s_pdfiumMutex;

namespace {

// ── Cache "trang co chu" (SPEC_PERF_DESK_ABOUT phan 1.1) ────────────────
// Bo dem theo (doc, pageIndex). Gia tri CHI doi khi OCR chen chu vao trang,
// luc do invalidatePage. Dong doc thi clearDocument. Truy cap tu luong giao
// dien la du (worker paste qua queued invokeMethod), them mutex cho chac.
static QMutex   g_hasTextMutex;
static QHash<OcrTextCache::DocHandle, QHash<int,bool>> g_hasTextCache;

int pageHasTextCachedFlag(FPDF_DOCUMENT doc, int pageIndex) {
    QMutexLocker lock(&g_hasTextMutex);
    auto dIt = g_hasTextCache.constFind(OcrTextCache::DocHandle(doc));
    if (dIt == g_hasTextCache.cend()) return -1;
    auto pIt = dIt->constFind(pageIndex);
    if (pIt == dIt->cend()) return -1;
    return pIt.value() ? 1 : 0;
}

// Trang co chu THUC SU (FPDFText_CountChars > 0) khong? Khong LoadPage lien tuc
// voi nhau qua con tro; tra so ky tu qua charsOut (nullptr = khong can).
bool pageHasTextCount(FPDF_DOCUMENT doc, int pageIndex, int* charsOut) {
    if (!doc || pageIndex < 0) return false;
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
    if (!page) return false;
    int n = 0;
    FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
    if (tp) { n = FPDFText_CountChars(tp); FPDFText_ClosePage(tp); }
    FPDF_ClosePage(page);
    if (charsOut) *charsOut = n;
    return n > 0;
}

}  // namespace

// ── Cache "trang co chu" — thi hanh (SPEC_PERF_DESK_ABOUT phan 1.1) ───────────
namespace OcrTextCache {

int hasTextStatus(DocHandle doc, int pageIndex) {
    if (!doc || pageIndex < 0) return 0;
    return pageHasTextCachedFlag(reinterpret_cast<FPDF_DOCUMENT>(doc), pageIndex);
}

void setHasText(DocHandle doc, int pageIndex, bool hasText) {
    if (!doc || pageIndex < 0) return;
    QMutexLocker lock(&g_hasTextMutex);
    g_hasTextCache[doc][pageIndex] = hasText;
}

void invalidatePage(DocHandle doc, int pageIndex) {
    if (!doc || pageIndex < 0) return;
    QMutexLocker lock(&g_hasTextMutex);
    auto dIt = g_hasTextCache.find(doc);
    if (dIt == g_hasTextCache.end()) return;
    dIt->remove(pageIndex);
    if (dIt->isEmpty()) g_hasTextCache.erase(dIt);
}

void clearDocument(DocHandle doc) {
    if (!doc) return;
    QMutexLocker lock(&g_hasTextMutex);
    g_hasTextCache.remove(doc);
}

}  // namespace OcrTextCache

OcrPanel::OcrPanel(QWidget* parent) : QWidget(parent) {
    m_status = new QLabel(this);
    m_status->setWordWrap(false);
    m_status->setObjectName("ocrStatus");
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    m_wholeBtn = new QPushButton("Recognize whole document", this);
    m_wholeBtn->setObjectName("ocrWholeBtn");
    m_wholeBtn->setDefault(true);

    m_pageBtn = new QPushButton("Recognize current page", this);
    m_pageBtn->setObjectName("ocrPageBtn");

    m_langCombo = new QComboBox(this);
    m_langCombo->setObjectName("ocrLangCombo");
    // Nhan cho nguoi doc duoc, gia tri (data) la ma Tesseract. Moi muc la ghep
    // doi voi "eng" — Tesseract nap cang nhieu tieng cang cham va kem chinh xac,
    // khong co muc "tat ca ngon ngu". Muc nao thieu traineddata thi disabled.
    struct LangItem { const char* label; const char* code; };
    static const LangItem kLangItems[] = {
        {"Vietnamese + English (recommended)", "vie+eng"},
        {"Vietnamese only",                  "vie"},
        {"English only",                     "eng"},
        {"Chinese (Simplified) + English",   "chi_sim+eng"},
        {"Chinese (Traditional) + English",  "chi_tra+eng"},
        {"Japanese + English",               "jpn+eng"},
        {"Korean + English",                 "kor+eng"},
        {"French + English",                 "fra+eng"},
        {"Russian + English",                "rus+eng"},
        {"Portuguese + English",             "por+eng"},
        {"Spanish + English",                "spa+eng"},
    };
    const QString tessDir = OcrEngine::tessdataDir();
    auto* comboModel = qobject_cast<QStandardItemModel*>(m_langCombo->model());
    for (const LangItem& it : kLangItems) {
        const int idx = m_langCombo->count();
        m_langCombo->addItem(QString::fromUtf8(it.label), QString::fromUtf8(it.code));
        bool allFiles = true;
        if (!tessDir.isEmpty()) {
            const QStringList ids = QString::fromUtf8(it.code).split(QLatin1Char('+'));
            for (const QString& id : ids) {
                if (!QFileInfo::exists(tessDir + QLatin1Char('/') + id
                                       + QLatin1String(".traineddata"))) {
                    allFiles = false;
                    break;
                }
            }
        } else {
            allFiles = false;
        }
        if (!allFiles) {
            m_langCombo->setItemData(idx, QStringLiteral("Language pack not installed"),
                                     Qt::ToolTipRole);
            if (comboModel && comboModel->item(idx))
                comboModel->item(idx)->setEnabled(false);
        }
    }
    // Nho lua chon giua cac phien (QSettings), mac dinh vie+eng. Neu muc da luu
    // bi disabled (thieu file) thi lui ve mac dinh.
    QSettings settings;
    const QString saved = settings.value(QStringLiteral("ocr/langs"),
                                         QStringLiteral("vie+eng")).toString();
    int savedIdx = m_langCombo->findData(saved);
    if (savedIdx < 0 || !(comboModel && comboModel->item(savedIdx)
                          && comboModel->item(savedIdx)->isEnabled()))
        savedIdx = 0;
    m_langCombo->setCurrentIndex(savedIdx);
    connect(m_langCombo, &QComboBox::currentIndexChanged, this, [this](int i) {
        QSettings s;
        s.setValue(QStringLiteral("ocr/langs"), m_langCombo->itemData(i).toString());
    });
    m_langCombo->setToolTip(
        "Both languages together handles mixed Vietnamese/English drawings; "
        "single language is slightly faster.");

    m_progress = new QProgressBar(this);
    m_progress->setObjectName("ocrProgress");
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progressLabel = new QLabel(this);
    m_progressLabel->setObjectName("ocrProgressLabel");
    m_cancelBtn = new QPushButton("Cancel", this);
    m_cancelBtn->setObjectName("ocrCancelBtn");

    const QString ocrDir = QDir::toNativeSeparators(QDir::tempPath() + QLatin1String("/torreader-ocr"));
    m_openLogBtn = new QPushButton("Open OCR log folder", this);
    m_openLogBtn->setObjectName("ocrOpenFolderBtn");
    m_openLogBtn->setToolTip(ocrDir);   // duong dan day du chi o tooltip, khong hien chu
    m_openLogBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_openLogBtn, &QPushButton::clicked, this, [ocrDir] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(ocrDir));
    });

    // ── Layout ──
    auto* langRow = new QHBoxLayout;
    langRow->addWidget(new QLabel("Language:", this));
    langRow->addWidget(m_langCombo, 1);

    auto* progressRow = new QHBoxLayout;
    progressRow->addWidget(m_progress, 1);
    progressRow->addWidget(m_progressLabel);
    progressRow->addWidget(m_cancelBtn);

    auto* logRow = new QHBoxLayout;
    logRow->addWidget(m_openLogBtn);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(4);
    lay->addWidget(m_status);
    lay->addWidget(m_wholeBtn);
    lay->addWidget(m_pageBtn);
    lay->addLayout(langRow);
    lay->addLayout(progressRow);
    lay->addLayout(logRow);
    lay->addStretch(1);

    connect(m_wholeBtn, &QPushButton::clicked, this, [this] {
        emit recognizeWholeRequested(m_langCombo->currentData().toString());
    });
    connect(m_pageBtn, &QPushButton::clicked, this, [this] {
        emit recognizeCurrentPageRequested(m_langCombo->currentData().toString());
    });
    connect(m_cancelBtn, &QPushButton::clicked, this, &OcrPanel::cancelRequested);

    applyPanelTheme();
    setOcrRunning(false);
    updateButtonState();
}

void OcrPanel::setDocument(PdfDocument* doc) {
    m_doc = doc;
    m_wordsByPage.clear();
    m_currentPage = -1;
    setOcrRunning(false);
    m_hasRun = false;   // doc moi -> chua OCR
    updateButtonState();
    updateStatus();
}

void OcrPanel::setDarkMode(bool dark) {
    m_dark = dark;
    applyPanelTheme();
}

void OcrPanel::setCurrentPage(int page) {
    m_currentPage = page;
    updateStatus();
}

void OcrPanel::setOcrRunning(bool running) {
    const bool wasRunning = m_ocrRunning;
    m_ocrRunning = running;
    if (wasRunning && !running)
        m_hasRun = true;   // mot luot OCR da chay xong/huy -> nut doi thanh Re-recognize
    if (running) {
        m_progress->setValue(0);
        m_progressLabel->clear();
    }
    updateButtonState();
    if (!running) updateStatus();
}

void OcrPanel::setProgress(int pageIndex1Based, int totalPages) {
    m_progressPage = pageIndex1Based;
    m_progressTotal = totalPages;
    if (totalPages > 0) m_progress->setMaximum(totalPages);
    m_progress->setValue(pageIndex1Based);
    m_progressLabel->setText(QStringLiteral("page %1 / %2").arg(pageIndex1Based).arg(totalPages));
}

void OcrPanel::setPageWords(int page, int words) {
    m_wordsByPage.insert(page, words);
    updateStatus();
}

void OcrPanel::refresh() { updateStatus(); }

void OcrPanel::updateStatus() {
    m_statusFull = statusText();
    elideStatus();
    updateButtonState();
}

void OcrPanel::elideStatus() {
    if (!m_status || m_statusFull.isEmpty()) return;
    const QFontMetrics fm(m_status->font());
    const QString elided = fm.elidedText(m_statusFull, Qt::ElideMiddle, m_status->width());
    if (m_status->text() != elided) m_status->setText(elided);
}

void OcrPanel::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    elideStatus();
}

void OcrPanel::applyPanelTheme() {
    const ThemeTokens& t = m_dark ? darkHC() : lightHC();
    // QSS cuc bo CHI cho panel: cho nut phu mot vien nhin thay duoc (nut chinh
    // da co setDefault). Dung token theme, khong va cung mau. KHONG dong buildQss.
    setStyleSheet(QStringLiteral(
        "QPushButton#ocrPageBtn { border: 1px solid %1; }"
        "QPushButton#ocrPageBtn:disabled { border: 1px solid %1; color: %2; }")
        .arg(t.border, t.fgDim));
}

void OcrPanel::updateButtonState() {
    const bool hasDoc = m_doc && m_doc->isOpen();
    m_wholeBtn->setEnabled(hasDoc && !m_ocrRunning);
    m_pageBtn->setEnabled(hasDoc && !m_ocrRunning);
    m_langCombo->setEnabled(!m_ocrRunning);
    m_cancelBtn->setEnabled(m_ocrRunning);
    m_cancelBtn->setVisible(m_ocrRunning);
    m_progress->setVisible(m_ocrRunning);
    m_progressLabel->setVisible(m_ocrRunning);
    if (m_hasRun) {
        m_wholeBtn->setText("Re-recognize whole document");
        m_wholeBtn->setToolTip("Already recognized — click to run again");
    } else {
        m_wholeBtn->setText("Recognize whole document");
        m_wholeBtn->setToolTip(QString());
    }
}

QString OcrPanel::statusText() const {
    if (!m_doc || !m_doc->isOpen()) return QStringLiteral("No document open");
    const int page = m_currentPage >= 0 ? m_currentPage : 0;
    FPDF_DOCUMENT raw = m_doc->raw();
    if (!raw) return QStringLiteral("No document open");
    const QString pageLabel = QString::number(page + 1);
    if (pageHasTextCount(raw, page, nullptr))
        return QStringLiteral("Page %1 — has text").arg(pageLabel);
    if (OcrTextLayer::pageDone(raw, page)) {
        const int w = m_wordsByPage.value(page, -1);
        if (w > 0) return QStringLiteral("Page %1 — recognized (%2 words)").arg(pageLabel).arg(w);
        return QStringLiteral("Page %1 — recognized").arg(pageLabel);
    }
    return QStringLiteral("Page %1 — no text").arg(pageLabel);
}
