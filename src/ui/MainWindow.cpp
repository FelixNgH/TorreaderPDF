#include "MainWindow.h"
#include <QDebug>
#include "PdfView.h"
#include "PdfGpuView.h"
#include "ThumbnailPanel.h"
#include "ContinuousView.h"
#include "FindBar.h"
#include "MergeDialog.h"
#include "SignDialog.h"
#include "AboutDialog.h"
#include "PrintDialog.h"
#include "core/PdfDocument.h"
#include "core/PdfRenderer.h"
#include "core/PdfEditor.h"
#include "core/TextSearch.h"
#include "core/VectorLayer.h"
#include "annotations/AnnotationManager.h"
#include "core/GoogleAuth.h"
#include "core/Translator.h"
#include "TranslationPopup.h"
#include "NoteInputDialog.h"
#include "core/UpdateChecker.h"
#include "GateDialog.h"

#include <fpdf_text.h>
#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <fpdf_annot.h>
#include <fpdf_save.h>
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

static const char* kDarkQss = R"(
QMainWindow, QWidget                    { background:#1E1E1E; color:#D4D4D4; }
QSplitter::handle                       { background:#333; width:1px; }
QToolBar                                { background:#2D2D30; border-bottom:1px solid #111; spacing:2px; padding:2px 8px; }
QToolButton                             { color:#D4D4D4; padding:2px 6px; border-radius:3px; border:none; background:transparent; }
QToolButton:hover                       { background:#1177BB; color:white; }
QToolButton:checked                     { background:#1177BB; color:white; }
QToolButton:pressed                     { background:#005f9e; }
QTabWidget::pane                        { border:none; }
QTabBar::tab                            { background:#2D2D30; color:#AAA; padding:5px 14px; min-width:80px; }
QTabBar::tab:selected                   { background:#1E1E1E; color:white; border-bottom:2px solid #007ACC; }
QTabBar::tab:hover:!selected            { background:#3E3E42; }
QTabBar QToolButton                     { background:#1177BB; color:white; border:none; border-radius:2px; min-width:20px; font-weight:bold; }
QTabBar QToolButton:hover               { background:#005f9e; }
QPushButton#sidebarTab                  { background:#2D2D30; color:#9DA5B4; border:none; border-bottom:1px solid #333; border-radius:0; padding:3px 2px; font-size:11px; }
QPushButton#sidebarTab:checked          { background:#1E1E1E; color:white; border-bottom:2px solid #007ACC; }
QPushButton#sidebarTab:hover:!checked   { background:#3E3E42; color:#D4D4D4; }
QStatusBar                              { background:#2D2D30; color:#9DA5B4; }
QListWidget, QTreeWidget                { background:#1E1E1E; color:#D4D4D4; border:none; outline:none; }
QListWidget::item:hover,
QTreeWidget::item:hover                 { background:#2A2D2E; }
QListWidget::item:selected,
QTreeWidget::item:selected              { background:#094771; color:white; }
QScrollBar:vertical                     { background:#252526; width:8px; border:none; }
QScrollBar::handle:vertical             { background:#555; border-radius:4px; min-height:20px; }
QScrollBar::add-line:vertical,
QScrollBar::sub-line:vertical           { height:0; }
QScrollBar:horizontal                   { background:#252526; height:8px; border:none; }
QScrollBar::handle:horizontal           { background:#555; border-radius:4px; }
QScrollBar::add-line:horizontal,
QScrollBar::sub-line:horizontal         { width:0; }
QMenu                                   { background:#2D2D30; color:#D4D4D4; border:1px solid #555; }
QMenu::item:selected                    { background:#094771; }
QMenu::separator                        { background:#555; height:1px; margin:2px 0; }
QDialog, QMessageBox                    { background:#2D2D30; color:#D4D4D4; }
QPushButton                             { background:#3E3E42; color:#D4D4D4; border:1px solid #555; border-radius:3px; padding:4px 14px; }
QPushButton:hover                       { background:#4E4E52; }
QPushButton:pressed                     { background:#007ACC; border-color:#007ACC; }
QPushButton:default                     { border-color:#007ACC; }
QTextEdit, QLineEdit                    { background:#252526; color:#D4D4D4; border:1px solid #555; border-radius:3px; padding:3px 6px; }
QTextEdit:focus, QLineEdit:focus        { border-color:#007ACC; }
QLabel                                  { background:transparent; }
)";

static const char* kLightQss = R"(
QMainWindow, QWidget                    { background:#F5F5F5; color:#1F2937; }
QSplitter::handle                       { background:#B0B8C1; width:2px; height:2px; }
QToolBar                                { background:#FFFFFF; border-bottom:2px solid #CBD5E1; spacing:2px; padding:2px 8px; }
QToolBar::separator                     { background:#CBD5E1; width:1px; margin:4px 3px; }
QToolButton                             { color:#000000; padding:2px 6px; border-radius:3px; border:none; background:transparent; }
QToolButton:hover                       { background:#2563EB; color:white; }
QToolButton:checked                     { background:#2563EB; color:white; }
QToolButton:pressed                     { background:#1D4ED8; }
QTabWidget::pane                        { border:none; background:transparent; }
QTabBar                                 { background:#F1F5F9; border-bottom:none; }
QTabBar::tab                            { background:#E2E8F0; color:#475569; padding:5px 14px; min-width:80px; border-right:1px solid #CBD5E1; }
QTabBar::tab:selected                   { background:white; color:#111827; border-bottom:2px solid #2563EB; }
QTabBar::tab:hover:!selected            { background:#F1F5F9; color:#1F2937; }
QTabBar QToolButton                     { background:#2563EB; color:white; border:none; border-radius:2px; min-width:20px; font-weight:bold; }
QTabBar QToolButton:hover               { background:#1D4ED8; }
QPushButton#sidebarTab                  { background:#E2E8F0; color:#475569; border:none; border-bottom:1px solid #CBD5E1; border-radius:0; padding:3px 2px; font-size:11px; }
QPushButton#sidebarTab:checked          { background:white; color:#111827; border-bottom:2px solid #2563EB; }
QPushButton#sidebarTab:hover:!checked   { background:#F1F5F9; color:#1F2937; }
QStatusBar                              { background:#FFFFFF; color:#6B7280; border-top:2px solid #CBD5E1; }
QListWidget, QTreeWidget                { background:white; color:#1F2937; border:none; outline:none; border-right:1px solid #E2E8F0; }
QListWidget::item:hover,
QTreeWidget::item:hover                 { background:#EFF6FF; }
QListWidget::item:selected,
QTreeWidget::item:selected              { background:#DBEAFE; color:#1E40AF; }
QScrollBar:vertical                     { background:#F1F5F9; width:8px; border:none; }
QScrollBar::handle:vertical             { background:#94A3B8; border-radius:4px; min-height:20px; }
QScrollBar::add-line:vertical,
QScrollBar::sub-line:vertical           { height:0; }
QScrollBar:horizontal                   { background:#F1F5F9; height:8px; border:none; }
QScrollBar::handle:horizontal           { background:#94A3B8; border-radius:4px; }
QScrollBar::add-line:horizontal,
QScrollBar::sub-line:horizontal         { width:0; }
QMenu                                   { background:white; color:#1F2937; border:1px solid #CBD5E1; }
QMenu::item:selected                    { background:#DBEAFE; color:#1E40AF; }
QMenu::separator                        { background:#CBD5E1; height:1px; margin:2px 0; }
QDialog, QMessageBox                    { background:#F9FAFB; color:#1F2937; }
QPushButton                             { background:#F1F5F9; color:#374151; border:1px solid #CBD5E1; border-radius:3px; padding:4px 14px; }
QPushButton:hover                       { background:#E2E8F0; }
QPushButton:pressed                     { background:#2563EB; color:white; border-color:#2563EB; }
QPushButton:default                     { border-color:#2563EB; color:#2563EB; }
QTextEdit, QLineEdit                    { background:white; color:#1F2937; border:1px solid #CBD5E1; border-radius:3px; padding:3px 6px; }
QTextEdit:focus, QLineEdit:focus        { border-color:#2563EB; }
QLabel                                  { background:transparent; }
)";

// ── Constructor / Destructor ─────────────────────────────────────────────────

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    m_editor     = std::make_unique<PdfEditor>(this);
    m_textSearch = new TextSearch(this);

    menuBar()->hide();
    setupActionBar();
    new QShortcut(QKeySequence(Qt::Key_Delete), this, [this]{
        if (m_selPage >= 0 && m_selIdx >= 0) deleteSelectedAnnot(m_selPage, m_selIdx);
    });
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
          if (auto* t = currentTab()) if (t->view) t->view->setTool(PdfGpuView::ViewTool::Pan);
          if (m_thumbPanel) m_thumbPanel->setActiveToolButton(0);
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
    m_docTabs->setDocumentMode(true);
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
        if (auto* t = currentTab()) if (t->view) t->view->setTool(static_cast<PdfGpuView::ViewTool>(id));
    });
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
            this, [this](const QString& query) {
        auto* t = currentTab();
        if (!t || !t->doc->isOpen() || query.trimmed().isEmpty()) return;
        m_searchResults.clear();
        m_searchCurrentIdx = -1;
        m_thumbPanel->clearSearchResults();
        m_textSearch->cancel();
        m_textSearch->search(t->doc.get(), query, Qt::CaseInsensitive);
    });
    // Found results go to both SearchPanel and FindBar's result tracking
    connect(m_textSearch, &TextSearch::found,
            m_thumbPanel, &ThumbnailPanel::addSearchResult);
    connect(m_textSearch, &TextSearch::progress,
            m_thumbPanel, &ThumbnailPanel::setSearchProgress);
    connect(m_thumbPanel, &ThumbnailPanel::searchResultSelected,
            this, [this](int page, QRectF rect) {
        m_textSearch->cancel();
        m_searchCurrentIdx = -1;
        onPageChanged(page);
        if (m_continuousMode && m_continuousView)
            m_continuousView->scrollToPage(page);
        applySearchHighlights(m_searchResults, -1);
        if (m_findBar) m_findBar->setCurrentMatch(-1);
    });

    // FindBar connections
    connect(m_findBar, &FindBar::searchRequested,
            this, [this](const QString& query, Qt::CaseSensitivity cs) {
        auto* t = currentTab();
        if (!t || !t->doc->isOpen() || query.trimmed().isEmpty()) return;
        m_searchResults.clear();
        m_searchCurrentIdx = -1;
        if (m_thumbPanel) m_thumbPanel->clearSearchResults();
        m_textSearch->cancel();
        m_textSearch->search(t->doc.get(), query, cs);
    });
    connect(m_textSearch, &TextSearch::found,
            this, [this](const SearchResult& r) {
        m_searchResults.append(r);
        if (m_findBar) m_findBar->onFound();
    });
    connect(m_textSearch, &TextSearch::searchComplete,
            this, [this](int total) {
        m_searchCurrentIdx = total > 0 ? 0 : -1;
        if (m_findBar) {
            m_findBar->onSearchComplete(total);
            if (total > 0) m_findBar->setCurrentMatch(0);
        }
        if (total > 0)
            applySearchHighlights(m_searchResults, 0);
    });
    connect(m_findBar, &FindBar::navigateNext,
            this, [this]() {
        if (m_searchResults.isEmpty()) return;
        m_textSearch->cancel();
        int prevIdx = m_searchCurrentIdx;
        m_searchCurrentIdx = (m_searchCurrentIdx + 1) % m_searchResults.size();
        if (prevIdx > m_searchCurrentIdx)
            statusBar()->showMessage("Reached end of document, continued from top", 3000);
        const auto& r = m_searchResults[m_searchCurrentIdx];
        onPageChanged(r.pageIndex);
        if (m_continuousMode && m_continuousView)
            m_continuousView->scrollToPage(r.pageIndex);
        applySearchHighlights(m_searchResults, m_searchCurrentIdx);
        if (m_findBar) m_findBar->setCurrentMatch(m_searchCurrentIdx);
    });
    connect(m_findBar, &FindBar::navigatePrev,
            this, [this]() {
        if (m_searchResults.isEmpty()) return;
        m_textSearch->cancel();
        int prevIdx = m_searchCurrentIdx;
        m_searchCurrentIdx = (m_searchCurrentIdx - 1 + m_searchResults.size()) % m_searchResults.size();
        if (prevIdx < m_searchCurrentIdx)
            statusBar()->showMessage("Reached beginning of document, continued from end", 3000);
        const auto& r = m_searchResults[m_searchCurrentIdx];
        onPageChanged(r.pageIndex);
        if (m_continuousMode && m_continuousView)
            m_continuousView->scrollToPage(r.pageIndex);
        applySearchHighlights(m_searchResults, m_searchCurrentIdx);
        if (m_findBar) m_findBar->setCurrentMatch(m_searchCurrentIdx);
    });
    connect(m_findBar, &FindBar::clearSearchHighlights,
            this, &MainWindow::clearAllSearchHighlights);
    connect(m_thumbPanel, &ThumbnailPanel::searchCleared, this, [this] {
        m_textSearch->cancel();
        m_searchResults.clear();
        m_searchCurrentIdx = -1;
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

    // Start in light mode
    applyTheme(false);

    // Permanent hint bar — shows shortcut hints on the right side of the status bar
    auto* hintLabel = new QLabel(
        "Ctrl+Scroll: Zoom  ·  Alt+Drag: Translate  "
        "·  Scroll: Flip page  ·  Right-click thumbnail: Page options");
    hintLabel->setStyleSheet("color:#9CA3AF; font-size:10px; padding-right:8px;");
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
        w->setFuture(QtConcurrent::run([mgr, pg] {
            mgr->pinPage(pg);
        }));
    });

    connect(qApp, &QApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState st) {
        if (st != Qt::ApplicationActive) hideNotePopup();
    });
}

MainWindow::~MainWindow() {
    for (auto* t : m_openDocs) {
        disconnect(t->pageReadyConn);
        disconnect(t->scrollConn);
        delete t;
    }
}

// ── Theme ────────────────────────────────────────────────────────────────────

void MainWindow::applyTheme(bool dark) {
    m_darkMode = dark;
    qApp->setStyleSheet(dark ? kDarkQss : kLightQss);
    for (auto* t : m_openDocs)
        if (t->view) t->view->setDarkMode(dark);
    if (m_continuousView) m_continuousView->setDarkMode(dark);
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
    m_continuousAct->setToolTip("Continuous scroll — all pages in one strip  (C)");
    m_continuousAct->setShortcut(QKeySequence("C"));
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
    // Explicit dark text — the box keeps a light background in both themes,
    // so without this the zoom % turns grey-white and unreadable in dark mode.
    m_zoomEdit->setStyleSheet(
        "QLineEdit { background:#F1F5F9; color:#0F172A; border:1px solid #CBD5E1; "
        "border-radius:3px; padding:1px 4px; font-size:11px; }");
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

    // Dark mode
    auto* darkAct = tb->addAction("Dark Mode");
    darkAct->setCheckable(true);
    connect(darkAct, &QAction::toggled, this, &MainWindow::applyTheme);
    tb->addSeparator();

    // About
    tb->addAction("About", this, [this] {
        AboutDialog dlg(this);
        dlg.exec();
    });
    tb->addSeparator();

    // Translate
    m_translateAct = tb->addAction("Translate");
    m_translateAct->setToolTip(
        "Enable Google Translate — hold Alt and drag over text to select & translate\n"
        "Works in both Single and Continuous modes\n"
        "Right-click to reset consent");
    m_translateAct->setShortcut(QKeySequence("T"));
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

    // Right-click the Translate button → reset consent
    auto* translateBtn = qobject_cast<QToolButton*>(tb->widgetForAction(m_translateAct));
    if (translateBtn) {
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
        connect(copyBtn, &QPushButton::clicked, &dlg, [this, url, copyBtn] {
            QGuiApplication::clipboard()->setText(url);
            copyBtn->setText("Copied!");
            if (statusBar()) statusBar()->showMessage("Link copied to clipboard", 3000);
        });
        connect(openBtn, &QPushButton::clicked, &dlg, [url] { QDesktopServices::openUrl(QUrl(url)); });
        connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
        dlg.exec();
    });

    // Help — all keyboard shortcuts & usage instructions (right of Translate)
    auto* helpAct = tb->addAction("Help");
    helpAct->setToolTip("Show all keyboard shortcuts and usage instructions  (F1)");
    helpAct->setShortcut(QKeySequence("F1"));
    connect(helpAct, &QAction::triggered, this, [this]() {
        QMessageBox mb(this);
        mb.setWindowTitle("Help — Shortcuts & Instructions");
        mb.setTextFormat(Qt::RichText);
        mb.setText(
            "<h3 style='margin-top:0'>Keyboard shortcuts</h3>"
            "<table cellspacing='6'>"
            "<tr><td><b>Ctrl+O</b></td><td>Open PDF</td></tr>"
            "<tr><td><b>Ctrl+S</b> / <b>Ctrl+Shift+S</b></td><td>Save / Save As</td></tr>"
            "<tr><td><b>Ctrl+M</b></td><td>Merge PDFs</td></tr>"
            "<tr><td><b>Ctrl+Shift+E</b></td><td>Extract all pages</td></tr>"
            "<tr><td><b>Ctrl+P</b></td><td>Print</td></tr>"
            "<tr><td><b>C</b></td><td>Toggle Continuous scroll</td></tr>"
            "<tr><td><b>Ctrl+Shift+F</b></td><td>Fit page</td></tr>"
            "<tr><td><b>Ctrl+=</b> / <b>Ctrl+&minus;</b></td><td>Zoom in / out</td></tr>"
            "<tr><td><b>T</b></td><td>Translate mode</td></tr>"
            "<tr><td><b>F1</b></td><td>Show this help</td></tr>"
            "</table>"
            "<h3>Mouse</h3>"
            "<table cellspacing='6'>"
            "<tr><td><b>Ctrl+Scroll</b></td><td>Zoom in / out</td></tr>"
            "<tr><td><b>Alt+Drag</b></td><td>Select text to translate</td></tr>"
            "<tr><td><b>Scroll</b></td><td>Flip page</td></tr>"
            "<tr><td><b>Right-click thumbnail</b></td><td>Page options: Insert / Delete / Extract</td></tr>"
            "<tr><td><b>Right-click Translate</b></td><td>Reset translation consent</td></tr>"
            "</table>");
        mb.setStandardButtons(QMessageBox::Ok);
        mb.exec();
    });
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
        m_notePopup->setStyleSheet("QLabel{ background:#FFFDE7; border:1px solid #C9B458; color:#111827; }");
    }
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

bool MainWindow::canFastPath(DocTab* t, int page) const {
    return t->overlayCapablePage.value(page, false);
}

bool MainWindow::baseIsVector(DocTab* t, int page) const {
    return t && t->vecLayer && t->vecLayer->isReady()
        && t->vecLayer->pageIndex() == page && t->vecLayer->isComplete();
}

void MainWindow::refreshAnnotVisuals(DocTab* t, int page) {
    if (!t || !t->annotMgr || !t->doc || !t->doc->isOpen()) {
        if (t && t->view) t->view->clearAnnotVisuals();
        return;
    }
    bool overlayCapable = false;
    bool hasForeign = false;
    QList<AnnotVisual> visuals;
    const quint32 rev = t->annotMgr ? t->annotMgr->pageRevision(page) : 0;
    if (t->visualsCache.contains(page) && t->visualsRev.value(page, 0xFFFFFFFFu) == rev) {
        visuals        = t->visualsCache.value(page);
        overlayCapable = t->overlayCapablePage.value(page, false);
        hasForeign     = t->visualsHasForeign.value(page, false);
        qDebug().noquote() << "[perf] visuals CACHE HIT page=" << page
                           << "n=" << visuals.size();
    } else {
        QElapsedTimer _rescanTimer;
        _rescanTimer.start();
        visuals = t->annotMgr->loadPageVisuals(page, &overlayCapable, &hasForeign);
        qDebug().noquote() << "[perf] visuals RESCAN page=" << page << "ms=" << _rescanTimer.elapsed();
        t->visualsCache.insert(page, visuals);
        t->visualsRev[page] = rev;
        t->overlayCapablePage[page] = overlayCapable;
        t->visualsHasForeign[page]  = hasForeign;
    }
    if (!overlayCapable)
        t->pagesNeedGenerate.insert(page);  // fallback path relies on renderer having annots
    t->renderer->setPageAnnotRender(page, !overlayCapable || hasForeign);
    t->renderer->setPageAnnotOverlay(page, overlayCapable);
    qDebug().noquote() << "[overlay] page=" << page << "overlayCapable=" << overlayCapable << "hasForeign=" << hasForeign << "visuals=" << visuals.size();
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
            t->annotMgr->pinPage_locked(page);
            FPDF_PAGE pg = t->annotMgr->acquireSharedPage(page);
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

    // ── Annotation signals ────────────────────────────────────────────────────
    connect(tab->annotMgr.get(), &AnnotationManager::pageContentChanged, this,
            [this, tab](int page) {
        if (!m_openDocs.contains(tab)) return;
        refreshAnnotVisuals(tab, page);
        if (m_thumbPanel && m_thumbPanel->isCommentsTabVisible()) refreshCommentsForPage(tab, page);
        if (baseIsVector(tab, page) && tab->vecLayer && tab->annotMgr && tab->doc) {
            {
                QMutexLocker lk(&s_pdfiumMutex);
                tab->annotMgr->pinPage_locked(page);
                FPDF_PAGE pg = tab->annotMgr->acquireSharedPage(page);
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
        if (!tab->annotMgr) return;
        const auto& list = annotsForPage(tab, page);
        m_selPage = -1; m_selIdx = -1;
        for (int i = list.size() - 1; i >= 0; --i) {
            if (list[i].type == QLatin1String("Widget")) continue;
            QRectF hitRect = list[i].rect.normalized().adjusted(-3, -3, 3, 3);
            if (hitRect.contains(pt)) {
                m_selPage = page; m_selIdx = i;
                qDebug().noquote() << "[markup] picked annot page=" << page << "idx=" << i << "type=" << list[i].type;
                { QString pUid = list[i].uid; QString pType = list[i].type; QString pText = list[i].text;
                tab->view->setSelectedAnnot(list[i].rect.normalized());
                if (pType == QLatin1String("FreeText")) {
                    tab->view->setDragNote(list[i].rect.normalized());
                } else {
                    tab->view->setDragTarget(pUid, QString(), 0.0f, QColor());
                } }
                m_thumbPanel->selectCommentFor(page, i);
                if (!list[i].text.isEmpty()) {
                    showNotePopup(list[i].text, list[i].author);
                }
                return;
            }
        }
        tab->view->clearSelectedAnnot();
        m_thumbPanel->selectCommentFor(-1, -1);
        hideNotePopup();
    });

    connect(tab->view, &PdfGpuView::annotationContextRequested, this,
            [this, tab](int page, QPointF pt, QPoint gpos) {
        if (!tab->annotMgr) return;
        QElapsedTimer _perfRC;
        _perfRC.start();
        int idx = -1;
        QString ctxType, ctxUid;
        QRectF ctxRect;
        {
            const auto& list = annotsForPage(tab, page);
            for (int i = list.size() - 1; i >= 0; --i) {
                if (list[i].type == QLatin1String("Widget")) continue;
                QRectF hitRect = list[i].rect.normalized().adjusted(-3, -3, 3, 3);
                if (hitRect.contains(pt)) { idx = i; break; }
            }
            if (idx < 0) { tab->view->clearSelectedAnnot(); m_thumbPanel->selectCommentFor(-1, -1); return; }
            ctxType = list[idx].type;
            ctxUid  = list[idx].uid;
            ctxRect = list[idx].rect.normalized();
        }
        m_selPage = page; m_selIdx = idx;
        tab->view->setSelectedAnnot(ctxRect);
        if (ctxType == QLatin1String("FreeText")) {
            tab->view->setDragNote(ctxRect.normalized());
        } else {
            tab->view->setDragTarget(ctxUid, QString(), 0.0f, QColor());
        }
        m_thumbPanel->selectCommentFor(page, idx);
        QMenu menu(this);
        const bool canEditText = (ctxType == QLatin1String("FreeText") || ctxType == QLatin1String("Note"));
        QAction* editAct = canEditText ? menu.addAction("Edit text…") : nullptr;
        QAction* propAct = menu.addAction("Properties…");
        QAction* del     = menu.addAction("Delete");
        qDebug().noquote() << "[perf] rightclick handled ms=" << _perfRC.elapsed();
        QAction* chosen  = menu.exec(gpos);
        if (chosen == del) {
            deleteSelectedAnnot(page, idx);
        } else if (editAct && chosen == editAct) {
            editSelectedAnnot(page, idx);
        } else if (chosen == propAct) {
            int realIdxProp = idx;
            if (!ctxUid.isEmpty()) {
                int rp = tab->annotMgr->findAnnotIndexByUid(page, ctxUid);
                if (rp >= 0) realIdxProp = rp;
            }
            QDialog dlg(this);
            dlg.setWindowTitle("Markup properties");
            auto* form = new QFormLayout(&dlg);
            QString realType; QColor realColor = m_annotStyle.strokeColor; float realWidth = m_annotStyle.strokeWidth; float realFont = 11.0f;
            bool curHasFill = false; int curFillAlpha = 255;
            tab->annotMgr->getAnnotEditState(page, realIdxProp, realType, realColor, realWidth, realFont, &curHasFill, &curFillAlpha);
            QColor curColor = realColor;
            auto* colorBtn = new QPushButton("Choose…");
            colorBtn->setStyleSheet(QString("background:%1;color:white;").arg(curColor.name()));
            connect(colorBtn, &QPushButton::clicked, &dlg, [&]{
                QColor c = QColorDialog::getColor(curColor, &dlg, "Markup color");
                if (c.isValid()) { curColor = c; colorBtn->setStyleSheet(QString("background:%1;color:white;").arg(c.name())); }
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
                if (tab->annotMgr->rebuildTextNote(page, realIdxProp, curColor,
                                                   static_cast<float>(fontSpin->value()))) {
                    {
                        MarkupUndoEntry ue; ue.kind = MarkupUndoEntry::RestyleAnnot; ue.page = page;
                        ue.uid = ctxUid;
                        ue.oldColor = realColor; ue.newColor = curColor;
                        ue.oldFontSize = realFont; ue.newFontSize = static_cast<float>(fontSpin->value());
                        ue.isFreeText = true;
                        if (!ctxUid.isEmpty()) pushUndo(tab, ue);
                    }
                    tab->dirty = true; updateTabDirty(tab);
                    invalidateAnnotPage(tab, page);
                    tab->pagesNeedGenerate.insert(page);
                    refreshAnnotVisuals(tab, page);
                    if (baseIsVector(tab, page)) {
                        if (tab->renderer) tab->renderer->invalidatePage(page);
                        if (tab->view) tab->view->update();
                    } else {
                        if (tab->renderer) { tab->renderer->invalidatePage(page); tab->renderer->requestPage(page, tab->zoom); }
                        if (tab->view) tab->view->invalidateTiles();
                        if (tab->view) tab->view->invalidateSharp();
                    }
                    m_selPage = -1; m_selIdx = -1;
                    tab->view->clearSelectedAnnot();
                    refreshCommentsForPage(tab, page);
                } else {
                    statusBar()->showMessage(
                        "Chú thích này của phần mềm khác — đổi màu/cỡ chữ sẽ làm mất định dạng gốc nên đã bỏ qua", 5000);
                }
            } else {
                if (tab->annotMgr->setAnnotStyle(page, realIdxProp, curColor,
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
                        if (!ctxUid.isEmpty()) pushUndo(tab, ue);
                    }
                    tab->dirty = true; updateTabDirty(tab);
                    invalidateAnnotPage(tab, page);
                    tab->pagesNeedGenerate.insert(page);
                    refreshAnnotVisuals(tab, page);
                    if (canFastPath(tab, page)) {
                        if (tab->view) tab->view->update();
                    } else if (baseIsVector(tab, page)) {
                        if (tab->renderer) tab->renderer->invalidatePage(page);
                        if (tab->view) tab->view->update();
                    } else {
                        if (tab->renderer) { tab->renderer->invalidatePage(page); tab->renderer->requestPage(page, tab->zoom); }
                        if (tab->view) tab->view->invalidateTiles();
                        if (tab->view) tab->view->invalidateSharp();
                    }
                    refreshCommentsForPage(tab, page);
                }
            }
        }
    });

    connect(tab->view, &PdfGpuView::annotationMoveRequested, this,
            [this, tab](int page, double dx, double dy) {
        QElapsedTimer _totalT; _totalT.start();
        QElapsedTimer _stepT; _stepT.start();
        qDebug().noquote() << "[markup] move BAT DAU page=" << page
                           << "selIdx=" << m_selIdx << "dx=" << dx << "dy=" << dy;
        if (m_selPage != page || m_selIdx < 0 || !tab->annotMgr) {
            qDebug().noquote() << "[markup] move BO QUA: khong co annot dang chon";
            qDebug().noquote() << "[perf] MOVE handler total ms=" << _totalT.elapsed();
            return;
        }
        QString moveType, moveUid;
        {
            _stepT.restart();
            const auto& annots = annotsForPage(tab, page);
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
            int r = tab->annotMgr->findAnnotIndexByUid(page, moveUid);
            qDebug().noquote() << "[perf] MOVE step findAnnotIndexByUid ms=" << _stepT.elapsed();
            if (r >= 0) realMoveIdx = r;
        }
        _stepT.restart();
        bool isForeign = !tab->annotMgr->isOwnAnnot(page, realMoveIdx);
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
            FPDF_PAGE mp = tab->annotMgr->acquireSharedPage(page);
            qDebug().noquote() << "[perf] MOVE step acquireSharedPage ms=" << _stepT.elapsed();
            bool selfOwned = false;
            if (!mp) {
                _stepT.restart();
                mp = FPDF_LoadPage(tab->doc->raw(), page);
                qDebug().noquote() << "[perf] MOVE step FPDF_LoadPage (fallback) ms=" << _stepT.elapsed();
                selfOwned = (mp != nullptr);
            }
            if (mp) {
                _stepT.restart();
                switch (FPDFPage_GetRotation(mp)) {
                    case 1: dxU = -dy; dyU = dx;  break;
                    case 2: dxU = -dx; dyU = -dy; break;
                    case 3: dxU =  dy; dyU = -dx; break;
                    default: break;
                }
                qDebug().noquote() << "[perf] MOVE step GetRotation+switch ms=" << _stepT.elapsed();
                if (selfOwned) {
                    _stepT.restart();
                    FPDF_ClosePage(mp);
                    qDebug().noquote() << "[perf] MOVE step FPDF_ClosePage (fallback) ms=" << _stepT.elapsed();
                }
            }
        }
        {
            MarkupUndoEntry ue; ue.kind = MarkupUndoEntry::MoveAnnot; ue.page = page;
            ue.uid = moveUid;
            if (isForeign && ue.uid.isEmpty()) {
                _stepT.restart();
                ue.uid = tab->annotMgr->ensureExternalUid(page, realMoveIdx);
                qDebug().noquote() << "[perf] MOVE step ensureExternalUid ms=" << _stepT.elapsed();
            }
            ue.dxU = dxU; ue.dyU = dyU;
            if (!ue.uid.isEmpty()) {
                _stepT.restart();
                pushUndo(tab, ue);
                qDebug().noquote() << "[perf] MOVE step pushUndo ms=" << _stepT.elapsed();
            }
            else qDebug() << "[undo] move KHONG ghi duoc: annot khong co uid page=" << page;
        }
        qDebug().noquote() << "[perf] MOVE pre-phase ms=" << _totalT.elapsed();
        bool ok = tab->annotMgr->moveAnnot(page, realMoveIdx, dxU, dyU);
        if (!ok) {
            qDebug().noquote() << "[markup] move THAT BAI (moveAnnot tra false)";
            statusBar()->showMessage("Chú thích này của phần mềm khác — không di chuyển được mà không làm hỏng nó", 4000);
            qDebug().noquote() << "[perf] MOVE handler total ms=" << _totalT.elapsed();
            return;
        }
        tab->annotPageCache.remove(page);
        tab->visualsCache.remove(page);
        tab->visualsRev.remove(page);
        if (baseIsVector(tab, page) && tab->vecLayer && tab->annotMgr && tab->doc) {
            QMutexLocker lk(&s_pdfiumMutex);
            tab->annotMgr->pinPage_locked(page);
            FPDF_PAGE pg = tab->annotMgr->acquireSharedPage(page);
            if (pg) tab->vecLayer->rebuildNoteTiles(tab->doc->raw(), pg);
        }
        if (tab->view) tab->view->invalidateTileTextures();
        refreshAnnotVisuals(tab, page);
        int newIdx = moveUid.isEmpty() ? -1 : tab->annotMgr->findAnnotIndexByAnyUid(page, moveUid);
        if (newIdx >= 0) {
            const auto& nl = annotsForPage(tab, page);
            if (newIdx < nl.size() && tab->view) tab->view->setSelectedAnnot(nl[newIdx].rect.normalized());
        }
        if (tab->view) tab->view->update();
        if (isPageObjNote) {
            qDebug().noquote() << "[perf] note icon -> rebuild vector layer page=" << page;
            buildVectorLayer(tab, page, true);
        }
        qDebug().noquote() << "[perf] MOVE handler total ms=" << _totalT.elapsed();
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
                if (tab == currentTab())
                    m_thumbPanel->setDocument(tab->doc.get(), tab->renderer.get(),
                                              tab->thumbPool.get(), false);
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
                refreshAnnotVisuals(tab, tab->currentPage);
                if (!m_searchResults.isEmpty())
                    applySearchHighlights(m_searchResults, m_searchCurrentIdx);
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
        }
        refreshAnnotVisuals(tab, 0);
        tab->renderer->requestPage(0, tab->zoom);

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

// ── Search highlight helpers (shared by FindBar + SearchPanel) ─────────────────
void MainWindow::applySearchHighlights(const QList<SearchResult>& results, int currentIdx) {
    auto* t = currentTab();
    if (!t) return;
    qDebug().noquote() << "[find] applySearchHighlights mode="
             << (m_continuousMode ? "continuous" : "single")
             << "page=" << t->currentPage << "rects=" << results.size() << "currentIdx=" << currentIdx;
    // Filter results for the current page only
    QList<QRectF> pageRects;
    int pageLocalIdx = -1;
    int localCount = 0;
    for (int i = 0; i < results.size(); ++i) {
        if (results[i].pageIndex == t->currentPage) {
            pageRects.append(results[i].boundingBox);
            if (i == currentIdx) pageLocalIdx = localCount;
            ++localCount;
        }
    }
    // FindBar passes currentIdx as the global index; adjust to page-local index
    int effectiveIdx = pageLocalIdx;
    if (m_continuousMode && m_continuousView) {
        m_continuousView->setHighlights(t->currentPage, pageRects, effectiveIdx);
        if (auto* view = t->view) view->clearHighlights();
    } else {
        if (auto* view = t->view) {
            if (effectiveIdx >= 0)
                view->setHighlights(pageRects, effectiveIdx);
            else
                view->setHighlights(pageRects);
        }
        if (m_continuousView) m_continuousView->clearHighlights();
    }
}

void MainWindow::clearAllSearchHighlights() {
    if (auto* t = currentTab()) {
        if (t->view) t->view->clearHighlights();
    }
    if (m_continuousView) m_continuousView->clearHighlights();
}

// ── Tab switching / closing ───────────────────────────────────────────────────

void MainWindow::onTabChanged(int) {
    hideNotePopup();
    m_selPage = -1;
    m_selIdx = -1;
    auto* _t = currentTab();
    if (_t && _t->view) _t->view->clearSelectedAnnot();
    // Reset search state when switching tabs
    m_searchResults.clear();
    m_searchCurrentIdx = -1;
    if (m_findBar) m_findBar->reset();
    clearAllSearchHighlights();
    auto* t = currentTab();
    if (t) {
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
        m_docTabs->removeTab(idx);
        delete t->view;
        t->view = nullptr;
        if (m_openDocs.isEmpty()) {
            m_docTabs->addTab(new PdfView(m_docTabs), "Welcome");
            m_thumbPanel->clearThumbnails();
            setWindowTitle("TorReader PDF");
        }
        if (t->annotMgr) t->annotMgr->stopScan();
        // Destroy renderer on a background thread so ~PdfRenderer()::waitForDone()
        // does not block the main thread while waiting for any in-flight PDFium render.
        auto* closeJob = new QFutureWatcher<void>(qApp);
        QObject::connect(closeJob, &QFutureWatcher<void>::finished,
                         closeJob, &QObject::deleteLater);
        QFuture<void> scanFut = t->annotScanFuture;
        closeJob->setFuture(QtConcurrent::run([t, workingTmp, scanFut]() mutable {
            if (scanFut.isValid()) scanFut.waitForFinished();
            delete t;
            if (!workingTmp.isEmpty()) QFile::remove(workingTmp);
        }));
        return;
    }
}

// ── Page navigation ───────────────────────────────────────────────────────────

void MainWindow::onPageChanged(int pageIndex) {
    m_lastNavMs = QDateTime::currentMSecsSinceEpoch();
    hideNotePopup();
    m_selPage = -1;
    m_selIdx = -1;
    auto* _t = currentTab();
    if (_t && _t->view) _t->view->clearSelectedAnnot();
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
        refreshAnnotVisuals(t, pageIndex);

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

        // Show placeholder from cache immediately — NO render wait
        QImage cached = t->renderer->bestCachedForPage(pageIndex);
        QSizeF sz = t->doc->pageSize(pageIndex);
        if (!cached.isNull()) {
            t->view->setPage(pageIndex, cached, sz);
        } else {
            // Show pending page immediately. Old image is cleared; placeholder thumbnail
            // is shown while full render loads (same pattern as Okular/Acrobat).
            t->view->setPendingPage(pageIndex, sz);
            QImage thumb = m_thumbPanel->thumbnailForPage(pageIndex);
            if (!thumb.isNull()) {
                qDebug() << "[perf] placeholder feed thumb page=" << pageIndex;
                t->view->setPlaceholder(thumb);
            } else {
                qDebug() << "[perf] placeholder feed blank page=" << pageIndex;
            }
        }
        t->renderer->cancelPending();
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
    if (!GoogleAuth::checkAndRequest(this)) {
        statusBar()->showMessage("Translation requires consent — select text again and click Enable.", 5000);
        return;
    }
    auto* t = currentTab();
    if (!t || !t->doc->isOpen()) return;

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
