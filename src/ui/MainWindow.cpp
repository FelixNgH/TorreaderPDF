#include "MainWindow.h"
#include <QDebug>
#include "PdfView.h"
#include "PdfGpuView.h"
#include "ThumbnailPanel.h"
#include "ContinuousView.h"
#include "MergeDialog.h"
#ifdef TORREADER_ENABLE_SIGNER
#include "SignDialog.h"
#endif
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
#include "NotificationBar.h"

#include <fpdf_text.h>
#include <fpdf_doc.h>
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
QToolButton:hover                       { background:#3E3E42; }
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
QToolButton                             { color:#374151; padding:2px 6px; border-radius:3px; border:none; background:transparent; }
QToolButton:hover                       { background:#E5E7EB; }
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
    connect(m_thumbPanel, &ThumbnailPanel::pageClicked,
            this, &MainWindow::onPageChanged);
    connect(m_thumbPanel, &ThumbnailPanel::pageContextMenu,
            this, &MainWindow::showThumbnailContextMenu);

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

    // ── Notification bar ─────────────────────────────────────────────────────
    m_notifBar = new NotificationBar(this);
    m_notifBar->setContent(
        "TorReader PDF",
        "Download the latest version at torreader.cloud");
    const QString kNotifId = "download-latest";
    if (!NotificationBar::wasDismissed(kNotifId)) {
        connect(m_notifBar, &NotificationBar::dismissed, [kNotifId]() {
            NotificationBar::markDismissed(kNotifId);
        });
        QTimer::singleShot(2000, this, [this]() {
            m_notifBar->showNotification();
            repositionNotifBar();
        });
    }
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

#ifdef TORREADER_ENABLE_SIGNER
    // Sign
    auto* signAct = tb->addAction("Sign PDF…", this, &MainWindow::onSignPdf);
    signAct->setToolTip("Digitally sign this document with a certificate (.pfx/.p12)");
    tb->addSeparator();
#endif

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
            if (t && t->doc->isOpen())
                m_continuousView->setDocument(t->doc.get(), t->renderer.get());
        } else {
            m_docTabs->setMinimumHeight(0);
            m_docTabs->setMaximumHeight(QWIDGETSIZE_MAX);
            m_continuousView->hide();
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

void MainWindow::onSaveFile() {
    auto* t = currentTab();
    if (!t) return;
    if (!t->dirty) {
        statusBar()->showMessage("No unsaved changes", 2000);
        return;
    }
    const QString working  = t->doc->filePath();
    const QString original = t->originalPath;

    // Overwrite the original in place. If it's locked by another program or is
    // read-only, the remove/copy fails → fall back to Save As.
    QFile::remove(original);
    if (QFile::exists(original) || !QFile::copy(working, original)) {
        QMessageBox::information(this, "Save",
            "Couldn't overwrite the original file — it may be open in another "
            "program or read-only.\nPlease choose a new location.");
        onSaveAsFile();
        return;
    }

    loadTabFile(t, original);   // reopen from the freshly-saved original
    QFile::remove(working);     // discard the working copy
    t->dirty = false;
    updateTabDirty(t);
    statusBar()->showMessage("Saved: " + QFileInfo(original).fileName(), 3000);
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

    const QString working = t->doc->filePath();
    QFile::remove(dest);
    if (!QFile::copy(working, dest)) {
        QMessageBox::warning(this, "Save As", "Could not write to:\n" + dest);
        return;
    }

    const QString oldWorking = t->dirty ? working : QString();
    t->originalPath = dest;      // the tab now belongs to the new file
    loadTabFile(t, dest);
    if (!oldWorking.isEmpty() && oldWorking != dest)
        QFile::remove(oldWorking);
    t->dirty = false;
    updateTabDirty(t);
    statusBar()->showMessage("Saved as: " + QFileInfo(dest).fileName(), 4000);
}

void MainWindow::onMergeFiles() {
    MergeDialog dlg(m_editor.get(), this);
    dlg.exec();
}

#ifdef TORREADER_ENABLE_SIGNER
void MainWindow::onSignPdf() {
    auto* t = currentTab();
    if (!t || !t->doc->isOpen()) {
        QMessageBox::information(this, "Sign PDF", "Please open a PDF file first.");
        return;
    }

    SignDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;

    QString outPath = QFileDialog::getSaveFileName(
        this, "Save Signed PDF As",
        QFileInfo(t->doc->filePath()).completeBaseName() + "_signed.pdf",
        "PDF Files (*.pdf)");
    if (outPath.isEmpty()) return;
    if (!outPath.endsWith(".pdf", Qt::CaseInsensitive)) outPath += ".pdf";

    SignParams sp = dlg.params();
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
#endif // TORREADER_ENABLE_SIGNER

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

    if (t->thumbPool && !t->thumbPool->open(path))
        t->thumbPool.reset(); // fallback: thumbnail panel uses PdfRenderer

    t->tileCache = std::make_unique<TileCacheFile>();
    {
        uint64_t hash = TileCacheFile::hashFile(path);
        uint64_t sz   = static_cast<uint64_t>(QFileInfo(path).size());
        if (t->tileCache->open(path, hash, sz, t->doc->pageCount()))
            t->renderer->setTileCache(t->tileCache.get());
    }

    t->renderer->requestPage(t->currentPage, t->zoom);
    if (t == currentTab()) {
        // File vừa được mở lại (edit/save) trên cùng con trỏ doc → ép sidebar
        // dựng lại thumbnail + bookmark, nếu không panel giữ nội dung cũ.
        syncSidebarToTab(m_openDocs.indexOf(t), /*forceRebuild=*/true);
        if (m_continuousMode && m_continuousView)
            m_continuousView->setDocument(t->doc.get(), t->renderer.get());
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
        if (tab->doc->isOpen()) tab->renderer->requestPage(tab->currentPage, z);
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

        // Open FormibDoc in background — avoids blocking UI thread with f.readAll()
        // + formibpdf_open() parsing (can take several seconds for large files).
        // FormibPDF is still the primary renderer; it just starts slightly after PDFium.
        auto* fbWatcher = new QFutureWatcher<FormibDocPtr>(this);
        connect(fbWatcher, &QFutureWatcher<FormibDocPtr>::finished, this,
                [fbWatcher, tab]() {
            fbWatcher->deleteLater();
            if (tab) tab->renderer->setFormibDoc(fbWatcher->result());
        });
        fbWatcher->setFuture(QtConcurrent::run([path]() -> FormibDocPtr {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) return nullptr;
            QByteArray data = f.readAll();
            FormibDoc* raw = formibpdf_open(
                reinterpret_cast<const uint8_t*>(data.constData()),
                static_cast<uint32_t>(data.size()));
            return raw ? makeFormibDoc(raw) : nullptr;
        }));

        // Open persistent tile cache (second open = instant page load)
        tab->tileCache = std::make_unique<TileCacheFile>();
        {
            uint64_t hash = TileCacheFile::hashFile(path);
            uint64_t sz   = static_cast<uint64_t>(QFileInfo(path).size());
            if (tab->tileCache->open(path, hash, sz, tab->doc->pageCount()))
                tab->renderer->setTileCache(tab->tileCache.get());
        }

        // Open parallel thumbnail pool (4× FPDF_DOCUMENT → parallel rendering)
        tab->thumbPool = std::make_unique<ThumbnailRenderPool>(this);
        if (!tab->thumbPool->open(path))
            tab->thumbPool.reset(); // fallback: thumbnail panel uses PdfRenderer

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
        }
        tab->renderer->requestPage(0, tab->zoom);

        if (tab == currentTab()) {
            syncSidebarToTab(tabIdx);
            if (m_continuousMode && m_continuousView)
                m_continuousView->setDocument(tab->doc.get(), tab->renderer.get());
        }
    });

    watcher->setFuture(QtConcurrent::run([docPtr, path]() -> bool {
        return docPtr->open(path);
    }));
}

// ── Tab switching / closing ───────────────────────────────────────────────────

void MainWindow::onTabChanged(int) {
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
        if (m_continuousMode && m_continuousView && t->doc->isOpen())
            m_continuousView->setDocument(t->doc.get(), t->renderer.get());
    } else {
        syncSidebarToTab(-1);
        setWindowTitle("TorReader PDF");
        statusBar()->showMessage("TorReader PDF  ·  Open a PDF to get started");
        if (m_continuousMode && m_continuousView)
            m_continuousView->clearDocument();
    }
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
                QString("\"%1\" has unsaved changes.\nSave before closing?")
                    .arg(QFileInfo(t->originalPath).fileName()),
                QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                QMessageBox::Save);
            if (r == QMessageBox::Cancel) return;
            if (r == QMessageBox::Save) {
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
        t->view->beginLoading();
        if (qAbs(pageIndex - oldPage) > 2)
            t->renderer->cancelPending();
        else
            t->renderer->clearStalePending();
    }
    t->renderer->requestPage(pageIndex, t->zoom);
    t->renderer->preloadAdjacent(pageIndex, t->zoom);
    m_thumbPanel->setCurrentPage(pageIndex);

    if (m_continuousMode && m_continuousView)
        m_continuousView->scrollToPage(pageIndex);

    statusBar()->showMessage(
        QString("Page %1 / %2").arg(pageIndex + 1).arg(total), 2000);
}

void MainWindow::onZoomChanged(double scale) {
    auto* t = currentTab();
    if (!t) return;
    t->zoom = qBound(0.1, scale, 10.0);
    if (m_continuousMode && m_continuousView && m_continuousView->isVisible()) {
        m_continuousView->setZoom(t->zoom);
    } else {
        if (t->view) t->view->setZoom(t->zoom);
        if (t->doc->isOpen()) {
            t->renderer->cancelPending();
            t->renderer->requestPage(t->currentPage, t->zoom);
        }
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
    // Warn if any tab has unsaved in-memory edits.
    int dirtyCount = 0;
    for (auto* t : m_openDocs) if (t->dirty) ++dirtyCount;
    if (dirtyCount > 0) {
        auto r = QMessageBox::question(this, "Unsaved Changes",
            QString("%1 document(s) have unsaved changes.\nQuit without saving?")
                .arg(dirtyCount),
            QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        if (r == QMessageBox::Cancel) { e->ignore(); return; }
    }
    e->accept();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::resizeEvent(QResizeEvent* e) {
    QMainWindow::resizeEvent(e);
    repositionNotifBar();
}

void MainWindow::repositionNotifBar() {
    if (!m_notifBar || !m_notifBar->isVisible()) return;
    m_notifBar->adjustSize();
    QRect cw = centralWidget() ? centralWidget()->geometry() : rect();
    const int margin = 16;
    int x = cw.right() - m_notifBar->width() - margin;
    int y = cw.bottom() - m_notifBar->height() - margin;
    m_notifBar->move(x, y);
    m_notifBar->raise();
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
