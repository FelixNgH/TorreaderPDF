#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "ThumbnailRenderPool.h"
#include <QMutexLocker>
#include <QElapsedTimer>
#include <QDebug>
#include <QFileInfo>
#include <QThread>
#include <fpdf_progressive.h>

// Progressive-pause helpers (mirrors PdfRenderer.h pattern).
struct ThumbPauseCtx {
    QElapsedTimer timer;
    int sliceMs = 50;
};
static FPDF_BOOL ThumbNeedToPauseNow(IFSDK_PAUSE* pThis) {
    auto* ctx = static_cast<ThumbPauseCtx*>(pThis->user);
    return ctx->timer.elapsed() >= ctx->sliceMs;
}

// PDFium global state (font cache, page-parser internals) is NOT per-document.
// Concurrent calls from thumbnail threads and the main renderer collide and
// cause STATUS_BREAKPOINT crashes — especially on complex/large-page PDFs that
// exercise more shared PDFium state. Use the same global mutex as the main
// renderer so all PDFium calls are fully serialized.
extern QMutex s_pdfiumMutex;

// ── ThumbnailWorker ───────────────────────────────────────────────────────────

ThumbnailWorker::ThumbnailWorker(FPDF_DOCUMENT doc, int slot, TileCacheFile* cache, QObject* parent)
    : QThread(parent), m_doc(doc), m_slot(slot), m_cache(cache) {}

void ThumbnailWorker::enqueue(int pageIndex, int priority) {
    QMutexLocker lock(&m_mutex);
    if (m_queued.contains(pageIndex)) return;  // already queued, skip duplicate
    m_queued.insert(pageIndex);
    m_queue.push(ThumbRequest{pageIndex, priority});
    m_cond.wakeOne();
}

void ThumbnailWorker::stop() {
    QMutexLocker lock(&m_mutex);
    m_stop = true;
    m_cond.wakeAll();
}

void ThumbnailWorker::run() {
    while (true) {
        ThumbRequest req{-1, 2};
        {
            QMutexLocker lock(&m_mutex);
            while (m_queue.empty() && !m_stop)
                m_cond.wait(&m_mutex);
            if (m_stop && m_queue.empty()) break;
            req = m_queue.top();
            m_queue.pop();
            m_queued.remove(req.pageIndex);
        }

        if (!m_doc || req.pageIndex < 0) continue;

        // ── Check disk cache first (no pdfium mutex needed) ──
        if (m_cache && m_cache->isOpen()) {
            QImage cached = m_cache->readPage(req.pageIndex, CacheZoom::Thumb);
            if (!cached.isNull()) {
                qDebug() << "[perf] thumb cache hit page=" << req.pageIndex;
                qDebug() << "[perf] thumb WORKER emit page=" << req.pageIndex << "worker=" << (void*)this << "thread=" << QThread::currentThreadId();
                emit thumbnailReady(req.pageIndex, cached);
                continue;
            }
        }

        // ── Progressive render via PDFium (releases mutex between slices) ──
        QImage image;
        QElapsedTimer timer;
        timer.start();

        FPDF_PAGE   fpdfPage = nullptr;
        FPDF_BITMAP bmp      = nullptr;
        int renderStatus     = FPDF_RENDER_READY;

        // Step 1: Start (lock mutex for this slice only)
        {
            // Priority 2 (background): yield to main page render via tryLock
            if (req.priority >= 2) {
                if (!s_pdfiumMutex.tryLock(5)) {
                    // Main render holds mutex → re-queue, try another page
                    QMutexLocker lock(&m_mutex);
                    m_queued.insert(req.pageIndex);
                    m_queue.push(req);
                    QThread::msleep(20);
                    continue;
                }
                s_pdfiumMutex.unlock();
            }

            QMutexLocker thumbLock(&s_pdfiumMutex);

            fpdfPage = FPDF_LoadPage(m_doc, req.pageIndex);
            if (!fpdfPage) continue;

            double w = FPDF_GetPageWidth(fpdfPage);
            double h = FPDF_GetPageHeight(fpdfPage);
            int imgW = qMax(1, (int)(w * ThumbnailRenderPool::kThumbScale));
            int imgH = qMax(1, (int)(h * ThumbnailRenderPool::kThumbScale));

            image = QImage(imgW, imgH, QImage::Format_ARGB32);
            image.fill(Qt::white);

            bmp = FPDFBitmap_CreateEx(imgW, imgH, FPDFBitmap_BGRA,
                                      image.bits(), image.bytesPerLine());

            ThumbPauseCtx pctx;
            pctx.timer.start();
            IFSDK_PAUSE pause;
            pause.version = 1;
            pause.NeedToPauseNow = ThumbNeedToPauseNow;
            pause.user = &pctx;

            renderStatus = FPDF_RenderPageBitmap_Start(bmp, fpdfPage, 0, 0,
                                                       imgW, imgH, 0,
                                                       FPDF_RENDER_LIMITEDIMAGECACHE,
                                                       &pause);
            // mutex released here
        }

        // Step 2: Continue loop — mutex locked/unlocked per slice
        while (renderStatus == FPDF_RENDER_TOBECONTINUED) {
            {
                QMutexLocker lock(&m_mutex);
                if (m_stop) break;
            }
            {
                QMutexLocker thumbLock(&s_pdfiumMutex);

                ThumbPauseCtx pctx;
                pctx.timer.start();
                IFSDK_PAUSE pause;
                pause.version = 1;
                pause.NeedToPauseNow = ThumbNeedToPauseNow;
                pause.user = &pctx;

                renderStatus = FPDF_RenderPage_Continue(fpdfPage, &pause);
                // mutex released here
            }
        }

        // Step 3: Close
        {
            QMutexLocker thumbLock(&s_pdfiumMutex);
            if (renderStatus == FPDF_RENDER_TOBECONTINUED || renderStatus == FPDF_RENDER_DONE)
                FPDF_RenderPage_Close(fpdfPage);
            if (bmp) { FPDFBitmap_Destroy(bmp); bmp = nullptr; }
            if (fpdfPage) { FPDF_ClosePage(fpdfPage); fpdfPage = nullptr; }
        }

        if (!image.isNull() && renderStatus == FPDF_RENDER_DONE) {
            qDebug() << "[perf] thumb done page=" << req.pageIndex
                     << "ms=" << timer.elapsed();
            if (m_cache && m_cache->isOpen())
                m_cache->writePage(req.pageIndex, CacheZoom::Thumb, image);
            qDebug() << "[perf] thumb WORKER emit page=" << req.pageIndex << "worker=" << (void*)this << "thread=" << QThread::currentThreadId();
            emit thumbnailReady(req.pageIndex, image);
        } else if (renderStatus == FPDF_RENDER_FAILED) {
            qDebug() << "[perf] thumb FAILED page=" << req.pageIndex;
        }
    }

    if (m_doc) {
        QMutexLocker thumbLock(&s_pdfiumMutex);
        FPDF_CloseDocument(m_doc);
        m_doc = nullptr;
    }
}

// ── ThumbnailRenderPool ───────────────────────────────────────────────────────

ThumbnailRenderPool::ThumbnailRenderPool(QObject* parent) : QObject(parent) {}

ThumbnailRenderPool::~ThumbnailRenderPool() { close(); }

bool ThumbnailRenderPool::open(const QString& pdfPath) {
    if (m_open) close();
    m_path = pdfPath;
    QByteArray pathBytes = pdfPath.toUtf8();

    // Open disk cache for thumbnails
    {
        QFileInfo fi(pdfPath);
        uint64_t hash = TileCacheFile::hashFile(pdfPath);
        int pgCount = 0;
        {
            QMutexLocker thumbLock(&s_pdfiumMutex);
            FPDF_DOCUMENT tmp = FPDF_LoadDocument(pathBytes.constData(), nullptr);
            if (tmp) { pgCount = FPDF_GetPageCount(tmp); FPDF_CloseDocument(tmp); }
        }
        if (pgCount > 0)
            m_cache.open(pdfPath, hash, static_cast<uint64_t>(fi.size()), pgCount);
    }

    QMutexLocker thumbLock(&s_pdfiumMutex);
    for (int i = 0; i < kDocs; ++i) {
        FPDF_DOCUMENT doc = FPDF_LoadDocument(pathBytes.constData(), nullptr);
        if (!doc) { qDebug() << "[perf] thumb pool FAILED to open — FPDF_LoadDocument failed"; close(); return false; }

        if (i == 0) m_pageCount = FPDF_GetPageCount(doc);

        auto* w = new ThumbnailWorker(doc, i, &m_cache, this);
        auto c = connect(w, &ThumbnailWorker::thumbnailReady, this,
                         [this](int pg, QImage img) {
                             qDebug() << "[perf] thumb POOL relay page=" << pg << "pool=" << (void*)this << "thread=" << QThread::currentThreadId();
                             emit thumbnailReady(pg, img);
                         });
        if (!c)
            qDebug() << "[perf] thumb POOL RELAY CONNECT FAILED worker=" << (void*)w;
        else
            qDebug() << "[perf] thumb POOL relay connected worker=" << (void*)w << "pool=" << (void*)this;
        w->start();
        m_workers.push_back(w);
    }
    m_open = true;
    return true;
}

void ThumbnailRenderPool::close() {
    for (auto* w : m_workers) w->stop();
    for (auto* w : m_workers) { w->wait(); delete w; }
    m_workers.clear();
    m_cache.close();
    m_open = false;
    m_pageCount = 0;
    m_path.clear();
}

void ThumbnailRenderPool::requestThumbnail(int pageIndex, int priority) {
    if (!m_open) { qDebug() << "[perf] thumb requestThumbnail skipped page=" << pageIndex << "pool not open"; return; }
    if (pageIndex < 0 || pageIndex >= m_pageCount) { qDebug() << "[perf] thumb requestThumbnail OOB page=" << pageIndex << "count=" << m_pageCount; return; }
    m_workers[pageIndex % kDocs]->enqueue(pageIndex, priority);
}

void ThumbnailRenderPool::prefetchRange(int first, int last) {
    if (!m_open) { qDebug() << "[perf] thumb prefetch SKIPPED — pool not open"; return; }
    int total = qMin(last, m_pageCount - 1) - first + 1;
    qDebug() << "[perf] thumb prefetch start total=" << total
             << "range=" << first << "-" << qMin(last, m_pageCount - 1);
    for (int i = first; i <= qMin(last, m_pageCount - 1); ++i)
        requestThumbnail(i, 2);
}
