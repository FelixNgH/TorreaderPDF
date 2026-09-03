#include "OcrPanel.h"
#include "ThemeTokens.h"
#include "../core/PdfDocument.h"
#include "../core/OcrTextLayer.h"
#include "../core/PageCache.h"
#include "../core/OcrEngine.h"
#include "../core/PdfiumLock.h"

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
#include <QTimer>
#include <QtConcurrent>
#include <QPointer>
#include <QApplication>
#include <fpdfview.h>
#include <fpdf_text.h>
#include <fpdf_edit.h>   // FPDFPage_CountObjects

extern QMutex s_pdfiumMutex;

namespace {

// ── Cache "trang co chu" (SPEC_PERF_DESK_ABOUT phan 1.1) ────────────────
// Bo dem theo (doc, pageIndex). Gia tri CHI doi khi OCR chen chu vao trang,
// luc do invalidatePage. Dong doc thi clearDocument. Truy cap tu luong giao
// dien la du (worker paste qua queued invokeMethod), them mutex cho chac.
static QMutex   g_hasTextMutex;
static QHash<OcrTextCache::DocHandle, QHash<int,bool>> g_hasTextCache;

// Ngưỡng "trang nang" cho phep kiem "co chu": CUNG gia tri voi chan >400000 cua
// [fgnlayer] SKIP (MainWindow) va fullQCapPx prefetch (PdfRenderer) — cung nguon
// FPDFPage_CountObjects. Do 02/09 tren trang 2.540.585 object: FPDF_LoadPage =
// 1.686 ms (khong duoc lam duoi khoa), FPDFText_LoadPage tren trang DA parse van
// = 155 ms (> 50 ms), FPDFPage_CountObjects = 0,35 ms, FPDFText_CountChars = 0 ms.
static constexpr int kSkipTextCheckObjects = 400000;

int pageHasTextCachedFlag(FPDF_DOCUMENT doc, int pageIndex) {
    QMutexLocker lock(&g_hasTextMutex);
    auto dIt = g_hasTextCache.constFind(OcrTextCache::DocHandle(doc));
    if (dIt == g_hasTextCache.cend()) return -1;
    auto pIt = dIt->constFind(pageIndex);
    if (pIt == dIt->cend()) return -1;
    return pIt.value() ? 1 : 0;
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
    m_hasTextChecking.clear();   // trang kiem thuoc doc cu — bo het
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
    ensureHasTextKnown();
    m_statusFull = statusText();
    elideStatus();
    updateButtonState();
}

// Dam bao bo dem "trang co chu" da co gia tri truoc khi statusText doc (chi doc
// bo dem — khong parse dong bo). Va 02/09 (SPEC_PERF_HEAVYPAGE, huong (b) + chan
// (a)): luong nen KHONG duoc GIU s_pdfiumMutex luc FPDF_LoadPage — do 19/08 thay
// day sang luong nen van giu khoa 2.513 ms tren trang CAD 2,54 trieu object, luong
// chinh cho 2.262 giay ([lockwait] MainWindow:1831). Chay o dau khong giai quyet
// tranh khoa — chi KHONG NAP trang va KHONG mo text page trang nang moi het.
// Worker chi duoc giu khoa cho: (1) FPDFText_CountChars tren text page DA ton tai
// (0 ms), (2) FPDFPage_CountObjects O(1) (0,35 ms) de chan trang >400k object.
// Trang chua trong PageCache hoac nang ma chua co text page → HOAN (tri hoan,
// trang van o trang thai trung tinh "checking…"), m_hasTextRetry no 1 giay sau —
// an ke khi bo dung hinh/OCR/search da nap trang va dung text page.
// Cay an toan: moi worker chi post VE MOT LAN (done → setHasText; tri hoan →
// armHasTextRetry), nen tai mot trang luong co nhat mot worker song — khong
// tich lop QtConcurrent. Handle tu tryAcquire duoc dung TRONG luc giu
// s_pdfiumMutex: moi duong evict/invalidate/closeEntry deu can khoa do, nen
// handle khong the chet giua chung — khong can PageBorrow (borrow chi can khi
// tha khoa giua chung, nhu render slice). QPointer phong panel bi xoa.
void OcrPanel::ensureHasTextKnown() {
    if (!m_doc || !m_doc->isOpen()) return;
    const int page = m_currentPage >= 0 ? m_currentPage : 0;
    FPDF_DOCUMENT raw = m_doc->raw();
    if (!raw) return;
    const auto dh = reinterpret_cast<OcrTextCache::DocHandle>(raw);
    if (OcrTextCache::hasTextStatus(dh, page) != -1) return;  // da biet
    if (m_hasTextChecking.contains(page)) return;             // dang kiem roi
    m_hasTextChecking.insert(page);

    FPDF_DOCUMENT d = raw;
    const int pg = page;
    QPointer<OcrPanel> self(this);
    (void)QtConcurrent::run([d, pg, self]() {
        bool done = false, has = false;
        {
            TimedPdfiumLock lk(__FILE__, __LINE__);
            FPDF_TEXTPAGE tp = PageCache::tryAcquireTextPage(d, pg);
            if (tp) {                                   // co roi → an ke, 0 ms
                has = (FPDFText_CountChars(tp) > 0);
                done = true;
            } else if (FPDF_PAGE p = PageCache::tryAcquire(d, pg)) {
                // tryAcquire (KHONG phai acquire): HIT-only — acquire se
                // FPDF_LoadPage duoi khoa = 1.686 ms tren trang CAD.
                if (FPDFPage_CountObjects(p) <= kSkipTextCheckObjects) {
                    FPDF_TEXTPAGE t = PageCache::textPage(d, pg);  // trang nhe: ms
                    has = t && (FPDFText_CountChars(t) > 0);
                    done = true;
                }
                // Trang nang + chua co text page → tri hoan, khong mo text page.
            }
            // Trang chua trong dem → tri hoan cho bo dung hinh nap.
        }
        // setHasText/xoa trang kiem/cap nhat UI PHAI o luong chinh.
        QMetaObject::invokeMethod(qApp, [self, d, pg, done, has]() {
            if (!self) return;
            self->m_hasTextChecking.remove(pg);
            if (done) {
                OcrTextCache::setHasText(reinterpret_cast<OcrTextCache::DocHandle>(d), pg, has);
                self->updateStatus();
            } else {
                self->armHasTextRetry();
            }
        }, Qt::QueuedConnection);
    });
}

// Duy nhat MOT worker tri hoan moi trang: don (single-shot) — phat → updateStatus
// → ensureHasTextKnown lai. Gia mot lan kiem = hash lookup + CountObjects, duoi
// 1 ms. Worker co the CHO lay khoa o luong nen — luong chinh khong bao gio cho.
void OcrPanel::armHasTextRetry() {
    if (!m_hasTextRetry) {
        m_hasTextRetry = new QTimer(this);
        m_hasTextRetry->setSingleShot(true);
        m_hasTextRetry->setInterval(1000);
        connect(m_hasTextRetry, &QTimer::timeout, this, &OcrPanel::updateStatus);
    }
    if (!m_hasTextRetry->isActive()) m_hasTextRetry->start();
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
    // CHI doc bo dem — khong parse dong bo (SPEC_PERF_HEAVYPAGE). ensureHasTextKnown()
    // da khoi dong kiem o luong nen; worker xong set cache roi goi updateStatus lai.
    const int st = OcrTextCache::hasTextStatus(
        reinterpret_cast<OcrTextCache::DocHandle>(raw), page);
    if (st == 1)
        return QStringLiteral("Page %1 — has text").arg(pageLabel);
    if (st == -1)   // chua biet — dang kiem o luong nen
        return QStringLiteral("Page %1 — checking text…").arg(pageLabel);
    if (OcrTextLayer::pageDone(raw, page)) {
        const int w = m_wordsByPage.value(page, -1);
        if (w > 0) return QStringLiteral("Page %1 — recognized (%2 words)").arg(pageLabel).arg(w);
        return QStringLiteral("Page %1 — recognized").arg(pageLabel);
    }
    return QStringLiteral("Page %1 — no text").arg(pageLabel);
}
