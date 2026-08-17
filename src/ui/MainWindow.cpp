#include "MainWindow.h"
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
#include "PrintDialog.h"
#include "core/PdfDocument.h"
#include "core/PdfRenderer.h"
#include "core/PdfEditor.h"
#include "core/TextSearch.h"
#include "core/VectorLayer.h"
#include "core/ForeignAnnotLayer.h"
#include "core/PdfCoords.h"
#include "core/PdfLinks.h"
#include "core/PageCache.h"
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
#include <QPushButton>
#include <QCoreApplication>
#include <QFrame>
#include <QTextStream>
#include <QPointer>
#include <QMap>
#include <QSharedPointer>
#include <QScrollBar>
#include <QThread>

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
            const int page = (m_continuousMode && m_continuousView)
                                 ? m_continuousView->currentPage() : t->currentPage;
            const TextSelection::PageInfo info = TextSelection::pageFor(t->doc->raw(), page);
            if (!info.tp) return;
            int total = 0;
            { QMutexLocker lock(&s_pdfiumMutex); total = FPDFText_CountChars(info.tp); }
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
    m_thumbPanel->setMinimumWidth(220);  // 2 tab buttons per row + spacing

    m_docTabs = new QTabWidget;
    m_docTabs->setTabsClosable(true);
    m_docTabs->setMovable(true);
    // documentMode bat khong duoc: Fusion ve them vach trang o vien tab bar
    m_docTabs->setElideMode(Qt::ElideRight);
    m_docTabs->tabBar()->setAutoHide(false);  // always show tab bar, even with a single file
    m_docTabs->addTab(new PdfView(m_docTabs), "Welcome");

    m_continuousView = new ContinuousView;

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
        if (m_continuousMode && m_continuousView) {
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
        if (m_continuousMode && m_continuousView) {
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
        if (m_continuousMode && m_continuousView) {
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
        if (m_continuousMode && m_continuousView)
            m_continuousView->scrollToPage(r.pageIndex);
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
        if (m_continuousMode && m_continuousView)
            m_continuousView->scrollToPage(r.pageIndex);
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
        if (auto* t = currentTab()) {
            t->currentPage = page;
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
        QApplication::setOverrideCursor(Qt::WaitCursor);
        PdfEditor* editor = m_editor.get();
        auto* watcher = new QFutureWatcher<bool>(this);
        connect(watcher, &QFutureWatcher<bool>::finished, this,
                [this, watcher, t, path, tmp]() {
            watcher->deleteLater();
            QApplication::restoreOverrideCursor();
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
                [this, watcher, t, path, tmp]() {
            watcher->deleteLater();
            QApplication::restoreOverrideCursor();
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
        if (auto* t = currentTab()) onAnnotMove(t, page, dx, dy);
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
        bool cap = false, foreign = false;
        QList<AnnotVisual> vis;
        if (t->visualsCache.contains(page)) {
            vis = t->visualsCache.value(page);
        } else {
            vis = t->annotMgr->loadPageVisuals(page, &cap, &foreign);
        }
        m_continuousView->setAnnotVisualsForPage(page, vis);
        qDebug().noquote() << "[cont] annotVisuals pushed page=" << page << "n=" << vis.size();
    });

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
        connect(w, &QFutureWatcher<void>::finished, this, [this, w, t, pg] {
            w->deleteLater();
            if (!m_openDocs.contains(t)) return;
            t->warmingPage = -1;
            if (t->currentPage != pg) return;
            QElapsedTimer _t; _t.start();
            refreshAnnotVisuals(t, pg);
            annotsForPage(t, pg);
            qDebug().noquote() << "[perf] markup WARM done page=" << pg << "ms=" << _t.elapsed();
        });
        AnnotationManager* mgr = t->annotMgr.get();
        FPDF_DOCUMENT d = t->doc ? t->doc->raw() : nullptr;
        w->setFuture(QtConcurrent::run([mgr, d, pg] {
            Q_UNUSED(mgr);
            // Nap trang hien tai vao PageCache (luong nen) — markup overlay duoc am.
            QMutexLocker lk(&s_pdfiumMutex);
            if (d) PageCache::acquire(d, pg);
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
        const int pg = (m_continuousMode && m_continuousView)
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
}

MainWindow::~MainWindow() {
    for (auto* t : m_openDocs) {
        disconnect(t->pageReadyConn);
        disconnect(t->scrollConn);
        if (t->doc) TextSelection::closeDocument(t->doc->raw());
        delete t;
    }
}

// ── Theme ────────────────────────────────────────────────────────────────────

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

    // Continuous scroll mode
    m_continuousAct = tb->addAction("Continuous");
    m_continuousAct->setCheckable(true);
    m_continuousAct->setToolTip("Continuous scroll — all pages in one strip  (Ctrl+Shift+C)");
    m_continuousAct->setShortcut(QKeySequence("Ctrl+Shift+C"));
    connect(m_continuousAct, &QAction::toggled, this, [this](bool on) {
        m_continuousMode = on;
        auto* t = currentTab();
        if (on) {
            m_docTabs->setFixedHeight(m_docTabs->tabBar()->sizeHint().height());
            m_continuousView->show();
            if (t && t->doc->isOpen()) {
                auto pgSz = t->doc->pageSize(t->currentPage);
                double renderScale = PdfRenderer::kFullRenderMaxPx
                    / qMax(qMax(pgSz.width(), pgSz.height()), 1.0);
                qDebug() << "[perf] cont inherit renderScale from single ="
                         << renderScale << "(tabZoom=" << t->zoom << ")";
                m_continuousView->setZoom(renderScale);
                m_continuousView->setDocument(t->doc.get(), t->renderer.get());
                // Push annot visuals for current page to continuous view
                if (t->annotMgr)
                refreshAnnotVisuals(t, t->currentPage);
            }
        } else {
            m_docTabs->setMinimumHeight(0);
            m_docTabs->setMaximumHeight(QWIDGETSIZE_MAX);
            m_continuousView->hide();
            // Sync GPU view state on returning to single mode
            if (t && t->doc->isOpen()) {
                t->view->setZoom(t->zoom);
                t->renderer->cancelPending();
                t->renderer->requestPage(t->currentPage, t->zoom);
            }
        }
    });
    tb->addSeparator();

    // Fit Page — fits page to the smaller of width/height (width-only in Continuous mode)
    tb->addAction("Fit Page", this, [this] {
        auto* t = currentTab();
        if (!t || !t->doc->isOpen()) return;
        auto sz = t->doc->pageSize(t->currentPage);
        if (sz.isEmpty()) return;
        if (m_continuousMode && m_continuousView) {
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
        if (!t->annotMgr->saveDocument()) { onSaveAsFile(); return; }
    }

    // 2. Release our own handles on the original so it can be overwritten
    //    (the open PDFium document + thumbnail pool lock the file on Windows).
    //    Huy ca request link dang bay truoc khi doc bi dong (SPEC_NO_SYNC_PAGELOAD).
    PdfLinks::clearCache();
    TextSelection::closeDocument(t->doc->raw());
    t->doc->close();
    if (t->thumbPool) t->thumbPool->close();
    if (t->renderer) t->renderer->setTileCache(nullptr);

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
            QMessageBox::warning(this, "Save As", "Could not write to:\n" + dest);
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

const QList<AnnotInfo>& MainWindow::annotsForPage(DocTab* t, int page) {
    if (t->annotPageCache.contains(page)) {
        qDebug().noquote() << "[perf] annotsForPage page=" << page
                 << "cache=HIT count=" << t->annotPageCache[page].size();
        return t->annotPageCache[page];
    }
    QElapsedTimer _perf;
    _perf.start();
    auto list = t->annotMgr ? t->annotMgr->loadPage(page) : QList<AnnotInfo>();
    qint64 ms = _perf.elapsed();
    t->annotPageCache[page] = list;
    qDebug().noquote() << "[perf] annotsForPage page=" << page
             << "cache=MISS count=" << list.size() << "ms=" << ms;
    return t->annotPageCache[page];
}

void MainWindow::invalidateAnnotPage(DocTab* t, int page) {
    t->annotPageCache.remove(page);
    t->visualsCache.remove(page);
    t->visualsRev.remove(page);
    t->visualsHasForeign.remove(page);
    t->overlayCapablePage.remove(page);
    if (t->fgnLayer && t->fgnLayer->pageIndex() == page) {
        t->fgnLayer.reset();
        if (t->view) t->view->setForeignAnnotLayer(nullptr);
    }
}

void MainWindow::buildVectorLayer(DocTab* t, int pageIndex, bool force) {
    if (t->vecBuilding.contains(pageIndex)
        || (!force && t->vecLayer && t->vecLayer->pageIndex() == pageIndex))
        return;
    int pg = pageIndex;
    t->vecBuilding.insert(pg);
    auto layer = std::make_shared<VectorLayer>();
    auto* w = new QFutureWatcher<bool>(this);
    connect(w, &QFutureWatcher<bool>::finished, this, [this, w, t, pg, layer]{
        w->deleteLater();
        t->vecBuilding.remove(pg);
        if (!m_openDocs.contains(t)) return;
        if (t->currentPage != pg) return;
        if (w->result()) {
            t->vecLayer = layer;
            t->view->setVectorLayer(layer);
        } else {
            t->vecLayer.reset();
            t->view->setVectorLayer(nullptr);
        }
    });
    FPDF_DOCUMENT d = t->doc->raw();
    w->setFuture(QtConcurrent::run([layer, d, pg]{
        QMutexLocker lk(&s_pdfiumMutex);
        return layer->build(d, pg);
    }));
}

void MainWindow::ensureForeignAnnotLayer(DocTab* t, int pageIndex) {
    if (!t || !t->doc || !t->doc->isOpen() || !m_openDocs.contains(t)) return;
    // 🔴 CHI dung lop nay khi trang THUC SU ve bang lop vector. Nen raster von da co san
    //    chu thich (co FPDF_ANNOT) nen khong can bu. Thieu chot nay => dung lop vo ich,
    //    ton 9 giay giu khoa pdfium tren trang nang (do that 2026-08-09).
    if (t->visualsHasForeign.value(pageIndex, false)
        && baseIsVector(t, pageIndex)
        && !t->fgnBuilding.contains(pageIndex)
        && !(t->fgnLayer && t->fgnLayer->pageIndex() == pageIndex)) {
        int pgF = pageIndex;
        t->fgnBuilding.insert(pgF);
        auto fl = std::make_shared<ForeignAnnotLayer>();
        auto* wf = new QFutureWatcher<bool>(this);
        connect(wf, &QFutureWatcher<bool>::finished, this, [this, wf, t, pgF, fl]{
            wf->deleteLater();
            t->fgnBuilding.remove(pgF);
            if (!m_openDocs.contains(t)) return;
            if (t->currentPage != pgF) return;
            if (wf->result()) {
                t->fgnLayer = fl;
                if (t->view) t->view->setForeignAnnotLayer(fl);
            } else {
                t->fgnLayer.reset();
                if (t->view) t->view->setForeignAnnotLayer(nullptr);
            }
        });
        FPDF_DOCUMENT df = t->doc->raw();
        wf->setFuture(QtConcurrent::run([fl, df, pgF]{
            QMutexLocker lk(&s_pdfiumMutex);
            return fl->build(df, pgF, PdfRenderer::kFullRenderMaxPx);
        }));
    }
}

bool MainWindow::canFastPath(DocTab* t, int page) const {
    return t->overlayCapablePage.value(page, false);
}

bool MainWindow::baseIsVector(DocTab* t, int page) const {
    return t && t->vecLayer && t->vecLayer->isReady()
        && t->vecLayer->pageIndex() == page && t->vecLayer->isComplete();
}

namespace {
struct AnnotVisualsRes {
    QList<AnnotVisual> visuals;
    bool overlayCapable = false;
    bool hasForeign = false;
};
}

void MainWindow::applyAnnotVisuals(DocTab* t, int page,
                                   const QList<AnnotVisual>& visuals,
                                   bool overlayCapable, bool hasForeign) {
    if (!t) return;
    if (!overlayCapable)
        t->pagesNeedGenerate.insert(page);  // fallback path relies on renderer having annots
    if (t->renderer) {
        t->renderer->setPageAnnotRender(page, !overlayCapable || hasForeign);
        t->renderer->setPageAnnotOverlay(page, overlayCapable);
    }
    qDebug().noquote() << "[overlay] page=" << page << "overlayCapable=" << overlayCapable << "hasForeign=" << hasForeign << "visuals=" << visuals.size();
    ensureForeignAnnotLayer(t, page);
    if (overlayCapable) {
        if (t->view) { t->view->setAnnotVisuals(visuals); t->view->clearPendingMarkups(); }
        if (m_continuousMode && m_continuousView)
            m_continuousView->setAnnotVisualsForPage(page, visuals);
    } else {
        if (t->view) t->view->clearAnnotVisuals();
        if (m_continuousMode && m_continuousView)
            m_continuousView->setAnnotVisualsForPage(page, {});
    }
}

void MainWindow::refreshAnnotVisuals(DocTab* t, int page) {
    if (!t || !t->annotMgr || !t->doc || !t->doc->isOpen()) {
        if (t && t->view) t->view->clearAnnotVisuals();
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
    // Trang dang co rescan chay roi thi khong bam them lan nua.
    if (t->visualsScanning.contains(page)) {
        qDebug().noquote() << "[perf] visuals rescan in-flight page=" << page << "— skip duplicate";
        return;
    }
    t->visualsScanning.insert(page);
    QElapsedTimer _rescanTimer; _rescanTimer.start();
    AnnotationManager* mgr = t->annotMgr.get();
    auto res = std::make_shared<AnnotVisualsRes>();
    t->annotVisualsFuture = QtConcurrent::run([mgr, page, res] {
        res->visuals = mgr->loadPageVisuals(page, &res->overlayCapable, &res->hasForeign);
    });
    auto* w = new QFutureWatcher<void>(this);
    w->setFuture(t->annotVisualsFuture);
    connect(w, &QFutureWatcher<void>::finished, this,
            [this, w, t, page, rev, res, _rescanTimer]() mutable {
        w->deleteLater();
        if (!m_openDocs.contains(t)) return;   // tab dong — khong dong vao bo nho da xoa
        t->visualsScanning.remove(page);
        const AnnotVisualsRes r = *res;
        const qint64 rescanMs = _rescanTimer.elapsed();
        qDebug().noquote() << "[perf] visuals RESCAN page=" << page << "ms=" << rescanMs;
        // Ket qua nao cung ghi vao dem (du trang da doi).
        t->visualsCache.insert(page, r.visuals);
        t->visualsRev[page] = rev;
        t->overlayCapablePage[page] = r.overlayCapable;
        t->visualsHasForeign[page]  = r.hasForeign;
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
    const QList<AnnotInfo>& fresh = annotsForPage(t, page);
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

void MainWindow::pushUndo(DocTab* t, const MarkupUndoEntry& e) {
    t->redoStack.clear();
    t->undoStack.append(e);
    while (t->undoStack.size() > 100)
        t->undoStack.removeFirst();
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
    switch (e.kind) {
    case MarkupUndoEntry::AddShape: {
        int ai = t->annotMgr->findAnnotIndexByAnyUid(e.page, e.uid);
        if (ai >= 0) t->annotMgr->removeAnnot(e.page, ai);
        break;
    }
    case MarkupUndoEntry::DeleteShape: {
        t->annotMgr->addSnapshot(e.page, e.snap);
        int nc = t->annotMgr->annotCount(e.page);
        if (nc > 0) t->annotMgr->setAnnotUid(e.page, nc - 1, e.uid);
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
    }
    t->redoStack.append(e);
    m_selPage = -1; m_selIdx = -1;
    if (t->view) t->view->clearSelectedAnnot();
    applyMarkupRefresh(t, e.page, touchedPO);
    if (foreignMove) {
        if (t->renderer) { t->renderer->invalidatePage(e.page); t->renderer->requestPage(e.page, t->zoom); }
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
    switch (e.kind) {
    case MarkupUndoEntry::DeleteShape: {
        int ai = t->annotMgr->findAnnotIndexByAnyUid(e.page, e.uid);
        if (ai >= 0) t->annotMgr->removeAnnot(e.page, ai);
        break;
    }
    case MarkupUndoEntry::AddShape: {
        t->annotMgr->addSnapshot(e.page, e.snap);
        int nc = t->annotMgr->annotCount(e.page);
        if (nc > 0) t->annotMgr->setAnnotUid(e.page, nc - 1, e.uid);
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
    }
    t->undoStack.append(e);
    m_selPage = -1; m_selIdx = -1;
    if (t->view) t->view->clearSelectedAnnot();
    applyMarkupRefresh(t, e.page, touchedPO);
    if (foreignMove) {
        if (t->renderer) { t->renderer->invalidatePage(e.page); t->renderer->requestPage(e.page, t->zoom); }
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
    if (canFastPath(t, page)) { if (t->view) t->view->update(); }
    else if (baseIsVector(t, page)) {
        if (t->renderer) t->renderer->invalidatePage(page);
        if (t->view) t->view->update();
        qDebug() << "[perf] skip full render (nen la vector) page=" << page;
    } else {
        if (t->renderer) { t->renderer->invalidatePage(page); t->renderer->requestPage(page, t->zoom); }
        if (t->view) { t->view->invalidateTiles(); t->view->invalidateSharp(); }
    }
    // Undo/redo: chi refresh comments neu bang dang hien
    if (touchedPageObjects || (m_thumbPanel && m_thumbPanel->isCommentsTabVisible()))
        refreshCommentsForPage(t, page);
    if (touchedPageObjects && baseIsVector(t, page) && t->annotMgr && t->doc) {
        {
            QMutexLocker lk(&s_pdfiumMutex);
            FPDF_PAGE pg = PageCache::acquire(t->doc->raw(), page);
            if (pg) t->vecLayer->rebuildNoteTiles(t->doc->raw(), pg);
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
                                         const QString& uid, const QString& type) {
    if (t->view) {
        t->view->setSelectedAnnot(rectPdf);
        if (type == QLatin1String("FreeText")) {
            t->view->setDragNote(rectPdf.normalized());
        } else {
            t->view->setDragTarget(uid, QString(), 0.0f, QColor());
        }
    }
    if (m_continuousView) {
        m_continuousView->setSelectedAnnot(page, rectPdf);
        if (type == QLatin1String("FreeText"))
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
    if (!t->annotMgr) return;
    const auto& list = annotsForPage(t, page);
    m_selPage = -1; m_selIdx = -1;
    for (int i = list.size() - 1; i >= 0; --i) {
        if (list[i].type == QLatin1String("Widget")) continue;
        QRectF hitRect = list[i].rect.normalized().adjusted(-3, -3, 3, 3);
        if (hitRect.contains(pt)) {
            m_selPage = page; m_selIdx = i;
            qDebug().noquote() << "[markup] picked annot page=" << page << "idx=" << i << "type=" << list[i].type;
            setMarkupSelectionViews(t, page, list[i].rect.normalized(),
                                    list[i].uid, list[i].type);
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
    QElapsedTimer _perfRC;
    _perfRC.start();
    int idx = -1;
    QString ctxType, ctxUid;
    QRectF ctxRect;
    {
        const auto& list = annotsForPage(t, page);
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
        ctxRect = list[idx].rect.normalized();
    }
    m_selPage = page; m_selIdx = idx;
    setMarkupSelectionViews(t, page, ctxRect, ctxUid, ctxType);
    m_thumbPanel->selectCommentFor(page, idx);
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
                    if (t->renderer) { t->renderer->invalidatePage(page); t->renderer->requestPage(page, t->zoom); }
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
                if (canFastPath(t, page)) {
                    if (t->view) t->view->update();
                } else if (baseIsVector(t, page)) {
                    if (t->renderer) t->renderer->invalidatePage(page);
                    if (t->view) t->view->update();
                } else {
                    if (t->renderer) { t->renderer->invalidatePage(page); t->renderer->requestPage(page, t->zoom); }
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
        QMutexLocker lock(&s_pdfiumMutex);
        qDebug().noquote() << "[perf] MOVE step pdfiumMutex lock ms=" << _stepT.elapsed();
        _stepT.restart();
        FPDF_PAGE mp = PageCache::acquire(t->doc->raw(), page);
        qDebug().noquote() << "[perf] MOVE step acquireSharedPage ms=" << _stepT.elapsed();
        if (mp) {
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
    {
        MarkupUndoEntry ue; ue.kind = MarkupUndoEntry::MoveAnnot; ue.page = page;
        ue.uid = moveUid;
        if (isForeign && ue.uid.isEmpty()) {
            _stepT.restart();
            ue.uid = t->annotMgr->ensureExternalUid(page, realMoveIdx);
            qDebug().noquote() << "[perf] MOVE step ensureExternalUid ms=" << _stepT.elapsed();
        }
        ue.dxU = dxU; ue.dyU = dyU;
        if (!ue.uid.isEmpty()) {
            _stepT.restart();
            pushUndo(t, ue);
            qDebug().noquote() << "[perf] MOVE step pushUndo ms=" << _stepT.elapsed();
        }
        else qDebug() << "[undo] move KHONG ghi duoc: annot khong co uid page=" << page;
    }
    qDebug().noquote() << "[perf] MOVE pre-phase ms=" << _totalT.elapsed();
    bool ok = t->annotMgr->moveAnnot(page, realMoveIdx, dxU, dyU);
    if (!ok) {
        qDebug().noquote() << "[markup] move THAT BAI (moveAnnot tra false)";
        statusBar()->showMessage("Chú thích này của phần mềm khác — không di chuyển được mà không làm hỏng nó", 4000);
        qDebug().noquote() << "[perf] MOVE handler total ms=" << _totalT.elapsed();
        return;
    }
    t->annotPageCache.remove(page);
    t->visualsCache.remove(page);
    t->visualsRev.remove(page);
    if (baseIsVector(t, page) && t->vecLayer && t->annotMgr && t->doc) {
        QMutexLocker lk(&s_pdfiumMutex);
        FPDF_PAGE pg = PageCache::acquire(t->doc->raw(), page);
        if (pg) t->vecLayer->rebuildNoteTiles(t->doc->raw(), pg);
    }
    if (t->view) t->view->invalidateTileTextures();
    refreshAnnotVisuals(t, page);
    int newIdx = moveUid.isEmpty() ? -1 : t->annotMgr->findAnnotIndexByAnyUid(page, moveUid);
    if (newIdx >= 0) {
        const auto& nl = annotsForPage(t, page);
        if (newIdx < nl.size())
            setMarkupSelectionViews(t, page, nl[newIdx].rect.normalized(),
                                    nl[newIdx].uid, nl[newIdx].type);
    }
    if (t->view) t->view->update();
    if (isPageObjNote) {
        qDebug().noquote() << "[perf] note icon -> rebuild vector layer page=" << page;
        buildVectorLayer(t, page, true);
    }
    qDebug().noquote() << "[perf] MOVE handler total ms=" << _totalT.elapsed();
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
        // Foreign — cannot undo
        if (!t->annotMgr->removeAnnot(page, realIdx)) return;
        statusBar()->showMessage("Đã xoá — thao tác này không hoàn tác được", 4000);
    }
    invalidateAnnotPage(t, page);
    t->dirty = true; updateTabDirty(t);
    if (t->view) t->view->clearSelectedAnnot();
    m_selPage = -1; m_selIdx = -1;
    refreshAnnotVisuals(t, page);
    t->pagesNeedGenerate.insert(page);
    bool isPageObjectAnnot = (annotType == QLatin1String("FreeText") ||
                              annotType == QLatin1String("Note"));
    if (canFastPath(t, page) && !isPageObjectAnnot) {
        if (t->view) t->view->update();
    } else if (baseIsVector(t, page)) {
        if (t->renderer) t->renderer->invalidatePage(page);
        if (t->view) t->view->update();
    } else {
        if (t->renderer) { t->renderer->invalidatePage(page); t->renderer->requestPage(page, t->zoom); }
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
    NoteInputDialog dlg(annotText, this, /*singleLine=*/(annotType == QLatin1String("FreeText")));
    dlg.setWindowTitle("Edit text");
    if (dlg.exec() != QDialog::Accepted) return;
    QString newText = dlg.text();
    QString oldText = annotText;
    if (!t->annotMgr->retextNote(page, realIdx, newText)) {
        statusBar()->showMessage(
            "Chú thích này của phần mềm khác — sửa sẽ làm mất định dạng gốc nên đã bỏ qua", 5000);
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
        if (t->renderer) { t->renderer->invalidatePage(page); t->renderer->requestPage(page, t->zoom); }
        if (t->view) t->view->invalidateTiles();
        if (t->view) t->view->invalidateSharp();
    }
    refreshCommentsForPage(t, page);
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

    if (m_continuousMode)
        m_continuousAct->setChecked(false);

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
            if (t->renderer) { t->renderer->invalidatePage(page); t->renderer->requestPage(page, t->zoom); }
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
    if (t->renderer) { t->renderer->invalidatePage(t->currentPage); t->renderer->requestPage(t->currentPage, t->zoom); }
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
        if (t->renderer) { t->renderer->invalidatePage(m_pendPage); t->renderer->requestPage(t->currentPage, t->zoom); }
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

// Reload after in-place file modification (delete page, reorder, etc.)
// Reopen a tab's viewer/renderer/thumbnails on `path` (no file swap).
// Shared by edit (working copy) and Save/Save As (original). The thumbnail pool
// keeps its OWN FPDF_LoadDocument handles that LOCK the file on Windows, so it is
// closed before and reopened after — otherwise a later overwrite would fail.
void MainWindow::loadTabFile(DocTab* t, const QString& path, bool structureChanged) {
    // Doc sap dong/mo lai: cache link theo trang cu phai xoa (SPEC_PDF_LINKS).
    PdfLinks::clearCache();
    TextSelection::closeDocument(t->doc->raw());
    t->doc->close();
    t->renderer->setTileCache(nullptr);
    if (t->thumbPool) t->thumbPool->close();

    t->doc->open(path);
    t->annotCacheValid = false;
    t->annotPageCache.clear();
    t->overlayCapablePage.clear();
    t->visualsCache.clear();
    t->visualsRev.clear();
    t->visualsHasForeign.clear();
    t->pagesNeedGenerate.clear();
    t->vecLayer.reset();
    t->vecBuilding.clear();
    t->renderer->setDocument(t->doc.get());
    t->annotMgr->setDocument(t->doc->raw(), path);
    if (t->annotLayer) {
        t->annotLayer->setDocument(t->doc->raw());
        t->annotLayer->setAnnotationManager(t->annotMgr.get());
    }

    if (t->thumbPool && !t->thumbPool->open(path))
        t->thumbPool.reset(); // fallback: thumbnail panel uses PdfRenderer

    t->tileCache = std::make_shared<TileCacheFile>();
    {
        uint64_t hash = TileCacheFile::hashFile(path);
        uint64_t sz   = static_cast<uint64_t>(QFileInfo(path).size());
        if (t->tileCache->open(path, hash, sz, t->doc->pageCount()))
            t->renderer->setTileCache(t->tileCache);
    }

    refreshAnnotVisuals(t, t->currentPage);
    t->renderer->requestPage(t->currentPage, t->zoom);

    // Prefetch trang lien ke sau khi mo/mo lai (SPEC_PAGECACHE_CORE muc 4).
    schedulePagePrefetch();

    // ── Vector overlay: rebuild for current page after reload ──
    {
        int pg = t->currentPage;
        t->vecBuilding.insert(pg);
        auto layer = std::make_shared<VectorLayer>();
        auto* vw = new QFutureWatcher<bool>(this);
        connect(vw, &QFutureWatcher<bool>::finished, this, [this, vw, t, pg, layer]{
            vw->deleteLater();
            t->vecBuilding.remove(pg);
            if (!m_openDocs.contains(t)) return;
            if (t->currentPage != pg) return;
            if (vw->result()) {
                t->vecLayer = layer;
                t->view->setVectorLayer(layer);
            } else {
                t->vecLayer.reset();
                t->view->setVectorLayer(nullptr);
            }
        });
        FPDF_DOCUMENT d = t->doc->raw();
        vw->setFuture(QtConcurrent::run([layer, d, pg]{
            QMutexLocker lk(&s_pdfiumMutex);
            return layer->build(d, pg);
        }));
    }
    if (t == currentTab()) {
        // File vừa được mở lại (edit/save) trên cùng con trỏ doc → ép sidebar
        // dựng lại thumbnail + bookmark, nếu không panel giữ nội dung cũ.
        syncSidebarToTab(m_openDocs.indexOf(t), /*forceRebuild=*/structureChanged);
        if (m_thumbPanel && m_thumbPanel->isCommentsTabVisible())
            onCommentsRequested();
        if (m_continuousMode && m_continuousView) {
            auto pgSz = t->doc->pageSize(t->currentPage);
            double renderScale = PdfRenderer::kFullRenderMaxPx
                / qMax(qMax(pgSz.width(), pgSz.height()), 1.0);
            qDebug() << "[perf] cont inherit renderScale from single ="
                     << renderScale << "(tabZoom=" << t->zoom << ")";
            m_continuousView->setZoom(renderScale);
            m_continuousView->setDocument(t->doc.get(), t->renderer.get());
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
    tab->renderer = std::make_unique<PdfRenderer>(this);
    tab->annotMgr = std::make_unique<AnnotationManager>(this);
    tab->view     = new PdfGpuView(m_docTabs);
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
    });

    // ── Tile wiring ───────────────────────────────────────────────────────────
    connect(tab->view, &PdfGpuView::tilesNeeded,
            tab->renderer.get(), &PdfRenderer::requestRegion);
    connect(tab->renderer.get(), &PdfRenderer::regionReady,
            tab->view, [tab](int page, double scale, QRect regionPx, QImage img) {
        if (tab->view) tab->view->setRegion(page, scale, regionPx, img);
    });

    // ── Lop annot phan mem khac: dung vung sac net theo zoom ──
    connect(tab->view, &PdfGpuView::tilesNeeded, this,
            [this, tab](int page, double scale, QRect regionPx) {
        if (!m_openDocs.contains(tab) || !tab->doc || !tab->doc->isOpen()) return;
        if (!tab->visualsHasForeign.value(page, false)) return;
        if (!baseIsVector(tab, page)) return;
        if (!(tab->fgnLayer && tab->fgnLayer->pageIndex() == page)) return;
        if (tab->fgnRegionBuilding) return;
        tab->fgnRegionBuilding = true;
        auto fl = tab->fgnLayer;
        FPDF_DOCUMENT d = tab->doc->raw();
        auto* wr = new QFutureWatcher<bool>(this);
        connect(wr, &QFutureWatcher<bool>::finished, this,
                [this, wr, tab, fl, page, scale, regionPx]{
            wr->deleteLater();
            tab->fgnRegionBuilding = false;
            if (!m_openDocs.contains(tab)) return;
            if (wr->result() && tab->view)
                tab->view->setForeignAnnotRegion(page, scale, regionPx, fl->regionImage());
        });
        wr->setFuture(QtConcurrent::run([fl, d, page, scale, regionPx]{
            QMutexLocker lk(&s_pdfiumMutex);
            return fl->buildRegion(d, page, scale, regionPx);
        }));
    });

    // ── Annotation signals ────────────────────────────────────────────────────
    connect(tab->annotMgr.get(), &AnnotationManager::pageContentChanged, this,
            [this, tab](int page) {
        if (!m_openDocs.contains(tab)) return;
        refreshAnnotVisuals(tab, page);
        if (m_thumbPanel && m_thumbPanel->isCommentsTabVisible()) refreshCommentsForPage(tab, page);
        if (baseIsVector(tab, page) && tab->vecLayer && tab->annotMgr && tab->doc) {
            {
                QMutexLocker lk(&s_pdfiumMutex);
                FPDF_PAGE pg = PageCache::acquire(tab->doc->raw(), page);
                if (pg) tab->vecLayer->rebuildNoteTiles(tab->doc->raw(), pg);
            }
            if (tab->view) tab->view->invalidateTileTextures();
        }
        if (tab == currentTab() && tab->view) tab->view->update();
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
            int ai = ue.uid.isEmpty() ? -1 : tab->annotMgr->findAnnotIndexByUid(pageIdx, ue.uid);
            if (ai >= 0) ue.snap = tab->annotMgr->snapshotAnnot(pageIdx, ai);
            if (!ue.uid.isEmpty() && ue.snap.valid) pushUndo(tab, ue);
        }
        invalidateAnnotPage(tab, pageIdx);
        refreshAnnotVisuals(tab, pageIdx);
        { static const bool dump = qEnvironmentVariableIsSet("TORREADER_DUMP");
          if (dump) {
              QMutexLocker lock(&s_pdfiumMutex);
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
            if (tab->view) tab->view->addPendingMarkup(tool, style, start, end);
            qDebug().noquote() << "[perf] markup SLOW path (overlay incapable) page=" << pageIdx;
            if (tab->renderer) {
                if (tab->view) tab->view->invalidateTiles();
                if (tab->view) tab->view->invalidateSharp();
                tab->renderer->invalidatePage(pageIdx);
                tab->renderer->requestPage(pageIdx, tab->zoom);
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
            int ai = ue.uid.isEmpty() ? -1 : tab->annotMgr->findAnnotIndexByUid(pageIdx, ue.uid);
            if (ai >= 0) ue.snap = tab->annotMgr->snapshotAnnot(pageIdx, ai);
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
            if (tab->view) tab->view->addPendingMarkup(AnnotTool::Freehand, style, QPointF(), QPointF(), pts);
            qDebug().noquote() << "[perf] markup SLOW path (overlay incapable) page=" << pageIdx;
            if (tab->renderer) {
                if (tab->view) tab->view->invalidateTiles();
                if (tab->view) tab->view->invalidateSharp();
                tab->renderer->invalidatePage(pageIdx);
                tab->renderer->requestPage(pageIdx, tab->zoom);
            }
        }
        refreshCommentsForPage(tab, pageIdx);
    });

    connect(tab->view, &PdfGpuView::noteRequested,
            this, [this, tab](int pageIndex, QPointF pdfPoint) {
        if (!tab) return;
        qDebug().noquote() << "[markup] signal=noteRequested page=" << pageIndex
                 << "point=(" << pdfPoint.x() << "," << pdfPoint.y() << ")";
        NoteInputDialog dlg({}, this);
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
        buildVectorLayer(tab, pageIndex, true);
        qDebug().noquote() << "[perf] note icon -> rebuild vector layer page=" << pageIndex;
        tab->dirty = true;
        updateTabDirty(tab);
        invalidateAnnotPage(tab, pageIndex);
        tab->pagesNeedGenerate.insert(pageIndex);
        refreshAnnotVisuals(tab, pageIndex);
        // Note uses page objects — must re-render (slow path always)
        if (baseIsVector(tab, pageIndex)) {
            if (tab->renderer) tab->renderer->invalidatePage(pageIndex);
            if (tab->view) tab->view->update();
        } else {
            qDebug().noquote() << "[markup] request re-render page=" << pageIndex << "zoom=" << tab->zoom;
            if (tab->renderer) {
                if (tab->view) tab->view->invalidateTiles();
                if (tab->view) tab->view->invalidateSharp();
                tab->renderer->invalidatePage(pageIndex);
                tab->renderer->requestPage(pageIndex, tab->zoom);
            }
        }
        refreshCommentsForPage(tab, pageIndex);
    });

    connect(tab->view, &PdfGpuView::textBoxRequested, this,
            [this, tab](int page, QRectF rectPdf) {
        qDebug().noquote() << "[markup] signal=textBoxRequested page=" << page
                 << "rect=(" << rectPdf.x() << "," << rectPdf.y() << ","
                 << rectPdf.width() << "," << rectPdf.height() << ")";
        NoteInputDialog dlg({}, this, /*singleLine=*/true);
        dlg.setWindowTitle("Add text");
        if (dlg.exec() != QDialog::Accepted) return;
        QString txt = dlg.text();
        double w = qMax(24.0, static_cast<double>(txt.length()) * 5.5 + 6.0);
        QRectF r(rectPdf.topLeft(), QSizeF(w, 18.0));
        tab->annotMgr->createInlineNote(page, r, txt, dlg.author(), false, m_annotStyle.strokeColor, m_annotStyle.fontSize);
        {
            MarkupUndoEntry ue; ue.kind = MarkupUndoEntry::AddNote; ue.page = page;
            ue.uid = tab->annotMgr->lastCreatedUid();
            ue.noteRect = r;
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
        invalidateAnnotPage(tab, page);
        tab->pagesNeedGenerate.insert(page);
        refreshAnnotVisuals(tab, page);
        // FreeText uses page objects — must re-render (slow path always)
        if (baseIsVector(tab, page)) {
            if (tab->renderer) tab->renderer->invalidatePage(page);
            if (tab->view) tab->view->update();
        } else {
            qDebug().noquote() << "[markup] request re-render page=" << page << "zoom=" << tab->zoom;
            if (tab->renderer) {
                if (tab->view) tab->view->invalidateTiles();
                if (tab->view) tab->view->invalidateSharp();
                tab->renderer->invalidatePage(page);
                tab->renderer->requestPage(page, tab->zoom);
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

    // ── Load PDF in background thread ─────────────────────────────────────────
    PdfDocument* docPtr = tab->doc.get();
    auto* watcher = new QFutureWatcher<bool>(this);

    connect(watcher, &QFutureWatcher<bool>::finished, this,
            [this, watcher, tab, path, name]() mutable {
        watcher->deleteLater();
        int tabIdx = m_openDocs.indexOf(tab);
        if (tabIdx < 0) return; // closed during load

        if (!watcher->result()) {
            // Failed — remove tab
            disconnect(tab->scrollConn);
            m_docTabs->removeTab(m_docTabs->indexOf(tab->view));
            m_openDocs.removeAt(tabIdx);
            delete tab->view;
            delete tab;
            statusBar()->showMessage("Failed to open: " + name, 4000);
            if (m_openDocs.isEmpty())
                m_docTabs->addTab(new PdfView(m_docTabs), "Welcome");
            return;
        }

        // Success — finish setup on main thread
        tab->renderer->setDocument(tab->doc.get());
        tab->annotMgr->setDocument(tab->doc->raw(), path);
        tab->annotLayer = std::make_unique<AnnotationLayer>(this);
        tab->annotLayer->setDocument(tab->doc->raw());
        tab->annotLayer->setAnnotationManager(tab->annotMgr.get());

        // Open persistent tile cache + thumbnail pool in background
        tab->tileCache = std::make_shared<TileCacheFile>();

        {
            struct InitResult { uint64_t hash; uint64_t size; ThumbnailRenderPool* pool; };
            auto* initWatcher = new QFutureWatcher<InitResult>(this);
            connect(initWatcher, &QFutureWatcher<InitResult>::finished, this,
                    [initWatcher, tab, path, this]() {
                initWatcher->deleteLater();
                if (m_openDocs.indexOf(tab) < 0) return; // tab closed during init
                auto r = initWatcher->result();
                if (tab->tileCache->open(path, r.hash, r.size, tab->doc->pageCount()))
                    tab->renderer->setTileCache(tab->tileCache);
                if (r.pool) {
                    tab->thumbPool.reset(r.pool);
                } else {
                    tab->thumbPool.reset();
                }
                if (tab == currentTab()) {
                    m_thumbPanel->setDocument(tab->doc.get(), tab->renderer.get(),
                                              tab->thumbPool.get(), false);
                    // OcrPanel can biet trang dang xem (khong thi Status giu "No document open").
                    m_thumbPanel->setCurrentPage(tab->currentPage);
                }
            });
            initWatcher->setFuture(QtConcurrent::run([path]() -> InitResult {
                InitResult r{};
                r.hash = TileCacheFile::hashFile(path);
                r.size = static_cast<uint64_t>(QFileInfo(path).size());
                auto* pool = new ThumbnailRenderPool();
                if (!pool->open(path)) {
                    pool->close();
                    delete pool;
                } else {
                    r.pool = pool;
                }
                return r;
    }));
        }

        connect(tab->renderer.get(), &PdfRenderer::pagePartial,
                this, [this, tab](int idx, double sc, QImage img) {
            if (img.isNull()) return;
            if (idx != tab->currentPage) return;
            tab->view->showPartial(idx, sc, img);
        });

        tab->pageReadyConn = connect(
            tab->renderer.get(), &PdfRenderer::pageReady,
            this, [this, tab](int idx, QImage img) {
                if (img.isNull()) return;
                if (idx != tab->currentPage) {
                    qDebug() << "[Main] pageReady stale: got" << idx << "but current=" << tab->currentPage;
                    return;
                }
                qDebug() << "[Main] pageReady idx=" << idx
                         << "imgSize=" << img.size()
                         << "hasImage=" << tab->view->hasImage();
                tab->view->setPage(idx, img, tab->doc->pageSize(idx));
                tab->view->setPageBoxOrigin(tab->doc->pageBoxOriginCached(idx));
                // Link noi bo dang cho center (trang chua cache, render xong moi
                // reset panOffset) — center lai cho dung vi tri (SPEC_PDF_LINKS).
                if (tab->pendingLinkCenterActive
                    && tab->pendingLinkCenterPage == idx)
                    applyPendingLinkCenter(tab);
                refreshAnnotVisuals(tab, tab->currentPage);
                if (!tab->searchResults.isEmpty())
                    applySearchHighlights(tab->searchResults, tab->searchCurrentIdx);
                statusBar()->showMessage(
                    QString("Page %1 / %2").arg(idx + 1).arg(tab->doc->pageCount()), 5000);
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
        tab->renderer->requestPage(0, tab->zoom);
        notifyOcrStatusForPage(0);   // 1 dong o thanh trang thai (SPEC_OCR_TAB phan 1c)
        schedulePagePrefetch();       // prefetch trang lien ke (SPEC_PAGECACHE_CORE muc 4)

        // ── Vector overlay: kick off build for page 0 immediately ──
        {
            constexpr int kVecPage0 = 0;
            tab->vecBuilding.insert(kVecPage0);
            auto layer = std::make_shared<VectorLayer>();
            auto* vw = new QFutureWatcher<bool>(this);
            connect(vw, &QFutureWatcher<bool>::finished, this, [this, vw, tab, layer]{
                vw->deleteLater();
                tab->vecBuilding.remove(0);
                if (!m_openDocs.contains(tab)) return;
                if (tab->currentPage != 0) return;
                if (vw->result()) {
                    tab->vecLayer = layer;
                    tab->view->setVectorLayer(layer);
                } else {
                    tab->vecLayer.reset();
                    tab->view->setVectorLayer(nullptr);
                }
            });
            FPDF_DOCUMENT d = tab->doc->raw();
            vw->setFuture(QtConcurrent::run([layer, d]{
                QMutexLocker lk(&s_pdfiumMutex);
                return layer->build(d, 0);
            }));
        }

        if (tab == currentTab()) {
            syncSidebarToTab(tabIdx);
            if (m_thumbPanel && m_thumbPanel->isCommentsTabVisible())
                onCommentsRequested();
            if (m_continuousMode && m_continuousView) {
                auto pgSz = tab->doc->pageSize(0);
                double renderScale = PdfRenderer::kFullRenderMaxPx
                    / qMax(qMax(pgSz.width(), pgSz.height()), 1.0);
                qDebug() << "[perf] cont inherit renderScale from single ="
                         << renderScale << "(tabZoom=" << tab->zoom << ")";
                m_continuousView->setZoom(renderScale);
                m_continuousView->setDocument(tab->doc.get(), tab->renderer.get());
            }
        }
    });

    watcher->setFuture(QtConcurrent::run([docPtr, path]() -> bool {
        return docPtr->open(path);
    }));
}

// ── Probe-only: lai che do xem tu dong lenh (dung cho --viewprobe) ────────────

void MainWindow::probeSetView(bool continuous, double zoomPercent, int page1Based,
                              double centerXpt, double centerYpt) {
    auto* t = currentTab();
    if (!t || !t->doc || !t->doc->isOpen()) return;

    // Che do xem lien tuc — chi phat toggled khi khac trang thai hien tai
    if (m_continuousAct && m_continuousAct->isChecked() != continuous) {
        m_continuousAct->setChecked(continuous);
        QCoreApplication::processEvents();
    }

    // Zoom: di thang vao luong onZoomChanged (cung nhu o Zoom Edit nhan Enter)
    onZoomChanged(zoomPercent / 100.0);
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
        QMutexLocker lock(&s_pdfiumMutex);
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
        QElapsedTimer timer; timer.start();
        onPageChanged(target);
        if (!fired) {
            QTimer::singleShot(timeoutMs, &loop, [&] { timedOut = true; loop.quit(); });
            loop.exec();
        }
        disconnect(conn);
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
    if (m_continuousAct && m_continuousAct->isChecked() != continuous) {
        m_continuousAct->setChecked(continuous);
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
    if (m_continuousAct && m_continuousAct->isChecked() != continuous) {
        m_continuousAct->setChecked(continuous);
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
            m_continuousView->scrollToPage(pg);
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
             << (m_continuousMode ? "continuous" : "single")
             << "page=" << t->currentPage << "rects=" << results.size() << "currentIdx=" << currentIdx;
    if (m_continuousMode && m_continuousView) {
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
    if (t) {
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
        if (t->vecLayer && t->vecLayer->pageIndex() == t->currentPage)
            t->view->setVectorLayer(t->vecLayer);
        else
            t->view->setVectorLayer(nullptr);
        if (m_continuousMode && m_continuousView && t->doc->isOpen()) {
            auto pgSz = t->doc->pageSize(t->currentPage);
            double renderScale = PdfRenderer::kFullRenderMaxPx
                / qMax(qMax(pgSz.width(), pgSz.height()), 1.0);
            qDebug() << "[perf] cont inherit renderScale from single ="
                     << renderScale << "(tabZoom=" << t->zoom << ")";
            m_continuousView->setZoom(renderScale);
            m_continuousView->setDocument(t->doc.get(), t->renderer.get());
            if (t->annotMgr) refreshAnnotVisuals(t, t->currentPage);
        }
    } else {
        syncSidebarToTab(-1);
        setWindowTitle("TorReader PDF");
        statusBar()->showMessage("TorReader PDF  ·  Open a PDF to get started");
        if (m_continuousMode && m_continuousView)
            m_continuousView->clearDocument();
    }
    updateUndoActions();
}

void MainWindow::onCommentsRequested() {
    auto* t = currentTab();
    if (!t || !t->annotMgr || !t->doc || !t->doc->isOpen()) return;
    if (t->annotCacheValid) {
        m_thumbPanel->setComments(t->annotCache);
        return;
    }
    if (t->annotScanInFlight) {
        qDebug().noquote() << "[comments] FULL scan skipped (already in flight)";
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
            [this, t, throttleTimer](int pageIndex, QList<AnnotInfo> annots) {
        if (m_openDocs.indexOf(t) < 0) return;
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
            [this, t](int scanned, int total) {
        if (m_openDocs.indexOf(t) < 0) return;
        if (t == currentTab() && m_thumbPanel)
            m_thumbPanel->setCommentsProgress(scanned, total);
    });

    // Timer fires: push accumulated results to panel
    connect(throttleTimer, &QTimer::timeout, this, [this, t]() {
        if (m_openDocs.indexOf(t) < 0) return;
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
        t->annotCacheValid = true;
        t->annotScanInFlight = false;

        qDebug().noquote() << "[comments] scan DONE cacheSize=" << t->annotCache.size()
                 << "valid=" << t->annotCacheValid;

        if (t == currentTab()) {
            m_thumbPanel->setComments(t->annotCache);
            qint64 ms = QDateTime::currentMSecsSinceEpoch() - scanStartMs;
            qDebug().noquote() << "[comments] FULL scan done pages=" << t->doc->pageCount()
                     << "found=" << t->annotCache.size() << "ms=" << ms;
        }
    });

    mgr->resetScan();
    t->annotScanFuture = QtConcurrent::run([mgr, pageCount, startPage]() {
        mgr->loadAllStreaming(pageCount, startPage);
    });
    watcher->setFuture(t->annotScanFuture);
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
        // Working-copy temp to clean up after the tab is gone (if any).
        QString workingTmp = (t->doc->filePath() != t->originalPath)
                             ? t->doc->filePath() : QString();

        m_openDocs.removeAt(i);
        // Cancel queued renders immediately so the background threads can wind down
        // while the UI is already updating — avoids waitForDone() blocking the close.
        t->renderer->cancelPending();
        disconnect(t->pageReadyConn);
        disconnect(t->scrollConn);
        // Doc dong: bo nho dem link theo trang cung duoc xoa (SPEC_PDF_LINKS).
        PdfLinks::clearCache();
        // Giai phong text page dem truoc khi doc bi huy o luong nen ben duoi.
        if (t->doc) TextSelection::closeDocument(t->doc->raw());
        m_docTabs->removeTab(idx);
        delete t->view;
        t->view = nullptr;
        if (m_openDocs.isEmpty()) {
            m_docTabs->addTab(new PdfView(m_docTabs), "Welcome");
            m_thumbPanel->clearThumbnails();
            setWindowTitle("TorReader PDF");
        }
        if (t->annotMgr) t->annotMgr->stopScan();
        if (m_navDeferTab == t) { m_navDeferTab = nullptr; m_navDeferPage = -1; }
        // Destroy renderer on a background thread so ~PdfRenderer()::waitForDone()
        // does not block the main thread while waiting for any in-flight PDFium render.
        auto* closeJob = new QFutureWatcher<void>(qApp);
        QObject::connect(closeJob, &QFutureWatcher<void>::finished,
                         closeJob, &QObject::deleteLater);
        QFuture<void> scanFut = t->annotScanFuture;
        QFuture<void> visualsFut = t->annotVisualsFuture;
        closeJob->setFuture(QtConcurrent::run([t, workingTmp, scanFut, visualsFut]() mutable {
            if (scanFut.isValid()) scanFut.waitForFinished();
            if (visualsFut.isValid()) visualsFut.waitForFinished();
            delete t;
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
        t->renderer->setCurrentPage(pageIndex);

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
        if (!t->vecBuilding.contains(pageIndex)
            && !(t->vecLayer && t->vecLayer->pageIndex() == pageIndex)) {
            int pg = pageIndex;
            t->vecBuilding.insert(pg);
            auto layer = std::make_shared<VectorLayer>();
            auto* w = new QFutureWatcher<bool>(this);
            connect(w, &QFutureWatcher<bool>::finished, this, [this, w, t, pg, layer]{
                w->deleteLater();
                t->vecBuilding.remove(pg);
                if (!m_openDocs.contains(t)) return;
                if (t->currentPage != pg) return;
                if (w->result()) {
                    t->vecLayer = layer;
                    t->view->setVectorLayer(layer);
                } else {
                    t->vecLayer.reset();
                    t->view->setVectorLayer(nullptr);
                }
            });
            FPDF_DOCUMENT d = t->doc->raw();
            w->setFuture(QtConcurrent::run([layer, d, pg]{
                QMutexLocker lk(&s_pdfiumMutex);
                return layer->build(d, pg);
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
            t->renderer->requestPage(pageIndex, t->zoom);
            statusBar()->showMessage(
                QString("Page %1 / %2 — Loading…").arg(pageIndex + 1).arg(t->doc->pageCount()));
        } else {
            qDebug() << "[Main] cache miss page=" << pageIndex << "— waiting 400ms settle (totalPending=" << totalPending << "ms)";
            m_settleTimer->start();
        }
    }
    // Sync sidebar thumbnail immediately (same event-loop pass)
    m_thumbPanel->setCurrentPage(pageIndex);

    if (m_continuousMode && m_continuousView)
        m_continuousView->scrollToPage(pageIndex);

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

    double oldZoom = t->zoom;
    onPageChanged(pageIndex);

    if (m_continuousMode && m_continuousView) {
        m_continuousView->scrollToPage(pageIndex);
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
    { QString aUid = list[annotIndex].uid; QString aType = list[annotIndex].type;
    if (aType == QLatin1String("FreeText")) {
        t->view->setDragNote(r);
    } else {
        t->view->setDragTarget(aUid, QString(), 0.0f, QColor());
    } }

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

        if (m_continuousMode && m_continuousView) {
            if (targetPdf.isEmpty()) {
                m_continuousView->scrollToPage(dest);
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
    if (m_continuousMode && m_continuousView && m_continuousView->isVisible()) {
        m_continuousView->setZoom(t->zoom);
    } else {
        if (t->view) t->view->setZoom(t->zoom);
    }
    if (m_zoomEdit)
        m_zoomEdit->setText(QString::number(qRound(t->zoom * 100)) + "%");
    // Refresh annot visuals for current page (data unchanged but overlay re-scales)
    if (t->view && t->doc && t->doc->isOpen())
        refreshAnnotVisuals(t, t->currentPage);
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
        if (r == QMessageBox::Yes) {
            for (auto* t : m_openDocs) {
                if (!t->dirty) continue;
                m_docTabs->setCurrentWidget(t->view);
                onSaveFile();
            }
        }
    }
    e->accept();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
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
    watcher->setFuture(QtConcurrent::run([rawDoc, pageIdx, rectPts]() -> QString {
        QString text;
        QMutexLocker lock(&s_pdfiumMutex);
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
    }));
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
// NANG: FPDF_LoadPage + FPDFText_LoadPage day du, giu s_pdfiumMutex. Chi dung
// o hinh thuc user nguoi dung (menu chuot phai, bam OCR, chon chu) — LUOT LAT
// TRANG di qua notifyOcrStatusForPage (cache + QtConcurrent, xem ben duoi).
bool MainWindow::pageHasTextSync(FPDF_DOCUMENT doc, int pageIndex) {
    if (!doc || pageIndex < 0) return false;
    QMutexLocker lock(&s_pdfiumMutex);
    FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
    if (!page) return false;
    int n = 0;
    FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
    if (tp) { n = FPDFText_CountChars(tp); FPDFText_ClosePage(tp); }
    FPDF_ClosePage(page);
    return n > 0;
}

bool MainWindow::pageNeedsOcr(FPDF_DOCUMENT doc, int pageIndex) {
    return doc && pageIndex >= 0
        && !pageHasTextSync(doc, pageIndex)
        && !OcrTextLayer::pageDone(doc, pageIndex);
}

bool MainWindow::docHasAnyText(FPDF_DOCUMENT doc, int currentPage) {
    if (!doc) return false;
    // Chi kiem MAU (3 trang dau + trang hien tai) de khoi nap toan bo file
    // tren luong giao dien — file nhieu trang (CAD) se treo khi Ctrl+F.
    // Mau du de quyet dinh co hoi OCR hay khong: tai lieu tu khoa co chu o
    // dau do thi it nhat roi vao cac trang dau hoac trang dang xem.
    QMutexLocker lock(&s_pdfiumMutex);
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
    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher, tab, pages, traces]() {
        watcher->deleteLater();
        if (m_ocrWatcher == watcher) m_ocrWatcher = nullptr;
        m_ocrCancel.reset();
        if (m_thumbPanel && m_thumbPanel->ocrPanel())
            m_thumbPanel->ocrPanel()->setOcrRunning(false);
        // Tab co the da dong giua chung — khong dong cham vao UI cua no.
        if (!m_openDocs.contains(tab) || !tab->doc || !tab->doc->isOpen()) {
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
        if (tab->renderer && tab->view)
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
    watcher->setFuture(QtConcurrent::run([doc, pages, dpi, langs, totalPages, pdfPath,
                                          traces, self, cancel]() {
        for (int p : pages) {
            if (cancel->loadRelaxed()) break;   // nhan Cancel
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
    QtConcurrent::run([doc, page, self]() {
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
    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher, tab=t]() {
        watcher->deleteLater();
        if (m_ocrWatcher == watcher) m_ocrWatcher = nullptr;
        if (!m_openDocs.contains(tab) || !tab->doc || !tab->doc->isOpen()) {
            m_ocrSearchPendingQuery.clear();
            statusBar()->clearMessage();
            return;
        }
        for (int p = 0; p < tab->doc->pageCount(); ++p) {
            if (tab->renderer) tab->renderer->invalidatePage(p);
            TextSelection::closePage(tab->doc->raw(), p);
        }
        if (tab->renderer && tab->view)
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
    watcher->setFuture(QtConcurrent::run([raw, pages, dpi, langs]() {
        for (int p = 0; p < pages; ++p) {
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
