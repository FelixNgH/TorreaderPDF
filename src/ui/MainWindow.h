#pragma once
#include <QMainWindow>
#include <QTabWidget>
#include <QAction>
#include <QLineEdit>
#include <QList>
#include <QFuture>
#include <memory>
#include "core/PdfDocument.h"
#include "core/PdfRenderer.h"
#include "core/TileCacheFile.h"
#include "core/ThumbnailRenderPool.h"
#include "core/TextSearch.h"
#include "core/PdfSigner.h"
#include "PdfView.h"
#include "PdfGpuView.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationLayer.h"
#include "annotations/AnnotationTypes.h"
#include "annotations/MarkupUndo.h"

class QMimeData;
class QLabel;
class PdfEditor;
class ThumbnailPanel;
class ContinuousView;
class QSplitter;
class Translator;
class TranslationPopup;
class GoogleAuth;
class UpdateChecker;
class QTimer;
class FindBar;
class VectorLayer;

struct DocTab {
    std::unique_ptr<PdfDocument>       doc;
    std::unique_ptr<PdfRenderer>       renderer;
    std::unique_ptr<AnnotationManager> annotMgr;
    std::unique_ptr<AnnotationLayer>   annotLayer;
    std::shared_ptr<TileCacheFile>        tileCache;
    std::unique_ptr<ThumbnailRenderPool>  thumbPool;
    PdfGpuView* view        = nullptr;
    int      currentPage    = 0;
    double   zoom           = 1.0;
    QMetaObject::Connection pageReadyConn;
    QMetaObject::Connection scrollConn;
    QList<AnnotInfo> annotCache;
    bool     annotCacheValid = false;
    bool     annotScanInFlight = false;  // guards concurrent full annot scans
    QFuture<void> annotScanFuture;
    QHash<int, QList<AnnotInfo>> annotPageCache;
    QHash<int, bool> overlayCapablePage;   // cached per-page overlay capability
    QHash<int, QList<AnnotVisual>> visualsCache;     // cached loadPageVisuals result per page
    QHash<int, quint32>            visualsRev;        // pageRevision at time of cache
    QHash<int, bool>               visualsHasForeign; // cached hasForeign per page
    QSet<int>        pagesNeedGenerate;     // pages needing FPDFPage_GenerateContent before save
    QString  originalPath;        // real on-disk file — Save target & tab name source
    bool     dirty = false;       // has unsaved in-memory edits (working copy != original)
    std::shared_ptr<VectorLayer> vecLayer;  // GPU vector overlay for heavy pages
    int warmingPage = -1;
    QSet<int> vecBuilding;  // pages currently building vector layer (anti-duplicate)
    QList<MarkupUndoEntry> undoStack;
    QList<MarkupUndoEntry> redoStack;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void openFile(const QString& path);

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void closeEvent(QCloseEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onOpenFile();
    void onSaveFile();
    void onSaveAsFile();
    void onMergeFiles();
    void onSignPdf();
    void onExtractAll();
    void onPrintFile();
    void onTabChanged(int idx);
    void onTabClose(int idx);
    void onPageChanged(int pageIndex);
    void onCommentActivated(int pageIndex, int annotIndex);
    void onZoomChanged(double scale);
    void onTextRegionSelected(int pageIdx, QRectF rectPts, QPoint globalPos);

private:
    void setupActionBar();
    void applyTheme(bool dark);
    void syncSidebarToTab(int idx, bool forceRebuild = false);
    DocTab* currentTab() const;
    void showThumbnailContextMenu(int pageIndex, QPoint globalPos);
    void reloadTab(DocTab* t, const QString& filePath, const QString& tmpPath);
    void loadTabFile(DocTab* t, const QString& path, bool structureChanged = true);
    void updateTabDirty(DocTab* t);
    void performSign(DocTab* t, SignParams sp);
    void onFinalizeSignature();
    void onCancelSignature();
    void onCommentsRequested();
    void refreshCommentsForPage(DocTab* t, int page);
    SignParams m_pendSp;
    int  m_pendPage = -1;
    bool m_pendActive = false;
    QAction* m_finalizeSigAct = nullptr;
    QAction* m_cancelSigAct   = nullptr;

    QTabWidget*      m_docTabs       = nullptr;
    QList<DocTab*>   m_openDocs;
    ThumbnailPanel*  m_thumbPanel    = nullptr;
    ContinuousView*  m_continuousView = nullptr;
    QSplitter*       m_splitter      = nullptr;

    QAction*   m_continuousAct = nullptr;
    QAction*   m_translateAct  = nullptr;
    QLineEdit* m_zoomEdit      = nullptr;
    bool       m_darkMode      = false;
    bool       m_continuousMode = false;

    std::unique_ptr<PdfEditor>  m_editor;
    TextSearch*                 m_textSearch    = nullptr;
    FindBar*                    m_findBar       = nullptr;

    // Translation feature
    Translator*        m_translator  = nullptr;
    TranslationPopup*  m_transPopup  = nullptr;
    GoogleAuth*        m_googleAuth  = nullptr;
    UpdateChecker*     m_updateChecker = nullptr;
    QPoint             m_lastTransPos;

    int m_selPage = -1;
    int m_selIdx  = -1;
    AnnotStyle m_annotStyle;

    void deleteSelectedAnnot(int page, int index);
    void editSelectedAnnot(int page, int index);
    void showNotePopup(const QString& text, const QString& author);
    void hideNotePopup();
    QLabel* m_notePopup = nullptr;
    QMetaObject::Connection m_sigPickConn;

    // Settle timer: defers full-quality render until page stops changing for 400ms.
    // Prevents mutex contention during fast scrolling through many pages.
    QTimer* m_settleTimer = nullptr;
    QTimer* m_warmTimer = nullptr;
    QTimer* m_preloadTimer = nullptr;   // hoan preload trang ke ben
    qint64  m_settleStartMs = 0;  // timestamp of first onPageChanged in scroll sequence
    qint64  m_lastNavMs = 0;      // last user nav timestamp — warm cache skips if < 5s idle

    // ── Undo/Redo ────────────────────────────────────────────────────────────
    void pushUndo(DocTab* t, const MarkupUndoEntry& e);
    void doUndo();
    void doRedo();
    void applyMarkupRefresh(DocTab* t, int page, bool touchedPageObjects = true);
    void updateUndoActions();
    QAction* m_undoAct = nullptr;
    QAction* m_redoAct = nullptr;

    // ── Markup helpers ───────────────────────────────────────────────────────
    void showAnnotOverlayImmediate(DocTab* tab, int pageIdx);
    void scheduleReRender(DocTab* tab, int pageIdx);
    void refreshAnnotVisuals(DocTab* t, int page);
    bool canFastPath(DocTab* t, int page) const;
    bool baseIsVector(DocTab* t, int page) const;

    const QList<AnnotInfo>& annotsForPage(DocTab* t, int page);
    void invalidateAnnotPage(DocTab* t, int page);
    void buildVectorLayer(DocTab* t, int pageIndex, bool force = false);
    // annotsForPage returns a reference into the cache — DO NOT retain it
    // across any call that may invalidate the cache (invalidateAnnotPage,
    // removeAnnot, refreshAnnotVisuals, refreshCommentsForPage, etc.).
    QTimer* m_markupTimer = nullptr;
    DocTab* m_markupTab   = nullptr;
    int     m_markupPage  = -1;
    bool    m_commentsVisible = false;
    // ── Search state (shared between FindBar and SearchPanel) ─────────────
    QList<SearchResult> m_searchResults;
    int                 m_searchCurrentIdx = -1;
    void applySearchHighlights(const QList<SearchResult>& results, int currentIdx);
    void clearAllSearchHighlights();
};
