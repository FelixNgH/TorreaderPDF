#pragma once
#include <QMainWindow>
#include <QTabWidget>
#include <QAction>
#include <QLineEdit>
#include <QList>
#include <QSet>
#include <QHash>
#include <QFuture>
#include <QFutureWatcher>
#include <QAtomicInt>
#include <limits>
#include <memory>
#include "core/PdfDocument.h"
#include "core/PdfRenderer.h"
#include "core/TileCacheFile.h"
#include "core/ThumbnailRenderPool.h"
#include "core/TextSearch.h"
#include "core/TextSelection.h"
#include "core/PdfSigner.h"
#include "core/PdfLinks.h"
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
class ForeignAnnotLayer;

// Trang thai chon chu theo CHI SO KY TU (SPEC_TEXTSEL_ADOBE phan 2). Luu trong
// DocTab de moi tai lieu giu vung chon rieng. anchor/focus da chuan hoa theo
// thu tu doc (anchor <= focus khi tinh doan).
struct TextSel {
    int  anchorPage = -1;
    int  anchorChar = -1;
    int  focusPage  = -1;
    int  focusChar  = -1;
    bool active     = false;
};

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
    QFuture<void> annotVisualsFuture;    // rescan loadPageVisuals in flight (SPEC_NAV_INSTANT)
    QSet<int>     visualsScanning;       // trang dang co rescan chay (tranh trung lap)
    QHash<int, QList<AnnotInfo>> annotPageCache;
    QHash<int, bool> overlayCapablePage;   // cached per-page overlay capability
    QHash<int, QList<AnnotVisual>> visualsCache;     // cached loadPageVisuals result per page
    QHash<int, quint32>            visualsRev;        // pageRevision at time of cache
    QHash<int, bool>               visualsHasForeign; // cached hasForeign per page
    QSet<int>        pagesNeedGenerate;     // pages needing FPDFPage_GenerateContent before save
    QString  originalPath;        // real on-disk file — Save target & tab name source
    bool     dirty = false;       // has unsaved in-memory edits (working copy != original)
    std::shared_ptr<VectorLayer> vecLayer;  // GPU vector overlay for heavy pages
    std::shared_ptr<ForeignAnnotLayer> fgnLayer;   // lop annot phan mem khac cho trang vector thuan
    QSet<int> fgnBuilding;                          // trang dang dung, tranh dung chong
    bool fgnRegionBuilding = false;
    int warmingPage = -1;
    QSet<int> vecBuilding;  // pages currently building vector layer (anti-duplicate)
    QList<MarkupUndoEntry> undoStack;
    QList<MarkupUndoEntry> redoStack;
    QSet<int> ocrBusyPages;   // trang dang chay OCR o luong nen (tranh chay chong)
    bool ocrAllBusy = false;  // OCR ca tai lieu dang chay

    // ── Search state rieng cua TUNG tai lieu (SPEC_SEARCH_STATE_R3) ──────
    // Moi DocTab giu ket qua tim kiem cua chinh minh — doi tab qua lai thay
    // lai ket qua cu, khong phai tim lai tu dau. Dong tab la tu het.
    QList<SearchResult> searchResults;
    int                 searchCurrentIdx = -1;
    QString             searchQuery;       // truy van dang dung cua tai lieu nay

    // ── Link noi bo (SPEC_PDF_LINKS) ──────────────────────────────────────
    // Trang dich chua render xong thi center lai khi pageReady (render async
    // reset m_panOffset sau setPage). Xem applyPendingLinkCenter().
    int     pendingLinkCenterPage = -1;
    QRectF  pendingLinkCenterRect;   // toa do hien thi
    bool    pendingLinkCenterActive = false;

    // ── Chon chu (SPEC_TEXTSEL_ADOBE phan 2) ─────────────────────────────
    TextSel textSel;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void openFile(const QString& path);

    // Probe-only: lai che do xem tu dong lenh (dung cho --viewprobe).
    // centerXpt/centerYpt: toa do TRANG PDF (goc duoi-trai, don vi point).
    // Truyen NaN (mac dinh) = giu nguyen vi tri cuon nhu cu.
    void probeSetView(bool continuous, double zoomPercent, int page1Based,
                      double centerXpt = std::numeric_limits<double>::quiet_NaN(),
                      double centerYpt = std::numeric_limits<double>::quiet_NaN());

    // Probe-only (--searchnav-test): tim kiem THAT roi "bam" ket qua thu
    // resultIdx1Based qua dung tin hieu searchResultSelected.
    void probeSearchNav(bool continuous, double zoomPercent, const QString& query,
                        int resultIdx1Based, int waitMs);

    // Probe-only (--searchstate-test): nghiem thu 3 loi trang thai tim kiem.
    void probeSearchState(const QString& pathA, const QString& pathB, const QString& query,
                          bool continuous, double zoomPercent, int waitMs);

    // Chup cua so ra pngPath + ghi dump mau ra txtPath (dung cho --uiprobe va
    // phim tat Ctrl+Shift+F12). errOut: ly do khi tra ve false.
    bool probeSnapshot(const QString& pngPath, const QString& txtPath,
                       QString* errOut = nullptr, int shotTab = -1);

    // Probe-only (--pageflip-bench, SPEC_PERF_DESK_ABOUT phan 1): lat qua lai
    // giua p1 p2 (0-based), LOOP lan, do thoi gian moi lan doi trang tu luc yeu
    // cau (onPageChanged) toi khi pageReady (trang ve xong). In theo khuon:
    //   [pageflip] from=<a> to=<b> ms=<..> pageHasTextCached=<0|1>
    //   [pageflip] TONG n=<..> min=<..> max=<..> mean=<..>
    // Chi dung cho probe — khong lien quan den nguoi dung.
    void probeFlipBench(int p1, int p2, int loops);

    // Probe-only: chuyen tiep toi ThumbnailPanel::selectTab de --uiprobe chon tab
    // sidebar (0..5) trong MainWindow. Chi dung cho harness, khong can nguoi dung.
    void probeSelectSidebarTab(int id);

    // Probe-only (--uiprobe-dialog, SPEC_PROBE_DIALOG_FRAMES phan 1): kich hoat
    // QAction cua hop thoai tren toolbar (name = merge/about/sign/print), chup
    // CHINH hop thoai (active modal) ra <outDir>/dialog_<name>.png + .txt roi dong
    // bang reject()/close() de khong ket trong vong lap modal. Chi cho harness.
    bool probeDialog(const QString& name, const QString& outDir, QString* errOut,
                     int grabDelayMs = 1200);

    // Probe-only (--uiprobe-frames, SPEC phan 2): chay kich ban trinh dien co dinh
    // (thumbnails, trang 2-4, search, comments, ocr, dark on/off), chup mot khung
    // sau moi buoc ra <outDir>/frame_XXX.png. Tra ve so khung da ghi.
    int probeFrames(const QString& outDir, int intervalMs);

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
    void onNavDeferred();
    void onCommentActivated(int pageIndex, int annotIndex);
    void onZoomChanged(double scale);
    void onTextRegionSelected(int pageIdx, QRectF rectPts, QPoint globalPos);
    void onOcrPageRequested(int pageIdx);
    void onOcrAllRequested();

    // Chon chu theo chi so ky tu (SPEC_TEXTSEL_ADOBE). View bao tin hieu khi
    // nguoi dung chon chu; MainWindow ghi vao DocTab::textSel va day rect ve.
    void onTextSelectionChanged(int anchorPage, int anchorChar,
                                int focusPage, int focusChar);
    void onTextSelectionCleared();
    void onCopySelectionRequested(QPoint globalPos);

    // Link PDF (SPEC_PDF_LINKS): dieu huong noi bo / mo link ngoai sau khi
    // xac nhan. Goi tu ca ContinuousView va PdfGpuView.
    void onLinkActivated(DocTab* t, int page, const PdfLink& link);
    void applyPendingLinkCenter(DocTab* t);

signals:
    // OCR chay o luong nen phat tin hieu ve giao dien (SPEC_OCR_TAB phan 1b).
    // pageIndex1Based: trang dang xu ly (1-based); totalPages: so trang cua doc.
    void ocrProgress(int pageIndex1Based, int totalPages);
    // Mot trang OCR xong (de panel cap nhat "Recognized (N words)").
    void ocrPageFinished(int pageIndex, int words);

private:
    void setupActionBar();
    void applyTheme(bool dark);
    void syncSidebarToTab(int idx, bool forceRebuild = false);
    DocTab* currentTab() const;
    void showThumbnailContextMenu(int pageIndex, QPoint globalPos);
    void reloadTab(DocTab* t, const QString& filePath, const QString& tmpPath);
    void loadTabFile(DocTab* t, const QString& path, bool structureChanged = true);
    // SPEC_PAGECACHE_CORE muc 4: sau khi trang on dinh 300 ms, prefetch trang lien ke.
    void schedulePagePrefetch();
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
    QAction*   m_selectTextAct = nullptr;
    QAction*   m_translateAct  = nullptr;
    QAction*   m_darkAct       = nullptr;
    QLineEdit* m_zoomEdit      = nullptr;
    QLabel*    m_hintLabel     = nullptr;
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

    // ── Markup pick/context/move — dung chung cho PdfGpuView VA ContinuousView ──
    // (SPEC_CONTINUOUS_MARKUP_EDIT_2026-08-16). Ba handler nay la than mot loi:
    // PdfGpuView goi theo tab, ContinuousView goi theo currentTab — cung duong
    // AnnotationManager + undo (MarkupUndoEntry), khong co duong thu hai.
    void onAnnotPick(DocTab* t, int page, const QPointF& pt);
    void onAnnotContext(DocTab* t, int page, const QPointF& pt, const QPoint& gpos);
    void onAnnotMove(DocTab* t, int page, double dx, double dy);
    // Day trang thai chon xuong CA HAI view (gpu + continuous) cho khop dien mao.
    void setMarkupSelectionViews(DocTab* t, int page, const QRectF& rectPdf,
                                 const QString& uid, const QString& type);
    void clearMarkupSelectionViews(DocTab* t);

    // Probe-only (--contedit-test, SPEC_CONTINUOUS_MARKUP_EDIT muc NGHIEM THU):
    // chay day du chuoi pick → drag → undo tren ContinuousView, in theo khuon
    // [contedit] ... Khong dung chuot that.
    void probeContEdit(int page0, double x, double y, double dx, double dy);

    // ── Chon chu (SPEC_TEXTSEL_ADOBE) ──────────────────────────────────────
    // Xoa vung chon cua tab hien tai + cap nhat ca 2 view. Esc, doi tool, moc.
    void clearTextSelection();
    // Copy textForRange cua DocTab::textSel vao clipboard (Ctrl+C / menu).
    void copyTextSelectionToClipboard();
    // Day rect chon xuong ca 2 view tu DocTab::textSel cua tab hien tai.
    void pushSelectionToViews(DocTab* t);

    // Settle timer: defers full-quality render until page stops changing for 400ms.
    // Prevents mutex contention during fast scrolling through many pages.
    QTimer* m_settleTimer = nullptr;
    QTimer* m_warmTimer = nullptr;
    QTimer* m_preloadTimer = nullptr;   // hoan preload trang ke ben
    qint64  m_settleStartMs = 0;  // timestamp of first onPageChanged in scroll sequence
    qint64  m_lastNavMs = 0;      // last user nav timestamp — warm cache skips if < 5s idle

    // ── Nav instant (SPEC_NAV_INSTANT_2026-08-16): placeholder len dau, viec nang
    //    (refreshAnnotVisuals + ensureForeignAnnotLayer) hoan 120ms chong doi.
    QTimer* m_navDeferTimer = nullptr;
    DocTab* m_navDeferTab   = nullptr;   // tab dang cho viec nang (hoac dang chay)
    int     m_navDeferPage  = -1;        // trang dang cho viec nang (hoac dang chay)
    int     m_navFlipFrom   = -1;        // trang truoc (de log [nav])
    qint64  m_navFlipAtMs   = 0;         // thoi diem vao onPageChanged (log [nav])
    qint64  m_navPlaceholderMs = 0;      // do placeholderMs trong lan lat nay (log [nav])
    // Deferred work dang chay ngoai UI thread; dong [nav] se in tai apply-back
    // voi visualsMs = thoi gian that cua rescan. Vi hoan (deferredStartMs) ghi rieng.
    bool    m_navLogArmed      = false;
    qint64  m_navDeferredStartMs = 0;    // flip → luc deferred work bat dau (log [nav])
    qint64  m_navVisualsStartMs  = 0;    // deferred work bat dau → rescan xong (visualsMs)

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
    // Áp kết quả visuals vào renderer + view (cả 2 view). Dùng chung cho đường
    // CACHE HIT (sync) và đường RESCAN (async, áp lại trên UI thread).
    void applyAnnotVisuals(DocTab* t, int page,
                           const QList<AnnotVisual>& visuals,
                           bool overlayCapable, bool hasForeign);
    bool canFastPath(DocTab* t, int page) const;
    bool baseIsVector(DocTab* t, int page) const;

    const QList<AnnotInfo>& annotsForPage(DocTab* t, int page);
    void invalidateAnnotPage(DocTab* t, int page);
    void buildVectorLayer(DocTab* t, int pageIndex, bool force = false);
    void ensureForeignAnnotLayer(DocTab* t, int pageIndex);
    // annotsForPage returns a reference into the cache — DO NOT retain it
    // across any call that may invalidate the cache (invalidateAnnotPage,
    // removeAnnot, refreshAnnotVisuals, refreshCommentsForPage, etc.).
    QTimer* m_markupTimer = nullptr;
    DocTab* m_markupTab   = nullptr;
    int     m_markupPage  = -1;
    bool    m_commentsVisible = false;
    // ── Search state (SPEC_SEARCH_STATE_R3) ───────────────────────────────
    // Ket qua gio nam trong DocTab::searchResults (xem struct DocTab). Day la
    // tab dang so huu lan tim kiem dang chay (TextSearch chay bat dong bo) de
    // ket qua stream ve dung tab, khong lan sang tab khac khi doi tab giua chung.
    DocTab* m_searchTab = nullptr;
    void applySearchHighlights(const QList<SearchResult>& results, int currentIdx);
    void clearAllSearchHighlights();

    // ── OCR qua thao tac chuot (SPEC_OCR_4) ─────────────────────────────
    // Kiem trang/tai lieu dang OCR chua. docHasText: co chu thuc su hay khong
    // (FPDFText_CountChars). Dung cho menu chuot phai + dai nhac + tim kiem.
    // pageHasTextSync: phien ban NANG (FPDF_LoadPage + FPDFText_LoadPage) CHI
    // dung boi cac hanh dong nguoi dung hi hưu (menu chuot phai, bam OCR, chon
    // chu) — QUA TRINH LAT TRANG CHAY QUA notifyOcrStatusForPage (cache + async,
    // SPEC_PERF_DESK_ABOUT phan 1) chứ KHONG qua ham nay.
    static bool pageHasTextSync(FPDF_DOCUMENT doc, int pageIndex);
    bool pageNeedsOcr(FPDF_DOCUMENT doc, int pageIndex);
    bool docHasAnyText(FPDF_DOCUMENT doc, int currentPage); // chi kiem mau (3 trang dau + trang hien tai)
    void runOcr(FPDF_DOCUMENT doc, int firstPage, int lastPage, DocTab* tab,
                const QString& sourceTag,
                const QString& langs = QStringLiteral("vie+eng"),
                std::shared_ptr<QAtomicInt> cancelFlag = nullptr);
                // chay nhan dang o luong nen, khong block
    // Bao ket qua OCR mot trang ra thanh trang thai (khong im lang - muc 2 SPEC
    // PROBE_LOG_SNAPSHOT). words == 0 thi chi cho noi luu dau vet.
    void showOcrTraceMessage(int pageIndex, int words);
    // Ctrl+Shift+F12: chup cua so + dump mau ra %TEMP% (muc 3).
    void captureUiSnapshot();
    // Thay the dai nhac OCR (SPEC_OCR_TAB phan 1c): khi trang dang xem khong
    // co chu chi hien MOT DONG o thanh trang thai, khong chiem cho, khong tắt.
    // SPEC_PERF_DESK_ABOUT phan 1: chi doc CACHE (OcrTextCache), khong bao gio
    // goi FPDF_LoadPage tren luong giao dien; chua co thi de timer 250ms lo,
    // xong kiem bang QtConcurrent roi moi cap nhat UI.
    void notifyOcrStatusForPage(int pageIndex);
    // Debounce: doi 250ms roi moi danh gia (lat nhanh chi danh gia trang dung).
    void onOcrNotifyTimeout();
    // Ap trang thai OCR cho trang dang xem TU CACHE (da biet chac) — goi tu
    // notifyOcrStatusForPage (cache hit) va tu worker async sau khi co ket qua.
    void applyOcrStatusNow();
    QTimer*        m_ocrNotifyTimer = nullptr;
    FPDF_DOCUMENT  m_ocrNotifyDoc   = nullptr;
    int            m_ocrNotifyPage  = -1;
    // Day cong cu xuong CA HAI view (PdfGpuView + ContinuousView) + log
    // [tool] set id=... view=... de nghiem thu bang so (SPEC phan 2 + 4).
    void pushToolToViews(PdfGpuView::ViewTool tool, int sidebarId);
    // OCR tab trong sidebar (SPEC_OCR_TAB phan 1b).
    void onOcrWholeFromTab(const QString& langs);
    void onOcrPageFromTab(const QString& langs);
    // Tim kiem khi tai lieu khong co chu: hoi OCR mot lan, xong chay lai tim kiem.
    void handleSearchRequest(const QString& query, Qt::CaseSensitivity cs,
                             bool matchDiacritics = false);
    void maybeAskOcrForSearch(const QString& query, Qt::CaseSensitivity cs);
    QString m_ocrSearchPendingQuery;
    Qt::CaseSensitivity m_ocrSearchPendingCs = Qt::CaseInsensitive;
    bool m_ocrSearchAsked = false;   // da hoi OCR cho tim kiem trong phien nay
    std::shared_ptr<QAtomicInt> m_ocrCancel;   // co Cancel cho luot OCR dang chay
    QFutureWatcher<void>* m_ocrWatcher = nullptr;
    int  m_ocrWatcherFirst = 0;
    int  m_ocrWatcherLast  = -1;
    QString m_ocrSourceTag;                  // "menu"/"select"/"search"/"banner"
    // Vung chon nguoi dung keo truoc khi OCR (de ap lai sau khi nhan dang xong).
    int     m_pendingSelPage = -1;
    QRectF  m_pendingSelRect;
    QPoint  m_pendingSelPos;
};
