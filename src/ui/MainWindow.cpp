#include "MainWindow.h"
#include <QDebug>
#include "PdfView.h"
#include "PdfGpuView.h"
#include "ThumbnailPanel.h"
#include "ContinuousView.h"
#include "MergeDialog.h"
#include "SignDialog.h"
#include "AboutDialog.h"
#include "PrintDialog.h"
#include "core/PdfDocument.h"
#include "core/PdfRenderer.h"
#include "core/PdfEditor.h"
#include "core/TextSearch.h"
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
    new QShortcut(QKeySequence::Undo, this, [this]{ doUndo(); });
    new QShortcut(QKeySequence::Redo, this, [this]{ doRedo(); });
    new QShortcut(QKeySequence(Qt::Key_Escape), this, [this]{
        if (auto* t = currentTab()) if (t->view) t->view->setTool(PdfGpuView::ViewTool::Pan);
        m_selPage = -1; m_selIdx = -1;
    });
    new QShortcut(QKeySequence(Qt::Key_Delete), this, [this]{
        if (m_selPage >= 0 && m_selIdx >= 0) deleteSelectedAnnot(m_selPage, m_selIdx);
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
    connect(m_thumbPanel, &ThumbnailPanel::commentActivated, this, &MainWindow::onPageChanged);
    connect(m_thumbPanel, &ThumbnailPanel::annotStyleChanged, this,
            [this](QColor color, double width, bool fill){
        m_annotStyle.strokeColor = color;
        m_annotStyle.strokeWidth = static_cast<float>(width);
        m_annotStyle.opacity     = 1.0f;
        m_annotStyle.fillColor   = fill ? color : QColor(Qt::transparent);
    });

    // Text search
    connect(m_thumbPanel, &ThumbnailPanel::searchRequested,
            this, [this](const QString& query) {
        auto* t = currentTab();
        if (!t || !t->doc->isOpen() || query.trimmed().isEmpty()) return;
        m_thumbPanel->clearSearchResults();
        m_textSearch->cancel();
        m_textSearch->search(t->doc.get(), query, Qt::CaseInsensitive);
    });
    connect(m_textSearch, &TextSearch::found,
            m_thumbPanel, &ThumbnailPanel::addSearchResult);
    connect(m_thumbPanel, &ThumbnailPanel::searchResultSelected,
            this, [this](int page, QRectF rect) {
        onPageChanged(page);
        if (m_continuousMode && m_continuousView)
            m_continuousView->scrollToPage(page);
        if (auto* t = currentTab())
            if (t->view) t->view->setHighlights({rect});
    });

    // Continuous view page/zoom sync
    connect(m_continuousView, &ContinuousView::pageChanged,
            this, [this](int page) {
        if (auto* t = currentTab()) {
            t->currentPage = page;
            m_thumbPanel->setCurrentPage(page);
            statusBar()->showMessage(
                QString("Page %1 / %2").arg(page + 1).arg(t->doc->pageCount()), 2000);
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
        "Ctrl+Scroll: Zoom  ·  Ctrl+Drag: Translate  "
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
    tb->setIconSize({16, 16});
    tb->setToolButtonStyle(Qt::ToolButtonTextOnly);

    // File
    tb->addAction("Open",    this, &MainWindow::onOpenFile)->setShortcut(QKeySequence::Open);
    tb->addAction("Save",    this, &MainWindow::onSaveFile)->setShortcut(QKeySequence::Save);
    tb->addAction("Save As", this, &MainWindow::onSaveAsFile)->setShortcut(QKeySequence::SaveAs);
    tb->addSeparator();

    // Edit
    tb->addAction("Merge PDFs", this, &MainWindow::onMergeFiles)->setShortcut(QKeySequence("Ctrl+M"));
    tb->addAction("Extract All", this, &MainWindow::onExtractAll)->setShortcut(QKeySequence("Ctrl+Shift+E"));
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
    tb->addAction("Print", this, &MainWindow::onPrintFile)->setShortcut(QKeySequence::Print);
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
        "Enable Google Translate — hold Ctrl and drag over text to select & translate\n"
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
            "<tr><td><b>Ctrl+Drag</b></td><td>Select text to translate</td></tr>"
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

    // 1. Flush the in-memory document (page edits + annotations) to a temp file
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

    // 3. Overwrite the original with the temp.
    QFile::remove(original);
    if (!QFile::exists(original) && QFile::copy(tmp, original)) {
        QFile::remove(tmp);
        loadTabFile(t, original);                          // reopen from the saved original
        if (prevFile != original) QFile::remove(prevFile); // discard any page-edit working copy
        t->dirty = false;
        updateTabDirty(t);
        statusBar()->showMessage("Saved: " + QFileInfo(original).fileName(), 3000);
        return;
    }

    // 4. Couldn't overwrite (locked by another program / read-only) — reopen from the
    //    temp so the tab keeps its edits, then offer Save As.
    loadTabFile(t, tmp);
    QMessageBox::information(this, "Save",
        "Couldn't overwrite the original file — it may be open in another "
        "program or read-only.\nPlease choose a new location.");
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
        t->annotMgr->setDocument(t->doc->raw(), tmp);
        if (!t->annotMgr->saveDocument()) {
            QMessageBox::warning(this, "Save As", "Could not write to:\n" + dest);
            QFile::remove(tmp);
            return;
        }
    }

    QFile::remove(dest);
    if (!QFile::copy(tmp, dest)) {
        QMessageBox::warning(this, "Save As", "Could not write to:\n" + dest);
        QFile::remove(tmp);
        return;
    }
    QFile::remove(tmp);

    t->originalPath = dest;      // the tab now belongs to the new file
    loadTabFile(t, dest);
    if (prevFile != srcOriginal && prevFile != dest)
        QFile::remove(prevFile); // discard a page-edit working copy, never the source original
    t->dirty = false;
    updateTabDirty(t);
    statusBar()->showMessage("Saved as: " + QFileInfo(dest).fileName(), 4000);
}

void MainWindow::deleteSelectedAnnot(int page, int index) {
    auto* t = currentTab();
    if (!t || !t->annotMgr || index < 0) return;
    AnnotSnapshot snap = t->annotMgr->snapshotAnnot(page, index);
    if (!t->annotMgr->removeAnnot(page, index)) return;
    DrawAction da; da.kind = DrawAction::Delete; da.page = page; da.snap = snap;
    m_undoStack.append(da); m_redoStack.clear();
    t->dirty = true; updateTabDirty(t);
    if (t->view) t->view->clearSelectedAnnot();
    m_selPage = -1; m_selIdx = -1;
    if (t->renderer) { t->renderer->setTileCache(nullptr); t->renderer->clearCache(); t->renderer->requestPage(page, t->zoom); }
    t->annotCacheValid = false;
}

void MainWindow::editSelectedAnnot(int page, int index) {
    auto* t = currentTab();
    if (!t || !t->annotMgr || index < 0) return;
    auto list = t->annotMgr->loadPage(page);
    if (index >= list.size()) return;
    NoteInputDialog dlg(list[index].text, this, /*singleLine=*/(list[index].type == QLatin1String("FreeText")));
    dlg.setWindowTitle("Edit text");
    if (dlg.exec() != QDialog::Accepted) return;
    QString newText = dlg.text();
    AnnotSnapshot s = t->annotMgr->snapshotAnnot(page, index);
    if (!s.valid) return;
    s.contents = newText;
    if (s.subtype == FPDF_ANNOT_FREETEXT) {
        double w = qMax(24.0, static_cast<double>(newText.length()) * 5.5 + 6.0);
        s.rr = s.rl + static_cast<float>(w);
    }
    if (!t->annotMgr->removeAnnot(page, index)) return;
    t->annotMgr->addSnapshot(page, s);
    t->dirty = true; updateTabDirty(t);
    if (t->view) t->view->clearSelectedAnnot();
    m_selPage = -1; m_selIdx = -1;
    if (t->renderer) { t->renderer->setTileCache(nullptr); t->renderer->clearCache(); t->renderer->requestPage(page, t->zoom); }
    t->annotCacheValid = false;
}

void MainWindow::doUndo() {
    if (m_undoStack.isEmpty()) return;
    auto* t = currentTab();
    if (!t || !t->annotMgr) return;
    DrawAction da = m_undoStack.takeLast();
    bool ok = false;
    if (da.kind == DrawAction::Add) {
        int cnt = t->annotMgr->loadPage(da.page).size();
        ok = (cnt > 0) && t->annotMgr->removeAnnot(da.page, cnt - 1);
    } else if (da.kind == DrawAction::Move) {
        int cnt = t->annotMgr->loadPage(da.page).size();
        if (cnt > 0) t->annotMgr->removeAnnot(da.page, cnt - 1);
        ok = t->annotMgr->addSnapshot(da.page, da.snap);
    } else if (da.kind == DrawAction::NoteMove) {
        int cnt = t->annotMgr->loadPage(da.page).size();
        if (cnt > 0) ok = t->annotMgr->moveNote(da.page, cnt - 1, -da.a.x(), -da.a.y());
    } else {
        ok = t->annotMgr->addSnapshot(da.page, da.snap);
    }
    if (!ok) return;
    m_redoStack.append(da);
    t->dirty = true; updateTabDirty(t);
    if (t->view) t->view->clearSelectedAnnot();
    if (t->renderer) { t->renderer->setTileCache(nullptr); t->renderer->clearCache(); }
    if (t->currentPage != da.page) { t->currentPage = da.page; m_thumbPanel->setCurrentPage(da.page); }
    if (t->view) { t->view->setPendingPage(t->currentPage, t->doc->pageSize(t->currentPage)); t->view->beginLoading(); }
    if (t->renderer) t->renderer->requestPage(da.page, t->zoom);
    t->annotCacheValid = false;
}

void MainWindow::doRedo() {
    if (m_redoStack.isEmpty()) return;
    auto* t = currentTab();
    if (!t || !t->annotMgr || !t->annotLayer) return;
    DrawAction da = m_redoStack.takeLast();
    if (da.kind == DrawAction::Add) {
        if (da.isNote)
            t->annotMgr->createPopupNote(da.page, da.a, da.text, da.author);
        else if (da.isText)
            t->annotMgr->createInlineNote(da.page,
                (da.b.isNull() ? QRectF(da.a.x(), da.a.y(), 160, 20) : QRectF(da.a, da.b)),
                da.text, da.author, da.textBg, da.style.strokeColor, da.style.fontSize);
        else
            t->annotLayer->commitAnnotation(da.page, da.tool, da.style, da.a, da.b, {});
    } else if (da.kind == DrawAction::Move) {
        int cnt = t->annotMgr->loadPage(da.page).size();
        if (cnt > 0) t->annotMgr->removeAnnot(da.page, cnt - 1);
        t->annotMgr->addSnapshot(da.page, da.snapAfter);
    } else if (da.kind == DrawAction::NoteMove) {
        int cnt = t->annotMgr->loadPage(da.page).size();
        if (cnt > 0) t->annotMgr->moveNote(da.page, cnt - 1, da.a.x(), da.a.y());
    } else {
        int cnt = t->annotMgr->loadPage(da.page).size();
        if (cnt > 0) t->annotMgr->removeAnnot(da.page, cnt - 1);
    }
    m_undoStack.append(da);
    t->dirty = true; updateTabDirty(t);
    if (t->renderer) { t->renderer->setTileCache(nullptr); t->renderer->clearCache(); }
    if (t->currentPage != da.page) { t->currentPage = da.page; m_thumbPanel->setCurrentPage(da.page); }
    if (t->view) { t->view->setPendingPage(t->currentPage, t->doc->pageSize(t->currentPage)); t->view->beginLoading(); }
    if (t->renderer) t->renderer->requestPage(da.page, t->zoom);
    t->annotCacheValid = false;
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
            if (t->renderer) { t->renderer->setTileCache(nullptr); t->renderer->clearCache(); t->renderer->requestPage(page, t->zoom); }
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
    SignParams sp = m_pendSp;
    sp.pageIndex = m_pendPage;
    sp.rectPt = rect;
    m_pendActive = false; m_pendPage = -1;
    if (m_finalizeSigAct) m_finalizeSigAct->setVisible(false);
    if (m_cancelSigAct)   m_cancelSigAct->setVisible(false);
    statusBar()->clearMessage();
    if (t->renderer) { t->renderer->setTileCache(nullptr); t->renderer->clearCache(); t->renderer->requestPage(t->currentPage, t->zoom); }
    performSign(t, sp);
}

void MainWindow::onCancelSignature() {
    auto* t = currentTab();
    if (t && m_pendActive) {
        int idx = -1;
        t->annotMgr->findSignatureDraftRect(m_pendPage, &idx);
        if (idx >= 0) t->annotMgr->removeAnnot(m_pendPage, idx);
        if (t->renderer) { t->renderer->setTileCache(nullptr); t->renderer->clearCache(); t->renderer->requestPage(t->currentPage, t->zoom); }
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
void MainWindow::loadTabFile(DocTab* t, const QString& path) {
    t->doc->close();
    t->renderer->setTileCache(nullptr);
    if (t->thumbPool) t->thumbPool->close();

    t->doc->open(path);
    t->annotCacheValid = false;
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

    t->renderer->requestPage(t->currentPage, t->zoom);
    if (t == currentTab()) {
        // File vừa được mở lại (edit/save) trên cùng con trỏ doc → ép sidebar
        // dựng lại thumbnail + bookmark, nếu không panel giữ nội dung cũ.
        syncSidebarToTab(m_openDocs.indexOf(t), /*forceRebuild=*/true);
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

    // The previous working copy (if this tab was already dirty) is replaced.
    QString oldWorking = t->dirty ? t->doc->filePath() : QString();

    QString working = makeTmpPath(t->originalPath);
    QFile::remove(working);
    if (!QFile::rename(tmpPath, working) && QFile::exists(tmpPath)) {
        QFile::copy(tmpPath, working);
        QFile::remove(tmpPath);
    }

    loadTabFile(t, working);

    if (!oldWorking.isEmpty() && oldWorking != working)
        QFile::remove(oldWorking);

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
    connect(tab->renderer.get(), &PdfRenderer::tileReady,
            tab->view, [tab](int page, double scale, int col, int row, QImage img) {
        if (tab->view) tab->view->setTile(page, scale, col, row, img);
    });
    connect(tab->view, &PdfGpuView::tilesNeeded,
            tab->renderer.get(), &PdfRenderer::requestTiles);

    // ── Annotation signals ────────────────────────────────────────────────────
    connect(tab->view, &PdfGpuView::shapeCommitRequested,
            this, [this, tab](int pageIdx, AnnotTool tool, QPointF start, QPointF end) {
        if (!tab->annotLayer) return;
        qDebug().noquote() << "[markup] signal=shapeCommitRequested page=" << pageIdx
                 << "tool=" << static_cast<int>(tool)
                 << "start=(" << start.x() << "," << start.y() << ")"
                 << "end=(" << end.x() << "," << end.y() << ")";
        AnnotStyle style = m_annotStyle;
        tab->annotLayer->commitAnnotation(pageIdx, tool, style, start, end, {});
        {   int _n = tab->annotMgr ? tab->annotMgr->loadPage(pageIdx).size() : -1;
            qDebug().noquote() << "[markup] committed page=" << pageIdx << "annotCountAfter=" << _n; }
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
        { DrawAction da; da.page = pageIdx; da.isText = false;
          da.tool = tool; da.style = style; da.a = start; da.b = end;
          m_undoStack.append(da); m_redoStack.clear(); }
        qDebug().noquote() << "[markup] request re-render page=" << pageIdx << "zoom=" << tab->zoom;
        if (tab->renderer) {
            tab->renderer->setTileCache(nullptr);
            tab->annotCacheValid = false;
            tab->renderer->clearCache();
            tab->renderer->requestPage(pageIdx, tab->zoom);
        }
        tab->dirty = true;
        updateTabDirty(tab);
    });

    connect(tab->view, &PdfGpuView::noteRequested,
            this, [this, tab](int pageIndex, QPointF pdfPoint) {
        if (!tab) return;
        qDebug().noquote() << "[markup] signal=noteRequested page=" << pageIndex
                 << "point=(" << pdfPoint.x() << "," << pdfPoint.y() << ")";
        NoteInputDialog dlg({}, this);
        if (dlg.exec() != QDialog::Accepted) return;
        tab->annotMgr->createPopupNote(pageIndex, pdfPoint, dlg.text(), dlg.author());
        {   int _n = tab->annotMgr ? tab->annotMgr->loadPage(pageIndex).size() : -1;
            qDebug().noquote() << "[markup] committed page=" << pageIndex << "annotCountAfter=" << _n; }
        { DrawAction da; da.page = pageIndex; da.isNote = true; da.a = pdfPoint;
          da.text = dlg.text(); da.author = dlg.author();
          m_undoStack.append(da); m_redoStack.clear(); }
        qDebug().noquote() << "[markup] request re-render page=" << pageIndex << "zoom=" << tab->zoom;
        if (tab->renderer) {
            tab->renderer->setTileCache(nullptr);
            tab->annotCacheValid = false;
            tab->renderer->clearCache();
            tab->renderer->requestPage(pageIndex, tab->zoom);
        }
        tab->dirty = true;
        updateTabDirty(tab);
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
        {   int _n = tab->annotMgr ? tab->annotMgr->loadPage(page).size() : -1;
            qDebug().noquote() << "[markup] committed page=" << page << "annotCountAfter=" << _n; }
        { DrawAction da; da.page = page; da.isText = true;
          da.a = r.topLeft(); da.b = r.bottomRight();
          da.text = txt; da.author = dlg.author(); da.textBg = false; da.style = m_annotStyle;
          m_undoStack.append(da); m_redoStack.clear(); }
        qDebug().noquote() << "[markup] request re-render page=" << page << "zoom=" << tab->zoom;
        if (tab->renderer) {
            tab->renderer->setTileCache(nullptr);
            tab->annotCacheValid = false;
            tab->renderer->clearCache();
            tab->renderer->requestPage(page, tab->zoom);
        }
    });

    connect(tab->view, &PdfGpuView::annotationPickRequested, this,
            [this, tab](int page, QPointF pt) {
        if (!tab->annotMgr) return;
        auto list = tab->annotMgr->loadPage(page);
        m_selPage = -1; m_selIdx = -1;
        for (int i = list.size() - 1; i >= 0; --i) {
            if (list[i].type != QLatin1String("Widget") && list[i].rect.normalized().contains(pt)) {
                m_selPage = page; m_selIdx = i;
                tab->view->setSelectedAnnot(list[i].rect.normalized());
                if (!list[i].text.isEmpty()) {
                    showNotePopup(list[i].text, list[i].author);
                }
                return;
            }
        }
        tab->view->clearSelectedAnnot();
        hideNotePopup();
    });

    connect(tab->view, &PdfGpuView::annotationContextRequested, this,
            [this, tab](int page, QPointF pt, QPoint gpos) {
        if (!tab->annotMgr) return;
        auto list = tab->annotMgr->loadPage(page);
        int idx = -1;
        for (int i = list.size() - 1; i >= 0; --i)
            if (list[i].type != QLatin1String("Widget") && list[i].rect.normalized().contains(pt)) { idx = i; break; }
        if (idx < 0) { tab->view->clearSelectedAnnot(); return; }
        m_selPage = page; m_selIdx = idx;
        tab->view->setSelectedAnnot(list[idx].rect.normalized());
        QMenu menu(this);
        const bool canEditText = (list[idx].type == QLatin1String("FreeText") || list[idx].type == QLatin1String("Note"));
        QAction* editAct = canEditText ? menu.addAction("Edit text…") : nullptr;
        QAction* propAct = menu.addAction("Properties…");
        QAction* del     = menu.addAction("Delete");
        QAction* chosen  = menu.exec(gpos);
        if (chosen == del) {
            deleteSelectedAnnot(page, idx);
        } else if (editAct && chosen == editAct) {
            editSelectedAnnot(page, idx);
        } else if (chosen == propAct) {
            QDialog dlg(this);
            dlg.setWindowTitle("Markup properties");
            auto* form = new QFormLayout(&dlg);
            QString realType; QColor realColor = m_annotStyle.strokeColor; float realWidth = m_annotStyle.strokeWidth; float realFont = 11.0f;
            tab->annotMgr->getAnnotEditState(page, idx, realType, realColor, realWidth, realFont);
            QColor curColor = realColor;
            auto* colorBtn = new QPushButton("Choose…");
            colorBtn->setStyleSheet(QString("background:%1;color:white;").arg(curColor.name()));
            connect(colorBtn, &QPushButton::clicked, &dlg, [&]{
                QColor c = QColorDialog::getColor(curColor, &dlg, "Markup color");
                if (c.isValid()) { curColor = c; colorBtn->setStyleSheet(QString("background:%1;color:white;").arg(c.name())); }
            });
            form->addRow("Color", colorBtn);
            const bool isText = (list[idx].type == QLatin1String("FreeText"));
            QSpinBox* widthSpin = nullptr;
            QCheckBox* fillChk = nullptr;
            QSpinBox* fontSpin = nullptr;
            if (isText) {
                fontSpin = new QSpinBox; fontSpin->setRange(6, 96);
                fontSpin->setValue(qRound(realFont));
                form->addRow("Font size (pt)", fontSpin);
            } else {
                widthSpin = new QSpinBox; widthSpin->setRange(1, 24);
                widthSpin->setValue(qMax(1, qRound(realWidth)));
                fillChk = new QCheckBox;
                form->addRow("Width (px)", widthSpin);
                form->addRow("Fill", fillChk);
            }
            auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
            form->addRow(bb);
            connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
            connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
            if (dlg.exec() != QDialog::Accepted) return;
            if (isText) {
                if (tab->annotMgr->rebuildTextNote(page, idx, curColor,
                                                   static_cast<float>(fontSpin->value()))) {
                    tab->dirty = true; updateTabDirty(tab);
                    if (tab->renderer) { tab->renderer->setTileCache(nullptr); tab->renderer->clearCache(); tab->renderer->requestPage(page, tab->zoom); }
                    tab->annotCacheValid = false;
                    m_annotStyle.strokeColor = curColor;
                    m_annotStyle.fontSize    = static_cast<float>(fontSpin->value());
                    m_selPage = -1; m_selIdx = -1;
                    tab->view->clearSelectedAnnot();
                }
            } else {
                if (tab->annotMgr->setAnnotStyle(page, idx, curColor,
                                                 static_cast<float>(widthSpin->value()),
                                                 fillChk->isChecked())) {
                    tab->dirty = true; updateTabDirty(tab);
                    if (tab->renderer) { tab->renderer->setTileCache(nullptr); tab->renderer->clearCache(); tab->renderer->requestPage(page, tab->zoom); }
                    tab->annotCacheValid = false;
                    m_annotStyle.strokeColor = curColor;
                    m_annotStyle.strokeWidth = static_cast<float>(widthSpin->value());
                }
            }
        }
    });

    connect(tab->view, &PdfGpuView::annotationMoveRequested, this,
            [this, tab](int page, double dx, double dy) {
        if (m_selPage != page || m_selIdx < 0 || !tab->annotMgr) return;
        // Note/FreeText: use moveNote (handles both annotation + page objects, no undo)
        {
            auto list = tab->annotMgr->loadPage(page);
            if (m_selIdx >= 0 && m_selIdx < list.size()) {
                auto t = list[m_selIdx].type;
                if (t == "FreeText" || t == "Note") {
                    if (tab->annotMgr->moveNote(page, m_selIdx, dx, dy)) {
                        auto nl = tab->annotMgr->loadPage(page);
                        m_selIdx = nl.size() - 1;
                        if (m_selIdx >= 0 && m_selIdx < nl.size())
                            tab->view->setSelectedAnnot(nl[m_selIdx].rect.normalized());
                        tab->dirty = true; updateTabDirty(tab);
                        if (tab->renderer) { tab->renderer->setTileCache(nullptr); tab->renderer->clearCache(); tab->renderer->requestPage(page, tab->zoom); }
                        tab->annotCacheValid = false;
                        DrawAction da; da.kind = DrawAction::NoteMove; da.page = page;
                        da.a = QPointF(dx, dy);
                        m_undoStack.append(da); m_redoStack.clear();
                    }
                    return;   // skip snapshot path
                }
            }
        }
        AnnotSnapshot s = tab->annotMgr->snapshotAnnot(page, m_selIdx);
        if (!s.valid) return;
        qDebug().noquote() << "[markup] signal=annotationMoveRequested page=" << page
                 << "dx=" << dx << "dy=" << dy;
        // Convert display-space delta (dx=rightward, dy=upward) to unrotated PDF delta
        double dxU = dx, dyU = dy;
        {
            QMutexLocker lock(&s_pdfiumMutex);
            FPDF_PAGE mp = FPDF_LoadPage(tab->doc->raw(), page);
            if (mp) {
                switch (FPDFPage_GetRotation(mp)) {
                    case 1: dxU = -dy; dyU = dx;  break;
                    case 2: dxU = -dx; dyU = -dy; break;
                    case 3: dxU =  dy; dyU = -dx; break;
                    default: /* rot 0 */ break;
                }
                FPDF_ClosePage(mp);
            }
        }
        AnnotSnapshot before = s;
        s.rl += dxU; s.rr += dxU; s.rt += dyU; s.rb += dyU;
        for (auto& stroke : s.ink)
            for (auto& p : stroke) p = QPointF(p.x() + dxU, p.y() + dyU);
        if (!tab->annotMgr->removeAnnot(page, m_selIdx)) return;
        tab->annotMgr->addSnapshot(page, s);
        {   int _n = tab->annotMgr ? tab->annotMgr->loadPage(page).size() : -1;
            qDebug().noquote() << "[markup] committed page=" << page << "annotCountAfter=" << _n; }
        { DrawAction da; da.kind = DrawAction::Move; da.page = page; da.snap = before; da.snapAfter = s; m_undoStack.append(da); m_redoStack.clear(); }
        auto list = tab->annotMgr->loadPage(page);
        m_selIdx = list.size() - 1;
        if (m_selIdx >= 0 && m_selIdx < list.size())
            tab->view->setSelectedAnnot(list[m_selIdx].rect.normalized());
        tab->dirty = true; updateTabDirty(tab);
        qDebug().noquote() << "[markup] request re-render page=" << page << "zoom=" << tab->zoom;
        if (tab->renderer) { tab->renderer->setTileCache(nullptr); tab->renderer->clearCache(); tab->renderer->requestPage(page, tab->zoom); }
        tab->annotCacheValid = false;
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

        // REMOVED: FormibDoc is NEVER used in practice.
        // Evidence:
        //   (a) FormibPDF branch in PageRenderTask::run() only activates when
        //       scaleFactor <= 0.16 (isPreviewScale), but the main A1-fit view
        //       is ~0.6, so it never enters.
        //   (b) Thumbnails go through ThumbnailRenderPool (mutex-free PDFium),
        //       not via PdfRenderer, so no thumbnail path hits FormibPDF either.
        //   (c) FormibGate::disabled never turns on because recordReject() is
        //       never called, so the m_formibDoc teardown path never runs.
        //   Consequence: QFile::readAll() + formibpdf_open() on a 61 MB file
        //   wasted ~hundreds MB RAM + seconds of CPU on every open.
        //   To re-enable: restore the QFutureWatcher<FormibDocPtr> + QtConcurrent
        //   block below, then verify the scale threshold triggers in your use case.
        //   See also: PdfRenderer.cpp ~line 63 (isPreviewScale check).

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
                // Clear the "Loading…" status from onPageChanged
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
        tab->renderer->requestPage(0, tab->zoom);

        if (tab == currentTab()) {
            syncSidebarToTab(tabIdx);
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

// ── Tab switching / closing ───────────────────────────────────────────────────

void MainWindow::onTabChanged(int) {
    hideNotePopup();
    m_selPage = -1;
    m_selIdx = -1;
    auto* _t = currentTab();
    if (_t && _t->view) _t->view->clearSelectedAnnot();
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
        {
            QString nm = QFileInfo(t->originalPath.isEmpty()
                                   ? t->doc->filePath() : t->originalPath).fileName();
            setWindowTitle("TorReader PDF — " + QString(t->dirty ? "● " : "") + nm);
        }
        statusBar()->showMessage(
            QString("Page %1 / %2").arg(t->currentPage + 1).arg(t->doc->pageCount()), 2000);
        if (m_zoomEdit)
            m_zoomEdit->setText(QString::number(qRound(t->zoom * 100)) + "%");
        if (m_continuousMode && m_continuousView && t->doc->isOpen()) {
            auto pgSz = t->doc->pageSize(t->currentPage);
            double renderScale = PdfRenderer::kFullRenderMaxPx
                / qMax(qMax(pgSz.width(), pgSz.height()), 1.0);
            qDebug() << "[perf] cont inherit renderScale from single ="
                     << renderScale << "(tabZoom=" << t->zoom << ")";
            m_continuousView->setZoom(renderScale);
            m_continuousView->setDocument(t->doc.get(), t->renderer.get());
        }
    } else {
        syncSidebarToTab(-1);
        setWindowTitle("TorReader PDF");
        statusBar()->showMessage("TorReader PDF  ·  Open a PDF to get started");
        if (m_continuousMode && m_continuousView)
            m_continuousView->clearDocument();
    }
}

void MainWindow::onCommentsRequested() {
    auto* t = currentTab();
    if (!t || !t->annotMgr || !t->doc || !t->doc->isOpen()) return;
    if (t->annotCacheValid) {
        m_thumbPanel->setComments(t->annotCache);
        return;
    }

    int pageCount = t->doc->pageCount();
    auto* mgr = t->annotMgr.get();
    auto* watcher = new QFutureWatcher<QList<AnnotInfo>>(this);
    connect(watcher, &QFutureWatcher<QList<AnnotInfo>>::finished, this,
            [this, watcher, t]() {
        watcher->deleteLater();
        if (m_openDocs.indexOf(t) < 0) return;
        auto comments = watcher->result();
        t->annotCache = comments;
        t->annotCacheValid = true;
        if (t == currentTab())
            m_thumbPanel->setComments(comments);
    });
    watcher->setFuture(QtConcurrent::run([mgr, pageCount]() {
        return mgr->loadAll(pageCount);
    }));
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
        // Destroy renderer on a background thread so ~PdfRenderer()::waitForDone()
        // does not block the main thread while waiting for any in-flight PDFium render.
        auto* closeJob = new QFutureWatcher<void>(qApp);
        QObject::connect(closeJob, &QFutureWatcher<void>::finished,
                         closeJob, &QObject::deleteLater);
        closeJob->setFuture(QtConcurrent::run([t, workingTmp]() {
            delete t;
            if (!workingTmp.isEmpty()) QFile::remove(workingTmp);
        }));
        return;
    }
}

// ── Page navigation ───────────────────────────────────────────────────────────

void MainWindow::onPageChanged(int pageIndex) {
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
