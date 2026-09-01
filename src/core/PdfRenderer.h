#pragma once
#include <QObject>
#include <QHash>
#include <QSet>
#include <QImage>
#include <QRect>
#include <QMutex>
#include <QMutexLocker>
#include <QSemaphore>
#include <QRunnable>
#include <QAtomicInt>
#include <QThreadPool>
#include <memory>
#include <functional>
#include <atomic>
#include <vector>
#include <QElapsedTimer>
#include <QList>
#include <QPair>
#include <fpdf_progressive.h>
#include "PdfDocument.h"
#include "TileCacheFile.h"

struct RenderRequest {
    int    pageIndex;
    double scaleFactor;
    bool   renderAnnotations;
    bool   fullQuality = false;
    int    generation;
    bool   hideOwnAnnots = false;
    bool   useZoomScale = false;   // GOC1: use scaleFactor instead of kFullRenderMaxPx
};

class PdfRenderer;
class ProgressiveRenderTask;

class PageRenderTask : public QObject, public QRunnable {
    Q_OBJECT
public:
    PageRenderTask(PdfRenderer* renderer, PdfDocument* pdfDoc, RenderRequest req,
                   QObject* receiver, std::shared_ptr<QAtomicInt> genRef);
    void run() override;
signals:
    void finished(int pageIndex, QImage image);
    void pageObjectCount(int pageIndex, int count);
private:
    PdfRenderer*                 m_renderer = nullptr;
    PdfDocument*                 m_pdfDoc = nullptr;
    RenderRequest                m_req;
    std::shared_ptr<QAtomicInt>  m_genRef;
};

struct ProgressivePauseCtx {
    QElapsedTimer timer;
    static int sliceMsFromEnv();
    int sliceMs = sliceMsFromEnv();
};

inline FPDF_BOOL ProgressiveNeedToPauseNow(IFSDK_PAUSE* pThis) {
    auto* ctx = static_cast<ProgressivePauseCtx*>(pThis->user);
    return ctx->timer.elapsed() >= ctx->sliceMs;
}

class ProgressiveRenderTask : public QObject, public QRunnable {
    Q_OBJECT
public:
    ProgressiveRenderTask(PdfRenderer* renderer, PdfDocument* pdfDoc, RenderRequest req,
                          QObject* receiver, std::shared_ptr<QAtomicInt> genRef);
    ~ProgressiveRenderTask() override;
    void run() override;
signals:
    void pagePartial(int pageIndex, double scale, QImage image);
    void finished(int pageIndex, QImage image);
    void pageObjectCount(int pageIndex, int count);
private:
    PdfRenderer*                 m_renderer;
    PdfDocument*                 m_pdfDoc;
    RenderRequest                m_req;
    std::shared_ptr<QAtomicInt>  m_genRef;
    FPDF_BITMAP                  m_bmp = nullptr;
    FPDF_PAGE                    m_fpdfPage = nullptr;
    FPDF_DOCUMENT                m_poolHandle = nullptr;  // VIỆC B: handle từ pool (nếu dùng)
    int                          m_bmpW = 0;
    int                          m_bmpH = 0;
    int                          m_renderStatus = FPDF_RENDER_READY;
    double                       m_renderScale = 0.0;
    QImage                       m_image;
    QElapsedTimer                m_lastEmitTimer;
    double scaleFactor() const { return m_renderScale; }
};

class RegionRenderTask : public QObject, public QRunnable {
    Q_OBJECT
public:
    RegionRenderTask(PdfRenderer* renderer, PdfDocument* pdfDoc,
                     int pageIndex, double scale, QRect regionPx,
                     QObject* receiver,
                     std::shared_ptr<QAtomicInt> genRef,
                     bool renderAnnotations = true,
                     bool hideOwnAnnots = false);
    void run() override;
signals:
    void finished(int pageIndex, double scale, QRect regionPx, QImage img);
private:
    PdfRenderer*                m_renderer = nullptr;
    PdfDocument*                m_pdfDoc = nullptr;
    int                         m_pageIndex;
    double                      m_scale;
    QRect                       m_regionPx;
    std::shared_ptr<QAtomicInt> m_genRef;
    int                         m_reqGen = 0;
    bool                        m_renderAnnotations = true;
    bool                        m_hideOwnAnnots = false;
    FPDF_PAGE                   m_fpdfPage = nullptr;
    FPDF_BITMAP                 m_bmp = nullptr;
    int                         m_bmpW = 0;
    int                         m_bmpH = 0;
    int                         m_renderStatus = FPDF_RENDER_READY;
    QImage                      m_image;
};

class TileBatchRenderTask : public QObject, public QRunnable {
    Q_OBJECT
public:
    TileBatchRenderTask(PdfRenderer* renderer, PdfDocument* pdfDoc,
                        int pageIndex, double scale,
                        QVector<QPoint> tiles,
                        QObject* receiver,
                        std::shared_ptr<QAtomicInt> genRef,
                        bool renderAnnotations = true,
                        bool hideOwnAnnots = false);
    void run() override;
signals:
    void tileDone(int page, double scale, int col, int row, QImage img);
private:
    static constexpr int kTileSize = 512;
    PdfRenderer*                m_renderer = nullptr;
    PdfDocument*                m_pdfDoc = nullptr;
    int                         m_pageIndex;
    double                      m_scale;
    QVector<QPoint>             m_tiles;
    std::shared_ptr<QAtomicInt> m_genRef;
    int                         m_reqGen = 0;
    bool                        m_renderAnnotations = true;
    bool                        m_hideOwnAnnots = false;
};

struct TileKey {
    int    page;
    double qscale;
    int    col;
    int    row;
    bool operator==(const TileKey& o) const {
        return page == o.page && qAbs(qscale - o.qscale) < 1e-9 && col == o.col && row == o.row;
    }
};
inline size_t qHash(const TileKey& k, size_t seed = 0) {
    return qHash(k.page, qHash(k.col, qHash(k.row, qHash(size_t(k.qscale * 1000), seed))));
}

struct RegionEntry { QRect rect; QImage img; };

class PdfRenderer : public QObject {
    Q_OBJECT
    friend class ProgressiveRenderTask;
public:
    explicit PdfRenderer(QObject* parent = nullptr);
    ~PdfRenderer() override;

    void setDocument(PdfDocument* doc);
    void requestPage(int pageIndex, double scale);
    void requestTiles(int page, double scale, QRect viewportPx);
    void preloadAdjacent(int pageIndex, double scale);
void cancelPending();
    // Huy RIENG cac lenh render che do lien tuc dang chay (khong dong toi thumbnail,
    // preload hay tile). Dung khi nguoi dung cuon sang trang khac: viec cu thanh vo nghia.
    void cancelContinuous();
    // VIỆC 2 (SPEC_SMOOTH_123 31/08): huy dung MOT trang dang render trong che do lien
    // tuc (tra khe ve), khong dong toi cac trang khac. ContinuousView goi khi trang do
    // khong con nhin thay nua. Khong duoc huy trang con visible — chi co cancelContinuous()
    // moi huy sach (dung cho zoom change).
    void cancelContinuousPage(int page);
    // Trang nay co dang xep hang / dang render trong che do lien tuc khong?
    bool isContinuousPageActive(int page) const;
    // Chi xoa HANG DOI (yeu cau chua chay) — KHONG dung bo dem the he, nen render DANG CHAY
    // van song. Dung khi trang chinh doi ma trang cu VAN CON NHIN THAY: viec dang lam do van
    // co gia tri, huy di la phi. (cancelContinuous() lam ca hai: xoa hang doi + giet dang chay.)
    void cancelContinuousQueueOnly();
    // R2 (SPEC_PERF_HEAVYPAGE): chi huy render FULL-QUALITY dang chay (m_fullRenderGen),
    // khong dong toi thumbnail, preload hay tile.
    void cancelFullQuality();
    // R2: chan lenh render full-quality moi cho trang da co lop vector san sang.
    // MainWindow goi khi lop vector cua trang do ready+complete (suppress=true) va khi
    // lop bi bo / trang doi / build that bai (suppress=false).
    void setSuppressFullQuality(int pageIndex, bool suppress);
    void clearStalePending();
    void clearCache();
    void invalidatePage(int pageIndex);
    void setCurrentPage(int page);
    void setTileCache(std::shared_ptr<TileCacheFile> cache);
    void requestRegion(int pageIndex, double scale, QRect regionPx);
    // Like requestPage but only checks memory/disk cache — emits pageReady if hit,
    // returns true on hit, false if no cache found (no render started).
    bool requestFromCacheOnly(int pageIndex, double scale);
    // Like requestFromCacheOnly but for continuous mode: memory cache ONLY (no disk I/O),
    // validates image dimensions match requested scale, emits continuousPageReady.
    bool requestFromCacheOnlyForContinuous(int pageIndex, double scale);
    // Continuous-mode render: no newest-wins, multiple pages can render concurrently.
    // Passes through to ProgressiveRenderTask without bumping the full-quality gate.
    // priority: 0 = trang chinh + 1 lan can (lam TRUOC), 2 = trang xa (xep SAU).
    void requestPageForContinuous(int pageIndex, double scale, int priority = 2);

    static std::atomic<int> s_renderCount;
    FPDF_PAGE acquirePage(int pageIndex);
    // Cap doi cua acquirePage() — giam borrow cua trang trong PageCache (SPEC R1).
    void releasePage(int pageIndex);
    void setPageObjectCount(int pageIndex, int count) { m_pageObjectCount[pageIndex] = count; }

    // 🔴🔴 2026-09-01 — GOC CUA "tao Note/Text xong khong hien gi".
    // Render chay tren POOL HANDLE: mot ban sao tai lieu RIENG (`FPDF_LoadMemDocument` tren
    // byte goc cua tep). Chu thich vua tao nam trong tai lieu DANG MO TRONG BO NHO, ban sao
    // do KHONG HE CO ⇒ ve bao nhieu lan cung khong ra. Do bang pixel: 0 thay doi.
    // ⇒ Trang nao da co chu thich moi thi PHAI ve tu tai lieu CHINH, khong dung ban sao.
    // 🔴🔴 2026-09-01: nap lai cac BAN SAO tai lieu sau khi sua chu thich.
    // Vi sao can: bo ve chay tren ban sao rieng (pool) nen KHONG thay chu thich vua tao. Truoc
    // do toi vong qua bang cach bat trang do ve tu TAI LIEU CHINH — nhung duong ve tien-dan
    // NHA KHOA giua cac lat, nen luong giao dien co the sua tai lieu ngay giua mot luot ve
    // ⇒ PDFium tu bung loi (0x80000003) roi hong heap (0xc0000374). Da thay trong Event Log.
    // ⇒ Tra lai cach ly: nap lai ban sao tu tep DA LUU. Ban sao co chu thich moi, va moi luong
    //   ve lai co tai lieu rieng nhu cu — khong con dua nhau.
    void reloadHandlePool();

    void markAnnotDirty(int pageIndex) {
        QMutexLocker lk(&m_annotDirtyMx);
        m_annotDirtyPages.insert(pageIndex);
    }
    QList<int> annotDirtyPages() const {
        QMutexLocker lk(&m_annotDirtyMx);
        return QList<int>(m_annotDirtyPages.cbegin(), m_annotDirtyPages.cend());
    }
    bool pageAnnotDirty(int pageIndex) const {
        QMutexLocker lk(&m_annotDirtyMx);
        return m_annotDirtyPages.contains(pageIndex);
    }
    int  pageObjectCount(int pageIndex) const { return m_pageObjectCount.value(pageIndex, 0); }
    void setPageAnnotRender(int page, bool on) { m_pageAnnotRender[page] = on; }
    void setPageAnnotOverlay(int page, bool on) { m_pageAnnotOverlay[page] = on; }
    int pageAnnotRenderOf(int page) const  { return m_pageAnnotRender.value(page, true) ? 1 : 0; }
    int pageAnnotOverlayOf(int page) const { return m_pageAnnotOverlay.value(page, false) ? 1 : 0; }

    // VIỆC THUMBNAIL SONG SONG: public wrapper để ThumbnailWorker mượn/trả pool handle
    // true = pool handle chi-doc dang san sang => thumbnail KHONG con tranh s_pdfiumMutex,
    // nen khong con ly do tam dung thumbnail luc mo file (xem MainWindow::pauseThumbnails).
    bool hasPoolHandles() const;
    FPDF_DOCUMENT borrowPoolHandleForThumbnail();
    void returnPoolHandleForThumbnail(FPDF_DOCUMENT handle);

    // ponytail: only 1 full-quality render at a time; newest-wins

    // Full-quality renders: 4000px long edge. Resolution is free for PDFium
    // (content-bound, not pixel-bound). Benchmark on page 4 (4.5M drawing
    // commands): 200px=3105ms, 800px=3049ms, 4000px=3265ms.
    // DO NOT lower — it would NOT save render time, only degrade quality.
    static constexpr double kFullRenderMaxPx = 4000.0;
    // DPI cua Continuous (owner chot 2026-08-31: siet theo DPI tung trang, khong tran pixel).
    // Doi theo luot: TORREADER_CONT_DPI=<n>. Mac dinh 150.
    static qint64 globalCacheBytes();   // bai do bo nho
    qint64        tabCacheBytes() const;      // bo dem anh cua RIENG tab nay
    static int    contDpi();
    // So pixel canh dai KY VONG cho mot trang o DPI hien hanh. MOT nguon su that duy nhat —
    // dung cho ca luc RENDER, luc GHI dem va luc DOC dem. Truoc day 3 cho dung 3 cong thuc
    // khac nhau nen chot ghi-dem so voi 4000px da CHAN SACH viec ghi dem o 75 DPI.
    static double contTargetLongPx(double longSidePt);
    // Thumbnail renders cap at this long-edge pixel count to avoid 43MB
    // thumbnail images for the page-navigation panel.
    // 2026-08-31: 400px lam thumbnail trang A0 ra 357x252 — owner bao "do phan giai rat thap".
    // Nang len 900: van re (900x637x4 = 2,3 MB/trang) va du net tren man hinh scale 200%.
    static constexpr double kThumbMaxPx = 900.0;  // moc TOT owner xac nhan 31/08

    // GOC1: when true, continuous renders use zoom-based scale (capped at
    // kFullRenderMaxPx) instead of always kFullRenderMaxPx.
    void setContinuousUseZoomScale(bool on) { m_continuousUseZoomScale = on; }

    // Exposed for MainWindow to show placeholder immediately on page navigation
    QImage bestCachedForPage(int pageIndex) const;

signals:
    void pageReady(int pageIndex, QImage image);
    void continuousPageReady(int pageIndex, QImage image, double renderedScale);
    void pagePartial(int pageIndex, double scale, QImage image);
    void regionReady(int pageIndex, double scale, QRect regionPx, QImage img);
    void tileReady(int page, double scale, int col, int row, QImage img);
    void objectCountReady(int pageIndex, int count);

private:
    // VIỆC B: Pool handle chỉ-đọc cho render song song. Mở N handle từ cùng file,
    // mỗi luồng thợ dùng handle riêng => render không cần s_pdfiumMutex.
    // KHÔNG được dùng pool handle cho: SỬA annotation, lưu file, OCR, tìm kiếm.
    struct PooledHandle {
        FPDF_DOCUMENT doc = nullptr;
        bool inUse = false;
    };
    std::vector<PooledHandle> m_handlePool;
    mutable QMutex m_handlePoolMutex;
    // 🔴 2026-08-31: 3 la QUA IT. Do duoc: ~18 tho cung tranh (m_thumbPool 8 luong tren may
    // 16 nhan + m_mainPool 4 + kMaxContinuousRenders 6) ma chi co 3 handle => phan lon phai
    // lui ve s_pdfiumMutex => van NOI TIEP nhu cu, do la vi sao thumbnail lau nhat van ~9,6s.
    // Nang len 12 gan bang so tho. GIA RE: moi handle mo bang FPDF_LoadMemDocument tren CUNG
    // mot vung nho da anh xa (m_doc->mmapData()), nen KHONG ton them ban sao file 58 MB —
    // chi ton cau truc noi bo cua PDFium.
    // 🔴 HA 12 -> 4 (2026-08-31): owner do duoc app ngon 6 GB RAM voi 2 tab. Toi da GIA DINH SAI
    // rang them handle la "re" vi chung vung nho mmap — thuc te PDFium dung CAU TRUC RIENG cho
    // moi handle. 12 handle x 2 tab = 24 ban => no RAM. 4 la du de render song song ma khong phinh.
    static constexpr int kPoolSize = 12;  // moc TOT owner xac nhan 31/08
    // 🔴 HAN MUC RIENG CHO THUMBNAIL (2026-08-31). Da THU va THAT BAI khi khong co han muc:
    // dung san 75 thumbnail cung luc thi chung chiem SACH 12 handle, render trang bi day ve
    // o khoa chung => thumbnail TB 247 -> 1.796 ms, lau nhat 5.250 -> 17.135 ms, so thumbnail
    // xong TUT 75 -> 21. Bai hoc: 12 handle la tai nguyen CHUNG, them thong luong cho mot viec
    // la lay mat cua viec khac. Nen chia truoc: thumbnail toi da 4/12, render trang luon con >= 8.
    static constexpr int kThumbHandleQuota = 4;
    int m_thumbHandlesInUse = 0;

    void initHandlePool();
    void closeHandlePool();
    FPDF_DOCUMENT borrowPoolHandle();
    void returnPoolHandle(FPDF_DOCUMENT handle);

    static double quantize(double scale) {
        return std::round(scale * 20.0) / 20.0;
    }
    void evictCache();
    bool isHeavyPage(int pageIndex);
    void cacheInsert(int pageIndex, const QImage& img);

    PdfDocument*                m_doc = nullptr;
    QHash<int, QImage>          m_cache;             // key = pageIndex
    QHash<qint64, qint64>       m_pendingRequests;  // compositeKey(pageIndex,fullQuality) → timestamp (ms)
    std::shared_ptr<QAtomicInt> m_regionGeneration;
    std::shared_ptr<QAtomicInt> m_generation;
    std::shared_ptr<QAtomicInt> m_tileGeneration;
    QThreadPool                 m_thumbPool;
    QThreadPool                 m_mainPool;
    // Ngan sach cache anh trang, DUNG CHUNG cho MOI tab (khong phai moi tab 1.5GB).
    // 1 trang 4000x2825 ARGB = 43 MB. Ngan sach tinh THEO RAM may luc khoi dong:
    // 12% tong RAM vat ly, kep trong [512 MB, 2 GB]; khong lay duoc RAM thi mac dinh 1 GB.
    // (2026-08-30: 384 MB co dinh qua nho — 3 tai lieu A0 (~135 MB/tai lieu) la vuot tran,
    // don-them-don lien tuc => tai lieu thu 3 khong bao gio len. May owner 31,8 GB RAM.)
    static qint64               maxCacheBytes();
    static std::atomic<qint64>  s_globalCacheBytes;   // tong byte cache cua TAT CA PdfRenderer

    qint64         m_cacheBytes  = 0;
    int            m_currentPage = 0;
    int            m_lastTilePage  = -1;
    double         m_lastTileScale = 0.0;
    int            m_regionInFlightPage = -1;
    double         m_regionInFlightScale = 0.0;
    QRect          m_regionInFlightRect;
    std::shared_ptr<TileCacheFile> m_tileCache;
    QSet<int>                   m_writingCache;      // pages currently being written to disk cache

    // Tile cache for light pages (heavy pages skip tiles entirely)
    static constexpr int        kTileSize       = 512;
    static constexpr qint64     kMaxTileBytes   = 150LL * 1024 * 1024;
    QHash<TileKey, QImage>      m_tileCacheInMem;
    qint64                      m_tileCacheBytes = 0;
    QSet<TileKey>               m_pendingTiles;

    // Region render cache — caches rendered regions (QRect + QImage) per page/scale.
    // Key: TileKey with col/row snapped to kRegionGrid (512px) grid.
    static constexpr int      kRegionGrid = 512;
    static constexpr qint64   kMaxRegionCacheBytes = 256LL * 1024 * 1024;
    QHash<TileKey, RegionEntry> m_regionCache;
    QList<TileKey>            m_regionOrder;   // insertion order for LRU eviction
    qint64                    m_regionCacheBytes = 0;
    static TileKey regionKey(int page, double scale, const QRect& r);
    void evictRegionCache();

    // Heavy page detection: caches FPDFPage_CountObjects result per page
    QHash<int, int> m_pageObjectCount;
    static constexpr int kHeavyObjectThreshold = 100000;

    std::shared_ptr<QAtomicInt> m_fullRenderGen;
    std::shared_ptr<QAtomicInt> m_progressiveGen;
    std::atomic<bool>           m_fullRenderRunning{false};
    std::atomic<int>            m_fullRenderPage{-1};
    double                      m_fullRenderScale = 1.0;
    std::shared_ptr<QAtomicInt> m_continuousGen;
    std::atomic<int>            m_continuousRunning{0};
    // Dem so lan mot trang tra ve anh RONG (bi cat giua chung). Cat giua chung => lat ve DO
    // cuoi cung nam lai tren man hinh VINH VIEN (khung ten ban ve o goc la phan ve sau cung
    // nen mat dau tien). Phai dat lai lenh render, nhung co tran de khong lap vo tan.
    QHash<int,int>              m_contNullRetry;
    static constexpr int        kContNullRetryMax = 3;
    // VIỆC 2: per-page generation for cancelContinuousPage — bump one page's gen
    // to drop its in-flight render without affecting other pages.
    QHash<int, std::shared_ptr<QAtomicInt>> m_contPageGen;
    // VIỆC 2 (SPEC_SMOOTH_123 31/08): hang doi co UU TIEN (0=trang chinh + lan can lam
    // TRUOC, 2=trang xa xep SAU) va huy theo TUNG TRANG. QList de duyet de luon giu do
    // co tiep tuc duoc cap nhat (trang cu lenh cancel se bi loai khi chay).
    struct ContJob {
        int    page;
        double scale;
        int    priority;   // 0 = lam truoc (trang chinh/lan can), 2 = xa
    };
    QList<ContJob>              m_continuousQueue;
    // R2: cac trang co lop vector ready+complete -> khong phai phat lenh render full-quality
    // cho chung nua (chi ap dung cho trang chinh single-page; continuous khong doi).
    QSet<int>                   m_suppressedFullQuality;
    // 2026-08-31: 2 khe la QUA IT cho mot khung nhin hien nhieu trang — chinh vi the ma
    // ContinuousView phai HUY SACH moi lan trang chinh doi (cach duy nhat giai phong khe),
    // va do la goc cua "mo file cho 30 giay". Nang len 6 de con cho GIU render trang dang hien.
    static constexpr int        kMaxContinuousRenders = 6;   // moc TOT 31/08
    QHash<int, bool>            m_pageAnnotRender;
    QHash<int, bool>            m_pageAnnotOverlay;
    QSet<int>                   m_annotDirtyPages;   // xem markAnnotDirty()
    mutable QMutex              m_annotDirtyMx;
    bool                        m_continuousUseZoomScale = false;  // GOC1

};
