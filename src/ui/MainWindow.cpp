#include "MainWindow.h"
#include "../core/PdfiumLock.h"
#include "ThemeTokens.h"
#include <QDebug>
#include "PdfView.h"
#include "PdfGpuView.h"
#include "ThumbnailPanel.h"
#include "ContinuousView.h"
#include "FindBar.h"
#include "OcrPanel.h"
#include "MergeDialog.h"
#include "SignDialog.h"
#include "AboutDialog.h"
#include "core/OcrEngine.h"
#include "core/OcrTextLayer.h"
#include "core/PdfiumLock.h"
#include "core/DocTaskGate.h"
#include "core/Bisect.h"
#include "core/FastExit.h"
#include "PrintDialog.h"
#include "core/PdfDocument.h"
#include "core/PdfRenderer.h"
#include "core/PdfEditor.h"
#include "core/TextSearch.h"
#include "core/VectorLayer.h"
#include "core/ForeignAnnotLayer.h"
#include "core/OwnAnnotHideGuard.h"
#include "core/PdfCoords.h"
#include "core/PdfLinks.h"
#include "core/PageCache.h"
#include "core/VectorCacheFile.h"
#include "annotations/AnnotationManager.h"
#include "core/GoogleAuth.h"
#include "core/Translator.h"
#include "TranslationPopup.h"
#include "NoteInputDialog.h"
#include "core/UpdateChecker.h"
#include "GateDialog.h"
#include "UiProbe.h"

#include <fpdf_text.h>
#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <fpdf_annot.h>
#include <fpdf_save.h>
#include <cmath>
// 0927 LƯỢT 8: trần chờ pool render khi đóng tab/thoát app — LƯỢT 26: trị số giờ nằm
// ở mặc định `renderWaitMs = 3000` của MainWindow::shutdownTab (MainWindow.h).
#include <algorithm>
#include <vector>
#include <QList>
#include <functional>

extern QMutex s_pdfiumMutex;

#include <QApplication>
#include <QSplitter>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QToolBar>
#include <QLabel>
#include <QAction>
#include <QActionGroup>

#include <QStatusBar>
#include <QFileDialog>
#include <QFileInfo>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QCloseEvent>
#include <QTimer>
#include <QIcon>
#include <QElapsedTimer>
#include <QPixmap>
#include <QColorDialog>
#include <QInputDialog>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QFile>
#include <QPair>
#include <QTabBar>
#include <QShortcut>
#include <QToolButton>
#include <QVBoxLayout>
#include <QDir>
#include <QDateTime>
#include <QHash>
#include <QUrl>
#include <QDesktopServices>
#include <QRegularExpression>
#include <QToolTip>
#include <QCursor>
#include <QDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QClipboard>
#include <QGuiApplication>
#include <QSpinBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QPlainTextEdit>   // 0927 L6: probeEditAnnotViaGui tim QPlainTextEdit trong hop
#include <QPushButton>
#include <QCoreApplication>
#include <QFrame>
#include <QTextStream>
#include <QPointer>
#include <QMap>
#include <QSharedPointer>
#include <QScrollBar>
#include <QThread>
#ifdef Q_OS_WIN
#include <windows.h>   // 🔴 LƯỢT 28 (Thử A): GetProcessHeaps/HeapCompact
#include <heapapi.h>
#endif

namespace {
struct MarkupDebugWriter {
    FPDF_FILEWRITE base;
    QFile file;
    static int writeBlock(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
        auto* ctx = reinterpret_cast<MarkupDebugWriter*>(self);
        return ctx->file.write(static_cast<const char*>(data), size) == static_cast<qint64>(size) ? 1 : 0;
    }
};
}

// ── Helpers ──────────────────────────────────────────────────────────────────

// 🔴 LƯỢT 28 (VIỆC 1c — Thử A): HeapCompact MỌI heap cua process. Sau dong tab
// nang, free-list khoi lon (>16 KB, ngoai LFH) dai ra ⇒ moi lan malloc lon cua
// PDFium pha do dai free-list (L26: CPU/ranh-gioi object tang don dieu). Chi goi
// tren luong UI, sau closeJob.finished — KHONG doi thu tu dong tab.
// Do: TORREADER_HEAPCOMPACT=1. Neu so ung ⇒ mac dinh bat (TORREADER_NO_HEAPCOMPACT tat).
static void compactAllHeaps() {
#ifdef Q_OS_WIN
    const unsigned n = GetProcessHeaps(0, nullptr);
    if (n == 0 || n > 1024) return;
    std::vector<HANDLE> heaps(n);
    if (GetProcessHeaps(static_cast<DWORD>(n), heaps.data()))
        for (HANDLE h : heaps) HeapCompact(h, 0);
#endif
}
static bool heapCompactOnClose() {
    static const bool on = qEnvironmentVariableIsSet("TORREADER_HEAPCOMPACT")
        && !qEnvironmentVariableIsSet("TORREADER_NO_HEAPCOMPACT");
    return on;
}

// Write temp file to system temp dir (not next to PDF) to avoid permission issues
// on protected locations: Downloads, network shares, read-only USB, UAC folders.
static QString makeTmpPath(const QString& pdfPath) {
    return QDir::temp().filePath(
        QFileInfo(pdfPath).baseName() + "_" +
        QString::number(QDateTime::currentMSecsSinceEpoch()) + ".tortmp");
}

bool removeWorkingCopy(const QString& path) {
    if (path.isEmpty()) return false;
    QFileInfo fi(path);
    const QString tmpDir = QDir::temp().absolutePath();
    const bool inTmp  = fi.absoluteFilePath().startsWith(tmpDir + "/", Qt::CaseInsensitive);
    const bool isTort = (fi.suffix().compare("tortmp", Qt::CaseInsensitive) == 0);
    if (!inTmp || !isTort) {
        qWarning() << "[safety] TU CHOI xoa file khong phai ban nhap:" << path;
        return false;
    }
    return QFile::remove(path);
}

// Thay thế dest bằng nội dung của srcTmp sao cho: hoặc thành công hoàn toàn,
// hoặc dest GIỮ NGUYÊN như cũ. Không bao giờ để dest ở trạng thái mất/hỏng.
bool replaceFileAtomically(const QString& srcTmp, const QString& dest, QString* errOut) {
    // 1. srcTmp không tồn tại hoặc kích thước 0 → trả false ngay
    if (!QFileInfo::exists(srcTmp)) {
        if (errOut) *errOut = QStringLiteral("source does not exist: ") + srcTmp;
        return false;
    }
    if (QFileInfo(srcTmp).size() == 0) {
        if (errOut) *errOut = QStringLiteral("source is empty: ") + srcTmp;
        return false;
    }
    // 2. staged = dest + ".savetmp" — cùng thư mục với dest
    const QString staged = dest + QStringLiteral(".savetmp");
    QFile::remove(staged);
    // 3. QFile::copy(srcTmp, staged)
    if (!QFile::copy(srcTmp, staged)) {
        if (errOut) *errOut = QStringLiteral("copy to staged failed: ") + staged;
        QFile::remove(staged);
        return false;
    }
    // 4. Kiểm kích thước (bắt copy đứt giữa chừng)
    if (QFileInfo(staged).size() != QFileInfo(srcTmp).size()) {
        if (errOut) *errOut = QStringLiteral("staged size mismatch");
        QFile::remove(staged);
        return false;
    }
    // 5. Backup dest cũ nếu tồn tại
    bool haveBackup = false;
    const QString backup = dest + QStringLiteral(".savebak");
    if (QFileInfo::exists(dest)) {
        QFile::remove(backup);
        if (!QFile::rename(dest, backup)) {
            if (errOut) *errOut = QStringLiteral("backup rename failed: ") + dest;
            QFile::remove(staged);
            return false;
        }
        haveBackup = true;
    }
    // 6. Rename staged → dest
    if (!QFile::rename(staged, dest)) {
        if (haveBackup) QFile::rename(backup, dest);
        QFile::remove(staged);
        if (errOut) *errOut = QStringLiteral("final rename failed: ") + staged + QStringLiteral(" -> ") + dest;
        return false;
    }
    // 7. Thành công → xoá backup
    if (haveBackup) QFile::remove(backup);
    return true;
}

// ── Theme stylesheets ────────────────────────────────────────────────────────

// Dung CHUNG mot khuon QSS cho ca 2 theme; mau lay tu bang token.
// Phong cach High Contrast: ranh gioi do vien 1px (token border) tao ra,
// hover = DOI MAU VIEN sang token focus, KHONG to nen, KHONG doi do day.
// Moi thanh phan luon co san vien 1px nen hover khong gay xo lech boc cuc.
static QString buildQss(const ThemeTokens& t) {
    // Dung ten thay cho so de trach loi so sot (xem %6 bi thieu truoc day).
    QString qss = QStringLiteral(R"(
QMainWindow, QWidget                    { background:@bg@; color:@fg@; }
QSplitter::handle:horizontal               { background:@border@; width:1px; }
QSplitter::handle:vertical                 { background:@border@; height:1px; }
QToolBar                                { background:@bgAlt@; border-bottom:1px solid @border@; spacing:2px; padding:2px 8px; }
QToolBar::separator                     { background:@border@; width:1px; margin:4px 3px; }
QToolButton                             { color:@fg@; padding:2px 6px; border:1px solid transparent; background:transparent; }
QToolButton:hover                       { border:1px dashed @focus@; background:@hoverBg@; }
QToolButton:checked                     { background:@selBg@; color:@selFg@; border:1px solid @focus@; }
QToolButton:pressed                     { background:@selBg@; color:@selFg@; border:1px solid @focus@; }
QToolButton:focus                       { border:1px dotted @focus@; }
QTabWidget::pane                        { border:none; border-top:1px solid @border@; }
QTabBar                                 { background:transparent; }
QTabBar::tab                            { background:@bgAlt@; color:@fgDim@; padding:5px 14px; min-width:80px; border:1px solid transparent; border-bottom:1px solid @border@; margin-right:2px; margin-top:1px; }
QTabBar::tab:selected                   { background:@selBg@; color:@selFg@; border:1px solid @focus@; border-bottom:1px solid @border@; }
QTabBar::tab:hover:!selected            { border:1px dashed @focus@; border-bottom:1px solid @border@; background:@hoverBg@; }
QTabBar::tab:focus                      { border:1px dotted @focus@; border-bottom:1px solid @border@; }
QTabBar QToolButton                     { background:transparent; color:@fg@; border:1px solid transparent; min-width:20px; font-weight:bold; }
QTabBar QToolButton:hover               { border:1px dashed @focus@; background:@hoverBg@; }
QPushButton#sidebarTab                  { background:@bgAlt@; color:@fgDim@; border:1px solid transparent; border-bottom:1px solid @border@; border-right:1px solid @border@; border-radius:0; padding:3px 2px; font-size:11px; }
QPushButton#sidebarTab:checked          { background:@selBg@; color:@selFg@; border:1px solid @focus@; border-bottom:1px solid @border@; }
QPushButton#sidebarTab:hover:!checked   { border:1px dashed @focus@; border-bottom:1px solid @border@; background:@bgAlt@; }
QWidget#sidebarTabGrid                  { background:@bgAlt@; }
QFrame#sidebarSep                       { color:@border@; }
QStatusBar                              { background:@bgAlt@; color:@fgDim@; border-top:1px solid @border@; }
QListWidget, QTreeWidget                { background:@bg@; color:@fg@; border:1px solid @border@; outline:none; }
QListWidget::item, QTreeWidget::item    { border:1px solid transparent; padding:1px 2px; }
QListWidget::item:hover,
QTreeWidget::item:hover                 { border:1px dashed @focus@; background:@hoverBg@; }
QListWidget::item:selected,
QTreeWidget::item:selected              { background:@selBg@; color:@selFg@; border:1px solid @focus@; }
QScrollBar:vertical                     { background:@bgAlt@; width:10px; }
QScrollBar:horizontal                   { background:@bgAlt@; height:10px; }
QScrollBar::handle:vertical,
QScrollBar::handle:horizontal           { background:@sliderBg@; min-height:20px; min-width:20px; }
QScrollBar::handle:vertical:hover,
QScrollBar::handle:horizontal:hover     { background:@sliderHover@; }
QScrollBar::handle:vertical:pressed,
QScrollBar::handle:horizontal:pressed   { background:@sliderActive@; }
QScrollBar::add-line:vertical,
QScrollBar::sub-line:vertical           { height:0; }
QScrollBar::add-line:horizontal,
QScrollBar::sub-line:horizontal         { width:0; }
QWidget#contCorner                      { background:@bgAlt@; }
QFrame#ocrBar                           { background:@bgAlt@; color:@fg@; border:1px solid @border@; }
QFrame#ocrBar QLabel                    { background:transparent; color:@fg@; }
QFrame#ocrBar QPushButton#ocrAction     { background:@bgAlt@; color:@fg@; border:1px solid @border@; padding:2px 10px; }
QFrame#ocrBar QPushButton#ocrAction:hover { border:1px solid @focus@; }
QFrame#ocrBar QPushButton#ocrClose      { background:transparent; color:@fg@; border:1px solid transparent; padding:0px; font-size:14px; font-weight:bold; }
QFrame#ocrBar QPushButton#ocrClose:hover { border:1px dashed @focus@; }
QMenu                                   { background:@bgAlt@; color:@fg@; border:1px solid @border@; }
QMenu::item                             { padding:3px 18px; border:1px solid transparent; }
QMenu::item:hover                       { border:1px dashed @focus@; background:@hoverBg@; }
QMenu::item:selected                    { background:@selBg@; color:@selFg@; border:1px solid @focus@; }
QMenu::separator                        { background:@border@; height:1px; margin:2px 0; }
QDialog, QMessageBox                    { background:@bgAlt@; color:@fg@; border:1px solid @border@; }
QPushButton                             { background:@bgAlt@; color:@fg@; border:1px solid transparent; padding:4px 14px; }
QPushButton:hover                       { border:1px dashed @focus@; background:@hoverBg@; }
QPushButton:pressed                     { background:@selBg@; color:@selFg@; border:1px solid @focus@; }
QPushButton:default                     { border:1px solid @focus@; }
QPushButton:focus                       { border:1px dotted @focus@; }
QTextEdit, QLineEdit                    { background:@bg@; color:@fg@; border:1px solid @border@; padding:3px 6px; }
QTextEdit:focus, QLineEdit:focus        { border:1px solid @focus@; }
QComboBox                               { background:@bg@; color:@fg@; border:1px solid @border@; padding:3px 6px; }
QComboBox:focus                         { border:1px solid @focus@; }
QComboBox::drop-down                    { border-left:1px solid @border@; width:20px; }
QLabel                                  { background:transparent; }
)");
    qss.replace(QLatin1String("@bg@"),      QLatin1String(t.bg));
    qss.replace(QLatin1String("@bgAlt@"),   QLatin1String(t.bgAlt));
    qss.replace(QLatin1String("@fg@"),      QLatin1String(t.fg));
    qss.replace(QLatin1String("@fgDim@"),   QLatin1String(t.fgDim));
    qss.replace(QLatin1String("@border@"),  QLatin1String(t.border));
    qss.replace(QLatin1String("@accent@"),  QLatin1String(t.accent));
    qss.replace(QLatin1String("@focus@"),   QLatin1String(t.focus));
    qss.replace(QLatin1String("@selBg@"),   QLatin1String(t.selBg));
    qss.replace(QLatin1String("@selFg@"),   QLatin1String(t.selFg));
    qss.replace(QLatin1String("@warnBg@"),  QLatin1String(t.warnBg));
    qss.replace(QLatin1String("@hoverBg@"), QLatin1String(t.hoverBg));
    qss.replace(QLatin1String("@sliderBg@"),       QLatin1String(t.sliderBg));
    qss.replace(QLatin1String("@sliderHover@"),    QLatin1String(t.sliderHover));
    qss.replace(QLatin1String("@sliderActive@"),   QLatin1String(t.sliderActive));
    return qss;
}

// ── Constructor / Destructor ─────────────────────────────────────────────────

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    m_editor     = std::make_unique<PdfEditor>(this);
    m_textSearch = new TextSearch(this);

    menuBar()->hide();
    setupActionBar();
    new QShortcut(QKeySequence(Qt::Key_Delete), this, [this]{
        // LUOT 41 (30/09): Continuous chi-xem markup — Delete khong xoa khi
        // dang o Continuous (owner chot 30/09). Single (PdfGpuView) giu nguyen.
        if (probeContinuousVisible()) {
            statusBar()->showMessage(
                "Chế độ Continuous chỉ xem — sang Single để sửa markup", 2500);
            return;
        }
        if (m_selPage >= 0 && m_selIdx >= 0) deleteSelectedAnnot(m_selPage, m_selIdx);
    });
    // Ctrl+C trong che do Select → Copy vung chon vao clipboard (SPEC_TEXTSEL_ADOBE).
    {
        auto* a = new QAction(this);
        a->setShortcutContext(Qt::ApplicationShortcut);
        a->setShortcut(QKeySequence::Copy);
        connect(a, &QAction::triggered, this, [this]{
            auto* t = currentTab();
            if (!t || !t->textSel.active) return;
            copyTextSelectionToClipboard();
        });
        addAction(a);
    }
    // Ctrl+A trong che do Select → chon toan bo chu trang hien tai.
    {
        auto* a = new QAction(this);
        a->setShortcutContext(Qt::ApplicationShortcut);
        a->setShortcut(QKeySequence(QKeySequence::SelectAll));
        connect(a, &QAction::triggered, this, [this]{
            auto* t = currentTab();
            if (!t || !t->doc || !t->doc->isOpen()) return;
const int page = (m_fastMode && m_continuousView)
                                  ? m_continuousView->currentPage() : t->currentPage;
            const TextSelection::PageInfo info = TextSelection::pageFor(t->doc->raw(), page);
            if (!info.tp) return;
            int total = 0;
            { TimedPdfiumLock lock(__FILE__, __LINE__); total = FPDFText_CountChars(info.tp); }
            if (total > 0)
                onTextSelectionChanged(page, 0, page, total - 1);
        });
        addAction(a);
    }
    // Ctrl+Shift+F12: chup cua so + dump mau ra %TEMP% (SPEC_PROBE_LOG_SNAPSHOT muc 3).
    {
        auto* a = new QAction(this);
        a->setShortcutContext(Qt::ApplicationShortcut);
        a->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F12")));
        connect(a, &QAction::triggered, this, &MainWindow::captureUiSnapshot);
        addAction(a);
    }
    // ── Find bar shortcuts (application-level, don't let widgets eat them) ──
    { auto* a = new QAction(this); a->setShortcutContext(Qt::ApplicationShortcut);
      a->setShortcut(QKeySequence("Ctrl+F"));
      connect(a, &QAction::triggered, this, [this]{
          if (!m_findBar) return;
          // Position bar at top-right of the right panel
          if (auto* p = qobject_cast<QWidget*>(m_findBar->parent())) {
              int bw = m_findBar->sizeHint().width();
              int x = qMax(0, p->width() - bw - 8);
              const int tabH = m_docTabs->tabBar()->height();
              const int bh   = m_findBar->sizeHint().height();
              const int y    = qMax(0, (tabH - bh) / 2);
              m_findBar->move(x, y);
              m_findBar->resize(bw, m_findBar->sizeHint().height());
          }
          m_findBar->showAndFocus();
      }); addAction(a); }
    { auto* escAction = new QAction(this); escAction->setShortcutContext(Qt::ApplicationShortcut);
      escAction->setShortcut(QKeySequence(Qt::Key_Escape));
      connect(escAction, &QAction::triggered, this, [this]{
          if (m_findBar && m_findBar->isVisible()) {
              m_findBar->closeBar();
              return;
          }
          clearTextSelection();
          pushToolToViews(PdfGpuView::ViewTool::Pan, 0);
          if (m_selectTextAct) m_selectTextAct->setChecked(false);
          m_selPage = -1; m_selIdx = -1;
      }); addAction(escAction); }
    { auto* a = new QAction(this); a->setShortcutContext(Qt::ApplicationShortcut);
      a->setShortcut(QKeySequence(Qt::Key_F3));
      connect(a, &QAction::triggered, this, [this]{
          if (m_findBar && m_findBar->isVisible()) {
              // Simulate Enter for next match
              QMetaObject::invokeMethod(m_findBar, "onNext", Qt::QueuedConnection);
          } else if (m_findBar) {
              // Reopen and search with last query
              m_findBar->showAndFocus();
              QMetaObject::invokeMethod(m_findBar, "onReturnPressed", Qt::QueuedConnection);
          }
      }); addAction(a); }
    { auto* a = new QAction(this); a->setShortcutContext(Qt::ApplicationShortcut);
      a->setShortcuts({QKeySequence("Shift+F3"), QKeySequence("Ctrl+Shift+F3")});
      connect(a, &QAction::triggered, this, [this]{
          if (m_findBar && m_findBar->isVisible())
              QMetaObject::invokeMethod(m_findBar, "onPrev", Qt::QueuedConnection);
      }); addAction(a); }

    new QShortcut(QKeySequence(Qt::Key_PageDown), this, [this]{
        if (auto* t = currentTab()) {
            int n = qMin(t->currentPage + 1, t->doc->pageCount() - 1);
            if (n != t->currentPage) onPageChanged(n);
        }
    });
    new QShortcut(QKeySequence(Qt::Key_PageUp), this, [this]{
        if (auto* t = currentTab()) {
            int n = qMax(t->currentPage - 1, 0);
            if (n != t->currentPage) onPageChanged(n);
        }
    });
    new QShortcut(QKeySequence(Qt::Key_Home), this, [this]{
        if (auto* t = currentTab()) if (t->currentPage != 0) onPageChanged(0);
    });
    new QShortcut(QKeySequence(Qt::Key_End), this, [this]{
        if (auto* t = currentTab()) {
            int last = t->doc->pageCount() - 1;
            if (t->currentPage != last) onPageChanged(last);
        }
    });

    m_splitter = new QSplitter(Qt::Horizontal, this);

    m_thumbPanel = new ThumbnailPanel(m_splitter);

    // Anh thumbnail cua MOI trang duoc dung lam anh THO cho che do lien tuc:
    // pan/zoom co ngay hinh mo de ve, net dan sau khi khung hinh on dinh.
    connect(m_thumbPanel, &ThumbnailPanel::lowResPageAvailable, this,
            [this](int pg, const QImage& img) { if (m_continuousView) m_continuousView->setPageLowRes(pg, img); });


    m_thumbPanel->setMinimumWidth(220);  // 2 tab buttons per row + spacing

    m_docTabs = new QTabWidget;
    m_docTabs->setTabsClosable(true);
    m_docTabs->setMovable(true);
    // documentMode bat khong duoc: Fusion ve them vach trang o vien tab bar
    m_docTabs->setElideMode(Qt::ElideRight);
    m_docTabs->tabBar()->setAutoHide(false);  // always show tab bar, even with a single file
    addWelcomeTab();

    m_continuousView = new ContinuousView;

    // 🔴 SUA 2026-08-31: khoi connect nay TRUOC DAY nam o dong ~415, tuc TRUOC khi
    // m_continuousView duoc tao (dong 431) => Qt bao "connect(ContinuousView, MainWindow):
    // invalid nullptr parameter" va duong xin thumbnail CHUA BAO GIO chay. Do la ly do
    // Continuous van hien "Loading..." du da co co che bac 2.
    // VIỆC 3: ContinuousView xin thumbnail cho trang visible chua co noi dung.
    // Pool tu dedup boi m_queuedPrio nen khong spam; uu tien 0 = cao nhat.
    connect(m_continuousView, &ContinuousView::needThumbnail, this, [this](int page) {
        auto* t = currentTab();
        if (t && t->thumbPool && t->thumbPool->isOpen())
            t->thumbPool->requestThumbnail(page, 0);
    });

    // Right panel: tab bar always visible; continuous view shown below when active.
    auto* rightPanel = new QWidget(m_splitter);
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    rightLayout->addWidget(m_docTabs);
    rightLayout->addWidget(m_continuousView);
    m_continuousView->hide();

    m_splitter->setSizes({180, 820});
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    setCentralWidget(m_splitter);

    // ── Find bar (floating over right panel) ─────────────────────────
    m_findBar = new FindBar(rightPanel);
    m_findBar->hide();
    // Reposition FindBar on parent resize
    rightPanel->installEventFilter(this);

    connect(m_docTabs, &QTabWidget::currentChanged,    this, &MainWindow::onTabChanged);
    connect(m_docTabs, &QTabWidget::tabCloseRequested, this, &MainWindow::onTabClose);
    connect(m_thumbPanel, &ThumbnailPanel::requestComments, this, &MainWindow::onCommentsRequested);
    connect(m_thumbPanel, &ThumbnailPanel::pageClicked,
            this, &MainWindow::onPageChanged);
    connect(m_thumbPanel, &ThumbnailPanel::pageContextMenu,
            this, &MainWindow::showThumbnailContextMenu);
    connect(m_thumbPanel, &ThumbnailPanel::annotToolSelected, this, [this](int id){
        // Dong bo 2 chieu (SPEC_FIX_PICK_TOOL): bam tool khac Select (id != 10)
        // thi tat nut toolbar Select; bam Select o sidebar thi nut toolbar sang.
        // setChecked TRUOC khi pushToolToViews de signal toggled cua nut toolbar
        // (day Pan khi tat) khong ghi de len tool vua chon — vi du bam Rect khi
        // Select dang ON phai ra Rect, khong ra Pan.
        if (m_selectTextAct) m_selectTextAct->setChecked(id == 10);
        pushToolToViews(static_cast<PdfGpuView::ViewTool>(id), id);
    });
    // Insert Image (SPEC_INSERT_IMAGE_2026-08-30): nut Insert trong tab Comments.
    connect(m_thumbPanel, &ThumbnailPanel::insertImageRequested,
            this, &MainWindow::onInsertImage);
    // ── OCR tab trong sidebar (SPEC_OCR_TAB phan 1b) ────────────────────
    // Panel chi phat lenh + hien trang thai; chinh runOcr lo chay nhan dang.
    if (auto* ocrP = m_thumbPanel->ocrPanel()) {
        connect(ocrP, &OcrPanel::recognizeWholeRequested,
                this, &MainWindow::onOcrWholeFromTab);
        connect(ocrP, &OcrPanel::recognizeCurrentPageRequested,
                this, &MainWindow::onOcrPageFromTab);
        connect(ocrP, &OcrPanel::cancelRequested, this, [this] {
            if (m_ocrCancel) m_ocrCancel->storeRelaxed(1);
        });
        connect(this, &MainWindow::ocrProgress, ocrP, &OcrPanel::setProgress);
        connect(this, &MainWindow::ocrPageFinished, ocrP,
                [ocrP](int page, int words) {
            ocrP->setPageWords(page, words);
            ocrP->refresh();
        });
    }
    connect(m_thumbPanel, &ThumbnailPanel::commentActivated, this, &MainWindow::onCommentActivated);
    connect(m_thumbPanel, &ThumbnailPanel::commentTextEdited, this,
            [this](int page, int idx, const QString& text) {
        auto* t = currentTab();
        if (!t || !t->annotMgr) return;
        const auto& lst = annotsForPage(t, page);
        if (idx < 0 || idx >= lst.size()) return;
        const QString oldText = lst[idx].text;
        if (oldText == text) return;
        const QString uid = lst[idx].uid;
        if (!t->annotMgr->setAnnotContents(page, idx, text)) return;
        MarkupUndoEntry ue;
        ue.kind = MarkupUndoEntry::ContentsEdit;
        ue.page = page; ue.uid = uid;
        ue.oldText = oldText; ue.newText = text;
        if (!ue.uid.isEmpty()) pushUndo(t, ue);
        t->dirty = true; updateTabDirty(t);
        invalidateAnnotPage(t, page);
        refreshAnnotVisuals(t, page);
        refreshCommentsForPage(t, page);
        if (t->view) t->view->update();
    });
    connect(m_thumbPanel, &ThumbnailPanel::annotStyleChanged, this,
            [this](QColor color, double width, bool fill, int fillOpacityPct, double fontSize){
        m_annotStyle.strokeColor = color;
        m_annotStyle.strokeWidth = static_cast<float>(width);
        m_annotStyle.fontSize    = static_cast<float>(fontSize);
        m_annotStyle.opacity     = 1.0f;
        QColor fc = color;
        fc.setAlpha(qBound(0, qRound(255.0 * fillOpacityPct / 100.0), 255));
        m_annotStyle.fillColor = fill ? fc : QColor(Qt::transparent);
    });

    // Text search — shared state between FindBar and SearchPanel
    connect(m_thumbPanel, &ThumbnailPanel::searchRequested,
            this, [this](const QString& query, bool matchDiacritics) {
        auto* t = currentTab();
        if (!t || !t->doc->isOpen() || query.trimmed().isEmpty()) return;
        handleSearchRequest(query, Qt::CaseInsensitive, matchDiacritics);
    });
    // Found results go to both SearchPanel and FindBar's result tracking
    connect(m_textSearch, &TextSearch::found,
            m_thumbPanel, &ThumbnailPanel::addSearchResult);
    connect(m_textSearch, &TextSearch::progress,
            m_thumbPanel, &ThumbnailPanel::setSearchProgress);
    connect(m_thumbPanel, &ThumbnailPanel::searchResultSelected,
            this, [this](int page, QList<QRectF> rects) {
        auto* t = currentTab();
        if (!t) return;
        m_textSearch->cancel();
        t->searchCurrentIdx = -1;
        if (rects.isEmpty()) return;
        const QRectF firstRect = rects.first();

        // Tim chi so cua ket qua vua bam trong ket qua cua tab (so theo page + rect dau).
        for (int i = 0; i < t->searchResults.size(); ++i) {
            const SearchResult& r = t->searchResults[i];
            if (r.pageIndex != page || r.rects.isEmpty()) continue;
            if (r.rects.first() == firstRect) { t->searchCurrentIdx = i; break; }
        }

        onPageChanged(page);

        // Cuon toi DUNG VI TRI: tam ket qua vao giua vung nhin. GiU NGUYEN zoom.
        QPoint scrollBefore(0, 0), scrollAfter(0, 0);
        QPointF rectCenterVp(1e9, 1e9);
        if (m_fastMode && m_continuousView) {
            auto* hb = m_continuousView->horizontalScrollBar();
            auto* vb = m_continuousView->verticalScrollBar();
            scrollBefore = QPoint(hb->value(), vb->value());
            m_continuousView->scrollToPageRect(page, firstRect);
            scrollAfter = QPoint(hb->value(), vb->value());
            rectCenterVp = m_continuousView->probeRectCenterInViewport(page, firstRect);
        } else if (auto* t = currentTab()) {
            if (auto* view = t->view) {
                rectCenterVp = view->pdfToWidget(firstRect.center());
                scrollBefore = rectCenterVp.toPoint();
                view->centerOnPageRect(firstRect);
                rectCenterVp = view->pdfToWidget(firstRect.center());
                scrollAfter = rectCenterVp.toPoint();
            }
        }

        applySearchHighlights(t->searchResults, t->searchCurrentIdx);
        if (m_findBar) m_findBar->setCurrentMatch(t->searchCurrentIdx);

        // Nghiem thu bang so: rect co trong vung nhin va tam cach tam vung nhin
        // khong qua 10% chieu cao vung nhin?
        bool rectTrongVungNhin = false;
        int vpW = 0, vpH = 0;
        if (m_fastMode && m_continuousView) {
            vpW = m_continuousView->viewport()->width();
            vpH = m_continuousView->viewport()->height();
        } else if (auto* t = currentTab()) {
            if (auto* view = t->view) { vpW = view->width(); vpH = view->height(); }
        }
        if (vpW > 0 && vpH > 0)
            rectTrongVungNhin = (rectCenterVp.x() >= 0 && rectCenterVp.x() <= vpW
                                 && rectCenterVp.y() >= 0 && rectCenterVp.y() <= vpH
                                 && qAbs(rectCenterVp.y() - vpH / 2.0) <= 0.1 * vpH);
// Chi tiet de doi chieu: vi tri tam rect trong viewport va gioi han cuon.
        QString diag;
        if (m_fastMode && m_continuousView) {
            diag = QStringLiteral(" rectCenterVp=%1,%2 rangeYMax=%3")
                        .arg(rectCenterVp.x(), 0, 'f', 1).arg(rectCenterVp.y(), 0, 'f', 1)
                        .arg(m_continuousView->verticalScrollBar()->maximum());
        } else {
            diag = QStringLiteral(" rectCenterVp=%1,%2 vpH=%3")
                        .arg(rectCenterVp.x(), 0, 'f', 1).arg(rectCenterVp.y(), 0, 'f', 1)
                        .arg(vpH);
        }
        qInfo().noquote() << QString("[searchnav] page=%1 rectPdf=%2,%3,%4,%5 scrollBefore=%6,%7 scrollAfter=%8,%9 rectTrongVungNhin=%10%11")
            .arg(page)
            .arg(QString::number(firstRect.x(), 'f', 1))
            .arg(QString::number(firstRect.y(), 'f', 1))
            .arg(QString::number(firstRect.width(), 'f', 1))
            .arg(QString::number(firstRect.height(), 'f', 1))
            .arg(scrollBefore.x()).arg(scrollBefore.y())
            .arg(scrollAfter.x()).arg(scrollAfter.y())
            .arg(rectTrongVungNhin ? 1 : 0)
            .arg(diag);
    });

    // FindBar connections
    connect(m_findBar, &FindBar::searchRequested,
            this, [this](const QString& query, Qt::CaseSensitivity cs, bool matchDiacritics) {
        auto* t = currentTab();
        if (!t || !t->doc->isOpen() || query.trimmed().isEmpty()) return;
        handleSearchRequest(query, cs, matchDiacritics);
    });
    connect(m_textSearch, &TextSearch::found,
            this, [this](const SearchResult& r) {
        DocTab* t = m_searchTab;
        if (!t || !m_openDocs.contains(t)) t = currentTab();
        if (!t) return;
        t->searchResults.append(r);
        if (m_findBar) m_findBar->onFound();
    });
    connect(m_textSearch, &TextSearch::searchComplete,
            this, [this](int total) {
        DocTab* t = m_searchTab;
        m_searchTab = nullptr;
        if (!t || !m_openDocs.contains(t)) t = currentTab();
        if (!t) return;
        t->searchCurrentIdx = total > 0 ? 0 : -1;
        if (m_findBar) {
            m_findBar->onSearchComplete(total);
            if (total > 0) m_findBar->setCurrentMatch(0);
        }
        // Chi ap highlight neu tab so huu lan tim kiem con la tab dang xem.
        if (total > 0 && t == currentTab())
            applySearchHighlights(t->searchResults, 0);
    });
    connect(m_findBar, &FindBar::navigateNext,
            this, [this]() {
        auto* t = currentTab();
        if (!t || t->searchResults.isEmpty()) return;
        m_textSearch->cancel();
        int prevIdx = t->searchCurrentIdx;
        t->searchCurrentIdx = (t->searchCurrentIdx + 1) % t->searchResults.size();
        if (prevIdx > t->searchCurrentIdx)
            statusBar()->showMessage("Reached end of document, continued from top", 3000);
        const auto& r = t->searchResults[t->searchCurrentIdx];
        onPageChanged(r.pageIndex);
        if (m_fastMode && m_continuousView)
            m_continuousView->scrollToPage(r.pageIndex, "findNext");
        applySearchHighlights(t->searchResults, t->searchCurrentIdx);
        if (m_findBar) m_findBar->setCurrentMatch(t->searchCurrentIdx);
    });
    connect(m_findBar, &FindBar::navigatePrev,
            this, [this]() {
        auto* t = currentTab();
        if (!t || t->searchResults.isEmpty()) return;
        m_textSearch->cancel();
        int prevIdx = t->searchCurrentIdx;
        t->searchCurrentIdx = (t->searchCurrentIdx - 1 + t->searchResults.size()) % t->searchResults.size();
        if (prevIdx < t->searchCurrentIdx)
            statusBar()->showMessage("Reached beginning of document, continued from end", 3000);
        const auto& r = t->searchResults[t->searchCurrentIdx];
        onPageChanged(r.pageIndex);
        if (m_fastMode && m_continuousView)
            m_continuousView->scrollToPage(r.pageIndex, "findNext");
        applySearchHighlights(t->searchResults, t->searchCurrentIdx);
        if (m_findBar) m_findBar->setCurrentMatch(t->searchCurrentIdx);
    });
    connect(m_findBar, &FindBar::clearSearchHighlights,
            this, &MainWindow::clearAllSearchHighlights);
    connect(m_thumbPanel, &ThumbnailPanel::searchCleared, this, [this] {
        m_textSearch->cancel();
        if (auto* t = currentTab()) {
            t->searchResults.clear();
            t->searchCurrentIdx = -1;
            t->searchQuery.clear();
        }
        clearAllSearchHighlights();
        if (m_findBar) m_findBar->reset();
        qDebug() << "[find] search cleared";
    });

    // Continuous view page/zoom sync
    connect(m_continuousView, &ContinuousView::pageChanged,
            this, [this](int page) {
        // 🔴 LƯỢT 33b (L): su kien nay cua ContinuousView DUNG — khi o che do Single
        // (view an) hen gio 80 ms cuoi cung cua lan doi tab cu van no va ghi de
        // currentPage cua tab dang xem ve 0 ⇒ mat vi tri. Single co duong rieng
        // (PdfGpuView::scrolledToPage → onPageChanged). Chi x ly khi o Continuous.
        if (!m_fastMode) return;
        if (auto* t = currentTab()) {
            t->currentPage = page;
            // 🔴 LƯỢT 33b (L): luu offset cuon THAT de quay lai tab khong nhay ve dinh.
            if (m_continuousView) { t->contScrollY = m_continuousView->scrollY(); t->contPosSaved = true; }
            // 🔴🔴 2026-09-01: bao cho bo render biet trang HIEN TAI la trang nao.
            // Thieu buoc nay thi m_currentPage mai la 0, va MOI anh chat luong day du cua
            // trang khac 0 deu bi vut o cua cuoi voi "drop reason=notCurrent" — day la ly do
            // "tao Text o trang 3 khong bao gio hien" (bat duoc trong log cua owner).
            if (t->renderer) t->renderer->setCurrentPage(page);
            t->fgnDeferred.clear();   // moi lan hien duoc hoan lop bu toi da 1 lan
            m_thumbPanel->setCurrentPage(page);
            statusBar()->showMessage(
                QString("Page %1 / %2").arg(page + 1).arg(t->doc->pageCount()), 2000);
            refreshAnnotVisuals(t, page);
            schedulePagePrefetch();
        }
    });
    connect(m_continuousView, &ContinuousView::zoomChanged,
            this, [this](double z) {
        if (auto* t = currentTab()) {
            t->zoom = z;
            if (m_zoomEdit)
                m_zoomEdit->setText(QString::number(qRound(z * 100)) + "%");
        }
    });

    // ── Chú thích của phần mềm khác: VÙNG SẮC NÉT theo zoom cho chế độ CUỘN (2026-09-21) ──
    // Single (PdfGpuView) đã có đường này qua `tilesNeeded → buildRegion` (§2.2.2). Continuous
    // KHÔNG có ⇒ lớp bù toàn trang dựng MỘT lần ở zoom nhỏ (maxPx=900/1103) rồi bị kéo giãn khi
    // zoom lên ⇒ chú thích đối tác NHÒE (owner báo). Nay nối vào CÙNG hàm buildRegion, chỉ khác
    // view nhận ảnh. `m_fgnRegionBuilding` nằm trong ContinuousView (chốt chống-chồng-việc): nó
    // chỉ phát một yêu cầu tại một thời điểm; `setForeignAnnotRegion` mở chốt ở MỌI nhánh.
    connect(m_continuousView, &ContinuousView::foreignAnnotRegionNeeded, this,
            [this](int page, double scale, QRect regionPx) {
        auto resetLatch = [this, page, scale, regionPx]() {
            if (m_continuousView)
                m_continuousView->setForeignAnnotRegion(page, scale, regionPx, QImage());
        };
        DocTab* t = currentTab();
        if (!m_fastMode || !t || !m_openDocs.contains(t) || !t->doc || !t->doc->isOpen()) {
            resetLatch(); return;
        }
        // Dùng ĐÚNG lớp mà ContinuousView đang vẽ cho trang này — không phải t->fgnLayer (bản
        // Single) vốn có thể đã trỏ sang trang khác sau khi cuộn.
        auto fl = m_continuousView->foreignAnnotLayer(page);
        if (!fl || !fl->isReady()) { resetLatch(); return; }
        FPDF_DOCUMENT d = t->doc->raw();
        auto* wr = new QFutureWatcher<bool>(this);
        connect(wr, &QFutureWatcher<bool>::finished, this, [this, wr, page, scale, regionPx, fl]{
            wr->deleteLater();
            if (!m_continuousView) return;
            m_continuousView->setForeignAnnotRegion(page, scale, regionPx,
                wr->result() ? fl->regionImage() : QImage());
        });
        // Ghi vào fgnRegionFuture của tab ⇒ cancelForeignAnnotTasks() chờ xong trước khi
        // giải phóng tài liệu (buildRegion cầm con trỏ FPDF_DOCUMENT).
        // 🔴 LƯỢT 22: token ĐĂNG KÝ LÚC SPAWN (UI thread) — beginClose thấy cả task
        // còn xếp hàng; RAII move vào lambda.
        trdoc::Task task(d, "fgnlayer/buildRegion");
        t->fgnRegionFuture = QtConcurrent::run([fl, d, page, scale, regionPx,
                                                task = std::move(task)]{
            return fl->buildRegion(d, page, scale, regionPx);
        });
        wr->setFuture(t->fgnRegionFuture);
        t->addBgWait(t->fgnRegionFuture);    // L22: chờ HẾT — L22b: addBgWait dọn mục đã xong
    });

    // ── Link (SPEC_PDF_LINKS): hover hien URI/Page N, click xu ly ──
    connect(m_continuousView, &ContinuousView::linkHovered,
            this, [this](const QString& txt) {
        statusBar()->showMessage(txt, 4000);
    });
    connect(m_continuousView, &ContinuousView::linkActivated,
            this, [this](int page, const PdfLink& link) {
        if (auto* t = currentTab()) onLinkActivated(t, page, link);
    });

    // Extract selected pages (multi-select from thumbnails or bookmarks)
    connect(m_thumbPanel, &ThumbnailPanel::extractPagesRequested,
            this, [this](QList<int> pageIndices) {
        auto* t = currentTab();
        if (!t || !t->doc->isOpen() || pageIndices.isEmpty()) return;
        QString out = QFileDialog::getSaveFileName(
            this, "Extract Pages to New File", {}, "PDF Files (*.pdf)");
        if (out.isEmpty()) return;
        if (!out.endsWith(".pdf", Qt::CaseInsensitive)) out += ".pdf";
        QApplication::setOverrideCursor(Qt::WaitCursor);
        PdfEditor* editor = m_editor.get();
        QString srcPath   = t->doc->filePath();
        auto* watcher = new QFutureWatcher<bool>(this);
        connect(watcher, &QFutureWatcher<bool>::finished, this,
                [this, watcher, out]() {
            watcher->deleteLater();
            QApplication::restoreOverrideCursor();
            if (watcher->result())
                openFile(out);
            else
                QMessageBox::warning(this, "Extract Error", m_editor->lastError());
        });
        watcher->setFuture(QtConcurrent::run([editor, srcPath, pageIndices, out]() -> bool {
            return editor->extractPageList(srcPath, pageIndices, out);
        }));
    });

    // Page drag-reorder
    connect(m_thumbPanel, &ThumbnailPanel::pagesReordered,
            this, [this](QList<int> newOrder) {
        auto* t = currentTab();
        if (!t || !t->doc->isOpen() || newOrder.size() != t->doc->pageCount()) return;
        QString path = t->doc->filePath();
        QString tmp  = makeTmpPath(path);
        int newCurrent = newOrder.indexOf(t->currentPage);
        if (newCurrent >= 0) t->currentPage = newCurrent;
        // 🔴🔴 2026-09-01: bao cho bo render biet trang HIEN TAI la trang nao.
        // Thieu buoc nay thi m_currentPage mai la 0, va MOI anh chat luong day du cua
        // trang khac 0 deu bi vut o cua cuoi voi "drop reason=notCurrent" — day la ly do
        // "tao Text o trang 3 khong bao gio hien" (bat duoc trong log cua owner).
        if (t->renderer && newCurrent >= 0) t->renderer->setCurrentPage(newCurrent);
        QApplication::setOverrideCursor(Qt::WaitCursor);
        PdfEditor* editor = m_editor.get();
        auto* watcher = new QFutureWatcher<bool>(this);
        connect(watcher, &QFutureWatcher<bool>::finished, this,
                [this, watcher, t, path, tmp, s = t->serial]() {
            watcher->deleteLater();
            QApplication::restoreOverrideCursor();
            if (!tabAlive(t, s)) return;   // 🔴 L20: tab đóng giữa lúc reorder — đừng chạm reloadTab
            if (watcher->result())
                reloadTab(t, path, tmp);
            else
                QMessageBox::warning(this, "Reorder Error", m_editor->lastError());
        });
        watcher->setFuture(QtConcurrent::run([editor, path, newOrder, tmp]() -> bool {
            return editor->reorderPages(path, newOrder, tmp);
        }));
    });

    // Bookmark drag-reorder
    connect(m_thumbPanel, &ThumbnailPanel::bookmarksReordered,
            this, [this](QList<int> newOrder) {
        auto* t = currentTab();
        if (!t || !t->doc->isOpen()) return;
        QString path = t->doc->filePath();
        QString tmp  = makeTmpPath(path);
        QApplication::setOverrideCursor(Qt::WaitCursor);
        PdfEditor* editor = m_editor.get();
        auto* watcher = new QFutureWatcher<bool>(this);
        connect(watcher, &QFutureWatcher<bool>::finished, this,
                [this, watcher, t, path, tmp, s = t->serial]() {
            watcher->deleteLater();
            QApplication::restoreOverrideCursor();
            if (!tabAlive(t, s)) return;   // 🔴 L20: tab đóng giữa lúc reorder — đừng chạm reloadTab
            if (watcher->result())
                reloadTab(t, path, tmp);
            else
                QMessageBox::warning(this, "Reorder Error", m_editor->lastError());
        });
        watcher->setFuture(QtConcurrent::run([editor, path, newOrder, tmp]() -> bool {
            return editor->reorderBookmarks(path, newOrder, tmp);
        }));
    });

    setWindowIcon(QIcon(":/icons/TorReader.ico"));
    setAcceptDrops(true);

    // Start in light mode; TORREADER_FORCE_THEME=dark|light ep theme cho viec
    // nghiem thu bang anh (khong phai bam chuot).
    const QByteArray forceTheme = qgetenv("TORREADER_FORCE_THEME");
    applyTheme(forceTheme == "dark");
    if (!forceTheme.isEmpty() && m_darkAct)
        m_darkAct->setChecked(forceTheme == "dark");

    // Permanent hint bar — shows shortcut hints on the right side of the status bar
    auto* hintLabel = new QLabel(
        "Ctrl+Scroll: Zoom  ·  Alt+Drag: Translate  "
        "·  Scroll: Flip page  ·  Right-click thumbnail: Page options");
    m_hintLabel = hintLabel;
    hintLabel->setStyleSheet(QStringLiteral("color:%1; font-size:10px; padding-right:8px;")
                                 .arg(m_darkMode ? darkHC().fgDim : lightHC().fgDim));
    statusBar()->addPermanentWidget(hintLabel);

    statusBar()->showMessage("TorReader PDF  ·  Open a PDF to get started");

    // ── Translation feature ───────────────────────────────────────────────────
    m_googleAuth = new GoogleAuth(this);
    m_translator = new Translator(this);
    m_transPopup = new TranslationPopup(nullptr); // top-level floating window

    connect(m_translator, &Translator::finished,
            this, [this](const QString& orig, const QString& trans) {
        m_transPopup->showTranslation(orig, trans, m_lastTransPos);
    });
    connect(m_translator, &Translator::failed,
            this, [this](const QString& err) {
        statusBar()->showMessage("Translation failed: " + err, 4000);
    });

    connect(m_continuousView, &ContinuousView::textRegionSelected,
            this, &MainWindow::onTextRegionSelected);

    // Markup pick/context/move (SPEC_CONTINUOUS_MARKUP_EDIT_2026-08-16): dung
    // chung handler voi PdfGpuView — cung AnnotationManager + undo path.
    connect(m_continuousView, &ContinuousView::annotationPickRequested,
            this, [this](int page, QPointF pt) {
        if (auto* t = currentTab()) onAnnotPick(t, page, pt);
    });
    connect(m_continuousView, &ContinuousView::annotationContextRequested,
            this, [this](int page, QPointF pt, QPoint gpos) {
        if (auto* t = currentTab()) onAnnotContext(t, page, pt, gpos);
    });
    connect(m_continuousView, &ContinuousView::annotationMoveRequested,
            this, [this](int page, double dx, double dy) {
        // LUOT 41 (30/09): Continuous chi-xem markup — lop phong thu thu hai.
        // ContinuousView khong con phat tin hieu nay (mousePressEvent da bo
        // nhanh khoi dong keo), nhung chan them o day phong khi con duong khac.
        if (probeContinuousVisible()) {
            qDebug().noquote() << "[continuous-readonly] chan move/resize";
            return;
        }
        if (auto* t = currentTab()) onAnnotMove(t, page, dx, dy);
    });
    // Insert Image (SPEC_INSERT_IMAGE_2026-08-30): co gian Stamp bang tay nam goc.
    // LUOT 41 (30/09): KHONG con hieu luc trong Continuous (chi con CHON).
    connect(m_continuousView, &ContinuousView::annotationResizeRequested,
            this, [this](int page, QRectF newRectDisp) {
        if (probeContinuousVisible()) {
            qDebug().noquote() << "[continuous-readonly] chan move/resize";
            return;
        }
        if (auto* t = currentTab()) onAnnotResize(t, page, newRectDisp);
    });

    connect(m_continuousView, &ContinuousView::textSelectionChanged,
            this, &MainWindow::onTextSelectionChanged);
    connect(m_continuousView, &ContinuousView::textSelectionCleared,
            this, &MainWindow::onTextSelectionCleared);

    connect(m_continuousView, &ContinuousView::pageContextRequested,
            this, [this](int page, QPoint gpos) {
        auto* t = currentTab();
        if (!t || !t->doc->isOpen()) return;
        QMenu menu(this);
        // Muc copy chu la muc DAU TIEN khi dang co vung chon (SPEC_TEXTSEL_ADOBE).
        if (t->textSel.active) {
            QAction* copyAct = menu.addAction("Copy text");
            copyAct->setShortcut(QKeySequence::Copy);
            connect(copyAct, &QAction::triggered, this, [this]{
                copyTextSelectionToClipboard();
            });
            menu.addSeparator();
        }
        // Chi kiem TRANG HIEN TAI (nap 1 trang, re). Muc "all pages" LUON bat:
        // runOcr tu bo qua trang da OCR, bam nham khi da xong cung vo hai.
        // Khong tinh truoc docNeedsOcr — duyet toan bo trang = treo tren file lon.
        const bool canPage = pageNeedsOcr(t->doc->raw(), page);
        QAction* pageAct = menu.addAction("Recognize text on this page");
        QAction* allAct  = menu.addAction("Recognize text on all pages");
        pageAct->setEnabled(canPage);
        if (!canPage) pageAct->setToolTip("Already recognized");
        QAction* chosen = menu.exec(gpos);
        if (chosen == pageAct) onOcrPageRequested(page);
        else if (chosen == allAct) onOcrAllRequested();
    });

    connect(m_continuousView, &ContinuousView::needAnnotVisuals, this, [this](int page) {
        auto* t = currentTab();
        if (!t || !t->annotMgr || !t->doc || !t->doc->isOpen()) return;
        if (t->visualsScanning.contains(page)) {
            // Rescan loadPageVisuals dang chay nen — apply-back se tu day visuals +
            // ensureForeignAnnotLayer. Chi lay lai lop bu neu trang bi DEFER lan 1.
            if (t == currentTab()) ensureForeignAnnotLayer(t, page);
            return;
        }
        if (t->visualsCache.contains(page)) {
            const QList<AnnotVisual> vis = t->visualsCache.value(page);
            m_continuousView->setAnnotVisualsForPage(page, vis);
            if (t == currentTab()) ensureForeignAnnotLayer(t, page);
            return;
        }
        // 🔴 VIỆC 1 (SPEC_SMOOTH_123 31/08): cache miss KHONG goi loadPageVisuals
        // DONG BO tren GUI (site da cho 58s khi khoa ban). Day viec that sang luong
        // nen: refreshAnnotVisuals chay QtConcurrent, nhan ket qua qua signal
        // (applyAnnotVisuals) roi cap nhat giao dien. Khong tra rong lam markup mat.
        refreshAnnotVisuals(t, page);
    });

#ifndef TORREADER_STORE_BUILD
    // ── Gate: update check (fires once after window is shown) ─────────────────
    m_updateChecker = new UpdateChecker(this);
    connect(m_updateChecker, &UpdateChecker::updateAvailable,
            this, [this](const QString& ver, const QString& title,
                         const QString& body, bool blocking) {
        qDebug().noquote()
            << "[gate] local=" << FELIXPDF_VERSION
            << "remote=" << ver
            << "blocking=" << (blocking ? 1 : 0);
        qDebug().noquote() << "[gate] showing dialog blocking=" << (blocking ? 1 : 0);
        GateDialog dlg(title, body, blocking, this);
        dlg.exec();
    });
    connect(m_updateChecker, &UpdateChecker::checkFailed,
            this, [this](const QString& reason) {
        qDebug().noquote() << "[gate] check failed — fail-open, app continues:" << reason;
    });
    connect(m_updateChecker, &UpdateChecker::upToDate,
            this, []() {
        qDebug().noquote() << "[gate] up to date — no dialog";
    });
    QTimer::singleShot(0, this, [this]() { m_updateChecker->checkForUpdates(); });
#else
    m_updateChecker = nullptr;
    qDebug().noquote() << "[gate] store build — update check disabled";
#endif

    // Settle timer: defers full-quality render until 400ms after last page change.
    // During fast scrolling, no renders start — avoids mutex contention.
    // When the user stops on a page, the timer fires and starts the render.
    // Caps total deferral at ~600ms so continuous scrolling doesn't starve rendering forever.
    m_settleTimer = new QTimer(this);
    m_settleTimer->setSingleShot(true);
    m_settleTimer->setInterval(400);
    connect(m_settleTimer, &QTimer::timeout, this, [this]() {
        auto* t = currentTab();
        if (!t || !t->doc->isOpen()) return;
        qint64 elapsed = m_settleStartMs > 0 ? QDateTime::currentMSecsSinceEpoch() - m_settleStartMs : 0;
        if (elapsed > 600) {
            qDebug() << "[Main] settle FORCED after" << elapsed << "ms of continuous scrolling page=" << t->currentPage;
        } else {
            qDebug() << "[Main] settle timeout — starting render for page" << t->currentPage;
        }
        m_settleStartMs = 0;
        if (!m_fastMode)
            t->renderer->requestPage(t->currentPage, t->zoom);
        statusBar()->showMessage(
            QString("Page %1 / %2 — Loading…").arg(t->currentPage + 1).arg(t->doc->pageCount()));
    });

    // Debounce OCR-status (SPEC_PERF_DESK_ABOUT phan 1.3): lat nhanh qua nhieu
    // trang chi land gia DINH mot lan o trang dung lai (250ms). Kiem that su
    // chay o QtConcurrent, khong block UI.
    m_ocrNotifyTimer = new QTimer(this);
    m_ocrNotifyTimer->setSingleShot(true);
    m_ocrNotifyTimer->setInterval(250);
    connect(m_ocrNotifyTimer, &QTimer::timeout, this, &MainWindow::onOcrNotifyTimeout);

    // Nav instant (SPEC_NAV_INSTANT_2026-08-16): placeholder len dau, việc nặng
    // (refreshAnnotVisuals + ensureForeignAnnotLayer) hoan 120ms dung chung. Moi
    // lan doi trang thi start() lai — lat nhanh chi lam viec nang cho trang dung.
    m_navDeferTimer = new QTimer(this);
    m_navDeferTimer->setSingleShot(true);
    m_navDeferTimer->setInterval(120);
    connect(m_navDeferTimer, &QTimer::timeout, this, &MainWindow::onNavDeferred);

    m_warmTimer = new QTimer(this);
    m_warmTimer->setSingleShot(true);
    m_warmTimer->setInterval(800);
    connect(m_warmTimer, &QTimer::timeout, this, [this]() {
        DocTab* t = currentTab();
        if (!t || !t->doc || !t->doc->isOpen() || !t->annotMgr) return;
        const int pg = t->currentPage;
        if (t->warmingPage == pg) return;
        if (t->vecBuilding.contains(pg)) { m_warmTimer->start(); return; }
        t->warmingPage = pg;
        auto* w = new QFutureWatcher<void>(this);
        connect(w, &QFutureWatcher<void>::finished, this, [this, w, t, pg, s = t->serial] {
            w->deleteLater();
            if (!tabAlive(t, s)) return;
            t->warmingPage = -1;
            if (t->currentPage != pg) return;
            QElapsedTimer _t; _t.start();
            refreshAnnotVisuals(t, pg);
            annotsForPage(t, pg);
            qDebug().noquote() << "[perf] markup WARM done page=" << pg << "ms=" << _t.elapsed();
        });
        FPDF_DOCUMENT d = t->doc ? t->doc->raw() : nullptr;
        // 🔴 0928 LƯỢT 22 (reviewer mục 2): token ĐĂNG KÝ LÚC SPAWN trên luồng UI,
        // RAII move vào lambda — markupWarm future KHÔNG lưu ở tab, trước đây
        // beginClose thấy 0 việc khi task còn xếp hàng ⇒ doc đóng, task chạy với
        // `d` đã free. Thân task chỉ chạm `d` (không đọc t->) ⇒ token là đủ.
        trdoc::Task task(d, "markupWarm");
        w->setFuture(QtConcurrent::run([d, pg, task = std::move(task)] {
            // Nap trang hien tai vao PageCache (luong nen) — markup overlay duoc am.
            TimedPdfiumLock lk(__FILE__, __LINE__);
            if (d) {
                PageCache::acquire(d, pg);
                PageCache::PageBorrow _b(d, pg);   // R1: cap doi acquire()
            }
        }));
    });

    // SPEC_PAGECACHE_CORE muc 4: khi trang on dinh 300 ms → prefetch trang lien ke
    // (trang truoc + sau). User lat di noi khac thi timer duoc start lai (huy prefetch cu).
    m_preloadTimer = new QTimer(this);
    m_preloadTimer->setSingleShot(true);
    m_preloadTimer->setInterval(300);
    connect(m_preloadTimer, &QTimer::timeout, this, [this]() {
        DocTab* t = currentTab();
        if (!t || !t->doc || !t->doc->isOpen()) return;
        const int pg = (m_fastMode && m_continuousView)
                           ? m_continuousView->currentPage() : t->currentPage;
        if (t->currentPage != pg) return;   // da lat di noi khac
        FPDF_DOCUMENT d = t->doc->raw();
        const int total = t->doc->pageCount();
        if (pg - 1 >= 0)        PageCache::prefetch(d, pg - 1);
        if (pg + 1 < total)     PageCache::prefetch(d, pg + 1);
    });

    connect(qApp, &QApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState st) {
        if (st != Qt::ApplicationActive) hideNotePopup();
    });

    // Probe-only (SPEC_OCR_TAB muc NGHIEM THU 4): TORREADER_PROBE_TOOL=10 bat
    // nut Select tren toolbar SAU khi mo file de log "[tool] set id=10 view=gpu|
    // continuous" — chung minh CA HAI view deu nhan cong cu. Binh thuong khong
    // dat bien nay nen khong anh huong gi.
    // TORREADER_PROBE_TOOL=seq (SPEC_FIX_PICK_TOOL muc NGHIEM THU 3): chay 3
    // kich ban chon cong cu de nghiem thu trang thai nut: Select toolbar
    // (id 10) → Pick luoi (id 0) → Rect (id 4). Khong dung chuot that — goi
    // dung luong dien tu khi bam nut, nen log "[tool] state" la bang chung.
    if (qEnvironmentVariableIsSet("TORREADER_PROBE_TOOL")) {
        const QString pt = QString::fromLocal8Bit(qgetenv("TORREADER_PROBE_TOOL"));
        QTimer::singleShot(3000, this, [this, pt] {
            qInfo().noquote() << "[toolprobe] tabs=" << m_openDocs.size()
                              << "cur=" << (m_docTabs ? m_docTabs->currentIndex() : -1);
            if (pt == QLatin1String("10")) {
                if (m_selectTextAct) m_selectTextAct->setChecked(true);
            } else if (pt == QLatin1String("seq") && m_thumbPanel) {
                if (m_selectTextAct) m_selectTextAct->setChecked(true);
                QTimer::singleShot(500, this, [this]{ m_thumbPanel->activateToolFromGrid(0); });
                QTimer::singleShot(1000, this, [this]{ m_thumbPanel->activateToolFromGrid(4); });
            }
        });
    }
    cleanupOrphanTorcache();
}
void MainWindow::cleanupOrphanTorcache() {
    // Startup cleanup: remove leftover .torcache files from temp directory
    QDir tempDir = QDir::temp();
    QStringList filters;
    filters << "*.torcache";
    tempDir.setNameFilters(filters);
    QFileInfoList fileInfoList = tempDir.entryInfoList(QDir::Files);

    qint64 totalSize = 0;
    int count = 0;
    for (const QFileInfo& fi : fileInfoList) {
        const QString filePath = fi.absoluteFilePath();
        const qint64 fileSize = fi.size();
        // 0927 M1: KHONG xoa thang nua. File .torcache con QLockFile banh sat
        // (`<cache>.lock`, giu suot phien) ⇒ chi xoa khi `tryLock(0)` THANH CONG,
        // tuc la khong con instance nao chu. Khong co khoa nay: instance 2 khoi
        // dong se xoa dem dang chay cua instance 1 (Linux unlink LUON thanh cong
        // nen khong the trong vao he dieu hanh).
        QString err;
        const auto r = TileCacheRegistry::removeIfUnlocked(filePath, &err);
        const bool removed = (r == TileCacheRegistry::RemoveResult::Removed
                           || r == TileCacheRegistry::RemoveResult::NotFound);
        const QString why = (r == TileCacheRegistry::RemoveResult::Locked) ? QStringLiteral("lockInstanceKhac")
                          : (r == TileCacheRegistry::RemoveResult::NotFound) ? QStringLiteral("khongCon")
                                                                             : err;
        // 0927: log ca THAT BAI (truoc day bo qua im lang — do do la lý do file
        // .torcache con sot ma khong ai thay). File con lai se duoc don o lan sau.
        qDebug().noquote() << QString("[torcache] XOA ok=%1 %2 ly do=moCoi loi=%3")
                                 .arg(removed ? 1 : 0).arg(filePath).arg(why);
        if (removed) { totalSize += fileSize; count++; }
    }
    if (count > 0) {
        double totalSizeMB = static_cast<double>(totalSize) / (1024 * 1024);
        qDebug().noquote() << QString("[torcache] Startup cleanup: removed %1 files (%2 MB)").arg(count).arg(totalSizeMB, 0, 'f', 2);
    }

    // 0927 LƯỢT 6: don `.torvec` / `.torvec.tmp` mồ côi. App chết giữa chừng
    // (crash, kill) thì vong doi trong phiên bi cat ⇒ file con lai %TEMP%. CHI
    // don khi `.lock` cua `.torcache` cung key khong con instance sống giữ
    // (removeIfUnlocked tra Locked = con nguoi dung). Cac ban ghi tam cua task
    // chua xong cua app vua chet cung di (`.torvec.tmp`).
    const QStringList vecNames = tempDir.entryList(
        QStringList() << "*.torvec" << "*.torvec.tmp", QDir::Files);
    QSet<QString> doneKeys;
    for (const QString& name : vecNames) {
        const QString key = VectorCache::keyFromVecFileName(name);
        if (key.isEmpty() || doneKeys.contains(key)) continue;
        doneKeys.insert(key);
        // `.torcache` cua cung key — neu no con bi instance khac giu thi CHUA
        // duoc don .torvec (tai lieu do dang chay).
        QString err;
        const auto r = TileCacheRegistry::removeIfUnlocked(tempDir.filePath(key + ".torcache"), &err);
        if (r == TileCacheRegistry::RemoveResult::Locked) {
            qDebug().noquote() << QString("[torvec] XOA ok=0 %1 ly do=moCoi loi=lockInstanceKhac")
                                     .arg(tempDir.filePath(key + "_p*.torvec"));
            continue;
        }
        VectorCache::purgeKey(key, "moCoi");
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// 0927 LƯỢT 8 — ĐƯỜNG THOÁT DUY NHẤT CHO MỘT TAB
//
// Ba đường trước đây tự chế thứ tự riêng và lệch nhau (thoát app / đóng tab /
// lưu-rồi-nạp-lại) ⇒ đó là lý do "bản GỐC không crash, bản có sửa thì crash":
//   • ~MainWindow KHÔNG gọi cancelPending()  → task render chạy tiếp 14 s TRONG khi
//     luồng UI đã bắt đầu phá tài liệu. crash8_quaivat.log: #000032 FORGET-DOC lúc
//     00:24:11.028 rồi #000035 RENDER-CLOSE + #000036 PAGE-CLOSE (tid=1ef8) lúc
//     00:24:11.716 — chết trong FPDF_ClosePage của task nền, không có PAGE-CLOSE-XONG.
//   • ~MainWindow KHÔNG đóng `heavyPrivPage` (FPDF_LoadPage thô ở :2448, chỉ
//     closeHeavyPriv() đóng, mà hàm đó không có caller nào ở đường thoát).
//   • ~MainWindow KHÔNG xoá `t->view` (con của m_docTabs) và `m_continuousView` —
//     chúng bị Qt huỷ SAU ~MainWindow, tức SAU khi ~PdfDocument đã FPDF_CloseDocument
//     + unmap. ~ContinuousView lại còn `m_vecPool.waitForDone()` vô hạn (ContinuousView.cpp:317).
//
// THỨ TỰ (đúng thứ tự này, đổi là hỏng):
//   ① ngắt watcher + dừng worker .torcache
//   ② dừng + CHỜ quét annot (stopScan làm hết trong ~1 trang)
//   ③ huỷ lớp bu ngoài / vùng nặng  (đã có sẵn, chờ future)
//   ④ 🔴 HUỶ RENDER + CHỜ pool CÓ HẠN  ← bản sửa hồi quy quaivat
//   ⑤ đóng handle riêng của tab: heavyPrivPage, TextSelection
// (sau đó: `delete t` ⇒ ~ThumbnailRenderPool → ~AnnotationManager → ~PdfRenderer
//  (đóng 12 doc pool) → ~PdfDocument (đóng trang PageCache, FPDF_CloseDocument, unmap);
//  hoặc với `loadTabFile` thì `setDocument()` mở lại pool trên tài liệu mới)
//
// KHÔNG đụng `m_openDocs` ở đây — tab còn sống sau khi gọi (đường lưu-rồi-nạp-lại).
// Hai đường xoá tab (đóng tab, thoát app) tự gỡ khỏi `m_openDocs`.
// ═══════════════════════════════════════════════════════════════════════════════
void MainWindow::shutdownTab(DocTab* t, const char* why, int renderWaitMs, bool pdfiumTailOnUi, bool waitBgOnUi) {
    if (!t) return;
    disconnect(t->pageReadyConn);
    disconnect(t->scrollConn);
    // 🔴🔴 0928 LƯỢT 14 — BƯỚC 0: TĂNG THẾ HỆ HUỶ TRƯỚC KHI CHỜ BẤT CỨ THỨ GÌ.
    // Đây là đường CHUNG cho thoát app (why="thoatApp"), đóng tab ("dongTab") và
    // nạp lại ("naiLai") — cả ba đều đi qua đây.
    //   (a) `t->vecGen` tăng ⇒ các `VectorLayer::build` của tab dừng ở ranh giới
    //       lát kế tiếp (≤ 40 ms, xem kVecSliceMs trong VectorLayer.cpp).
    //   (b) `trdoc::cancelAll` bật cờ chung ⇒ MỌI task nền còn lại (có/không vết
    //       huỷ riêng) cũng biết là tài liệu sắp đóng.
    // Trước đây bước này KHÔNG có: mọi task nền cứ chạy tiếp tới khi
    // `FPDF_CloseDocument` xoá sạch vùng nhớ dưới chân nó ⇒ 4/4 minidump LƯỢT 13
    // chết ở `VectorLayer::build` trên luồng QtConcurrent.
    // 🔴 0928 LƯỢT 26 (VIỆC 2, DO): [dongtab] in thoi gian MOI pha BLOCK tren UI —
    // r25 freeze step-A/I KHONG phai waitIdle (0 lan WAIT-POOL-HO); thu phạm là pha
    // nao thi do sang closeJob nen, khong doan. 1 dong/pha, chi no khi dong tab.
    QElapsedTimer _swT; _swT.start(); qint64 _swPrev = 0;
    auto _phase = [why, &_swT, &_swPrev](const char* mark) {
        const qint64 el = _swT.elapsed();
        qDebug().noquote() << "[dongtab]" << why << mark << "ms=" << (el - _swPrev);
        _swPrev = el;
    };
    t->vecGen->fetch_add(1, std::memory_order_acq_rel);
    if (t->doc) trdoc::cancelAll(t->doc->raw());
    stopThumbPool(t);          // 0903: worker ghi nền .torcache phai het truoc khi UI cham PDFium
    PdfLinks::clearCache();
    _phase("huy+stopThumb");

    if (t->annotMgr) t->annotMgr->stopScan();
    PdfCloseTrace::note("WAIT-ANNOT",
        QStringLiteral("tab=%1 stopScan xong — cho annotScan/annotVisuals/annotPage ve het")
            .arg(why));
    // LƯỢT 37 (mục C): annotVisualsFuture = loadPageVisuals trang QUÁI VẬT, giữ khoá
    // pdfium ~1,7 s (FPDF_LoadPage 2,18M object). Ba future NÀY cũng nằm trong bgWaits ⇒
    // đường đóng tab (waitBgOnUi=false) KHÔNG chờ trên UI; closeJob chờ ở nền. quit/reload
    // vẫn chờ (đúng thứ tự cũ).
    if (waitBgOnUi) {
        if (t->annotScanFuture.isValid())    t->annotScanFuture.waitForFinished();
        if (t->annotVisualsFuture.isValid()) t->annotVisualsFuture.waitForFinished();
        if (t->annotPageFuture.isValid())    t->annotPageFuture.waitForFinished();
    }
    _phase("cho-annot");
    // 🔴🔴 0928 LƯỢT 22 (reviewer mục 3): các SLOT future (annotVisualsFuture,
    // annotPageFuture, fgnFuture, fgnRegionFuture, heavyRegionFuture) bị ghi đè
    // theo trang — waitForFinished ở trên chỉ chờ CÁI CUỐI. bgSync là DANH SÁCH:
    // MỌI QtConcurrent::run của tab (fgn/visuals/page/scan/region/heavy/translate)
    // đã addFuture — chờ HẾT rồi mới tới `delete t`.
    // KHÔNG gom vec build/OCR vào bgSync: chúng không đọc t-> (token + shared_ptr
    // vecGen che), chờ build CAD 14 s / OCR phút trên UI là đứng hình.
    // THỨ TỰ: HỦY TRƯỚC — CHỜ SAU. ForeignAnnotLayer::build chỉ kiểm m_cancel TRƯỚC
    // khi lấy khoá (không cắt giữa được, CAD 14 s) ⇒ phải gọi cancel của
    // cancelForeignAnnotTasks NGAY Ở ĐÂY, nếu không bgSync.waitForFinished chờ
    // nguyên trang quái vật thay vì cắt lát (heavyRegion) — đứng hình UI.
    if (t->fgnPending) t->fgnPending->cancel();
    if (t->fgnLayer) t->fgnLayer->cancel();
    if (t->heavyRegionCancel) t->heavyRegionCancel->storeRelease(1);
    // LƯỢT 37 (mục C): ĐÓNG TAB không chờ HẾT danh sách future trên UI nữa — đo r37
    // [BLOCK] probeCloseTab=2191ms, trong đó cho-bgsync=1684ms (loadPageVisuals trang
    // QUÁI VẬT 2,18M object giữ khoá pdfium đang FPDF_LoadPage). Các task này CHỈ đọc
    // doc/annotMgr (sống tới `delete t`), KHÔNG đụng t->view (đã xoá trên UI trước đó),
    // và lambda watcher đã chốt m_openDocs.contains(tab). ⇒ caller (onTabClose) dời
    // đúng vòng chờ này xuống closeJob NỀN, trước shutdownHeavy + doc->close. quit/reload
    // vẫn chờ trên UI (waitBgOnUi=true).
    if (waitBgOnUi) {
        for (auto& w : t->bgWaits) w.wait();   // 🔴 L22: chờ HẾT danh sách future của tab
    }
    _phase("cho-bgsync");
    PdfCloseTrace::note("WAIT-BGSYNC", QStringLiteral("tab=%1 — het danh sach future nen").arg(why));
    PdfCloseTrace::note("WAIT-ANNOT-DONE", QStringLiteral("tab=%1 — moi dung t->doc").arg(why));

    cancelForeignAnnotTasks(t);   // lop bu dang build + buildRegion trang nang, cho xong het
    _phase("cho-fgn");

    // ④ 🔴 ĐÚNG THỨ TỰ: HUỶ TRƯỚC, CHỜ SAU. Task render kiểm tra generation ở đầu
    // mỗi slice (~50 ms) nên cancelPending() cắt được cả trang CAD 14 s. Trước đây
    // ~MainWindow bỏ qua bước này: bump generation chỉ xảy ra trong ~PdfRenderer, tức
    // SAU ~AnnotationManager đã đóng trang của doc chính.
    if (t->renderer) {
        t->renderer->cancelPending();
        // 🔴 0928 LƯỢT 26 (VIỆC 2): đóng tab ("dongTab") chỉ chờ 300 ms trên UI.
        // ĐO r25: freeze 3549ms step-A lap2 KHÔNG phải waitIdle (0 lần WAIT-POOL-HO
        // trong cả 2 run) — mà là UI chặn xin s_pdfiumMutex tại AnnotationManager
        // :1175 (2615ms) + :1569 (665ms) và fallback PdfRenderer.cpp:1300. Trần ở
        // đây vẫn hạ xuống 300 để UI không BAO GIỜ chờ render quá mức đó (chỉ thị
        // L26). Chờ còn lại ĐÃ CÓ dưới nền: closeJob `delete t` → ~PdfRenderer::
        // waitIdle(kPoolWaitMs) + PdfDocument::close → beginClose theo token; quá
        // trần UI thì task chạy nốt rồi tự trả handle/page — lưới an toàn không đổi.
        t->renderer->waitIdle(renderWaitMs);
    }
    _phase("cho-render");

    // ⑤ heavyPrivPage: FPDF_LoadPage thô ngoài PageCache, chỉ closeHeavyPriv() đóng,
    // mà hàm đó chưa bao giờ được gọi ở đường thoát ⇒ rò handle trang đang mở tới
    // lúc FPDF_CloseDocument. Cần cancelForeignAnnotTasks() xong trước (nó chờ
    // heavyRegionFuture — tác vụ duy nhất đọc handle này).
    // 🔴 0928 LƯỢT 26 (VIỆC 2): đường "dongTab" (pdfiumTailOnUi=false) dời hai lệnh
    // này xuống closeJob nền — đo r26: pha này chặn UI tới 576 ms chờ s_pdfiumMutex
    // sau lát vẽ nền. cancelForeignAnnotTasks đã chạy xong TRÊN UI trước khi closeJob
    // được xếp hàng ⇒ thứ tự "huỷ-trước-chờ-sau, đóng-trước ~PdfDocument" giữ nguyên.
    if (pdfiumTailOnUi) {
        closeHeavyPriv(t);
        if (t->doc) TextSelection::closeDocument(t->doc->raw());
    }
    _phase("dong-heavy+text");
}

MainWindow::~MainWindow() {
    // 0927 LƯỢT 8: thứ tự teardown DUY NHẤT — xem shutdownTab() để đọc lý do.
    // ⓪ `m_continuousView` là con của centralWidget: ~ContinuousView có
    //    `m_vecPool.waitForDone()` VÔ HẠN và các task đó đọc FPDF_DOCUMENT qua
    //    `m_doc`. Để Qt tự huỷ nó thì nó chết SAU ~PdfDocument (đã CloseDocument +
    //    unmap). Xoá ở đây = nó chết khi mọi tài liệu còn sống.
    delete m_continuousView;
    m_continuousView = nullptr;

    const auto docs = m_openDocs;
    for (auto* t : docs) {
        if (!t) continue;
        shutdownTab(t, "thoatApp");
        m_openDocs.removeAll(t);  // watcher con treo se thay "tab da chet" va bo qua
        delete t->view;    // con của m_docTabs — xoá ở đây để nó chết TRƯỚC tài liệu
        t->view = nullptr;
        // 0927 LƯỢT 6: bản nháp `.tortmp` trong %TEMP% (mở ở `onTabClose` khi
        // user đóng tab, KHÔNG có ở đường thoát app) ⇒ app tắt cửa sổ là để
        // lại file PDF trong %TEMP%. `delete t` đã đóng ~PdfDocument nên handle
        // trên file đã nhả ⇒ xoá được (trên Windows xoá file còn handle là
        // ERROR_SHARING_VIOLATION). Đúng luật app: user trả lời "không lưu" ở
        // hộp thoát = bỏ bản nháp, y như `onTabClose` vốn vậy.
         // 🔴 0928 LƯỢT 21: thoát app lúc open() còn chạy ⇒ cùng crash dump 32832.
         // Chờ ở đây là chính đáng (app đang tắt, future chỉ còn vài giây).
         if (t->openFuture.isValid()) t->openFuture.waitForFinished();
         const QString working = (t->doc->filePath() != t->originalPath) ? t->doc->filePath()
                                                                        : QString();
        delete t;
        if (!working.isEmpty() && !removeWorkingCopy(working))
            qWarning().noquote() << "[safety] ban nhap con lai:" << working;
    }
    m_openDocs.clear();
}

// ── Theme ────────────────────────────────────────────────────────────────────

void MainWindow::probeSetZoomSingle(double z) {
    if (auto* t = currentTab()) { t->zoom = z; if (t->view) t->view->setZoom(z); }
}

void MainWindow::probeSetZoom(double z) {
    if (m_continuousView) m_continuousView->setZoom(z);
}

QImage MainWindow::probeGrabView() {
    if (m_fastMode && m_continuousView) {
        // LUOT 38: ContinuousView ve bang QPainter trong paintEvent (khong phai
        // paintGL) nen QOpenGLWidget::grabFramebuffer() tra FBO tho = DEN.
        // QWidget::grab() moi ghep duoc noi dung dang thay tren man hinh.
        return m_continuousView->viewport()->grab().toImage();
    }
    if (auto* t = currentTab()) {
        if (auto* gl = qobject_cast<QOpenGLWidget*>(t->view))
            return gl->grabFramebuffer();
        if (t->view) return t->view->grab().toImage();
    }
    return QImage();
}

bool MainWindow::probeCreateNote(int page, double xPt, double yPt) {
    auto* tb = currentTab();
    if (!tb || !tb->annotMgr) return false;
    return tb->annotMgr->createPopupNote(page, QPointF(xPt, yPt), "probe note", "probe");
}

bool MainWindow::probeCreateText(int page, double xPt, double yPt, double wPt, double hPt) {
    auto* tb = currentTab();
    if (!tb || !tb->annotMgr) return false;
    return tb->annotMgr->createInlineNote(page, QRectF(xPt, yPt, wPt, hPt),
                                          "PROBE TEXT", "probe");
}

void MainWindow::probeScrollToPage(int p) {
    if (m_continuousView) m_continuousView->scrollToPage(p, "probeScrollToPage");
}

// Probe-only (--owner3tab-probe LƯỢT 17): chi-doc, khong doi trang thai gi.
int MainWindow::probeTabCount() const {
    return m_docTabs ? m_docTabs->count() : 0;
}
// LƯỢT 37 muc B: man chao la man chao khi ContinuousView BI AN (applyWelcomeVisibility).
bool MainWindow::probeContinuousVisible() const {
    return m_continuousView && m_continuousView->isVisible();
}
bool MainWindow::probePageHasImage(int p) const {
    return m_continuousView && m_continuousView->hasPageImage(p);
}
// 🔴 LƯỢT 31b (probe zoomsharp): scale THAT cua anh trang p trong ContinuousView.
double MainWindow::probePageImageScale(int p) const {
    return m_continuousView ? m_continuousView->pageImageScale(p) : -1.0;
}
// 🔴 LƯỢT 37 (bước O): bằng chứng "thu nhỏ nền đang bay" + "guard đã bỏ bao nhiêu bản cũ".
int MainWindow::probeContDownscaleInFlight() const {
    return m_continuousView ? m_continuousView->probeDownscaleInFlight() : 0;
}
qint64 MainWindow::probeContStaleDrops() const {
    return m_continuousView ? m_continuousView->probeStaleDrops() : 0;
}
// 🔴 LƯỢT 33a (bai do J/K/L): trang view Single cua tab dang THUC VE (m_pageIndex
// cua PdfGpuView — khong phai gia tri tab tu LUU), va tam viewport cua Continuous.
int MainWindow::probeTabShownPage(int idx) const {
    auto* t = m_openDocs.value(idx);
    return (t && t->view) ? t->view->currentPage() : -1;
}
bool MainWindow::probeContCenter(int* page, QPointF* c) const {
    return m_continuousView && m_continuousView->probeViewportCenter(page, c);
}
// 🔴 LƯỢT 33f (muc 5): offset cuon THAT cua Continuous — tab dang load dung o DINH
// thi scrollY()==0; khong dung "trang o tam viewport" (trang thap hon khung nhin
// thi tam roi trang ke tiep du view dung dinh — L f2 "page=2" ban 33e/f dau).
int MainWindow::probeContScrollY() const {
    return m_continuousView ? m_continuousView->scrollY() : -1;
}
// 🔴 LƯỢT 33e (muc 4): so dong comment THUC co trong panel (CHI-DOC).
int MainWindow::probePanelCommentRows() const {
    return m_thumbPanel ? m_thumbPanel->probeCommentRowCount() : -1;
}
// 🔴 LƯỢT 22b (reviewer mục 1): đếm chú thích bằng API THẬT — loadPage đọc thẳng
// tài liệu qua AnnotationManager, không đi qua các lambda scan có `tabAlive`. Đây
// là chuẩn để phân biệt "file không có annot" (0 hợp lệ) với "panel báo 0 dù file
// có annot" (thiếu `!` ⇒ FAIL).
int MainWindow::probeRealAnnotCount(int idx) {
    auto* t = m_openDocs.value(idx);
    if (!t || !t->annotMgr || !t->doc || !t->doc->isOpen()) return -1;
    int n = 0;
    for (int p = 0, e = t->doc->pageCount(); p < e; ++p)
        n += int(t->annotMgr->loadPage(p).size());
    return n;
}

QString MainWindow::probeZoomAnchor(int page, double zFrom, double zTo, double fx, double fy) {
    if (!m_continuousView) return QStringLiteral("ZOOMANCHOR_SKIP no continuous view\n");
    return m_continuousView->probeZoomAnchor(page, zFrom, zTo, fx, fy);
}

void MainWindow::applyTheme(bool dark) {
    m_darkMode = dark;
    const ThemeTokens& t = dark ? darkHC() : lightHC();
    qApp->setStyleSheet(buildQss(t));
    if (m_hintLabel)
        m_hintLabel->setStyleSheet(
            QStringLiteral("color:%1; font-size:10px; padding-right:8px;").arg(t.fgDim));
    if (m_zoomEdit)
        m_zoomEdit->setStyleSheet(
            QStringLiteral("QLineEdit { background:%1; color:%2; border:1px solid %3; "
                           "padding:1px 4px; font-size:11px; }")
                .arg(t.bg, t.fg, t.border));
    for (auto* t : m_openDocs)
        if (t->view) t->view->setDarkMode(dark);
    // LƯỢT 35: tab "Welcome" (PdfView) KHONG nam trong m_openDocs → phai cap nhat
    // rieng, neu khong man chao van o sang khi Dark Mode bat (owner test 4 ca).
    if (m_docTabs)
        for (int i = 0; i < m_docTabs->count(); ++i)
            if (auto* pv = qobject_cast<PdfView*>(m_docTabs->widget(i))) pv->setDarkMode(dark);
    if (m_continuousView) m_continuousView->setDarkMode(dark);
    if (m_thumbPanel) m_thumbPanel->setDarkMode(dark);
}

// ── Action bar ───────────────────────────────────────────────────────────────

void MainWindow::setupActionBar() {
    auto* tb = addToolBar("Actions");
    tb->setMovable(false);
    tb->setFloatable(false);
    tb->setIconSize({20, 20});
    tb->setToolButtonStyle(Qt::ToolButtonTextOnly);

    // File
    auto* openAct = tb->addAction("Open",    this, &MainWindow::onOpenFile);
    openAct->setShortcut(QKeySequence::Open);
    openAct->setShortcutContext(Qt::ApplicationShortcut);
    auto* saveAct = tb->addAction("Save",    this, &MainWindow::onSaveFile);
    saveAct->setShortcut(QKeySequence::Save);
    saveAct->setShortcutContext(Qt::ApplicationShortcut);
    auto* saveAsAct = tb->addAction("Save As", this, &MainWindow::onSaveAsFile);
    saveAsAct->setShortcut(QKeySequence::SaveAs);
    saveAsAct->setShortcutContext(Qt::ApplicationShortcut);
    tb->addSeparator();

    // Undo / Redo markup
    m_undoAct = tb->addAction(QString::fromUtf8("\xE2\x86\xB6"), this, &MainWindow::doUndo);
    m_undoAct->setToolTip("Undo markup (Ctrl+Z)");
    m_undoAct->setShortcut(QKeySequence::Undo);
    m_undoAct->setShortcutContext(Qt::ApplicationShortcut);
    m_redoAct = tb->addAction(QString::fromUtf8("\xE2\x86\xB7"), this, &MainWindow::doRedo);
    m_redoAct->setToolTip("Redo markup (Ctrl+Y)");
    QList<QKeySequence> redoKeys{ QKeySequence(QKeySequence::Redo) };
    for (const QKeySequence& k : { QKeySequence("Ctrl+Y"), QKeySequence("Ctrl+Shift+Z") })
        if (!redoKeys.contains(k)) redoKeys.append(k);
    m_redoAct->setShortcuts(redoKeys);
    m_redoAct->setShortcutContext(Qt::ApplicationShortcut);
    m_undoAct->setEnabled(false);
    m_redoAct->setEnabled(false);
    if (QWidget* w = tb->widgetForAction(m_undoAct)) {
        QFont f = w->font(); f.setPointSizeF(f.pointSizeF() + 5.0); f.setBold(true); w->setFont(f);
    }
    if (QWidget* w = tb->widgetForAction(m_redoAct)) {
        QFont f = w->font(); f.setPointSizeF(f.pointSizeF() + 5.0); f.setBold(true); w->setFont(f);
    }
    tb->addSeparator();

    // Edit
    auto* mergeAct = tb->addAction("Merge PDFs", this, &MainWindow::onMergeFiles);
    mergeAct->setShortcut(QKeySequence("Ctrl+M"));
    mergeAct->setShortcutContext(Qt::ApplicationShortcut);
    auto* extractAct = tb->addAction("Extract All", this, &MainWindow::onExtractAll);
    extractAct->setShortcut(QKeySequence("Ctrl+Shift+E"));
    extractAct->setShortcutContext(Qt::ApplicationShortcut);
    tb->addSeparator();

    // Sign
    auto* signAct = tb->addAction("Sign PDF…", this, &MainWindow::onSignPdf);
    signAct->setToolTip("Digitally sign this document with a certificate (.pfx/.p12)");
    m_finalizeSigAct = tb->addAction("✔ Finalize Signature", this, &MainWindow::onFinalizeSignature);
    m_cancelSigAct   = tb->addAction("✖ Cancel Signature",   this, &MainWindow::onCancelSignature);
    m_finalizeSigAct->setVisible(false);
    m_cancelSigAct->setVisible(false);
    tb->addSeparator();

    // Print
    auto* printAct = tb->addAction("Print", this, &MainWindow::onPrintFile);
    printAct->setShortcut(QKeySequence::Print);
    printAct->setShortcutContext(Qt::ApplicationShortcut);
    tb->addSeparator();
// View mode actions
     QActionGroup* viewModeGroup = new QActionGroup(this);
     viewModeGroup->setExclusive(true);
     m_viewQualityAct = viewModeGroup->addAction("View Quality && Edit Comments");   // 0903 owner: ro nghia hon
     m_viewQualityAct->setCheckable(true);
     m_viewQualityAct->setToolTip("High Quality Single Page and Editable");
     m_viewFastAct = viewModeGroup->addAction("View Fast");
     m_viewFastAct->setCheckable(true);
     m_viewFastAct->setToolTip("Continuous scroll — all pages in one strip  (Ctrl+Shift+C)");
     m_viewFastAct->setShortcut(QKeySequence("Ctrl+Shift+C"));
     // 🔴 2026-08-31 owner chot: MAC DINH la View Fast (Continuous). Quality chi de xem net 1 to.
     // Truoc day 3 cho mac dinh MAU THUAN nhau: nut sang Quality, MainWindow::m_fastMode=false,
     // nhung ContinuousView::m_fastMode=true => co luc app dang o Continuous ma nut lai la Quality.
     m_viewFastAct->setChecked(true);
     connect(viewModeGroup, &QActionGroup::triggered, this, [this](QAction *act) {
         if (act == m_viewQualityAct) {
             setViewMode(false); // false for Quality (single page)
         } else if (act == m_viewFastAct) {
             setViewMode(true); // true for Fast (continuous)
         }
     });
     tb->addAction(m_viewQualityAct);
     tb->addAction(m_viewFastAct);
     tb->addSeparator();

    // Fit Page — fits page to the smaller of width/height (width-only in Continuous mode)
tb->addAction("Fit Page", this, [this] {
         auto* t = currentTab();
         if (!t || !t->doc->isOpen()) return;
         auto sz = t->doc->pageSize(t->currentPage);
         if (sz.isEmpty()) return;
         if (m_fastMode && m_continuousView) {
             double z = (m_continuousView->viewport()->width() - 40.0) / sz.width();
             m_continuousView->setZoom(qBound(0.1, z, 10.0));
         } else if (t->view) {
            onZoomChanged(qMin(static_cast<double>(t->view->width())  / sz.width(),
                               static_cast<double>(t->view->height()) / sz.height()));
            t->view->centerPage();
        }
    })->setShortcut(QKeySequence("Ctrl+Shift+F"));
    tb->addSeparator();

    // Zoom − [%] +
    auto* zoomOutBtn = new QToolButton;
    zoomOutBtn->setText("−");
    zoomOutBtn->setToolTip("Zoom out  (Ctrl+−)");
    zoomOutBtn->setShortcut(QKeySequence::ZoomOut);
    tb->addWidget(zoomOutBtn);

    m_zoomEdit = new QLineEdit("100%");
    m_zoomEdit->setFixedWidth(54);
    m_zoomEdit->setAlignment(Qt::AlignCenter);
    m_zoomEdit->setToolTip("Zoom level — type a value and press Enter  (e.g. 150%)");
    // Style theo theme (set lai trong applyTheme).
    tb->addWidget(m_zoomEdit);

    auto* zoomInBtn = new QToolButton;
    zoomInBtn->setText("+");
    zoomInBtn->setToolTip("Zoom in  (Ctrl+=)");
    zoomInBtn->setShortcut(QKeySequence::ZoomIn);
    tb->addWidget(zoomInBtn);

    connect(zoomOutBtn, &QToolButton::clicked, this, [this] {
        if (auto* t = currentTab()) onZoomChanged(t->zoom - 0.15);
    });
    connect(zoomInBtn,  &QToolButton::clicked, this, [this] {
        if (auto* t = currentTab()) onZoomChanged(t->zoom + 0.15);
    });
    connect(m_zoomEdit, &QLineEdit::editingFinished, this, [this] {
        QString txt = m_zoomEdit->text().remove('%').trimmed();
        bool ok;
        double pct = txt.toDouble(&ok);
        if (ok && pct >= 10.0 && pct <= 1000.0) onZoomChanged(pct / 100.0);
    });
    tb->addSeparator();

    // Select — nut chon chu tren TOOLBAR CHINH (dong bo 2 chieu voi nut
    // "Select" id 10 trong sidebar, SPEC_TEXT_UX_SELECT_COPY muc 1).
    // Dat CANH cum dieu huong (zoom/Continuous/Fit Page), KHONG de cuoi toolbar
    // vi de bi tran vao menu overflow, nguoi dung khong nhin thay.
    m_selectTextAct = new QAction("Select", this);
    m_selectTextAct->setCheckable(true);
    m_selectTextAct->setToolTip("Select text (drag to select, Ctrl+C to copy)");
    m_selectTextAct->setShortcut(QKeySequence("Ctrl+Shift+S"));
    m_selectTextAct->setShortcutContext(Qt::ApplicationShortcut);
    connect(m_selectTextAct, &QAction::toggled, this, [this](bool on) {
        pushToolToViews(on ? PdfGpuView::ViewTool::SelectText : PdfGpuView::ViewTool::Pan,
                        on ? 10 : 0);
    });
    tb->addAction(m_selectTextAct);
    // Ten de bo do giao dien (UiProbe) tim duoc nut nay (chi doc, khong co quy
    // tac QSS nao dung ten nay nen khong doi ve ngoai) — cung kieu m_toolButtons.
    if (QWidget* w = tb->widgetForAction(m_selectTextAct))
        w->setObjectName(QStringLiteral("actionSelectText"));
    tb->addSeparator();

    // Dark mode
    auto* darkAct = tb->addAction("Dark Mode");
    darkAct->setCheckable(true);
    m_darkAct = darkAct;
    connect(darkAct, &QAction::toggled, this, &MainWindow::applyTheme);
    tb->addSeparator();

    // About
    tb->addAction("About", this, [this] {
        AboutDialog dlg(m_darkMode, this);
        dlg.exec();
    });
    tb->addSeparator();

    // Translate — nut tren toolbar (giua About va Share app).
    m_translateAct = new QAction("Translate", this);
    m_translateAct->setToolTip(
        "Enable Google Translate — hold Ctrl and drag over text to select & translate\n"
        "Works in both Single and Continuous modes\n"
        "Right-click to reset consent");
    m_translateAct->setShortcut(QKeySequence("Ctrl+Shift+T"));
    connect(m_translateAct, &QAction::triggered, this, [this]() {
        if (GoogleAuth::checkAndRequest(this)) {
            QMessageBox mb(this);
            mb.setWindowTitle("Google Translate Enabled");
            mb.setText(
                "<b>Translation is enabled.</b><br><br>"
                "Hold <b>Ctrl</b> and drag over text to select it.<br>"
                "Works in both <b>Single page</b> and <b>Continuous</b> modes.<br>"
                "Release to automatically translate the selected text to Vietnamese.<br><br>"
                "<i>To disable/reset: right-click the Translate button.</i>");
            mb.setIcon(QMessageBox::Information);
            mb.setStandardButtons(QMessageBox::Ok);
            mb.setWindowModality(Qt::ApplicationModal);
            mb.exec();
        }
    });
    tb->addAction(m_translateAct);

    // Chuot phai nut Translate → Reset Translation Consent (vi tri cu).
    if (auto* translateBtn = qobject_cast<QToolButton*>(tb->widgetForAction(m_translateAct))) {
        translateBtn->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(translateBtn, &QToolButton::customContextMenuRequested,
                this, [this](const QPoint& pos) {
            QMenu m(this);
            m.addAction(
                "\xE2\x9A\xA0 Reset Translation Consent",
                [this]() {
                    GoogleAuth::resetConsent();
                    QMessageBox::information(
                        this, "Translation Reset",
                        "Translation consent has been reset.\n"
                        "Click Translate again to re-enable.");
                });
            m.exec(qobject_cast<QWidget*>(sender())->mapToGlobal(pos));
        });
    }
    tb->addSeparator();

    // 🔴 2026-09-01 (owner): nut "Join TorReader" ngay ben canh "Share app".
    // Owner chot: BAM LA MO WEB LUON, khong hien hop thoai trung gian.
    // "Popup" theo y owner = chu chi dan hien khi RE CHUOT vao nut (tooltip), khong phai dialog.
    // Phan Sign up / dang nhap Google / Microsoft nam o TRANG WEB, app chi mo dung trang do.
    {
        QAction* joinAct = tb->addAction("Join TorReader", this, [] {
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://torreader.cloud/login")));
        });
        // ⚠️ Moi chu HIEN RA cho nguoi dung phai la TIENG ANH — app khong co tieng Viet
        //    trong menu/tooltip (owner chot 2026-09-01).
        joinAct->setToolTip(QStringLiteral(
            "Join TorReader for more apps — read DWF, SKP and IFC files"));
        joinAct->setStatusTip(joinAct->toolTip());
    }

    tb->addAction("Share app", this, [this] {
        const QString url = QStringLiteral("https://torreader.cloud/#download");
        QDialog dlg(this);
        dlg.setWindowTitle("Share TorReader PDF");
        auto* lay = new QVBoxLayout(&dlg);
        lay->addWidget(new QLabel("Send this link to download TorReader PDF (free, portable):", &dlg));
        auto* edit = new QLineEdit(url, &dlg);
        edit->setReadOnly(true);
        edit->selectAll();
        edit->setMinimumWidth(340);
        lay->addWidget(edit);
        auto* row = new QHBoxLayout();
        auto* copyBtn = new QPushButton("Copy link", &dlg);
        auto* openBtn = new QPushButton("Open in browser", &dlg);
        auto* closeBtn = new QPushButton("Close", &dlg);
        row->addWidget(copyBtn); row->addWidget(openBtn); row->addStretch(); row->addWidget(closeBtn);
        lay->addLayout(row);

        // ── Ung ho du an ──────────────────────────────────────────────
        auto* line = new QFrame(&dlg);
        line->setObjectName("sidebarSep");
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Plain);
        lay->addWidget(line);

        lay->addWidget(new QLabel("Support the project:", &dlg));
        auto* row2 = new QHBoxLayout();
        auto* xBtn  = new QPushButton("Follow on X", &dlg);
        auto* ghBtn = new QPushButton("Star on GitHub", &dlg);
        xBtn->setToolTip("Follow @FelixNgHuy on X (Twitter)");
        ghBtn->setToolTip("Give the project a star on GitHub — helps others find it");
        row2->addWidget(xBtn); row2->addWidget(ghBtn); row2->addStretch();
        lay->addLayout(row2);

        connect(xBtn, &QPushButton::clicked, &dlg, [] {
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://x.com/FelixNgHuy")));
        });
        connect(ghBtn, &QPushButton::clicked, &dlg, [] {
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/FelixNgH/TorreaderPDF")));
        });

        connect(copyBtn, &QPushButton::clicked, &dlg, [this, url, copyBtn] {
            QGuiApplication::clipboard()->setText(url);
            copyBtn->setText("Copied!");
            if (statusBar()) statusBar()->showMessage("Link copied to clipboard", 3000);
        });
        connect(openBtn, &QPushButton::clicked, &dlg, [url] { QDesktopServices::openUrl(QUrl(url)); });
        connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
        dlg.exec();
    });

    // F1 opens AboutDialog → Shortcuts tab
    auto* f1Act = new QAction(this);
    f1Act->setShortcut(QKeySequence("F1"));
    connect(f1Act, &QAction::triggered, this, [this] {
        AboutDialog dlg(m_darkMode, this);
        dlg.showShortcutsTab();
        dlg.exec();
    });
    addAction(f1Act);
}

// ── File operations ──────────────────────────────────────────────────────────

// 0927 M6 (--torcache-probe): nap lai tai lieu cua tab `docIdx` bang DUNG duong
// `taiLai` cua nguoi dung (giong dong 3106: `loadTabFile(t, t->pdfPath, true)`).
// Khong viet lai logic trong harness — neu kiem chung duoc log `ly do=taiLai` thi
// chinh day la duong app that su chay.
bool MainWindow::probeLoadTabFile(int docIdx) {
    if (docIdx < 0 || docIdx >= m_openDocs.size()) return false;
    DocTab* t = m_openDocs[docIdx];
    if (!t || t->pdfPath.isEmpty()) return false;
    loadTabFile(t, t->pdfPath, true);
    return true;
}

void MainWindow::probeCloseOpenDoc(int docIdx) {
    if (docIdx < 0 || docIdx >= m_openDocs.size()) return;
    const int ti = m_docTabs->indexOf(m_openDocs[docIdx]->view);
    if (ti >= 0) onTabClose(ti);
}

QString MainWindow::probeMemBreakdown() const {
    // Bai do 31/08: liet ke TUNG kho anh dang giu, de biet 95% RAM nam o dau.
    auto MB = [](qint64 b){ return double(b) / 1048576.0; };
    qint64 tabCache = 0;
    int nTab = 0;
    for (DocTab* t : m_openDocs) {
        if (!t) continue;
        ++nTab;
        if (t->renderer) tabCache += t->renderer->tabCacheBytes();
    }
    qint64 gpuImg = 0, gpuVec = 0;
    for (DocTab* t : m_openDocs) {
        if (!t || !t->view) continue;
        gpuImg += t->view->bytesHeldImages();
        gpuVec += t->view->bytesVectorLayer();
    }
    qint64 contImg = m_continuousView ? m_continuousView->bytesPageImages() : 0;
    qint64 contLow = m_continuousView ? m_continuousView->bytesPageLowRes() : 0;
    int    nImg    = m_continuousView ? m_continuousView->countPageImages() : 0;
    int    nLow    = m_continuousView ? m_continuousView->countPageLowRes() : 0;
    const qint64 tong = tabCache + contImg + contLow
                      + PdfRenderer::globalCacheBytes() + PageCache::totalBytes();
    return QString("  so tab=%1 | anh raster theo tab=%2MB | globalCache=%3MB | PageCache=%4MB (%5 trang)\n"
                   "  ContinuousView: anh day du=%6MB (%7 trang) | anh tho=%8MB (%9 trang)\n"
                   "  PdfGpuView (view Single, MOI TAB mot bo, giu ca khi an):"
                   " 6 QImage=%11MB | lop vector=%12MB\n"
                    "  GPU texture: %13MB (%14 cai)\n"
                    // 🔴 LƯỢT 27 (săn rò): CÂN BẰNG handle PDFium TOÀN CỤC — đọc được
                    // cả khi tab đã đóng (m_openDocs rong). docMo/poolMo=0 ⇒ moi
                    // FPDF_DOCUMENT/pool doc da FPDF_CloseDocument. pageMo AM la do
                    // trang ve bang pool handle + heavyPrivPage mo FPDF_LoadPage THÔ
                    // (khong tang g_pdfiumPageOpen) nhung dong qua closePage (tang
                    // g_pdfiumPageClose) => lech dem, KHONG phai ro handle. doomed/
                    // orphan>0 luc docMo=0 moi la ro FPDF_PAGE that; liveDocs=0 ⇒
                    // PdfCloseTrace khong con doc nao.
                    "  SO HANDLER: docMo=%15 poolMo=%16 pageMo=%17 | liveDocs=%18 PageCache doomed=%19 orphan=%20 deadDocs=%21\n"
                    "  => TONG app dem duoc = %10 MB")
        .arg(nTab).arg(MB(tabCache), 0, 'f', 0)
        .arg(MB(PdfRenderer::globalCacheBytes()), 0, 'f', 0)
        .arg(MB(PageCache::totalBytes()), 0, 'f', 0).arg(PageCache::entryCount())
        .arg(MB(contImg), 0, 'f', 0).arg(nImg)
        .arg(MB(contLow), 0, 'f', 0).arg(nLow)
        .arg(MB(tong + gpuImg + gpuVec), 0, 'f', 0)
        .arg(MB(gpuImg), 0, 'f', 0).arg(MB(gpuVec), 0, 'f', 0)
        .arg(MB(g_gpuTexBytes.loadRelaxed()), 0, 'f', 0).arg(g_gpuTexAlive.loadRelaxed())
        .arg(g_pdfiumDocOpen.loadRelaxed()  - g_pdfiumDocClose.loadRelaxed())
        .arg(g_pdfiumPoolOpen.loadRelaxed() - g_pdfiumPoolClose.loadRelaxed())
        .arg(g_pdfiumPageOpen.loadRelaxed() - g_pdfiumPageClose.loadRelaxed())
        .arg(PdfCloseTrace::liveDocs())
        .arg(PageCache::doomedCount()).arg(PageCache::orphanCount()).arg(PageCache::deadDocCount());
}

// 🔴 LƯỢT 30 (VIỆC 1 — DO RAM): các hàm soi kho bo nho cho --ram-probe.
QString MainWindow::probePoolInfoAll() const {
    QString s;
    int i = 0;
    for (DocTab* t : m_openDocs) {
        if (!t || !t->renderer) { ++i; continue; }
        s += QStringLiteral("tab%1[%2] ").arg(i)
                 .arg(t->renderer->probePoolInfo());
        ++i;
    }
    return s;
}
int MainWindow::probeIdlePoolDocsAll() const {
    int n = 0;
    for (DocTab* t : m_openDocs)
        if (t && t->renderer) n += t->renderer->probeIdlePoolCount();
    return n;
}
bool MainWindow::probeCloseOneIdlePoolDocTab(int docIdx) {
    DocTab* t = m_openDocs.value(docIdx);
    return t && t->renderer && t->renderer->probeCloseOneIdlePoolDoc();
}
QString MainWindow::probeRamBreakdown() const {
    auto MB = [](qint64 b){ return double(b) / 1048576.0; };
    // Pool doc: tong so doc TUNG VE (nghi phạm A) — moi cai giu object cache cua no.
    int poolEver = 0, poolSlots = 0;
    for (DocTab* t : m_openDocs) {
        if (!t || !t->renderer) continue;
        const QString pi = t->renderer->probePoolInfo();
        poolEver  += pi.section(QLatin1String("everUsed="), 1).section(QLatin1Char(' '), 0, 0).toInt();
        poolSlots += pi.section(QLatin1String("slots="), 1).section(QLatin1Char(' '), 0, 0).toInt();
    }
    // Anh raster: globalCache (PdfRenderer dem theo tab) + PageCache + Continuous + vector.
    qint64 gpuVec = 0;
    for (DocTab* t : m_openDocs) {
        if (!t) continue;
        for (const auto& vl : t->vecLayers)
            if (vl) gpuVec += vl->approxBytes();
    }
    const qint64 contImg = m_continuousView ? m_continuousView->bytesPageImages() : 0;
    const qint64 contLow = m_continuousView ? m_continuousView->bytesPageLowRes() : 0;
    // Thumbnails (chi tab hien hanh co trong panel) — tong QImage dang giu.
    qint64 thumb = 0; int thumbN = 0;
    if (m_thumbPanel && currentTab() && currentTab()->doc && currentTab()->doc->isOpen()) {
        const int pg = currentTab()->doc->pageCount();
        for (int i = 0; i < pg; ++i) {
            QImage im = m_thumbPanel->thumbnailForPage(i);
            if (!im.isNull()) { thumb += im.sizeInBytes(); ++thumbN; }
        }
    }
    return QStringLiteral(
        "RAMBREAK poolSlots=%1 poolEverUsed=%2 | globalCache=%3MB | PageCache=%4MB(%5tr) "
        "| contDayDu=%6MB contTho=%7MB | vecLayers=%8MB | thumbs=%9MB(%10)")
        .arg(poolSlots).arg(poolEver)
        .arg(MB(PdfRenderer::globalCacheBytes()), 0, 'f', 0)
        .arg(MB(PageCache::totalBytes()), 0, 'f', 0).arg(PageCache::entryCount())
        .arg(MB(contImg), 0, 'f', 0).arg(MB(contLow), 0, 'f', 0)
        .arg(MB(gpuVec), 0, 'f', 0)
        .arg(MB(thumb), 0, 'f', 0).arg(thumbN);
}

void MainWindow::setViewMode(bool fastMode) {
    m_fastMode = fastMode;
    // 🔴 Dong bo NUT theo mode THUC TE. setChecked() khong phat triggered() nen khong lap vo tan.
    // Thieu buoc nay thi moi duong doi mode KHONG qua thanh cong cu (khoi dong, phim tat, phuc hoi
    // phien cu) deu lam nut lech voi trang thai that.
    if (m_viewFastAct && m_viewFastAct->isChecked() != fastMode)
        m_viewFastAct->setChecked(fastMode);
    if (m_viewQualityAct && m_viewQualityAct->isChecked() == fastMode)
        m_viewQualityAct->setChecked(!fastMode);
    auto* t = currentTab();
    if (fastMode) {
        // Fast mode: show continuous view, hide tab widget (set fixed height)
        m_docTabs->setFixedHeight(m_docTabs->tabBar()->sizeHint().height());
        m_continuousView->show();
        if (t && t->doc->isOpen()) {
            // 🔴 SUA 2026-08-31: KHONG goi cancelPending() o day nua.
            // Do duoc trong log owner khi bam nut View Fast:
            //   cont enter fast mode zoom= 0.3
            //   slice ms= 1514 page= 0                    <- trang dang xem da render 1,5 giay
            //   drop page= 0 reason=genMismatchMid        <- BI VUT
            //   cont drop page= 0 reason=nullResult       <- trang thanh RONG, phai lam lai
            // cancelPending() la lenh huy TOAN DIEN (tang ca 5 bo dem the he + clear ca 2 pool),
            // nen no giet luon luot render cua chinh trang nguoi dung dang nhin.
            //
            // Viec do la PHI, vi Single va Continuous dung CHUNG mot PdfRenderer va CHUNG bo dem:
            // de luot render ay chay xong thi Continuous huong luon (log cung phien:
            // "cache-only-for-cont hit mem page= 1" -> hien TUC THI).
            // Ly do cu ("giu khoa pdfium lam dung giao dien") khong con: luong giao dien nay
            // dung TryPdfiumLock, khong bao gio cho khoa nua.
            // Lenh render Single MOI da bi chan bang cac chot `if (!m_fastMode)`.
            // GOC1: pass user's zoom to continuous view. PdfRenderer handles
            // render resolution — no longer hardcodes kFullRenderMaxPx.
            qDebug() << "[perf] cont enter fast mode zoom=" << t->zoom;
            { QElapsedTimer _e; _e.start(); m_continuousView->setZoom(t->zoom); const qint64 _m=_e.elapsed(); if(_m>30) qDebug().noquote()<<"[conttoggle] setZoom ms="<<_m; }
            { QElapsedTimer _e; _e.start(); m_continuousView->setDocument(t->doc.get(), t->renderer.get(), &t->visualsCache, &t->vecLayers); const qint64 _m=_e.elapsed(); if(_m>30) qDebug().noquote()<<"[conttoggle] setDocument ms="<<_m; }
            // Enter continuous mode: clear any raster suppression commands set when in single mode,
            // because ContinuousView draws using its own separate vector layer.
            if (t->renderer) t->renderer->setSuppressFullQuality(t->currentPage, false);
            m_continuousView->setVectorCacheKey(t->pdfPath.isEmpty() ? t->doc->filePath() : t->pdfPath, t->pdfHash);
            // Push annot visuals for current page to continuous view
            if (t->annotMgr)
                { QElapsedTimer _e; _e.start(); refreshAnnotVisuals(t, t->currentPage); const qint64 _m=_e.elapsed(); if(_m>30) qDebug().noquote()<<"[conttoggle] refreshAnnotVisuals ms="<<_m; }
            // Scroll continuous view to the current page so position is preserved
            // when switching from Quality mode (VIỆC 3).
            // 🔴 LƯỢT 33b (L): dung offset THAT da luu (contScrollY), khong phai
            // scrollToPage(currentPage) — currentPage la trang o TAM, scrollToPage no
            // keo DINH trang do len dau ⇒ tam lech, vi tri bi troi dan moi lan doi che do.
            m_continuousView->restoreScrollY(t->contPosSaved ? t->contScrollY : 0);
        }
    } else {
        // Quality mode: show tab widget normally, hide continuous view
        m_docTabs->setMinimumHeight(0);
        m_docTabs->setMaximumHeight(QWIDGETSIZE_MAX);
        m_continuousView->hide();
        // Sync GPU view state on returning to single mode
        if (t && t->doc->isOpen()) {
            t->view->setZoom(t->zoom);
            t->renderer->cancelPending();
            t->renderer->setSuppressFullQuality(t->currentPage, false);
            t->renderer->requestPage(t->currentPage, t->zoom);
        }
    }
    // 🔴 LƯỢT 37 (reviewer lỗi 2): setViewMode(true) ở trên LUÔN show ContinuousView + ép
    // m_docTabs về chiều cao thanh tab, vô điều kiện. KHÔNG có tab nào ⇒ bật lại View Fast (hoặc
    // Quality) sẽ cho ContinuousView trống hiện ra và bóp tab Welcome ⇒ MẤT màn chào. Đây là MỌI
    // đường show/hide ContinuousView, nên chốt lại bằng đúng hàm đã dùng ở onTabChanged:
    // rỗng ⇒ ẩn ContinuousView + trả chiều cao cho Welcome; có tab + fast ⇒ khôi phục layout.
    applyWelcomeVisibility();
}
// ── File operations ──────────────────────────────────────────────────────────

void MainWindow::onOpenFile() {
    QString path = QFileDialog::getOpenFileName(
        this, "Open PDF", {}, "PDF Files (*.pdf)");
    if (!path.isEmpty()) openFile(path);
}

void MainWindow::showNotePopup(const QString& text, const QString& author) {
    if (text.isEmpty()) { hideNotePopup(); return; }
    if (!m_notePopup) {
        m_notePopup = new QLabel(nullptr, Qt::ToolTip | Qt::FramelessWindowHint);
        m_notePopup->setWordWrap(true);
        m_notePopup->setMargin(8);
        m_notePopup->setMaximumWidth(360);
    }
    const ThemeTokens& t = m_darkMode ? darkHC() : lightHC();
    m_notePopup->setStyleSheet(QStringLiteral("QLabel{ background:%1; border:1px solid %2; color:%3; }")
                                   .arg(t.warnBg, t.border, t.fg));
    m_notePopup->setText(author.isEmpty() ? text : (author + ":\n" + text));
    m_notePopup->adjustSize();
    m_notePopup->move(QCursor::pos() + QPoint(14, 14));
    m_notePopup->show();
}

void MainWindow::hideNotePopup() {
    if (m_notePopup) m_notePopup->hide();
}

void MainWindow::onSaveFile() {
    auto* t = currentTab();
    if (!t) return;
    if (!t->dirty) {
        statusBar()->showMessage("No unsaved changes", 2000);
        return;
    }
    const QString original = t->originalPath;
    const QString prevFile = t->doc->filePath();
    const QString tmp      = makeTmpPath(original);

    // 1. Materialize deferred page content before saving
    //    (annotations in /Annots are preserved by FPDF_SaveAsCopy only after GenerateContent).
    const QSet<int> dirtyPages = t->pagesNeedGenerate;
    for (int pg : t->pagesNeedGenerate)
        t->annotMgr->generateContentForPage(pg);
    t->pagesNeedGenerate.clear();

    // 2. Flush the in-memory document (page edits + annotations) to a temp file
    //    (the document is still open, so FPDF_SaveAsCopy can read it).
    if (t->annotMgr) {
        t->annotMgr->setDocument(t->doc->raw(), tmp);
        if (!t->annotMgr->saveDocument()) {
            // P1b (0921): bao RO ly do — nhat la khi /AP FreeText ngoai khong va duoc.
            // tmp la ban nhap, KHONG duoc thay the tep goc ⇒ don no di.
            QFile::remove(tmp);
            QMessageBox::warning(this, "Save",
                "Lưu thất bại — tệp gốc CHƯA bị thay đổi.\n\n" + t->annotMgr->lastError());
            onSaveAsFile(); return;
        }
    }

    // 2. Release our own handles on the original so it can be overwritten
    //    (the open PDFium document + thumbnail pool lock the file on Windows).
    //    Huy ca request link dang bay truoc khi doc bi dong (SPEC_NO_SYNC_PAGELOAD).
    PdfLinks::clearCache();
    // 🔴 Huy + cho xong tac vu lop bu truoc khi dong doc de ghi de file (crash 30/08).
    cancelForeignAnnotTasks(t);
    // R1/0903: thumbPool workers dung CHUNG doc — phai stop + CHO thoat HAN truoc khi
    // UI cham PDFium (closeDocument, doc->close) de ghi de file.
    stopThumbPool(t);
    qWarning() << "[probeSave] step5 stopThumbPool xong";
    TextSelection::closeDocument(t->doc->raw());
    t->doc->close();
    if (t->renderer) t->renderer->setTileCache(nullptr);
    qWarning() << "[probeSave] step6 doc closed";

    // 3. Replace the original atomically (không xoá trước — xem báo cáo SAFESAVE).
    QString err;
    if (replaceFileAtomically(tmp, original, &err)) {
        QFile::remove(tmp);
        loadTabFile(t, original, /*structureChanged=*/false);    // reopen from the saved original
        if (t->thumbPool)
            for (int pg : dirtyPages)
                t->thumbPool->requestThumbnail(pg, /*priority=*/2);
        if (prevFile != original) removeWorkingCopy(prevFile);   // discard any page-edit working copy
        t->dirty = false;
        updateTabDirty(t);
        statusBar()->showMessage("Saved: " + QFileInfo(original).fileName(), 3000);
        return;
    }
    qWarning() << "[save] replaceFileAtomically failed:" << err;

    // 4. Couldn't overwrite — reopen from the temp so the tab keeps its edits, then offer Save As.
    loadTabFile(t, tmp);
    QMessageBox::information(this, "Save",
        "Không ghi đè được file gốc — có thể nó đang mở ở chương trình khác, chỉ-đọc, "
        "hoặc OneDrive đang khoá để đồng bộ.\n"
        "File gốc của bạn VẪN CÒN NGUYÊN, không bị mất gì.\n"
        "Các thay đổi đang được giữ lại — hãy chọn nơi lưu mới.\n\n"
        "Chi tiết: " + err);
    onSaveAsFile();
}

// 0927 LƯỢT 2 — probe hook. KHÔNG dùng ngoài harness.
QString MainWindow::probeSaveViaGui(QString* errOut) {
    auto* t = currentTab();
    if (!t) { if (errOut) *errOut = "khong co tab"; return QString(); }
    const QString target = t->originalPath;
    if (target.isEmpty()) { if (errOut) *errOut = "tab chua co originalPath"; return QString(); }
    // onSaveFile() sớm thoát nếu !dirty ⇒ đánh dấu như người dùng vừa sửa.
    t->dirty = true;
    updateTabDirty(t);
    qWarning().noquote() << "[probeSave] goi onSaveFile, target=" << target
                         << " annotMgr=" << (t->annotMgr ? "co" : "khong")
                         << " pagesNeedGenerate=" << t->pagesNeedGenerate.size();
    onSaveFile();
    qWarning().noquote() << "[probeSave] onSaveFile xong, size="
                         << QFileInfo(target).size();
    // onSaveFile() báo lỗi bằng QMessageBox (modal) — probe không bấm được.
    // Ta chỉ cần biết tệp có đổi mã không: so checksum trước/sau.
    QFileInfo fi(target);
    if (!fi.exists()) { if (errOut) *errOut = "tep khong con sau save"; return QString(); }
    return target;
}

// 0927 LƯỢT 2 — probe: onSaveFile trong event loop + timeout. Bản đồng bộ
// (probeSaveViaGui) treo vì các bước trong onSaveFile chờ việc nền (dừng pool
// thumbnail, hủy task ngoài) còn event loop thì không quay. Ở đây ta bơm
// processEvents trong khi chờ, và CÓ ĐỦNG thời gian ⇒ treo thì báo chứ không
// treo cả tiến trình.
bool MainWindow::probeSaveViaGuiAsync(QString* errOut) {
    auto* t = currentTab();
    if (!t) { if (errOut) *errOut = "khong co tab"; return false; }
    const QString target = t->originalPath;
    if (target.isEmpty()) { if (errOut) *errOut = "tab chua co originalPath"; return false; }
    t->dirty = true;
    updateTabDirty(t);

    bool done = false;
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, this, [&done]() { done = true; });
    QTimer::singleShot(0, this, [this]() { onSaveFile(); });
    timer->start(1000);           // nhịp để ta có cửa sổ bơm event
    QElapsedTimer el; el.start();
    // 🔴 0927 LƯỢT 3: đóng mọi hộp thoại MODAL phát sinh trong lúc chờ.
    // onSaveFile() (và onSaveAsFile khi trượt) báo lỗi bằng QMessageBox — hộp
    // modal đó chặn vòng lặp cho tới khi có người bấm, mà probe không có ai ⇒
    // treo tới hết 20 s rồi báo sai nguyên nhân. Đo 27/09: đúng lỗi này làm
    // --ftmulti-probe treo 60 s, không in dòng FTMULTI.
    auto closeModals = []() {
        for (int guard = 0; guard < 8; ++guard) {
            QWidget* w = QApplication::activeModalWidget();
            if (!w) break;
            if (auto* mb = qobject_cast<QMessageBox*>(w)) {
                qWarning().noquote() << "[probeSave] dong hop bao loi:" << mb->text();
            }
            w->close();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
    };
    while (!done && el.elapsed() < 20000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QCoreApplication::sendPostedEvents();
        closeModals();
        if (done) break;
        QThread::msleep(5);
    }
    // onSaveFile có thể báo lỗi rồi bật onSaveAsFile (QFileDialog) — đóng nốt
    // cho sạch để không treo ở hộp chọn tệp.
    closeModals();
    const bool finished = done;
    timer->stop();
    if (!finished) {
        if (errOut) *errOut = "onSaveFile chua xong sau 20s (treo) — xem log [probeSave]";
        return false;
    }
    if (!QFileInfo(target).exists()) {
        if (errOut) *errOut = "tep khong con sau save";
        return false;
    }
    return true;
}

void MainWindow::onSaveAsFile() {
    auto* t = currentTab();
    if (!t) return;
    QString dest = QFileDialog::getSaveFileName(
        this, "Save As",
        t->originalPath.isEmpty() ? QString() : t->originalPath,
        "PDF Files (*.pdf)");
    if (dest.isEmpty()) return;
    if (!dest.endsWith(".pdf", Qt::CaseInsensitive)) dest += ".pdf";

    const QString srcOriginal = t->originalPath;
    const QString prevFile    = t->doc->filePath();
    const QString tmp         = makeTmpPath(dest);

    if (t->annotMgr) {
        // Materialize deferred page content before saving
        for (int pg : t->pagesNeedGenerate)
            t->annotMgr->generateContentForPage(pg);
        t->pagesNeedGenerate.clear();
        t->annotMgr->setDocument(t->doc->raw(), tmp);
        if (!t->annotMgr->saveDocument()) {
            QMessageBox::warning(this, "Save As",
                "Could not write to:\n" + dest + "\n\n" + t->annotMgr->lastError());
            QFile::remove(tmp);
            return;
        }
    }

    QString err;
    if (replaceFileAtomically(tmp, dest, &err)) {
        QFile::remove(tmp);
    } else {
        QMessageBox::warning(this, "Save As",
            "Could not write to:\n" + dest + "\n\n" + err);
        QFile::remove(tmp);
        return;
    }

    t->originalPath = dest;      // the tab now belongs to the new file
    loadTabFile(t, dest);
    if (prevFile != srcOriginal && prevFile != dest)
        removeWorkingCopy(prevFile); // discard a page-edit working copy, never the source original
    t->dirty = false;
    updateTabDirty(t);
    statusBar()->showMessage("Saved as: " + QFileInfo(dest).fileName(), 4000);
}

const QList<AnnotInfo>& MainWindow::annotsForPage(DocTab* t, int page, bool* outOk) {
    // 0927 LƯỢT 10 (--no-annotscan): day la noi DUY NHAT doc danh sach chu thich
    // 1 trang (AnnotationManager::loadPage, :1992 va :2006) cho sidebar comment +
    // chon doi tuong. Tra rong, bao "khong ok" de ben goi giu du lieu cu.
    if (trNoAnnotScan()) {
        if (outOk) *outOk = false;
        static const QList<AnnotInfo> kEmpty;
        return kEmpty;
    }
    if (t->annotPageCache.contains(page)) {
        qDebug().noquote() << "[perf] annotsForPage page=" << page
                 << "cache=HIT count=" << t->annotPageCache[page].size();
        if (outOk) *outOk = true;
        return t->annotPageCache[page];
    }
    // 🔴 TUYET DOI khong chan luong giao dien: loadPage nhan outOk, neu khoa ban
    //    thi tra ve !ok va ben goi GIU NGUYEN du lieu cu, hen lai. Khong tra rong
    //    nhu the that su khong co annot (CRASH 01:52 30/08 + Cloud mat).
    bool ok = true;
    QList<AnnotInfo> list;
    if (t->annotMgr)
        list = t->annotMgr->loadPage(page, &ok);
    if (!ok) {
        // 🔴 VIỆC 1 (SPEC_SMOOTH_123 31/08): khoa ban → GIU NGUYEN du lieu cu dang hien,
        // KHONG xoa cache. DUONG THOAT BAT BUOC la day viec that sang LUONG NEN (loadPage
        // block hop le o QtConcurrent), KHONG phai hen lai bang GUI timer (livelock).
        qDebug().noquote() << "[annot] loadPage SKIP — khoa ban, day sang luong nen page=" << page;
        if (outOk) *outOk = false;
        if (!t->annotRetryPending.contains(page)) {
            t->annotRetryPending.insert(page);
            AnnotationManager* mgr = t->annotMgr.get();
            const int pg = page;
            auto res = std::make_shared<QList<AnnotInfo>>();
            // 🔴 LƯỢT 22: token đăng ký LÚC SPAWN (UI thread) — `mgr->document()`
            // được chốt NGAY tại đây, không đọc `mgr` trên luồng nền lúc tab chết.
            // mgr vẫn sống an toàn tới `delete t` vì shutdownTab chờ bgSync (dưới).
            trdoc::Task task(mgr ? mgr->document() : nullptr, "annotPage");
            auto fut = QtConcurrent::run([mgr, pg, res, task = std::move(task)] {
                if (!mgr) return;
                *res = mgr->loadPage(pg);   // blocking OK o luong nen
            });
            t->annotPageFuture = fut;
            t->addBgWait(fut);   // L22: chờ HẾT, slot bị đè theo trang — L22b: có dọn mục xong
            auto* w = new QFutureWatcher<void>(this);
            w->setFuture(fut);
            connect(w, &QFutureWatcher<void>::finished, this,
                    [this, w, t, pg, res, s = t->serial]() {
                w->deleteLater();
                if (!tabAlive(t, s)) return;   // tab dong — bo qua
                t->annotRetryPending.remove(pg);
                t->annotPageCache[pg] = *res;
                if (t == currentTab()) {
                    refreshCommentsForPage(t, pg);
                    refreshAnnotVisuals(t, pg);
                }
            });
        }
        if (t->annotPageCache.contains(page))
            return t->annotPageCache[page];   // tra du lieu cu (neu co)
        static const QList<AnnotInfo> kEmpty;
        return kEmpty;
    }
    QElapsedTimer _perf;
    _perf.start();
    t->annotPageCache[page] = list;
    qint64 ms = _perf.elapsed();
    qDebug().noquote() << "[perf] annotsForPage page=" << page
             << "cache=MISS count=" << list.size() << "ms=" << ms;
    if (outOk) *outOk = true;
    return t->annotPageCache[page];
}

void MainWindow::invalidateAnnotPage(DocTab* t, int page) {
    t->annotPageCache.remove(page);
    // 🔴 LÁT C 0902: visualsCache GIU LAI — no la nen de duong CẬP NHẬT CÓ CHỌN LỌC
    // (mergeAnnotVisuals) tron delta vao, thay vi pha no di de roi nap lai ca trang
    // phai xep hang 1–2,4 giay sau bo dung trang. An toan: moi lan su dung van kiem
    // `visualsRev == pageRevision` (moi mutation deu bump revision), va moi thay doi
    // KHONG co delta (vd annot ngoai bi xoa) se truh thanh cache-miss → nap lai.
    if (t->heavyRegionPage == page)
        t->heavyRegionPage = -1;   // LAT C: noi dung doi — anh region cu het han
    // LAT G: noi dung trang doi => handle rieng (danh sach annot da cu) phai NAP LAI,
    // va danh dien vung nhin phai reset de lan goi theo su kien dung lai BAT KE vung
    // nhin co doi khong (neu khong, chot vung-nhin-seen chan luon viec ve chu thich moi).
    if (t->heavyRegionPages.contains(page)) {
        t->heavySeenPage = -1;
        if (t->heavyPrivIndex == page) {
            if (t->heavyRegionBuilding) t->heavyPrivStale = true;   // dang render: cho tac vu nap lai
            else closeHeavyPriv(t);                                  // re: dong ngay
        }
    }
    // LAT E 09/02 (UU TIEN 2): trang nang nen vector ve chu thich TU ANH VUNG, khong
    // tu lop vector (lop vector KHONG chua annot). Chu thich vua tao/xoa phai duoc
    // dung lai NGAY — goi thang updateHeavyRegion tu su kien chu thich doi (giu nguyen
    // nhu ban tham chiếu latABCDEFG, chi bo dong ho polling 1s); no tu guard (khong
    // phai trang hien hanh / khong heavy / dang dung thi no return), nen goi vo hai.
    if (t->heavyRegionPages.contains(page) && t == currentTab()
        && t->currentPage == page && t->view)
        updateHeavyRegion(t);
    t->fgnDeferred.remove(page);   // noi dung doi — duoc hoan lai toi da 1 lan cho lan hien moi
    if (t->fgnLayer && t->fgnLayer->pageIndex() == page) {
        t->fgnLayer.reset();
        if (t->view) t->view->setForeignAnnotLayer(nullptr);
    }
    // 🔴 LƯỢT 39: chu thich doi (tao/xoa) => reset dang ky xep hang cho trang nay
    // de refreshAnnotVisuals lan toi bao gio co yeu cau LUON dung lai, khong de
    // retry counter canh trang vĩnh viễn invisible.
    t->visualsRetry.remove(page);
    t->visualsHeavyDue.remove(page);
}

void MainWindow::buildVectorLayer(DocTab* t, int pageIndex, bool force) {
    // LÁT B 09/02: "da co lop cho trang nay chua" doc tu KHO CHUNG (vecLayers), khong con
    // lop rieng mot nua. Continuous co the da nap san trang nay -> Single khoi dung lai.
    if (t->vecBuilding.contains(pageIndex)
        || (!force && t->vecLayers.contains(pageIndex)))
        return;
    int pg = pageIndex;
    // force = "noi dung trang DA DOI, dung lai di". Tu day tro di trang nay KHONG duoc lay
    // tu cache .torvec nua (cache khoa theo hash file tren dia, khong thay doi trong bo nho).
    if (force) t->torvecDirty.insert(pg);
    t->vecBuilding.insert(pg);
    auto layer = std::make_shared<VectorLayer>();
    auto* w = new QFutureWatcher<bool>(this);
    connect(w, &QFutureWatcher<bool>::finished, this, [this, w, t, pg, layer, s = t->serial]{
        w->deleteLater();
        if (!tabAlive(t, s)) return;   // 🔴 L20: chốt TRƯỚC khi dereference (t có thể đã bị closeJob delete)
        t->vecBuilding.remove(pg);
        if (t->currentPage != pg) return;
        if (t != currentTab()) return;
        if (w->result()) {
            setTabVectorLayer(t, layer, pg);
        } else {
            setTabVectorLayer(t, nullptr, pg);
        }
    });
    FPDF_DOCUMENT d = t->doc->raw();
    const QString pdfPath = t->pdfPath;
    const quint64 pdfHash = t->pdfHash;
    const QString docPath = t->doc->filePath();
    const bool allowCache = !t->torvecDirty.contains(pg);
    // 🔴 LƯỢT 22: token đăng ký LÚC SPAWN, RAII move vào lambda (beginClose thấy
    // cả task xếp hàng). Vec build KHÔNG vào bgSync — build CAD tới 14 s, chờ trên
    // UI là đứng hình; thân task không đọc `t->` (chỉ layer/d), token + shared_ptr
    // vecGen bảo vệ đủ.
    trdoc::Task task(d, "VectorLayer::build/markup");
    w->setFuture(QtConcurrent::run([layer, d, pg, pdfPath, pdfHash, docPath, allowCache,
                                    task = std::move(task)]{
        // Thu cache .torvec truoc — nap nhanh gap 45 lan so voi dung lai tu PDF.
        // Khoa cache co the chua kip dat (initWatcher chay bat dong bo). Tu bu: hashFile chi
        // doc 128 KB (64 KB dau + 64 KB cuoi) nen re, an toan goi o luong nen.
        const QString keyPath = pdfPath.isEmpty() ? docPath : pdfPath;
        const quint64 keyHash = pdfHash ? pdfHash : (quint64)TileCacheFile::hashFile(keyPath);
        if (allowCache && VectorCache::tryLoad(*layer, keyPath, keyHash, pg)) return true;
        // Đường MARKUP (ghi chú vừa thêm/xoá) vẫn dựng lại dù người dùng có đang
        // nhìn hay không — nhưng tài liệu đang đóng thì dừng ngay.
        if (task.cancelled()) return false;
        // `[&task]`: Task KHONG copy được (giữ số đếm trong sổ) — bắt buộc bắt tham chiếu.
        if (!layer->build(d, pg, [&task]() { return task.cancelled(); })) return false;
        if (allowCache) VectorCache::trySave(*layer, keyPath, keyHash, pg);
        return true;
    }));
}

void MainWindow::ensureForeignAnnotLayer(DocTab* t, int pageIndex) {
    if (!t || !t->doc || !t->doc->isOpen() || !m_openDocs.contains(t)) return;
    // 0927 LƯỢT 10 (--no-fgn): KHONG dung lop bu chu thich ngoai. Chan O DAY
    // (truoc ca ca logic DEFER/CountObjects/canFastPath) ⇒ khong schedule task nao,
    // khong render trang, khong giu khoa pdfium. ForeignAnnotLayer::build /
    // ::buildRegion cung chan rieng (core/ForeignAnnotLayer.cpp) cho duong goi
    // truc tiep tu MainWindow.cpp:757 (vung bù zoom cao) va tu harness headless.
    if (trNoFgn()) return;
    // (2026-08-30): Khoi dem FPDFPage_CountObjects o day la CHET — bien `objs` tinh roi
    // khong ai dung (chot C3 ngay duoi). Nhung no van lay TimedPdfiumLock TREN LUONG GIAO
    // DIEN: tren trang 2,54 trieu path moi lan dem la vai giay dung app (da do 34s). Da xoa.
    // C3 (owner chot 2026-08-30): BO nhanh "trang nang => SKIP" — no chan markup VINH
    // VIEN tren trang quai vat, Owner khong chap nhan. Thay bang: chi dung lop bu SAU
    // KHI trang da co noi dung de ve. Trang chua co noi dung (chua co lop vector san
    // sang VA chua co anh raster) => HOAN lai, lan goi sau se dung. Khi da co noi dung
    // roi (trang da hien ra) thi dung lop bu binh thuong: chay o luong nen, do phan giai
    // maxPx da thich ung theo zoom. Muc tieu: trang quai vat HIEN NGAY bang lop vector,
    // markup xuat hien sau vai giay — thay vi khong bao gio co markup.
    const bool hasRaster = (m_fastMode && m_continuousView)
                              ? m_continuousView->pageHasContent(pageIndex)
                              : (t->view && t->view->hasImage());
    if (!pageBaseIsVector(t, pageIndex) && !hasRaster) {
        // Trang CHUA co gi de ve thuc su: moi hoan 1 lan. Lan goi sau BUOC PHAI dung —
        // neu khong trang khong bao gio co lop vector thi markup mat VINH VIEN (30/08:
        // trang 110 objects bi hoan vo han vi baseVector=false). Xoa danh dau khi doi trang.
        if (t->fgnDeferred.contains(pageIndex)) {
            qDebug().noquote() << "[fgnlayer] dung sau DEFER page=" << pageIndex
                               << "baseVector=" << pageBaseIsVector(t, pageIndex);
        } else {
            t->fgnDeferred.insert(pageIndex);
            qDebug().noquote() << "[fgnlayer] DEFER lan 1 page=" << pageIndex
                               << "baseVector=" << pageBaseIsVector(t, pageIndex);
            return;
        }
    }
    // MUC 3 (SPEC_PERF_FGNLAYER_ADAPTIVE 2026-08-30): trang nang >400K object thi
    // KHONG duoc dung lop bu. FPDF_RenderPageBitmap trong ForeignAnnotLayer::build
    // giu khoa 14s+ va m_cancel chi kiem TRUOC khi lay khoa — PDFium khong cho huy
    // giua chung. Thay bang: ep trang ve RASTER (forceRasterPage) — da co FPDF_ANNOT.
    // So object lay tu PdfRenderer::pageObjectCount (da cache, KHONG lay khoa pdfium).
    if (t->renderer) {
        int objCount = t->renderer->pageObjectCount(pageIndex);
        if (objCount == 0) {
            // SAI THU TU khoi dong (do 02/09): so object den muon 2,7 gi (heavycap pre-count)
            // trong khi ensureForeignAnnotLayer can ngay => 2 trang hien tai bi DEFER.
            // Phep dem FPDFPage_CountObjects chi ton 0 ms, NHUNG PageCache::acquire PHAI
            // phan tich CA TRANG (do 2 giay tren trang 2,54 trieu object). Khoa CHAN o day
            // da dung luong giao dien 7,1 gi (log 02/09, ke om khoa: GiuSuot 2,9 gi).
            // LUONG GIAO DIEN khong duoc di cho khoa pdfium — TryPdfiumLock y het
            // AnnotationManager::loadPage: lay duoc thi dem; khong lay duoc thi HOAN va
            // de luong nen lam (luot ve nen xong phat objectCountReady -> goi lai, noi 4324).
            TryPdfiumLock lk(__FILE__, __LINE__);
            if (!lk.held()) {
                // LỖI 2 0902: danh dau trang nay vao fgnDeferred (registry "da hoan, lan
                // sau PHAI dung") de continuousPageReady ben duoi duoc no. Neu khong ghi,
                // luot goi nay roi di va KHONG con su kien nao keo no quay lai (do: 7 DEFER,
                // 0 dung lai).
                t->fgnDeferred.insert(pageIndex);
                qDebug().noquote() << "[fgnlayer] DEFER - khoa pdfium ban, cho luong nen dem object page=" << pageIndex;
                return;
            }
            FPDF_PAGE pg = PageCache::acquire(t->doc->raw(), pageIndex);
            if (!pg) {
                // Chi hoan khi THAT SU khong muon duoc trang.
                qDebug().noquote() << "[fgnlayer] DEFER - khong muon duoc trang page=" << pageIndex;
                return;
            }
            PageCache::PageBorrow _b(t->doc->raw(), pageIndex);   // RAII — cap doi acquire
            objCount = FPDFPage_CountObjects(pg);
            t->renderer->setPageObjectCount(pageIndex, objCount);
        }
        if (objCount > 400000) {
            qDebug().noquote() << "[fgnlayer] SKIP - trang nang objects=" << objCount << "page=" << pageIndex;
            // LAT C 09/02: tran 400000 chan LOP BU TOAN TRANG (ForeignAnnotLayer::build —
            // 87,8 giay/trang, giu khoa 14s+ khong huy duoc) — KHONG giết nen vector nua.
            // Trang ma overlay ve duoc het chu thich (canFastPath) thi khong can gi ca.
            // Trang can bo: chu thich ve theo VUNG NHIN bang o clip tung annot
            // (renderAnnotRegion — 18,6 ms/o do tren chinh trang nay), nen vector van la
            // nen. Nen vector dung duoc ⇒ du ca hai; nen vector KHONG dung duoc thi
            // updateHeavyRegion khong cho ra dau, view tu o lai raster (raster da co
            // FPDF_ANNOT) ⇒ thà cham con hon mat chu thich.
            // TORREADER_NOHEAVYROI=1: ve lenh cu (vat nen vector) — dung cho A/B nghiem thu.
            if (!canFastPath(t, pageIndex)
                && !qEnvironmentVariableIsSet("TORREADER_NOHEAVYROI")) {
                t->heavyRegionPages.insert(pageIndex);
                if (t->view) t->view->setVectorAnnotSafe(pageIndex, true);
                updateHeavyRegion(t);
            } else {
                forceRasterPage(t, pageIndex);
            }
            return;
        }
    }
    // 🔴 CHI dung lop nay khi trang THUC SU ve bang lop vector. Nen raster von da co san
    //    chu thich (co FPDF_ANNOT) nen khong can bu. Thieu chot nay => dung lop vo ich,
    //    ton 9 giay giu khoa pdfium tren trang nang (do that 2026-08-09).
    // 🔴 CHOT MOI (2026-08-19): `overlayCapable` = lop overlay QPainter ve duoc MOI annot
    //    tren trang — va `loadPageVisuals` append visual cho MOI annot, KHONG loc TRUID,
    //    tuc annot NGOAI cung da duoc overlay ve. Vay dung them lop bu la THUA HOAN TOAN.
    //    Do that: lop bu nay ton 1,4s (ranh) den 87,8s (ket) MOI TRANG, render ca trang
    //    3999x2828 HAI LAN chi de tim ~3.700 pixel. Chi dung no khi overlay BO TAY.
    // MUC 1 (SPEC_PERF_FGNLAYER_ADAPTIVE 2026-08-30): lop bu chi de HIEN THI o zoom hien tai.
    // Zoom cao sau do da co buildRegion() lo vung net. Render 4000px khi man hinh chi ve
    // ~1300px la thua >9 lan dien tich (do 30/08: 7,5 s/trang giu khoa pdfium).
    const double zoomNow = t->view ? t->view->zoom() : 1.0;
    const QSizeF pagePt  = t->vecLayers.contains(pageIndex)
                             ? t->vecLayers.value(pageIndex)->pageSizePt()
                             : ((m_fastMode && m_continuousView)
                                 ? m_continuousView->pageSizePt(pageIndex) : QSizeF());
    const double longSidePt = (std::max)(pagePt.width(), pagePt.height());
    const double dpr = t->view ? t->view->devicePixelRatioF() : 1.0;
    int maxPx = int(std::ceil(longSidePt * zoomNow * dpr * 1.15));
    maxPx = std::clamp(maxPx, 900, int(PdfRenderer::kFullRenderMaxPx));

    const bool hasLayer = t->fgnLayer && t->fgnLayer->pageIndex() == pageIndex;
    // MUC 2 (SPEC_PERF_FGNLAYER_ADAPTIVE 2026-08-30): lop bu gan voi mot muc zoom.
    // Chi dung lai khi PHONG TO dang ke (zoomNow > m_builtZoom * 1.6) — thu nho dung
    // lai anh cu van net, tranh bom viec moi nac zoom.
    const bool needRebuild = hasLayer && zoomNow > t->fgnLayer->builtZoom() * 1.6;

    // 🔴 QUAY LUI 2026-09-01: da thu bo dieu kien "nen phai la vector" de pha vong luan quan.
    // Ket qua: lop bu VAN dung ra ANH RONG (diffPx=0) ngay ca tren tep co Note vang ro rang
    // (da dem duoc 76 pixel vang trong anh PDFium cua chinh tep do) ⇒ loi nam SAU HON, o chinh
    // phep so sanh hai luot ve cua ForeignAnnotLayer, khong phai o dieu kien goi.
    // ⇒ Khong dung lop bu nhieu hon vo ich. Giu nguyen dieu kien cu cho toi khi sua duoc phep so sanh.
    if (!canFastPath(t, pageIndex)
        && t->visualsHasForeign.value(pageIndex, false)
        && pageBaseIsVector(t, pageIndex)
        && !t->fgnBuilding.contains(pageIndex)
        && (!hasLayer || needRebuild)) {
        int pgF = pageIndex;
        t->fgnBuilding.insert(pgF);
        // LỖI 3 0902 (GIU LOP): ghi noi dung trang luc bat dau dung. pageRevision chi la
        // QHash lookup (khong lay khoa pdfium) — lay o luong giao dien truoc khi chay nen.
        const quint32 revAtStart = t->annotMgr ? t->annotMgr->pageRevision(pgF) : 0;
        auto fl = std::make_shared<ForeignAnnotLayer>();
        fl->setBuiltZoom(zoomNow);
        qDebug().noquote() << "[fgnlayer] maxPx=" << maxPx
                           << " zoom=" << zoomNow
                           << " page=" << pgF
                           << " rebuild=" << needRebuild;
        auto* wf = new QFutureWatcher<bool>(this);
        connect(wf, &QFutureWatcher<bool>::finished, this, [this, wf, t, pgF, fl, revAtStart, s = t->serial]{
            wf->deleteLater();
            // 🔴 Guard PHIEN DAU TIEN: tab co the da dong va t da bi xoa (closeJob chay
            //    o luong nen). Chi so sanh con tro, KHONG duoc lay gi cua t khi chua soat.
            if (!tabAlive(t, s)) return;
            t->fgnBuilding.remove(pgF);
            if (t->fgnFuture.isValid()) t->fgnFuture = QFuture<bool>();
            t->fgnPending.reset();
            // LỖI 1 0902: `t->currentPage` la trang o TAM man hinh (pageAtCenter), KHONG
            // phai trang vua thao tac chu thich. Continuous luu lop bu THEO TRANG
            // (setForeignAnnotLayer(pgF,...)) va nhieu trang cung song → dung tam de
            // quyet discard se VUT layer cua trang vua chu thich khi tam lech trang
            // (cung ho pageAtCenter 31/08: "trang dang xem khong ai ve"). Single van
            // discard khi doi trang (chi mot trang hien thoi).
            if (t->currentPage != pgF && !(m_fastMode && m_continuousView)) return;
            if (t != currentTab()) return;
            if (wf->result()) {
                t->fgnLayer = fl;
                if (t->view) t->view->setForeignAnnotLayer(fl);
                // C2 (owner chot 2026-08-30): day lop bu xuong ContinuousView — Single
                // chi ve markup qua lop bu, Continuous phai lam DUNG NHU vay.
                if (m_fastMode && m_continuousView && t == currentTab())
                    m_continuousView->setForeignAnnotLayer(pgF, fl);
            } else {
                // LỖI 3 0902 (GIU LOP — owner): "KHONG tao duoc gia tri moi" ≠ "gia tri cu SAI".
                // Luot dung that bai do tranh chap/huy (build() tra false) o day truoc day VUT
                // luon lop dang hien thi tot va khong ai dat lenh dung lai → chu thich bien
                // mat den khi su kien khac vo tinh kich (do: 4/23 luot bi vat, "[fgnlayer]
                // set page= -1"). Phan biet hai ca bang pageRevision (moi mutation cua annot
                // deu bump — AnnotationManager::bumpPageRevision):
                //   • NOI DUNG KHONG DOI → lop cu van DUNG (cham nhat la thieu net o zoom
                //     moi — thà cũ còn hơn mất) → GIU NGUYEN, khong cham vao view.
                //   • NOI DUNG DA DOI giua luot dung (rev khac) → lop cu SAI → BAT BUOC vat
                //     (rang buoc 4). invalidateAnnotPage da vat t->fgnLayer + Single view
                //     khi no bump rev, con lop THEO TRANG cua Continuous thi no khong cham
                //     → vat tai day, hep dung trang pgF.
                // Ca hai ca deu danh dau fgnDeferred ( cung co che voi DEFER khoa ban o
                // 1867) de SU KIEN KE TIEP nhet len: continuousPageReady (~4694) dut qua
                // fgnDeferred goi lai ensureForeignAnnotLayer; Single co zoomChanged (6016)
                // va doi trang (5858) goi truc tiep — dieu kien "!hasLayer || needRebuild"
                // van con dung nen se dung lai. KHONG timer, KHONG polling.
                const bool contentChanged =
                    t->annotMgr && t->annotMgr->pageRevision(pgF) != revAtStart;
                t->fgnDeferred.insert(pgF);
                if (contentChanged) {
                    if (t->fgnLayer && t->fgnLayer->pageIndex() == pgF) {
                        t->fgnLayer.reset();
                        if (t->view) t->view->setForeignAnnotLayer(nullptr);
                    }
                    if (m_fastMode && m_continuousView && t == currentTab())
                        m_continuousView->setForeignAnnotLayer(pgF, nullptr);
                    qDebug().noquote() << "[fgnlayer] build FAIL page=" << pgF
                                       << "- noi dung doi → vat lop cu, DEFER dung lai";
                } else {
                    qDebug().noquote() << "[fgnlayer] build FAIL page=" << pgF
                                       << "- GIU lop cu, DEFER cho su kien ke tiep dung lai";
                }
            }
        });
        FPDF_DOCUMENT df = t->doc->raw();
        t->fgnPending = fl;
        // 🔴 LƯỢT 22: token lúc SPAWN + bgSync — fgnFuture là SLOT, hai trang build
        // song song thì cái cũ bị ghi đè và cancelForeignAnnotTasks không chờ nó.
        trdoc::Task task(df, "fgnlayer/build");
        t->fgnFuture = QtConcurrent::run([fl, df, pgF, maxPx, task = std::move(task)]{
            return fl->build(df, pgF, maxPx);
        });
        wf->setFuture(t->fgnFuture);
        t->addBgWait(t->fgnFuture);   // L22/L22b
    }
}

// Huy + cho xong moi tac vu lop bu (ForeignAnnotLayer) DANG THAM CHIEU toi
// FPDF_DOCUMENT sap bi dong/giai phong. Phai goi TRUOC khi dong doc — neu khong tac vu
// nen con so huu df chet, pdfium ghi vao vung da giai phong => CRASH (30/08 16:53:21,
// pdfium.dll 0xc0000005). Co co huy nen cho xong rat nhanh, khong dong bang UI.
void MainWindow::cancelForeignAnnotTasks(DocTab* t) {
    if (!t) return;
    if (t->fgnPending) {
        const int pg = t->fgnPending->pageIndex();
        t->fgnPending->cancel();
        qDebug().noquote() << "[fgnlayer] CANCEL page=" << pg
                           << "(huy vi tai lieu dang dong)";
    }
    if (t->fgnLayer) t->fgnLayer->cancel();
    if (t->fgnFuture.isValid()) t->fgnFuture.waitForFinished();
    if (t->fgnRegionFuture.isValid()) t->fgnRegionFuture.waitForFinished();
    // LAT C: tac vu buildRegion cua che do trang nang cung nam con tro FPDF_DOCUMENT —
    // phai huy + cho xong TRUOC khi doc bi dong (ngu crash 30/08 nhu lop bu toan trang).
    if (t->heavyRegionCancel) t->heavyRegionCancel->storeRelease(1);
    if (t->heavyRegionFuture.isValid()) t->heavyRegionFuture.waitForFinished();
    t->heavyRegionCancel.reset();
    t->heavyRegionFuture = QFuture<bool>();
    t->heavyRegionBuilding = false;  // watcher co the chua kip chay khi UI thread busy
    t->heavyRegionPage = -1;
    // LAT G: tac vu da xong (waitForFinished) => main thread nam handle rieng, dong an toan
    // TRUOC khi doc bi dong/giai phong (neu khong handle tro toi doc da chet = crash/ro ri).
    closeHeavyPriv(t);
    t->heavySeenPage = -1; t->heavySeenZoom = 0.0; t->heavySeenVis = QRect();
    t->fgnPending.reset();
}

// 0903 — VĂNG APP KHI ĐÓNG TAB. Worker cua ThumbnailRenderPool render bang CUNG
// FPDF_DOCUMENT voi doc va xuong PDFium tren luong rieng. Chung cua with net PDFium
// tren UI thread (dong view, mo tab Welcome, clearCache) = STATUS_BREAKPOINT trong
// pdfium.dll. close() = stop() + wait() → worker thoat HAN truoc khi tiep tuc don doc.
// Chong song giua worker va cac net PDFium tren UI thread chinh la nguyen nhan crash.
// wait() CO BUOC (~<=100ms): stop() don hang doi va run() break ngay khi m_stop,
// khong drain pending; worker chi con xong 1 slice render (~50ms) dang chay.
void MainWindow::stopThumbPool(DocTab* t) {
    if (t && t->thumbPool) t->thumbPool->close();
}

bool MainWindow::canFastPath(DocTab* t, int page) const {
    // 🔴 2026-09-01: dung co RIENG, KHONG dung `overlayCapablePage` (co do con phuc vu
    // viec khac). "Duong tat" chi hop le khi overlay ve HET — con mot annot khong ve duoc
    // thi van phai dung LOP BU, neu khong chu thich ngoai se khong ai ve tren nen vector.
    return t->overlayVeHetPage.value(page, t->overlayCapablePage.value(page, false));
}

bool MainWindow::baseIsVector(DocTab* t, int page) const {
    // LÁT B 09/02: kho chung khoa theo trang, nen "layer->pageIndex()==page" la THUA —
    // lay dung value(page) la ra lop cua trang do (neu co).
    const auto L = t ? t->vecLayers.value(page) : nullptr;
    return L && L->isReady() && L->isComplete();
}

// NEN lop vector cua trang dang xet. LÁT B 09/02: Single va Continuous CUNG doc mot kho
// DocTab::vecLayers nen baseIsVector(da doc kho) la du; chot Continuous duoi day chi con
// la mao hiem (doc dung cai hash ay qua con tro cua view), giu lai cho an toan.
// Thieu chot nay tung la goc lo "Comment mat" (30/08): Continuous DEFER vo han vi
// baseVector=false, mac du lop vector cua no da san sang de ve.
bool MainWindow::pageBaseIsVector(DocTab* t, int page) const {
    if (baseIsVector(t, page)) return true;
    if (m_fastMode && m_continuousView)
        return m_continuousView->pageHasVector(page);
    return false;
}

// Trang co markup khong overlay duoc (canFastPath false) thi phai ve bang raster: lop
// vector dung tu page object KHONG chua annotation, nen giu lop vector lam nen => raster
// bi chan (drop reason=vectorReady) va markup VO HINH. Danh dau + go lop vector ra ngay.
void MainWindow::forceRasterPage(DocTab* t, int pg) {
    if (!t) return;
    if (canFastPath(t, pg)) return;   // overlay capable: lop vector van dung duoc
    // LAT C 09/02: trang nang dang o che do "nen vector + chu thich region" (lop bu toan
    // trang van BI CAM vi tran 400000 o ensureForeignAnnotLayer) — chu thich do duong
    // buildRegion ve, nen KHONG duoc vat bo nen vector nua.
    if (t->heavyRegionPages.contains(pg)) return;
    t->forceRasterPages.insert(pg);
    if (baseIsVector(t, pg))
        setTabVectorLayer(t, nullptr, pg);
}

// LÁT C 09/02 — lop chu thich TRONG SUOT cho VUNG NHIN cua trang nang nen vector.
// O VUONG TUNG annot, clip dung o — cong thuc --annotroi-bench (do tren chinh
// trang 2,54M object: 18,6 ms/o): clip khien PDFium chi duyet noi dung trong o
// chu thich, khong phai toan trang. Gao nen trang (white-key nhu
// ForeignAnnotLayer::build) va blit vao anh trong suot theo vung region.
// OwnAnnotHideGuard(overlayOnly=true) an cac annot mà overlay tu ve ⇒ khong
// ve trung. KHONG dung buildRegion: no render CA VUNG NHIN — trang CAD chen
// duc net ⇒ do 4s+7s giu khoa. KHONG goi build(): lop bu toan trang van bi
// tran 400000 cam. Khoa tha giua cac o (moi o <25ms) de UI xen vao duoc.
static QImage renderAnnotRegion(FPDF_PAGE priv, int n, int pageIndex, double scale,
                                const QRect& regionPx, QAtomicInt& cancel)
{
    QImage out(regionPx.size(), QImage::Format_ARGB32);
    if (out.isNull()) return QImage();
    out.fill(Qt::transparent);
    const int margin = 4;   // px le quanh o chu thich — khop build()/--annotroi-bench
    // LAT E 09/02 — handle RIENG, KHONG dung handle CHIA SE cua PageCache (chong crash
    // 0x80000003: render dong bo de len handle PageCache dang tam dung giua slice).
    // LAT G 09/02 — handle nay duoc GOI VAO DA NAP SAN va duoc DocTab GIU LAI (xem
    // heavyPrivPage): FPDF_LoadPage tren trang 2,54M object ton 2,0-2,1 giay MOI LAN,
    // nen nap mot lan roi dung lai. Ham nay chi ve, khong nap, khong dong handle.
    if (!priv || n < 0) return QImage();
    qint64 inkPx = 0;
    int drawn = 0;
    QElapsedTimer tAll; tAll.start();
    bool abort = false;
    for (int i = 0; i < n; ++i) {
        if (cancel.loadAcquire()) { abort = true; break; }
        QImage patch;
        int dx = 0, dy = 0;
        {
            TimedPdfiumLock lk(__FILE__, __LINE__);
            OwnAnnotHideGuard hide(priv, true, /*overlayOnly=*/true);
            FS_RECTF r{};
            FPDF_ANNOTATION a = FPDFPage_GetAnnot(priv, i);
            if (!a) continue;
            const bool keep = !(FPDFAnnot_GetFlags(a) & FPDF_ANNOT_FLAG_HIDDEN)
                           && FPDFAnnot_GetSubtype(a) != FPDF_ANNOT_POPUP
                           && FPDFAnnot_GetRect(a, &r);
            FPDFPage_CloseAnnot(a);
            if (!keep) continue;
            const int rot   = FPDFPage_GetRotation(priv) & 3;
            const int fullW = qMax(1, int(FPDF_GetPageWidth(priv)  * scale));
            const int fullH = qMax(1, int(FPDF_GetPageHeight(priv) * scale));
            int cx1, cy1, cx2, cy2;
            FPDF_PageToDevice(priv, 0, 0, fullW, fullH, rot, r.left,  r.top,    &cx1, &cy1);
            FPDF_PageToDevice(priv, 0, 0, fullW, fullH, rot, r.right, r.bottom, &cx2, &cy2);
            const int ax = qMin(cx1, cx2), ay = qMin(cy1, cy2);
            const int aw = qMax(1, qAbs(cx2 - cx1)), ah = qMax(1, qAbs(cy2 - cy1));
            if (QRect(ax, ay, aw, ah).intersected(regionPx).isEmpty()) continue;  // ngoai vung nhin
            patch = QImage(aw + 2 * margin, ah + 2 * margin, QImage::Format_ARGB32);
            if (patch.isNull()) { abort = true; break; }
            patch.fill(Qt::white);   // PDFium se ve de len nen trang + chu thich
            FPDF_BITMAP bmp = FPDFBitmap_CreateEx(patch.width(), patch.height(), FPDFBitmap_BGRA,
                                                  patch.bits(), patch.bytesPerLine());
            if (!bmp) continue;
            FPDF_RenderPageBitmap(bmp, priv, margin - ax, margin - ay,
                                  fullW, fullH, rot, FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE);
            FPDFBitmap_Destroy(bmp);
            dx = ax - margin - regionPx.left();
            dy = ay - margin - regionPx.top();
        }
        // Blit ngoai khoa: gao nen trang tinh, giu muc chu thich (nhu blitWhiteKey).
        for (int y = 0; y < patch.height(); ++y) {
            const int ty = dy + y;
            if (ty < 0 || ty >= out.height()) continue;
            const QRgb* rs = reinterpret_cast<const QRgb*>(patch.constScanLine(y));
            QRgb*       dr = reinterpret_cast<QRgb*>(out.scanLine(ty));
            for (int x = 0; x < patch.width(); ++x) {
                const int tx = dx + x;
                if (tx < 0 || tx >= out.width()) continue;
                const QRgb p = rs[x];
                if (qRed(p) >= 250 && qGreen(p) >= 250 && qBlue(p) >= 250) continue;
                dr[tx] = qRgba(qRed(p), qGreen(p), qBlue(p), 255);
                ++inkPx;
            }
        }
        ++drawn;
    }
    // LAT G 09/02: KHONG dong handle o day — handle rieng do DocTab giu lai de dung
    // lai (dong khi doi trang / dong tab / dong tai lieu / noi dung doi).
    qDebug().noquote() << "[heavyroi] REGION page=" << pageIndex
                       << "rect=" << regionPx << "scale=" << scale
                       << "annots=" << n << "drawn=" << drawn << "inkPx=" << inkPx
                       << "ms=" << tAll.elapsed();
    if (inkPx == 0 || abort) return QImage();   // khong co gi de chong — tra rong de lan sau dung lai
    return out;
}

// LAT G 09/02 — dong handle rieng cua heavy region. CHI goi khi KHONG co tac vu build
// dang chay (heavyRegionBuilding == false) hoac sau waitForFinished, vi luc do main
// thread la chu duyet duy nhat cua heavyPrivPage.
void MainWindow::closeHeavyPriv(DocTab* t) {
    if (!t || !t->heavyPrivPage) return;
    TimedPdfiumLock lk(__FILE__, __LINE__);
    // [closeorder] LƯỢT 7: đi qua sổ khai thác — heavyPrivPage là handle thứ HAI của
    // cùng (doc,trang) ngoài PageCache, nên phải được tính vào sổ, nếu không thì
    // "PAGE-OPEN == PAGE-CLOSE" của PageCache không phản ánh hết handle thật.
    PdfCloseTrace::closePage(t->doc ? t->doc->raw() : nullptr, t->heavyPrivPage,
                             "MainWindow::closeHeavyPriv");
    t->heavyPrivPage  = nullptr;
    t->heavyPrivIndex = -1;
    t->heavyPrivStale = false;
}

// LÁT C 09/02 — chu thich cho trang nang nen vector, ve theo VUNG NHIN.
// PdfGpuView setVectorAnnotSafe(true) da bat nen vector; con chu thich do
// setForeignAnnotRegion() chong len: anh TRONG SUOT do renderAnnotRegion() sinh
// (pixel THAT do PDFium ve — dung phông, dung nền), khong phai QPainter phang.
// GOI THEO SU KIEN (khong con dong ho polling 1s — no gay vang app tren may owner):
// UpdateRequest (pan/repaint), zoomChanged, doi trang, invalidateAnnotPage.
void MainWindow::updateHeavyRegion(DocTab* t) {
    if (!t || !t->view || !t->doc || !t->doc->isOpen()) return;
    if (!m_openDocs.contains(t) || t != currentTab()) return;
    const int pg = t->currentPage;
    if (!t->heavyRegionPages.contains(pg)) return;
    if (t->heavyRegionBuilding) return;
    const auto L = t->vecLayers.value(pg);
    if (!L || !L->isReady() || !L->isComplete()) return;  // chua co nen: raster dang lo, co chu thich san
    const double zoom = t->view->zoom();
    const QSizeF szPt = L->pageSizePt();
    if (zoom <= 0.01 || szPt.isEmpty()) return;
    // Vung nhin ∩ trang quy theo px cua trang tai scale=zoom — cung thu tuc voi
    // PdfGpuView::requestTiles (widgetToPdf = (wp - pageOrigin()) / zoom).
    const QPointF a = t->view->widgetToPdf(QPointF(0, 0));
    const QPointF b = t->view->widgetToPdf(QPointF(t->view->width(), t->view->height()));
    QRectF visPt(QPointF(qMin(a.x(), b.x()), qMin(a.y(), b.y())),
                 QPointF(qMax(a.x(), b.x()), qMax(a.y(), b.y())));
    visPt = visPt.intersected(QRectF(QPointF(0, 0), szPt));
    if (visPt.isEmpty()) return;
    QRect vis(qRound(visPt.left() * zoom), qRound(visPt.top() * zoom),
              qMax(1, qRound(visPt.width() * zoom)), qMax(1, qRound(visPt.height() * zoom)));
    // LAT G 0902 — CHOT VUNG NHIN THAT SU: su kien (UpdateRequest/zoom) co the den
    // lien tuc. Neu vung nhin KHONG doi va KHONG bi invalidate (invalidateAnnotPage
    // reset heavySeenPage=-1) thi thoat NGAY — khong lay khoa, khong mo handle, khong
    // dung region. Do: 9 lan dung region/4 phut khi khong ai dong vao. Danh dien nay
    // moc truc tiep vung nhin hien tai.
    if (t->heavySeenPage == pg && qAbs(t->heavySeenZoom - zoom) < 1e-9
        && t->heavySeenVis == vis) return;
    t->heavySeenPage = pg; t->heavySeenZoom = zoom; t->heavySeenVis = vis;
    // Da co anh o DUNG zoom nay phu vung nhin ⇒ giu, khong dung lai.
    if (t->heavyRegionPage == pg && qAbs(t->heavyRegionScale - zoom) < 1e-6
        && t->heavyRegionPx.contains(vis)) return;
    // Dung thua 1/4 moi be: pan nhe khong khoi dung lai tu dau.
    const QRect pagePx(0, 0, int(szPt.width() * zoom), int(szPt.height() * zoom));
    QRect build = vis.adjusted(-vis.width() / 4, -vis.height() / 4,
                                vis.width() / 4, vis.height() / 4)
                      .intersected(pagePx);
    if (build.isEmpty()) build = vis;
    t->heavyRegionBuilding = true;
    if (!t->heavyRegionCancel) t->heavyRegionCancel = std::make_shared<QAtomicInt>(0);
    auto cancel = t->heavyRegionCancel;
    cancel->storeRelease(0);
    FPDF_DOCUMENT d = t->doc->raw();
    qDebug().noquote() << "[heavyroi] dung region page=" << pg << "scale=" << zoom
                       << "rect=" << build;
    auto img = std::make_shared<QImage>();
    auto* w = new QFutureWatcher<bool>(this);
    connect(w, &QFutureWatcher<bool>::finished, this, [this, w, t, img, pg, zoom, build, s = t->serial]{
        w->deleteLater();
        if (!tabAlive(t, s)) return;   // 🔴 L20: chốt TRƯỚC khi dereference (tab đã đóng)
        t->heavyRegionBuilding = false;
        if (t->heavyRegionFuture.isValid()) t->heavyRegionFuture = QFuture<bool>();
        if (t->currentPage != pg || !t->view) {
            // LAT G: da roi trang nay trong luc dang dung — handle rieng khong con can,
            // dong ngay (main thread, tac vu da xong => khong con tranh chap).
            closeHeavyPriv(t);
            return;
        }
        // LAT E 09/02 (UU TIEN 3): anh rong = trong vung KHONG con chu thich nao (da xoa
        // het). Ban cu `return` o day ⇒ anh vung CU (chua chu thich da xoa) van nam trong
        // view, nen vector van pureVector ⇒ "xoa text/Note thi anh van luu lai". Phai XOA.
        // Van gan heavyRegionPage = pg de lan goi ke tiep dung han (khong dung lai rong);
        // moi lan noi dung doi, invalidateAnnotPage dat no ve -1 ⇒ dung lai.
        const bool empty = !w->result() || img->isNull();
        t->heavyRegionPage  = pg;
        t->heavyRegionScale = zoom;
        t->heavyRegionPx    = build;
        if (empty) {
            // ponytail: PdfGpuView::clearForeignAnnotRegion (ban latABCDEFG) khong nam
            // trong pham vi sua (chi MainWindow) — anh TRONG SUOT 1px tuong minh:
            // setForeignAnnotRegion tu choi anh null, paint khong in hong nao.
            QImage blank(1, 1, QImage::Format_ARGB32);
            blank.fill(Qt::transparent);
            t->view->setForeignAnnotRegion(pg, zoom, QRect(), blank);
        } else {
            t->view->setForeignAnnotRegion(pg, zoom, build, *img);
        }
    });
    // LAT G 09/02 — handle rieng NAP MOT LAN, dung lai. Chi FPDF_LoadPage khi chua co
    // handle cho dung trang nay va chua bi danh dau stale. Do: 2,0-2,1 giay LAN DAU TIEN
    // cho trang 2,54M object; cac lan sau TAI SU DUNG => 0 lan LoadPage.
    // 🔴 LƯỢT 22: token lúc SPAWN + bgSync (heavyRegionFuture là slot — watcher ở
    // 2623 clear nó, spawn mới đè lên). Thân task VẪN đọc/ghi t->heavyPriv* — an
    // toàn vì `delete t` (closeJob) chạy SAU shutdownTab, mà shutdownTab chờ bgSync
    // HẾT — không còn cảnh "chờ cái mới nhất, task cũ chạm t đã free".
    trdoc::Task task(d, "heavyRegion");
    t->heavyRegionFuture = QtConcurrent::run([this, t, img, d, pg, zoom, build, cancel,
                                              task = std::move(task)]{
        FPDF_PAGE priv = nullptr;
        int n = -1;
        {
            TimedPdfiumLock lk(__FILE__, __LINE__);
            if (t->heavyPrivStale || t->heavyPrivIndex != pg || !t->heavyPrivPage) {
                if (t->heavyPrivPage) {
                    PdfCloseTrace::closePage(d, t->heavyPrivPage, "MainWindow::updateHeavyPriv");
                    t->heavyPrivPage = nullptr;
                }
                QElapsedTimer lt; lt.start();
                t->heavyPrivPage = FPDF_LoadPage(d, pg);
                t->heavyPrivIndex = pg;
                t->heavyPrivStale = false;
                // 0927 LƯỢT 8: handle thứ HAI của cùng (doc, trang) ngoài PageCache —
                // phải có cả MỞ lẫn ĐÓNG trong sổ khai thác, nếu không thì sổ vẫn
                // lệch và không phân biệt được "còn ai giữ" với "đã đóng hai lần".
                if (t->heavyPrivPage)
                    PdfCloseTrace::openPage(d, t->heavyPrivPage, pg, "MainWindow::updateHeavyPriv");
                qDebug().noquote() << "[heavyroi] NAP handle rieng page=" << pg
                                   << "ms=" << lt.elapsed();
            }
            priv = t->heavyPrivPage;
            if (priv) n = FPDFPage_GetAnnotCount(priv);
        }
        *img = renderAnnotRegion(priv, n, pg, zoom, build, *cancel);
        return !img->isNull();
    });
    w->setFuture(t->heavyRegionFuture);
    t->addBgWait(t->heavyRegionFuture);   // L22: mọi lần spawn, không chỉ cái cuối — L22b: có dọn
}

// R2 (SPEC_PERF_HEAVYPAGE): lop vector san sang cho trang hien tai thi raster full-quality
// la viec vo ich (drawPageBase se bo qua khi pureVector). Cac vi tri gan lop vector cho view
// phai di qua ham nay de: (1) huy lenh full-quality dang chay, (2) chan lenh moi cho trang do.
// Trang khong dung duoc vector / lop bi bo / trang doi => bo chan, raster lam viec binh thuong.
void MainWindow::setTabVectorLayer(DocTab* t, std::shared_ptr<VectorLayer> layer, int pg) {
    // Trang buoc dung raster (co markup khong overlay duoc) thi KHONG duoc lay lop vector
    // lam nen — se che mat markup. Ep ve duong raster: lop rong => baseIsVector false
    // => bo chan raster va go setSuppressFullQuality(pg, false) o duoi.
    if (t && t->forceRasterPages.contains(pg))
        layer.reset();
    // LÁT B 09/02: ghi vao KHO CHUNG theo trang — khong con mot slot bi ghi de.
    // layer rong = XOÁ trang do khoi kho (baseIsVector pg se false).
    if (layer) t->vecLayers.insert(pg, layer);
    else       t->vecLayers.remove(pg);
    if (layer) {
        qDebug().noquote() << "[vecdiag] page=" << layer->pageIndex()
                           << "ready=" << layer->isReady()
                           << "complete=" << layer->isComplete()
                           << "verts=" << layer->verts().size()
                           << "colors=" << layer->colors().size()
                           << "widths=" << layer->widths().size()
                           << "depths=" << layer->depths().size()
                           << "clipIdx=" << layer->clipIdx().size()
                           << "fillVerts=" << layer->fillVerts().size()
                           << "fillColors=" << layer->fillColors().size()
                           << "fillDepths=" << layer->fillDepths().size()
                           << "fillClipIdx=" << layer->fillClipIdx().size()
                           << "fillOpaqueFloats=" << layer->fillOpaqueFloats()
                           << "clips=" << layer->clips().size()
                           << "texts=" << layer->textTiles().size()
                           << "images=" << layer->imageTiles().size()
                           << "pageSizePt=" << layer->pageSizePt()
                           << "rotation=" << layer->rotation()
                           << "uid=" << layer->uid();
    } else {
        qDebug().noquote() << "[vecdiag] page=" << pg << "layer=NULL";
    }
    if (t->view) t->view->setVectorLayer(layer);
    if (!t->renderer) return;
    // EVICT (LÁT B 09/02) — chinh sach cua ContinuousView nay danh thang vao kho chung,
    // nen Single cung phai duoi: giu toi da 3 trang quanh trang chinh (ban kinh 1),
    // bo trang vua gan ra ngoai le. Thieu chot nay la kho phinh RAM (mot trang ~201 MB).
    for (auto it = t->vecLayers.begin(); it != t->vecLayers.end(); ) {
        if (it.key() != pg && qAbs(it.key() - t->currentPage) > 1) {
            t->renderer->setSuppressFullQuality(it.key(), false);
            it = t->vecLayers.erase(it);
        } else {
            ++it;
        }
    }
    if (!m_fastMode && baseIsVector(t, pg)) {
        qDebug().noquote() << "[perf] drop page=" << pg << "reason=vectorReady (single)";
        t->renderer->cancelFullQuality();
        t->renderer->setSuppressFullQuality(pg, true);
    } else {
        t->renderer->setSuppressFullQuality(pg, false);
    }
    // R3 (SPEC_PERF_HEAVYPAGE muc R3.2): ban dung trang hien tai da nga ngu xong
    // (lop vector thanh cong HOAC bo cuoc) — thumbnail co the chay lai PDFium.
    if (t->thumbPool && t == currentTab()) t->thumbPool->setRenderPaused(false);
    // Trang hien ra bang LOP VECTOR (khong co pageReady vi R2 chan raster). Van phai
    // lam day du viec "trang da hien": link center, visuals, to ket qua tim, thanh
    // trang thai. Thieu no la nguon goc cua 3 loi ngay 2026-08-19.
    if (baseIsVector(t, pg) && pg == t->currentPage) {
        finishPageDisplay(t, pg);
        // R3: lop vector da san sang => ve luon thumbnail cua trang nay bang GPU thay vi
        // de PDFium duyet lai 2,54 trieu path (do 2026-08-19: 5.355 ms/thumbnail).
        // 🔴 LƯỢT 36 (mục 2): renderVectorThumbnail chay tren UI (do r36: 339–457 ms/lan,
        // 20 lap buoc N = dong than freeze 777 ms). .torcache_da co anh ⇒ worker phat tu
        // dia, UI khong can ve nua — SKIP.
        if (t->view && t->thumbPool && t == currentTab()) {
            if (t->thumbPool->hasCachedThumb(pg))
                qDebug().noquote() << "[thumb] SKIP GPU thumbnail — .torcache da co page=" << pg;
            else {
                const QImage vt = t->view->renderVectorThumbnail(ThumbnailRenderPool::kThumbScale);
                if (!vt.isNull()) t->thumbPool->insertThumbnail(pg, vt);
            }
        } else if (t != currentTab()) {
            qDebug().noquote() << "[thumb] SKIP GPU thumbnail — tab KHONG hien hanh page=" << pg;
        }
    }
}

// Cac viec phai lam KHI TRANG DA HIEN RA, bat ke no hien bang raster hay bang lop
// vector. Duong raster goi tu handler pageReady; duong vector goi tu setTabVectorLayer.
void MainWindow::finishPageDisplay(DocTab* t, int idx) {
    if (!t || !m_openDocs.contains(t) || !t->doc || !t->doc->isOpen()) return;
    if (idx != t->currentPage) return;
    if (t->view) t->view->setPageBoxOrigin(t->doc->pageBoxOriginCached(idx));
    if (t->pendingLinkCenterActive && t->pendingLinkCenterPage == idx)
        applyPendingLinkCenter(t);
    refreshAnnotVisuals(t, t->currentPage);
    if (!t->searchResults.isEmpty())
        applySearchHighlights(t->searchResults, t->searchCurrentIdx);
    statusBar()->showMessage(
        QString("Page %1 / %2").arg(idx + 1).arg(t->doc->pageCount()), 5000);
    emit pageDisplayed(idx);
}

// Chi tab DANG HIEN moi duoc render thumbnail. Do that 2026-08-19 tren phien 2 tai lieu:
// 269 thumbnail, TONG 94.019 ms render, 30 cai render xong roi VUT (`DROPPED-EARLY
// reason=listNotReady`), va CA HAI doc deu bi thumbnail cung luc ke ca tab dang AN.
// Hau qua do duoc: `[lockwait] ms= 4306 at AnnotationManager.cpp:199 main= 1` — luong
// chinh xep hang sau thumbnail cua tab khong ai nhin.
void MainWindow::syncThumbnailPoolsToActiveTab() {
    DocTab* cur = currentTab();
    // 🔴🔴 0928 LƯỢT 16 — THUMBNAIL CHỈ CHO TAB ĐANG XEM. Log r8 (28/09) đo được:
    // tab3 "Phan ngam" đang hiện mà worker thumbnail của MEP (tab nền) vẫn bốc
    // trang 84–87, giữ khoá chung 1,2–2,6 s/trang ([lockhold] ThumbnailRenderPool.cpp
    // :261/:399) ⇒ trang chính tab3 `cont settle timeout` lặp 15 s và đóng tab3 phải
    // chờ 2,9 s mới lấy được khoá để FPDF_CloseDocument ⇒ cửa sổ văng mở toang.
    // Bản 08-31 bỏ tạm-dừng tab nền vì PAUSE làm mất thumbnail (RESUME 0 lần) —
    // ở đây hàng đợi KHÔNG bị xoá, bỏ băng là bốc tiếp, nên tab nền đứng lại mà
    // không mất ô nào. `m_thumbCloseJobs > 0` (LƯỢT 16b: bộ đếm, không phải bool):
    // đang đóng tab ⇒ giữ băng cả tab hiện hành cho tới khi MỌI `delete t` chạy
    // xong (closeJob.finished gọi lại hàm này).
    for (DocTab* t : m_openDocs) {
        if (!t || !t->thumbPool) continue;
        const bool frozen = (t != cur) || m_thumbCloseJobs > 0;
        if (t->thumbPool->setFrozen(frozen) && frozen)
            qDebug().noquote() << "[thumbq] doi tab dong bang tab=" << m_openDocs.indexOf(t);
        if (!frozen && t->thumbPool->isRenderPaused()) {
            t->thumbPool->setRenderPaused(false);
            qDebug().noquote() << "[perf] thumb pool RESUME tab=" << m_openDocs.indexOf(t);
        }
    }
    // Tab nen: huy render TRANG (dat, lam lai duoc).
    for (DocTab* t : m_openDocs) {
        if (!t || t == cur || !t->renderer) continue;
        t->renderer->cancelPending();
        // 🔴 LƯỢT 33b (J): tab nền cũng phải DỪNG annot scan (giữ khoá ~1,7 s/trang
        // nặng) — loadAllStreaming tự chặn ở ranh giới trang, stopScan cắt sớm vòng lặp.
        if (t->annotMgr) t->annotMgr->stopScan();
        qDebug().noquote() << "[perf] huy render tab NEN tab=" << m_openDocs.indexOf(t);
    }
}

// R3 (SPEC_PERF_HEAVYPAGE muc R3.2): tam dung thumbnail khi mo file de trang dang
// xem duoc uu tien dung s_pdfiumMutex. Dong ho an toan tu go tam dung sau 10s bat
// ke ban dung vector co chay/xong hay khong — khong dua vao mot loi goi duy nhat.
// 10s chu khong 3s: da do duoc canh dong ho nha SOM hon luc trang xong (3,0s so voi 5,3s)
// lam thumbnail quay lai giành khoa, ban dung vector cham them ~1,3 giay.
void MainWindow::pauseThumbnails(DocTab* t) {
    if (!t->thumbPool) return;
    // 🔴 SUA 2026-08-31 — GOC CUA "mo tab thu 2, thumbnail cho rat lau".
    // Cu dung 10 giay nay co ly do CHINH DANG khi thumbnail con tranh s_pdfiumMutex voi
    // trang dang xem. Nhung tu khi thumbnail render bang POOL HANDLE chi-doc rieng, no
    // KHONG con tranh o khoa nao ca => tam dung chi con HAI, khong con loi.
    if (t->renderer && t->renderer->hasPoolHandles()) {
        qDebug().noquote() << "[thumb] KHONG tam dung — da co pool handle rieng";
        return;
    }
    t->thumbPool->setRenderPaused(true);
    DocTab* pt = t;
    QTimer::singleShot(10000, this, [this, pt, s = pt->serial]{
        if (!tabAlive(pt, s)) return; // tab da dong — khong cham
        if (pt != currentTab()) return;        // tab an: cu de tam dung, khong ai nhin
        if (pt->thumbPool) pt->thumbPool->setRenderPaused(false);
    });
}

namespace {
struct AnnotVisualsRes {
    QList<AnnotVisual> visuals;
    bool overlayCapable = false;
    bool hasForeign = false;
    bool aborted = false;   // 🔴 LƯỢT 33e (mục 1): BO vì tab nền/khoá bận — danh sách
                            // rỗng này KHÔNG phải dữ liệu của trang, cấm ghi cache.
    bool unloadable = false; // 🔴 LƯỢT 33f (mục 3): fpage null — trang doc hong,
                            // danh dau + DUNG thu lai (hong phai tam thoi).
};
}

void MainWindow::applyAnnotVisuals(DocTab* t, int page,
                                   const QList<AnnotVisual>& visuals,
                                   bool overlayCapable, bool hasForeign) {
    struct _SlotMs {
        QElapsedTimer t; const char* name; int pg;
        _SlotMs(const char* n, int p) : name(n), pg(p) { t.start(); }
        ~_SlotMs() { if (t.elapsed() > 50) qDebug().noquote() << "[slotms]" << name << "ms=" << t.elapsed() << "page=" << pg; }
    };
    _SlotMs _sm("applyAnnotVisuals", page);
    if (!t) return;
    // 🔴 CHOT TAB (SPEC_CONT_MARKUP_TAB_SAFE muc 2): callback bat dong bo cua tab khong
    // hien hanh KHONG duoc dong vao view dung chung. `m_continuousView` la MOT instance
    // cho MỌI tab — onclickback cua tab 1 ve muon ghi trang cua tailieu 1 vao view dang
    // phuc vu tailieu 2 ⇒ trắng màn hình + trắng thumbnail (owner đã gặp).
    const bool tabIsCurrent = (t == currentTab());
    if (!overlayCapable)
        t->pagesNeedGenerate.insert(page);  // fallback path relies on renderer having annots
    if (t->renderer) {
        // 🔴🔴 NGUYEN TAC 2026-09-01 (owner chot): "MARKUP LA LOP RIENG, HOAT DONG DOC LAP".
        //
        // Benh cu: `overlayCapable` la mot phan xet cho CA TRANG — no false khi co BAT KY annot
        // nao overlay khong ve duoc (thuong la annot NGOAI cua phan mem khac). Khi false, thiet
        // ke cu bat MARKUP CUA TA di nho ANH RASTER ve ho. Tuc so phan markup cua ta bi buoc vao
        // annot cua nguoi khac. Ma nen VECTOR khong chua annotation nao ⇒ lop vector dung xong
        // luc nao la markup bay luc do; zoom la thu kich hoat dung vector ⇒ "mat markup khi zoom",
        // o CA Single lan Continuous vi hai ben dung chung co nay.
        //
        // Sua dung goc: TACH DOI TRACH NHIEM, bo phan xet chung cho ca trang.
        //   • markup CUA TA (co TRUID, overlay ve duoc) → LUON do overlay ve, LUON giau o nen.
        //   • annot NGOAI                                → van do nen raster / lop bu lo.
        // Nho vay markup cua ta khong con phu thuoc nen la raster hay vector.
        int nOwnDrawable = 0;
        for (const AnnotVisual& av : visuals) if (av.paintByOverlay) ++nOwnDrawable;
        const bool ownByOverlay = (nOwnDrawable > 0);
        if (ownByOverlay != overlayCapable)
            qDebug().noquote() << "[overlay] TACH DOI page=" << page
                               << "markupCuaTa=" << nOwnDrawable
                               << "overlayCapable(cu)=" << overlayCapable
                               << "hasForeign=" << hasForeign;
        // 🔴🔴 GOC CUA "tao Note/Text xong khong hien gi" — do bang PIXEL, 2026-09-01.
        // `setPageAnnotRender(page, false)` bao bo render: DUNG VE CHU THICH NAO CA.
        // Ban cu dat dieu kien la `hasForeign || !ownByOverlay`: trang co markup cua ta va
        // KHONG co chu thich ngoai ⇒ ca hai ve deu false ⇒ TAT ve chu thich. Nhung Text/Note
        // KHONG do overlay ve — tat di la khong ai ve chung. Do duoc: visuals tang 7→9, trang
        // render lai, ma so pixel doi = 0.
        // ⇒ Dieu kien dung: chi duoc tat khi overlay ve HET. Con MOT thu overlay khong ve thi
        //   nen van phai ve chu thich.
        bool overlayVeHetTrang = overlayCapable;
        for (const AnnotVisual& av : visuals)
            if (!av.paintByOverlay) { overlayVeHetTrang = false; break; }
        t->renderer->setPageAnnotRender(page, hasForeign || !overlayVeHetTrang);
        t->renderer->setPageAnnotOverlay(page, ownByOverlay);
        // 🔴 GO BO 2026-09-01 (owner bac bo, dung): ban truoc ep CA TRANG ve raster khi co chu
        // thich ngoai ⇒ vut mat ban ve vector ⇒ "Continuous mo cam". Single van net VA van hien
        // comment, chung to KHONG can hy sinh nen vector. Comment chi la LOP MONG DE LEN.
        // Duong dung: lop bu `fgnLayer` ve rieng annot ngoai tren nen vector — no truoc gio khong
        // dung duoc chi vi THIEU MOT SOI DAY bao so object (da noi lai o PdfRenderer.cpp).
        // 🔴🔴 2026-09-01, ban CUOI: trang co chu thich NGOAI phai o lai nen RASTER.
        // Ly do: chi PDFium ve dung chu thich (dung /AP: phong chu, nen, vien). Moi ban ve
        // "gan dung" bang QPainter deu ra sai font/sai nen — owner da bat ngay.
        // Lan truoc luat nay bi bo vi Continuous MO CAM, nhung goc cua mo la bo lam net vung
        // nhin bi bo cuoc sau moi lan zoom (m_primaryPage = -1) — DA SUA cung ngay. Nay raster
        // van net vi vung dang nhin duoc render lai o dung muc zoom, dung nhu Single.
        // 🔴 GO BO ban "mo rong" 2026-09-01: tung siet vecSafe thanh "overlay ve HET moi annot".
        // Hau qua: MOI trang co du chi MOT FreeText deu bi tuoc nen vector ⇒ Single mat che do
        // render toan trang bang vector (owner bao ngay). Cai gia qua lon so voi cai duoc.
        // ⇒ Tro ve dieu kien hep: CHI chu thich NGOAI moi buoc trang o lai raster.
        //   Text/Note cua app duoc lo bang duong khac: Note do overlay ve (huy hieu), con
        //   FreeText hien khi trang ve bang raster — va sua markup xong nay DA ep ve lai ngay.
        // 🔴🔴 CHOT 2026-09-01 — LUAT NAY CHI AP CHO CONTINUOUS.
        // Do duoc: heavy.pdf co 9 chu thich ngoai ⇒ luat "co annot ngoai thi bo nen vector"
        // TUOC LUON nen vector cua Single tren chinh tep owner hay dung ⇒ "hu che do render
        // toan trang cua Single". Single truoc gio VAN ON, khong duoc dong vao no.
        // Continuous thi can, vi chinh no la ben owner bao mat comment.
        // Continuous: trang co BAT KY annot nao overlay khong ve (FreeText, Note, chu thich
        // ngoai) thi phai o lai nen raster — vi chi PDFium ve dung chung. Khong con khoa vao
        // rieng `hasForeign`: Text/Note do CHINH APP tao cung can raster y het.
        // 🔴🔴 2026-09-01: AP CUNG LUAT CHO SINGLE (owner: "Single, zoom 75 khong hien markup").
        // O 75% nen Single chuyen sang VECTOR — ma lop vector KHONG chua annotation, con overlay
        // chi ve duoc vai loai. Tep cua owner co 41 chu thich, phan lon overlay khong ve
        // ⇒ trang trang markup. Continuous da co luat nay va owner xac nhan chay tot; Single
        // thieu no.
        // ⚠️ Khac lan truoc (da that bai): lan do toi khoa vao `!hasForeign` — QUA RONG, tuoc
        //    nen vector cua MOI trang co chu thich ngoai ke ca khi overlay ve duoc het. Nay khoa
        //    vao dung cau hoi: "overlay co ve HET moi chu thich cua trang khong?".
        // Single mat nen vector tren trang co chu thich, nhung KHONG mo: Single co duong ve lai
        // vung dang nhin theo dung muc zoom, nen van net.
        // LAT C 09/02: `|| heavyRegionPages` — trang nang den che do "nen vector + chu
        // thich region" thi chu thich do renderAnnotRegion ve, khong phai overlay; giu nen vector.
        // Continuous (dong duoi) KHONG doi: no chua co duong region nen van theo luat cu.
        if (t->view) t->view->setVectorAnnotSafe(page, overlayVeHetTrang || t->heavyRegionPages.contains(page));
        if (m_continuousView && tabIsCurrent)
            m_continuousView->setVectorAnnotSafe(page, overlayVeHetTrang);
    }
qDebug().noquote() << "[overlay] page=" << page << "overlayCapable=" << overlayCapable << "hasForeign=" << hasForeign << "visuals=" << visuals.size();
     if (tabIsCurrent) ensureForeignAnnotLayer(t, page);
     else qDebug().noquote() << "[fgnlayer] SKIP — tab KHONG hien hanh page=" << page;
     if (m_fastMode && m_continuousView) {
         if (!tabIsCurrent) {
             qDebug().noquote() << "[overlay] SKIP continuousView — callback cua tab KHONG hien hanh page=" << page;
         } else {
             // Continuous lam dung nhu Single: lop vector lam NEN + lop bu ve markup DE LEN
             // (SPEC_BO_RASTERONLY 2026-08-30). Khong con ai bat rasterOnly — moi trang deu
             // duoc dung lop vector. 🔴 LUON day DAY DU visuals du xuong — day danh sach RONG
             // khi overlayCapable=false lam mat CA Stamp anh cua TorReader (paintByOverlay=true,
             // le ra ve duoc) lan markup ngoai tren tap markup NGOAI (goc chung cua "Comment
             // mat" + "anh chen khong hien"). ContinuousView tu loc `!av.paintByOverlay` khi ve.
             if (m_fastMode && m_continuousView && tabIsCurrent)
                 m_continuousView->setAnnotVisualsForPage(page, visuals);
         }
     }
     if (overlayCapable) {
        if (t->view) { t->view->setAnnotVisuals(visuals); t->view->clearPendingMarkups(); }
    } else {
        if (t->view) t->view->clearAnnotVisuals();
    }
}

// 🔴🔴 LÁT C 0902 — CẬP NHẬT CÓ CHỌN LỌC (owner: "no bi ai do chiem quyen va phai cho
// xong moi hien"). Chu thich KHONG bao gio hong — no chi LUON THUA: loadPageVisuals
// phai xep hang sau bo dung trang (1–2,4 giay/luot) roi TU om khoa them 2 giay nua de
// phan tich ca trang. Nhung du lieu do DA NAM SAN trong visualsCache (lat A), va cai
// vua tao/xoa da duoc AnnotationManager dung AnnotVisual luc DANH GIU KHOA (µs).
// O day chi tron delta do vao nen — KHONG lay s_pdfiumMutex, KHONG xep hang.
// Phat lai delta theo thu tu la idempotent (thay/xoa theo uid) → ap 2 lan vo hai.
bool MainWindow::mergeAnnotVisuals(DocTab* t, int page, QList<AnnotVisual>* out) {
    if (!t || !t->annotMgr || !t->visualsCache.contains(page)) return false;
    const QList<VisualDelta> deltas = t->annotMgr->visualDeltas(page);
    if (deltas.isEmpty()) return false;
    QList<AnnotVisual> list = t->visualsCache.value(page);
    for (const VisualDelta& d : deltas) {
        int i = -1;
        for (int k = 0; k < list.size(); ++k)
            if (list[k].uid == d.av.uid) { i = k; break; }
        if (d.removed) { if (i >= 0) list.removeAt(i); }
        else if (i >= 0) list[i] = d.av;
        else list.append(d.av);
    }
    t->visualsCache.insert(page, list);
    t->visualsRev[page] = t->annotMgr->pageRevision(page);
    // co veHet/overlayCapable tinh lai TREN DANH SACH (dung dinh nghia cu o 2189/2308);
    // capable chi duoc ha xuong, khong bao gio nang len khi chua nap lai ca trang —
    // false la duong AN TOAN (cua bu ve lo), khong lam mat chu thich.
    bool veHet = true;
    for (const AnnotVisual& av : list) if (!av.paintByOverlay) { veHet = false; break; }
    const bool capable = t->overlayCapablePage.value(page, false) && veHet;
    t->overlayVeHetPage[page] = veHet;
    t->overlayCapablePage[page] = capable;
    if (out) *out = list;
    qDebug().noquote() << "[annot] visuals MERGE page=" << page << "deltas=" << deltas.size()
                       << "n=" << list.size() << "— khong lay khoa, khong cho bo dung trang";
    return true;
}

void MainWindow::refreshAnnotVisuals(DocTab* t, int page) {
    // 0927 LƯỢT 10 (--no-annotscan): KHONG quet chu thich de ve. `loadPageVisuals`
    // (:2970) la noi duy nhat, no FPDFPage_GetAnnot/FORM* tren TAI LIEU CHINH.
    if (trNoAnnotScan()) return;
    if (!t || !t->annotMgr || !t->doc || !t->doc->isOpen()) {
        if (t && t->view) t->view->clearAnnotVisuals();
        return;
    }
    // 🔴 LÁT C 0902: co delta + co nen → tron, ap ngay, KHONG nap lai ca trang.
    QList<AnnotVisual> merged;
    if (mergeAnnotVisuals(t, page, &merged)) {
        applyAnnotVisuals(t, page, merged, t->overlayCapablePage.value(page, false),
                          t->visualsHasForeign.value(page, false));
        return;
    }
    const quint32 rev = t->annotMgr->pageRevision(page);
    if (t->visualsCache.contains(page) && t->visualsRev.value(page, 0xFFFFFFFFu) == rev) {
        // CACHE HIT — giu nguyen dong bo (re, da do tot).
        const QList<AnnotVisual> visuals = t->visualsCache.value(page);
        const bool overlayCapable = t->overlayCapablePage.value(page, false);
        const bool hasForeign     = t->visualsHasForeign.value(page, false);
        qDebug().noquote() << "[perf] visuals CACHE HIT page=" << page
                           << "n=" << visuals.size();
        applyAnnotVisuals(t, page, visuals, overlayCapable, hasForeign);
        return;
    }
    // CACHE MISS — loadPageVisuals NANG chay o QtConcurrent (SPEC_NAV_INSTANT).
    // 🔴 LƯỢT 33e (mục 5 — J, đo r33e): trang CHƯA CÓ ẢNH ⇒ overlay chưa có gì để
    // vẽ đè lên — HOAN việc quét visuals đến khi ảnh tới. Đường báo lại đã có sẵn
    // cho cả hai chế độ: Continuous ACCEPT ⇒ needAnnotVisuals ⇒ refresh
    // (~:989-1004); Single pageReady ⇒ finishPageDisplay ⇒ refresh (~:2963).
    // Đo r33e J: loadPageVisuals(3) chen FPDF_LoadPage trang quái vật (1701 ms)
    // vào đúng cửa sổ tab mới đang mở — tại thời điểm trang 3 còn CHƯA có ảnh nào.
    // 🔴 LƯỢT 39b: cổng cũ áp dụng TOT CẢ trang => normal pages (< 100k obj)
    // mà raster cache bị evict VẪN bị defer vô hạn nếu không có vector layer.
    // Fix: Chỉ defer nếu trang là MONSTER (≥100k obj). Regular pages: rebuild ngay.
    const bool isMonster = t->renderer && t->renderer->pageObjectCount(page) >= 100000;
    const bool pageIsCurrentAndVecReady = (page == t->currentPage && t->vecLayers.contains(page));
    const bool rasterCached = t->renderer && !t->renderer->bestCachedForPage(page).isNull();
    const bool continuousImage = m_fastMode && m_continuousView && m_continuousView->hasPageImage(page);

    // Regular pages (< 100k): rebuild ngay khi displayed (vector/raster/Continuous)
    // Monster pages (≥100k): chỉ defer để tránh khoá lâu khi đang render
    const bool coAnh = rasterCached || continuousImage || pageIsCurrentAndVecReady || !isMonster;
    if (!coAnh) {
        qDebug().noquote() << "[perf] visuals HOAN page=" << page
                           << "— MONSTER trang chua co anh, quet khi anh toi";
        return;
    }
    // 🔴 LƯỢT 33f (mục 3 — reviewer): trang đã chốt KHÔNG ĐỌC ĐƯỢC (fpage null) —
    // đừng spawn nữa; vòng hẹn 33e lặp vô hạn trên trang hỏng.
    if (t->visualsUnreadable.contains(page)) {
        qDebug().noquote() << "[perf] visuals BO page=" << page << "— trang khong doc duoc";
        return;
    }
    // 🔴 LƯỢT 33f (muc 1 — do full probe r33f): trang NANG (≥100k object — cung
    // nguong kHeavyObjectThreshold cua PdfRenderer) parse MAIN-DOC den 1,65 s DEM
    // KHOA — do: `[annot] AnnotationManager.cpp:1582 giu ms= 1651` (page 3, count= 0
    // — KHONG co chu thich nao!) dung luc flip trang ke `lockwait ms= 1651` ⇒ page 4
    // tu 1135 (r33d) len 2695-3101 ms; page 5/6 (300k obj, scan 285 ms) cung lech
    // 1500. GOC: duong HOAN 33e danh thuc luot quet DUNG LUC ANH TOI = dung luc user
    // flip ⇒ scan va flip tranh nhau MOT khoa. Trang nang: HOAN THEM 2 s — user con
    // o trang thi quet (khong con flip tranh); flip roi thi bo (quay lai se quet
    // tiep). Trang nhe (f1 cua M: 1-10k obj) giu nguyen duong 33e — overlay den ngay.
    if (t->renderer && t->renderer->pageObjectCount(page) >= 100000
        && !t->visualsHeavyDue.contains(page)) {
        t->visualsHeavyDue.insert(page);
        qDebug().noquote() << "[perf] visuals HOAN 2s page=" << page
                           << "— trang quai vat, de flip xong moi quet";
        QTimer::singleShot(2000, this, [this, t, s = t->serial, page] {
            if (!tabAlive(t, s)) return;
            t->visualsHeavyDue.remove(page);
            if (t == currentTab() && t->currentPage == page)
                refreshAnnotVisuals(t, page);
        });
        return;
    }
    // Trang dang co rescan chay roi thi khong bam them lan nua.
    if (t->visualsScanning.contains(page)) {
        qDebug().noquote() << "[perf] visuals rescan in-flight page=" << page << "— skip duplicate";
        return;
    }
    t->visualsScanning.insert(page);
    QElapsedTimer _rescanTimer; _rescanTimer.start();
    AnnotationManager* mgr = t->annotMgr.get();
    // 🔴 LƯỢT 33f (muc 1): trang NANG (≥100k obj — cung nguong tren) scan dem khoa
    // 0,3-1,65 s — loadPageVisuals se BO CO CO khi co render dang giu bo dem (chan
    // flip ke tiep; do r33f page 4: 1135→2695 ms, page 5/6: 285 ms, count= 0!).
    const bool heavyPage = t->renderer && t->renderer->pageObjectCount(page) >= 100000;
    auto res = std::make_shared<AnnotVisualsRes>();
    // 🔴 LƯỢT 22: token lúc SPAWN — `mgr->document()` chốt trên luồng UI, không đọc
    // mgr đã free trên nền. annotVisualsFuture là SLOT bị ghi đè theo trang
    // (visualsScanning chặn trùng theo từng trang) ⇒ bgSync giữ HẾT để shutdownTab chờ.
    trdoc::Task task(mgr ? mgr->document() : nullptr, "annotVisuals");
    t->annotVisualsFuture = QtConcurrent::run([mgr, page, res, heavyPage, task = std::move(task)] {
        res->visuals = mgr->loadPageVisuals(page, &res->overlayCapable, &res->hasForeign,
                                            &res->aborted, &res->unloadable, heavyPage);
    });
    t->addBgWait(t->annotVisualsFuture);   // L22/L22b
    auto* w = new QFutureWatcher<void>(this);
    w->setFuture(t->annotVisualsFuture);
    connect(w, &QFutureWatcher<void>::finished, this,
            [this, w, t, page, rev, res, _rescanTimer, s = t->serial]() mutable {
        w->deleteLater();
        if (!tabAlive(t, s)) return;   // tab dong — khong dong vao bo nho da xoa
        t->visualsScanning.remove(page);
        const AnnotVisualsRes r = *res;
        // 🔴 LƯỢT 33e (mục 1 — reviewer lỗi 1): kết quả ABORTED (tab nền / khoá bận /
        // đọc trang hỏng) KHÔNG được ghi visualsCache/visualsRev. Bản 33d ghi danh sách
        // rỗng kèm rev hiện tại ⇒ tab quay lại thành hiện hành gặp CACHE HIT, áp danh
        // sách rỗng ⇒ markup + chú thích ngoại của trang biến mất vĩnh viễn.
        // Nếu trang này vẫn là thứ người dùng đang xem ⇒ hẹn giờ thử lại (khi đó tab
        // đã thành hiện hành hoặc khoá đã rảnh). Nếu không ⇒ lần đổi tab
        // (onTabChanged → refreshAnnotVisuals) sẽ quét lại.
        if (r.aborted) {
            qDebug().noquote() << "[perf] visuals RESCAN ABORTED page=" << page
                               << "— khong ghi cache";
            // 🔴 LƯỢT 33f (mục 3 — reviewer): vòng hẹn 500 ms của 33e KHÔNG có trần ⇒
            // lặp vô hạn khi trang hỏng (fpage null) hoặc khoá bị giữ lâu. Nay:
            //  • fpage null ⇒ đánh dấu trang KHÔNG ĐỌC ĐƯỢC, dừng thử (trang không
            //    mở nổi thì annot cũng không ghi được — không có gì để quét lại).
            //  • ABORT tạm thời (tab nền / khoá bận): tối đa 5 lần, backoff
            //    500→8000 ms; dem theo trang, xoa dem khi quet thanh cong.
            if (r.unloadable) {
                t->visualsUnreadable.insert(page);
                qDebug().noquote() << "[perf] visuals TRANG KHONG DOC DUOC page=" << page
                                   << "— dung thu lai";
            } else if (t == currentTab() && t->currentPage == page) {
                const int n = t->visualsRetry.value(page, 0);
                if (n < 5) {
                    t->visualsRetry[page] = n + 1;
                    QTimer::singleShot(500 * (1 << qMin(n, 4)), this, [this, t, s, page] {
                        if (tabAlive(t, s) && t == currentTab() && t->currentPage == page)
                            refreshAnnotVisuals(t, page);
                    });
                }
            }
            return;
        }
        t->visualsRetry.remove(page);
        t->visualsUnreadable.remove(page);
        const qint64 rescanMs = _rescanTimer.elapsed();
        qDebug().noquote() << "[perf] visuals RESCAN page=" << page << "ms=" << rescanMs;
        // Ket qua nao cung ghi vao dem (du trang da doi).
        t->visualsCache.insert(page, r.visuals);
        t->visualsRev[page] = rev;
        // 🔴 GO BO 2026-09-01: ban truoc ha `overlayCapablePage` xuong false khi co visual
        // paintByOverlay=false. Y dinh la de lop bu duoc dung, NHUNG co nay con duoc dung o
        // cho khac (dong ~2132) nen ha no xuong lam SINGLE MAT MARKUP. Owner bao ngay.
        // ⇒ Tra co ve nguyen ban. Muon lop bu dung thi phai dung MOT co RIENG, khong duoc
        //   muon co dang phuc vu viec khac — bai hoc dung cua ca ngay hom nay.
        t->overlayCapablePage[page] = r.overlayCapable;
        // Co RIENG: overlay chi thuc su ve HET khi MOI visual deu paintByOverlay.
        // Do duoc tren file that cua owner: overlayCapable=TRUE ma overlay ve 0/1 ⇒ neu tin
        // co cu thi `canFastPath` bao "khong can lop bu" ⇒ chu thich ngoai khong ai ve.
        bool veHet = r.overlayCapable;
        for (const AnnotVisual& av : r.visuals)
            if (!av.paintByOverlay) { veHet = false; break; }
        t->overlayVeHetPage[page] = veHet;
        if (veHet != r.overlayCapable)
            qDebug().noquote() << "[overlay] can LOP BU page=" << page
                               << "— co visual overlay khong ve (paintByOverlay=false)";
        t->visualsHasForeign[page]  = r.hasForeign;
        // 🔴 LÁT C 0902: tao/xoá xay ra TRONG LUC scan chay (luong tao phai cho
        // scan nhả khoa) → anh cat tren day CHUA co chu thich moi. Delta ghi sau
        // luc loadPageVisuals don sach van con trong hang → tron de len anh cat,
        // khong duoc de no ghi de mat chu thich vua tao.
        QList<AnnotVisual> mergedNow;
        const bool mergedNowOk = mergeAnnotVisuals(t, page, &mergedNow);
        // Trang da doi / tab dong → VUT, khong day vao view.
        if (t->currentPage != page) {
            qDebug().noquote() << "[overlay] page=" << page
                               << "stale result — cached, view untouched";
            // [nav] dong cua lan lat nay (rescan xong nhung trang da doi) → stale=1.
            if (m_navLogArmed && m_navDeferTab == t && m_navDeferPage == page) {
                const qint64 visualsMs = QDateTime::currentMSecsSinceEpoch() - m_navVisualsStartMs;
                qDebug().noquote() << QString("[nav] flip from=%1 to=%2 placeholderMs=%3 deferredStartMs=%4 visualsMs=%5 stale=1")
                    .arg(m_navFlipFrom).arg(page).arg(m_navPlaceholderMs)
                    .arg(m_navDeferredStartMs).arg(visualsMs);
                m_navLogArmed = false;
                m_navDeferTab = nullptr;
                m_navDeferPage = -1;
            }
            return;
        }
        if (mergedNowOk)
            applyAnnotVisuals(t, page, mergedNow, t->overlayCapablePage.value(page, false),
                              r.hasForeign);   // LÁT C 0902: day danh sach DA TRON delta, khong phai anh cat cu
        else
            applyAnnotVisuals(t, page, r.visuals, r.overlayCapable, r.hasForeign);
        // [nav] dong cua lan lat nay (rescan xong, trang van hien tai) → stale=0.
        if (m_navLogArmed && m_navDeferTab == t && m_navDeferPage == page) {
            const qint64 visualsMs = QDateTime::currentMSecsSinceEpoch() - m_navVisualsStartMs;
            qDebug().noquote() << QString("[nav] flip from=%1 to=%2 placeholderMs=%3 deferredStartMs=%4 visualsMs=%5 stale=0")
                .arg(m_navFlipFrom).arg(page).arg(m_navPlaceholderMs)
                .arg(m_navDeferredStartMs).arg(visualsMs);
            m_navLogArmed = false;
            m_navDeferTab = nullptr;
            m_navDeferPage = -1;
        }
    });
}

void MainWindow::refreshCommentsForPage(DocTab* t, int page) {
    // 0927 LƯỢT 10 (--no-annotscan): KHONG quet danh sach chu thich trang.
    if (trNoAnnotScan()) return;
    if (!t) return;
    invalidateAnnotPage(t, page);
    if (!t->annotCacheValid) {
        qDebug().noquote() << "[comments] cache build requested (was invalid)";
        if (t == currentTab() && m_thumbPanel && m_thumbPanel->isCommentsTabVisible())
            onCommentsRequested();
        return;
    }
    QElapsedTimer _perf;
    _perf.start();
    bool ok = false;
    // Copy by value — `fresh` khong duoc giu reference qua cac buoc cap nhat ben duoi.
    const QList<AnnotInfo> fresh = annotsForPage(t, page, &ok);
    if (!ok) {
        // Khoa ban: GIU NGUYEN du lieu cu dang hien, KHONG xoa annotCache, KHONG
        // setComments (khong duoc bien mat markup dang hien). annotsForPage da hen lai.
        qDebug().noquote() << "[comments] incremental SKIP — khoa ban, giu du lieu cu page=" << page;
        return;
    }
    for (int i = t->annotCache.size() - 1; i >= 0; --i) {
        if (t->annotCache[i].pageIndex == page)
            t->annotCache.removeAt(i);
    }
    int insertPos = 0;
    for (int i = 0; i < t->annotCache.size(); ++i) {
        if (t->annotCache[i].pageIndex >= page) {
            insertPos = i;
            break;
        }
        insertPos = i + 1;
    }
    for (const auto& info : fresh)
        t->annotCache.insert(insertPos++, info);
    qint64 ms = _perf.elapsed();
    qDebug().noquote() << "[perf] comments incremental page=" << page
             << "entries=" << fresh.size() << "ms=" << ms;
    if (t == currentTab() && m_thumbPanel && m_thumbPanel->isCommentsTabVisible())
        m_thumbPanel->setComments(t->annotCache);
}

// ── Undo/Redo ────────────────────────────────────────────────────────────

// 🔴 P3 (0921): sidecar "backup rồi xoá" chỉ còn cần khi còn một mục undo/redo tham
// chiếu tới nó. Khi mục bị loại khỏi stack (pushUndo cắt trần 100, xoá redoStack) hoặc
// khi đóng tab, phải XOÁ sidecar — nếu không mỗi lần xoá annot ngoài lại bỏ lại một bản
// sao CẢ TỆP (13 MB) không ai dọn.
static void dropUndoSidecar(const MarkupUndoEntry& e) {
    if (e.kind == MarkupUndoEntry::DeleteForeign && !e.trashPath.isEmpty())
        QFile::remove(e.trashPath);
}

void MainWindow::pushUndo(DocTab* t, const MarkupUndoEntry& e) {
    for (const auto& old : t->redoStack) dropUndoSidecar(old);   // P3: sidecar mồ côi
    t->redoStack.clear();
    t->undoStack.append(e);
    while (t->undoStack.size() > 100) {
        dropUndoSidecar(t->undoStack.first());                   // P3: mục bị cắt → dọn sidecar
        t->undoStack.removeFirst();
    }
    qDebug().noquote() << "[undo] push kind=" << static_cast<int>(e.kind)
                       << "page=" << e.page << "uid=" << e.uid
                       << "stack=" << t->undoStack.size();
    updateUndoActions();
}

void MainWindow::doUndo() {
    auto* t = currentTab();
    qDebug().noquote() << "[undo] doUndo goi: hasTab=" << (t != nullptr)
                       << "undoStack=" << (t ? t->undoStack.size() : -1)
                       << "redoStack=" << (t ? t->redoStack.size() : -1);
    if (!t) { qDebug().noquote() << "[undo] doUndo BO QUA: khong co tab"; return; }
    if (!t->annotMgr) { qDebug().noquote() << "[undo] doUndo BO QUA: annotMgr null"; return; }
    if (t->undoStack.isEmpty()) { qDebug().noquote() << "[undo] doUndo BO QUA: undoStack rong"; return; }
    MarkupUndoEntry e = t->undoStack.takeLast();
    qDebug().noquote() << "[undo] apply kind=" << static_cast<int>(e.kind)
                       << "page=" << e.page << "uid=" << e.uid;
    bool touchedPO = false;
    bool foreignMove = false;
    bool opOk = true;   // 🔴 P0 (0921): chỉ đẩy sang redoStack khi thao tác THẬT SỰ xong
    switch (e.kind) {
    case MarkupUndoEntry::AddShape: {
        int ai = t->annotMgr->findAnnotIndexByAnyUid(e.page, e.uid);
        if (ai >= 0) t->annotMgr->removeAnnot(e.page, ai);
        break;
    }
    case MarkupUndoEntry::DeleteShape: {
        bool snapOk = t->annotMgr->addSnapshot(e.page, e.snap);
        // 🔴 P2 (0921): addSnapshot co the that bai (subtype PDFium khong tao lai duoc…).
        // Neu van setAnnotUid(nc-1) thi uid gan len annot CUOI TRANG — co the la annot
        // NGOAI ⇒ annot doi tac bong thanh "cua ta" va qua mat chot P0. Giong nhanh Note.
        if (snapOk) {
            int nc = t->annotMgr->annotCount(e.page);
            if (nc > 0) t->annotMgr->setAnnotUid(e.page, nc - 1, e.uid);
        } else {
            qWarning().noquote() << "[undo] addSnapshot that bai — KHONG gan uid page=" << e.page;
        }
        break;
    }
    case MarkupUndoEntry::MoveAnnot: {
        int ai = t->annotMgr->findAnnotIndexByAnyUid(e.page, e.uid);
        if (ai >= 0) {
            t->annotMgr->moveAnnot(e.page, ai, -e.dxU, -e.dyU);
            if (!t->annotMgr->isOwnAnnot(e.page, ai)) foreignMove = true;
        }
        touchedPO = true;
        break;
    }
    case MarkupUndoEntry::RetextAnnot: {
        int ai = t->annotMgr->findAnnotIndexByAnyUid(e.page, e.uid);
        if (ai >= 0) t->annotMgr->retextNote(e.page, ai, e.oldText);
        touchedPO = true;
        break;
    }
    case MarkupUndoEntry::RestyleAnnot: {
        int ai = t->annotMgr->findAnnotIndexByAnyUid(e.page, e.uid);
        if (ai < 0) break;
        if (e.isFreeText) t->annotMgr->rebuildTextNote(e.page, ai, e.oldColor, e.oldFontSize);
        else              t->annotMgr->setAnnotStyle(e.page, ai, e.oldColor, e.oldWidth, e.oldFill, e.oldFillAlpha);
        touchedPO = e.isFreeText;
        break;
    }
    case MarkupUndoEntry::AddNote: {
        int ai = t->annotMgr->findAnnotIndexByAnyUid(e.page, e.uid);
        if (ai >= 0) t->annotMgr->removeAnnot(e.page, ai);
        buildVectorLayer(t, e.page, true);
        qDebug().noquote() << "[perf] note icon -> rebuild vector layer page=" << e.page;
        touchedPO = true;
        break;
    }
    case MarkupUndoEntry::DeleteNote: {
        bool ok = e.noteIsPopup
            ? t->annotMgr->createPopupNote(e.page, e.noteRect.topLeft(), e.noteText, e.noteAuthor)
            : t->annotMgr->createInlineNote(e.page, e.noteRect, e.noteText, e.noteAuthor,
                                            e.noteWithBackground, e.noteColor, e.noteFontSize);
        if (ok) {
            int nc = t->annotMgr->annotCount(e.page);
            if (nc > 0) t->annotMgr->setAnnotUid(e.page, nc - 1, e.uid);
        }
        buildVectorLayer(t, e.page, true);
        qDebug().noquote() << "[perf] note icon -> rebuild vector layer page=" << e.page;
        touchedPO = true;
        break;
    }
    case MarkupUndoEntry::ContentsEdit: {
        int ai = t->annotMgr->findAnnotIndexByAnyUid(e.page, e.uid);
        if (ai >= 0) t->annotMgr->setAnnotContents(e.page, ai, e.oldText);
        break;
    }
    case MarkupUndoEntry::ResizeStamp: {
        int ai = t->annotMgr->findAnnotIndexByAnyUid(e.page, e.uid);
        if (ai >= 0) t->annotMgr->setAnnotRectDisplay(e.page, ai, e.rectOld);
        break;
    }
    case MarkupUndoEntry::DeleteForeign: {
        // VIỆC 3-A3 (0921): dựng lại annot NGOÀI từ sidecar (byte-bằng), rồi nạp lại tab
        // vì tệp đã đổi ở tầng QPDF (mô hình in-memory cũ đã hết hiệu lực).
        if (t->annotMgr->restoreAnnotFromBackup(e.page, e.trashIndex, e.trashPath)) {
            loadTabFile(t, t->pdfPath, true);
            touchedPO = true;
            statusBar()->showMessage("Đã hoàn tác xoá chú thích (nguyên vẹn)", 4000);
        } else {
            statusBar()->showMessage("Không hoàn tác được: " + t->annotMgr->lastError(), 5000);
            opOk = false;   // 🔴 P0: thất bại thì giữ ở undoStack, không cho redo "xoá" bừa
        }
        break;
    }
    }
    if (!opOk) {
        t->undoStack.append(e);
        updateUndoActions();
        return;
    }
    t->redoStack.append(e);
    m_selPage = -1; m_selIdx = -1;
    if (t->view) t->view->clearSelectedAnnot();
    applyMarkupRefresh(t, e.page, touchedPO);
    if (foreignMove) {
        if (t->renderer) { t->renderer->invalidatePage(e.page); if (!m_fastMode) t->renderer->requestPage(e.page, t->zoom); }
        if (t->view) { t->view->invalidateTiles(); t->view->invalidateSharp(); }
        qDebug().noquote() << "[undo] foreignMove -> ep render lai trang" << e.page;
    }
    if (e.kind == MarkupUndoEntry::ContentsEdit)
        refreshCommentsForPage(t, e.page);
    updateUndoActions();
}

void MainWindow::doRedo() {
    auto* t = currentTab();
    qDebug().noquote() << "[redo] doRedo goi: hasTab=" << (t != nullptr)
                       << "undoStack=" << (t ? t->undoStack.size() : -1)
                       << "redoStack=" << (t ? t->redoStack.size() : -1);
    if (!t) { qDebug().noquote() << "[redo] doRedo BO QUA: khong co tab"; return; }
    if (!t->annotMgr) { qDebug().noquote() << "[redo] doRedo BO QUA: annotMgr null"; return; }
    if (t->redoStack.isEmpty()) { qDebug().noquote() << "[redo] doRedo BO QUA: redoStack rong"; return; }
    MarkupUndoEntry e = t->redoStack.takeLast();
    qDebug().noquote() << "[redo] apply kind=" << static_cast<int>(e.kind)
                       << "page=" << e.page << "uid=" << e.uid;
    bool touchedPO = false;
    bool foreignMove = false;
    bool opOk = true;   // 🔴 P0 (0921): chỉ đẩy sang undoStack khi thao tác THẬT SỰ xong
    switch (e.kind) {
    case MarkupUndoEntry::DeleteShape: {
        int ai = t->annotMgr->findAnnotIndexByAnyUid(e.page, e.uid);
        if (ai >= 0) t->annotMgr->removeAnnot(e.page, ai);
        break;
    }
    case MarkupUndoEntry::AddShape: {
        bool snapOk = t->annotMgr->addSnapshot(e.page, e.snap);
        // 🔴 P2 (0921): addSnapshot co the that bai (subtype PDFium khong tao lai duoc…).
        // Neu van setAnnotUid(nc-1) thi uid gan len annot CUOI TRANG — co the la annot
        // NGOAI ⇒ annot doi tac bong thanh "cua ta" va qua mat chot P0. Giong nhanh Note.
        if (snapOk) {
            int nc = t->annotMgr->annotCount(e.page);
            if (nc > 0) t->annotMgr->setAnnotUid(e.page, nc - 1, e.uid);
        } else {
            qWarning().noquote() << "[undo] addSnapshot that bai — KHONG gan uid page=" << e.page;
        }
        break;
    }
    case MarkupUndoEntry::MoveAnnot: {
        int ai = t->annotMgr->findAnnotIndexByAnyUid(e.page, e.uid);
        if (ai >= 0) {
            t->annotMgr->moveAnnot(e.page, ai, e.dxU, e.dyU);
            if (!t->annotMgr->isOwnAnnot(e.page, ai)) foreignMove = true;
        }
        touchedPO = true;
        break;
    }
    case MarkupUndoEntry::RetextAnnot: {
        int ai = t->annotMgr->findAnnotIndexByAnyUid(e.page, e.uid);
        if (ai >= 0) t->annotMgr->retextNote(e.page, ai, e.newText);
        touchedPO = true;
        break;
    }
    case MarkupUndoEntry::RestyleAnnot: {
        int ai = t->annotMgr->findAnnotIndexByAnyUid(e.page, e.uid);
        if (ai < 0) break;
        if (e.isFreeText) t->annotMgr->rebuildTextNote(e.page, ai, e.newColor, e.newFontSize);
        else              t->annotMgr->setAnnotStyle(e.page, ai, e.newColor, e.newWidth, e.newFill, e.newFillAlpha);
        touchedPO = e.isFreeText;
        break;
    }
    case MarkupUndoEntry::AddNote: {
        bool ok = e.noteIsPopup
            ? t->annotMgr->createPopupNote(e.page, e.noteRect.topLeft(), e.noteText, e.noteAuthor)
            : t->annotMgr->createInlineNote(e.page, e.noteRect, e.noteText, e.noteAuthor,
                                            e.noteWithBackground, e.noteColor, e.noteFontSize);
        if (ok) {
            int nc = t->annotMgr->annotCount(e.page);
            if (nc > 0) t->annotMgr->setAnnotUid(e.page, nc - 1, e.uid);
        }
        buildVectorLayer(t, e.page, true);
        qDebug().noquote() << "[perf] note icon -> rebuild vector layer page=" << e.page;
        touchedPO = true;
        break;
    }
    case MarkupUndoEntry::DeleteNote: {
        int ai = t->annotMgr->findAnnotIndexByAnyUid(e.page, e.uid);
        if (ai >= 0) t->annotMgr->removeAnnot(e.page, ai);
        buildVectorLayer(t, e.page, true);
        qDebug().noquote() << "[perf] note icon -> rebuild vector layer page=" << e.page;
        touchedPO = true;
        break;
    }
    case MarkupUndoEntry::ContentsEdit: {
        int ai = t->annotMgr->findAnnotIndexByAnyUid(e.page, e.uid);
        if (ai >= 0) t->annotMgr->setAnnotContents(e.page, ai, e.newText);
        break;
    }
    case MarkupUndoEntry::ResizeStamp: {
        int ai = t->annotMgr->findAnnotIndexByAnyUid(e.page, e.uid);
        if (ai >= 0) t->annotMgr->setAnnotRectDisplay(e.page, ai, e.rectNew);
        break;
    }
    case MarkupUndoEntry::DeleteForeign: {
        // VIỆC 3-A3 (0921): redo = xoá lại (sidecar vẫn còn để hoàn tác tiếp).
        // 🔴 P0 (0921): BẮT BUỘC saveDocument() TRƯỚC loadTabFile. loadTabFile đóng rồi
        // mở lại tệp TỪ ĐĨA (t->doc->close(); t->doc->open(path)) ⇒ nếu chỉ xoá trong bộ
        // nhớ thì việc xoá bị ĐÈ MẤT: redo báo "đã xoá" mà annot vẫn còn, trong khi e vẫn
        // được append vào undoStack ⇒ Ctrl+Z kế tiếp CHÈN THÊM một bản ⇒ annot đối tác
        // bị NHÂN ĐÔI. Theo đúng khuôn nhánh Undo (restoreAnnotFromBackup tự ghi tệp).
        if (!t->annotMgr->removeAnnot(e.page, e.trashIndex)) { opOk = false; break; }
        if (!t->annotMgr->saveDocument()) {
            loadTabFile(t, t->pdfPath, true);   // nạp lại từ đĩa để bộ nhớ khớp tệp
            statusBar()->showMessage("Không lưu được nên bỏ thao tác làm lại: "
                                     + t->annotMgr->lastError(), 5000);
            opOk = false;
            break;
        }
        loadTabFile(t, t->pdfPath, true);
        touchedPO = true;
        break;
    }
    }
    if (!opOk) {
        // Thao tác chưa thành công: giữ nguyên mục để làm lại, TUYỆT ĐỐI không đẩy sang
        // undoStack (nếu đẩy, Ctrl+Z sẽ "hoàn tác" một việc chưa từng xảy ra ⇒ nhân bản).
        t->redoStack.append(e);
        updateUndoActions();
        return;
    }
    t->undoStack.append(e);
    m_selPage = -1; m_selIdx = -1;
    if (t->view) t->view->clearSelectedAnnot();
    applyMarkupRefresh(t, e.page, touchedPO);
    if (foreignMove) {
        if (t->renderer) { t->renderer->invalidatePage(e.page); if (!m_fastMode) t->renderer->requestPage(e.page, t->zoom); }
        if (t->view) { t->view->invalidateTiles(); t->view->invalidateSharp(); }
        qDebug().noquote() << "[redo] foreignMove -> ep render lai trang" << e.page;
    }
    if (e.kind == MarkupUndoEntry::ContentsEdit)
        refreshCommentsForPage(t, e.page);
    updateUndoActions();
}

void MainWindow::applyMarkupRefresh(DocTab* t, int page, bool touchedPageObjects) {
    t->dirty = true; updateTabDirty(t);
    invalidateAnnotPage(t, page);
    if (touchedPageObjects)
        t->pagesNeedGenerate.insert(page);
    refreshAnnotVisuals(t, page);
    // Trang khong overlay duoc ma dang lay lop vector thi go lop ra truoc khi ve lai
    // raster, neu khong re-render bi chan (drop reason=vectorReady) va markup vo hinh.
    forceRasterPage(t, page);
    if (canFastPath(t, page)) { if (t->view) t->view->update(); }
    else if (baseIsVector(t, page)) {
        if (t->renderer) t->renderer->invalidatePage(page);
        if (t->view) t->view->update();
        qDebug() << "[perf] skip full render (nen la vector) page=" << page;
    } else {
        if (t->renderer) { t->renderer->invalidatePage(page); if (!m_fastMode) t->renderer->requestPage(page, t->zoom); }
        if (t->view) { t->view->invalidateTiles(); t->view->invalidateSharp(); }
    }
    // Undo/redo: chi refresh comments neu bang dang hien
    if (touchedPageObjects || (m_thumbPanel && m_thumbPanel->isCommentsTabVisible()))
        refreshCommentsForPage(t, page);
    if (touchedPageObjects && baseIsVector(t, page) && t->annotMgr && t->doc) {
        {
            TimedPdfiumLock lk(__FILE__, __LINE__);
            FPDF_PAGE pg = PageCache::acquire(t->doc->raw(), page);
            if (pg) {
                PageCache::PageBorrow _b(t->doc->raw(), page);   // R1: cap doi acquire()
                t->vecLayers.value(page)->rebuildNoteTiles(t->doc->raw(), pg);
            }
        }
        if (t->view) { t->view->invalidateTileTextures(); t->view->update(); }
    }
}

void MainWindow::updateUndoActions() {
    auto* t = currentTab();
    if (m_undoAct) m_undoAct->setEnabled(t && !t->undoStack.isEmpty());
    if (m_redoAct) m_redoAct->setEnabled(t && !t->redoStack.isEmpty());
    qDebug().noquote() << "[undo] actions undo=" << (m_undoAct ? m_undoAct->isEnabled() : false)
                       << "redo=" << (m_redoAct ? m_redoAct->isEnabled() : false)
                       << "undoStack=" << (t ? t->undoStack.size() : -1)
                       << "redoStack=" << (t ? t->redoStack.size() : -1);
}

// ── Chon/keo markup — DUNG CHUNG cho PdfGpuView + ContinuousView ─────────────
// (SPEC_CONTINUOUS_MARKUP_EDIT_2026-08-16). Ba ham nay giu nguyen DUONG undo
// san co (pushUndo + MarkupUndoEntry::MoveAnnot) va AnnotationManager::moveAnnot
// (tinh tien, khong xoa-dung-lai). View nao goi thi dung chung mot noi.

void MainWindow::setMarkupSelectionViews(DocTab* t, int page, const QRectF& rectPdf,
                                         const QString& uid, const QString& type, bool isOwn) {
    // 🔴 VIỆC 3-A1 (0921): tay nắm gốc (co giãn) CHỈ hiện cho annot CỦA TA. Annot phần
    // mềm khác mà hiện tay nắm = hứa suông: kéo sẽ đổi /Rect mà /AP giữ nguyên (hình
    // méo) và backend guardWrite từ chối. Chặn ngay ở tầng hiển thị.
    const bool canResize = isOwn && (type == QLatin1String("Stamp")
                                  || type == QLatin1String("FreeText"));
    if (t->view) {
        t->view->setSelectedAnnot(rectPdf);
        // Insert Image: Stamp cua TorReader co 4 tay nam goc de co gian.
        // 🔴 0903 owner chot: FreeText CUNG phai keo goc duoc — khung quyet dinh cho
        // xuong dong, co chu GIU NGUYEN (owner: "nam goc khung keo, text xuong dong theo").
        // Duong ghi la CHUNG (annotationResizeRequested -> onAnnotResize ->
        // setAnnotRectDisplay), khong rieng cho Stamp, nen chi can mo cong nay.
        t->view->setSelectResizable(canResize);
        if (type == QLatin1String("FreeText") && isOwn) {
            t->view->setDragNote(rectPdf.normalized());
        } else {
            t->view->setDragTarget(uid, QString(), 0.0f, QColor());
        }
    }
    if (m_continuousView) {
        m_continuousView->setSelectedAnnot(page, rectPdf);
        m_continuousView->setSelectResizable(canResize);
        if (type == QLatin1String("FreeText") && isOwn)
            m_continuousView->setDragNote(rectPdf.normalized());
        else
            m_continuousView->setDragTarget(uid, QString(), 0.0f, QColor());
    }
}

void MainWindow::clearMarkupSelectionViews(DocTab* t) {
    if (t->view) t->view->clearSelectedAnnot();
    if (m_continuousView) m_continuousView->clearSelectedAnnot();
}

void MainWindow::onAnnotPick(DocTab* t, int page, const QPointF& pt) {
    // 🔴 LOG-ONLY 2026-09-01: owner bao "Note/Text khong select duoc trong Continuous".
    // Duong chon KHONG he loc bo chung, nen phai do moi biet chet o dau: khong toi day,
    // hay toi ma khong trung o nao.
    // 🔴 LƯỢT 40b: thêm retry khi lock bận, không dùng visual fallback (rủi ro index)
    qDebug().noquote() << "[pick] onAnnotPick page=" << page << "diem=" << pt
                       << "cheDo=" << (m_fastMode ? "FAST" : "SINGLE");

    if (!t->annotMgr) return;
    bool ok = true;
    const auto& list = annotsForPage(t, page, &ok);

    // Nếu lock bận, retry tự động (không block UI)
    if (!ok) {
        if (!t->pickRetryAttempt) t->pickRetryAttempt = 0;
        t->pickRetryAttempt++;
        if (t->pickRetryAttempt <= 10) {
            qDebug().noquote() << "[pick] lock ban, retry sau 40ms (lan " << t->pickRetryAttempt << ")";
            QTimer::singleShot(40 * t->pickRetryAttempt, this, [this, t, page, pt, s = t->serial]() {
                if (tabAlive(t, s) && t->currentPage == page) {
                    onAnnotPick(t, page, pt);
                }
            });
            return;
        } else {
            t->pickRetryAttempt = 0;
            qDebug().noquote() << "[pick] lock ban sau 10 lan, dung";
            statusBar()->showMessage("Chú thích đang được cập nhật, thử bấm lại", 2000);
            return;
        }
    }
    t->pickRetryAttempt = 0;   // reset khi ok=true

    m_selPage = -1; m_selIdx = -1;
    for (int i = list.size() - 1; i >= 0; --i) {
        if (list[i].type == QLatin1String("Widget")) continue;
        QRectF hitRect = list[i].rect.normalized().adjusted(-3, -3, 3, 3);
        if (i >= list.size() - 12)
            qDebug().noquote() << "[pick]   ung vien" << i << list[i].type
                               << "o=" << hitRect << "trung=" << hitRect.contains(pt);
        if (hitRect.contains(pt)) {
            m_selPage = page; m_selIdx = i;
            qDebug().noquote() << "[markup] picked annot page=" << page << "idx=" << i << "type=" << list[i].type;
            setMarkupSelectionViews(t, page, list[i].rect.normalized(),
                                    list[i].uid, list[i].type, list[i].isOwn);
            m_thumbPanel->selectCommentFor(page, i);
            if (!list[i].text.isEmpty()) {
                showNotePopup(list[i].text, list[i].author);
            }
            return;
        }
    }
    clearMarkupSelectionViews(t);
    m_thumbPanel->selectCommentFor(-1, -1);
    hideNotePopup();
}

void MainWindow::onAnnotContext(DocTab* t, int page, const QPointF& pt, const QPoint& gpos) {
    if (!t->annotMgr) return;

    // 🔴 LƯỢT 40b: retry khi lock bận
    bool ok = true;
    const auto& list = annotsForPage(t, page, &ok);

    if (!ok) {
        if (!t->ctxRetryAttempt) t->ctxRetryAttempt = 0;
        t->ctxRetryAttempt++;
        if (t->ctxRetryAttempt <= 10) {
            qDebug().noquote() << "[pick] context: lock ban, retry sau 40ms (lan " << t->ctxRetryAttempt << ")";
            QTimer::singleShot(40 * t->ctxRetryAttempt, this, [this, t, page, pt, gpos, s = t->serial]() {
                if (tabAlive(t, s) && t->currentPage == page) {
                    onAnnotContext(t, page, pt, gpos);
                }
            });
            return;
        } else {
            t->ctxRetryAttempt = 0;
            qDebug().noquote() << "[pick] context: lock ban sau 10 lan, dung";
            statusBar()->showMessage("Chú thích đang được cập nhật, thử bấm lại", 2000);
            return;
        }
    }
    t->ctxRetryAttempt = 0;   // reset khi ok=true

    QElapsedTimer _perfRC;
    _perfRC.start();
    int idx = -1;
    QString ctxType, ctxUid;
    bool ctxOwn = false;
    QRectF ctxRect;
    {
        for (int i = list.size() - 1; i >= 0; --i) {
            if (list[i].type == QLatin1String("Widget")) continue;
            QRectF hitRect = list[i].rect.normalized().adjusted(-3, -3, 3, 3);
            if (hitRect.contains(pt)) { idx = i; break; }
        }
        if (idx < 0) {
            // Chuot phai tren vung trang trong → menu OCR (SPEC_OCR_4 muc 2b)
            clearMarkupSelectionViews(t);
            m_thumbPanel->selectCommentFor(-1, -1);
            QMenu pageMenu(this);
            if (t->textSel.active) {
                QAction* copyAct = pageMenu.addAction("Copy text");
                copyAct->setShortcut(QKeySequence::Copy);
                connect(copyAct, &QAction::triggered, this, [this]{
                    copyTextSelectionToClipboard();
                });
                pageMenu.addSeparator();
            }
            const bool canPage = pageNeedsOcr(t->doc->raw(), page);
            QAction* pageAct = pageMenu.addAction("Recognize text on this page");
            QAction* allAct  = pageMenu.addAction("Recognize text on all pages");
            pageAct->setEnabled(canPage);
            if (!canPage) pageAct->setToolTip("Already recognized");
            QAction* chosen = pageMenu.exec(gpos);
            if (chosen == pageAct) onOcrPageRequested(page);
            else if (chosen == allAct) onOcrAllRequested();
            return;
        }
        ctxType = list[idx].type;
        ctxUid  = list[idx].uid;
        ctxOwn  = list[idx].isOwn;
        ctxRect = list[idx].rect.normalized();
    }
    m_selPage = page; m_selIdx = idx;
    setMarkupSelectionViews(t, page, ctxRect, ctxUid, ctxType, ctxOwn);
    m_thumbPanel->selectCommentFor(page, idx);
    // LUOT 41 (30/09): Continuous chi-xem markup — chuot phai trung markup trong
    // Continuous chi CHON (nhu tren), KHONG hien menu Edit/Properties/Delete.
    // Vung trong van la menu OCR nhu cu (nhanh idx<0 o tren, khong dung toi day).
    if (probeContinuousVisible()) {
        statusBar()->showMessage(
            "Chế độ Continuous chỉ xem — sang Single để sửa markup", 2500);
        return;
    }
    QMenu menu(this);
    // Muc copy chu la muc DAU TIEN khi dang co vung chon (SPEC_TEXTSEL_ADOBE).
    if (t->textSel.active) {
        QAction* copyAct = menu.addAction("Copy text");
        copyAct->setShortcut(QKeySequence::Copy);
        connect(copyAct, &QAction::triggered, this, [this]{
            copyTextSelectionToClipboard();
        });
        menu.addSeparator();
    }
    const bool canEditText = (ctxType == QLatin1String("FreeText") || ctxType == QLatin1String("Note"));
    QAction* editAct = canEditText ? menu.addAction("Edit text…") : nullptr;
    QAction* propAct = menu.addAction("Properties…");
    QAction* del     = menu.addAction("Delete");
    menu.addSeparator();
    // Chen 2 muc OCR vao menu chuot phai san co (SPEC_OCR_4 muc 2b)
    const bool canPage = pageNeedsOcr(t->doc->raw(), page);
    QAction* ocrPageAct = menu.addAction("Recognize text on this page");
    QAction* ocrAllAct  = menu.addAction("Recognize text on all pages");
    ocrPageAct->setEnabled(canPage);
    if (!canPage) ocrPageAct->setToolTip("Already recognized");
    qDebug().noquote() << "[perf] rightclick handled ms=" << _perfRC.elapsed();
    QAction* chosen  = menu.exec(gpos);
    if (chosen == ocrPageAct) {
        onOcrPageRequested(page);
    } else if (chosen == ocrAllAct) {
        onOcrAllRequested();
    } else if (chosen == del) {
        deleteSelectedAnnot(page, idx);
    } else if (editAct && chosen == editAct) {
        editSelectedAnnot(page, idx);
    } else if (chosen == propAct) {
        int realIdxProp = idx;
        if (!ctxUid.isEmpty()) {
            int rp = t->annotMgr->findAnnotIndexByUid(page, ctxUid);
            if (rp >= 0) realIdxProp = rp;
        }
        QDialog dlg(this);
        dlg.setWindowTitle("Markup properties");
        auto* form = new QFormLayout(&dlg);
        QString realType; QColor realColor = m_annotStyle.strokeColor; float realWidth = m_annotStyle.strokeWidth; float realFont = 11.0f;
        bool curHasFill = false; int curFillAlpha = 255;
        t->annotMgr->getAnnotEditState(page, realIdxProp, realType, realColor, realWidth, realFont, &curHasFill, &curFillAlpha);
        QColor curColor = realColor;
        const char* btnFg = m_darkMode ? darkHC().fg : lightHC().fg;
        auto* colorBtn = new QPushButton("Choose…");
        colorBtn->setStyleSheet(QString("background:%1;color:%2;").arg(curColor.name()).arg(btnFg));
        connect(colorBtn, &QPushButton::clicked, &dlg, [&]{
            QColor c = QColorDialog::getColor(curColor, &dlg, "Markup color");
            if (c.isValid()) { curColor = c; colorBtn->setStyleSheet(QString("background:%1;color:%2;").arg(c.name()).arg(btnFg)); }
        });
        form->addRow("Color", colorBtn);
        const bool isText = (ctxType == QLatin1String("FreeText"));
        QSpinBox* widthSpin = nullptr;
        QCheckBox* fillChk = nullptr;
        QSpinBox* fontSpin = nullptr;
        QSpinBox* fillOpacitySpin = nullptr;
        if (isText) {
            fontSpin = new QSpinBox; fontSpin->setRange(6, 96);
            fontSpin->setValue(qRound(realFont));
            form->addRow("Font size (pt)", fontSpin);
        } else {
            widthSpin = new QSpinBox; widthSpin->setRange(1, 24);
            widthSpin->setValue(qMax(1, qRound(realWidth)));
            fillChk = new QCheckBox;
            fillChk->setChecked(curHasFill);
            form->addRow("Width (px)", widthSpin);
            form->addRow("Fill", fillChk);
            fillOpacitySpin = new QSpinBox;
            fillOpacitySpin->setRange(0, 100);
            fillOpacitySpin->setSuffix("%");
            fillOpacitySpin->setValue(qBound(0, qRound(curFillAlpha * 100.0 / 255.0), 100));
            form->addRow("Fill opacity", fillOpacitySpin);
        }
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        form->addRow(bb);
        connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted) return;
        if (isText) {
            if (t->annotMgr->rebuildTextNote(page, realIdxProp, curColor,
                                               static_cast<float>(fontSpin->value()))) {
                {
                    MarkupUndoEntry ue; ue.kind = MarkupUndoEntry::RestyleAnnot; ue.page = page;
                    ue.uid = ctxUid;
                    ue.oldColor = realColor; ue.newColor = curColor;
                    ue.oldFontSize = realFont; ue.newFontSize = static_cast<float>(fontSpin->value());
                    ue.isFreeText = true;
                    if (!ctxUid.isEmpty()) pushUndo(t, ue);
                }
                t->dirty = true; updateTabDirty(t);
                invalidateAnnotPage(t, page);
                t->pagesNeedGenerate.insert(page);
                refreshAnnotVisuals(t, page);
                if (baseIsVector(t, page)) {
                    if (t->renderer) t->renderer->invalidatePage(page);
                    if (t->view) t->view->update();
                } else {
                    if (t->renderer) { t->renderer->invalidatePage(page); if (!m_fastMode) t->renderer->requestPage(page, t->zoom); }
                    if (t->view) t->view->invalidateTiles();
                    if (t->view) t->view->invalidateSharp();
                }
                m_selPage = -1; m_selIdx = -1;
                clearMarkupSelectionViews(t);
                refreshCommentsForPage(t, page);
            } else {
                statusBar()->showMessage(
                    "Chú thích này của phần mềm khác — đổi màu/cỡ chữ sẽ làm mất định dạng gốc nên đã bỏ qua", 5000);
            }
        } else {
            if (t->annotMgr->setAnnotStyle(page, realIdxProp, curColor,
                                             static_cast<float>(widthSpin->value()),
                                             fillChk->isChecked(),
                                             qBound(0, qRound(fillOpacitySpin->value() * 255.0 / 100.0), 255))) {
                {
                    MarkupUndoEntry ue; ue.kind = MarkupUndoEntry::RestyleAnnot; ue.page = page;
                    ue.uid = ctxUid;
                    ue.oldColor = realColor; ue.newColor = curColor;
                    ue.oldWidth = realWidth; ue.newWidth = static_cast<float>(widthSpin->value());
                    ue.oldFill = curHasFill; ue.newFill = fillChk->isChecked();
                    ue.oldFillAlpha = curFillAlpha; ue.newFillAlpha = qBound(0, qRound(fillOpacitySpin->value() * 255.0 / 100.0), 255);
                    ue.isFreeText = false;
                    if (!ctxUid.isEmpty()) pushUndo(t, ue);
                }
                t->dirty = true; updateTabDirty(t);
                invalidateAnnotPage(t, page);
                t->pagesNeedGenerate.insert(page);
                refreshAnnotVisuals(t, page);
                forceRasterPage(t, page);
                if (canFastPath(t, page)) {
                    if (t->view) t->view->update();
                } else if (baseIsVector(t, page)) {
                    if (t->renderer) t->renderer->invalidatePage(page);
                    if (t->view) t->view->update();
                } else {
                    if (t->renderer) { t->renderer->invalidatePage(page); if (!m_fastMode) t->renderer->requestPage(page, t->zoom); }
                    if (t->view) t->view->invalidateTiles();
                    if (t->view) t->view->invalidateSharp();
                }
                refreshCommentsForPage(t, page);
            }
        }
    }
}

void MainWindow::onAnnotMove(DocTab* t, int page, double dx, double dy) {
    QElapsedTimer _totalT; _totalT.start();
    QElapsedTimer _stepT; _stepT.start();
    qDebug().noquote() << "[markup] move BAT DAU page=" << page
                       << "selIdx=" << m_selIdx << "dx=" << dx << "dy=" << dy;
    if (m_selPage != page || m_selIdx < 0 || !t->annotMgr) {
        qDebug().noquote() << "[markup] move BO QUA: khong co annot dang chon";
        qDebug().noquote() << "[perf] MOVE handler total ms=" << _totalT.elapsed();
        return;
    }
    QString moveType, moveUid;
    {
        _stepT.restart();
        const auto& annots = annotsForPage(t, page);
        qDebug().noquote() << "[perf] MOVE step annotsForPage ms=" << _stepT.elapsed();
        if (m_selIdx < 0 || m_selIdx >= annots.size()) {
            qDebug().noquote() << "[markup] move BO QUA: khong co annot dang chon";
            qDebug().noquote() << "[perf] MOVE handler total ms=" << _totalT.elapsed();
            return;
        }
        _stepT.restart();
        moveType = annots[m_selIdx].type;
        moveUid  = annots[m_selIdx].uid;
        qDebug().noquote() << "[perf] MOVE step readTypeUid ms=" << _stepT.elapsed();
    }
    int realMoveIdx = m_selIdx;
    if (!moveUid.isEmpty()) {
        _stepT.restart();
        int r = t->annotMgr->findAnnotIndexByUid(page, moveUid);
        qDebug().noquote() << "[perf] MOVE step findAnnotIndexByUid ms=" << _stepT.elapsed();
        if (r >= 0) realMoveIdx = r;
    }
    _stepT.restart();
    bool isForeign = !t->annotMgr->isOwnAnnot(page, realMoveIdx);
    qDebug().noquote() << "[perf] MOVE step isOwnAnnot ms=" << _stepT.elapsed();
    bool isPageObjNote = (moveType == "FreeText" || moveType == "Note");
    qDebug().noquote() << "[markup] move isForeign=" << isForeign
                       << "type=" << moveType << "uid=" << moveUid;

    double dxU = dx, dyU = dy;
    {
        _stepT.restart();
        TimedPdfiumLock lock(__FILE__, __LINE__);
        qDebug().noquote() << "[perf] MOVE step pdfiumMutex lock ms=" << _stepT.elapsed();
        _stepT.restart();
        FPDF_PAGE mp = PageCache::acquire(t->doc->raw(), page);
        qDebug().noquote() << "[perf] MOVE step acquireSharedPage ms=" << _stepT.elapsed();
        if (mp) {
            PageCache::PageBorrow _b(t->doc->raw(), page);   // R1: cap doi acquire()
            _stepT.restart();
            switch (FPDFPage_GetRotation(mp)) {
                case 1: dxU = -dy; dyU = dx;  break;
                case 2: dxU = -dx; dyU = -dy; break;
                case 3: dxU =  dy; dyU = -dx; break;
                default: break;
            }
            qDebug().noquote() << "[perf] MOVE step GetRotation+switch ms=" << _stepT.elapsed();
        }
    }
    MarkupUndoEntry moveUndo;   // 🔴 P1 (0921): chỉ đẩy vào stack SAU khi moveAnnot trả true
    {
        MarkupUndoEntry ue; ue.kind = MarkupUndoEntry::MoveAnnot; ue.page = page;
        ue.uid = moveUid;
        // 🔴 VIỆC 3-A5 (0921): BỎ ensureExternalUid. Trước đây với annot NGOÀI (uid rỗng)
        // nó ghi /TRXUID vào annot đối tác TRƯỚC khi moveAnnot kịp từ chối ⇒ lời hứa
        // "không chạm gì" chỉ đúng ở tầng moveAnnot. Nay annot ngoài bị từ chối di chuyển
        // (A2) nên không có gì để hoàn tác; không ghi /TRXUID vào annot ngoài nữa.
        ue.dxU = dxU; ue.dyU = dyU;
        moveUndo = ue;
        if (ue.uid.isEmpty())
            qDebug() << "[undo] move KHONG ghi duoc: annot khong co uid page=" << page;
    }
    qDebug().noquote() << "[perf] MOVE pre-phase ms=" << _totalT.elapsed();
    bool ok = t->annotMgr->moveAnnot(page, realMoveIdx, dxU, dyU);
    if (!ok) {
        qDebug().noquote() << "[markup] move THAT BAI (moveAnnot tra false)";
        statusBar()->showMessage("Chú thích này của phần mềm khác — không di chuyển được mà không làm hỏng nó", 4000);
        qDebug().noquote() << "[perf] MOVE handler total ms=" << _totalT.elapsed();
        return;                     // KHÔNG đẩy undo khi moveAnnot thất bại
    }
    if (!moveUndo.uid.isEmpty()) {
        _stepT.restart();
        pushUndo(t, moveUndo);
        qDebug().noquote() << "[perf] MOVE step pushUndo ms=" << _stepT.elapsed();
    }
    t->annotPageCache.remove(page);
    t->visualsCache.remove(page);
    t->visualsRev.remove(page);
    if (baseIsVector(t, page) && t->vecLayers.contains(page) && t->annotMgr && t->doc) {
        TimedPdfiumLock lk(__FILE__, __LINE__);
        FPDF_PAGE pg = PageCache::acquire(t->doc->raw(), page);
        if (pg) {
            PageCache::PageBorrow _b(t->doc->raw(), page);   // R1: cap doi acquire()
            t->vecLayers.value(page)->rebuildNoteTiles(t->doc->raw(), pg);
        }
    }
    if (t->view) t->view->invalidateTileTextures();
    refreshAnnotVisuals(t, page);
    int newIdx = moveUid.isEmpty() ? -1 : t->annotMgr->findAnnotIndexByAnyUid(page, moveUid);
    if (newIdx >= 0) {
        const auto& nl = annotsForPage(t, page);
        if (newIdx < nl.size())
            setMarkupSelectionViews(t, page, nl[newIdx].rect.normalized(),
                                    nl[newIdx].uid, nl[newIdx].type, nl[newIdx].isOwn);
    }
    if (t->view) t->view->update();
    if (isPageObjNote) {
        qDebug().noquote() << "[perf] note icon -> rebuild vector layer page=" << page;
        buildVectorLayer(t, page, true);
    }
    qDebug().noquote() << "[perf] MOVE handler total ms=" << _totalT.elapsed();
}

// ── Insert Image (SPEC_INSERT_IMAGE_2026-08-30) ───────────────────────────────
void MainWindow::onInsertImage() {
    auto* t = currentTab();
    if (!t || !t->doc || !t->doc->isOpen() || !t->annotMgr || !t->annotLayer) {
        statusBar()->showMessage("Hãy mở một PDF rồi mới chèn ảnh được", 4000);
        return;
    }
    if (!t->view) { statusBar()->showMessage("Không có vùng xem", 4000); return; }

    const QString path = QFileDialog::getOpenFileName(
        this, "Insert image into PDF", QString(), "Images (*.png *.jpg *.jpeg *.bmp)");
    if (path.isEmpty()) return;
    QImage img(path);
    if (img.isNull()) { statusBar()->showMessage("Không đọc được ảnh", 4000); return; }
    if (img.width() < 4 || img.height() < 4) { statusBar()->showMessage("Ảnh quá nhỏ", 4000); return; }

// Vi tri giua vung dang xem cua trang hien tai (point hien thi, Y-down).
        const int page = t->currentPage;
        QPointF center;
        QSizeF pageSizePt;
        double viewWpt = 0.0, viewHpt = 0.0;
        if (m_fastMode && m_continuousView) {
            int pg = -1;
            if (!m_continuousView->probeViewportCenter(&pg, &center)) {
                statusBar()->showMessage("Không xác định được vị trí chèn", 4000);
                return;
            }
            pageSizePt = m_continuousView->pageSizePt(pg);
            viewWpt = double(m_continuousView->viewport()->width()) / m_continuousView->zoom();
            viewHpt = double(m_continuousView->viewport()->height()) / m_continuousView->zoom();
        } else {
            center = t->view->widgetToPdf(t->view->rect().center());
            pageSizePt = t->view->pageSizePt();
            viewWpt = double(t->view->width()) / t->view->zoom();
            viewHpt = double(t->view->height()) / t->view->zoom();
        }

    // Kich thuoc ban dau: vua khung nhin, toi da 1/3 chieu rong trang, giu ty le anh.
    double w = qMin(viewWpt * 0.5, pageSizePt.width() / 3.0);
    double h = w * double(img.height()) / double(qMax(1, img.width()));
    if (h > viewHpt * 0.5) { h = viewHpt * 0.5; w = h * double(img.width()) / double(qMax(1, img.height())); }
    if (w <= 1.0 || h <= 1.0) { statusBar()->showMessage("Vùng xem quá nhỏ để chèn ảnh", 4000); return; }

    QRectF rectDisp(center.x() - w / 2.0, center.y() - h / 2.0, w, h);
    const QString uid = t->annotLayer->insertStampImage(page, img, rectDisp);
    if (uid.isEmpty()) {
        statusBar()->showMessage("Chèn ảnh thất bại: " + t->annotMgr->lastError(), 4000);
        return;
    }
    qDebug().noquote() << "[imgstamp] inserted page=" << page << "uid=" << uid
                       << "rect=" << rectDisp;
    {
        MarkupUndoEntry ue; ue.kind = MarkupUndoEntry::AddShape; ue.page = page; ue.uid = uid;
        ue.snap = t->annotMgr->lastCreatedSnapshot();
        pushUndo(t, ue);
    }
    t->dirty = true;
    updateTabDirty(t);
    invalidateAnnotPage(t, page);
    refreshAnnotVisuals(t, page);
    refreshCommentsForPage(t, page);
    // Trang khong chay duong overlay nhanh (co annot ngoai) thi phai render lai
    // raster de Stamp hien (AP da sinh san trong insertStampImage).
    if (canFastPath(t, page)) {
        if (t->view) t->view->update();
    } else {
        forceRasterPage(t, page);
        if (t->renderer) { t->renderer->invalidatePage(page); if (!m_fastMode) t->renderer->requestPage(page, t->zoom); }
        if (t->view) { t->view->invalidateTiles(); t->view->invalidateSharp(); }
    }
    // Chon ngay Stamp moi (co tay nam goc de keo/co gian).
    const int newIdx = t->annotMgr->findAnnotIndexByAnyUid(page, uid);
    if (newIdx >= 0) {
        const auto& nl = annotsForPage(t, page);
        if (newIdx < nl.size()) {
            m_selPage = page; m_selIdx = newIdx;
            setMarkupSelectionViews(t, page, nl[newIdx].rect.normalized(), nl[newIdx].uid, nl[newIdx].type, nl[newIdx].isOwn);
            m_thumbPanel->selectCommentFor(page, newIdx);
        }
    }
    statusBar()->showMessage("Chèn ảnh xong — kéo để di chuyển, kéo góc để co giãn (Shift: tự do)", 4000);
}

void MainWindow::onAnnotResize(DocTab* t, int page, QRectF newRectDisp) {
    if (!t || !t->annotMgr) return;
    if (m_selPage != page || m_selIdx < 0) {
        qDebug().noquote() << "[imgstamp] resize BO QUA: khong co annot dang chon";
        return;
    }
    const auto& list = annotsForPage(t, page);
    if (m_selIdx >= list.size()) return;
    const QString uid = list[m_selIdx].uid;
    const QRectF oldRectDisp = list[m_selIdx].rect.normalized();
    if (oldRectDisp == newRectDisp) return;   // khong doi — bo qua
    int realIdx = m_selIdx;
    if (!uid.isEmpty()) {
        const int r = t->annotMgr->findAnnotIndexByUid(page, uid);
        if (r >= 0) realIdx = r;
    }
    if (!t->annotMgr->setAnnotRectDisplay(page, realIdx, newRectDisp)) {
        statusBar()->showMessage("Co giãn thất bại", 4000);
        qDebug().noquote() << "[imgstamp] resize THAT BAI page=" << page << "rect=" << newRectDisp;
        return;
    }
    {
        MarkupUndoEntry ue; ue.kind = MarkupUndoEntry::ResizeStamp; ue.page = page; ue.uid = uid;
        ue.rectOld = oldRectDisp; ue.rectNew = newRectDisp;
        pushUndo(t, ue);
    }
    qDebug().noquote() << "[imgstamp] resized page=" << page << "old=" << oldRectDisp
                       << "new=" << newRectDisp;
    t->dirty = true;
    updateTabDirty(t);
    invalidateAnnotPage(t, page);
    // Day lai khung chon theo rect moi (uid khong doi sau co gian).
    const int newIdx = uid.isEmpty() ? -1 : t->annotMgr->findAnnotIndexByAnyUid(page, uid);
    if (newIdx >= 0) {
        const auto& nl = annotsForPage(t, page);
        if (newIdx < nl.size())
            setMarkupSelectionViews(t, page, nl[newIdx].rect.normalized(), nl[newIdx].uid, nl[newIdx].type, nl[newIdx].isOwn);
    }
    refreshAnnotVisuals(t, page);     // overlap cache het han (bumpPageRevision ben trong)
    refreshCommentsForPage(t, page);
    if (canFastPath(t, page)) {
        if (t->view) t->view->update();
    } else {
        forceRasterPage(t, page);
        if (t->renderer) { t->renderer->invalidatePage(page); if (!m_fastMode) t->renderer->requestPage(page, t->zoom); }
        if (t->view) { t->view->invalidateTiles(); t->view->invalidateSharp(); }
    }
}

void MainWindow::deleteSelectedAnnot(int page, int index) {
    auto* t = currentTab();
    if (!t || !t->annotMgr || index < 0) return;
    const auto& list = annotsForPage(t, page);
    if (index >= list.size()) return;
    const QString annotType = list[index].type;
    const QString annotUid  = list[index].uid;
    int realIdx = index;
    if (!annotUid.isEmpty()) {
        int r = t->annotMgr->findAnnotIndexByUid(page, annotUid);
        if (r >= 0) realIdx = r;
    }
    bool isOwn = !annotUid.isEmpty();
    if (!isOwn) isOwn = t->annotMgr->isOwnAnnot(page, realIdx);
    bool isNoteOrFT = (annotType == QLatin1String("Note") || annotType == QLatin1String("FreeText"));

    if (isOwn && isNoteOrFT) {
        // DeleteNote — capture all data before deletion, then undoable
        QColor noteColor = list[index].color;
        float noteFontSize = 11.0f;
        if (annotUid.isEmpty()) {
            QString esType; QColor esColor; float esW, esFs;
            if (t->annotMgr->getAnnotEditState(page, realIdx, esType, esColor, esW, esFs) && esFs > 0)
                noteFontSize = esFs;
        }
        MarkupUndoEntry ue; ue.kind = MarkupUndoEntry::DeleteNote; ue.page = page; ue.uid = annotUid;
        ue.noteRect = list[index].rect;
        ue.noteText = list[index].text;
        ue.noteAuthor = list[index].author;
        ue.noteColor = noteColor;
        ue.noteFontSize = noteFontSize;
        ue.noteWithBackground = (annotType == QLatin1String("FreeText"));
        ue.noteIsPopup = (annotType == QLatin1String("Note"));
        if (!t->annotMgr->removeAnnot(page, realIdx)) return;
        if (!annotUid.isEmpty()) pushUndo(t, ue);
        else statusBar()->showMessage("Đã xoá — thao tác này không hoàn tác được", 4000);
        buildVectorLayer(t, page, true);
        qDebug().noquote() << "[perf] note icon -> rebuild vector layer page=" << page;
    } else if (isOwn) {
        // DeleteShape (existing behavior for shapes)
        MarkupUndoEntry ue; ue.kind = MarkupUndoEntry::DeleteShape; ue.page = page; ue.uid = annotUid;
        ue.snap = t->annotMgr->snapshotAnnot(page, realIdx);
        const bool canUndoThis = ue.snap.valid && !ue.uid.isEmpty();
        if (!t->annotMgr->removeAnnot(page, realIdx)) return;
        if (canUndoThis) pushUndo(t, ue);
        else statusBar()->showMessage("Đã xoá — thao tác này không hoàn tác được", 4000);
    } else {
        // 🔴 VIỆC 3-A3 (0921): annot NGOÀI — owner chốt "backup rồi xoá". Sao lưu NGUYÊN
        // VẸN xuống sidecar trước; Ctrl+Z sẽ trích đúng object đó dựng lại byte-bằng.
        // Không sao lưu được thì TỪ CHỐI xoá (không được xoá thứ không thể hoàn tác).
        const QString side = t->annotMgr->backupAnnotForDelete(page, realIdx);
        if (side.isEmpty()) {
            statusBar()->showMessage("Không sao lưu được chú thích nên đã bỏ qua thao tác xoá", 5000);
            return;
        }
        MarkupUndoEntry ue; ue.kind = MarkupUndoEntry::DeleteForeign;
        ue.page = page; ue.uid = annotUid; ue.trashPath = side; ue.trashIndex = realIdx;
        if (!t->annotMgr->removeAnnot(page, realIdx)) {
            t->annotMgr->discardAnnotBackup(side);
            return;
        }
        pushUndo(t, ue);
        statusBar()->showMessage("Đã xoá chú thích của phần mềm khác — Ctrl+Z để hoàn tác", 4000);
        // 🔴 LÁT C 0902: annot NGOAI khong co uid → khong co delta xoa → nen cu
        // trong cache la ma SOI. Huy nen ep nap lai ca trang cho duong nay (hiem).
        t->visualsCache.remove(page);
        t->visualsRev.remove(page);
        t->visualsHasForeign.remove(page);
    }
    invalidateAnnotPage(t, page);
    t->dirty = true; updateTabDirty(t);
    if (t->view) t->view->clearSelectedAnnot();
    m_selPage = -1; m_selIdx = -1;
    refreshAnnotVisuals(t, page);
    t->pagesNeedGenerate.insert(page);
    forceRasterPage(t, page);
    bool isPageObjectAnnot = (annotType == QLatin1String("FreeText") ||
                              annotType == QLatin1String("Note"));
    if (canFastPath(t, page) && !isPageObjectAnnot) {
        if (t->view) t->view->update();
    } else if (baseIsVector(t, page)) {
        if (t->renderer) t->renderer->invalidatePage(page);
        if (t->view) t->view->update();
    } else {
        if (t->renderer) { t->renderer->invalidatePage(page); if (!m_fastMode) t->renderer->requestPage(page, t->zoom); }
        if (t->view) t->view->invalidateTiles();
        if (t->view) t->view->invalidateSharp();
    }
    refreshCommentsForPage(t, page);
}

void MainWindow::editSelectedAnnot(int page, int index) {
    auto* t = currentTab();
    if (!t || !t->annotMgr || index < 0) return;
    const auto& list = annotsForPage(t, page);
    if (index >= list.size()) return;
    const QString annotText = list[index].text;
    const QString annotType = list[index].type;
    const QString annotUid  = list[index].uid;
    int realIdx = index;
    if (!annotUid.isEmpty()) {
        int r = t->annotMgr->findAnnotIndexByUid(page, annotUid);
        if (r >= 0) realIdx = r;
    }
    // 0927 BƯỚC 1 đề 2: FreeText dùng hộp NHIỀU DÒNG (Enter = xuống
    // dòng) → bỏ cờ singleLine. Chỉ đổi 2 cờ duy nhất để ghép an toàn.
    // 🔴 0927 LƯỢT 6 / VIỆC 2: truyền theme ĐANG CHẠY của app (không
    // hard-code) ⇒ hộp "Edit text…" sáng khi app sáng, tối khi bật Dark Mode.
    NoteInputDialog dlg(annotText, this, /*singleLine=*/false, m_darkMode);
    dlg.setWindowTitle("Edit text");
    if (dlg.exec() != QDialog::Accepted) return;
    QString newText = dlg.text();
    QString oldText = annotText;
    if (!t->annotMgr->retextNote(page, realIdx, newText)) {
        // 🔴 0927 LƯỢT 6: FreeText NGOÀI giờ SỬA ĐƯỢC (SPEC 0927 BƯỚC 2), nên
        // nhánh này gần như chỉ còn chạy cho chú thích KHÔNG phải FreeText
        // (Text/note popup) — in lý do THẬT từ guardWrite thay vì đoán.
        const QString why = t->annotMgr->lastError();
        statusBar()->showMessage(
            why.isEmpty()
                ? QStringLiteral("Không sửa được chú thích này — đã bỏ qua")
                : QStringLiteral("Không sửa được: %1").arg(why), 6000);
        return;
    }
    {
        MarkupUndoEntry ue; ue.kind = MarkupUndoEntry::RetextAnnot; ue.page = page;
        ue.uid = annotUid;
        ue.oldText = oldText;
        ue.newText = newText;
        if (!ue.uid.isEmpty()) pushUndo(t, ue);
    }
    invalidateAnnotPage(t, page);
    t->dirty = true; updateTabDirty(t);
    if (t->view) t->view->clearSelectedAnnot();
    m_selPage = -1; m_selIdx = -1;
    refreshAnnotVisuals(t, page);
    t->pagesNeedGenerate.insert(page);
    // editSelectedAnnot is always FreeText path → keep slow (page objects)
                if (baseIsVector(t, page)) {
                    if (t->renderer) t->renderer->invalidatePage(page);
                    if (t->view) t->view->update();
                } else {
                    if (t->renderer) { t->renderer->invalidatePage(page); if (!m_fastMode) t->renderer->requestPage(page, t->zoom); }
                    if (t->view) t->view->invalidateTiles();
                    if (t->view) t->view->invalidateSharp();
                }
                m_selPage = -1; m_selIdx = -1;
                clearMarkupSelectionViews(t);
                refreshCommentsForPage(t, page);
}

// 0927 LUOT 6 (VIEC 1) --ftngoai-edit-probe: chay DUNG editSelectedAnnot()
// (ham cua muc "Edit text…" tren menu chuot phai) nhung tu dong dien text vao
// hop nhap nhieu dong va bam OK. VY TRONG: mot QTimer::singleShot chay TRUOC
// khi goi editSelectedAnnot() no; den luc NoteInputDialog::exec() mo len, timer
// da san sang va tim thay hop bang cach so cac cua so con dang MO (khong dua
// vao activeModalWidget() — do tren Windows trong exec() long no co the NULL,
// chinh la nguyen do --textdlg-probe bi treo 27/09). Sau khi accept(),
// editSelectedAnnot() chay tiep binh thuong: retextNote + invalidate + undo.
// 0927 LUOT 6 (VIEC 2): bat/tat Dark Mode qua CHINH QAction tren toolbar
// (m_darkAct) bang trigger() — dung nhu nguoi dung bam nut. Khong goi thang
// applyTheme() de bo qua buoc check()/sync cua QAction va de harness do duong
// that cua nguoi dung, dung nhu --sidebar-edit-probe goi QPushButton::click().
bool MainWindow::probeSetDarkMode(bool dark) {
    if (!m_darkAct) return false;
    if (m_darkAct->isChecked() != dark) m_darkAct->trigger();
    // QAction::trigger() tren QAction co checked la toggle nen da doi; dam bao
    // trang thai cuoi dung (phong trong hoi trigger bi bo qua).
    if (m_darkAct->isChecked() != dark) {
        m_darkAct->setChecked(dark);
        applyTheme(dark);
    }
    return m_darkMode == dark;
}

QString MainWindow::probeEditAnnotViaGui(int page, int indexInList, const QString& text,
                                         QString* errOut) {
    auto fail = [errOut](const QString& m) {
        if (errOut) *errOut = m;
        return m;
    };
    auto* t = currentTab();
    if (!t || !t->annotMgr) return fail(QStringLiteral("khong mo duoc tab"));
    const auto& list = annotsForPage(t, page);
    if (indexInList < 0 || indexInList >= list.size())
        return fail(QStringLiteral("chi so %1 ngoai danh sach (%2)")
                        .arg(indexInList).arg(list.size()));
    const QString kind = list[indexInList].type;
    if (kind != QLatin1String("FreeText") && kind != QLatin1String("Note"))
        return fail(QStringLiteral("khong phai chu thich chu (loai=%1)").arg(kind));

    bool filled = false, accepted = false, sawDialog = false;
    // 🔴 0927 LUOT 6 (theo y reviewer): dem so vong poll. Truoc day, neu
    // tim thay hop nhung khong tim duoc QDialogButtonBox (hoac OK bi khoa), thi
    // `filled=true` nen poll tro ve ngay — TRONG KHI dlg.exec() van chay vo
    // hanh ⇒ treo vo han. Gio co tran: het 150 vong (80 ms x 150 = 12 s) thi
    // tu dong dong hop bang reject() de exec() thoat va probe bao loi.
    int polls = 0;
    QTimer poll;
    poll.setInterval(80);
    QObject::connect(&poll, &QTimer::timeout, [&]() {
        if (++polls > 150) {
            // Đặng đóng để đóng: activeModalWidget() đã dừ uổng
            // NULL (chính lý do --textdlg-probe treo 27/09) ⇒ quét cợ cợ sổ
            // đã mữ, ép kiểu QDialog rồi mối gọi reject() để exec() thoát.
            if (auto* d = qobject_cast<QDialog*>(QApplication::activeModalWidget())) d->reject();
            for (QWidget* w : QApplication::topLevelWidgets())
                if (auto* d = qobject_cast<QDialog*>(w))
                    if (d->isWindow() && d->isVisible() && d->findChild<QPlainTextEdit*>()
                        && d->windowTitle().contains(QStringLiteral("text"), Qt::CaseInsensitive)) {
                        d->reject(); break;
                    }
            poll.stop();
            return;
        }
        QWidget* dlgNow = nullptr;
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (!w->isWindow() || !w->isVisible()) continue;
            if (!w->findChild<QPlainTextEdit*>()) continue;
            if (!w->windowTitle().contains(QStringLiteral("text"), Qt::CaseInsensitive)) continue;
            dlgNow = w; break;
        }
        if (!dlgNow) return;
        sawDialog = true;
        if (filled) return;
        // Dien text QUA QPlainTextEdit::setPlainText: day la duong duy nhat
        // NoteInputDialog::text() doc (khong phai QLineEdit cua author) — nen
        // gia tri vao day CHINH LA gia tri ma retextNote() se nhan.
        if (auto* ed = dlgNow->findChild<QPlainTextEdit*>()) {
            ed->setPlainText(text);
            filled = true;
            // Bam OK bang chinh QPushButton cua QDialogButtonBox.
            if (auto* bb = dlgNow->findChild<QDialogButtonBox*>())
                if (auto* ok = bb->button(QDialogButtonBox::Ok))
                    if (ok->isEnabled()) { ok->click(); accepted = true; }
        }
    });
    poll.start();
    editSelectedAnnot(page, indexInList);   // <- ĐÚNG đường GUI
    poll.stop();
    if (!sawDialog) return fail(QStringLiteral("hop nhap khong mo"));
    if (!filled)   return fail(QStringLiteral("khong tim thay o nhieu dong trong hop"));
    if (!accepted) return fail(QStringLiteral("nut OK khong bam duoc"));
    return QString();
}

void MainWindow::onMergeFiles() {
    MergeDialog dlg(m_editor.get(), this);
    dlg.exec();
}

void MainWindow::onSignPdf() {
    auto* t = currentTab();
    if (!t || !t->doc->isOpen()) {
        QMessageBox::information(this, "Sign PDF", "Please open a PDF file first.");
        return;
    }

    SignDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;
    SignParams sp = dlg.params();

    if (!dlg.visibleSignature()) { performSign(t, sp); return; }

if (m_fastMode)
         m_viewFastAct->setChecked(false);

    statusBar()->showMessage("Drag a rectangle where the signature should appear…");
    QObject::disconnect(m_sigPickConn);
    m_sigPickConn = connect(t->view, &PdfGpuView::signatureRectPicked, this,
        [this, t, sp](int page, QRectF rectPt) {
            QObject::disconnect(m_sigPickConn);
            statusBar()->clearMessage();
            t->annotMgr->createSignatureDraft(page, rectPt, QStringLiteral("[ Signature ]\nMove to position,\nthen Finalize"));
            m_pendSp = sp;
            m_pendPage = page;
            m_pendActive = true;
            if (m_finalizeSigAct) m_finalizeSigAct->setVisible(true);
            if (m_cancelSigAct)   m_cancelSigAct->setVisible(true);
            statusBar()->showMessage("Move the signature to position, then click 'Finalize Signature'");
            refreshAnnotVisuals(t, page);
            t->pagesNeedGenerate.insert(page);
            forceRasterPage(t, page);
            if (t->renderer) { t->renderer->invalidatePage(page); if (!m_fastMode) t->renderer->requestPage(page, t->zoom); }
            if (t->view) t->view->invalidateTiles();
        });
    t->view->beginSignaturePick();
}

void MainWindow::performSign(DocTab* t, SignParams sp) {
    QString outPath = QFileDialog::getSaveFileName(
        this, "Save Signed PDF As",
        QFileInfo(t->doc->filePath()).completeBaseName() + "_signed.pdf",
        "PDF Files (*.pdf)");
    if (outPath.isEmpty()) return;
    if (!outPath.endsWith(".pdf", Qt::CaseInsensitive)) outPath += ".pdf";

    QString srcPath = t->doc->filePath();

    QApplication::setOverrideCursor(Qt::WaitCursor);

    auto* watcher = new QFutureWatcher<QPair<bool,QString>>(this);
    connect(watcher, &QFutureWatcher<QPair<bool,QString>>::finished, this,
            [this, watcher, outPath]() {
        QApplication::restoreOverrideCursor();
        auto result = watcher->result();
        bool ok = result.first;
        QString err = result.second;
        watcher->deleteLater();

        if (ok) {
            auto reply = QMessageBox::question(this, "Signing Successful",
                "Document signed successfully.\nOpen the signed file?",
                QMessageBox::Yes | QMessageBox::No);
            if (reply == QMessageBox::Yes)
                openFile(outPath);
        } else {
            QMessageBox::warning(this, "Signing Failed", err);
        }
    });
    watcher->setFuture(QtConcurrent::run([srcPath, outPath, sp]() -> QPair<bool,QString> {
        QString errorMsg;
        bool ok = PdfSigner::signDocument(srcPath, outPath, sp, errorMsg);
        return {ok, errorMsg};
    }));
}

void MainWindow::onFinalizeSignature() {
    auto* t = currentTab();
    if (!t || !m_pendActive) return;
    int idx = -1;
    QRectF rect = t->annotMgr->findSignatureDraftRect(m_pendPage, &idx);
    if (idx < 0) { QMessageBox::warning(this, "Sign", "Signature draft not found."); return; }
    t->annotMgr->removeAnnot(m_pendPage, idx);
    t->pagesNeedGenerate.insert(m_pendPage);
    refreshAnnotVisuals(t, m_pendPage);
    SignParams sp = m_pendSp;
    sp.pageIndex = m_pendPage;
    sp.rectPt = rect;
    m_pendActive = false; m_pendPage = -1;
    if (m_finalizeSigAct) m_finalizeSigAct->setVisible(false);
    if (m_cancelSigAct)   m_cancelSigAct->setVisible(false);
    statusBar()->clearMessage();
    if (t->renderer) { t->renderer->invalidatePage(t->currentPage); if (!m_fastMode) t->renderer->requestPage(t->currentPage, t->zoom); }
    if (t->view) t->view->invalidateTiles();
    performSign(t, sp);
}

void MainWindow::onCancelSignature() {
    auto* t = currentTab();
    if (t && m_pendActive) {
        int idx = -1;
        t->annotMgr->findSignatureDraftRect(m_pendPage, &idx);
        if (idx >= 0) {
            t->annotMgr->removeAnnot(m_pendPage, idx);
            t->pagesNeedGenerate.insert(m_pendPage);
        }
        refreshAnnotVisuals(t, m_pendPage);
        if (t->renderer) { t->renderer->invalidatePage(m_pendPage); if (!m_fastMode) t->renderer->requestPage(t->currentPage, t->zoom); }
        if (t->view) t->view->invalidateTiles();
    }
    m_pendActive = false; m_pendPage = -1;
    if (m_finalizeSigAct) m_finalizeSigAct->setVisible(false);
    if (m_cancelSigAct)   m_cancelSigAct->setVisible(false);
    statusBar()->clearMessage();
}

// Split the open PDF into single-page files named:
//   "<base> - <bookmark title> - <NNN>.pdf"  (or "<base> - <NNN>.pdf" if no bookmark)
// into a "<base> - pages" subfolder next to the source.
void MainWindow::onExtractAll() {
    auto* t = currentTab();
    if (!t || !t->doc || !t->doc->isOpen()) {
        QMessageBox::information(this, "Extract All", "Please open a PDF first.");
        return;
    }

    const QString srcPath = t->doc->filePath();
    QFileInfo fi(srcPath);
    const int total = t->doc->pageCount();
    if (total <= 0) return;

    // Sanitize: strip illegal filename chars, collapse spaces, cap length.
    auto sanitize = [](QString s) -> QString {
        s.replace(QRegularExpression("[\\\\/:*?\"<>|\\r\\n\\t]"), " ");
        s = s.simplified();
        if (s.size() > 80) s = s.left(80).trimmed();
        return s;
    };

    // Map page index → first bookmark title that targets it (PDFium outline walk).
    FPDF_DOCUMENT doc = t->doc->raw();
    QHash<int, QString> pageTitle;
    std::function<void(FPDF_BOOKMARK)> walk = [&](FPDF_BOOKMARK bm) {
        while (bm) {
            unsigned long len = FPDFBookmark_GetTitle(bm, nullptr, 0);
            std::vector<char> buf(len + 2, 0);
            FPDFBookmark_GetTitle(bm, buf.data(), len);
            QString title = QString::fromUtf16(
                reinterpret_cast<const char16_t*>(buf.data())).trimmed();
            FPDF_DEST dest = FPDFBookmark_GetDest(doc, bm);
            if (!dest) {
                FPDF_ACTION a = FPDFBookmark_GetAction(bm);
                if (a && FPDFAction_GetType(a) == PDFACTION_GOTO)
                    dest = FPDFAction_GetDest(doc, a);
            }
            int page = dest ? FPDFDest_GetDestPageIndex(doc, dest) : -1;
            if (page >= 0 && !title.isEmpty() && !pageTitle.contains(page))
                pageTitle.insert(page, title);
            if (FPDF_BOOKMARK child = FPDFBookmark_GetFirstChild(doc, bm))
                walk(child);
            bm = FPDFBookmark_GetNextSibling(doc, bm);
        }
    };
    walk(FPDFBookmark_GetFirstChild(doc, nullptr));

    const QString base  = sanitize(fi.completeBaseName());
    const int     width = QString::number(total).size();
    QStringList names;
    for (int i = 0; i < total; ++i) {
        QString num   = QString("%1").arg(i + 1, width, 10, QChar('0'));
        QString title = sanitize(pageTitle.value(i));
        // Số thứ tự ở ĐẦU tên file (zero-pad theo tổng số trang: >100 → 001, >1000 → 0001).
        names << (title.isEmpty() ? QString("%1 - %2").arg(num, base)
                                  : QString("%1 - %2 - %3").arg(num, base, title));
    }

    const QString outDir = fi.absolutePath() + "/" + base + " - pages";
    QDir().mkpath(outDir);

    QApplication::setOverrideCursor(Qt::WaitCursor);
    PdfEditor* editor = m_editor.get();
    auto* watcher = new QFutureWatcher<int>(this);
    connect(watcher, &QFutureWatcher<int>::finished, this,
            [this, watcher, outDir]() {
        watcher->deleteLater();
        QApplication::restoreOverrideCursor();
        int n = watcher->result();
        if (n >= 0) {
            QMessageBox::information(this, "Extract All",
                QString("Extracted %1 pages to:\n%2")
                    .arg(n).arg(QDir::toNativeSeparators(outDir)));
            QDesktopServices::openUrl(QUrl::fromLocalFile(outDir));
        } else {
            QMessageBox::warning(this, "Extract All", m_editor->lastError());
        }
    });
    watcher->setFuture(QtConcurrent::run([editor, srcPath, names, outDir]() -> int {
        return editor->extractAllPages(srcPath, names, outDir);
    }));
}

void MainWindow::onPrintFile() {
    auto* t = currentTab();
    if (!t || !t->doc->isOpen()) return;
    PrintDialog::print(t->doc.get(), this);
}

// ── Sidebar sync ─────────────────────────────────────────────────────────────

void MainWindow::syncSidebarToTab(int docIdx, bool forceRebuild) {
    if (docIdx < 0 || docIdx >= m_openDocs.size()) {
        m_thumbPanel->clearThumbnails();
        return;
    }
    auto* t = m_openDocs[docIdx];
    m_thumbPanel->setDocument(t->doc.get(), t->renderer.get(),
                              t->thumbPool.get(), forceRebuild);
    m_thumbPanel->setCurrentPage(t->currentPage);
    if (t->annotMgr && t->doc && t->doc->isOpen()) {
        m_thumbPanel->setAnnotMgr(t->annotMgr.get(), t->doc->pageCount());
        if (t->annotCacheValid)
            m_thumbPanel->setComments(t->annotCache);
        else {
            m_thumbPanel->setComments({});
            if (m_thumbPanel->isCommentsTabVisible())
                onCommentsRequested();
        }
    }
}

// ── Tab helpers ───────────────────────────────────────────────────────────────

DocTab* MainWindow::currentTab() const {
    QWidget* w = m_docTabs->currentWidget();
    for (auto* t : m_openDocs)
        if (t->view == w) return t;
    return nullptr;
}

// LƯỢT 35 — man chao MOT NGUON DUY NHAT: tab "Welcome" la PdfView (widget thuong,
// ve chu bang CPU). An ContinuousView khi khong con tai lieu nao (ve chu cua no len
// viewport QOpenGLWidget bi vo sau khi GL resource cua cac tab bi huy). KHONG doi
// m_fastMode — mo lai tai lieu thi setViewMode(true) tu khoi phuc layout.
void MainWindow::applyWelcomeVisibility() {
    if (m_openDocs.isEmpty()) {
        m_docTabs->setMinimumHeight(0);
        m_docTabs->setMaximumHeight(QWIDGETSIZE_MAX);   // bo setFixedHeight cua fast mode
        if (m_continuousView) m_continuousView->hide();
    } else if (m_fastMode) {
        m_docTabs->setFixedHeight(m_docTabs->tabBar()->sizeHint().height());
        if (m_continuousView) m_continuousView->show();
    }
}

PdfView* MainWindow::addWelcomeTab() {
    auto* pv = new PdfView(m_docTabs);
    pv->setDarkMode(m_darkMode);   // tab chao khong nam trong m_openDocs → tu dong bo theme
    m_docTabs->addTab(pv, "Welcome");
    return pv;
}

// Reload after in-place file modification (delete page, reorder, etc.)
// Reopen a tab's viewer/renderer/thumbnails on `path` (no file swap).
// Shared by edit (working copy) and Save/Save As (original). The thumbnail pool
// keeps its OWN FPDF_LoadDocument handles that LOCK the file on Windows, so it is
// closed before and reopened after — otherwise a later overwrite would fail.
void MainWindow::loadTabFile(DocTab* t, const QString& path, bool structureChanged) {
    // 0927 LƯỢT 8: đường "LƯU RỒI NẠP LẠI" dùng CHUNG `shutdownTab()` — trước đây nó
    // tự chế thứ tự riêng và SAI theo đúng kiểu đã gặp: `t->doc->close()` (PageCache
    // forgetDocument + FPDF_CloseDocument + UnmapViewOfFile) chạy TRƯỚC
    // `renderer->setDocument()` (cancel + chờ + đóng 12 doc pool) ⇒ 12 doc pool còn
    // sống trên buffer đã unmap, và task render có thể đang giữ FPDF_PAGE của chúng.
    // Cũng thiếu luôn `closeHeavyPriv` + chờ quét annot.
    shutdownTab(t, "naiLai");
    t->annotMgr->resetScan();   // stopScan() là then chốt MỘT CHIỀU — phải mở lại cho tài liệu mới
    // Doc sap dong/mo lai: cache link theo trang cu phai xoa (SPEC_PDF_LINKS).
    PdfLinks::clearCache();
    t->doc->close();
    t->renderer->setTileCache(nullptr);

    t->doc->open(path);
    t->annotCacheValid = false;
    t->annotPageCache.clear();
    t->overlayCapablePage.clear();
    t->visualsCache.clear();
    t->visualsUnreadable.clear();   // 33g: danh dau unreadable/retry/heavyDue la cua tai lieu CU
    t->visualsRetry.clear();
    t->visualsHeavyDue.clear();
    t->visualsRev.clear();
    t->visualsHasForeign.clear();
    t->pagesNeedGenerate.clear();
    t->vecLayers.clear();   // KHO CHUNG: moi tai lieu = kho rong
    t->vecBuilding.clear();
    t->forceRasterPages.clear();   // dieu kien "bat buoc raster" la cua tai lieu CU
    t->torvecDirty.clear();        // co "trang da sua" cung la cua tai lieu CU
    t->heavyRegionPages.clear();   // LÁT C: che do region cung thuoc tai lieu CU
    t->heavyRegionCancel.reset();
    t->heavyRegionBuilding = false;
    t->heavyRegionPage = -1;
    t->heavyRegionScale = 0.0;
    t->heavyRegionPx = QRect();
    // Lop bu cua tai lieu CU vo nghia voi doc moi — reset de du lieu sau khong dung nham.
    t->fgnLayer.reset();
    t->fgnBuilding.clear();
    t->fgnRegionBuilding = false;
    t->renderer->setDocument(t->doc.get());
    t->annotMgr->setDocument(t->doc->raw(), path);
    if (t->annotLayer) {
        t->annotLayer->setDocument(t->doc->raw());
        t->annotLayer->setAnnotationManager(t->annotMgr.get());
    }

    // 0927 M2: cache .torcache phai co TRUOC pool thumbnail (pool muon CHUNG doi
    // tuong cua tab) va truoc khi doi hash sang tai lieu moi.
    {
        uint64_t hash = TileCacheFile::hashFile(path);
        uint64_t sz   = static_cast<uint64_t>(QFileInfo(path).size());
        // Luu hash de 4 cho build .torvec dung lai (SPEC_PERF_HEAVYPAGE buoc B).
        t->pdfHash = hash;
        t->pdfPath = path;
        // 0927 M3: cache CU (hash cua tai lieu CU) chi "chet nhe" trong bo dem —
        // `make_shared` moi ghi de `t->tileCache` nen file .torcache cu KHONG BAO
        // GIO duoc xoa, chi con lai tren dia cho tới lan khoi dong sau. Phai nha +
        // xoa TRUOC khi acquire moi (duong dan moi doi vi hash doi).
        if (t->tileCache) {
            const auto rel = TileCacheRegistry::release(t->tileCache, "taiLai");
            if (rel.kept)
                qDebug().noquote() << QString("[torcache] GIU ok=1 %1 ly do=taiLai (giu file)").arg(rel.path);
            else if (rel.removed)
                qDebug().noquote() << QString("[torcache] XOA ok=1 %1 ly do=taiLai loi=").arg(rel.path);
            else
                qDebug().noquote() << QString("[torcache] XOA ok=0 %1 ly do=taiLai loi=%2")
                                         .arg(rel.path)
                                         .arg(rel.stillUsed > 0
                                              ? QStringLiteral("conTab=%1").arg(rel.stillUsed)
                                              : (rel.error.isEmpty() ? QStringLiteral("khongCoTrongRegistry")
                                                                    : rel.error));
        }
        t->tileCache = TileCacheRegistry::acquire(path, hash, sz, t->doc->pageCount());
        if (t->tileCache->isOpen())
            t->renderer->setTileCache(t->tileCache);
    }

    // 0927 M2: tra `t->tileCache` cho pool de ca hai ghi cung MOT doi tuong
    // (truoc day pool tu mo them 1 doi tuong rieng tren cung duong dan .torcache).
    if (t->thumbPool && !t->thumbPool->open(path, t->doc->raw(), 0, 0, t->doc->pageCount(), t->tileCache))
        t->thumbPool.reset(); // fallback: thumbnail panel uses PdfRenderer
    else {
        pauseThumbnails(t);
        // Dung san TOAN BO thumbnail khi mo file (moc TOT owner xac nhan 31/08).
        // An toan vi thumbnail bi chan o kThumbHandleQuota, va tab NEN bi tam dung (duoi).
        if (t->thumbPool) {
            const int n = t->doc->pageCount();
            qDebug().noquote() << "[thumb] dung san TOAN BO" << n << "trang";
            t->thumbPool->prefetchRange(0, n - 1);
        }
    }

    // 🔴 2026-08-31: AP DUNG chinh che do dang chon khi mo tai lieu.
    // Truoc day setViewMode() CHI duoc goi tu thanh cong cu (dong ~1164). setChecked() luc
    // khoi tao KHONG phat triggered(), nen nut sang "View Fast" ma chua ai bat che do do —
    // mo PDF len van la Single. Day la cho lam nut lech voi thuc te.
    if (m_fastMode)
        setViewMode(true);

    refreshAnnotVisuals(t, t->currentPage);
    if (!m_fastMode)
        t->renderer->requestPage(t->currentPage, t->zoom);
    // R1: pin lai trang hien tai +-1 sau reload.
    const int pg0 = t->currentPage;
    if (pg0 - 1 >= 0) PageCache::pin(t->doc->raw(), pg0 - 1);
    PageCache::pin(t->doc->raw(), pg0);
    if (pg0 + 1 < t->doc->pageCount()) PageCache::pin(t->doc->raw(), pg0 + 1);

    // Prefetch trang lien ke sau khi mo/mo lai (SPEC_PAGECACHE_CORE muc 4).
    schedulePagePrefetch();

    // ── Vector overlay: rebuild for current page after reload ──
    {
        int pg = t->currentPage;
        t->vecBuilding.insert(pg);
        auto layer = std::make_shared<VectorLayer>();
        auto* vw = new QFutureWatcher<bool>(this);
        connect(vw, &QFutureWatcher<bool>::finished, this, [this, vw, t, pg, layer, s = t->serial]{
            vw->deleteLater();
            if (!tabAlive(t, s)) return;   // 🔴 L20: chốt TRƯỚC khi dereference
            t->vecBuilding.remove(pg);
            if (t->currentPage != pg) return;
            if (t != currentTab()) return;
            if (vw->result()) {
                setTabVectorLayer(t, layer, pg);
            } else {
                setTabVectorLayer(t, nullptr, pg);
            }
        });
        FPDF_DOCUMENT d = t->doc->raw();
        const QString pdfPath = t->pdfPath;
        const quint64 pdfHash = t->pdfHash;
        const QString docPath = path;
        const bool allowCache = !t->torvecDirty.contains(pg);
        // 0928: vet huy theo lat — trang da bi bo roi trong khi build thi dung viec.
        const quint32 myGen = t->vecGen->load(std::memory_order_acquire);
        auto gen = t->vecGen;   // 🔴 L22: shared_ptr — sống cùng lambda, tab chết không kéo theo
        // 🔴 LƯỢT 22: token lúc SPAWN + RAII move; lambda KHÔNG còn cầm `t`
        // (bản cũ đọc t->vecGen/t->currentPage trên luồng nền — reviewer mục 2:
        // `delete t` nền chạy lúc task còn xếp hàng ⇒ UAF). vecGen qua shared_ptr;
        // `currentPage != pg` BỎ: vecGen đã tăng MỖI LẦN đổi trang (6455) — check
        // đó thừa mà lại là deref `t`; task.close có token + cancelled() che.
        trdoc::Task task(d, "VectorLayer::build/naiLai");
        vw->setFuture(QtConcurrent::run([layer, d, pg, pdfPath, pdfHash, docPath, allowCache,
                                        gen, myGen, task = std::move(task)]{
            // Thu cache .torvec truoc — nap nhanh gap 45 lan so voi dung lai tu PDF.
            // Khoa cache co the chua kip dat (initWatcher chay bat dong bo). Tu bu: hashFile chi
            // doc 128 KB (64 KB dau + 64 KB cuoi) nen re, an toan goi o luong nen.
            const QString keyPath = pdfPath.isEmpty() ? docPath : pdfPath;
            const quint64 keyHash = pdfHash ? pdfHash : (quint64)TileCacheFile::hashFile(keyPath);
            if (allowCache && VectorCache::tryLoad(*layer, keyPath, keyHash, pg)) return true;
            const auto huy = [gen, myGen, &task] {
                return gen->load(std::memory_order_acquire) != myGen
                    || task.cancelled();
            };
            if (!layer->build(d, pg, huy)) return false;
            if (allowCache) VectorCache::trySave(*layer, keyPath, keyHash, pg);
            return true;
        }));
    }
    if (t == currentTab()) {
        // File vừa được mở lại (edit/save) trên cùng con trỏ doc → ép sidebar
        // dựng lại thumbnail + bookmark, nếu không panel giữ nội dung cũ.
        syncSidebarToTab(m_openDocs.indexOf(t), /*forceRebuild=*/structureChanged);
        if (m_thumbPanel && m_thumbPanel->isCommentsTabVisible())
            onCommentsRequested();
if (m_fastMode && m_continuousView) {
             // GOC1: use user's zoom, not kFullRenderMaxPx
             qDebug() << "[perf] cont reload fast mode zoom=" << t->zoom;
             m_continuousView->setZoom(t->zoom);
              m_continuousView->setDocument(t->doc.get(), t->renderer.get(), &t->visualsCache, &t->vecLayers);
              m_continuousView->setVectorCacheKey(t->pdfPath.isEmpty() ? t->doc->filePath() : t->pdfPath, t->pdfHash);
         }
    }
}

// Update the tab label + window title to reflect the dirty (unsaved) state.
void MainWindow::updateTabDirty(DocTab* t) {
    if (!t || !t->view) return;
    int idx = m_docTabs->indexOf(t->view);
    if (idx < 0) return;
    QString name  = QFileInfo(t->originalPath).fileName();
    QString label = (t->dirty ? "● " : "") + name;   // ● prefix when unsaved
    m_docTabs->setTabText(idx, label);
    if (t == currentTab())
        setWindowTitle("TorReader PDF — " + label);
}

// Apply an edit result (`tmpPath`) as the tab's in-memory WORKING COPY.
// The original file on disk is NOT touched — the user must Save to overwrite it.
// (filePath = the edited input; unused now that we no longer overwrite in place.)
void MainWindow::reloadTab(DocTab* t, const QString& filePath, const QString& tmpPath) {
    Q_UNUSED(filePath);
    if (t->originalPath.isEmpty()) t->originalPath = t->doc->filePath();

    const QString curPath = t->doc->filePath();
    QString oldWorking;
    if (t->dirty) {
        QFileInfo cfi(curPath);
        QFileInfo ofi(t->originalPath);
        QString c = cfi.canonicalFilePath();
        QString o = ofi.canonicalFilePath();
        if (c.isEmpty()) c = cfi.absoluteFilePath();
        if (o.isEmpty()) o = ofi.absoluteFilePath();
        if (c != o)
            oldWorking = curPath;
    }

    QString working = makeTmpPath(t->originalPath);
    QFile::remove(working);
    if (!QFile::rename(tmpPath, working) && QFile::exists(tmpPath)) {
        QFile::copy(tmpPath, working);
        QFile::remove(tmpPath);
    }

    loadTabFile(t, working);

    if (!oldWorking.isEmpty() && oldWorking != working)
        removeWorkingCopy(oldWorking);

    t->dirty = true;
    updateTabDirty(t);
    statusBar()->showMessage("Edited — press Ctrl+S to save (or Save As)", 4000);
}

// ── Open file (async — loads PDF in background, UI stays responsive) ──────────

void MainWindow::openFile(const QString& path) {
    if (m_openDocs.isEmpty() && m_docTabs->count() == 1) {
        QWidget* w = m_docTabs->widget(0);
        m_docTabs->removeTab(0);
        delete w;
    }

    QString name = QFileInfo(path).fileName();

    auto* tab     = new DocTab;
    tab->originalPath = path;   // real file on disk; edits keep it untouched until Save
    tab->doc      = std::make_unique<PdfDocument>();
    // 🔴 0928 LƯỢT 22 (reviewer mục 4): KHÔNG parent = MainWindow. Ba object này
    // chết trong `delete t` ở closeJob — LUỒNG NỀN. Là con của MainWindow thì
    // ~QObject sẽ gỡ chúng khỏi danh sách children của MainWindow từ luồng nền,
    // đúng lúc luồng UI đang new QFutureWatcher(this) (thêm vào cùng danh sách)
    // ⇒ data race. unique_ptr trong DocTab ĐÃ sở hữu chúng — parent chỉ thừa.
    // An toàn khi delete nền: mọi kết nối 2 chiều của chúng bị ngắt TRÊN UI THREAD
    // ở onTabClose (disconnect nullptr-trước-sau) trước khi closeJob chạy; phần
    // nặng (12 handle pool pdfium) vẫn xuống nền như LƯỢT 16 — không gì chạy
    // QTimer/event trên các object này nên không cần deleteLater.
    tab->renderer = std::make_unique<PdfRenderer>(nullptr);
    tab->annotMgr = std::make_unique<AnnotationManager>(nullptr);
    tab->view     = new PdfGpuView(m_docTabs);
    // LÁT A 09/02: Single view đọc kho chú thích duy nhất của CHÍNH tab này.
    tab->view->setVisualsStore(&tab->visualsCache);
    // LÁT B 09/02: tương tự cho kho lớp vector — view per-tab, gắn &tab->vecLayers
    // một lần lúc tạo, chết cùng tab (PdfGpuView chỉ đọc qua con trỏ này).
    tab->view->setVectorStore(&tab->vecLayers);
    tab->view->setDarkMode(m_darkMode);
    tab->view->setViewMode(PdfGpuView::ViewMode::Single);
    tab->view->beginLoading();
    connect(tab->view, &PdfGpuView::textRegionSelected,
            this, &MainWindow::onTextRegionSelected);
    connect(tab->view, &PdfGpuView::textSelectionChanged,
            this, &MainWindow::onTextSelectionChanged);
    connect(tab->view, &PdfGpuView::textSelectionCleared,
            this, &MainWindow::onTextSelectionCleared);
    connect(tab->view, &PdfGpuView::copySelectionRequested,
            this, &MainWindow::onCopySelectionRequested);
    // Link (SPEC_PDF_LINKS): doc link tu PdfDocument cua tab nay.
    tab->view->setLinksDocument(tab->doc.get());
    connect(tab->view, &PdfGpuView::linkHovered,
            this, [this](const QString& txt) {
        statusBar()->showMessage(txt, 4000);
    });
    connect(tab->view, &PdfGpuView::linkActivated,
            this, [this, tab](int page, const PdfLink& link) {
        onLinkActivated(tab, page, link);
    });

    m_openDocs.append(tab);
    m_docTabs->addTab(tab->view, name + "…");
    m_docTabs->setCurrentWidget(tab->view);
    statusBar()->showMessage("Opening: " + name + "  (large files may take a moment…)");
    // 🔴 LƯỢT 33b (J): đánh dấu trang 0 của tab ĐANG MỞ là khẩn NGAY lúc này (trước
    // khi doc mở xong) để thumbnail/annot-visuals của tab nền VÀ của chính tab mới
    // NHƯỜNG s_pdfiumMutex cho lượt vẽ trang đầu — tránh 3 lượt FPDF_LoadPage trang
    // nặng xếp hàng sau nhau. ContinuousView xoá khi nhận ảnh (ACCEPT); trần 1,5 s
    // (đủ cho trang đầu nhẹ về; trang quái vật quá trần thì nền vẽ bình thường trở
    // lại — KHÔNG được giữ khẩn lâu vì nó chặn annot/thumbnail của bước khác, xem step C/K).
    g_pdfiumUrgentPage().store(0, std::memory_order_release);
    g_pdfiumUrgentPageDeadline().store(QDateTime::currentMSecsSinceEpoch() + 1500,
                                       std::memory_order_release);
    // 🔴 LƯỢT 33f (mục 2): doc chưa mở xong ⇒ token 0 = "tab đang mở" — mọi tab đã
    // load coi là KHÁC doc ⇒ trang hiển thị của chúng nhường (giữ nguyên đường J).
    g_pdfiumUrgentDoc().store(0, std::memory_order_release);

    // ── Connect per-tab signals (before async load, safe — renderer not yet set) ──
    tab->scrollConn = connect(
        tab->view, &PdfGpuView::scrolledToPage,
        this, [this, tab](int pageIdx) {
            if (tab == currentTab()) onPageChanged(pageIdx);
        });

    connect(tab->view, &PdfGpuView::zoomChanged, this, [this, tab](double z) {
        tab->zoom = z;
        if (tab == currentTab())
            onZoomChanged(z);
        // SU KIEN 0902 (thay dong ho polling 1s gay vang app): zoom doi → vung nhin
        // trang nang co the vuot anh region cu. updateHeavyRegion tu guard day du.
        updateHeavyRegion(tab);
    });
    // SU KIEN 0902 — PAN TRONG TRANG khong phat tin hieu nao cua PdfGpuView
    // (wheelEvent chi update(); tilesNeeded bi chan early-return khi nen vector).
    // QEvent::Paint la han noi cua Qt: den DUNG MOT LAN moi dam repaint, SAU khi
    // pan offset da cap nhat — bo dem "vung nhin co the doi" khong poll, khong
    // dong ho. Gia mot so sanh danh dien vung nhin trong updateHeavyRegion.
    tab->view->installEventFilter(this);

    // ── Tile wiring ───────────────────────────────────────────────────────────
    connect(tab->view, &PdfGpuView::tilesNeeded,
            tab->renderer.get(), &PdfRenderer::requestRegion);
    connect(tab->renderer.get(), &PdfRenderer::regionReady,
            tab->view, [tab](int page, double scale, QRect regionPx, QImage img) {
        if (tab->view) tab->view->setRegion(page, scale, regionPx, img);
    });

    // ── Lop annot phan mem khac: dung vung sac net theo zoom ──
    connect(tab->view, &PdfGpuView::tilesNeeded, this,
            [this, tab, s = tab->serial](int page, double scale, QRect regionPx) {
        if (!tabAlive(tab, s) || !tab->doc || !tab->doc->isOpen()) return;
        if (!tab->visualsHasForeign.value(page, false)) return;
        if (!baseIsVector(tab, page)) return;
        if (!(tab->fgnLayer && tab->fgnLayer->pageIndex() == page)) return;
        if (tab->fgnRegionBuilding) return;
        tab->fgnRegionBuilding = true;
        auto fl = tab->fgnLayer;
        FPDF_DOCUMENT d = tab->doc->raw();
        auto* wr = new QFutureWatcher<bool>(this);
        connect(wr, &QFutureWatcher<bool>::finished, this,
                [this, wr, tab, fl, page, scale, regionPx, s = tab->serial]{
            wr->deleteLater();
            if (!tabAlive(tab, s)) return;   // tab da dong — t co the da bi xoa
            tab->fgnRegionBuilding = false;
            if (tab->fgnRegionFuture.isValid()) tab->fgnRegionFuture = QFuture<bool>();
            if (wr->result() && tab->view)
                tab->view->setForeignAnnotRegion(page, scale, regionPx, fl->regionImage());
        });
        // 🔴 LƯỢT 22: token lúc SPAWN + bgSync (slot fgnRegionFuture còn bị lệnh ở
        // 760 — đường Continuous — ghi đè song song với đường này).
        trdoc::Task task(d, "fgnlayer/buildRegion");
        tab->fgnRegionFuture = QtConcurrent::run([fl, d, page, scale, regionPx,
                                                  task = std::move(task)]{
            return fl->buildRegion(d, page, scale, regionPx);
        });
        wr->setFuture(tab->fgnRegionFuture);
        tab->addBgWait(tab->fgnRegionFuture);   // L22/L22b
    });

    // ── Annotation signals ────────────────────────────────────────────────────
    // 🔴🔴 GOC THAT 2026-09-01 — "tao Note/Text xong khong hien gi".
    // `createPopupNote` / `createInlineNote` phat `annotationAdded`, KHONG phat
    // `pageContentChanged`. Ma trong MainWindow **KHONG AI LANG NGHE** `annotationAdded`
    // ⇒ tao xong thi giao dien khong he duoc bao ⇒ khong quet lai visuals, khong ve lai anh
    //   trang ⇒ tren man hinh khong co gi doi. Phai cho toi khi mot su kien khac (lat trang,
    //   doi che do) tinh co ep ve lai thi Text moi hien; con Note thi khong bao gio.
    // Do bang PIXEL (--newannot-probe): tao Note ok=1 nhung so pixel doi = 0 o CA HAI che do.
    // ⚠️ Truoc do tôi vá vào `pageContentChanged` — DUNG CO CHE nhung SAI TIN HIEU, nen vo hieu.
    connect(tab->annotMgr.get(), &AnnotationManager::annotationAdded, this,
            [this, tab, s = tab->serial](int page, AnnotInfo) {
        if (!tabAlive(tab, s)) return;
        qDebug().noquote() << "[annot] annotationAdded page=" << page << "— quet lai + ve lai";
        refreshAnnotVisuals(tab, page);   // LÁT C 0902: duong nay gio la TRON DELTA — hien ngay, khong cho khoa
        if (m_thumbPanel && m_thumbPanel->isCommentsTabVisible()) refreshCommentsForPage(tab, page);
        // 🔴 LÁT C 0902 — goc cua "Note thi khong hien": Note/FreeText khong do overlay
        // ve, chung song trong ANH RASTER. Nen dang la vector thi lenh ve lai o duong
        // duoi bi chan (drop reason=vectorReady) va chu thich VO HINH VINH VIEN.
        // Cung cach ma applyMarkupRefresh (undo/redo) van dung — o day thieu no.
        forceRasterPage(tab, page);
        if (tab->renderer) {
            tab->renderer->markAnnotDirty(page);
            // 🔴 GO BO 2026-09-01: da thu goi reloadHandlePool() o day de ban sao co chu thich moi.
            // KEM ON DINH HON: nap lai se DONG cac ban sao ma cac luong ve DANG MUON ⇒ sap ngay
            // trong luot do (do duoc: 2/4 lan chay probe khong hoan tat).
            // Muon lam dung thi phai nap lai theo kieu HOAN LAI: cho moi ban sao duoc tra ve roi
            // moi thay, khong duoc dong khi con nguoi muon.   // xem PdfRenderer::markAnnotDirty
            tab->renderer->invalidatePage(page);
            if (!m_fastMode && tab->view) tab->renderer->requestPage(page, tab->zoom);
            // 🔴🔴 2026-09-01 — GOC CUA "tao ben Single, sang Continuous khong thay dau".
            // Ban cu chi bao cho Continuous khi DANG o che do Fast. Tao chu thich o Single thi
            // nhanh nay bi bo qua ⇒ ContinuousView van giu ANH CU (chup tu truoc khi co chu
            // thich) ⇒ chuyen sang View Fast la thay trang thieu chu thich.
            // ContinuousView la MOT widget dung chung cho moi tab; bo anh cu cua no luon AN TOAN
            // ke ca khi dang an — lan sau hien len se tu ve lai.
            if (m_continuousView && tab == currentTab()) {
                m_continuousView->invalidatePage(page);
                if (m_fastMode) m_continuousView->datLaiLenhVeTrang(page);
            }
        }
        if (tab->view) tab->view->update();
        if (m_fastMode && m_continuousView) m_continuousView->viewport()->update();
    });

    connect(tab->annotMgr.get(), &AnnotationManager::pageContentChanged, this,
            [this, tab, s = tab->serial](int page) {
        if (!tabAlive(tab, s)) return;
        refreshAnnotVisuals(tab, page);
        if (m_thumbPanel && m_thumbPanel->isCommentsTabVisible()) refreshCommentsForPage(tab, page);
        // 🔴🔴 2026-09-01 — GOC CUA "new Text phai nhay trang qua lai moi hien".
        // Text/Note KHONG do overlay ve, chung nam trong ANH do PDFium dung. Tao moi mot cai
        // ma khong ve lai anh trang thi khong co gi thay doi tren man hinh — phai doi mot su
        // kien khac (lat trang / doi che do) tinh co ep ve lai thi no moi hien.
        // ⇒ Sua markup xong: LUON bo anh cu va dat ve lai, ke ca khi nen dang la vector.
        if (tab->renderer) {
            tab->renderer->markAnnotDirty(page);
            // 🔴 GO BO 2026-09-01: da thu goi reloadHandlePool() o day de ban sao co chu thich moi.
            // KEM ON DINH HON: nap lai se DONG cac ban sao ma cac luong ve DANG MUON ⇒ sap ngay
            // trong luot do (do duoc: 2/4 lan chay probe khong hoan tat).
            // Muon lam dung thi phai nap lai theo kieu HOAN LAI: cho moi ban sao duoc tra ve roi
            // moi thay, khong duoc dong khi con nguoi muon.   // xem PdfRenderer::markAnnotDirty
            tab->renderer->invalidatePage(page);
            if (!m_fastMode && tab->view) tab->renderer->requestPage(page, tab->zoom);
            // 🔴🔴 2026-09-01 — GOC CUA "tao ben Single, sang Continuous khong thay dau".
            // Ban cu chi bao cho Continuous khi DANG o che do Fast. Tao chu thich o Single thi
            // nhanh nay bi bo qua ⇒ ContinuousView van giu ANH CU (chup tu truoc khi co chu
            // thich) ⇒ chuyen sang View Fast la thay trang thieu chu thich.
            // ContinuousView la MOT widget dung chung cho moi tab; bo anh cu cua no luon AN TOAN
            // ke ca khi dang an — lan sau hien len se tu ve lai.
            if (m_continuousView && tab == currentTab()) {
                m_continuousView->invalidatePage(page);
                if (m_fastMode) m_continuousView->datLaiLenhVeTrang(page);
            }
        }
        if (baseIsVector(tab, page) && tab->vecLayers.contains(page) && tab->annotMgr && tab->doc) {
            {
                TimedPdfiumLock lk(__FILE__, __LINE__);
                FPDF_PAGE pg = PageCache::acquire(tab->doc->raw(), page);
                if (pg) {
                    PageCache::PageBorrow _b(tab->doc->raw(), page);   // R1: cap doi acquire()
                    tab->vecLayers.value(page)->rebuildNoteTiles(tab->doc->raw(), pg);
                }
            }
            if (tab->view) tab->view->invalidateTileTextures();
        }
        if (tab == currentTab() && tab->view) tab->view->update();
        // 🔴 CHE DO CONTINUOUS: annot NGOAI co overlayCapable=false, markup nam TRONG
        //    ANH RASTER => chi update() khong du, phai bo raster cu di roi ve lai.
        if (m_fastMode && m_continuousView && tab == currentTab()) {
            if (baseIsVector(tab, page)) {
                // 🔴 CUNG LOI voi nhanh cong cu Text (xem chu thich o ham tao Text): "chi update"
                // ma khong dat lenh ve lai ⇒ man hinh con moi lop vector, khong co chu thich.
                m_continuousView->invalidatePage(page);
                m_continuousView->datLaiLenhVeTrang(page);
                m_continuousView->viewport()->update();
                qDebug().noquote() << "[annot] pageContentChanged -> ve lai (vector) page=" << page;
            } else {
                m_continuousView->invalidatePage(page);
                // 🔴🔴 2026-09-01 — GOC CUA "Continuous khong hien, doi tab roi quay lai moi hien".
                // Nhanh nay BO anh cu roi chi ve lai man hinh, KHONG dat lenh render moi. No chay
                // NGAY SAU handler annotationAdded (vua render xong anh co chu thich) nen xoa
                // luon thanh qua do ⇒ tren man hinh trong tron, phai cho toi khi doi tab moi co
                // ai render lai. Them mot dong dat lenh, dung nhu ben annotationAdded.
                m_continuousView->datLaiLenhVeTrang(page);
                m_continuousView->viewport()->update();
                qDebug().noquote() << "[annot] pageContentChanged -> invalidate raster page=" << page;
            }
        }
    }, Qt::QueuedConnection);

    connect(tab->view, &PdfGpuView::shapeCommitRequested,
            this, [this, tab](int pageIdx, AnnotTool tool, QPointF start, QPointF end) {
        if (!tab->annotLayer) return;
        qDebug().noquote() << "[markup] signal=shapeCommitRequested page=" << pageIdx
                 << "tool=" << static_cast<int>(tool)
                 << "start=(" << start.x() << "," << start.y() << ")"
                 << "end=(" << end.x() << "," << end.y() << ")";
        AnnotStyle style = m_annotStyle;
        QElapsedTimer _perfC;
        _perfC.start();
        tab->annotLayer->commitAnnotation(pageIdx, tool, style, start, end, {});
        qDebug().noquote() << "[perf] markup commit ms=" << _perfC.elapsed();
        {
            MarkupUndoEntry ue; ue.kind = MarkupUndoEntry::AddShape; ue.page = pageIdx;
            ue.uid = tab->annotLayer->lastCreatedUid();
            ue.snap = tab->annotLayer->lastCreatedSnapshot();
            if (!ue.uid.isEmpty() && ue.snap.valid) pushUndo(tab, ue);
        }
        invalidateAnnotPage(tab, pageIdx);
        refreshAnnotVisuals(tab, pageIdx);
        { static const bool dump = qEnvironmentVariableIsSet("TORREADER_DUMP");
          if (dump) {
              TimedPdfiumLock lock(__FILE__, __LINE__);
              MarkupDebugWriter fw;
              fw.base.version = 1;
              fw.base.WriteBlock = MarkupDebugWriter::writeBlock;
              fw.file.setFileName(QCoreApplication::applicationDirPath() + "/markup_debug.pdf");
              bool openOk = fw.file.open(QIODevice::WriteOnly);
              bool saveOk = openOk && FPDF_SaveAsCopy(tab->doc->raw(), &fw.base, FPDF_NO_INCREMENTAL);
              if (fw.file.isOpen()) fw.file.close();
              FPDF_PAGE fpage = FPDF_LoadPage(tab->doc->raw(), pageIdx);
              int annotCount = fpage ? FPDFPage_GetAnnotCount(fpage) : -1;
              if (fpage) FPDF_ClosePage(fpage);
              qDebug().noquote() << "[dump] saved markup_debug.pdf ok=" << (saveOk ? "true" : "false") << "annots=" << annotCount;
          } }
        tab->dirty = true;
        updateTabDirty(tab);
        tab->pagesNeedGenerate.insert(pageIdx);
        QElapsedTimer _perfR; _perfR.start();
        if (canFastPath(tab, pageIdx)) {
            if (tab->view) tab->view->update();
            qDebug().noquote() << "[perf] markup FAST path page=" << pageIdx << "ms=" << _perfR.elapsed();
        } else {
            // Trang nay markup khong overlay duoc => phai ve bang raster. Neu dang lay lop
            // vector lam nen thi go no ra, neu khong raster bi chan (`drop reason=vectorReady`)
            // va markup se vo hinh (loi nguoi dung bao 2026-08-19: Cloud ve duoc khong hien).
            forceRasterPage(tab, pageIdx);
            if (tab->view) tab->view->addPendingMarkup(tool, style, start, end);
            qDebug().noquote() << "[perf] markup SLOW path (overlay incapable) page=" << pageIdx;
            if (tab->renderer) {
                if (tab->view) tab->view->invalidateTiles();
                if (tab->view) tab->view->invalidateSharp();
                tab->renderer->invalidatePage(pageIdx);
                if (!m_fastMode) tab->renderer->requestPage(pageIdx, tab->zoom);
            }
        }
        refreshCommentsForPage(tab, pageIdx);
    });

    connect(tab->view, &PdfGpuView::freehandCommitRequested,
            this, [this, tab](int pageIdx, const QVector<QPointF>& pts) {
        if (!tab->annotLayer) return;
        qDebug().noquote() << "[markup] signal=freehandCommitRequested page=" << pageIdx
                 << "pts=" << pts.size();
        AnnotStyle style = m_annotStyle;
        { QElapsedTimer _p; _p.start();
          tab->annotLayer->commitAnnotation(pageIdx, AnnotTool::Freehand, style, {}, {}, pts);
          qDebug().noquote() << "[perf] markup commit ms=" << _p.elapsed(); }
        {
            MarkupUndoEntry ue; ue.kind = MarkupUndoEntry::AddShape; ue.page = pageIdx;
            ue.uid = tab->annotLayer->lastCreatedUid();
            ue.snap = tab->annotLayer->lastCreatedSnapshot();
            if (!ue.uid.isEmpty() && ue.snap.valid) pushUndo(tab, ue);
        }
        tab->dirty = true;
        updateTabDirty(tab);
        invalidateAnnotPage(tab, pageIdx);
        tab->pagesNeedGenerate.insert(pageIdx);
        refreshAnnotVisuals(tab, pageIdx);
        QElapsedTimer _perfR2; _perfR2.start();
        if (canFastPath(tab, pageIdx)) {
            if (tab->view) tab->view->update();
            qDebug().noquote() << "[perf] markup FAST path page=" << pageIdx << "ms=" << _perfR2.elapsed();
        } else {
            forceRasterPage(tab, pageIdx);
            if (tab->view) tab->view->addPendingMarkup(AnnotTool::Freehand, style, QPointF(), QPointF(), pts);
            qDebug().noquote() << "[perf] markup SLOW path (overlay incapable) page=" << pageIdx;
            if (tab->renderer) {
                if (tab->view) tab->view->invalidateTiles();
                if (tab->view) tab->view->invalidateSharp();
                tab->renderer->invalidatePage(pageIdx);
                if (!m_fastMode) tab->renderer->requestPage(pageIdx, tab->zoom);
            }
        }
        refreshCommentsForPage(tab, pageIdx);
    });

    connect(tab->view, &PdfGpuView::noteRequested,
            this, [this, tab](int pageIndex, QPointF pdfPoint) {
        if (!tab) return;
        qDebug().noquote() << "[markup] signal=noteRequested page=" << pageIndex
                 << "point=(" << pdfPoint.x() << "," << pdfPoint.y() << ")";
        // giu nguyen singleLine mac dinh (false) — chi them tham so theme
        NoteInputDialog dlg({}, this, /*singleLine=*/false, m_darkMode);
        if (dlg.exec() != QDialog::Accepted) return;
        tab->annotMgr->createPopupNote(pageIndex, pdfPoint, dlg.text(), dlg.author());
        {
            MarkupUndoEntry ue; ue.kind = MarkupUndoEntry::AddNote; ue.page = pageIndex;
            ue.uid = tab->annotMgr->lastCreatedUid();
            ue.noteRect = QRectF(pdfPoint, QSizeF(1, 1));
            ue.noteText = dlg.text();
            ue.noteAuthor = dlg.author();
            ue.noteColor = m_annotStyle.strokeColor;
            ue.noteFontSize = m_annotStyle.fontSize;
            ue.noteWithBackground = false;
            ue.noteIsPopup = true;
            if (!ue.uid.isEmpty()) pushUndo(tab, ue);
        }
        // 🔴🔴 GO BO 2026-09-01 — GOC CUA "Note lam mat PDF" (Text thi khong sao).
        // Day la KHAC BIET DUY NHAT giua duong Note va duong Text: Note DUNG LAI LOP VECTOR.
        // Trong luc dung lai, lop vector do dang; ma nen dang la vector ⇒ trang mat noi dung.
        // Va viec dung lai nay khong con can thiet: trang co Note/Text/chu thich thi da bi chot
        // an toan buoc ve nen RASTER (setVectorAnnotSafe), nen icon Note se do PDFium ve vao anh.
        // ⇒ Bo di. Lop vector se tu dung lai binh thuong khi trang khong con vuong chu thich.
        qDebug().noquote() << "[notetool] KHONG dung lai lop vector (tranh lam trang do dang) page="
                           << pageIndex;
        tab->dirty = true;
        updateTabDirty(tab);
        invalidateAnnotPage(tab, pageIndex);
        tab->pagesNeedGenerate.insert(pageIndex);
        refreshAnnotVisuals(tab, pageIndex);
        // Note uses page objects — must re-render (slow path always)
        // 🔴🔴 2026-09-01: CUNG LOI voi cong cu Text (owner: "Note cung bi tinh huong y chang").
        // Nhanh nay BO anh trang roi CHI goi update() — khong dat lenh ve lai. Man hinh con lai
        // moi lop vector, ma lop vector khong chua annotation ⇒ Note khong hien / trang mat net.
        // ⇒ Lam y het nhanh raster ben duoi: bo cac lop dem cu VA dat lenh ve lai.
        qDebug().noquote() << "[notetool] tao Note page=" << pageIndex
                           << "zoom=" << tab->zoom
                           << "nenLaVector=" << baseIsVector(tab, pageIndex)
                           << "cheDoFast=" << m_fastMode;
        if (baseIsVector(tab, pageIndex)) {
            if (tab->renderer) {
                if (tab->view) tab->view->invalidateTiles();
                if (tab->view) tab->view->invalidateSharp();
                tab->renderer->invalidatePage(pageIndex);
                if (!m_fastMode) tab->renderer->requestPage(pageIndex, tab->zoom);
            }
            if (tab->view) tab->view->update();
        } else {
            qDebug().noquote() << "[markup] request re-render page=" << pageIndex << "zoom=" << tab->zoom;
            if (tab->renderer) {
                if (tab->view) tab->view->invalidateTiles();
                if (tab->view) tab->view->invalidateSharp();
                tab->renderer->invalidatePage(pageIndex);
                if (!m_fastMode) tab->renderer->requestPage(pageIndex, tab->zoom);
            }
        }
        refreshCommentsForPage(tab, pageIndex);
    });

    connect(tab->view, &PdfGpuView::textBoxRequested, this,
            [this, tab](int page, QRectF rectPdf) {
        qDebug().noquote() << "[markup] signal=textBoxRequested page=" << page
                 << "rect=(" << rectPdf.x() << "," << rectPdf.y() << ","
                 << rectPdf.width() << "," << rectPdf.height() << ")";
        // 0927 BƯỚC 1 đề 2: ô Text của ta nhận dòng (Enter = xuống
        // dòng) → bỏ cờ singleLine. Chỉ đổi 2 cờ duy nhất để ghép an toàn.
        NoteInputDialog dlg({}, this, /*singleLine=*/false, m_darkMode);
        dlg.setWindowTitle("Add text");
        qWarning() << "[textdlg] truoc exec, visible=" << dlg.isVisible();
        if (dlg.exec() != QDialog::Accepted) return;
        qWarning() << "[textdlg] exec Accepted, text=" << dlg.text();
        QString txt = dlg.text();
        // 🔴 FT-SIZE 0902: KHONG ghep o co dinh (w=length*5.5, h=18) nua — do chinh
        // la nguyen nhan chu 24pt bi cat cuth trong o 18pt. Giu nguyen o user keo
        // (lam chieu toi thieu); createInlineNote tu nong theo advance/chieu cao THAT.
        tab->annotMgr->createInlineNote(page, rectPdf, txt, dlg.author(), false, m_annotStyle.strokeColor, m_annotStyle.fontSize);
        {
            MarkupUndoEntry ue; ue.kind = MarkupUndoEntry::AddNote; ue.page = page;
            ue.uid = tab->annotMgr->lastCreatedUid();
            ue.noteRect = rectPdf;   // o keo ban dau; createInlineNote tu nong vua chu khi redo
            ue.noteText = txt;
            ue.noteAuthor = dlg.author();
            ue.noteColor = m_annotStyle.strokeColor;
            ue.noteFontSize = m_annotStyle.fontSize;
            ue.noteWithBackground = false;
            ue.noteIsPopup = false;
            if (!ue.uid.isEmpty()) pushUndo(tab, ue);
        }
        tab->dirty = true;
        updateTabDirty(tab);
        // 🔴 LOG-ONLY 2026-09-01: duong CONG CU TEXT tren thanh — duong that su cua owner.
        // Bai do goi thang createInlineNote nen KHONG di qua day; moi lan owner bao loi ma toi
        // do lai thay tot deu vi ly do nay. Ghi day du de mot luot bam cua owner la du dinh benh.
        qDebug().noquote() << "[textool] tao Text page=" << page
                           << "zoom=" << tab->zoom
                           << "nenLaVector=" << baseIsVector(tab, page)
                           << "cheDoFast=" << m_fastMode
                           << "veChuThichVaoAnh=" << (tab->renderer ? tab->renderer->pageAnnotRenderOf(page) : -1)
                           << "giauMarkupCuaTa=" << (tab->renderer ? tab->renderer->pageAnnotOverlayOf(page) : -1);
        invalidateAnnotPage(tab, page);
        tab->pagesNeedGenerate.insert(page);
        refreshAnnotVisuals(tab, page);
        // FreeText uses page objects — must re-render (slow path always)
        if (baseIsVector(tab, page)) {
            // 🔴🔴 GOC CUA "new Text o Single la mat trang" — tim ra 2026-09-01.
            // Nhanh nay BO anh trang (invalidatePage) roi CHI goi update() — KHONG dat lenh
            // render moi. Nen sau do man hinh chi con LOP VECTOR, ma lop vector khong chua
            // annotation va co the dang dung do ⇒ "chi con vai net".
            // Zoom hoac doi che do lam ep ve lai nen noi dung tro ve — dung nhu owner mo ta,
            // va chinh owner chan doan ra: "no tra Vector chu khong phai raster, nhung khong du".
            // ⇒ Lam y het nhanh raster ben duoi: bo cac lop dem cu VA dat lenh ve lai.
            if (tab->renderer) {
                if (tab->view) tab->view->invalidateTiles();
                if (tab->view) tab->view->invalidateSharp();
                tab->renderer->invalidatePage(page);
                if (!m_fastMode) tab->renderer->requestPage(page, tab->zoom);
            }
            if (tab->view) tab->view->update();
        } else {
            qDebug().noquote() << "[markup] request re-render page=" << page << "zoom=" << tab->zoom;
            if (tab->renderer) {
                if (tab->view) tab->view->invalidateTiles();
                if (tab->view) tab->view->invalidateSharp();
                tab->renderer->invalidatePage(page);
                if (!m_fastMode) tab->renderer->requestPage(page, tab->zoom);
            }
        }
        refreshCommentsForPage(tab, page);
    });

    connect(tab->view, &PdfGpuView::annotationPickRequested, this,
            [this, tab](int page, QPointF pt) {
        onAnnotPick(tab, page, pt);
    });

    connect(tab->view, &PdfGpuView::annotationContextRequested, this,
            [this, tab](int page, QPointF pt, QPoint gpos) {
        onAnnotContext(tab, page, pt, gpos);
    });

    connect(tab->view, &PdfGpuView::annotationMoveRequested, this,
            [this, tab](int page, double dx, double dy) {
        onAnnotMove(tab, page, dx, dy);
    });
    // Insert Image (SPEC_INSERT_IMAGE_2026-08-30): co gian Stamp bang tay nam goc.
    connect(tab->view, &PdfGpuView::annotationResizeRequested, this,
            [this, tab](int page, QRectF newRectDisp) {
        onAnnotResize(tab, page, newRectDisp);
    });

    // ── Load PDF in background thread ─────────────────────────────────────────
    PdfDocument* docPtr = tab->doc.get();
    auto* watcher = new QFutureWatcher<bool>(this);

    connect(watcher, &QFutureWatcher<bool>::finished, this,
            [this, watcher, tab, path, name, s = tab->serial]() mutable {
        watcher->deleteLater();
        int tabIdx = m_openDocs.indexOf(tab);
        if (tabIdx < 0 || tab->serial != s) return; // closed during load

        if (!watcher->result()) {
            // Failed — remove tab
            disconnect(tab->scrollConn);
            stopThumbPool(tab);       // no-op hom nay (pool chua tao) — giu bat-bien moi duong don tab
            m_docTabs->removeTab(m_docTabs->indexOf(tab->view));
            m_openDocs.removeAt(tabIdx);
            delete tab->view;
            delete tab;
            statusBar()->showMessage("Failed to open: " + name, 4000);
            if (m_openDocs.isEmpty()) {
                addWelcomeTab();
                applyWelcomeVisibility();
            }
            return;
        }

        // Success — finish setup on main thread
        tab->renderer->setDocument(tab->doc.get());
        tab->annotMgr->setDocument(tab->doc->raw(), path);
        if (tab == currentTab()) PageCache::setActiveDoc(tab->doc->raw());
        tab->annotLayer = std::make_unique<AnnotationLayer>(nullptr);  // L22: không parent — chết ở closeJob nền
        tab->annotLayer->setDocument(tab->doc->raw());
        tab->annotLayer->setAnnotationManager(tab->annotMgr.get());
        // 🔴 LÁT C 0902: noi soi day ANNOT_VISUAL_ADDED AnnotationLayer da phat tu
        // 31/08 ma KHONG AI LANG NGHE — hinh/ink/marker vua ve xong da co san
        // AnnotVisual (dung luc dang giu khoa). Bam no vao hang delta cua
        // AnnotationManager de refreshAnnotVisuals tron vao cache → hien NGAY,
        // khong xep hang sau bo dung trang nua.
        connect(tab->annotLayer.get(), &AnnotationLayer::annotVisualAdded,
                tab->annotMgr.get(), &AnnotationManager::recordVisualDelta);

        // Open persistent tile cache + thumbnail pool in background
        // 0927 M3/M2: doi tuong cache do REGISTRY tao (1 file = 1 doi tuong) va
        // pool thumbnail nhan CHUNG doi tuong do, khong tu mo them file thu hai.
        {
            struct InitResult { uint64_t hash; uint64_t size; };
            auto* initWatcher = new QFutureWatcher<InitResult>(this);
            connect(initWatcher, &QFutureWatcher<InitResult>::finished, this,
                    [initWatcher, tab, path, this, s = tab->serial]() {
                initWatcher->deleteLater();
                auto r = initWatcher->result();
                if (!tabAlive(tab, s)) return; // 🔴 L22b: tab closed during init — thiếu `!` là panel trống + UAF
                // Luu hash de 4 cho build .torvec dung lai (SPEC_PERF_HEAVYPAGE buoc B).
                tab->pdfHash = r.hash;
                tab->pdfPath = path;
                tab->tileCache = TileCacheRegistry::acquire(path, r.hash, r.size,
                                                             tab->doc->pageCount());
                if (tab->tileCache->isOpen())
                    tab->renderer->setTileCache(tab->tileCache);
                // R1: pool dung CHUNG doc voi renderer — tao tren MAIN thread de doc chac
                // chan con song (khong race dong tab nhu khi tao trong QtConcurrent).
                auto* pool = new ThumbnailRenderPool();
                // 🔴 0928 LƯỢT 16b — pool sinh trên đường mở tab async PHẢI nhận
                // đúng trạng thái băng TRƯỚC prefetchRange: `open()` chỉ thừa kế
                // `m_frozen` (mặc định false ⇒ worker mới bao giờ cũng unfrozen),
                // và đường này không đi qua sync. Tab nền hoặc đang đóng tab
                // (bộ đếm >0) ⇒ băng ngay, worker không bốc giành khoá PDFium.
                pool->setFrozen(tab != currentTab() || m_thumbCloseJobs > 0);
                // VIỆC THUMBNAIL SONG SONG: gán renderer để pool dùng pool handle render không khoá
                pool->setPdfRenderer(tab->renderer.get());
                if (!pool->open(path, tab->doc->raw(), r.hash, r.size, tab->doc->pageCount(),
                                tab->tileCache)) {
                    pool->close();
                    delete pool;
                    tab->thumbPool.reset();
                } else {
                    tab->thumbPool.reset(pool);
                    pauseThumbnails(tab);
                    // 🔴 SUA 2026-08-31 — GOC CUA "thumbnail chi len 5 trang, tab 2-3 khong len".
                    // prefetchRange() truoc day CHI dat trong loadTabFile(), ma openFile() —
                    // duong nguoi dung mo file THAT SU — KHONG he goi ham do (loadTabFile chi
                    // dung cho duong luu/nap lai). Nen lenh dung san thumbnail CHUA BAO GIO chay,
                    // chi con ThumbnailPanel tu xin vai trang quanh cho dang nhin.
                    // Day la LAN THU HAI trong ngay toi dat ban va nham vao loadTabFile.
                    // An toan: thumbnail bi chan o kThumbHandleQuota (4/12) va trang duoc dong
                    // dung sau khi va ro ri, nen dung san toan bo khong lam phinh RAM nua.
                    const int nPg = tab->doc->pageCount();
                    qDebug().noquote() << "[thumb] dung san TOAN BO" << nPg << "trang (duong openFile)";
                    // 🔴 LƯỢT 33b (J): đánh dấu trang 0 của tab vừa mở là URGENT TRƯỚC
                    // khi hàng đợi thumbnail bốc trang — nếu không, thumbnail MEP/trang
                    // quái vật giữ s_pdfiumMutex ~1,65 s/trang (đo r31b) và chặn chính
                    // trang 0 của tab này. ContinuousView sẽ xoá khi nhận ảnh (ACCEPT);
                    // trần 5 s chống đói nếu view không kịp xử lý.
                    if (tab == currentTab()) {
                        g_pdfiumUrgentPage().store(0, std::memory_order_release);
                        g_pdfiumUrgentPageDeadline().store(
                            QDateTime::currentMSecsSinceEpoch() + 5000, std::memory_order_release);
                        // 🔴 LƯỢT 33f (mục 2): doc đã mở ⇒ token = handle của tab này.
                        // Trang hiển thị cùng doc không nhường cửa sổ của chính nó;
                        // các tab khác (doc khác handle) vẫn nhường — giữ nguyên J.
                        g_pdfiumUrgentDoc().store(
                            tab->doc ? (uintptr_t)tab->doc->raw() : 0, std::memory_order_release);
                    }
                    tab->thumbPool->prefetchRange(0, nPg - 1);
                }
                if (tab == currentTab()) {
                    m_thumbPanel->setDocument(tab->doc.get(), tab->renderer.get(),
                                              tab->thumbPool.get(), false);
                    // OcrPanel can biet trang dang xem (khong thi Status giu "No document open").
                    m_thumbPanel->setCurrentPage(tab->currentPage);
                }
                // ── Vector overlay: kick off build for page 0 immediately ──
                {
                    constexpr int kVecPage0 = 0;
                    tab->vecBuilding.insert(kVecPage0);
                    auto layer = std::make_shared<VectorLayer>();
                    auto* vw = new QFutureWatcher<bool>(this);
                    connect(vw, &QFutureWatcher<bool>::finished, this, [this, vw, tab, layer, s = tab->serial]{
                        vw->deleteLater();
                        if (!tabAlive(tab, s)) return;   // 🔴 L20: chốt TRƯỚC khi dereference
                        tab->vecBuilding.remove(0);
                        if (tab->currentPage != 0) return;
                        if (tab != currentTab()) return;
                        if (vw->result()) {
                            setTabVectorLayer(tab, layer, 0);
                        } else {
                            setTabVectorLayer(tab, nullptr, 0);
                        }
                    });
                    FPDF_DOCUMENT d = tab->doc->raw();
                    const QString pdfPath = tab->pdfPath;
                    const quint64 pdfHash = tab->pdfHash;
                    const bool allowCache = !tab->torvecDirty.contains(0);
                    // 🔴 LƯỢT 22: token lúc SPAWN, RAII move vào lambda.
                    trdoc::Task task(d, "VectorLayer::build/moTab");
                    vw->setFuture(QtConcurrent::run([layer, d, pdfPath, pdfHash, path, allowCache,
                                                     task = std::move(task)]{
                        // Thu cache .torvec truoc — nap nhanh gap 45 lan so voi dung lai tu PDF.
                        // Khoa cache co the chua kip dat (initWatcher chay bat dong bo). Tu bu: hashFile chi
                        // doc 128 KB (64 KB dau + 64 KB cuoi) nen re, an toan luong nen.
                        const QString keyPath = pdfPath.isEmpty() ? path : pdfPath;
                        const quint64 keyHash = pdfHash ? pdfHash : (quint64)TileCacheFile::hashFile(keyPath);
                        if (allowCache && VectorCache::tryLoad(*layer, keyPath, keyHash, 0)) return true;
                        // `[&task]`: Task KHÔNG copy được (giữ số đếm trong sổ) — bắt buộc bắt tham chiếu.
                        if (!layer->build(d, 0, [&task]() { return task.cancelled(); })) return false;
                        if (allowCache) VectorCache::trySave(*layer, keyPath, keyHash, 0);
                        return true;
                    }));
                }
            });
            initWatcher->setFuture(QtConcurrent::run([path]() -> InitResult {
                InitResult r{};
                r.hash = TileCacheFile::hashFile(path);
                r.size = static_cast<uint64_t>(QFileInfo(path).size());
                return r;
            }));
        }

        // 🔴🔴 DAY CUOI 2026-09-01: `objectCountReady` truoc gio KHONG AI NGHE.
    // ensureForeignAnnotLayer bo qua khi chua biet so object ("DEFER"), nhung khong co ai goi
    // LAI khi so do da biet ⇒ lop bu treo vinh vien ⇒ chu thich ngoai khong bao gio duoc ve
    // tren nen vector. Noi day nay lai la mat xich cuoi cua chuoi: render xong → biet so object
    // → dung lop bu → comment hien tren nen VECTOR (khong phai hy sinh net bang cach ep raster).
    connect(tab->renderer.get(), &PdfRenderer::objectCountReady, this,
            [this, tab, s = tab->serial](int page, int count) {
        if (!tabAlive(tab, s)) return;
        if (tab != currentTab()) return;
        qDebug().noquote() << "[fgnlayer] da biet so object=" << count
                           << "page=" << page << "— thu dung lai lop bu";
        ensureForeignAnnotLayer(tab, page);
    });

    connect(tab->renderer.get(), &PdfRenderer::pagePartial,
                this, [this, tab, s = tab->serial](int idx, double sc, QImage img) {
            // 🔴 0928 LƯỢT 20 — CRASH ĐÓNG TAB (dump 32120): event queued từ worker
            // tới sau khi onTabClose `delete t->view` ⇒ tab->view NULL, showPartial
            // đọc 0x23c. contains(tab) KHÔNG dereference — an toàn kể cả tab đã bị
            // `delete t` ở closeJob nền.
            if (!tabAlive(tab, s) || !tab->view) return;
            if (img.isNull()) return;
            if (idx != tab->currentPage) return;
            QElapsedTimer _t; _t.start();
            tab->view->showPartial(idx, sc, img);
            if (_t.elapsed() > 50)
                qDebug().noquote() << "[slotms] showPartial ms=" << _t.elapsed() << "page=" << idx;
        });

        // 🔴 SUA 2026-08-31 (lan 2): lan dau toi noi vao `pageReady` — SAI, do la tin hieu cua
        // view Single, ma o che do Continuous duong Single da bi chan boi cac chot !m_fastMode
        // nen no KHONG BAO GIO phat (do duoc: "TAI SU DUNG" = 0 lan).
        // Duong Continuous phat `continuousPageReady`. Noi vao dung cho do.
        connect(tab->renderer.get(), &PdfRenderer::continuousPageReady, this,
                [this, tab, s = tab->serial](int idx, QImage img, double) {
            // 🔴 0928 LƯỢT 20: chốt tab đã đóng — tab->fgnDeferred bên dưới
            // dereference tab, mà tab chết ở closeJob nền.
            if (!tabAlive(tab, s)) return;
            if (img.isNull()) return;
            if (m_thumbPanel && tab == currentTab())
                m_thumbPanel->acceptFromFullRender(idx, img);
            // LỖI 2 0902 — duong quay lai cho DEFER vi khoa pdfium ban.
            // `continuousPageReady` = MOT LUOT VE THAT SU XONG, tuc background VUA
            // PHONG khoa pdfium va so object cua trang vua xong da duoc biet. Do la
            // thoi diem an toan de thu lai nhung trang truoc do bi `TryPdfiumLock`
            // tu choi (ghi vao fgnDeferred o ensureForeignAnnotLayer). Dung DUNG
            // trang cua chung (`pg`), khong phai `idx` vua xong. Khong timer, khong
            // polling — chi bat theo su kien render xong von da co.
            if (tab == currentTab()) {
                const QSet<int> pending = tab->fgnDeferred;
                for (int pg : pending) {
                    if (pg == idx) continue;   // idx tu no se duoc needAnnotVisuals lo
                    tab->fgnDeferred.remove(pg);
                    ensureForeignAnnotLayer(tab, pg);
                }
            }
        });

        tab->pageReadyConn = connect(
            tab->renderer.get(), &PdfRenderer::pageReady,
            this, [this, tab, s = tab->serial](int idx, QImage img) {
                // 🔴 0928 LƯỢT 20: pageReadyConn đã disconnect trong shutdownTab NHƯNG
                // event queued trước đó vẫn tới — lưới cuối trước khi dereference
                // tab->currentPage / tab->view bên dưới.
                if (!tabAlive(tab, s) || !tab->view) return;
                if (img.isNull()) return;
                if (idx != tab->currentPage) {
                    qDebug() << "[Main] pageReady stale: got" << idx << "but current=" << tab->currentPage;
                    return;
                }
                // 🔴 2026-08-31: anh trang DAY DU vua xong — dung luon lam thumbnail,
                // khoi bat PDFium doc lai noi dung trang mot lan nua.
                if (m_thumbPanel && tab == currentTab())
                    m_thumbPanel->acceptFromFullRender(idx, img);
                qDebug() << "[Main] pageReady idx=" << idx
                         << "imgSize=" << img.size()
                         << "hasImage=" << tab->view->hasImage();
                tab->view->setPage(idx, img, tab->doc->pageSize(idx));
                finishPageDisplay(tab, idx);
            });

        m_docTabs->setTabText(m_docTabs->indexOf(tab->view), name);
        setWindowTitle("TorReader PDF — " + name);
        statusBar()->showMessage(
            QString("Opened: %1  (%2 pages)").arg(name).arg(tab->doc->pageCount()), 5000);

        // Auto-fit first page to viewport so large architectural sheets (A0/A1)
        // are immediately visible without requiring manual "Fit Page" press.
        if (tab->view) {
            auto sz = tab->doc->pageSize(0);
            if (!sz.isEmpty()) {
                double vw = qMax(100.0, static_cast<double>(tab->view->width())  - 16.0);
                double vh = qMax(100.0, static_cast<double>(tab->view->height()) - 16.0);
                double fitZoom = qMin(vw / sz.width(), vh / sz.height());
                tab->zoom = qBound(0.05, fitZoom, 4.0);
                tab->view->setZoom(tab->zoom);
                if (m_zoomEdit)
                    m_zoomEdit->setText(QString::number(qRound(tab->zoom * 100)) + "%");
            }
            tab->view->setPendingPage(0, sz);
            tab->view->setPageBoxOrigin(tab->doc->pageBoxOriginCached(0));
        }
        refreshAnnotVisuals(tab, 0);
        if (!m_fastMode) tab->renderer->requestPage(0, tab->zoom);
        // R1: pin trang 0 (+-1) khi mo file — LRU khong duoi ngay trang dau.
        PageCache::pin(tab->doc->raw(), 0);
        if (tab->doc->pageCount() > 1) PageCache::pin(tab->doc->raw(), 1);
        notifyOcrStatusForPage(0);   // 1 dong o thanh trang thai (SPEC_OCR_TAB phan 1c)
        schedulePagePrefetch();       // prefetch trang lien ke (SPEC_PAGECACHE_CORE muc 4)

        if (tab == currentTab()) {
            syncSidebarToTab(tabIdx);
            if (m_thumbPanel && m_thumbPanel->isCommentsTabVisible())
                onCommentsRequested();
if (m_fastMode && m_continuousView) {
                // 🔴 SUA 2026-08-31 (lan 2, AN TOAN HON).
                // Van de goc: doan nay noi day ContinuousView nhung KHONG lam no HIEN RA
                // (m_continuousView bi hide() tu luc khoi tao) => mo file len van thay Single.
                // Lan 1 toi goi thang setViewMode(true) — SAI, vi ham do lam viec theo
                // currentTab() chu khong theo `tab` da nap xong o day, va no chay lai ca chuoi
                // setDocument/disconnect-reconnect => sinh
                // "QObject::connect(ContinuousView, MainWindow): invalid nullptr parameter"
                // va lam tab thu 2 khong nap duoc gi.
                // Gio: giu NGUYEN cach noi day cu (theo `tab`), chi them phan lam no HIEN RA.
                qDebug() << "[perf] cont loadTab fast mode zoom=" << tab->zoom;
                m_continuousView->setZoom(tab->zoom);
                m_continuousView->setDocument(tab->doc.get(), tab->renderer.get(), &tab->visualsCache, &tab->vecLayers);
                m_continuousView->setVectorCacheKey(tab->pdfPath.isEmpty() ? tab->doc->filePath() : tab->pdfPath, tab->pdfHash);
                m_docTabs->setFixedHeight(m_docTabs->tabBar()->sizeHint().height());
                m_continuousView->show();
                // 🔴 LƯỢT 33d (L): doc xong GIỮA LƯỢT ĐỔI TAB ⇒ view còn mang noi dung
                // tab cu (do r33b: "L tab=f2 page=2 loading=1"). Khi open xong, ep ve
                // DUNG offset da luu cua tab (tab moi mo: 0 = trang 0) — nhu duong
                // onTabChanged 33b — khong dung scrollToPage(currentPage) (no keo DINH
                // trang TAM len dau, lech vi tri).
                m_continuousView->restoreScrollY(tab->contPosSaved ? tab->contScrollY : 0);
            }
        }
    });

    // 🔴 0928 LƯỢT 21: LƯU future vào tab — đóng tab khi open() còn chạy thì
    // closeJob nền phải waitForFinished trước `delete t` (dump 32832).
    tab->openFuture = QtConcurrent::run([docPtr, path]() -> bool {
        // 🔴 LƯỢT 33b (J): mở tài liệu mới = KHẨN — đặt bộ đếm urgent để việc nền
        // (thumbnail tier>0, OCR probe) thấy pdfiumUrgentPending()>0 và NHƯỜNG khoá
        // ngay, doc mới + trang đầu không phải xếp sau hàng đợi nền.
        UrgentPdfiumScope _urgentOpen(true);
        return docPtr->open(path);
    });
    watcher->setFuture(tab->openFuture);
}

// ── Probe-only: lai che do xem tu dong lenh (dung cho --viewprobe) ────────────

void MainWindow::probeSetView(bool continuous, double zoomPercent, int page1Based,
                              double centerXpt, double centerYpt) {
    auto* t = currentTab();
    if (!t || !t->doc || !t->doc->isOpen()) return;

// Che do xem lien tuc — chi phat toggled khi khac trang thai hien tai
     if (m_viewFastAct && m_viewFastAct->isChecked() != continuous) {
         m_viewFastAct->setChecked(continuous);
         QCoreApplication::processEvents();
     }
     // 🔴 LƯỢT 33b (L): setChecked() tren QActionGroup EXCLUSIVE KHONG phat
     // triggered() ⇒ setViewMode() chua tung chay, m_fastMode giu nguyen Continuous,
     // nen bai do "mode=Single" that ra van o Continuous (doc view rieng theo tab =
     // gia). Goi thang setViewMode de che do Single duoc test THAT.
     if (m_fastMode != continuous) setViewMode(continuous);

    // Zoom: di thang vao luong onZoomChanged (cung nhu o Zoom Edit nhan Enter).
    // probe-only (--viewprobe, nghiem thu scrollbar 0921): zoomPercent <= 0 => chay DUNG
    // cong thuc nut "Fit Page" o che do Continuous (xem MainWindow.cpp:1282) de chup anh
    // trang thai fit. Khong doi hanh vi san pham.
    if (zoomPercent <= 0.0 && m_fastMode && m_continuousView) {
        auto sz = t->doc->pageSize(t->currentPage);
        if (!sz.isEmpty()) {
            double z = (m_continuousView->viewport()->width() - 40.0) / sz.width();
            m_continuousView->setZoom(qBound(0.1, z, 10.0));
        }
    } else {
        onZoomChanged(zoomPercent / 100.0);
    }
    QCoreApplication::processEvents();

    // Nhay trang qua onPageChanged (xu ly ca 2 che do don/lien tuc)
    int page = qBound(0, page1Based - 1, t->doc->pageCount() - 1);
    onPageChanged(page);
    QCoreApplication::processEvents();

    // Can tam: chuyen toa do TRANG PDF (goc duoi-trai) sang toa do hien thi
    // (PdfCoords::pdfToDisp — dung lai cau hinh da va /Rotate), roi trung tam
    // bang dung co che cuon cua PdfGpuView (centerOnPageRect). Chi o che do don
    // trang — che do lien tuc khong co loai giu duoc vi tri nhu vay (khong dong
    // vao ContinuousView.cpp).
    if (!continuous && !std::isnan(centerXpt) && !std::isnan(centerYpt) && t->view) {
        TimedPdfiumLock lock(__FILE__, __LINE__);
        FPDF_PAGE p = FPDF_LoadPage(t->doc->raw(), t->currentPage);
        if (p) {
            double wd = FPDF_GetPageWidth(p);
            double hd = FPDF_GetPageHeight(p);
            int rot = FPDFPage_GetRotation(p);
            const QPointF box = pdfBoxOrigin(p);
            const QPointF disp = pdfToDisp(centerXpt, centerYpt, wd, hd, rot, box.x(), box.y());
            FPDF_ClosePage(p);
            t->view->centerOnPageRect(QRectF(disp.x() - 1.0, disp.y() - 1.0, 2.0, 2.0));
            QCoreApplication::processEvents();
        }
    }
}

// Probe-only (--viewfast-probe, SPEC_VIEWFAST): nhay toi trang (0-based) qua dung
// duong onPageChanged — cap nhat t->currentPage ca 2 che do don/lien tuc.
void MainWindow::probeSetPage(int page0Based) {
    auto* t = currentTab();
    if (!t || !t->doc || !t->doc->isOpen()) return;
    onPageChanged(qBound(0, page0Based, t->doc->pageCount() - 1));
    QCoreApplication::processEvents();
}

// Probe-only (--viewfast-probe): trang dang xem o che do hien tai.
int MainWindow::probeCurrentPage() const {
    if (m_fastMode && m_continuousView) return m_continuousView->currentPage();
    if (auto* t = currentTab()) return t->currentPage;
    return -1;
}

// Probe-only (--viewfast-twodoc-probe): kich hoat tab doc theo chi so — chay qua
// dung QTabWidget::setCurrentIndex de currentChanged -> onTabChanged (setActiveDoc
// + setDocument cho ContinuousView) giong nguoi dung bam tab that su.
void MainWindow::probeActivateTab(int idx) {
    if (!m_docTabs || idx < 0 || idx >= m_docTabs->count()) return;
    m_docTabs->setCurrentIndex(idx);
    QCoreApplication::processEvents();
}

QString MainWindow::probeThumbCounters() const {
    auto* t = currentTab();
    const int pages = (t && t->doc && t->doc->isOpen()) ? t->doc->pageCount() : -1;
    int visible = 0;
    if (t && t->doc && t->doc->isOpen())
        for (int i = 0; i < pages; ++i)
            if (!m_thumbPanel->thumbnailForPage(i).isNull()) ++visible;
    return QStringLiteral("THUMB tab=%1 pages=%2 accepted=%3 rejected=%4 pending=%5 staleDrop=%6 earlyRet=%7 VISIBLE_in_list=%8")
        .arg(m_openDocs.indexOf(t)).arg(pages)
        .arg(m_thumbPanel->debugAcceptedCount())
        .arg(m_thumbPanel->debugRejectedCount())
        .arg(m_thumbPanel->debugPendingCount())
        .arg(m_thumbPanel->debugDroppedCount())
        .arg(m_thumbPanel->debugEarlyReturnCount())
        .arg(visible);
}

void MainWindow::probeResetThumbCounters() {
    if (m_thumbPanel) m_thumbPanel->debugResetCounters();
}

QString MainWindow::probeThumbVerify(int tabIdx, int timeoutMs) {
    probeActivateTab(tabIdx);
    auto* t = currentTab();
    const int pages = (t && t->doc && t->doc->isOpen()) ? t->doc->pageCount() : -1;
    QElapsedTimer et; et.start();
    int vis = 0;
    while (et.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        QThread::msleep(50);
        vis = 0;
        for (int i = 0; i < pages; ++i)
            if (!m_thumbPanel->thumbnailForPage(i).isNull()) ++vis;
        if (pages > 0 && vis == pages) break;
    }
    const bool ok = (pages > 0 && vis == pages);
    return QStringLiteral("VERIFY tab=%1 pages=%2 VISIBLE=%3 %4")
        .arg(tabIdx).arg(pages).arg(vis).arg(ok ? "OK" : "TIMEOUT");
}

// Probe-only (--contvec-probe): cuon ContinuousView toi trang roi bơm su kien de
// scrollTimer(80ms)/vecBuildTimer(700ms) chay het — dung de nghiem thu Viec A/B/C:
// cuon xuong vai trang roi quay lai trang nang, kiem log that co [torvec] HIT
// thay vi [torvec] SKIP reason=nokey / [cont] paint LOWRES.
void MainWindow::probeContScrollTo(int page) {
    auto* t = currentTab();
    if (!t || !t->doc || !t->doc->isOpen() || !m_continuousView) return;
    m_continuousView->scrollToPage(qBound(0, page, t->doc->pageCount() - 1), "probeContScrollTo");
    for (int i = 0; i < 40; ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(50);
    }
}

// ── Probe-only: lat trang (--pageflip-bench, SPEC_PERF_DESK_ABOUT phan 1) ─────
// Do thoi gian MỖI lần doi trang tu lúc yeu cầu (onPageChanged) tới lúc pageReady
// (trang ve xong). Chay QUA DUNG duong di nguoi dung: onPageChanged (rot qua
// settle timer + cache + render), khong goi renderer truc tiep. Giu o day vi
// can truy cap currentTab + renderer (pageReady).
void MainWindow::probeFlipBench(int p1, int p2, int loops) {
    // Wait den khi doc da mo xong (openFile chay bat dong bo qua QFutureWatcher).
    auto* tab = currentTab();
    const qint64 openDeadline = QDateTime::currentMSecsSinceEpoch() + 120000;
    while ((!tab || !tab->doc || !tab->doc->isOpen())
           && QDateTime::currentMSecsSinceEpoch() < openDeadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(20);
        tab = currentTab();
    }
    if (!tab || !tab->doc || !tab->doc->isOpen()) {
        fprintf(stdout, "[pageflip] FAIL: document did not open in 120s\n");
        fflush(stdout);
        return;
    }
    const int total = tab->doc->pageCount();
    p1 = qBound(0, p1, total - 1);
    p2 = qBound(0, p2, total - 1);
    if (p1 == p2 || loops <= 0) {
        fprintf(stdout, "[pageflip] FAIL: p1==p2 (hoac loops<=0)\n");
        fflush(stdout);
        return;
    }
    QTextStream out(stdout);
    out << "[pageflip] file=" << QFileInfo(tab->doc->filePath()).fileName()
        << " pages=" << total << " p1=" << (p1 + 1) << " p2=" << (p2 + 1)
        << " loops=" << loops << "\n";
    out.flush();

    // Doi pageReady cho DUNG trang target. Day la ham quyet dinh cua bench.
    // Tra ms; -1 neu timeout. Chu y: pageReady co the phat DONG BO trong chinh
    // onPageChanged (cache hit) — phai danh dau fired TRUOC khi loop.exec(), neu
    // goi loop.quit() truoc exec() thi exec() treo vo han. timer timeout chi cam
    // khi ta THUC SU cho.
    auto waitPage = [&](int target, int timeoutMs) -> qint64 {
        QEventLoop loop;
        bool fired = false;
        bool timedOut = false;
        QMetaObject::Connection conn = connect(
            tab->renderer.get(), &PdfRenderer::pageReady, &loop,
            [&](int idx, const QImage&) { if (idx == target) { fired = true; loop.quit(); } });
        // Trang thuan VECTOR khong bao gio phong pageReady (R2 chan raster). Bench
        // phai nghe them pageDisplayed de khong treo vo han tren trang ve bang vector.
        QMetaObject::Connection conn2 = connect(
            this, &MainWindow::pageDisplayed, &loop,
            [&](int idx) { if (idx == target) { fired = true; loop.quit(); } });
        QElapsedTimer timer; timer.start();
        onPageChanged(target);
        if (!fired) {
            QTimer::singleShot(timeoutMs, &loop, [&] { timedOut = true; loop.quit(); });
            loop.exec();
        }
        disconnect(conn);
        disconnect(conn2);
        if (timedOut) return -1;
        return timer.elapsed();
    };

    // Ban dau nhanh den p1 (khong do) de bat dau dung diem.
    if (waitPage(p1, 900000) < 0)
        fprintf(stdout, "[pageflip] WARN: setup flip to p1 timed out\n");
    fflush(stdout);
    // LAM NO render cache cua CA HAI trang (p1 roi p2) — luc do cac lat DO se
    // la cache hit (pageReady ~ngay), thoi gian do tach duoc phan pageHasText
    // dong bo tren luong UI ra khoi phan render (render 2T path duoi llvmpipe
    // mat nhieu phut, lam che at phan can do).
    if (waitPage(p2, 900000) < 0)
        fprintf(stdout, "[pageflip] WARN: warm render of p2 timed out\n");
    fflush(stdout);
    // Quay lai p1 de bat dau chuoi do cho dong nhat (cache hit ca 2 trang).
    if (waitPage(p1, 900000) < 0)
        fprintf(stdout, "[pageflip] WARN: rewind to p1 timed out\n");
    fflush(stdout);

    int cur = p1;
    QVector<qint64> msList;
    for (int i = 0; i < loops; ++i) {
        const int target = (cur == p1) ? p2 : p1;
        const int cached = OcrTextCache::hasTextStatus(
            reinterpret_cast<OcrTextCache::DocHandle>(tab->doc->raw()), target) >= 0 ? 1 : 0;
        const qint64 ms = waitPage(target, 120000);
        cur = target;
        if (ms >= 0) msList.append(ms);
        out << "[pageflip] from=" << (cur == p1 ? p2 : p1) + 1
            << " to=" << cur + 1
            << " ms=" << (ms >= 0 ? QString::number(ms) : QStringLiteral("timeout"))
            << " pageHasTextCached=" << cached << "\n";
        out.flush();
    }

    if (msList.isEmpty()) {
        out << "[pageflip] TONG n=0 (tat ca timeout)\n";
        out.flush();
        return;
    }
    qint64 mn = msList.first(), mx = msList.first();
    double sum = 0;
    for (qint64 v : msList) { mn = qMin(mn, v); mx = qMax(mx, v); sum += v; }
    out << "[pageflip] TONG n=" << msList.size()
        << " min=" << mn << " max=" << mx
        << " mean=" << QString::number(sum / msList.size(), 'f', 1) << "\n";
    out.flush();
}

// Probe-only: chuyen tiep toi ThumbnailPanel::selectTab de --uiprobe chon tab sidebar.
void MainWindow::probeSelectSidebarTab(int id) {
    if (m_thumbPanel) m_thumbPanel->selectTab(id);
}

// 0927 LUOT 4 (VIEC C): nap lai comment cua trang + ep ban ghi vao sidebar
// ngay (khong doi doi qua commentTextEdited cua MainWindow).
void MainWindow::probeRefreshComments(int page) {
    auto* t = currentTab();
    if (!t || !m_thumbPanel) return;
    t->annotCacheValid = false;
    onCommentsRequested();
    refreshCommentsForPage(t, page);
}

// ── Probe hop thoai (--uiprobe-dialog, SPEC_PROBE_DIALOG_FRAMES phan 1) ──
// Mo DUNG hop thoai bang cach kich hoat QAction TUONG UNG tren toolbar (khong
// bom phim, khong gia lap chuot). Hop thoai la cua so rieng + modal (exec) nen
// phai chup tu BEN TRONG vong lap modal bang QTimer::singleShot roi dong lai —
// nguoc lai exec() chan vi main khong thoat duoc.
bool MainWindow::probeDialog(const QString& name, const QString& outDir,
                             QString* errOut, int grabDelayMs) {
    QString actionText;
    if (name == QLatin1String("merge"))      actionText = QLatin1String("Merge PDFs");
    else if (name == QLatin1String("about")) actionText = QLatin1String("About");
    else if (name == QLatin1String("sign"))  actionText = QStringLiteral("Sign PDF…");
    else if (name == QLatin1String("print")) actionText = QLatin1String("Print");
    else {
        if (errOut) *errOut = QStringLiteral("unknown dialog name: ") + name;
        return false;
    }
    QAction* act = nullptr;
    const QList<QAction*> all = findChildren<QAction*>();
    for (auto* a : all) {
        if (a->text() == actionText) { act = a; break; }
    }
    if (!act) {
        if (errOut) *errOut = QStringLiteral("action '") + actionText
                              + QStringLiteral("' khong ton tai tren toolbar");
        return false;
    }
    if (!QDir().mkpath(outDir)) {
        if (errOut) *errOut = QStringLiteral("khong tao duoc ") + outDir;
        return false;
    }
    const QString png = outDir + QLatin1String("/dialog_") + name + QLatin1String(".png");
    const QString txt = outDir + QLatin1String("/dialog_") + name + QLatin1String(".txt");
    const ThemeTokens tokens = m_darkMode ? darkHC() : lightHC();

    // Trang thai dung chung cho lambda chup + doan ket sau trigger(). Dung con
    // tro de neu timer roi muon (tinh huong khong bao gio xay ra voi 4 dialog
    // nay) thi khong truy cap bien stack da huy.
    struct GrabState { bool done = false; bool ok = false; QString err; };
    auto state = std::make_shared<GrabState>();

    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, state, png, txt, tokens]() {
        QWidget* dlg = QApplication::activeModalWidget();
        if (!dlg) {
            // Khong co modal (About dung lambda exec, van la modal truoc khi
            // timeout) — phong thu: tim QDialog dang hien trong cac cua so top.
            const QList<QWidget*> tops = QApplication::topLevelWidgets();
            for (auto* tw : tops) {
                if (auto* d = qobject_cast<QDialog*>(tw)) {
                    if (d->isVisible()) { dlg = d; break; }
                }
            }
        }
        if (!dlg) {
            state->err = QStringLiteral("khong tim thay hop thoai modal sau khi kich action");
            state->done = true;
            return;
        }
        QString dump;
        state->ok = UiProbe::snapshot(dlg, png, txt, tokens, m_darkMode, &dump, &state->err);
        // Dong bang reject: tranh kich hoat nut mac dinh nguy hiem
        // (Sign/Print/…). exec() thoát, main tu thoat.
        if (auto* qd = qobject_cast<QDialog*>(dlg)) qd->reject(); else dlg->close();
        state->done = true;
    });
    timer->start(qMax(200, grabDelayMs));

    act->trigger();

    if (!state->done) {   // phong thu: thu hoi timer neu luot khong qua vong modal
        timer->stop();
        timer->deleteLater();
        if (errOut) *errOut = QStringLiteral("khong ket duoc vong lap modal, khong chup duoc");
        return false;
    }
    timer->deleteLater();
    if (!state->ok) {
        if (errOut) *errOut = state->err;
        return false;
    }
    return true;
}

// ── Probe nhieu khung (--uiprobe-frames, SPEC_PROBE_DIALOG_FRAMES phan 2) ──
// Chay kich ban trinh dien CO DINH, moi buoc chup mot khung sau khi cho
// intervalMs de giao dien ve xong. Chi dung API cong khai cua MainWindow
// (probeSelectSidebarTab / onPageChanged / m_darkAct / handleSearchRequest).
int MainWindow::probeFrames(const QString& outDir, int intervalMs) {
    auto* tab = currentTab();
    if (!tab || !tab->doc || !tab->doc->isOpen()) {
        qWarning().noquote() << "[frames] khong co tai lieu dang mo";
        return 0;
    }
    const int pages = tab->doc->pageCount();
    if (!QDir().mkpath(outDir)) {
        qWarning().noquote() << "[frames] khong tao duoc" << outDir;
        return 0;
    }
    QDir dir(outDir);
    int n = 0;
    const auto frame = [&](const QString& tag) {
        const QString png = dir.filePath(
            QStringLiteral("frame_%1.png").arg(n, 3, 10, QLatin1Char('0')));
        const QPixmap pm = grab();
        if (!pm.isNull() && pm.save(png, "PNG"))
            qInfo().noquote() << "[frames]" << tag << png;
        ++n;
        QCoreApplication::processEvents();
    };
    const auto settle = [&]() {
        const int loops = qMax(intervalMs / 50, 1);
        for (int i = 0; i < loops; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }
    };

    // 1. Tab Thumbnails (id 0) — 2 khung de GIF dung lai o dau.
    probeSelectSidebarTab(0);
    settle(); frame(QStringLiteral("thumb-1"));
    settle(); frame(QStringLiteral("thumb-2"));

    // 2. Trang 2, 3, 4 (0-based 1,2,3) — moi trang 1 khung.
    const int targets[] = {1, 2, 3};
    for (int pg : targets) {
        if (pg < pages) {
            onPageChanged(pg);
            settle();
            frame(QStringLiteral("page-%1").arg(pg + 1));
        } else {
            qInfo().noquote() << "[frames] bo qua trang" << (pg + 1)
                              << "(tai lieu chi co" << pages << "trang)";
        }
    }

    // 3. Tab Search (id 4): go san tu "Executive" + chay tim — 2 khung.
    probeSelectSidebarTab(4);
    settle();
    m_thumbPanel->setSearchResults(QStringLiteral("Executive"), {});
    handleSearchRequest(QStringLiteral("Executive"), Qt::CaseInsensitive);
    QCoreApplication::processEvents();
    frame(QStringLiteral("search-typing"));
    // TextSearch chay bat dong bo: cho searchComplete roi moi chup khung ket qua.
    bool searchDone = false;
    QMetaObject::Connection sc = connect(m_textSearch, &TextSearch::searchComplete,
                                         this, [&searchDone](int) { searchDone = true; });
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 30000;
    while (!searchDone && QDateTime::currentMSecsSinceEpoch() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(20);
    }
    disconnect(sc);
    settle(); frame(QStringLiteral("search-results"));

    // 4. Tab Comments (id 2) — 1 khung.
    probeSelectSidebarTab(2);
    settle(); frame(QStringLiteral("comments"));

    // 5. Tab OCR (id 5) — 2 khung.
    probeSelectSidebarTab(5);
    settle(); frame(QStringLiteral("ocr-1"));
    settle(); frame(QStringLiteral("ocr-2"));

    // 6. Dark mode BAT — 2 khung.
    if (m_darkAct && !m_darkAct->isChecked()) {
        m_darkAct->setChecked(true);
        settle();
    }
    frame(QStringLiteral("dark-on-1"));
    settle(); frame(QStringLiteral("dark-on-2"));

    // 7. Dark mode TAT — 1 khung.
    if (m_darkAct && m_darkAct->isChecked()) {
        m_darkAct->setChecked(false);
        settle();
    }
    frame(QStringLiteral("dark-off"));

    return n;
}

// Probe (--searchnav-test): tim kiem THAT qua TextSearch roi "bam" ket qua thu
// resultIdx1Based qua DUNG tin hieu searchResultSelected — cung duong di nguoi
// dung bam trong SearchPanel. Muon so lieu thuc thi chay khong can cua so.
void MainWindow::probeSearchNav(bool continuous, double zoomPercent,
                                 const QString& query, int resultIdx1Based, int waitMs) {
     if (m_viewFastAct && m_viewFastAct->isChecked() != continuous) {
         m_viewFastAct->setChecked(continuous);
         QCoreApplication::processEvents();
     }
    onZoomChanged(zoomPercent / 100.0);
    QCoreApplication::processEvents();

    // Cho TextSearch (bat dong bo) tra VE DAY DU: chi "bam" sau khi searchComplete.
    bool done = false;
    QMetaObject::Connection searchDone = connect(
        m_textSearch, &TextSearch::searchComplete, this,
        [&done](int) { done = true; });
    handleSearchRequest(query, Qt::CaseInsensitive);

    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + qMax(5000, waitMs);
    while (!done && QDateTime::currentMSecsSinceEpoch() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(20);
    }
    disconnect(searchDone);
    auto* st = currentTab();
    if (!st || st->searchResults.isEmpty()) {
        qInfo().noquote() << "[searchnav] NO_RESULTS query=" << query;
        return;
    }
    const int idx = qBound(0, resultIdx1Based - 1, st->searchResults.size() - 1);
    const SearchResult& r = st->searchResults[idx];
    m_thumbPanel->searchResultSelected(r.pageIndex, QList<QRectF>(r.rects.begin(), r.rects.end()));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(50);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

// Probe (--searchstate-test, SPEC_SEARCH_STATE_R3): nghiem thu 3 loi trang thai.
// 1) Tim o file A, mo file B → sidebar rong; quay lai A → thay lai dung ket qua
//    cu (khong phai tim lai). 2) O che do lien tuc: scroll qua tung trang co ket
//    qua → log [hl] pages=.. visiblePages=.. drawn=.. (moi trang phai drawn>0).
void MainWindow::probeSearchState(const QString& pathA, const QString& pathB,
                                   const QString& query, bool continuous,
                                   double zoomPercent, int waitMs) {
     if (m_viewFastAct && m_viewFastAct->isChecked() != continuous) {
         m_viewFastAct->setChecked(continuous);
         QCoreApplication::processEvents();
     }
    onZoomChanged(zoomPercent / 100.0);
    QCoreApplication::processEvents();

    // ── Mo file A, tim kiem (duong di that: handleSearchRequest) ──
    openFile(pathA);
    for (int i = 0; i < 60; ++i) { QCoreApplication::processEvents(); QThread::msleep(50); }
    handleSearchRequest(query, Qt::CaseInsensitive);
    bool doneA = false;
    QMetaObject::Connection connA = connect(m_textSearch, &TextSearch::searchComplete,
                                            this, [&doneA](int) { doneA = true; });
    const qint64 deadlineA = QDateTime::currentMSecsSinceEpoch() + qMax(5000, waitMs);
    while (!doneA && QDateTime::currentMSecsSinceEpoch() < deadlineA) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(20);
    }
    disconnect(connA);
    auto* tabA = currentTab();
    const int countA = tabA ? tabA->searchResults.size() : 0;
    qInfo().noquote() << QString("[searchstate] A search: results=%1 sidebar=%2 query='%3'")
        .arg(countA)
        .arg(m_thumbPanel ? m_thumbPanel->probeSearchCount() : -1)
        .arg(tabA ? tabA->searchQuery : QString());

    // ── Mo file B (openFile lam no tro thanh tab hien tai) ──
    openFile(pathB);
    for (int i = 0; i < 60; ++i) { QCoreApplication::processEvents(); QThread::msleep(50); }
    if (m_docTabs && m_docTabs->count() > 1)
        m_docTabs->setCurrentIndex(m_docTabs->count() - 1);
    QCoreApplication::processEvents();
    auto* tabB = currentTab();
    qInfo().noquote() << QString("[searchstate] switch to B: tabResults=%1 sidebar=%2 query='%3'")
        .arg(tabB ? tabB->searchResults.size() : -1)
        .arg(m_thumbPanel ? m_thumbPanel->probeSearchCount() : -1)
        .arg(m_thumbPanel ? m_thumbPanel->probeSearchQuery() : QString());

    // ── Quay lai A: phai thay lai dung ket qua cu ──
    if (m_docTabs && tabA && m_openDocs.contains(tabA) && tabA->view) {
        m_docTabs->setCurrentWidget(tabA->view);
        QCoreApplication::processEvents();
        qInfo().noquote() << QString("[searchstate] back to A: tabResults=%1 sidebar=%2 query='%3'")
            .arg(tabA->searchResults.size())
            .arg(m_thumbPanel ? m_thumbPanel->probeSearchCount() : -1)
            .arg(m_thumbPanel ? m_thumbPanel->probeSearchQuery() : QString());
    }

    // ── Continuous: scroll qua tung trang co ket qua → log [hl] ──
    if (continuous && m_continuousView && tabA && !tabA->searchResults.isEmpty()) {
        QSet<int> resultPages;
        for (const SearchResult& r : tabA->searchResults) resultPages.insert(r.pageIndex);
        QList<int> pages = resultPages.values();
        std::sort(pages.begin(), pages.end());
        for (int pg : pages) {
            m_continuousView->scrollToPage(pg, "probeSearchNav");
            QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
            QThread::msleep(40);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
            qInfo().noquote() << QString("[hl-scroll] page=%1 of %2 result pages")
                .arg(pg).arg(pages.size());
        }
    }
}

// ── Snapshot + dump mau (SPEC_PROBE_LOG_SNAPSHOT muc 3) ─────────────────────
// Chup cua so chinh ra PNG, ghi dump mau ra file .txt cung ten VA ra log.
// Chi DO, khong sua mau: pix=<mau diem anh that> lay bang w->grab().
bool MainWindow::probeSnapshot(const QString& pngPath, const QString& txtPath,
                               QString* errOut, int shotTab) {
    const ThemeTokens& t = m_darkMode ? darkHC() : lightHC();
    QString dump, err;
    // Nghiem thu SPEC_OCR_TAB: mo tab OCR (id 5) de dump thay duoc
    // "count sidebarTab=6" + cac nut trong panel OCR visible=1, roi phuc hoi
    // lai tab cu (--uiprobe va Ctrl+Shift+F12 deu dung ham nay).
    const int prevTab = m_thumbPanel ? m_thumbPanel->currentTabIndex() : 0;
    if (m_thumbPanel) {
        m_thumbPanel->selectTab(5);
        QCoreApplication::processEvents();
    }
    if (shotTab >= 0 && m_thumbPanel) {
        m_thumbPanel->selectTab(shotTab);
        QCoreApplication::processEvents();
    }
    const bool ok = UiProbe::snapshot(this, pngPath, txtPath, t, m_darkMode, &dump, &err);
    if (m_thumbPanel) m_thumbPanel->selectTab(prevTab);
    // Dump vao log de doc lai duoc khi khong mo duoc file txt.
    const QStringList lines = dump.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& line : lines)
        qInfo().noquote() << "[uiprobe]" << line;
    if (!ok) {
        qWarning().noquote() << "[uiprobe] FAIL" << err;
        if (errOut) *errOut = err;
    }
    return ok;
}

void MainWindow::captureUiSnapshot() {
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString png = QDir::tempPath() + QStringLiteral("/torreader-snap-") + stamp
                        + QStringLiteral(".png");
    const QString txt = QDir::tempPath() + QStringLiteral("/torreader-snap-") + stamp
                        + QStringLiteral(".txt");
    QString err;
    if (probeSnapshot(png, txt, &err))
        // Owner can copy duong dan nay -> hien du 8 giay.
        statusBar()->showMessage(QDir::toNativeSeparators(png), 8000);
    else
        statusBar()->showMessage("Snapshot failed: " + err, 8000);
}

// ── Search highlight helpers (shared by FindBar + SearchPanel) ─────────────────
void MainWindow::applySearchHighlights(const QList<SearchResult>& results, int currentIdx) {
    auto* t = currentTab();
    if (!t) return;
qDebug().noquote() << "[find] applySearchHighlights mode="
              << (m_fastMode ? "continuous" : "single")
              << "page=" << t->currentPage << "rects=" << results.size() << "currentIdx=" << currentIdx;
     if (m_fastMode && m_continuousView) {
        // Continuous: day TOAN BO ket qua (nhom theo trang) — view chi ve cac
        // trang dang trong vung nhin (xem ContinuousView::paintEvent).
        QHash<int, QList<QRectF>> byPage;
        for (int i = 0; i < results.size(); ++i)
            byPage[results[i].pageIndex].append(results[i].rects);
        int curPage = -1, pageLocalIdx = -1;
        if (currentIdx >= 0 && currentIdx < results.size()) {
            curPage = results[currentIdx].pageIndex;
            for (int i = 0; i < currentIdx; ++i)
                if (results[i].pageIndex == curPage) ++pageLocalIdx;
        }
        m_continuousView->setAllHighlights(byPage, curPage, pageLocalIdx);
        if (auto* view = t->view) view->clearHighlights();
    } else {
        // Single-page: chi ket qua cua trang dang xem (giu nguyen cach cu).
        QList<QRectF> pageRects;
        int pageLocalIdx = -1;
        int localCount = 0;
        for (int i = 0; i < results.size(); ++i) {
            if (results[i].pageIndex == t->currentPage) {
                pageRects.append(results[i].rects);
                if (i == currentIdx) pageLocalIdx = localCount;
                ++localCount;
            }
        }
        if (auto* view = t->view) {
            if (pageLocalIdx >= 0)
                view->setHighlights(pageRects, pageLocalIdx);
            else
                view->setHighlights(pageRects);
        }
        if (m_continuousView) m_continuousView->clearAllHighlights();
    }
}

void MainWindow::clearAllSearchHighlights() {
    if (auto* t = currentTab()) {
        if (t->view) t->view->clearHighlights();
    }
    if (m_continuousView) m_continuousView->clearAllHighlights();
}
// 0927 LƯỢT 6: `clearCacheSlot` + nút "Clear cache" ĐÃ BỊ GỠ HẲN (owner: "không
// được có nút clear cache nào nhé, app tắt pdf hay sao phải tự xóa chứ, sao lại
// tạo rác"). Cache không còn đường thủ công nào: tự xoá theo vòng đời tab
// (`TileCacheRegistry::release` + `VectorCache::purgeKey`) + tự dọn mồ côi lúc
// khởi động (`cleanupOrphanTorcache`).

// ── Tab switching / closing ───────────────────────────────────────────────────

void MainWindow::onTabChanged(int) {
    hideNotePopup();
    m_selPage = -1;
    m_selIdx = -1;
    auto* _t = currentTab();
    if (_t && _t->view) _t->view->clearSelectedAnnot();
    // Search state: moi tab giu RIENG ket qua (DocTab::searchResults). Chuyen
    // tab thi nap lai cua tab moi — tab khong co thi sidebar rong, khong tim lai.
    if (m_findBar) m_findBar->reset();
    clearAllSearchHighlights();
    auto* t = currentTab();
    // LÁT A 09/02: ContinuousView dung chung moi tab → repoint kho visuals VE TAB HIEN
    // HANH moi lan doi/dong tab, ke ca khi KHONG o fastMode (nhan tro, khong render).
    // Ngo le: dong tab hien hanh luc dang Single ⇒ con tro cu tro vao DocTab sap delete.
    if (m_continuousView) m_continuousView->setVisualsStore(t ? &t->visualsCache : nullptr);
    // LÁT B 09/02: kho lớp vector cũng repoint theo tab — ContinuousView dùng chung,
    // con trỏ cũ tro vào DocTab đã giải phóng là UB (kể cả khi đang ở chế độ Single).
    if (m_continuousView) m_continuousView->setVectorStore(t ? &t->vecLayers : nullptr);
    // SUA 2026-08-30: duoi cache uu tien tai lieu KHONG dang xem truoc — bao cho
    // PageCache doc nao la doc cua tab hien tai (doc chua mo xong thi raw() = null).
    PageCache::setActiveDoc(t ? t->doc->raw() : nullptr);
    if (t) {
        // 🔴 LƯỢT 33e (mục 3 — reviewer lỗi 3): tab vừa THÀNH HIỆN HÀNH phải xóa cờ
        // stopScan nó mang lúc còn nền (cờ MỘT CHIỀU — syncThumbnailPools đặt cho mọi
        // tab nền). Không xóa ⇒ mọi quyết định "nhường" sau này của tab đọc cờ cũ.
        // Chỉ xóa khi scan cũ ĐÃ xong: scan còn in-flight thì để nó tự dừng —
        // finished handler đọc scanStopped() đặt annotCacheValid rồi spawn lại (mục 2).
        if (t->annotMgr && !t->annotScanInFlight) t->annotMgr->resetScan();
        // Nap lai danh sach + truy van cua tab nay vao sidebar (rong thi de trong).
        if (m_thumbPanel)
            m_thumbPanel->setSearchResults(t->searchQuery, t->searchResults);
        if (!t->searchResults.isEmpty())
            applySearchHighlights(t->searchResults, t->searchCurrentIdx);
        // Only sync sidebar if the document is already open.
        // If it's still loading (async), the watcher's finished callback will call
        // syncSidebarToTab once the load completes. Calling it here with an unopened
        // doc poisons the setDocument early-return check and leaves lists empty.
        if (t->doc->isOpen())
            syncSidebarToTab(m_openDocs.indexOf(t));
        else
            m_thumbPanel->clearThumbnails();
        if (m_thumbPanel && m_thumbPanel->isCommentsTabVisible())
            onCommentsRequested();
        {
            QString nm = QFileInfo(t->originalPath.isEmpty()
                                   ? t->doc->filePath() : t->originalPath).fileName();
            setWindowTitle("TorReader PDF — " + QString(t->dirty ? "● " : "") + nm);
        }
        statusBar()->showMessage(
            QString("Page %1 / %2").arg(t->currentPage + 1).arg(t->doc->pageCount()), 2000);
        if (m_zoomEdit)
            m_zoomEdit->setText(QString::number(qRound(t->zoom * 100)) + "%");
        // Moi tab co view rieng → dong bo nut Select theo tool cua tab hien tai.
        if (m_selectTextAct && t->view)
            m_selectTextAct->setChecked(t->view->tool() == PdfGpuView::ViewTool::SelectText);
        // Dong bo tool xuong ContinuousView (view dung chung) theo tool cua tab.
        if (m_continuousView && t->view)
            m_continuousView->setTool(t->view->tool());
        // ── Vector overlay: show existing layer for this tab's current page ──
        // LÁT B 09/02: kho chung khoa theo trang -> lay dung trang hien hanh, khong con
        // chot "layer->pageIndex()==currentPage" (mau hien cua ki mot slot).
        setTabVectorLayer(t, t->vecLayers.value(t->currentPage), t->currentPage);
        if (m_fastMode && m_continuousView && t->doc->isOpen()) {
            // GOC1: use user's zoom, not kFullRenderMaxPx
            qDebug() << "[perf] cont tabChanged fast mode zoom=" << t->zoom;
            // 🔴 LƯỢT 33b (L): setDocument LUON reset cuon ve dinh (value=0) va xoa anh
            // ⇒ quay lai tab bi NHAY VE TRANG 1 + vung xem TRANG. Doc offset THAT cua tab
            // TRUOC setDocument (setDocument + hen gio pageChanged co the ghi de no),
            // setDocument (lay layout theo zoom cua tab), roi khoi phuc DUNG offset +
            // xin anh cac trang hien ngay.
            const int restoreY = t->contPosSaved ? t->contScrollY : 0;
            m_continuousView->setZoom(t->zoom);
            m_continuousView->setDocument(t->doc.get(), t->renderer.get(), &t->visualsCache, &t->vecLayers);
            m_continuousView->restoreScrollY(restoreY);
            if (t->annotMgr) refreshAnnotVisuals(t, t->currentPage);
        } else if (!m_fastMode && t->view && t->doc->isOpen()) {
            // 🔴 LƯỢT 33b (L): o che do Single moi tab co PdfGpuView rieng, no GIU trang
            // cua no — nhung khi quay lai tab lan DAU sau mot giai o Continuous, view co
            // the con o trang cu (0). Ép view ve DUNG trang da luu, giu zoom cua tab,
            // va xin anh ngay neu chua co (khong de TRANG). KHONG di qua onPageChanged
            // (no dung trang = ghi decurrentPage/vecGen khong can thiet).
            const int want = qBound(0, t->currentPage, t->doc->pageCount() - 1);
            if (t->view->currentPage() != want) {
                // View giu nguyen zoom cua no (widget ton tai theo tab) — KHONG setZoom
                // (no phat zoomChanged → onZoomChanged → requestPage khong can).
                const QImage cached = t->renderer ? t->renderer->bestCachedForPage(want) : QImage();
                const QSizeF sz = t->doc->pageSize(want);
                if (!cached.isNull()) {
                    t->view->setPage(want, cached, sz);
                    t->view->setPageBoxOrigin(t->doc->pageBoxOriginCached(want));
                } else {
                    t->view->setPendingPage(want, sz);
                    t->view->setPageBoxOrigin(t->doc->pageBoxOriginCached(want));
                    if (t->renderer) t->renderer->requestPage(want, t->zoom);
                }
            }
            // 🔴 LƯỢT 33e (mục 1 — reviewer lỗi 1): khi tab thành hiện hành PHẢI quét
            // lại trang hiển thị. Kết quả BO (tab nền lúc trước) không còn bị cache,
            // nhưng cache cũng không có dữ liệu ⇒ cache miss ⇒ rescan ở đây dựng lại
            // markup. Nhánh fastMode đã gọi refreshAnnotVisuals sau setDocument (:6417
            // cũ); Single không có đường nào gọi khi quay tab ⇒ thêm ở đây.
            if (t->annotMgr) refreshAnnotVisuals(t, want);
        }
} else {
         syncSidebarToTab(-1);
         setWindowTitle("TorReader PDF");
         statusBar()->showMessage("TorReader PDF  ·  Open a PDF to get started");
         // LƯỢT 35: Welcome tab dang hien hanh (khong co tai lieu nao) → an
         // ContinuousView, hien man chao bang PdfView (widget thuong). Day la hook
         // trung tam — moi duong dong het tab deu quy ve day qua currentChanged.
         applyWelcomeVisibility();
         if (m_fastMode && m_continuousView) {
             m_continuousView->clearDocument();
             m_continuousView->setVectorCacheKey(QString(), 0);
         }
     }
    syncThumbnailPoolsToActiveTab();
    updateUndoActions();
}

void MainWindow::onCommentsRequested() {
    // 0927 LƯỢT 10 (--no-annotscan): KHONG quet TOAN TAI LIEU (loadAllStreaming).
    if (trNoAnnotScan()) return;
    auto* t = currentTab();
    if (!t || !t->annotMgr || !t->doc || !t->doc->isOpen()) return;
    if (t->annotCacheValid) {
        m_thumbPanel->setComments(t->annotCache);
        return;
    }
    if (t->annotScanInFlight) {
        // 🔴 LƯỢT 33e (mục 2 — reviewer lỗi 2): scan cũ còn in-flight (thường là bị
        // stopScan cắt giữa chừng lúc tab còn nền) ⇒ bỏ qua nhưng ĐÁNH DẤU cần quét
        // lại — finished handler sẽ tự spawn khi nó dừng. Không đánh dấu ⇒
        // annotCacheValid=false mà không ai yêu cầu quét lại ⇒ panel Comments của tab
        // hiện hành trống vĩnh viễn.
        if (!t->annotCacheValid) t->annotRescanWanted = true;
        qDebug().noquote() << "[comments] FULL scan skipped (already in flight)"
                           << "canQuetLai=" << (t->annotRescanWanted ? 1 : 0);
        return;
    }

    int pageCount = t->doc->pageCount();
    int startPage = qBound(0, t->currentPage, pageCount - 1);
    auto* mgr = t->annotMgr.get();
    t->annotScanInFlight = true;
    t->annotCache.clear();
    t->annotCacheValid = false;

    qDebug().noquote() << "[comments] FULL scan start pages=" << pageCount
             << "startPage=" << startPage;
    m_thumbPanel->setCommentsLoading(true);

    qint64 scanStartMs = QDateTime::currentMSecsSinceEpoch();

    auto* watcher = new QFutureWatcher<void>(this);

    // Throttle timer: limit UI updates to ~7/sec during streaming scan
    auto* throttleTimer = new QTimer(this);
    throttleTimer->setSingleShot(true);

    QMetaObject::Connection pageConn;
    // Connect streaming signal — disconnected explicitly in finished
    pageConn = connect(mgr, &AnnotationManager::pageAnnotsLoaded, this,
            [this, t, throttleTimer, s = t->serial](int pageIndex, QList<AnnotInfo> annots) {
        if (!tabAlive(t, s)) return;   // 🔴 L22b: đảo lại — return khi tab CHẾT
        if (annots.isEmpty()) return;

        qDebug().noquote() << "[comments] recv page=" << pageIndex
                 << "n=" << annots.size()
                 << "cacheSize=" << t->annotCache.size();

        // Remove any stale entries for this page (shouldn't happen in practice)
        for (int i = t->annotCache.size() - 1; i >= 0; --i)
            if (t->annotCache[i].pageIndex == pageIndex)
                t->annotCache.removeAt(i);

        // Insert in page-ascending order (same logic as refreshCommentsForPage)
        int insertPos = 0;
        for (int i = 0; i < t->annotCache.size(); ++i) {
            if (t->annotCache[i].pageIndex >= pageIndex) break;
            insertPos = i + 1;
        }
        for (const auto& info : annots)
            t->annotCache.insert(insertPos++, info);

        // Throttle: reset timer so setComments runs at most ~7×/sec
        if (t == currentTab() && m_thumbPanel && m_thumbPanel->isCommentsTabVisible()) {
            if (!throttleTimer->isActive())
                throttleTimer->start(150);
        }
    });

    // Connect scan progress to panel (updates placeholder text until first real results)
    QMetaObject::Connection progressConn;
    progressConn = connect(mgr, &AnnotationManager::scanProgress, this,
            [this, t, s = t->serial](int scanned, int total) {
        if (!tabAlive(t, s)) return;   // 🔴 L22b: return khi tab CHẾT
        if (t == currentTab() && m_thumbPanel)
            m_thumbPanel->setCommentsProgress(scanned, total);
    });

    // Timer fires: push accumulated results to panel
    connect(throttleTimer, &QTimer::timeout, this, [this, t, s = t->serial]() {
        if (!tabAlive(t, s)) return;   // 🔴 L22b: return khi tab CHẾT
        if (t == currentTab() && m_thumbPanel)
            m_thumbPanel->setComments(t->annotCache);
    });

    connect(watcher, &QFutureWatcher<void>::finished, this,
            [this, watcher, t, throttleTimer, scanStartMs, pageConn, progressConn]() {
        disconnect(pageConn);
        disconnect(progressConn);
        watcher->deleteLater();
        if (m_openDocs.indexOf(t) < 0) return;

        throttleTimer->stop();
        // 🔴 LƯỢT 33d (mục 1): scan bị hủy giữa chừng (tab nền — stopScan/tab-nen)
        // KHÔNG được đánh dấu cache hoàn chỉnh; để valid=false ⇒ lần tab thành
        // hiện hành, onCommentsRequested sẽ quét tiếp từ đầu.
        t->annotCacheValid = !(t->annotMgr && t->annotMgr->scanStopped());
        t->annotScanInFlight = false;

        qDebug().noquote() << "[comments] scan DONE cacheSize=" << t->annotCache.size()
                 << "valid=" << t->annotCacheValid;

        if (t == currentTab()) {
            m_thumbPanel->setComments(t->annotCache);
            qint64 ms = QDateTime::currentMSecsSinceEpoch() - scanStartMs;
            qDebug().noquote() << "[comments] FULL scan done pages=" << t->doc->pageCount()
                     << "found=" << t->annotCache.size() << "ms=" << ms;
        }
        // 🔴 LƯỢT 33e (mục 2): tab thành hiện hành LÚC scan cũ còn chạy ⇒
        // annotRescanWanted được đặt (onTabChanged/onCommentsRequested). Scan cũ giờ
        // đã dừng thật — spawn lại từ đầu nếu kết quả bị hủy giữa chừng.
        if (t->annotRescanWanted) {
            t->annotRescanWanted = false;
            if (t == currentTab() && !t->annotCacheValid && m_thumbPanel
                && m_thumbPanel->isCommentsTabVisible()) {
                qDebug().noquote() << "[comments] scan bo-huy — QUET LAI theo co canQuetLai";
                onCommentsRequested();
            }
        }
    });

    mgr->resetScan();
    // 🔴 0928 LƯỢT 14: token sổ việc nền — loadAllStreaming() mượn trang của doc
    // qua PageCache. shutdownTab đã waitForFinished() future này, nhưng token
    // làm nổi bật nó trong `[dongdoc] cho vec=<n>` và là hàng rào cuối nếu
    // waitForFinished bị bỏ qua. LƯỢT 22: đăng ký LÚC SPAWN (UI thread) để
    // beginClose thấy cả lúc task còn xếp hàng.
    trdoc::Task task(mgr ? mgr->document() : nullptr, "annotScan");
    t->annotScanFuture = QtConcurrent::run([mgr, pageCount, startPage,
                                           task = std::move(task)]() {
        mgr->loadAllStreaming(pageCount, startPage);
    });
    watcher->setFuture(t->annotScanFuture);
    t->addBgWait(t->annotScanFuture);   // L22/L22b
}

void MainWindow::onTabClose(int idx) {
    QWidget* w = m_docTabs->widget(idx);
    for (int i = 0; i < m_openDocs.size(); ++i) {
        if (m_openDocs[i]->view != w) continue;
        DocTab* t = m_openDocs[i];

        // Warn about unsaved in-memory edits before discarding them.
        if (t->dirty) {
            m_docTabs->setCurrentIndex(idx);
            auto r = QMessageBox::question(this, "Unsaved Changes",
                QString("Do you want to save changes to \"%1\"?")
                    .arg(QFileInfo(t->originalPath).fileName()),
                QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
                QMessageBox::Yes);
            if (r == QMessageBox::Cancel) return;
            if (r == QMessageBox::Yes) {
                onSaveFile();
                if (t->dirty) return; // save failed / Save As cancelled → keep open
            }
        }
        // 🔴 P3 (0921): dọn sidecar "backup rồi xoá" của mọi mục undo/redo trước khi tab chết.
        for (const auto& ue : t->undoStack) dropUndoSidecar(ue);
        for (const auto& ue : t->redoStack) dropUndoSidecar(ue);
        // 0903: dung + CHO worker thumbnail thoat HET truoc khi UI cham PDFium
        // (clearCache, dong view, mo tab Welcome) — dong cua so dua gay văng pdfium.dll.
        // 0927 M2: pool hien dung CHUNG doi tuong cache cua tab (khong con mo rieng) —
        // `stopThumbPool()` = w->stop(); w->wait() (CHO HET task ghi nen thumbnail vao
        // .torcache) roi nha shared_ptr; nho do ma moi nha het handle tren file.
        // 0927 M5: do tre — `poolMs` la phan stop() + wait() tren UI thread (thu do
        // worker co thoat nhanh giua lat render), `ms` la ca phan dung pool + xoa dem.
        QElapsedTimer tcTimer;
        tcTimer.start();
        stopThumbPool(t);
        const qint64 poolMs = tcTimer.elapsed();
        // Close and remove tile cache file for this tab.
        // 0927 M1/M2: qua REGISTRY — no dem so tab dung, chi xoa file khi tab CUOI
        // cung roi (va no nha QLockFile + xoa .lock luon trong closeAndRemove).
        if (t->tileCache) {
            const auto rel = TileCacheRegistry::release(t->tileCache, "dongTab");
            // 🔴 LƯỢT 31 (A2 — reviewer bắt): GIU file (mac dinh, dong tab) khong
            // con bi ghi nham "XOA ok=0 ... khongCoTrongRegistry". `kept` phan biet
            // "giu co chu dich" voi "xoa that bai".
            if (rel.kept)
                qDebug().noquote() << QString("[torcache] GIU ok=1 %1 ly do=dongTab (giu file)")
                                          .arg(rel.path);
            else if (rel.removed)
                qDebug().noquote() << QString("[torcache] XOA ok=1 %1 ly do=dongTab loi=")
                                         .arg(rel.path);
            else
                qDebug().noquote() << QString("[torcache] XOA ok=0 %1 ly do=dongTab loi=%2")
                                         .arg(rel.path)
                                         .arg(rel.stillUsed > 0
                                              ? QStringLiteral("conTab=%1").arg(rel.stillUsed)
                                              : (rel.error.isEmpty() ? QStringLiteral("khongCoTrongRegistry")
                                                                    : rel.error));
        }
        qDebug().noquote() << QString("[torcache] dongTab ms=%1 poolMs=%2").arg(tcTimer.elapsed()).arg(poolMs);

        // Working-copy temp to clean up after the tab is gone (if any).
        // 🔴 0928 LƯỢT 21: DỜI tính `filePath()` vào closeJob, SAU waitForFinished —
        // `m_filePath` do open() (luồng nền) ghi ở dòng cuối; đọc trên UI thread
        // trong lúc open còn chạy là data race trên QString.

        // 0927 LƯỢT 8: bỏ hết khối tự chế thứ tự ở đây — `onTabClose` và
        // `~MainWindow` giờ dùng CHUNG `shutdownTab()` (huỷ render → chờ pool có
        // hạn → đóng heavyPrivPage/TextSelection → gỡ khỏi m_openDocs). Trước đây
        // đường đóng tab thiếu bước chờ annot (chuyển xuống job nền ⇒ tài liệu có
        // thể bị phá trong lúc `loadAllStreaming` còn chạy) và đường thoát app thì
        // thiếu bước huỷ render.
        if (m_navDeferTab == t) { m_navDeferTab = nullptr; m_navDeferPage = -1; }
        // 🔴 0928 LƯỢT 16 — đóng tab KHÔNG được chờ khoá của tab khác (log r8:
        // closeHandlePool chờ MEP thumbnail giữ khoá 2,9 s ⇒ cửa sổ UAF mở toang
        // 3 s). Đóng băng MỌI pool (kể cả tab sẽ thành hiện hành sau removeTab)
        // cho tới khi `delete t` dưới nền xong — closeJob.finished gọi lại sync.
        // 🔴 LƯỢT 16b — (1) bool → bộ đếm: hai tab đóng liên tiếp, job A xong
        // trước không được mở băng khi job B còn chạy; (2) gọi sync NGAY ở đây,
        // TRƯỚC shutdownTab: đóng tab nền có index > current thì removeTab không
        // nổ currentChanged ⇒ không được trông chờ đường removeTab→onTabChanged,
        // và teardown trên luồng UI phải chạy khi pool đã băng.
        ++m_thumbCloseJobs;
        syncThumbnailPoolsToActiveTab();
        m_openDocs.removeAt(i);
        // 🔴 0928 LƯỢT 26 (VIỆC 2): UI chỉ chờ render dừng 300 ms, và KHÔNG chạm
        // PDFium nữa (tail dời xuống closeJob dưới) — phần còn lại do nền lo
        // (delete t → ~PdfRenderer::waitIdle + beginClose token).
        // LƯỢT 37 (mục C): waitBgOnUi=false ⇒ bgWaits KHÔNG chặn UI; closeJob dưới chờ
        // chúng ở nền trước khi shutdownHeavy/doc->close. UI chỉ còn cho-annot + cho-render.
        shutdownTab(t, "dongTab", 300, /*pdfiumTailOnUi=*/false, /*waitBgOnUi=*/false);

        // 🔴 0928 LƯỢT 20 (dump TorReader_r8.exe.32120, PDB r8): shutdownTab mới ngắt
        // pageReadyConn/scrollConn. t->renderer/t->annotMgr/t->annotLayer/t->thumbPool
        // vẫn SỐNG — chúng chỉ chết ở `delete t` chạy nền (closeJob dưới) — và vẫn phát
        // pagePartial/pageReady/continuousPageReady/objectCountReady/annotationAdded/
        // pageContentChanged vào các lambda nối với `this` bắt `tab`; những lambda đó
        // dereference tab->view vừa bị `delete` ngay dưới ⇒ AV đọc NULL+0x23c ở
        // PdfGpuView::showPartial (PdfGpuView.cpp:952). NGắt MỌI kết nối từ các QObject
        // của tab tới MainWindow TRƯỚC khi đụng t->view. (tab->view không cần ngắt:
        // `delete t->view` tự gỡ kết nối của nó. Event đã post vào queue TRƯỚC khi
        // disconnect vẫn tới — chốt m_openDocs.contains(tab) trong từng lambda là
        // lưới cuối.)
        // 🔴🔴 0928 LƯỢT 22 (reviewer mục 4): bản L20 chỉ ngắt CÓ HƯỚNG vào `this`.
        // ~QObject của renderer/annotMgr/annotLayer/thumbPool chạy TRÊN LUỒNG NỀN
        // (closeJob `delete t`) sẽ sửa danh sách sender/receiver của MỌI đối tượng
        // còn sống nó từng nối — ContinuousView (nối continuousPageReady/regionReady/
        // requestRegion — ContinuousView.cpp:414) và ThumbnailPanel (nối pool
        // thumbnailReady, nhận con trỏ renderer/pool) — trong lúc luồng UI đang
        // connect/new. Data race trên danh sách children/sender = crash cùng họ.
        // SỬA: ngắt HAI CHIỀU (mọi sender→object và object→mọi receiver) NGAY ĐÂY,
        // trên luồng UI, TRƯỚC khi closeJob được phép chạy `delete t`. Sau lệnh này
        // ~QObject nền không còn đụng đối tượng sống nào nữa.
        // 🔴 LƯỢT 22b (reviewer mục 2): disconnect KHÔNG gỡ được sự kiện ĐÃ POST.
        // Renderer có con QFutureWatcher(this) + invokeMethod Queued — events đó
        // nằm trong hàng đợi UI, còn ~PdfRenderer chạy ở nền (closeJob dưới).
        // prepareForClose dọn trên UI: cờ closing + xoá watcher + removePostedEvents.
        if (t->renderer) t->renderer->prepareForClose();
        for (QObject* o : { static_cast<QObject*>(t->renderer ? t->renderer.get() : nullptr),
                            static_cast<QObject*>(t->annotMgr  ? t->annotMgr.get()  : nullptr),
                            static_cast<QObject*>(t->annotLayer? t->annotLayer.get(): nullptr),
                            static_cast<QObject*>(t->thumbPool ? t->thumbPool.get() : nullptr) }) {
            if (!o) continue;
            QObject::disconnect(o, nullptr, nullptr, nullptr);   // o là sender
            QObject::disconnect(nullptr, nullptr, o, nullptr);   // o là receiver
        }
        // 🔴 LƯỢT 22 (reviewer mục 5): ContinuousView/ThumbnailPanel còn GIỮ CON TRỎ
        // renderer/pool của tab đang đóng. ContinuousView chỉ được setDocument lại
        // khi fastMode (6155) ⇒ đóng tab hiện hành lúc ở Single để lại con trỏ treo.
        if (m_continuousView && t->renderer && m_continuousView->isUsingRenderer(t->renderer.get()))
            m_continuousView->clearDocument();   // null doc+renderer, ngắt kết nối của nó

        m_docTabs->removeTab(idx);
        delete t->view;
        t->view = nullptr;
        if (m_openDocs.isEmpty()) {
            addWelcomeTab();
            m_thumbPanel->clearThumbnails();
            setWindowTitle("TorReader PDF");
        }
        // 🔴🔴 0929 LƯỢT 33h (thay "delete t chạy nền" — chính là gốc crash r33g):
        // nền CHỈ làm phần nặng KHÔNG-QObject (shutdownHeavy + doc->close); `delete t`
        // (phần QObject, affinity UI) quay về UI ở finished handler. PDFium đã xong
        // một phần trong shutdownTab() ở trên.
        auto* closeJob = new QFutureWatcher<void>(qApp);
        // 🔴 0928 LƯỢT 16: `delete t` xong mới mở băng thumbnail cho tab hiện hành —
        // tránh đúng cảnh log r8: main vừa đổi tab xong là MEP thumbnail giành lại
        // khoá, trong lúc luồng nền còn FPDF_CloseDocument 12 handle của tab vừa đóng.
        // 🔴 LƯỢT 16b — --bộ đếm (mở băng chỉ khi closeJob CUỐI cùng xong) và dùng
        // `this` làm context: lambda gắn vào `this` tự ngắt khi MainWindow chết,
        // closeJob (con của qApp) không đảm bảo điều đó.
        QObject::connect(closeJob, &QFutureWatcher<void>::finished, this, [this, t]{
            // 🔴🔴 0929 LƯỢT 33h (dump r33g): mọi QObject của DocTab (renderer/annotMgr/
            // annotLayer/thumbPool) có affinity LUỒNG UI ⇒ PHẢI chết TRÊN UI. Nền đã làm
            // xong phần nặng KHÔNG-QObject (shutdownHeavy + doc->close). removePostedEvents
            // cho obj + MỌI CON ngay trước delete: Qt gỡ QMetaCallEvent đã post (vd
            // pageObjectCount queue) trên ĐÚNG luồng sở hữu queue ⇒ ~QObject không còn đua
            // với luồng UI đang giao event (UAF đọc 0x8 ở setPageObjectCount).
            for (QObject* o : { static_cast<QObject*>(t->renderer.get()),
                                static_cast<QObject*>(t->annotMgr.get()),
                                static_cast<QObject*>(t->annotLayer.get()),
                                static_cast<QObject*>(t->thumbPool.get()) }) {
                if (!o) continue;
                QCoreApplication::removePostedEvents(o);
                for (QObject* c : o->findChildren<QObject*>())
                    QCoreApplication::removePostedEvents(c);
            }
            delete t;   // ~PdfRenderer/~AnnotationManager gọi lại shutdownHeavy → no-op ⇒ UI không đứng
            --m_thumbCloseJobs;
            syncThumbnailPoolsToActiveTab();
            if (heapCompactOnClose()) compactAllHeaps();
        });
        QObject::connect(closeJob, &QFutureWatcher<void>::finished,
                         closeJob, &QObject::deleteLater);
        closeJob->setFuture(QtConcurrent::run([t]() mutable {
             // 🔴 0928 LƯỢT 21 (dump 32832): open() còn chạy trên pool ⇒ CHỜ nó xong
             // HẲN mới đụng doc. waitForFinished ở ĐÂY (luồng nền), không phải UI.
             if (t->openFuture.isValid()) t->openFuture.waitForFinished();
             // 🔴 LƯỢT 37 (mục C): bgWaits rời UI xuống ĐÂY — mọi QtConcurrent của tab
             // (annot/visual/region/fgn/translate) phải xong TRƯỚC shutdownHeavy (làm
             // PageCache::forgetDocument) + doc->close, nhưng KHÔNG trên UI (đó chính là
             // 1,68 s đứng hình). Task chỉ đọc doc/annotMgr (còn sống tới `delete t` ở
             // finished handler sau job này) ⇒ chờ ở nền là đúng thứ tự "xong-hẵng-đóng".
             for (auto& w : t->bgWaits) w.wait();
            // 🔴 0928 LƯỢT 26 (VIỆC 2): tail PDFium chạy Ở ĐÂY — sau open(), TRƯỚC teardown.
            closeHeavyPriv(t);
            if (t->doc) TextSelection::closeDocument(t->doc->raw());
            const QString workingTmp = (t->doc->filePath() != t->originalPath)
                                       ? t->doc->filePath() : QString();
            // 🔴🔴 0929 LƯỢT 33h: CHỈ phần nặng KHÔNG-QObject chạy ở nền:
            //   • renderer->shutdownHeavy() = huỷ + chờ pool + đóng 12 doc pool (mutex/atomic/
            //     QThreadPool, không đụng event queue) ⇒ ~PdfRenderer trên UI sau đó là no-op.
            //   • annotMgr->shutdownHeavy() = flush pending gen + PageCache::forgetDocument(m_doc)
            //     — PHẠM VI CHẠM doc ⇒ chạy TRƯỚC doc->close().
            //   • doc->close() = FPDF_CloseDocument doc chính + unmap (PdfDocument KHÔNG phải
            //     QObject ⇒ an toàn ở nền; ~PdfDocument gọi lại close() → no-op).
            // `delete t` (phần QObject) KHÔNG ở đây nữa — quay về UI ở finished handler trên.
            if (t->renderer) t->renderer->shutdownHeavy();
            if (t->annotMgr) t->annotMgr->shutdownHeavy();
            if (t->doc) t->doc->close();
            if (!workingTmp.isEmpty()) QFile::remove(workingTmp);
        }));
        return;
    }
}

// ── Page navigation ───────────────────────────────────────────────────────────

void MainWindow::onPageChanged(int pageIndex) {
    const qint64 flipAtMs = QDateTime::currentMSecsSinceEpoch();
    m_lastNavMs = flipAtMs;
    hideNotePopup();
    m_selPage = -1;
    m_selIdx = -1;
    auto* _t = currentTab();
    if (_t && _t->view) _t->view->clearSelectedAnnot();
    // Link noi bo: user tu dieu huong thi huy vi tri center dang cho.
    for (auto* tab : m_openDocs)
        tab->pendingLinkCenterActive = false;
    auto* t = currentTab();
    qDebug() << "[Main] onPageChanged req=" << pageIndex
             << "hasTab=" << (t != nullptr)
             << "isOpen=" << (t && t->doc ? t->doc->isOpen() : false)
             << "current=" << (t ? t->currentPage : -99);
    if (!t || !t->doc->isOpen()) return;
    int total = t->doc->pageCount();

    pageIndex = qBound(0, pageIndex, total - 1);
    {
        int oldPage = t->currentPage;
        if (pageIndex == oldPage) return;
        t->currentPage = pageIndex;
        // 🔴 0928 LƯỢT 13: đổi trang ⇒ TĂNG THẾ HỆ. Mọi `VectorLayer::build` đang
        // chạy cho trang cũ thấy thế hệ lệch và bỏ ở rạch lát kế tiếp. Không có
        // dòng này, End+PgUp×111 xếp 26 build vector cho trang 349→324 (đã lướt
        // qua) trước trang đang xem — đo được 143 s build vector mỗi phiên và
        // `[lockwait] ms= 2905` cho chính trang người dùng đang nhìn.
        t->vecGen->fetch_add(1, std::memory_order_acq_rel);
        // LAT G: roi trang cu — dong handle rieng neu khong co tac vu dang chay; neu
        // dang chay thi watcher cua no thay currentPage != pg va dong lai. Reset danh
        // dien vung nhin de trang moi duoc xet dung region tu dau.
        if (!t->heavyRegionBuilding) closeHeavyPriv(t);
        t->heavySeenPage = -1;
        // SU KIEN 0902 (thay polling 1s): trang MOI den co the la trang nang — goi
        // ngay updateHeavyRegion (tu guard: khong heavy / nen chua san sang thi return).
        updateHeavyRegion(t);
        // 🔴🔴 2026-09-01: bao cho bo render biet trang HIEN TAI la trang nao.
        // Thieu buoc nay thi m_currentPage mai la 0, va MOI anh chat luong day du cua
        // trang khac 0 deu bi vut o cua cuoi voi "drop reason=notCurrent" — day la ly do
        // "tao Text o trang 3 khong bao gio hien" (bat duoc trong log cua owner).
        if (t->renderer) t->renderer->setCurrentPage(pageIndex);
        t->fgnDeferred.clear();   // moi lan hien duoc hoan lop bu toi da 1 lan
        t->renderer->setCurrentPage(pageIndex);

        // ── R1 (SPEC_PERF_HEAVYPAGE): pin trang hien tai va +-1 để LRU khong duoi;
        //    unpin trang khong con trong pham vi. Khong cham s_pdfiumMutex. ──
        FPDF_DOCUMENT pinDoc = t->doc->raw();
        const int pin0 = pageIndex;
        const int pinM = pageIndex - 1;
        const int pinP = pageIndex + 1;
        if (pinM >= 0) PageCache::pin(pinDoc, pinM);
        PageCache::pin(pinDoc, pin0);
        if (pinP < total) PageCache::pin(pinDoc, pinP);
        // Unpin trang cu (neu khong con trong pham vi moi).
        for (int un : {oldPage, oldPage - 1, oldPage + 1}) {
            if (un >= 0 && un < total && un != pin0 && un != pinM && un != pinP)
                PageCache::unpin(pinDoc, un);
        }

        // ── Placeholder LEN DAU (SPEC_NAV_INSTANT) — tuyet doi KHONG co loi goi
        //    PDFium dong bo nao TRUOC khoi nay. View ve ngay, việc nặng de sau. ──
        QImage cached = t->renderer->bestCachedForPage(pageIndex);
        QSizeF sz = t->doc->pageSize(pageIndex);
        if (!cached.isNull()) {
            t->view->setPage(pageIndex, cached, sz);
            t->view->setPageBoxOrigin(t->doc->pageBoxOriginCached(pageIndex));
        } else {
            // Show pending page immediately. Old image is cleared; placeholder thumbnail
            // is shown while full render loads (same pattern as Okular/Acrobat).
            t->view->setPendingPage(pageIndex, sz);
            t->view->setPageBoxOrigin(t->doc->pageBoxOriginCached(pageIndex));
            QImage thumb = m_thumbPanel->thumbnailForPage(pageIndex);
            if (!thumb.isNull()) {
                qDebug() << "[perf] placeholder feed thumb page=" << pageIndex;
                t->view->setPlaceholder(thumb);
            } else {
                qDebug() << "[perf] placeholder feed blank page=" << pageIndex;
            }
        }
        t->renderer->cancelPending();
        const qint64 placeholderMs = QDateTime::currentMSecsSinceEpoch() - flipAtMs;

        // ── Hoan viec nang (refreshAnnotVisuals + ensureForeignAnnotLayer) qua
        //    QTimer 120ms dung chung. Lat nhanh chi lam viec nang cho trang dung. ──
        if (m_navDeferTab) {
            // Flip truoc chua kip chay viec nang thi da bi lat nhanh choi qua.
            qDebug().noquote() << QString("[nav] flip from=%1 to=%2 placeholderMs=%3 deferredStartMs=%4 visualsMs=-1 stale=1")
                .arg(m_navFlipFrom).arg(m_navDeferPage).arg(m_navPlaceholderMs).arg(m_navDeferredStartMs);
            m_navLogArmed = false;
        }
        m_navDeferTab = t;
        m_navDeferPage = pageIndex;
        m_navFlipFrom  = oldPage;
        m_navFlipAtMs  = flipAtMs;
        m_navPlaceholderMs = placeholderMs;
        m_navDeferredStartMs = -1;   // viec nang chua bat dau cho flip nay
        m_navLogArmed = false;
        m_navDeferTimer->start(120);

        // ── Vector overlay: build if page changed and not already building ──
        // 🔴 0928 LƯỢT 13: trang này phải còn là trang đang XEM thì mới đáng dựng.
        // Trước đây mỗi lần lật trang đều spawn 1 task build vector; khi người dùng
        // lướt nhanh (PgUp×111) thì 26 task xếp hàng TRƯỚC việc của trang đang xem
        // và mỗi cái giữ khoá chung 0,5–7 s. Không thêm điều kiện nào thì cứ mỗi
        // trang lướt qua là một lần giành khoá thừa — đó là gốc của 17 giay.
        if (!t->vecBuilding.contains(pageIndex)
            && !t->vecLayers.contains(pageIndex)) {
            int pg = pageIndex;
            t->vecBuilding.insert(pg);
            auto layer = std::make_shared<VectorLayer>();
            auto* w = new QFutureWatcher<bool>(this);
            connect(w, &QFutureWatcher<bool>::finished, this, [this, w, t, pg, layer, s = t->serial]{
                w->deleteLater();
                if (!tabAlive(t, s)) return;   // 🔴 L20: chốt TRƯỚC khi dereference
                t->vecBuilding.remove(pg);
                if (t->currentPage != pg) return;
                if (t != currentTab()) return;
                if (w->result()) {
                    setTabVectorLayer(t, layer, pg);
                } else {
                    setTabVectorLayer(t, nullptr, pg);
                }
            });
            FPDF_DOCUMENT d = t->doc->raw();
            const QString pdfPath = t->pdfPath;
            const quint64 pdfHash = t->pdfHash;
            const QString docPath = t->doc->filePath();
            const bool allowCache = !t->torvecDirty.contains(pg);
            const quint32 myGen = t->vecGen->load(std::memory_order_acquire);
            auto gen = t->vecGen;   // 🔴 L22: shared_ptr — lambda nền không đọc `t`
            // 🔴 LƯỢT 22: token lúc SPAWN + RAII move; bỏ đọc t->vecGen/t->currentPage
            // trên luồng nền (xem naiLai — cùng khuôn, cùng lý do).
            trdoc::Task task(d, "VectorLayer::build/latTrang");
            w->setFuture(QtConcurrent::run([layer, d, pg, pdfPath, pdfHash, docPath, allowCache,
                                            gen, myGen, task = std::move(task)]{
                // Thu cache .torvec truoc — nap nhanh gap 45 lan so voi dung lai tu PDF.
                // Khoa cache co the chua kip dat (initWatcher chay bat dong bo). Tu bu: hashFile chi
                // doc 128 KB (64 KB dau + 64 KB cuoi) nen re, an toan goi o luong nen.
                const QString keyPath = pdfPath.isEmpty() ? docPath : pdfPath;
                const quint64 keyHash = pdfHash ? pdfHash : (quint64)TileCacheFile::hashFile(keyPath);
                if (allowCache && VectorCache::tryLoad(*layer, keyPath, keyHash, pg)) return true;
                const auto huy = [gen, myGen, &task] {
                    return gen->load(std::memory_order_acquire) != myGen
                        || task.cancelled();
                };
                if (!layer->build(d, pg, huy)) return false;
                if (allowCache) VectorCache::trySave(*layer, keyPath, keyHash, pg);
                return true;
            }));
        }
    }
    // If page is already cached (memory or disk), serve immediately — no settle.
    // Only defer new renders (cache miss) via settle timer.
    bool cacheHit = t->renderer->requestFromCacheOnly(pageIndex, t->zoom);
    if (cacheHit) {
        qDebug() << "[Main] cache hit page=" << pageIndex << "— immediate, no settle";
    } else {
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_settleStartMs == 0)
            m_settleStartMs = now;
        qint64 totalPending = now - m_settleStartMs;
        if (totalPending >= 600) {
            qDebug() << "[Main] settle FORCED after" << totalPending << "ms of continuous scrolling page=" << pageIndex;
            m_settleStartMs = 0;
            if (!m_fastMode) t->renderer->requestPage(pageIndex, t->zoom);
            statusBar()->showMessage(
                QString("Page %1 / %2 — Loading…").arg(pageIndex + 1).arg(t->doc->pageCount()));
        } else {
            qDebug() << "[Main] cache miss page=" << pageIndex << "— waiting 400ms settle (totalPending=" << totalPending << "ms)";
            m_settleTimer->start();
        }
    }
    // Sync sidebar thumbnail immediately (same event-loop pass)
    m_thumbPanel->setCurrentPage(pageIndex);

    if (m_fastMode && m_continuousView)
        m_continuousView->scrollToPage(pageIndex, "onPageChanged");

    statusBar()->showMessage(
        QString("Page %1 / %2").arg(pageIndex + 1).arg(total));

    m_warmTimer->setInterval(400);
    m_warmTimer->start();

    schedulePagePrefetch();

    notifyOcrStatusForPage(pageIndex);
    }

// SPEC_PAGECACHE_CORE muc 4: moi lan doi trang thi start lai timer 300 ms —
// lat lien tuc se lien tuc reset (huy prefetch cu), chi prefetch khi that su dung.
void MainWindow::schedulePagePrefetch() {
    if (m_preloadTimer) m_preloadTimer->start();
}

// ── Nav instant (SPEC_NAV_INSTANT_2026-08-16): việc nặng chạy HOAN qua 120ms. ──
// QTimer dung chung, moi lan doi trang thi start() lai (debounce). Khi chay:
// kiem stale (trang/tab da doi) — sai thi bo qua, khong dong vao view.
void MainWindow::onNavDeferred() {
    DocTab* t = m_navDeferTab;
    if (!t) return;
    const int page = m_navDeferPage;
    const int from = m_navFlipFrom;
    const qint64 flipAt = m_navFlipAtMs;
    const qint64 placeholderMs = m_navPlaceholderMs;

    if (!m_openDocs.contains(t) || t->currentPage != page) {
        const qint64 deferredStartMs = QDateTime::currentMSecsSinceEpoch() - flipAt;
        qDebug().noquote() << QString("[nav] flip from=%1 to=%2 placeholderMs=%3 deferredStartMs=%4 visualsMs=-1 stale=1")
            .arg(from).arg(page).arg(placeholderMs).arg(deferredStartMs);
        m_navLogArmed = false;
        m_navDeferTab = nullptr;
        m_navDeferPage = -1;
        return;
    }
    const qint64 deferredStartMs = QDateTime::currentMSecsSinceEpoch() - flipAt;
    QElapsedTimer _sync; _sync.start();
    refreshAnnotVisuals(t, page);
    ensureForeignAnnotLayer(t, page);
    if (t->visualsScanning.contains(page)) {
        // Rescan NANG chay ngoai UI thread — arm log; apply-back se in [nav] line
        // voi visualsMs = thoi gian that (chay nền, khong tinh vao cam nhan).
        m_navLogArmed = true;
        m_navDeferredStartMs = deferredStartMs;
        m_navVisualsStartMs  = QDateTime::currentMSecsSinceEpoch();
        // Giu nguyen m_navDeferTab/Page — apply-back dung de nhan dien flip nay.
    } else {
        // CACHE HIT — dong bo, re: in ngay line [nav].
        const qint64 visualsMs = _sync.elapsed();
        qDebug().noquote() << QString("[nav] flip from=%1 to=%2 placeholderMs=%3 deferredStartMs=%4 visualsMs=%5 stale=0")
            .arg(from).arg(page).arg(placeholderMs).arg(deferredStartMs).arg(visualsMs);
        m_navLogArmed = false;
        m_navDeferTab = nullptr;
        m_navDeferPage = -1;
    }
}

void MainWindow::onCommentActivated(int pageIndex, int annotIndex) {
    auto* t = currentTab();
    if (!t || !t->doc->isOpen()) return;
    pageIndex = qBound(0, pageIndex, t->doc->pageCount() - 1);

    const auto& list = annotsForPage(t, pageIndex);
    if (annotIndex < 0 || annotIndex >= list.size()) return;
    QRectF r = list[annotIndex].rect.normalized();  // copy local — dangling guard
    const QString aUid = list[annotIndex].uid;       // copy truoc khi onPageChanged co the
    const QString aType = list[annotIndex].type;     // invalidate cache lam list danh

    double oldZoom = t->zoom;
    onPageChanged(pageIndex);

if (m_fastMode && m_continuousView) {
         m_continuousView->scrollToPage(pageIndex, "onNavDeferred");
         // ponytail: no zoom/center/select in continuous mode — would fight its layout
         return;
     }

    double viewportW = t->view->width();
    double want = viewportW * 0.5 / qMax(r.width(), 1.0);
    double newZoom = qBound(oldZoom, want, 2.0);

    t->zoom = newZoom;
    t->view->setZoom(newZoom);
    if (m_zoomEdit)
        m_zoomEdit->setText(QString::number(qRound(newZoom * 100)) + "%");

    t->view->centerOnPageRect(r);
    refreshAnnotVisuals(t, pageIndex);
    m_selPage = pageIndex;
    m_selIdx  = annotIndex;
    t->view->setSelectedAnnot(r);
    if (aType == QLatin1String("FreeText")) {
        t->view->setDragNote(r);
    } else {
        t->view->setDragTarget(aUid, QString(), 0.0f, QColor());
    }

    qDebug().noquote() << QString("[comments] activated page=%1 idx=%2 zoom=%3→%4 rect=(%5,%6,%7,%8)")
        .arg(pageIndex).arg(annotIndex)
        .arg(oldZoom, 0, 'f', 3).arg(newZoom, 0, 'f', 3)
        .arg(r.x(), 0, 'f', 1).arg(r.y(), 0, 'f', 1)
        .arg(r.width(), 0, 'f', 1).arg(r.height(), 0, 'f', 1);
}

// ── Link PDF (SPEC_PDF_LINKS muc 3 + 4) ──────────────────────────────────────

// Center lai trang dich khi render xong (single mode). pageReady goi setPage
// lai reset m_panOffset => phai ap lai vi tri center + flash.
void MainWindow::applyPendingLinkCenter(DocTab* t) {
    if (!t->pendingLinkCenterActive || !t->view) return;
    t->view->centerOnPageRect(t->pendingLinkCenterRect);
    t->view->flashRect(t->pendingLinkCenterRect);
    t->pendingLinkCenterActive = false;
}

void MainWindow::onLinkActivated(DocTab* t, int page, const PdfLink& link) {
    if (!t || !t->doc || !t->doc->isOpen()) return;

    // ── Link ngoai: hoi xac nhan truoc khi mo trinh duyet ──
    if (!link.uri.isEmpty()) {
        const QUrl url(link.uri);
        const QString scheme = url.scheme().toLower();
        if (scheme != "http" && scheme != "https" && scheme != "mailto") {
            statusBar()->showMessage(
                QString("Link blocked: scheme \"%1\" not allowed").arg(scheme), 5000);
            return;
        }
        QMessageBox box(this);
        box.setWindowTitle("Open external link?");
        box.setIcon(QMessageBox::Question);
        box.setText("This PDF contains a link to:\n\n" + link.uri);
        box.setStandardButtons(QMessageBox::Open | QMessageBox::Cancel);
        box.setDefaultButton(QMessageBox::Cancel);
        if (box.exec() == QMessageBox::Open) {
            QDesktopServices::openUrl(url);
            statusBar()->showMessage("Opening: " + link.uri, 4000);
        }
        return;
    }

    // ── Link noi bo: cuon/center toi trang dich + dung vi tri, KHONG doi zoom ──
    if (link.destPage >= 0) {
        const int dest = qBound(0, link.destPage, t->doc->pageCount() - 1);
        QRectF targetPdf;
        if (link.destX >= 0 && link.destY >= 0) {
            const QRectF src = link.rectPdf.normalized();
            double w = qBound(20.0, src.width()  > 4 ? src.width()  : 120.0, 400.0);
            double h = qBound(16.0, src.height() > 4 ? src.height() : 40.0,  200.0);
            targetPdf = QRectF(link.destX - w / 2.0, link.destY - h / 2.0, w, h);
        }

if (m_fastMode && m_continuousView) {
             if (targetPdf.isEmpty()) {
                 m_continuousView->scrollToPage(dest, "onLinkActivated");
             } else {
                 const PdfLinks::PageInfo info = PdfLinks::pageInfo(t->doc->raw(), dest);
                 const QRectF disp = pdfRectToDisp(targetPdf, info.dispW, info.dispH,
                                                   info.rot, info.boxX, info.boxY);
                 m_continuousView->scrollToPageRect(dest, disp);
                 m_continuousView->flashPageRect(dest, disp);
             }
         } else if (t->view) {
            onPageChanged(dest);
            if (!targetPdf.isEmpty()) {
                const PdfLinks::PageInfo info = PdfLinks::pageInfo(t->doc->raw(), dest);
                const QRectF disp = pdfRectToDisp(targetPdf, info.dispW, info.dispH,
                                                  info.rot, info.boxX, info.boxY);
                t->pendingLinkCenterPage = dest;
                t->pendingLinkCenterRect = disp;
                t->pendingLinkCenterActive = true;
                applyPendingLinkCenter(t);   // trang da cache: center ngay
            }
        }
        return;
    }

    // PDFACTION_LAUNCH / REMOTEGOTO / UNSUPPORTED: khong ho tro.
    statusBar()->showMessage("Link type not supported", 4000);
}

void MainWindow::onZoomChanged(double scale) {
    auto* t = currentTab();
    if (!t) return;
    t->zoom = qBound(0.1, scale, 10.0);
    if (m_fastMode && m_continuousView && m_continuousView->isVisible()) {
        m_continuousView->setZoom(t->zoom);
    } else {
        if (t->view) t->view->setZoom(t->zoom);
    }
    if (m_zoomEdit)
        m_zoomEdit->setText(QString::number(qRound(t->zoom * 100)) + "%");
    // Refresh annot visuals for current page (data unchanged but overlay re-scales)
    if (t->view && t->doc && t->doc->isOpen()) {
        refreshAnnotVisuals(t, t->currentPage);
        // MUC 2 (SPEC_PERF_FGNLAYER_ADAPTIVE 2026-08-30): phong to dang ke thi dung lai
        // lop bu o zoom moi — ensureForeignAnnotLayer tu quyet (builtZoom * 1.6).
        ensureForeignAnnotLayer(t, t->currentPage);
    }
}

// ── Drag-drop ─────────────────────────────────────────────────────────────────

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e) {
    for (const QUrl& url : e->mimeData()->urls()) {
        if (url.isLocalFile() &&
            url.toLocalFile().endsWith(".pdf", Qt::CaseInsensitive))
            openFile(url.toLocalFile());
    }
}

void MainWindow::closeEvent(QCloseEvent* e) {
    int dirtyCount = 0;
    for (auto* t : m_openDocs) if (t->dirty) ++dirtyCount;
    if (dirtyCount > 0) {
        auto r = QMessageBox::question(this, "Unsaved Changes",
            QString("Do you want to save changes to %1 document(s)?")
                .arg(dirtyCount),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
            QMessageBox::Yes);
        if (r == QMessageBox::Cancel) { e->ignore(); return; }
        // 0927 LUOT 11: user vua tra loi "Co" (luu). Neu luu that bai, ban nhap
        // `.tortmp` la ban DUY NHAT con lai cua thay doi cua user ⇒ duong thoat
        // nhanh khong duoc xoa no (xem core/FastExit.h).
        trExitDraftPolicy() = 2;
        if (r == QMessageBox::Yes) {
            for (auto* t : m_openDocs) {
                if (!t->dirty) continue;
                m_docTabs->setCurrentWidget(t->view);
                onSaveFile();
            }
        }
    }
// Clear tile cache for all tabs on exit
    // 0927: duyet tren BAN CHEP danh sach. Vong `onSaveFile()` o tren co the sua
    // m_openDocs (no -> loadTabFile), duyet truc tiep se lam vo chi so.
    QElapsedTimer closeTimer;
    closeTimer.start();
    const auto docs = m_openDocs;
    for (auto* t : docs) {
        if (!t) continue;
        // 0927 M2: pool giu CHUNG doi tuong cache cua tab nen chi can dung worker
        // truoc khi registry bo dem; stopThumbPool() = w->stop(); w->wait() (CHO HET
        // task ghi nen thumbnail vao .torcache) roi nha shared_ptr.
        stopThumbPool(t);
        // 0927 M1/M2: qua REGISTRY — chi xoa khi tab CUOI cua file do cung roi,
        // roi nha QLockFile + xoa .lock (closeAndRemove lo phan nay).
        if (t->tileCache) {
            const auto rel = TileCacheRegistry::release(t->tileCache, "thoatApp");
            if (rel.kept)
                qDebug().noquote() << QString("[torcache] GIU ok=1 %1 ly do=thoatApp (giu file)")
                                          .arg(rel.path);
            else if (rel.removed)
                qDebug().noquote() << QString("[torcache] XOA ok=1 %1 ly do=thoatApp loi=")
                                         .arg(rel.path);
            else
                qDebug().noquote() << QString("[torcache] XOA ok=0 %1 ly do=thoatApp loi=%2")
                                         .arg(rel.path)
                                         .arg(rel.stillUsed > 0
                                              ? QStringLiteral("conTab=%1").arg(rel.stillUsed)
                                              : (rel.error.isEmpty() ? QStringLiteral("khongCoTrongRegistry")
                                                                    : rel.error));
        }
    }
    qDebug().noquote() << QString("[torcache] closeEvent ms=%1 tab=%2").arg(closeTimer.elapsed()).arg(docs.size());
    // 0927 LUOT 11: den day MOI VIEC BAT BUOC da xong (hoi luu -> luu -> dung pool ->
    // rut .torcache/.torvec/.lock). Danh dau cho main() biet duoc phep thoat nhanh
    // sau app.exec() ma KHONG chay ham huy. Dat SAU e->accept() va KHONG dat o
    // nhanh Cancel o tren (truong hop do KHONG thoat).
    trMarkExitReady();
    e->accept();
}

// 0927 LUOT 11 -- xoa ban nhap `.tortmp` khi thoat nhanh. ~MainWindow (duong cu)
// lam viec nay SAU khi `delete t` da nha handle PDFium; thoat nhanh bo qua
// ~MainWindow nen phai lam SOM, luc handle con mo. `ponytail: tren Windows
// handle con mo nen QFile::remove that bai` — vi vay tra ve so file con lai de
// main() ghi ra log thay vi giong "da xoa sach". Tren Linux/macOS unlink khi con
// mo van duoc nen thuong = 0.
int MainWindow::removeDraftsForExit() {
    if (trExitDraftPolicy() != 1) return 0;   // 0 = chua biet, 2 = phai giu ban nhap
    int left = 0;
    for (auto* t : m_openDocs) {
        if (!t) continue;
        const QString working = (t->doc->filePath() != t->originalPath) ? t->doc->filePath()
                                                                      : QString();
        if (working.isEmpty()) continue;
        if (!removeWorkingCopy(working) && QFile::exists(working)) ++left;
    }
    return left;
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    // SU KIEN 0902 (thay polling 1s) — xem comment installEventFilter trong wiring tab:
    // UpdateRequest = Qt bao "view sap ve lai", den sau khi pan/zoom/resize da ap
    // dung, dung mot lan moi dam repaint. updateHeavyRegion tu gate (khong heavy /
    // vung nhin khong doi thi thoat nanogiay), nen gan mien phi cho moi view khac.
    if (event->type() == QEvent::UpdateRequest) {
        auto* t = currentTab();
        if (t && watched == t->view) updateHeavyRegion(t);
    }
    if (watched == m_findBar->parent() && event->type() == QEvent::Resize && m_findBar->isVisible()) {
        auto* p = qobject_cast<QWidget*>(m_findBar->parent());
        if (p) {
            int bw = m_findBar->sizeHint().width();
            const int tabH = m_docTabs->tabBar()->height();
            const int bh   = m_findBar->sizeHint().height();
            const int y    = qMax(0, (tabH - bh) / 2);
            m_findBar->move(qMax(0, p->width() - bw - 8), y);
        }
    }
    return QMainWindow::eventFilter(watched, event);
}



// ── Right-click context menu on thumbnail ────────────────────────────────────

void MainWindow::showThumbnailContextMenu(int pageIndex, QPoint globalPos) {
    auto* t = currentTab();
    if (!t || !t->doc->isOpen()) return;

    QMenu menu;
    menu.addAction(QString("Page %1 of %2")
                   .arg(pageIndex+1).arg(t->doc->pageCount()))->setEnabled(false);
    menu.addSeparator();

    // Insert pages from another PDF file (Adobe-style, no drag-drop)
    {
        auto doInsert = [this, t](int insertBefore) {
            QString src = QFileDialog::getOpenFileName(
                this, "Insert Pages from PDF", {}, "PDF Files (*.pdf)");
            if (src.isEmpty()) return;
            QString path = t->doc->filePath();
            QString tmp  = makeTmpPath(path);
            QApplication::setOverrideCursor(Qt::WaitCursor);
            bool ok = m_editor->insertPdf(path, insertBefore, src, tmp);
            QApplication::restoreOverrideCursor();
            if (!ok) { QMessageBox::warning(this, "Insert Error", m_editor->lastError()); return; }
            reloadTab(t, path, tmp);
            statusBar()->showMessage("Pages inserted", 3000);
        };
        auto* insMenu = menu.addMenu("Insert Pages from File…");
        insMenu->addAction("Before This Page", this, [doInsert, pageIndex]{ doInsert(pageIndex); });
        insMenu->addAction("After This Page",  this, [doInsert, pageIndex]{ doInsert(pageIndex + 1); });
    }
    menu.addSeparator();

    // Delete this page
    menu.addAction("Delete Page…", this, [this, t, pageIndex]{
        if (t->doc->pageCount() <= 1) {
            QMessageBox::information(this, "Delete Page",
                "Cannot delete the only page in a document.");
            return;
        }
        auto reply = QMessageBox::question(this, "Delete Page",
            QString("Permanently delete page %1 from\n\"%2\"?")
                .arg(pageIndex+1)
                .arg(QFileInfo(t->doc->filePath()).fileName()),
            QMessageBox::Yes | QMessageBox::Cancel);
        if (reply != QMessageBox::Yes) return;

        QString path = t->doc->filePath();
        QString tmp  = makeTmpPath(path);
        if (!m_editor->deletePages(path, {pageIndex}, tmp)) {
            QMessageBox::warning(this, "Error", m_editor->lastError()); return;
        }
        if (t->currentPage >= t->doc->pageCount() - 1)
            t->currentPage = qMax(0, t->currentPage - 1);
            // 🔴🔴 2026-09-01: bao cho bo render biet trang HIEN TAI la trang nao.
            // Thieu buoc nay thi m_currentPage mai la 0, va MOI anh chat luong day du cua
            // trang khac 0 deu bi vut o cua cuoi voi "drop reason=notCurrent" — day la ly do
            // "tao Text o trang 3 khong bao gio hien" (bat duoc trong log cua owner).
            if (t->renderer) t->renderer->setCurrentPage(t->currentPage);
        reloadTab(t, path, tmp);
        statusBar()->showMessage("Page deleted", 3000);
    });

    menu.addSeparator();

    // Extract page
    menu.addAction("Extract to New File…", this, [this, t, pageIndex]{
        QString out = QFileDialog::getSaveFileName(this, "Save Extracted Page", {}, "PDF (*.pdf)");
        if (out.isEmpty()) return;
        if (!m_editor->extractPages(t->doc->filePath(), pageIndex, pageIndex, out))
            QMessageBox::warning(this, "Error", m_editor->lastError());
        else openFile(out);
    });

    // Send to another open tab
    if (m_openDocs.size() > 1) {
        auto* sendMenu = menu.addMenu("Send to Tab →");
        for (int i = 0; i < m_openDocs.size(); ++i) {
            auto* other = m_openDocs[i];
            if (other == t) continue;
            QString name = QFileInfo(other->doc->filePath()).fileName();
            connect(sendMenu->addAction(name), &QAction::triggered, this,
                    [this, t, other, pageIndex]{
                QString path = other->doc->filePath();
                QString tmp  = makeTmpPath(path);
                if (!m_editor->insertPageFrom(path,
                        other->doc->pageCount(), t->doc->filePath(), pageIndex, tmp)) {
                    QMessageBox::warning(this, "Error", m_editor->lastError()); return;
                }
                reloadTab(other, path, tmp);
                statusBar()->showMessage(
                    "Page sent to " + QFileInfo(path).fileName(), 3000);
            });
        }
    }

    menu.exec(globalPos);
}

void MainWindow::onTextRegionSelected(int pageIdx, QRectF rectPts, QPoint globalPos)
{
    auto* t = currentTab();
    if (!t || !t->doc->isOpen()) return;

    // ── OCR 2a: trang khong co chu + chua OCR → OCR trang do truoc, roi ap lai vung chon ──
    if (pageNeedsOcr(t->doc->raw(), pageIdx) && OcrEngine::available(nullptr)) {
        statusBar()->showMessage("Recognizing text…", 0);
        runOcr(t->doc->raw(), pageIdx, pageIdx, t, QStringLiteral("select"));
        // Sau khi OCR xong, ap lai vung chon (goi lai onTextRegionSelected).
        m_pendingSelPage = pageIdx;
        m_pendingSelRect = rectPts;
        m_pendingSelPos  = globalPos;
        return;
    }

    if (!GoogleAuth::checkAndRequest(this)) {
        statusBar()->showMessage("Translation requires consent — select text again and click Enable.", 5000);
        return;
    }

    FPDF_DOCUMENT rawDoc = t->doc->raw();
    m_lastTransPos = globalPos;
    statusBar()->showMessage("Translating…", 3000);

    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this,
            [this, watcher]() {
        watcher->deleteLater();
        QString text = watcher->result();
        if (!text.isEmpty())
            m_translator->translate(text);
        else
            statusBar()->showMessage(
                "No selectable text in this area. "
                "Scanned pages may require OCR.", 4000);
    });
    // 🔴🔴 0928 LƯỢT 22 (reviewer mục 1 — "rawDoc thô chạy tự do"): task dịch vùng
    // chọn FPDF_LoadPage THÔ trên rawDoc, trước đây KHÔNG token, future KHÔNG lưu
    // ở tab ⇒ beginClose/shutdownTab không thấy nó; đóng tab ngay khi dịch chạy
    // ⇒ FPDF_CloseDocument trước khi task lấy khoá ⇒ đọc doc đã free. Nay: token
    // ĐĂNG KÝ LÚC SPAWN (UI thread) + future vào bgSync để đường đóng tab chờ HẾT.
    trdoc::Task task(rawDoc, "translate/region");
    auto fut = QtConcurrent::run([task = std::move(task), rawDoc, pageIdx, rectPts]() -> QString {
        QString text;
        // 0927 LƯỢT 10 (--no-textpage): KHONG mo FPDF_TEXTPAGE de trich chu trong vung.
        if (trNoTextPage()) return text;
        TimedPdfiumLock lock(__FILE__, __LINE__);
        FPDF_PAGE page = FPDF_LoadPage(rawDoc, pageIdx);
        if (page) {
            FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
            if (tp) {
                // QRectF: .top() = smaller PDF y, .bottom() = larger PDF y (y increases upward in PDF).
                // FPDFText_GetBoundedText expects (left, top, right, bottom) where top > bottom.
                int count = FPDFText_GetBoundedText(
                    tp,
                    rectPts.left(), rectPts.bottom(),
                    rectPts.right(), rectPts.top(),
                    nullptr, 0);
                if (count > 0) {
                    std::vector<unsigned short> buf(static_cast<size_t>(count + 1), 0);
                    FPDFText_GetBoundedText(
                        tp,
                        rectPts.left(), rectPts.bottom(),
                        rectPts.right(), rectPts.top(),
                        buf.data(), count + 1);
                    text = QString::fromUtf16(
                        reinterpret_cast<const char16_t*>(buf.data())).trimmed();
                }
                FPDFText_ClosePage(tp);
            }
            FPDF_ClosePage(page);
        }
        return text;
    });
    watcher->setFuture(fut);
    t->addBgWait(fut);   // L22: dịch vùng chọn — đóng tab phải chờ — L22b: có dọn
}

// ── Chon chu theo chi so ky tu (SPEC_TEXTSEL_ADOBE) ──────────────────────────

// View bao: nguoi dung keo chuot/chon chu. Ghi vao DocTab::textSel + day rect.
void MainWindow::onTextSelectionChanged(int anchorPage, int anchorChar,
                                        int focusPage, int focusChar) {
    auto* t = currentTab();
    if (!t || !t->doc || !t->doc->isOpen()) return;
    // Chuan hoa theo thu tu doc: nguoi dung keo nguoc len tren van chon dung.
    if (focusPage < anchorPage
        || (focusPage == anchorPage && focusChar < anchorChar)) {
        qSwap(anchorPage, focusPage);
        qSwap(anchorChar, focusChar);
    }
    if (anchorPage < 0 || anchorChar < 0 || focusPage < 0 || focusChar < 0) {
        clearTextSelection();
        return;
    }
    t->textSel = { anchorPage, anchorChar, focusPage, focusChar, true };
    pushSelectionToViews(t);
}

void MainWindow::onTextSelectionCleared() {
    clearTextSelection();
}

void MainWindow::clearTextSelection() {
    auto* t = currentTab();
    if (t) t->textSel = TextSel();
    if (m_continuousView) m_continuousView->clearSelectionRects();
    if (t && t->view) t->view->clearSelectionRects();
}

void MainWindow::pushSelectionToViews(DocTab* t) {
    if (!t || !t->doc || !t->doc->isOpen() || !t->textSel.active) return;
    const TextSel& s = t->textSel;
    // rect toa do HIEN THI (giong highlight tim kiem: Y-down, da ap /Rotate).
    const QHash<int, QVector<QRectF>> byPage = TextSelection::rangeRectsByPageDisp(
        t->doc->raw(), s.anchorPage, s.anchorChar, s.focusPage, s.focusChar);
    QHash<int, QList<QRectF>> disp;
    for (auto it = byPage.constBegin(); it != byPage.constEnd(); ++it)
        disp.insert(it.key(), QList<QRectF>(it->begin(), it->end()));
    if (m_continuousView)
        m_continuousView->setSelectionRects(disp);
    if (t->view)
        t->view->setSelectionRects(disp.value(t->view->currentPage()));
}

// Chuot phai co vung chon → menu "Copy text" (goi tu ca 2 view).
void MainWindow::onCopySelectionRequested(QPoint globalPos) {
    auto* t = currentTab();
    if (!t || !t->textSel.active) return;
    QMenu menu(this);
    QAction* copyAct = menu.addAction("Copy text");
    copyAct->setShortcut(QKeySequence::Copy);
    connect(copyAct, &QAction::triggered, this, [this]{
        copyTextSelectionToClipboard();
    });
    menu.exec(globalPos);
}

// Ctrl+C / menu "Copy text": chep textForRange cua ca doan (nhieu trang noi
// voi nhau, giua trang them "\n").
void MainWindow::copyTextSelectionToClipboard() {
    auto* t = currentTab();
    if (!t || !t->textSel.active) return;
    const TextSel& s = t->textSel;
    const QString text = TextSelection::rangeText(
        t->doc->raw(), s.anchorPage, s.anchorChar, s.focusPage, s.focusChar);
    if (text.isEmpty()) {
        statusBar()->showMessage("No selectable text in this area.", 4000);
        return;
    }
    QGuiApplication::clipboard()->setText(text);
    statusBar()->showMessage(QString("Copied %1 character(s)").arg(text.size()), 2500);
}

// ── OCR qua thao tac chuot (SPEC_OCR_4) ───────────────────────────────────────

// Dem so ky tu trong trang (FPDFText_CountChars). Tra ve -1 neu loi.
// OcrTextCache co san "trang co chu" thi tra luon, khoi cham PDFium. Neu chua
// biet thi lay trang TU PageCache (KHONG FPDF_LoadPage/ClosePage — PageCache la
// chu so huu duy nhat cua FPDF_PAGE/TEXTPAGE, SPEC_PERF_HEAVYPAGE R1).
bool MainWindow::pageHasTextSync(FPDF_DOCUMENT doc, int pageIndex) {
    if (!doc || pageIndex < 0) return false;
    // Bo dem "trang co chu" da biet thi tra luon — khoi cham PDFium.
    const int cached = OcrTextCache::hasTextStatus(
        reinterpret_cast<OcrTextCache::DocHandle>(doc), pageIndex);
    if (cached != -1) return cached == 1;

    bool has = false;
    {
        TimedPdfiumLock lock(__FILE__, __LINE__);
        FPDF_PAGE page = PageCache::acquire(doc, pageIndex);
        if (page) {
            PageCache::PageBorrow _b(doc, pageIndex);   // RAII — di cap voi acquire
            FPDF_TEXTPAGE tp = PageCache::textPage(doc, pageIndex);
            if (tp) has = (FPDFText_CountChars(tp) > 0);
        }
    }
    OcrTextCache::setHasText(reinterpret_cast<OcrTextCache::DocHandle>(doc),
                             pageIndex, has);
    return has;
}

bool MainWindow::pageNeedsOcr(FPDF_DOCUMENT doc, int pageIndex) {
    return doc && pageIndex >= 0
        && !pageHasTextSync(doc, pageIndex)
        && !OcrTextLayer::pageDone(doc, pageIndex);
}

bool MainWindow::docHasAnyText(FPDF_DOCUMENT doc, int currentPage) {
    if (!doc) return false;
    // 0927 LƯỢT 10 (--no-textpage): mau nay cung mo FPDFText_LoadPage tren TAI LIEU
    // CHINH (3 trang dau + trang hien tai) ⇒ chan de khong bo sót mot noi nao.
    if (trNoTextPage()) return false;
    // Chi kiem MAU (3 trang dau + trang hien tai) de khoi nap toan bo file
    // tren luong giao dien — file nhieu trang (CAD) se treo khi Ctrl+F.
    // Mau du de quyet dinh co hoi OCR hay khong: tai lieu tu khoa co chu o
    // dau do thi it nhat roi vao cac trang dau hoac trang dang xem.
    TimedPdfiumLock lock(__FILE__, __LINE__);
    const int n = FPDF_GetPageCount(doc);
    QVector<int> sample;
    for (int p = 0; p < n && p < 3; ++p) sample.append(p);
    if (currentPage >= 0 && !sample.contains(currentPage)) sample.append(currentPage);
    for (int p : sample) {
        FPDF_PAGE page = FPDF_LoadPage(doc, p);
        if (!page) continue;
        int cnt = 0;
        FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
        if (tp) { cnt = FPDFText_CountChars(tp); FPDFText_ClosePage(tp); }
        FPDF_ClosePage(page);
        if (cnt > 0) return true;
    }
    return false;
}

// ── Dau vet OCR (SPEC_PROBE_LOG_SNAPSHOT muc 2) ─────────────────────────────
namespace {
struct OcrPageTrace {
    int     page  = -1;    // 0-based nhu trong may
    int     words = 0;
    qint64  ms    = 0;
    QString txtPath;
};

// Ghi van ban OCR ra <temp>/torreader-ocr/<ten-file-pdf>-p<N>.txt (N 1-based):
// dong dau la mo ta, sau do van ban xuong dong theo lineIndex, tu cung dong
// cach nhau 1 khoang trang. Day la thu owner mo ra doc de biet OCR doc duoc gi.
// Tra ve duong dan file, rong neu ghi khong duoc.
QString writeOcrTraceFile(const QString& pdfPath, int pageIndex,
                          const QVector<OcrWord>& words, qint64 ms) {
    const QString dir = QDir::tempPath() + QLatin1String("/torreader-ocr");
    if (!QDir().mkpath(dir)) return QString();
    QString base = QFileInfo(pdfPath).fileName();
    if (base.isEmpty()) base = QStringLiteral("untitled");
    base.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    const QString path = QStringLiteral("%1/%2-p%3.txt").arg(dir, base).arg(pageIndex + 1);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) return QString();
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts << QStringLiteral("# %1 | page %2 | %3 words | %4 ms\n")
              .arg(pdfPath).arg(pageIndex + 1).arg(words.size()).arg(ms);
    // Gom theo lineIndex (QMap sap theo khoa = dung thu tu doc). Tu khong co
    // dong (lineIndex < 0) don xuong cuoi de khong mat chu.
    QMap<int, QStringList> byLine;
    QStringList noLine;
    for (const OcrWord& w : words) {
        if (w.lineIndex >= 0) byLine[w.lineIndex] << w.text;
        else                  noLine << w.text;
    }
    for (auto it = byLine.constBegin(); it != byLine.constEnd(); ++it)
        ts << it.value().join(QLatin1Char(' ')) << "\n";
    if (!noLine.isEmpty()) ts << noLine.join(QLatin1Char(' ')) << "\n";
    ts.flush();
    f.close();
    return path;
}
}  // namespace

void MainWindow::showOcrTraceMessage(int pageIndex, int words) {
    if (words > 0) {
        statusBar()->showMessage(QStringLiteral("Recognized %1 words on page %2 — log saved")
                                     .arg(words).arg(pageIndex + 1), 5000);
    } else {
        // Im lang chinh la thu owner dang phan nan — luon chi ro noi de xem.
        const QString dir = QDir::toNativeSeparators(QDir::tempPath()
                                                     + QLatin1String("/torreader-ocr/"));
        statusBar()->showMessage(QStringLiteral("No text recognized on page %1 — see %2")
                                     .arg(pageIndex + 1).arg(dir), 5000);
    }
}

// Chay OCR cho [firstPage..lastPage] o luong nen. Khong block giao dien.
// Chi nhan dang trang CHƯA OCR. xong thi bao cap nhat o gui (trang thai + renderer).
// langs: "vie+eng"/"vie"/"eng" (combo tab OCR). cancelFlag: set flag de huy
// giua chung (nut Cancel cua tab OCR).
void MainWindow::runOcr(FPDF_DOCUMENT doc, int firstPage, int lastPage, DocTab* tab,
                        const QString& sourceTag,
                        const QString& langs,
                        std::shared_ptr<QAtomicInt> cancelFlag) {
    if (!doc || !tab) return;
    QString whyNot;
    if (!OcrEngine::available(&whyNot)) {
        statusBar()->showMessage("OCR unavailable: " + whyNot, 5000);
        return;
    }
    if (lastPage < firstPage) return;

    const int dpi = OcrEngine::kDefaultDpi;

    // Tranh chay chong: mot tab chi mot luot OCR (con trang dang chay thi bo qua).
    if (m_ocrWatcher && m_ocrWatcher->isRunning()) {
        statusBar()->showMessage("Recognition already in progress…", 2500);
        return;
    }

    // Loc chi cac trang thuc su can OCR.
    QVector<int> pages;
    for (int p = firstPage; p <= lastPage; ++p)
        if (pageNeedsOcr(doc, p)) pages.append(p);
    if (pages.isEmpty()) {
        statusBar()->showMessage("This page already has recognized text.", 3000);
        return;
    }

    // Co Cancel cho luot nay (OCR tab trong sidebar set flag, worker kiem giua cac trang).
    m_ocrCancel = cancelFlag ? cancelFlag : std::make_shared<QAtomicInt>(0);
    auto cancel = m_ocrCancel;
    const int totalPages = FPDF_GetPageCount(doc);

    m_ocrSourceTag = sourceTag;
    m_ocrWatcherFirst = pages.first();
    m_ocrWatcherLast  = pages.last();
    if (m_thumbPanel && m_thumbPanel->ocrPanel())
        m_thumbPanel->ocrPanel()->setOcrRunning(true);

    // Dau vet OCR: duong dan pdf lay o luong giao dien, danh sach ket qua chia
    // chung voi lambda ket thuc (worker ghi xong -> finished moi doc, khong dua).
    const QString pdfPath = tab->doc ? tab->doc->filePath() : QString();
    auto traces = QSharedPointer<QVector<OcrPageTrace>>::create();
    QPointer<MainWindow> self(this);

    auto* watcher = new QFutureWatcher<void>(this);
    m_ocrWatcher = watcher;
    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher, tab, pages, traces, s = tab->serial]() {
        watcher->deleteLater();
        if (m_ocrWatcher == watcher) m_ocrWatcher = nullptr;
        m_ocrCancel.reset();
        if (m_thumbPanel && m_thumbPanel->ocrPanel())
            m_thumbPanel->ocrPanel()->setOcrRunning(false);
        // Tab co the da dong giua chung — khong dong cham vao UI cua no.
        if (!tabAlive(tab, s) || !tab->doc || !tab->doc->isOpen()) {
            m_pendingSelPage = -1;
            statusBar()->clearMessage();
            return;
        }
        // Lam moi renderer de thay chu OCR (highlight/search).
        for (int p : pages) {
            if (tab->renderer) tab->renderer->invalidatePage(p);
            // Text page dem cu chua lop OCR → xoa de FPDFText_* thay chu moi.
            TextSelection::closePage(tab->doc->raw(), p);
            // Cache "trang co chu" cu khong con dung (vua chen chu OCR vao trang).
            OcrTextCache::invalidatePage(
                reinterpret_cast<OcrTextCache::DocHandle>(tab->doc->raw()), p);
        }
        if (tab->renderer && tab->view && !m_fastMode)
            tab->renderer->requestPage(tab->currentPage, tab->zoom);
        if (tab->view) tab->view->invalidateTiles();
        if (m_continuousView) m_continuousView->invalidatePage(tab->currentPage);
        // Muc 2: bao ro so tu + noi luu dau vet cua trang OCR cuoi cung.
        if (traces->isEmpty()) statusBar()->clearMessage();
        else showOcrTraceMessage(traces->last().page, traces->last().words);
        // OCR do quet chon → ap lai vung chon cua nguoi dung.
        if (m_ocrSourceTag == QLatin1String("select") && m_pendingSelPage >= 0) {
            const int pg = m_pendingSelPage;
            const QRectF rc = m_pendingSelRect;
            const QPoint gp = m_pendingSelPos;
            m_pendingSelPage = -1;
            onTextRegionSelected(pg, rc, gp);
        }
        // OCR do nguoi dung chu dong bam Recognize (menu) → tu chuyen sang Select
        // de thay ngay chu dung duoc (SPEC_TEXT_UX_SELECT_COPY muc 4). Khong dong
        // cham vao "select" (nhanh do da co m_pendingSelPage).
        if (m_ocrSourceTag == QLatin1String("menu") && tab == currentTab()
            && tab->view && tab->view->tool() == PdfGpuView::ViewTool::Pan) {
            pushToolToViews(PdfGpuView::ViewTool::SelectText, 10);
            if (m_selectTextAct) m_selectTextAct->setChecked(true);
            statusBar()->showMessage(
                "Text is now selectable — drag to select, right-click to copy", 5000);
        }
        // Sau OCR: trang dang xem con can OCR thi nhac MOT DONG o thanh trang thai
        // (SPEC_OCR_TAB phan 1c) — khong con dai nhac "No selectable text".
        if (tab == currentTab())
            notifyOcrStatusForPage(tab->currentPage);
    });
    statusBar()->showMessage(QString("Recognizing text… (%1 page%2)")
                                 .arg(pages.size()).arg(pages.size() == 1 ? QString() : "s"), 0);
    // 🔴 LƯỢT 22: token đăng ký LÚC SPAWN (UI thread) + RAII move — beginClose thấy
    // cả OCR còn xếp hàng. Thân task chỉ chạm `doc` + shared state, không đọc t->.
    trdoc::Task task(doc, "ocr");
    watcher->setFuture(QtConcurrent::run([task = std::move(task), doc, pages, dpi, langs, totalPages, pdfPath,
                                          traces, self, cancel]() {
        for (int p : pages) {
            if (cancel->loadRelaxed() || task.cancelled()) break;   // nhan Cancel / dang dong
            QElapsedTimer timer;
            timer.start();
            const QVector<OcrWord> words =
                OcrEngine::recognizePage(doc, p, langs, dpi,
                                         [cancel] { return cancel->loadRelaxed() != 0; });
            if (cancel->loadRelaxed()) break;   // huy giua trang
            const qint64 ms = timer.elapsed();
            if (!words.isEmpty())
                OcrTextLayer::insertPage(doc, p, words);

            // Muc 2: de lai DAU VET DOC DUOC — file van ban + mot dong log.
            OcrPageTrace tr;
            tr.page    = p;
            tr.words   = int(words.size());
            tr.ms      = ms;
            tr.txtPath = writeOcrTraceFile(pdfPath, p, words, ms);
            traces->append(tr);
            qInfo().noquote() << QStringLiteral("[ocrtrace] page=%1 words=%2 ms=%3 file=%4")
                                     .arg(p + 1).arg(words.size()).arg(ms)
                                     .arg(tr.txtPath.isEmpty()
                                              ? QStringLiteral("(ghi khong duoc)")
                                              : tr.txtPath);
            // Thanh trang thai phai doi ngay sau TUNG trang (khong im lang) +
            // cap nhat panel OCR (tien do + so tu) qua tin hieu.
            QMetaObject::invokeMethod(qApp, [self, p, n = int(words.size()), totalPages] {
                if (!self) return;
                self->showOcrTraceMessage(p, n);
                self->ocrProgress(p + 1, totalPages);
                self->ocrPageFinished(p, n);
            }, Qt::QueuedConnection);
        }
    }));
}

void MainWindow::onOcrPageRequested(int pageIdx) {
    auto* t = currentTab();
    if (!t || !t->doc->isOpen()) return;
    runOcr(t->doc->raw(), pageIdx, pageIdx, t, QStringLiteral("menu"));
}

void MainWindow::onOcrAllRequested() {
    auto* t = currentTab();
    if (!t || !t->doc->isOpen()) return;
    runOcr(t->doc->raw(), 0, t->doc->pageCount() - 1, t, QStringLiteral("menu"));
}

// Nut "Recognize whole document" o tab OCR (SPEC_OCR_TAB phan 1b) — day la hanh
// vi MAC DINH owner yeu cau: quet toan PDF chu khong phai chi 1 trang.
void MainWindow::onOcrWholeFromTab(const QString& langs) {
    auto* t = currentTab();
    if (!t || !t->doc->isOpen()) return;
    runOcr(t->doc->raw(), 0, t->doc->pageCount() - 1, t, QStringLiteral("tab"), langs);
}

void MainWindow::onOcrPageFromTab(const QString& langs) {
    auto* t = currentTab();
    if (!t || !t->doc->isOpen()) return;
    runOcr(t->doc->raw(), t->currentPage, t->currentPage, t, QStringLiteral("tab"), langs);
}

// Thay the dai nhac OCR mot hang (SPEC_OCR_TAB phan 1c): moi khi trang dang xem
// khong co chu, chi hien MOT DONG o thanh trang thai trong 5 giay. Khong chiem
// cho, khong can bam X.
// SPEC_PERF_DESK_ABOUT phan 1: LUOT DOI TRANG KHONG DUOC goi FPDF_LoadPage tren
// luong giao dien (file CAD co trang 2,18 trieu path — FPDF_LoadPage mat giay).
// => Chi doc OcrTextCache; chua co trong cache thi de onOcrNotifyTimeout (250ms
// debounce, chong doi khi lat nhanh) kiem bang QtConcurrent, xong moi cap nhat
// UI. Trang thai "chua biet" IM LANG — khong bao "no selectable text" vo can cu.
void MainWindow::notifyOcrStatusForPage(int pageIndex) {
    auto* t = currentTab();
    if (!t || !t->doc || !t->doc->isOpen()) return;
    if (pageIndex != t->currentPage) return;
    if (!OcrEngine::available(nullptr)) return;
    m_ocrNotifyDoc  = t->doc->raw();
    m_ocrNotifyPage = pageIndex;
    m_ocrNotifyTimer->stop();
    m_ocrNotifyTimer->start();
    // Da co ket qua trong cache thi ap NGAY, khong cho debounce het han.
    if (OcrTextCache::hasTextStatus(
            reinterpret_cast<OcrTextCache::DocHandle>(m_ocrNotifyDoc), pageIndex) >= 0)
        applyOcrStatusNow();
}

void MainWindow::onOcrNotifyTimeout() {
    if (m_ocrNotifyPage < 0 || !m_ocrNotifyDoc) return;
    auto* t = currentTab();
    if (!t || !t->doc || !t->doc->isOpen()) return;
    if (t->doc->raw() != m_ocrNotifyDoc || t->currentPage != m_ocrNotifyPage) return;

    const int st = OcrTextCache::hasTextStatus(
        reinterpret_cast<OcrTextCache::DocHandle>(m_ocrNotifyDoc), m_ocrNotifyPage);
    if (st >= 0) { applyOcrStatusNow(); return; }

    // Chua biet: kiem o luong nen (giu s_pdfiumMutex o do, khong anh huong UI),
    // xong post lai cache + UI. QPointer phong tab/doc bi dong giua chung.
    FPDF_DOCUMENT doc = m_ocrNotifyDoc;
    const int page = m_ocrNotifyPage;
    QPointer<MainWindow> self(this);
    // 🔴 LƯỢT 22: token lúc SPAWN + RAII move (probe mồ côi, future bị bỏ).
    trdoc::Task task(doc, "ocrProbeThanhbar");
    QtConcurrent::run([task = std::move(task), doc, page, self]() {
        const bool hasText = MainWindow::pageHasTextSync(doc, page);
        QMetaObject::invokeMethod(qApp, [self, doc, page, hasText]() {
            if (!self) return;
            OcrTextCache::setHasText(reinterpret_cast<OcrTextCache::DocHandle>(doc),
                                     page, hasText);
            auto* t = self->currentTab();
            if (!t || !t->doc || !t->doc->isOpen()) return;
            if (t->doc->raw() != doc || t->currentPage != page) return;
            self->applyOcrStatusNow();
        }, Qt::QueuedConnection);
    });
}

void MainWindow::applyOcrStatusNow() {
    auto* t = currentTab();
    if (!t || !t->doc || !t->doc->isOpen()) return;
    const int page = t->currentPage;
    const FPDF_DOCUMENT doc = t->doc->raw();
    const int st = OcrTextCache::hasTextStatus(
        reinterpret_cast<OcrTextCache::DocHandle>(doc), page);
    if (st == 0 && !OcrTextLayer::pageDone(doc, page))
        statusBar()->showMessage("This page has no selectable text — see the OCR tab", 5000);
    if (m_thumbPanel && m_thumbPanel->ocrPanel())
        m_thumbPanel->ocrPanel()->refresh();
}

// Day cong cu xuong CA HAI view (PdfGpuView + ContinuousView) + log de nghiem thu
// bang so (SPEC_OCR_TAB phan 2.2 + muc NGHIEM THU 4). sidebarId: id nut cong cu
// sidebar can bat (Pan=0, Select=10), -1 = khong dong bo sidebar.
void MainWindow::pushToolToViews(PdfGpuView::ViewTool tool, int sidebarId) {
    if (auto* t = currentTab()) {
        if (t->view) {
            t->view->setTool(tool);
            qInfo().noquote() << "[tool] set id=" << int(tool) << " view=gpu";
        }
    }
    if (m_continuousView) {
        m_continuousView->setTool(tool);
        qInfo().noquote() << "[tool] set id=" << int(tool) << " view=continuous";
    }
    if (sidebarId >= 0 && m_thumbPanel)
        m_thumbPanel->setActiveToolButton(sidebarId);
    // Log trang thai nut de nghiem thu bang so (SPEC_FIX_PICK_TOOL muc 3):
    // toolbarSelect = trang thai nut "Select" tren toolbar, pickBtn = nut
    // "Pick" (id 0) trong luoi Comments dang sang hay khong.
    qInfo().noquote() << QStringLiteral("[tool] state toolbarSelect=%1 pickBtn=%2")
        .arg(m_selectTextAct && m_selectTextAct->isChecked() ? "on" : "off",
             m_thumbPanel && m_thumbPanel->activeTool() == 0 ? "on" : "off");
}

void MainWindow::maybeAskOcrForSearch(const QString& query, Qt::CaseSensitivity cs) {
    auto* t = currentTab();
    if (!t || !t->doc->isOpen() || query.trimmed().isEmpty()) return;

    // Tai lieu CO chu o it nhat mot trang → tim binh thuong, khong hoi.
    if (docHasAnyText(t->doc->raw(), t->currentPage)) { handleSearchRequest(query, cs); return; }
    // Da hoi roi (dang OCR, da tra loi, hoac engine khong san sang) → khong hoi lai.
    if (m_ocrSearchAsked || !OcrEngine::available(nullptr)) {
        handleSearchRequest(query, cs);
        return;
    }

    const int pages = t->doc->pageCount();
    const int seconds = qMax(1, pages * 2);
    const auto b = QMessageBox::question(
        this, "No text in document",
        QString("This document has no text. Recognize it now?\n(about %1 seconds)")
            .arg(seconds),
        QMessageBox::Yes | QMessageBox::No);
    m_ocrSearchAsked = true;
    if (b != QMessageBox::Yes) { handleSearchRequest(query, cs); return; }

    m_ocrSearchPendingQuery = query;
    m_ocrSearchPendingCs = cs;
    m_ocrSourceTag = QStringLiteral("search");
    FPDF_DOCUMENT raw = t->doc->raw();
    statusBar()->showMessage(QString("Recognizing text… (%1 pages)").arg(pages), 0);

    auto* watcher = new QFutureWatcher<void>(this);
    m_ocrWatcher = watcher;
    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher, tab=t, s = t->serial]() {
        watcher->deleteLater();
        if (m_ocrWatcher == watcher) m_ocrWatcher = nullptr;
        if (!tabAlive(tab, s) || !tab->doc || !tab->doc->isOpen()) {
            m_ocrSearchPendingQuery.clear();
            statusBar()->clearMessage();
            return;
        }
        for (int p = 0; p < tab->doc->pageCount(); ++p) {
            if (tab->renderer) tab->renderer->invalidatePage(p);
            TextSelection::closePage(tab->doc->raw(), p);
        }
        if (tab->renderer && tab->view && !m_fastMode)
            tab->renderer->requestPage(tab->currentPage, tab->zoom);
        if (tab->view) tab->view->invalidateTiles();
        if (m_continuousView) m_continuousView->invalidatePage(tab->currentPage);
        statusBar()->clearMessage();
        // Chay lai tim kiem da hoi.
        const QString q = m_ocrSearchPendingQuery;
        const Qt::CaseSensitivity csq = m_ocrSearchPendingCs;
        m_ocrSearchPendingQuery.clear();
        if (!q.isEmpty() && currentTab() == tab)
            handleSearchRequest(q, csq);
    });
    const int dpi = OcrEngine::kDefaultDpi;
    const QString langs = QStringLiteral("vie+eng");
    // 🔴 LƯỢT 22: token lúc SPAWN + RAII move.
    trdoc::Task task(raw, "ocrChayLai");
    watcher->setFuture(QtConcurrent::run([task = std::move(task), raw, pages, dpi, langs]() {
        for (int p = 0; p < pages; ++p) {
            if (task.cancelled()) break;
            const QVector<OcrWord> words =
                OcrEngine::recognizePage(raw, p, langs, dpi, [] { return false; });
            if (!words.isEmpty())
                OcrTextLayer::insertPage(raw, p, words);
        }
    }));
}

void MainWindow::handleSearchRequest(const QString& query, Qt::CaseSensitivity cs,
                                     bool matchDiacritics) {
    auto* t = currentTab();
    if (!t || !t->doc->isOpen() || query.trimmed().isEmpty()) return;
    t->searchResults.clear();
    t->searchCurrentIdx = -1;
    t->searchQuery = query;
    m_searchTab = t;
    if (m_thumbPanel) m_thumbPanel->clearSearchResults();
    m_textSearch->cancel();
    m_textSearch->search(t->doc.get(), query, cs, matchDiacritics);
}

// Probe-only (--markup-mouse-probe): Annotation testing helpers
DocTab* MainWindow::probeCurrentTab() const {
    return const_cast<DocTab*>(currentTab());
}

QWidget* MainWindow::probeCurrentView() const {
    auto* t = const_cast<DocTab*>(currentTab());
    return t ? t->view : nullptr;
}

void MainWindow::probeSelectAnnotTool(int id) {
    // Select markup tool (0=Pan, 2=Line, 3=Arrow, 4=Rectangle, 5=Ellipse, 6=Cloud, 10=SelectText)
    if (id < 0 || id > 10) return;
    if (id == 10) {
        // SelectText
        auto* t = currentTab();
        if (t && t->view) {
            if (m_selectTextAct) m_selectTextAct->setChecked(true);
            pushToolToViews(PdfGpuView::ViewTool::SelectText, 10);
        }
    } else {
        // Markup tools
        if (m_selectTextAct) m_selectTextAct->setChecked(false);
        pushToolToViews(static_cast<PdfGpuView::ViewTool>(id), id);
    }
}

int MainWindow::probeAnnotCount() const {
    auto* t = currentTab();
    if (!t || !t->annotMgr || !t->doc) return -1;
    QList<AnnotInfo> all = t->annotMgr->loadAll(t->doc->pageCount());
    return all.size();
}

int MainWindow::probeAnnotCountPerPage(int page) const {
    // LƯỢT 40b: đếm annotations trên một trang cụ thể
    auto* t = currentTab();
    if (!t || !t->annotMgr) return -1;
    bool ok = true;
    const auto& list = const_cast<MainWindow*>(this)->annotsForPage(t, page, &ok);
    return list.size();
}

int MainWindow::probeVisualCount(int page) const {
    auto* t = currentTab();
    if (!t) return -1;
    return t->visualsCache.value(page).size();
}
