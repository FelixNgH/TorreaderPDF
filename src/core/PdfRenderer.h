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
};

class PdfRenderer;

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
    int sliceMs = 50;
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
public:
    explicit PdfRenderer(QObject* parent = nullptr);
    ~PdfRenderer() override;

    void setDocument(PdfDocument* doc);
    void requestPage(int pageIndex, double scale);
    void requestTiles(int page, double scale, QRect viewportPx);
    void preloadAdjacent(int pageIndex, double scale);
    void cancelPending();
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
    void requestPageForContinuous(int pageIndex, double scale);

    static std::atomic<int> s_renderCount;
    FPDF_PAGE acquirePage(int pageIndex);
    void setPageObjectCount(int pageIndex, int count) { m_pageObjectCount[pageIndex] = count; }
    int  pageObjectCount(int pageIndex) const { return m_pageObjectCount.value(pageIndex, 0); }
    void setPageAnnotRender(int page, bool on) { m_pageAnnotRender[page] = on; }
    void setPageAnnotOverlay(int page, bool on) { m_pageAnnotOverlay[page] = on; }

    // ponytail: only 1 full-quality render at a time; newest-wins

    // Full-quality renders: 4000px long edge. Resolution is free for PDFium
    // (content-bound, not pixel-bound). Benchmark on page 4 (4.5M drawing
    // commands): 200px=3105ms, 800px=3049ms, 4000px=3265ms.
    // DO NOT lower — it would NOT save render time, only degrade quality.
    static constexpr double kFullRenderMaxPx = 4000.0;
    // Thumbnail renders cap at this long-edge pixel count to avoid 43MB
    // thumbnail images for the page-navigation panel.
    static constexpr double kThumbMaxPx = 400.0;

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
    static double quantize(double scale) {
        return std::round(scale * 20.0) / 20.0;
    }
    void evictCache();
    bool isHeavyPage(int pageIndex);

    PdfDocument*                m_doc = nullptr;
    QHash<int, QImage>          m_cache;             // key = pageIndex
    QHash<qint64, qint64>       m_pendingRequests;  // compositeKey(pageIndex,fullQuality) → timestamp (ms)
    std::shared_ptr<QAtomicInt> m_regionGeneration;
    std::shared_ptr<QAtomicInt> m_generation;
    std::shared_ptr<QAtomicInt> m_tileGeneration;
    QThreadPool                 m_thumbPool;
    QThreadPool                 m_mainPool;
    // Ngan sach cache anh trang, DUNG CHUNG cho MOI tab (khong phai moi tab 1.5GB).
    // 1 trang 4000x2825 ARGB = 43 MB  =>  1.5 GB giu duoc ~35 trang.
    static constexpr qint64     kMaxCacheBytes      = 384LL * 1024 * 1024;
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
    static constexpr int        kMaxContinuousRenders = 2;
    QHash<int, bool>            m_pageAnnotRender;
    QHash<int, bool>            m_pageAnnotOverlay;

};
