#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "ThumbnailRenderPool.h"
#include <QMutexLocker>

// PDFium global state (font cache, page-parser internals) is NOT per-document.
// Concurrent calls from thumbnail threads and the main renderer collide and
// cause STATUS_BREAKPOINT crashes — especially on complex/large-page PDFs that
// exercise more shared PDFium state. Use the same global mutex as the main
// renderer so all PDFium calls are fully serialized.
extern QMutex s_pdfiumMutex;

// ── ThumbnailWorker ───────────────────────────────────────────────────────────

ThumbnailWorker::ThumbnailWorker(FPDF_DOCUMENT doc, int slot, QObject* parent)
    : QThread(parent), m_doc(doc), m_slot(slot) {}

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
            m_queued.remove(req.pageIndex);  // allow re-request after render completes
        }

        if (!m_doc || req.pageIndex < 0) continue;

        QImage image;
        {
            QMutexLocker thumbLock(&s_pdfiumMutex);

            FPDF_PAGE page = FPDF_LoadPage(m_doc, req.pageIndex);
            if (!page) continue;

            double w = FPDF_GetPageWidth(page);
            double h = FPDF_GetPageHeight(page);
            int imgW = qMax(1, (int)(w * ThumbnailRenderPool::kThumbScale));
            int imgH = qMax(1, (int)(h * ThumbnailRenderPool::kThumbScale));

            image = QImage(imgW, imgH, QImage::Format_ARGB32);
            image.fill(Qt::white);

            FPDF_BITMAP bmp = FPDFBitmap_CreateEx(imgW, imgH, FPDFBitmap_BGRA,
                                                  image.bits(), image.bytesPerLine());
            // No FPDF_ANNOT: thumbnails don't need annotations, and annotation
            // rendering accesses shared PDFium state that requires s_pdfiumMutex.
            FPDF_RenderPageBitmap(bmp, page, 0, 0, imgW, imgH, 0, 0);
            FPDFBitmap_Destroy(bmp);
            FPDF_ClosePage(page);
        }

        if (!image.isNull())
            emit thumbnailReady(req.pageIndex, image);
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

    for (int i = 0; i < kDocs; ++i) {
        FPDF_DOCUMENT doc = FPDF_LoadDocument(pathBytes.constData(), nullptr);
        if (!doc) { close(); return false; }

        if (i == 0) m_pageCount = FPDF_GetPageCount(doc);

        auto* w = new ThumbnailWorker(doc, i, this);
        connect(w, &ThumbnailWorker::thumbnailReady,
                this, &ThumbnailRenderPool::thumbnailReady);
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
    m_open = false;
    m_pageCount = 0;
    m_path.clear();
}

void ThumbnailRenderPool::requestThumbnail(int pageIndex, int priority) {
    if (!m_open || pageIndex < 0 || pageIndex >= m_pageCount) return;
    m_workers[pageIndex % kDocs]->enqueue(pageIndex, priority);
}

void ThumbnailRenderPool::prefetchRange(int first, int last) {
    if (!m_open) return;
    for (int i = first; i <= qMin(last, m_pageCount - 1); ++i)
        requestThumbnail(i, 2);
}
