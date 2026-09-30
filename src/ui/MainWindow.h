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
#include <QFutureSynchronizer>
#include <QAtomicInt>
#include <atomic>
#include <limits>
#include <memory>
#include "core/PdfDocument.h"
#include "core/PdfRenderer.h"
#include "core/TileCacheFile.h"
#include "core/VectorCacheFile.h"
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
    // 🔴 0928 LƯỢT 22 — MÃ THẾ HỆ: mọi lambda queued bắt con trỏ DocTab chốt
    // `m_openDocs.contains(t)` chỉ theo ĐỊA CHỈ; malloc có thể cấp lại đúng địa
    // chỉ đó cho DocTab mới ⇒ event cũ chạm nhầm tab mới. Serial tăng đơn điệu
    // theo mỗi `new DocTab` — lambda chụp serial lúc spawn và so khi chạy.
    inline static quint64 sTabSerial = 0;   // chỉ luồng UI ghi (openFile)
    quint64 serial = ++sTabSerial;
    std::unique_ptr<PdfDocument>       doc;
    std::unique_ptr<PdfRenderer>       renderer;
    std::unique_ptr<AnnotationManager> annotMgr;
    std::unique_ptr<AnnotationLayer>   annotLayer;
    std::shared_ptr<TileCacheFile>        tileCache;
    std::unique_ptr<ThumbnailRenderPool>  thumbPool;
    PdfGpuView* view        = nullptr;
    int      currentPage    = 0;
    double   zoom           = 1.0;
    // 🔴 LƯỢT 33b (L): offset cuon THAT cua che do Continuous theo tung tab, de quay
    // lai tab khong bi NHAY VE TRANG 1 (setDocument luôn reset ve dinh). Luu moi lan
    // cuon lang (pageChanged), khoi phuc sau setDocument + lay layout.
    int      contScrollY    = 0;
    bool     contPosSaved   = false;
    QMetaObject::Connection pageReadyConn;
    QMetaObject::Connection scrollConn;
    QList<AnnotInfo> annotCache;
    bool     annotCacheValid = false;
    bool     annotScanInFlight = false;  // guards concurrent full annot scans
    // 🔴 LƯỢT 33e (muc 2 — reviewer loi 2): tab thanh hien hanh luc scan cu con
    // in-flight (bi stopScan cat) ⇒ danh dau de finished handler TU spawn lai.
    bool     annotRescanWanted = false;
    QFuture<void> annotScanFuture;
    QFuture<void> annotVisualsFuture;    // rescan loadPageVisuals in flight (SPEC_NAV_INSTANT)
    QFuture<void> annotPageFuture;       // loadPage background (VIỆC 1 SPEC_SMOOTH_123)
    // 🔴 0928 LƯỢT 21 — future mở file (docPtr->open chạy trên QtConcurrent).
    // ĐÓNG TAB KHI OPEN CÒN CHẠY ⇒ closeJob nền PHẢI waitForFinished TRƯỚC
    // `delete t` (~PdfDocument unmap mmap view trong lúc pdfium còn đọc nó —
    // dump TorReader_r8.exe.32832: pdfium đọc NULL tại PdfDocument.cpp:101).
    QFuture<bool> openFuture;
    // 🔴 0928 LƯỢT 22 — DANH SÁCH future (thay vì một SLOT bị ghi đè):
    // fgnFuture/annotVisualsFuture/annotPageFuture/fgnRegionFuture/heavyRegionFuture
    // mỗi trang một lần spawn, slot cũ bị ghi đè ⇒ shutdownTab chỉ chờ CÁI MỚI
    // NHẤT, task cũ vẫn chạm doc/mgr sau `delete t`. MỌI QtConcurrent::run của
    // tab chạm PDFium/mgr PHẢI push một lệnh chờ vào đây; shutdownTab chờ HẾT.
    // (Danh sách hàm-chờ, không phải QFutureSynchronizer<void>: Qt6 từ chối
    //  addFuture(QFuture<bool>) — không có conversion sang QFuture<void>.)
    // KHÔNG gom vec build/OCR vào: chúng không đọc t->, token sổ + shared_ptr
    //  vecGen bảo vệ; chờ chúng trên UI = đứng hình.
    // 🔴 LƯỢT 22b (reviewer mục 4): thêm cờ "done" để DỌN mục đã xong khi push —
    // bản L22 chỉ push, danh sách tăng đơn điệu theo số lần spawn của tab.
    struct BgWait { std::function<void()> wait; std::function<bool()> done; };
    QList<BgWait> bgWaits;
    template <class F> void addBgWait(F f) {   // copy theo giá trị — waitForFinished non-const
        for (int i = bgWaits.size() - 1; i >= 0; --i)
            if (bgWaits[i].done()) bgWaits.removeAt(i);
        bgWaits.push_back({ [f]() mutable { f.waitForFinished(); },
                            [f]() { return f.isFinished(); } });
    }
    QSet<int>     visualsScanning;       // trang dang co rescan chay (tranh trung lap)
    // 🔴 LƯỢT 33f (mục 3 — reviewer): tran cho vong hen thu lai ABORTED (500*n, max 5
    // lan) + danh dau trang KHONG DOC DUOC (fpage null) de dung thu vo han.
    QHash<int, int> visualsRetry;
    QSet<int>       visualsUnreadable;
    // 🔴 LƯỢT 33f (mục 1 — đo full probe): trang quái vật (≥1M object) đã hẹn HOAN 2 s
    // de flip xong moi quet — tranh chan flip ke tiep 1,65 s (page 4: 1135→3101 ms).
    QSet<int>       visualsHeavyDue;
    QHash<int, QList<AnnotInfo>> annotPageCache;
    // 🔴 LƯỢT 40b: retry counters cho lock-busy detection
    int pickRetryAttempt = 0;    // onAnnotPick retry counter
    int ctxRetryAttempt = 0;     // onAnnotContext retry counter
    QHash<int, bool> overlayCapablePage;   // cached per-page overlay capability
    // 🔴 CO RIENG 2026-09-01: "overlay co ve HET moi annot cua trang nay khong".
    // KHONG duoc gop vao `overlayCapablePage` — co do con phuc vu viec khac, ha no xuong
    // lam SINGLE MAT MARKUP (da tra gia). Co nay CHI dung cho `canFastPath` (quyet dinh
    // co can dung LOP BU cho chu thich ngoai hay khong).
    QHash<int, bool> overlayVeHetPage;
    QHash<int, QList<AnnotVisual>> visualsCache;     // cached loadPageVisuals result per page
    QHash<int, quint32>            visualsRev;        // pageRevision at time of cache
    QHash<int, bool>               visualsHasForeign; // cached hasForeign per page
    QSet<int>        pagesNeedGenerate;     // pages needing FPDFPage_GenerateContent before save
    QString  originalPath;        // real on-disk file — Save target & tab name source
    bool     dirty = false;       // has unsaved in-memory edits (working copy != original)
    // ── .torvec cache key (SPEC_PERF_HEAVYPAGE buoc B) ────────────────────────
    // pdfPath = file dang duoc render (doc->filePath()); pdfHash bam MOT LAN o luc
    // mo/mo lai, toi 4 cho build dung lai (KHONG bam lai file lon tren luong giao dien).
    QString  pdfPath;             // file dang duoc render cho lop vector
    uint64_t pdfHash = 0;         // hash cua pdfPath (da tinh san o luc mo)
    // LÁT B 09/02: KHO LỚP VECTOR DUY NHẤT theo tài liệu (khuôn Lát A). Trước đây
    // DocTab::vecLayer (một trang) + ContinuousView::m_vecLayers (ba trang) là HAI bản
    // chép lệch nhau -> trang quái vật Continuous nạp được cache nhưng Single thấy NULL.
    // Nay MỘT kho hash, cả hai view đọc qua con trỏ; eviction của Continuous đánh thẳng
    // vào đây. Key = pageIndex, value = layer trang đó (null = chưa có).
    QHash<int, std::shared_ptr<VectorLayer>> vecLayers;  // GPU vector overlay, theo trang
    std::shared_ptr<ForeignAnnotLayer> fgnLayer;   // lop annot phan mem khac cho trang vector thuan
    QSet<int> fgnBuilding;                          // trang dang dung, tranh dung chong
    // Trang da HOAN lop bu 1 lan vi chua co noi dung de ve (chot C3 2026-08-30).
    // Xoa khi doi trang (moi lan hien duoc hoan lai toi da 1 lan — lan goi sau phai DUNG).
    QSet<int> fgnDeferred;
    bool fgnRegionBuilding = false;
    // 🔴 VIEc CHONG CRASH (30/08): fgnPending giu lop bu DANG BUILD (de goi cancel()),
    //    fgnFuture la future cua tac vu lop bu. TRUOC khi dong/giai phong t->doc phai
    //    cancelForeignAnnotTasks() — neu khong pdfium ghi vao doc da giai phong.
    std::shared_ptr<ForeignAnnotLayer> fgnPending;
    QFuture<bool> fgnFuture;
    QFuture<bool> fgnRegionFuture;                  // future cua buildRegion dang chay
    int warmingPage = -1;
    QSet<int> vecBuilding;  // pages currently building vector layer (anti-duplicate)
    // 🔴 0928 LƯỢT 13 — THẾ HỆ ĐỂ HUY VIỆC THỪA. Tăng mỗi lần đổi trang; mọi
    // `VectorLayer::build` đang chạy cho trang cũ so `vecGen` này và bỏ ở RÁNH
    // GIỚI LÁT KẾ TIẾP. Đo được trên máy test: End+PgUp×111 làm app dựng vector
    // cho 26 trang đã lướt qua (349,348,…,324) — 143 s build vector trong một phiên
    // — và trang ĐANG XEM phải chờ 2905 ms trên khoá chung.
    // ponytail: đếm nguyên, đọc từ mọi luồng; không cần std::atomic vì chỉ luồng
    // giao diện ghi, worker chỉ đọc.
    // 🔴 0928 LƯỢT 22 — shared_ptr: lambda vec build chạy TRÊN LUỒNG NỀN đọc cái
    // này; `delete t` (closeJob nền) có thể đã giải phóng DocTab khi task còn xếp
    // hàng/chạy (future vec KHÔNG được chờ — build CAD tới 14 s). shared_ptr sống
    // cùng lambda, tab chết không kéo theo nó.
    std::shared_ptr<std::atomic<quint32>> vecGen = std::make_shared<std::atomic<quint32>>(0);
    // 🔴 Trang DA BI SUA trong phien nay (markup/di chuyen note...). Cache .torvec khoa theo
    //    HASH FILE TREN DIA, ma sua trong bo nho thi hash KHONG doi => cache se tra ve hinh
    //    CU va nuot moi chinh sua (loi 2026-08-19: keo comment, o chon di nhung hinh o lai).
    //    Trang nam trong tap nay thi CAM dung cache .torvec cho toi khi dong tai lieu.
    QSet<int> torvecDirty;
    // Trang co markup KHONG overlay duoc (vi du trang co LINK => overlayCapable=false).
    // Lop vector dung tu page object, khong chua annotation, nen nhung trang nay BAT BUOC
    // phai lay raster (co FPDF_ANNOT) lam nen, neu khong markup se VO HINH.
    QSet<int> forceRasterPages;
    // ── LÁT C 09/02: tach so phien "nen vector" khoi "lop bu toan trang" ─────────
    // Trang NANG (objCount > tran 400000) truoc day bi forceRasterPage vat bo nen
    // vector chi vi khong dung duoc lop bu toan trang (87,8 giay/trang, giu khoa
    // 14s+). Tran do VAN CHAN lop bu; nen vector giu lai, chu thich do PDFium ve
    // theo VUNG NHIN: o VUONG TUNG annot, clip dung o (cong thuc --annotroi-bench,
    // do 18,6 ms/o tren trang 2,54M object), gao nen trang, chong trong suot len
    // nen vector. KHONG dung buildRegion (render ca vung nhin: CAD chen duc day,
    // do 4s+7s giu khoa) va KHONG BAO GIO goi ForeignAnnotLayer::build.
    // SU KIEN 0902 (thay polling 1s gay vang app tren may owner): chi dung khi
    // su kien goi updateHeavyRegion — UpdateRequest (pan/repaint), zoomChanged,
    // doi trang, invalidateAnnotPage.
    QSet<int> heavyRegionPages;
    std::shared_ptr<QAtomicInt> heavyRegionCancel;  // don tap van region khi dong tai lieu
    bool heavyRegionBuilding = false;
    QFuture<bool> heavyRegionFuture;
    int   heavyRegionPage  = -1;    // trang cua anh region da dung gan nhat
    double heavyRegionScale = 0.0;  // zoom luc dung
    QRect  heavyRegionPx;           // vung da dung (px cua trang tai heavyRegionScale)
    // LAT G 09/02 — handle RIENG cho renderAnnotRegion: NAP MOT LAN roi DUNG LAI.
    // LAT E tuong FPDF_LoadPage o day "RE" — SAI: do tren trang 2,54M object no ton
    // 2,0-2,1 giay MOI LAN goi. Handle rieng van CAN (chong crash 0x80000003 —
    // render dong bo de len handle PageCache dang tam dung), nhung phai giu lai
    // trong DocTab chu khong nap/dong theo nhip dong ho.
    // Quy tac so huu: chi TAC VU build (luong nen) doc/ghi handle khi no dang chay
    // (heavyRegionBuilding); main thread chi cham vao handle khi KHONG co tac vu nao
    // (!building) hoac SAU waitForFinished (cancelForeignAnnotTasks / watcher).
    FPDF_PAGE heavyPrivPage  = nullptr;   // handle rieng cho heavyPrivIndex
    int       heavyPrivIndex = -1;        // trang cua handle dang mo
    bool      heavyPrivStale = false;     // noi dung doi khi dang render -> tac vu nap lai
    // LAT G 0902 — "danh dien vung nhin": su kien goi ham nay co the dens lien tuc
    // (moi lan repaint); chi dung lai khi vung nhin THAT SU doi.
    int    heavySeenPage = -1;
    double heavySeenZoom = 0.0;
    QRect  heavySeenVis;
    QList<MarkupUndoEntry> undoStack;
    QList<MarkupUndoEntry> redoStack;
    QSet<int> ocrBusyPages;   // trang dang chay OCR o luong nen (tranh chay chong)
    bool ocrAllBusy = false;  // OCR ca tai lieu dang chay
    // Livelock fix (2026-08-31): trang dang co mot lan hen lai loadPage dang cho.
    // Chi giu MOT lan hen lai cho mot trang, tranh hai vong hen lai song song.
    QSet<int> annotRetryPending;

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

    // Probe-only: expose tab hien tai cho --contvec-probe (currentTab la private).
    DocTab* currentTabForProbe() const { return currentTab(); }
    // Probe-only (0927 LƯỢT 2): gọi ĐÚNG đường Save của app (onSaveFile) —
    // không phải mgr.saveDocument() trần. Lý do: onSaveFile mới materialize
    // pagesNeedGenerate, giải phóng handle PDFium rồi replaceFileAtomically;
    // gọi tầng thấp sẽ BỎ QUA chính những bước đó (đã đo 27/09: probe gọi
    // mgr.saveDocument() ⇒ không đúng đường người dùng bấm Save).
    // Đánh dấu tab là dirty rồi trả về đường dẫn đã ghi (rỗng = thất bại).
    QString probeSaveViaGui(QString* errOut = nullptr);
    // Probe-only (0927 LƯỢT 2): chạy onSaveFile() TRONG 1 sự kiện vòng lặp
    // (QTimer 0) + vòng processEvents ⇒ các bước chờ pool/giá trong
    // onSaveFile vẫn thoát. Hàm trả về khi timer nổ. Rỗng nghĩa là treo.
    bool probeSaveViaGuiAsync(QString* errOut = nullptr);

    // Probe-only: lai che do xem tu dong lenh (dung cho --viewprobe).
    // centerXpt/centerYpt: toa do TRANG PDF (goc duoi-trai, don vi point).
    // Truyen NaN (mac dinh) = giu nguyen vi tri cuon nhu cu.
    void probeSetView(bool continuous, double zoomPercent, int page1Based,
                      double centerXpt = std::numeric_limits<double>::quiet_NaN(),
                      double centerYpt = std::numeric_limits<double>::quiet_NaN());

    // Probe-only (--contvec-probe): cuon ContinuousView toi trang (0-based) roi bơm
    // su kien de timers (scrollTimer/vecBuildTimer) chay het — dung de nghiem thu
    // Viec A/B/C (nap lai cache .torvec khi quay lai trang nang). Chi cho harness.
    void probeContScrollTo(int page);

    // Probe-only (--searchnav-test): tim kiem THAT roi "bam" ket qua thu
    // resultIdx1Based qua dung tin hieu searchResultSelected.
    void probeSearchNav(bool continuous, double zoomPercent, const QString& query,
                        int resultIdx1Based, int waitMs);

    // Probe-only (--searchstate-test): nghiem thu 3 loi trang thai tim kiem.
    void probeSearchState(const QString& pathA, const QString& pathB, const QString& query,
                          bool continuous, double zoomPercent, int waitMs);

    // Probe-only (--viewfast-probe, SPEC_VIEWFAST): set fast mode BEFORE openFile.
    void probeSetFastMode(bool on) { setViewMode(on); }
    // Bai do CUON (them 2026-08-31): can cuon that trong Continuous de ep dung tinh huong
    // "render bi cat giua chung" — bai do dung yen khong bao gio cham toi nhanh do.
    void probeScrollToPage(int p);   // dinh nghia trong .cpp (ContinuousView chi khai bao truoc o day)
    void probeSetZoom(double z);      // dat zoom cho ContinuousView (bai do markup+zoom)
    // Probe-only (--zoomanchor-probe 2026-09-21): do lech neo Ctrl+zoom o ContinuousView.
    QString probeZoomAnchor(int page, double zFrom, double zTo, double fx, double fy);
    void probeSetZoomSingle(double z); // dat zoom cho view Single
    // Bai do TAO ANNOT (2026-09-01): di DUNG duong ma nguoi dung di khi bam nut Note/Text.
    bool probeCreateNote(int page, double xPt, double yPt);
    bool probeCreateText(int page, double xPt, double yPt, double wPt, double hPt);
    // 🔴 Chup DUNG khung nhin dang hoat dong. `QWidget::grab()` KHONG lay duoc noi dung ve
    // bang GPU (QOpenGLWidget) — do duoc: so pixel giong het nhau qua moi luot, ke ca khi
    // app that su co doi. Phai dung grabFramebuffer() cua chinh widget GL.
    QImage probeGrabView();
    int  probePageCount() const { return currentTab() ? currentTab()->doc->pageCount() : 0; }
    // Probe-only (--viewfast-probe): navigate to a page (0-based) via onPageChanged.
    void probeSetPage(int page0Based);
    // Probe-only (--viewfast-probe): trang dang xem o che do hien tai.
    int probeCurrentPage() const;
    bool probeIsFastMode() const { return m_fastMode; }
    QString probeMemBreakdown() const;   // bai do bo nho 31/08
    // 🔴 LƯỢT 30 (VIỆC 1 — DO RAM): soi TỪNG kho giữ bộ nhớ để chỉ ra ai chiếm ≥80%.
    // docIdx = chi so trong m_openDocs (tab Welcome khong tinh).
    QString probePoolInfoAll() const;          // pool doc moi tab: slots/everUsed/idle/renders
    int     probeIdlePoolDocsAll() const;      // tong doc RANH moi tab (de lap vong dong)
    int     probeOpenDocCount() const { return m_openDocs.size(); }  // so DocTab (khong tinh Welcome)
    bool    probeContinuousVisible() const;   // LƯỢT 37 muc B: ContinuousView dang AN hay HIEN?
    bool    probeCloseOneIdlePoolDocTab(int docIdx);  // dong 1 doc ranh cua tab docIdx
    QString probeRamBreakdown() const;         // pool + globalCache + PageCache + vector + thumb
    void    probeCloseTab(int idx) { onTabClose(idx); }
    // Probe-only (--owner3tab-probe LƯỢT 17): chi-doc. So tab hien co (Welcome tinh 1)
    // va trang p da co anh trong m_pageImages cua ContinuousView (tinh hieu that
    // "trang render xong co anh" — cung kho "[perf] cont ACCEPT" ghi vao).
    int  probeTabCount() const;
    bool probePageHasImage(int p) const;
    // Probe-only (LƯỢT 31b): scale THAT cua anh trang p trong ContinuousView (-1 chua co).
    double probePageImageScale(int p) const;
    // Probe-only (LƯỢT 37 bước O): co thu nhỏ nen dang bay? / bao nhieu lan guard bo ban cu?
    int    probeContDownscaleInFlight() const;
    qint64 probeContStaleDrops() const;
    // 🔴 LƯỢT 33a (bai do J/K/L) — getter CHỈ-ĐỌC, phuc vu do "nhay ve trang 1 khi
    // doi tab": trang tab LUU (DocTab::currentPage), trang view Single THUC VE,
    // va tam viewport cua ContinuousView dung chung.
    int  probeTabSavedPage(int idx) const { auto* t = m_openDocs.value(idx); return t ? t->currentPage : -1; }
    int  probeTabShownPage(int idx) const;
    bool probeContCenter(int* page, QPointF* c) const;
    // 🔴 LƯỢT 33f (muc 5): offset cuon THAT cua Continuous (dinh = 0) — do vi tri
    // tab dang load bang scrollY, khong bang trang o tam viewport.
    int  probeContScrollY() const;
    // Probe-only (--owner3tab-probe LƯỢT 21): so closeJob `delete t` dang chay —
    // step G can biêt closeJob cuoi cung da xong chưa.
    int  probeCloseJobs() const { return m_thumbCloseJobs; }
    // 🔴 LƯỢT 22b (reviewer mục 1): probe PHẢI bắt được lỗi thiếu `!` ở tabAlive —
    // bản lỗi: initWatcher thoát sớm trên tab CÒN SỐNG ⇒ pdfHash=0; ba lambda quét
    // comment thoát sớm ⇒ annotCache=0 dù file CÓ chú thích (đếm bằng API thật).
    quint64 probePdfHash(int idx) const { auto* t = m_openDocs.value(idx); return t ? t->pdfHash : 0; }
    int   probeCommentCache(int idx) const { auto* t = m_openDocs.value(idx); return t ? int(t->annotCache.size()) : -1; }
    // 🔴 LƯỢT 33e (muc 4 — buoc M, vang lenh CHỈ-DOC): dem overlay visuals cua trang
    // dang hien trong tab — CA HAI view (Single + Continuous) deu ve tu chinh kho
    // DocTab::visualsCache (LAT A 09/02 — view khong giu ban chep rieng), nen day la
    // so visual THUC VE tren man hinh, khong phai so trong danh sach trung gian.
    int   probeOverlayVisualCount(int idx) const { auto* t = m_openDocs.value(idx);
        return t ? int(t->visualsCache.value(t->currentPage).size()) : -1; }
    // So dong comment THUC in trong panel (khong phai gia tri se day vao panel).
    int   probePanelCommentRows() const;
    void  probeRequestComments() { onCommentsRequested(); }
    // Note do PROBE tạo ra là nhân vật chứng, không phải việc của user — gỡ cờ
    // dirty để onTabClose không bật hộp "Unsaved Changes" modal giữa probe.
    void  probeClearDirty(int idx) { if (auto* t = m_openDocs.value(idx)) t->dirty = false; }
    int   probeRealAnnotCount(int idx);   // API thật: AnnotationManager::loadPage từng trang
    // Probe-only (--torcache-probe 0927): dong open-doc theo chi so m_openDocs —
    // tu tinh chi so cua m_docTabs, vi tab 0 la "Welcome" nen chi so cua harness
    // khong dung chi so tab thật.
    void probeCloseOpenDoc(int docIdx);
    // Probe-only (--torcache-probe 0927): goi DUNG duong `taiLai` cua nguoi dung
    // (loadTabFile) de harness kiem chung LOG tren may that ma khong phai viet
    // lai logic. docIdx la chi so trong m_openDocs.
    // 0927 LƯỢT 6: khong con `probeClearCacheSlot` — nút Clear cache đã bị gỡ hẳn.
    bool probeLoadTabFile(int docIdx);
    // Probe-only (--viewfast-twodoc-probe): kich hoat tab doc theo chi so 0-based
    // (giong nguoi dung bam vao tab thu idx) de mo 2 tai lieu cung luc o View Fast.
    void probeActivateTab(int idx);
    // Probe-only (SPEC_THUMB_DISPLAY 31/08): don so thumbnail that GAN duoc vao list
    // cua panel hien tai + so trang cua tab dang hien. In ra de chung minh
    // m_dbgAccepted == pageCount cho moi tab (khong dem "thumb done" nua).
    QString probeThumbCounters() const;
    void    probeResetThumbCounters();
    // Nghiém thu co polling (SPEC_THUMB_DISPLAY 31/08): kich hoat tab idx, quay
    // vong den khi list cua no DAY du trang (hoac het timeout). Khong phu thuoc
    // vao thoi gian mo/dung file (file A0 75 trang rat chậm) => khong flaky.
    // Tra ve "VERIFY tab=<i> pages=<n> VISIBLE=<v> <OK|TIMEOUT>".
    QString probeThumbVerify(int tabIdx, int timeoutMs);

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

    // 0927 LUOT 4 (VIEC C) --sidebar-edit-probe can vao ThumbnailPanel de
    // doc o sua chu thich + mo popup theo DUNG DUONG bam chuot cua nguoi dung.
    // CHI cho harness.
    ThumbnailPanel* probeThumbPanel() const { return m_thumbPanel; }
    // 0927 LUOT 6 (VIEC 1) --ftngoai-edit-probe: sua chu thich qua DUNG DUONG
    // CHUOT PHAI nguoi dung (chuot phai -> "Edit text…"), TU DONG dien `text`
    // vao hop nhap nhieu dong roi bam OK — de harness kiem ma khong can chuot.
    // KHONG sua truc tiep: editSelectedAnnot() la ham thuc su cua menu, nen
    // moi chay dung ma cua nguoi dung (ke ca QMessageBox khi that bai).
    // Tra ve loi (rong = thanh cong). CHI cho harness.
    QString probeEditAnnotViaGui(int page, int indexInList, const QString& text,
                                 QString* errOut = nullptr);
    // 0927 LUOT 6 (VIEC 2) --sidebar-popup-hold [dark]: bat/tat Dark Mode theo
    // DUNG DUONG NUT tren toolbar (QAction "Dark Mode".trigger()) — cung duong
    // voi nguoi dung bam chuot, nen ca qApp stylesheet + ThumbnailPanel::m_dark
    // doi nhat quyen. Tra false neu khong tim thay nut (harness se bao loi).
    bool probeSetDarkMode(bool dark);
    // Nap lai danh sach chu thich cho trang (dung khi probe vua tao annot moi).
    void probeRefreshComments(int page);

    // Probe-only (--markup-mouse-probe): expose methods for annotation testing
    DocTab* probeCurrentTab() const;  // Get current tab
    QWidget* probeCurrentView() const;  // Get PdfGpuView of current tab
    void probeSelectAnnotTool(int id);  // Select markup tool (0=Pan, 1=SelectText, 2=Line, etc.)
    int probeAnnotCount() const;  // Get total number of annotations in current doc
    int probeAnnotCountPerPage(int page) const;  // LƯỢT 40: Get annotations on specific page
    int probeVisualCount(int page) const;  // Get number of overlay visuals for page
    // LƯỢT 39p (Sonnet — probe TEST-ONLY, chi-doc): dang co annot nao dang duoc
    // CHON hay khong (m_selPage/m_selIdx do onAnnotPick() dat khi bam trung annot
    // bang cong cu Pan). Dung de --markup-mouse-probe kiem "co chon duoc markup
    // khac sau khi ve" — KHONG doi hanh vi, chi doc lai hai bien da co san.
    bool probeHasSelection() const { return m_selPage >= 0 && m_selIdx >= 0; }

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

    // 0927 LUOT 11 -- thoat NHANH: xoa ban nhap `.tortmp` cua cac tab, thay the
    // viec xoa trong ~MainWindow (duong cu) ma thoat nhanh bo qua. Chi goi SAU
    // closeEvent. Tra ve SO ban nhap CON LAI (0 = sach). `ponytail: Windows con
    // giu handle PDFium tren file nhap nen xoa co the that bai` — nhung do la ban
    // nhap user da bo, va ~MainWindow cu xoa vo dieu kien nen duong nay khong
    // lo lang: xoa het thuoc ve user da tra loi "Khong" o hop thoat.
    // Chinh sach giu/boa: core/FastExit.h (trExitDraftPolicy).
    int removeDraftsForExit();

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
    // "Trang nay DA HIEN RA tren man hinh" — duoc phat cho ca duong raster (pageReady)
    // va duong vector (setTabVectorLayer). Dung cho harness va cho cho nao can biet
    // trang da san sang MA KHONG quan tam no hien bang raster hay vector.
    void pageDisplayed(int pageIndex);

private:
     void setupActionBar();
     void setViewMode(bool fastMode);
     void applyTheme(bool dark);
    void syncSidebarToTab(int idx, bool forceRebuild = false);
    DocTab* currentTab() const;
    // LƯỢT 35 — man chao mot nguon duy nhat: khi khong con tai lieu nao, an
    // ContinuousView (ve chu tren GL bi vo) va hien tab "Welcome" (PdfView CPU).
    void    applyWelcomeVisibility();
    PdfView* addWelcomeTab();   // tao + them tab chao, dong bo dark mode hien hanh
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
    // 🔴 0928 LƯỢT 16 — băng thumbnail khi `delete t` của tab đang đóng còn chạy
    // nền (MainWindow.cpp closeJob). syncThumbnailPoolsToActiveTab giữ ĐÓNG BĂNG
    // mọi pool kể cả tab hiện hành khi >0 ⇒ đóng tab không phải xếp hàng sau
    // thumbnail tab khác giành khoá PDFium (log r8: chờ 2,9 s, app văng).
    // 🔴 LƯỢT 16b — bool → BỘ ĐẾM số closeJob đang chạy (++ ở onTabClose, --
    // trong closeJob.finished): đóng 2 tab liên tiếp mà job A xong trước job B,
    // bool sẽ mở băng giữa lúc `delete t` của B còn chạy nền — đúng cửa sổ tranh
    // khoá mà lượt 16 muốn chặn.
    int              m_thumbCloseJobs = 0;
    ThumbnailPanel*  m_thumbPanel    = nullptr;
    ContinuousView*  m_continuousView = nullptr;
    QSplitter*       m_splitter      = nullptr;

QAction*   m_viewQualityAct = nullptr;
     QAction*   m_viewFastAct = nullptr;
     QAction*   m_selectTextAct = nullptr;
     QAction*   m_translateAct  = nullptr;
     QAction*   m_darkAct       = nullptr;
     QLineEdit* m_zoomEdit      = nullptr;
     QLabel*    m_hintLabel     = nullptr;
     bool       m_darkMode      = false;
     bool       m_fastMode = true;   // owner chot 2026-08-31: mac dinh Continuous (View Fast)
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
    // Insert Image (SPEC_INSERT_IMAGE_2026-08-30): chen anh thanh Stamp + co gian.
    void onInsertImage();
    void onAnnotResize(DocTab* t, int page, QRectF newRectDisp);
    // Day trang thai chon xuong CA HAI view (gpu + continuous) cho khop dien mao.
    // VIỆC 3-A1 (0921): `isOwn` = annot CỦA TA. Chỉ annot của ta mới hiện tay nắm gốc
    // (co giãn); annot phần mềm khác không hiện, vì guardWrite sẽ từ chối mọi lần ghi.
    void setMarkupSelectionViews(DocTab* t, int page, const QRectF& rectPdf,
                                 const QString& uid, const QString& type, bool isOwn);
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
    // 🔴 LÁT C 0902: CẬP NHẬT CÓ CHỌN LỌC — trộn hàng chờ delta của AnnotationManager
    // vào visualsCache (KHÔNG lấy s_pdfiumMutex, KHÔNG xếp hàng sau bộ dựng trang).
    // true = cache da duoc tron (danh sach moi nam trong *out); false = khong co
    // delta/bo dem → duong nap lai ca trang nhu cu.
    bool mergeAnnotVisuals(DocTab* t, int page, QList<AnnotVisual>* out);
    // Áp kết quả visuals vào renderer + view (cả 2 view). Dùng chung cho đường
    // CACHE HIT (sync) và đường RESCAN (async, áp lại trên UI thread).
    void vaVungChuThich(DocTab* t, int page, const QList<AnnotVisual>& visuals);
    void applyAnnotVisuals(DocTab* t, int page,
                           const QList<AnnotVisual>& visuals,
                           bool overlayCapable, bool hasForeign);
    bool canFastPath(DocTab* t, int page) const;
    bool baseIsVector(DocTab* t, int page) const;
    // Nen lop vector cua view dang ve. LÁT B 09/02: Single va Continuous CUNG DOC mot
    // kho DocTab::vecLayers, nen "nen vector" chi con mot nghiã — khong con "cua view
    // nao". Root cua "Comment mat" (30-08) neu thieu chot nay.
    bool pageBaseIsVector(DocTab* t, int page) const;
    // Trang co markup khong overlay duoc (canFastPath false) thi phai ve bang raster:
    // lop vector lam nen se chan raster (drop reason=vectorReady) va markup VO HINH.
    // Danh dau trang + go lop vector ra ngay neu dang dung.
    void forceRasterPage(DocTab* t, int pg);
    // R2 (SPEC_PERF_HEAVYPAGE): gan lop vector cho view + cap nhat chan raster full-quality.
    // Lop ready+complete cua trang hien tai => huy lenh full-quality dang chay + chan lenh moi.
    // Lop moi thay the lop cua trang khac => bo chan trang cu (raster quay lai binh thuong).
    void setTabVectorLayer(DocTab* t, std::shared_ptr<VectorLayer> layer, int pg);
    // Cac viec phai lam KHI TRANG DA HIEN RA, bat ke no hien bang raster hay bang lop vector.
    // Duong raster goi tu handler pageReady; duong vector goi tu setTabVectorLayer.
    void finishPageDisplay(DocTab* t, int idx);
    // R3 (SPEC_PERF_HEAVYPAGE muc R3.2): tam dung thumbnail khi mo file de trang
    // dang xem dung s_pdfiumMutex cho ban dung vector cua CHINH trang do. Kem dong
    // ho an toan 3s tu go tam dung bat ke ban dung vector co xong hay khong.
    void pauseThumbnails(DocTab* t);
    // Chi tab DANG HIEN moi duoc render thumbnail. Thumbnail cua tab an khong ai nhin
    // ma van an khoa pdfium toan cuc, lam moi thao tac cua nguoi dung phai xep hang sau
    // no (do that 2026-08-19: 94 giay render thumbnail, 30 cai bi vut thang).
    void syncThumbnailPoolsToActiveTab();

    const QList<AnnotInfo>& annotsForPage(DocTab* t, int page, bool* outOk = nullptr);
    void invalidateAnnotPage(DocTab* t, int page);
    // LÁT C 09/02: ve chu thich theo VUNG NHIN cho trang nang nen vector (region,
    // ~20ms) thay lop bu toan trang. Goi theo SU KIEN: DeferredUpdate (pan),
    // zoomChanged, doi trang, invalidateAnnotPage. Tu guard — goi vo hai khi trang
    // khong heavy / khong phai tab hien hanh / vung nhin khong doi.
    void updateHeavyRegion(DocTab* t);
    // LAT G: dong handle rieng (goi khi KHONG co tac vu dang chay).
    // 0928 LƯỢT 26: static — chi cham t-> + khoa PDFium toan cuc ⇒ goi duoc o
    // closeJob nen (duong "dongTab" doi tail PDFium xuong nen, UI khong cho khoa).
    static void closeHeavyPriv(DocTab* t);
    void buildVectorLayer(DocTab* t, int pageIndex, bool force = false);
    void ensureForeignAnnotLayer(DocTab* t, int pageIndex);
    void cancelForeignAnnotTasks(DocTab* t);  // cancel+wait cac tac vu lop bu truoc khi dong doc
    // 0927 LƯỢT 8: ĐƯỜNG THOÁT DUY NHẤT cho MỘT tab — dùng chung cho đóng tab (nút X)
    // và thoát app (Alt+F4). Xem thân hàm để đọc thứ tự. KHÔNG `delete t` ở đây: hai
    // đường đó xoá tab ở nơi khác (nền / trực tiếp).
    // 🔴 0928 LƯỢT 26 (VIỆC 2): `renderWaitMs` = trần chờ render dừng TRÊN LUỒNG UI.
    // Đóng tab ("dongTab") truyền 300 + `pdfiumTailOnUi=false` — phần chờ còn lại đã
    // có trong closeJob nền (~PdfRenderer::waitIdle + beginClose theo token), và tail
    // chạm PDFium (closeHeavyPriv/TextSelection) chạy luôn ở nền đó.
    // Thoát app/nạp lại giữ nguyên 3000 + tail-on-UI.
    void shutdownTab(DocTab* t, const char* why, int renderWaitMs = 3000,
                     bool pdfiumTailOnUi = true, bool waitBgOnUi = true);
    // 🔴 0928 LƯỢT 22 — chốt "tab còn sống" cho lambda queued: ĐỊA CHỈ còn phải
    // KHỚP THẾ HỆ (serial chụp lúc spawn). Địa chỉ DocTab vừa free có thể được
    // malloc cấp lại cho tab mới — contains() thuần địa chỉ sẽ chạm nhầm tab mới.
    bool tabAlive(DocTab* t, quint64 serial) const {
        return m_openDocs.contains(t) && t->serial == serial;
    }
    // 0903: dung bo dung thumbnail cua tab + CHO worker thoat han TRUOC khi doc bi
    // dong/giai phong hoac UI cham PDFium. Goi o MOI duong giai phong tai lieu.
    void stopThumbPool(DocTab* t);
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
void cleanupOrphanTorcache();

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
