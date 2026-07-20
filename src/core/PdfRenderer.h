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
#include "PdfDocument.h"
#include "TileCacheFile.h"
#include "formibpdf.h"

// Thread-safe shared ownership of FormibDoc (auto-closes via custom deleter)
using FormibDocPtr = std::shared_ptr<FormibDoc>;
inline FormibDocPtr makeFormibDoc(FormibDoc* raw) {
    return FormibDocPtr(raw, [](FormibDoc* p){ if (p) formibpdf_close(p); });
}

// Adaptive verdict cache for FormibPDF. Shared (shared_ptr) between PdfRenderer
// and all PageRenderTasks. FormibPDF is a young renderer: it's fast for simple
// text/vector PDFs but fails (blank/garbled) on complex content. Without this,
// every failing page wastes a full parallel render before falling back to PDFium
// — making the app SLOWER and heavier than pure PDFium. This gate makes the app
// learn per-page and, if FormibPDF fails too often early, disables it for the
// whole document so the app degrades gracefully to plain PDFium speed.
struct FormibGate {
    std::atomic<int>  attempts{0};
    std::atomic<int>  rejects{0};
    std::atomic<bool> disabled{false};
    QMutex            mtx;
    QSet<int>         rejectedPages;   // pages where FormibPDF was already rejected

    // True if FormibPDF is worth trying for this page.
    bool shouldTry(int page) {
        if (disabled.load()) return false;
        QMutexLocker lk(&mtx);
        return !rejectedPages.contains(page);
    }
    void recordAccept() { attempts.fetch_add(1); }
    void recordReject(int page) {
        int a = attempts.fetch_add(1) + 1;
        int r = rejects.fetch_add(1) + 1;
        { QMutexLocker lk(&mtx); rejectedPages.insert(page); }
        // Learning window: after 8 attempts, if >70% failed, give up on FormibPDF
        // for this document entirely (avoids endless double-work on a file it
        // simply can't render).
        if (a >= 8 && static_cast<double>(r) / a > 0.70)
            disabled.store(true);
    }
    // Deliberate skip (e.g. oversized page) — don't retry, but don't count it
    // against the disable ratio (it isn't a FormibPDF quality failure).
    void skipPage(int page) {
        QMutexLocker lk(&mtx);
        rejectedPages.insert(page);
    }
};

struct RenderRequest {
    int    pageIndex;
    double scaleFactor;
    bool   renderAnnotations;
    int    generation;
};

// PDFium is NOT thread-safe — all FPDF_* calls must be serialized through
// s_pdfiumMutex (defined in PdfDocument.cpp). Single-doc path only.
// Decision 2026-07-04: no DocPool, no per-page mutex.

class PageRenderTask : public QObject, public QRunnable {
    Q_OBJECT
public:
    PageRenderTask(FPDF_DOCUMENT doc, PdfDocument* pdfDoc, RenderRequest req,
                   QObject* receiver, std::shared_ptr<QAtomicInt> genRef,
                   FormibDocPtr formibDoc = nullptr,
                   std::shared_ptr<FormibGate> gate = nullptr);
    void run() override;
signals:
    void finished(int pageIndex, QImage image);
private:
    FPDF_DOCUMENT                m_doc;
    PdfDocument*                 m_pdfDoc = nullptr;
    RenderRequest                m_req;
    std::shared_ptr<QAtomicInt>  m_genRef;

    FormibDocPtr                 m_formibDoc;
    std::shared_ptr<FormibGate>  m_gate;
};

// ── RegionRenderTask ───────────────────────────────────────────────────────────
class RegionRenderTask : public QObject, public QRunnable {
    Q_OBJECT
public:
    RegionRenderTask(FPDF_DOCUMENT doc, PdfDocument* pdfDoc,
                     int pageIndex, double scale, QRect regionPx,
                     QObject* receiver,
                     std::shared_ptr<QAtomicInt> genRef);
    void run() override;
signals:
    void finished(int pageIndex, double scale, QRect regionPx, QImage img);
private:
    FPDF_DOCUMENT               m_doc;
    PdfDocument*                m_pdfDoc = nullptr;
    int                         m_pageIndex;
    double                      m_scale;
    QRect                       m_regionPx;
    std::shared_ptr<QAtomicInt> m_genRef;
    int                         m_reqGen = 0;
};

class PdfRenderer : public QObject {
    Q_OBJECT
public:
    explicit PdfRenderer(QObject* parent = nullptr);
    ~PdfRenderer() override;

    void setDocument(PdfDocument* doc);
    /// Called from background thread after formibpdf_open() completes.
    /// Thread-safe: sets m_formibDoc and is only called before any render tasks start.
    void setFormibDoc(FormibDocPtr doc);
    void requestPage(int pageIndex, double scale);
    void preloadAdjacent(int pageIndex, double scale);
    void cancelPending();
    // Clear the pending-request tracking set without cancelling running tasks.
    // Stale tasks (generation mismatch) return early without emitting finished,
    // leaving their key stuck in m_pendingRequests forever. Call this on each
    // scroll tick so those stuck keys are released and pages can be re-requested.
    void clearStalePending();
    void clearCache();
    void setCurrentPage(int page);
    void setTileCache(TileCacheFile* cache);
    void requestRegion(int pageIndex, double scale, QRect regionPx);

signals:
    void pageReady(int pageIndex, QImage image);
    void regionReady(int pageIndex, double scale, QRect regionPx, QImage img);

private:
    static double quantize(double scale) {
        return std::round(scale * 20.0) / 20.0;
    }
    static qint64 cacheKey(int pageIndex, double qscale) {
        return static_cast<qint64>(pageIndex) * 10000LL
               + static_cast<int>(qscale * 100.0);
    }

    QImage bestCachedForPage(int pageIndex) const;
    void evictCache();
    /// Insert into m_cache with single-zoom eviction: when page P is cached at
    /// zoom Z, remove all other zoom levels for page P. Keeps only the most
    /// recently requested zoom per page. Cap: kMaxCacheBytes ~ 80 MB.
    void insertCache(int pageIndex, double qscale, const QImage& img);

    PdfDocument*                m_doc = nullptr;
    QHash<qint64, QImage>       m_cache;
    QSet<qint64>                m_pendingRequests;
    std::shared_ptr<QAtomicInt>             m_regionGeneration;
    std::shared_ptr<QAtomicInt>             m_generation;
    QThreadPool                 m_thumbPool;
    QThreadPool                 m_mainPool;
    static constexpr int        kMaxCachePages      = 40;
    static constexpr int        kMaxOpenPageHandles = 32;
    static constexpr qint64     kMaxCacheBytes      = 80LL * 1024 * 1024; // 80 MB single-zoom cap

    qint64         m_cacheBytes  = 0;
    int            m_currentPage = 0;
    TileCacheFile* m_tileCache   = nullptr;
    FormibDocPtr   m_formibDoc;   // shared with active PageRenderTasks
    std::shared_ptr<FormibGate> m_formibGate; // adaptive FormibPDF verdict cache
};
