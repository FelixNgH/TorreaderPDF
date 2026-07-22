#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "PdfRenderer.h"
#include <QMutex>
#include <QMutexLocker>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QDebug>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <fpdf_annot.h>
#include <fpdf_formfill.h>
#include <fpdf_edit.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <QDateTime>
#include <QElapsedTimer>
#include <QThread>

extern QMutex s_pdfiumMutex;

// ponytail: RAII wrapper that logs when s_pdfiumMutex is held >500ms
class TimedMutexLocker {
    QMutexLocker<QMutex> m_locker;
    QElapsedTimer m_timer;
    const char* m_label;
public:
    TimedMutexLocker(QMutex& m, const char* label)
        : m_locker(&m), m_label(label) { m_timer.start(); }
    ~TimedMutexLocker() {
        qint64 e = m_timer.elapsed();
        if (e > 500)
            qDebug() << "[perf] LONG LOCK ms=" << e << "at" << m_label;
    }
};

std::atomic<int> PdfRenderer::s_renderCount{0};

// ── PageRenderTask ────────────────────────────────────────────────────────────

PageRenderTask::PageRenderTask(PdfRenderer* renderer, PdfDocument* pdfDoc, RenderRequest req,
                               QObject* receiver,
                               std::shared_ptr<QAtomicInt> genRef,
                               FormibDocPtr formibDoc,
                               std::shared_ptr<FormibGate> gate)
    : m_renderer(renderer), m_pdfDoc(pdfDoc), m_req(req)
    , m_genRef(std::move(genRef))
    , m_formibDoc(std::move(formibDoc))
    , m_gate(std::move(gate))
{
    setAutoDelete(true);
    connect(this, &PageRenderTask::finished, receiver,
            [](int, QImage){}, Qt::QueuedConnection);
}


void PageRenderTask::run() {
    if (!m_genRef || m_genRef->loadRelaxed() != m_req.generation) {
        qDebug() << "[perf] drop page=" << m_req.pageIndex << "reason=genMismatch thread=" << QThread::currentThreadId() << "fullQuality=" << m_req.fullQuality;
        emit finished(m_req.pageIndex, QImage()); return;
    }

    qDebug() << "[perf] render start page=" << m_req.pageIndex
             << "scale=" << m_req.scaleFactor
             << "fullQuality=" << m_req.fullQuality
             << "thread=" << QThread::currentThreadId();

    // ── Try FormibPDF (no global mutex, fully parallel) ─────────────────────
    bool isPreviewScale = m_req.scaleFactor <= 0.16;
    bool tryFormib = isPreviewScale && m_formibDoc && (!m_gate || m_gate->shouldTry(m_req.pageIndex));
    if (tryFormib && formibpdf_page_count(m_formibDoc.get()) > 0) {
        double wPt = 0, hPt = 0;
        bool sizeOk = formibpdf_page_size(m_formibDoc.get(),
                                static_cast<uint32_t>(m_req.pageIndex),
                                &wPt, &hPt) && wPt > 0 && hPt > 0;
        if (sizeOk && qMax(wPt, hPt) > 8000.0) {
            if (m_gate) m_gate->skipPage(m_req.pageIndex);
            qDebug() << "[Renderer]   FormibPDF skip oversized page" << m_req.pageIndex
                     << "(" << wPt << "x" << hPt << ")";
        } else if (sizeOk) {
            double scale = m_req.scaleFactor;
            int imgW = qMax(1, static_cast<int>(wPt * scale));
            int imgH = qMax(1, static_cast<int>(hPt * scale));
            double maxDim = m_req.fullQuality ? 4000.0 : PdfRenderer::kThumbMaxPx;
            if (qMax(imgW, imgH) > maxDim) {
                double cap = maxDim / qMax(imgW, imgH);
                imgW = qMax(1, static_cast<int>(imgW * cap));
                imgH = qMax(1, static_cast<int>(imgH * cap));
            }
            qDebug() << "[Renderer]   FormibPDF page=" << m_req.pageIndex
                     << "ptSize=(" << wPt << "x" << hPt << ")"
                     << "px=(" << imgW << "x" << imgH << ")";
            QImage fimg(imgW, imgH, QImage::Format_RGBA8888);
            bool ok = formibpdf_render_page(
                m_formibDoc.get(),
                static_cast<uint32_t>(m_req.pageIndex),
                static_cast<uint32_t>(imgW),
                static_cast<uint32_t>(imgH),
                fimg.bits());
            qDebug() << "[Renderer]   FormibPDF render ok=" << ok;
            if (ok && !fimg.isNull()) {
                const QRgb* px = reinterpret_cast<const QRgb*>(fimg.constBits());
                int total = imgW * imgH;
                int sampleStep = qMax(1, total / 256);
                int sampled  = 0;
                int nonWhite = 0;
                int nearBlack = 0;
                for (int i = 0; i < total; i += sampleStep) {
                    QRgb c = px[i];
                    ++sampled;
                    if ((c & 0x00FFFFFF) != 0x00FFFFFF) ++nonWhite;
                    if (qRed(c) + qGreen(c) + qBlue(c) < 60) ++nearBlack;
                }
                double blackFrac = sampled > 0 ? (double)nearBlack / sampled : 0.0;
                bool hasContent  = nonWhite > 2;
                bool looksBroken = blackFrac > 0.60;
                if (hasContent && !looksBroken) {
                    if (m_genRef->loadRelaxed() != m_req.generation) {
                        emit finished(m_req.pageIndex, QImage()); return;
                    }
                    if (m_gate) m_gate->recordAccept();
                    qDebug() << "[Renderer]   -> USING FormibPDF for page" << m_req.pageIndex;
                    emit finished(m_req.pageIndex,
                                  fimg.convertToFormat(QImage::Format_ARGB32));
                    return;
                }
                if (m_gate) m_gate->recordReject(m_req.pageIndex);
            } else {
                if (m_gate) m_gate->recordReject(m_req.pageIndex);
                qDebug() << "[Renderer]   -> FormibPDF FAILED, fallback to PDFium page" << m_req.pageIndex;
            }
        } else {
            if (m_gate) m_gate->recordReject(m_req.pageIndex);
            qDebug() << "[Renderer]   FormibPDF page_size FAILED for page" << m_req.pageIndex;
        }
    }

    QImage image;
    if (m_genRef->loadRelaxed() != m_req.generation) {
        qDebug() << "[perf] drop page=" << m_req.pageIndex << "reason=genMismatchPostFormib thread=" << QThread::currentThreadId();
        emit finished(m_req.pageIndex, QImage()); return;
    }

    qDebug() << "[Renderer]   PDFium render page=" << m_req.pageIndex;
    QElapsedTimer renderTimer;
    renderTimer.start();
    {
        TimedMutexLocker lock(s_pdfiumMutex, "PageRenderTask::run");
        if (m_genRef->loadRelaxed() != m_req.generation) {
            qDebug() << "[perf] drop page=" << m_req.pageIndex << "reason=genMismatchPostLock thread=" << QThread::currentThreadId();
            emit finished(m_req.pageIndex, QImage()); return;
        }

        FPDF_PAGE page = m_renderer->acquirePage(m_req.pageIndex);
        if (!page) { emit finished(m_req.pageIndex, QImage()); return; }

        double w = FPDF_GetPageWidth(page);
        double h = FPDF_GetPageHeight(page);
        if (m_pdfDoc) m_pdfDoc->updatePageSize(m_req.pageIndex, w, h);

        double longSide = qMax(w, h);
        double maxPx = m_req.fullQuality ? PdfRenderer::kFullRenderMaxPx
            : qMin(m_req.scaleFactor * longSide, PdfRenderer::kThumbMaxPx);
        double scale = maxPx / qMax(longSide, 1.0);
        int imgW = qMax(1, static_cast<int>(w * scale));
        int imgH = qMax(1, static_cast<int>(h * scale));

        image = QImage(imgW, imgH, QImage::Format_ARGB32);
        image.fill(Qt::white);
        FPDF_BITMAP bmp = FPDFBitmap_CreateEx(imgW, imgH, FPDFBitmap_BGRA,
                                                image.bits(), image.bytesPerLine());
        PdfRenderer::s_renderCount.fetch_add(1);
        FPDF_RenderPageBitmap(bmp, page, 0, 0, imgW, imgH, 0, FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE);
        if (m_pdfDoc && FPDF_GetFormType(m_pdfDoc->raw()) != FORMTYPE_NONE) {
            FPDF_FORMFILLINFO ffi;
            memset(&ffi, 0, sizeof(ffi));
            ffi.version = 2;
            FPDF_FORMHANDLE form = FPDFDOC_InitFormFillEnvironment(m_pdfDoc->raw(), &ffi);
            if (form) {
                FORM_OnAfterLoadPage(page, form);
                FPDF_FFLDraw(form, bmp, page, 0, 0, imgW, imgH, 0, FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE);
                FORM_OnBeforeClosePage(page, form);
                FPDFDOC_ExitFormFillEnvironment(form);
            }
        }
        FPDFBitmap_Destroy(bmp);
        int objCount = FPDFPage_CountObjects(page);
        emit pageObjectCount(m_req.pageIndex, objCount);
        FPDF_ClosePage(page);
    }

    if (m_genRef->loadRelaxed() != m_req.generation) {
        qDebug() << "[perf] drop page=" << m_req.pageIndex << "reason=genMismatchPostRender thread=" << QThread::currentThreadId();
        emit finished(m_req.pageIndex, QImage()); return;
    }
    qDebug() << "[perf] render done page=" << m_req.pageIndex
             << "ms=" << renderTimer.elapsed()
             << "thread=" << QThread::currentThreadId();
    emit finished(m_req.pageIndex, image);
}

// ── ProgressiveRenderTask ─────────────────────────────────────────────────────

ProgressiveRenderTask::ProgressiveRenderTask(PdfRenderer* renderer, PdfDocument* pdfDoc, RenderRequest req,
                                             QObject* receiver, std::shared_ptr<QAtomicInt> genRef,
                                             FormibDocPtr formibDoc,
                                             std::shared_ptr<FormibGate> gate)
    : m_renderer(renderer), m_pdfDoc(pdfDoc), m_req(req)
    , m_genRef(std::move(genRef))
    , m_formibDoc(std::move(formibDoc))
    , m_gate(std::move(gate))
{
    setAutoDelete(true);
    connect(this, &ProgressiveRenderTask::finished, receiver,
            [](int, QImage){}, Qt::QueuedConnection);
    connect(this, &ProgressiveRenderTask::pagePartial, receiver,
            [](int, double, QImage){}, Qt::QueuedConnection);
}

ProgressiveRenderTask::~ProgressiveRenderTask() {
    if (m_bmp) {
        FPDFBitmap_Destroy(m_bmp);
        m_bmp = nullptr;
    }
}

void ProgressiveRenderTask::run() {
    if (!m_genRef || m_genRef->loadRelaxed() != m_req.generation) {
        qDebug() << "[perf] drop page=" << m_req.pageIndex << "reason=genMismatch" << "fullQuality=" << m_req.fullQuality << "thread=" << QThread::currentThreadId();
        emit finished(m_req.pageIndex, QImage()); return;
    }

    qDebug() << "[perf] progressive start page=" << m_req.pageIndex
             << "scale=" << m_req.scaleFactor
             << "fullQuality=" << m_req.fullQuality
             << "thread=" << QThread::currentThreadId();

    // ── Try FormibPDF (non-progressive, same as PageRenderTask) ────────────────
    bool isPreviewScale = m_req.scaleFactor <= 0.16;
    bool tryFormib = isPreviewScale && m_formibDoc && (!m_gate || m_gate->shouldTry(m_req.pageIndex));
    if (tryFormib && formibpdf_page_count(m_formibDoc.get()) > 0) {
        double wPt = 0, hPt = 0;
        bool sizeOk = formibpdf_page_size(m_formibDoc.get(),
                                static_cast<uint32_t>(m_req.pageIndex),
                                &wPt, &hPt) && wPt > 0 && hPt > 0;
        if (sizeOk && qMax(wPt, hPt) > 8000.0) {
            if (m_gate) m_gate->skipPage(m_req.pageIndex);
            qDebug() << "[Renderer]   FormibPDF skip oversized page" << m_req.pageIndex
                     << "(" << wPt << "x" << hPt << ")";
        } else if (sizeOk) {
            double scale = m_req.scaleFactor;
            int imgW = qMax(1, static_cast<int>(wPt * scale));
            int imgH = qMax(1, static_cast<int>(hPt * scale));
            double maxDim = m_req.fullQuality ? 4000.0 : PdfRenderer::kThumbMaxPx;
            if (qMax(imgW, imgH) > maxDim) {
                double cap = maxDim / qMax(imgW, imgH);
                imgW = qMax(1, static_cast<int>(imgW * cap));
                imgH = qMax(1, static_cast<int>(imgH * cap));
            }
            qDebug() << "[Renderer]   FormibPDF page=" << m_req.pageIndex
                     << "ptSize=(" << wPt << "x" << hPt << ")"
                     << "px=(" << imgW << "x" << imgH << ")";
            QImage fimg(imgW, imgH, QImage::Format_RGBA8888);
            bool ok = formibpdf_render_page(
                m_formibDoc.get(),
                static_cast<uint32_t>(m_req.pageIndex),
                static_cast<uint32_t>(imgW),
                static_cast<uint32_t>(imgH),
                fimg.bits());
            qDebug() << "[Renderer]   FormibPDF render ok=" << ok;
            if (ok && !fimg.isNull()) {
                const QRgb* px = reinterpret_cast<const QRgb*>(fimg.constBits());
                int total = imgW * imgH;
                int sampleStep = qMax(1, total / 256);
                int sampled  = 0;
                int nonWhite = 0;
                int nearBlack = 0;
                for (int i = 0; i < total; i += sampleStep) {
                    QRgb c = px[i];
                    ++sampled;
                    if ((c & 0x00FFFFFF) != 0x00FFFFFF) ++nonWhite;
                    if (qRed(c) + qGreen(c) + qBlue(c) < 60) ++nearBlack;
                }
                double blackFrac = sampled > 0 ? (double)nearBlack / sampled : 0.0;
                bool hasContent  = nonWhite > 2;
                bool looksBroken = blackFrac > 0.60;
                if (hasContent && !looksBroken) {
                    if (m_genRef->loadRelaxed() != m_req.generation) {
                        emit finished(m_req.pageIndex, QImage()); return;
                    }
                    if (m_gate) m_gate->recordAccept();
                    qDebug() << "[Renderer]   -> USING FormibPDF for page" << m_req.pageIndex;
                    emit finished(m_req.pageIndex,
                                  fimg.convertToFormat(QImage::Format_ARGB32));
                    return;
                }
                if (m_gate) m_gate->recordReject(m_req.pageIndex);
            } else {
                if (m_gate) m_gate->recordReject(m_req.pageIndex);
                qDebug() << "[Renderer]   -> FormibPDF FAILED, fallback to PDFium page" << m_req.pageIndex;
            }
        } else {
            if (m_gate) m_gate->recordReject(m_req.pageIndex);
            qDebug() << "[Renderer]   FormibPDF page_size FAILED for page" << m_req.pageIndex;
        }
    }

    QElapsedTimer renderTimer;
    renderTimer.start();

    // ── Step 1: FPDF_RenderPageBitmap_Start ───────────────────────────────────
    int startStatus = FPDF_RENDER_READY;
    {
        TimedMutexLocker lock(s_pdfiumMutex, "ProgressiveRenderTask::Start");
        if (m_genRef->loadRelaxed() != m_req.generation) {
            qDebug() << "[perf] drop page=" << m_req.pageIndex << "reason=genMismatchPreStart";
            emit finished(m_req.pageIndex, QImage()); return;
        }

        m_fpdfPage = m_renderer->acquirePage(m_req.pageIndex);
        if (!m_fpdfPage) { emit finished(m_req.pageIndex, QImage()); return; }

        double w = FPDF_GetPageWidth(m_fpdfPage);
        double h = FPDF_GetPageHeight(m_fpdfPage);
        if (m_pdfDoc) m_pdfDoc->updatePageSize(m_req.pageIndex, w, h);

        double longSide = qMax(w, h);
        double maxPx = m_req.fullQuality ? PdfRenderer::kFullRenderMaxPx
            : qMin(m_req.scaleFactor * longSide, PdfRenderer::kThumbMaxPx);
        m_renderScale = maxPx / qMax(longSide, 1.0);
        m_bmpW = qMax(1, static_cast<int>(w * m_renderScale));
        m_bmpH = qMax(1, static_cast<int>(h * m_renderScale));

        m_image = QImage(m_bmpW, m_bmpH, QImage::Format_ARGB32);
        m_image.fill(Qt::white);
        m_bmp = FPDFBitmap_CreateEx(m_bmpW, m_bmpH, FPDFBitmap_BGRA,
                                     m_image.bits(), m_image.bytesPerLine());

        ProgressivePauseCtx pctx;
        pctx.timer.start();
        IFSDK_PAUSE pause;
        pause.version = 1;
        pause.NeedToPauseNow = ProgressiveNeedToPauseNow;
        pause.user = &pctx;

        PdfRenderer::s_renderCount.fetch_add(1);
        m_renderStatus = FPDF_RenderPageBitmap_Start(m_bmp, m_fpdfPage, 0, 0,
                                                       m_bmpW, m_bmpH, 0,
                                                       FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE,
                                                       &pause);
        // Mutex unlocked here
    }

    // Emit first partial after Start regardless of status
    if (m_renderStatus == FPDF_RENDER_TOBECONTINUED || m_renderStatus == FPDF_RENDER_DONE) {
        QImage partial = m_image.copy();
        emit pagePartial(m_req.pageIndex, scaleFactor(), partial);
        qDebug() << "[perf] slice ms=" << renderTimer.elapsed() << "page=" << m_req.pageIndex;
    }

    // ── Step 2: Continue loop ──────────────────────────────────────────────────
    m_lastEmitTimer.start();
    while (m_renderStatus == FPDF_RENDER_TOBECONTINUED) {
        // Check generation before each slice — cancel if stale
        if (m_genRef->loadRelaxed() != m_req.generation) {
            qDebug() << "[perf] drop page=" << m_req.pageIndex << "reason=genMismatchMid progressive";
            break;
        }

        {
            TimedMutexLocker lock(s_pdfiumMutex, "ProgressiveRenderTask::Continue");
            ProgressivePauseCtx pctx;
            pctx.timer.start();
            IFSDK_PAUSE pause;
            pause.version = 1;
            pause.NeedToPauseNow = ProgressiveNeedToPauseNow;
            pause.user = &pctx;

            m_renderStatus = FPDF_RenderPage_Continue(m_fpdfPage, &pause);
            // Mutex unlocked here — other threads can use PDFium between slices
        }

        // Emit partial after Continue (throttled to 200ms)
        if (m_renderStatus == FPDF_RENDER_TOBECONTINUED || m_renderStatus == FPDF_RENDER_DONE) {
            if (m_renderStatus == FPDF_RENDER_DONE || m_lastEmitTimer.elapsed() >= 200) {
                QImage partial = m_image.copy();
                emit pagePartial(m_req.pageIndex, scaleFactor(), partial);
                if (m_renderStatus == FPDF_RENDER_TOBECONTINUED)
                    m_lastEmitTimer.restart();
                qDebug() << "[perf] slice ms=" << renderTimer.elapsed() << "page=" << m_req.pageIndex;
            }
        }
    }

    // ── Step 3: Close / finalise ──────────────────────────────────────────────
    bool cancelled = (m_genRef->loadRelaxed() != m_req.generation);

    // Emit one final partial if Done
    if (m_renderStatus == FPDF_RENDER_DONE && !cancelled) {
        QImage finalPartial = m_image.copy();
        emit pagePartial(m_req.pageIndex, scaleFactor(), finalPartial);
    }

    {
        TimedMutexLocker lock(s_pdfiumMutex, "ProgressiveRenderTask::Close");
        if (m_renderStatus == FPDF_RENDER_TOBECONTINUED || m_renderStatus == FPDF_RENDER_DONE)
            FPDF_RenderPage_Close(m_fpdfPage);
        if (m_bmp) { FPDFBitmap_Destroy(m_bmp); m_bmp = nullptr; }

        int objCount = 0;
        if (m_fpdfPage) {
            objCount = FPDFPage_CountObjects(m_fpdfPage);
            emit pageObjectCount(m_req.pageIndex, objCount);
            FPDF_ClosePage(m_fpdfPage);
            m_fpdfPage = nullptr;
        }
    }

    if (cancelled) {
        qDebug() << "[perf] drop page=" << m_req.pageIndex << "reason=genMismatchPost progressive";
        emit finished(m_req.pageIndex, QImage());
        return;
    }

    if (m_renderStatus == FPDF_RENDER_FAILED) {
        qDebug() << "[perf] render FAILED page=" << m_req.pageIndex;
        emit finished(m_req.pageIndex, QImage());
        return;
    }

    qDebug() << "[perf] render done page=" << m_req.pageIndex
             << "ms=" << renderTimer.elapsed()
             << "thread=" << QThread::currentThreadId();
    emit finished(m_req.pageIndex, std::move(m_image));
}

// ── RegionRenderTask ──────────────────────────────────────────────────────────

RegionRenderTask::RegionRenderTask(PdfRenderer* renderer, PdfDocument* pdfDoc,
                                   int pageIndex, double scale, QRect regionPx,
                                   QObject* receiver,
                                   std::shared_ptr<QAtomicInt> genRef)
    : m_renderer(renderer), m_pdfDoc(pdfDoc)
    , m_pageIndex(pageIndex), m_scale(scale), m_regionPx(regionPx)
    , m_genRef(std::move(genRef))
    , m_reqGen(m_genRef ? m_genRef->loadRelaxed() : 0)
{
    setAutoDelete(true);
    connect(this, &RegionRenderTask::finished, receiver,
            [](int, double, QRect, QImage){}, Qt::QueuedConnection);
}

void RegionRenderTask::run() {
    if (!m_genRef || m_genRef->loadRelaxed() != m_reqGen) {
        emit finished(m_pageIndex, m_scale, m_regionPx, QImage()); return;
    }

    int rw = m_regionPx.width();
    int rh = m_regionPx.height();
    constexpr qint64 kMaxArea = 16LL * 1024 * 1024;
    if (static_cast<qint64>(rw) * rh > kMaxArea) {
        emit finished(m_pageIndex, m_scale, m_regionPx, QImage()); return;
    }

    TimedMutexLocker lock(s_pdfiumMutex, "RegionRenderTask::run");
    if (!m_genRef || m_genRef->loadRelaxed() != m_reqGen) {
        emit finished(m_pageIndex, m_scale, m_regionPx, QImage()); return;
    }

    FPDF_PAGE page = m_renderer->acquirePage(m_pageIndex);
    if (!page) {
        emit finished(m_pageIndex, m_scale, m_regionPx, QImage()); return;
    }

    double wPt = FPDF_GetPageWidth(page);
    double hPt = FPDF_GetPageHeight(page);
    int fullW = qMax(1, static_cast<int>(wPt * m_scale));
    int fullH = qMax(1, static_cast<int>(hPt * m_scale));

    QImage image(rw, rh, QImage::Format_ARGB32);
    image.fill(Qt::white);
    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(rw, rh, FPDFBitmap_BGRA,
                                           image.bits(), image.bytesPerLine());
    PdfRenderer::s_renderCount.fetch_add(1);
    FPDF_RenderPageBitmap(bmp, page,
                          -m_regionPx.x(), -m_regionPx.y(),
                          fullW, fullH, 0, FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE);
    if (m_pdfDoc && FPDF_GetFormType(m_pdfDoc->raw()) != FORMTYPE_NONE) {
        FPDF_FORMFILLINFO ffi;
        memset(&ffi, 0, sizeof(ffi));
        ffi.version = 2;
        FPDF_FORMHANDLE form = FPDFDOC_InitFormFillEnvironment(m_pdfDoc->raw(), &ffi);
        if (form) {
            FORM_OnAfterLoadPage(page, form);
            FPDF_FFLDraw(form, bmp, page,
                         -m_regionPx.x(), -m_regionPx.y(),
                         fullW, fullH, 0, FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE);
            FORM_OnBeforeClosePage(page, form);
            FPDFDOC_ExitFormFillEnvironment(form);
        }
    }
    FPDFBitmap_Destroy(bmp);
    FPDF_ClosePage(page);

    if (!m_genRef || m_genRef->loadRelaxed() != m_reqGen) {
        emit finished(m_pageIndex, m_scale, m_regionPx, QImage()); return;
    }

    emit finished(m_pageIndex, m_scale, m_regionPx, image);
}

// ── TileBatchRenderTask ────────────────────────────────────────────────────────

TileBatchRenderTask::TileBatchRenderTask(PdfRenderer* renderer, PdfDocument* pdfDoc,
                                         int pageIndex, double scale,
                                         QVector<QPoint> tiles,
                                         QObject* receiver,
                                         std::shared_ptr<QAtomicInt> genRef)
    : m_renderer(renderer), m_pdfDoc(pdfDoc)
    , m_pageIndex(pageIndex), m_scale(scale)
    , m_tiles(std::move(tiles))
    , m_genRef(std::move(genRef))
    , m_reqGen(m_genRef ? m_genRef->loadRelaxed() : 0)
{
    setAutoDelete(true);
    connect(this, &TileBatchRenderTask::tileDone, receiver,
            [](int, double, int, int, QImage){}, Qt::QueuedConnection);
}

void TileBatchRenderTask::run() {
    if (!m_genRef || m_genRef->loadRelaxed() != m_reqGen) {
        emit tileDone(m_pageIndex, m_scale, 0, 0, QImage()); return;
    }
    if (m_tiles.isEmpty()) {
        emit tileDone(m_pageIndex, m_scale, 0, 0, QImage()); return;
    }

    {
        TimedMutexLocker lock(s_pdfiumMutex, "TileBatchRenderTask::run");
        if (m_genRef->loadRelaxed() != m_reqGen) {
            emit tileDone(m_pageIndex, m_scale, 0, 0, QImage()); return;
        }

        FPDF_PAGE page = m_renderer->acquirePage(m_pageIndex);
        if (!page) {
            emit tileDone(m_pageIndex, m_scale, 0, 0, QImage()); return;
        }

        double wPt = FPDF_GetPageWidth(page);
        double hPt = FPDF_GetPageHeight(page);
        if (m_pdfDoc) m_pdfDoc->updatePageSize(m_pageIndex, wPt, hPt);

        int fullW = qMax(1, static_cast<int>(wPt * m_scale));
        int fullH = qMax(1, static_cast<int>(hPt * m_scale));

        FPDF_FORMHANDLE form = nullptr;
        bool hasForms = m_pdfDoc && (FPDF_GetFormType(m_pdfDoc->raw()) != FORMTYPE_NONE);
        if (hasForms) {
            FPDF_FORMFILLINFO ffi;
            memset(&ffi, 0, sizeof(ffi));
            ffi.version = 2;
            form = FPDFDOC_InitFormFillEnvironment(m_pdfDoc->raw(), &ffi);
            if (form) FORM_OnAfterLoadPage(page, form);
        }

        for (const QPoint& tile : m_tiles) {
            if (m_genRef->loadRelaxed() != m_reqGen) {
                if (form) { FORM_OnBeforeClosePage(page, form); FPDFDOC_ExitFormFillEnvironment(form); }
                FPDF_ClosePage(page);
                return;
            }

            int col = tile.x();
            int row = tile.y();
            int rw = kTileSize;
            int rh = kTileSize;

            QImage image(rw, rh, QImage::Format_ARGB32);
            image.fill(Qt::white);
            FPDF_BITMAP bmp = FPDFBitmap_CreateEx(rw, rh, FPDFBitmap_BGRA,
                                                   image.bits(), image.bytesPerLine());
            PdfRenderer::s_renderCount.fetch_add(1);
            FPDF_RenderPageBitmap(bmp, page,
                                  -col * kTileSize, -row * kTileSize,
                                  fullW, fullH, 0, FPDF_ANNOT);
            if (form) {
                FPDF_FFLDraw(form, bmp, page,
                             -col * kTileSize, -row * kTileSize,
                             fullW, fullH, 0, FPDF_ANNOT);
            }
            FPDFBitmap_Destroy(bmp);

            emit tileDone(m_pageIndex, m_scale, col, row, std::move(image));
        }

        if (form) { FORM_OnBeforeClosePage(page, form); FPDFDOC_ExitFormFillEnvironment(form); }
        FPDF_ClosePage(page);
    }
}

// ── Page handle ────────────────────────────────────────────────────────────────

FPDF_PAGE PdfRenderer::acquirePage(int pageIndex) {
    if (!m_doc || !m_doc->raw()) return nullptr;
    return FPDF_LoadPage(m_doc->raw(), pageIndex);
}

// ── Heavy page detection ───────────────────────────────────────────────────────

bool PdfRenderer::isHeavyPage(int pageIndex) {
    auto it = m_pageObjectCount.constFind(pageIndex);
    if (it != m_pageObjectCount.constEnd())
        return it.value() > kHeavyObjectThreshold;
    // Not yet counted by background render — assume heavy (safe default:
    // full-page render at 4000px is fine, tiling a heavy page is not).
    return true;
}

// ── PdfRenderer ───────────────────────────────────────────────────────────────

PdfRenderer::PdfRenderer(QObject* parent)
    : QObject(parent)
    , m_generation(std::make_shared<QAtomicInt>(0))
    , m_regionGeneration(std::make_shared<QAtomicInt>(0))
    , m_tileGeneration(std::make_shared<QAtomicInt>(0))
    , m_fullRenderGen(std::make_shared<QAtomicInt>(0))
    , m_progressiveGen(std::make_shared<QAtomicInt>(0))
    , m_continuousGen(std::make_shared<QAtomicInt>(0))
    , m_formibGate(std::make_shared<FormibGate>())
{
    const int cpus = qMax(1, QThread::idealThreadCount());
    m_thumbPool.setMaxThreadCount(qMax(2, cpus / 2));
    m_thumbPool.setExpiryTimeout(-1);
    m_mainPool.setMaxThreadCount(4);
    m_mainPool.setExpiryTimeout(-1);
}

PdfRenderer::~PdfRenderer() {
    m_generation->fetchAndAddOrdered(999);
    m_thumbPool.waitForDone();
    m_mainPool.waitForDone();
    if (s_renderCount.load() > 0)
        qDebug() << "[Renderer] total FPDF_RenderPageBitmap calls:" << s_renderCount.load();
    m_formibDoc.reset();
}

void PdfRenderer::setDocument(PdfDocument* doc) {
    cancelPending();
    m_mainPool.waitForDone();
    m_thumbPool.waitForDone();
    m_doc = doc;
    clearCache();
    m_formibDoc.reset();
    m_pageObjectCount.clear();
    m_formibGate = std::make_shared<FormibGate>();
}

void PdfRenderer::setFormibDoc(FormibDocPtr doc) {
    m_formibDoc = std::move(doc);
}

void PdfRenderer::cancelPending() {
    m_generation->fetchAndAddOrdered(1);
    m_fullRenderGen->fetchAndAddOrdered(1);
    m_progressiveGen->fetchAndAddOrdered(1);
    m_continuousGen->fetchAndAddOrdered(1);
    m_tileGeneration->fetchAndAddOrdered(1);
    m_pendingRequests.clear();
    m_pendingTiles.clear();
    m_thumbPool.clear();
    m_mainPool.clear();
    m_fullRenderRunning = false;
}

void PdfRenderer::clearStalePending() {
    m_pendingRequests.clear();
}

// Single O(n log n) pass: sort entries by distance from current page, evict farthest first.
void PdfRenderer::evictCache() {
    if (m_cacheBytes <= kMaxCacheBytes) return;
    using KV = QPair<int, int>;  // distance, pageIndex
    QVector<KV> byDist;
    byDist.reserve(m_cache.size());
    for (auto it = m_cache.cbegin(); it != m_cache.cend(); ++it)
        byDist.append({qAbs(it.key() - m_currentPage), it.key()});
    std::sort(byDist.begin(), byDist.end(),
              [](const KV& a, const KV& b) { return a.first > b.first; });
    // ponytail: always keep at least 5 pages (current ± 2)
    const int kMinKeep = 5;
    int kept = 0;
    for (const auto& kv : byDist) {
        if (m_cacheBytes <= kMaxCacheBytes || m_cache.size() <= kMinKeep) break;
        ++kept;
        m_cacheBytes -= static_cast<qint64>(m_cache[kv.second].sizeInBytes());
        m_cache.remove(kv.second);
    }
}

QImage PdfRenderer::bestCachedForPage(int pageIndex) const {
    auto it = m_cache.constFind(pageIndex);
    if (it != m_cache.constEnd())
        return it.value();
    return {};
}

static qint64 pendingCacheKey(int pageIndex, bool fullQuality) {
    return (static_cast<qint64>(pageIndex) << 1) | (fullQuality ? 1 : 0);
}

void PdfRenderer::requestPage(int pageIndex, double scale) {
    if (!m_doc || !m_doc->isOpen()) return;

    // If FormibPDF proved unable to handle this document, free its in-memory copy
    if (m_formibDoc && m_formibGate && m_formibGate->disabled.load()) {
        qDebug() << "[Renderer] FormibPDF disabled for this document — freeing FormibDoc, using PDFium only";
        m_formibDoc.reset();
    }

    bool isThumb      = (scale <= 0.25);
    bool fullQuality  = !isThumb;
    qint64 pendKey    = pendingCacheKey(pageIndex, fullQuality);

    // Only full-quality images go into m_cache
    if (fullQuality && m_cache.contains(pageIndex)) {
        qDebug() << "[perf] cache hit page=" << pageIndex << "thread=" << QThread::currentThreadId();
        emit pageReady(pageIndex, m_cache[pageIndex]);
        return;
    }

    // Check persistent disk cache (only for full-quality)
    if (fullQuality && m_tileCache && m_tileCache->isOpen()) {
        if (m_tileCache->hasPage(pageIndex, CacheZoom::Full)) {
            QImage cached = m_tileCache->readPage(pageIndex, CacheZoom::Full);
            if (!cached.isNull()) {
                qDebug() << "[perf] cache hit page=" << pageIndex << "source=disk thread=" << QThread::currentThreadId();
                m_cache.insert(pageIndex, cached);
                m_cacheBytes += static_cast<qint64>(cached.sizeInBytes());
                evictCache();
                emit pageReady(pageIndex, cached);
                return;
            }
        }
    }

    m_pendingRequests.insert(pendKey, QDateTime::currentMSecsSinceEpoch());

    // ponytail: newest-wins for full-quality renders — cancel pending, keep at most 1
    std::shared_ptr<QAtomicInt> genRef = m_generation;
    int gen = m_generation->loadRelaxed();
    if (fullQuality) {
        if (m_fullRenderRunning) {
            qDebug() << "[perf] drop page=" << m_fullRenderPage << "reason=newest-wins replacing with page=" << pageIndex;
            m_fullRenderGen->fetchAndAddOrdered(1);
        }
        m_fullRenderRunning = true;
        m_fullRenderPage = pageIndex;
        m_fullRenderScale = scale;
        genRef = m_fullRenderGen;
        gen = m_fullRenderGen->loadRelaxed();
    }

    // Show any cached version (from preload) immediately (only for full-quality)
    if (fullQuality) {
        QImage placeholder = bestCachedForPage(pageIndex);
        if (!placeholder.isNull()) {
            emit pageReady(pageIndex, placeholder);
        }
    }

    RenderRequest req{pageIndex, scale, true, fullQuality, gen};

    if (fullQuality) {
        auto* ptask = new ProgressiveRenderTask(this, m_doc, req, this,
                                                 genRef,
                                                 m_formibDoc, m_formibGate);
        connect(ptask, &ProgressiveRenderTask::finished, this,
                [this, pageIndex, fullQuality, gen](int idx, QImage img) {
            qint64 finishedKey = pendingCacheKey(idx, fullQuality);
            m_pendingRequests.remove(finishedKey);
            if (fullQuality) {
                if (gen == m_fullRenderGen->loadRelaxed())
                    m_fullRenderRunning = false;
            }
            bool hasCache = m_cache.contains(idx);
            if (img.isNull()) {
                qDebug() << "[perf] drop page=" << idx << "reason=nullResult" << "currentPage=" << m_currentPage << "hasCache=" << hasCache;
            } else if (fullQuality && idx != m_currentPage) {
                qDebug() << "[perf] drop page=" << idx << "reason=notCurrent" << "currentPage=" << m_currentPage << "hasCache=" << hasCache;
                return;
            }
            if (!img.isNull()) {
                if (fullQuality && !m_cache.contains(pageIndex)) {
                    m_cacheBytes += static_cast<qint64>(img.sizeInBytes());
                    m_cache.insert(pageIndex, img);
                }
                evictCache();
                if (fullQuality && m_tileCache && m_tileCache->isOpen() && !m_writingCache.contains(pageIndex)) {
                    m_writingCache.insert(pageIndex);
                    QImage cacheImg = img;
                    auto* watcher = new QFutureWatcher<void>(this);
                    connect(watcher, &QFutureWatcher<void>::finished, this,
                            [this, watcher, pageIndex]() {
                        m_writingCache.remove(pageIndex);
                        watcher->deleteLater();
                    });
                    watcher->setFuture(QtConcurrent::run([tc = m_tileCache, pageIndex, cacheImg]() {
                        tc->writePage(pageIndex, CacheZoom::Full, cacheImg);
                    }));
                }
                emit pageReady(idx, img);
            } else if (fullQuality && idx == m_currentPage && !hasCache && !m_fullRenderRunning) {
                qDebug() << "[perf] SAFETY NET: re-request page=" << idx << "scale=" << m_fullRenderScale;
                requestPage(idx, m_fullRenderScale);
            }
        });
        connect(ptask, &ProgressiveRenderTask::pageObjectCount, this, &PdfRenderer::setPageObjectCount);
        connect(ptask, &ProgressiveRenderTask::pagePartial, this,
                [this](int idx, double sc, QImage img) {
            emit pagePartial(idx, sc, img);
        });
        if (isThumb)
            m_thumbPool.start(ptask, 0);
        else
            m_mainPool.start(ptask, 10);
        return;
    }

    auto* task = new PageRenderTask(this, m_doc, req, this,
                                    genRef,
                                    m_formibDoc, m_formibGate);

    connect(task, &PageRenderTask::finished, this,
            [this, pageIndex, fullQuality, gen](int idx, QImage img) {
        qint64 finishedKey = pendingCacheKey(idx, fullQuality);
        m_pendingRequests.remove(finishedKey);
        if (fullQuality) {
            if (gen == m_fullRenderGen->loadRelaxed())
                m_fullRenderRunning = false;
        }
        bool hasCache = m_cache.contains(idx);
        if (img.isNull()) {
            qDebug() << "[perf] drop page=" << idx << "reason=nullResult" << "currentPage=" << m_currentPage << "hasCache=" << hasCache;
        } else if (fullQuality && idx != m_currentPage) {
            qDebug() << "[perf] drop page=" << idx << "reason=notCurrent" << "currentPage=" << m_currentPage << "hasCache=" << hasCache;
            return;
        }
        if (!img.isNull()) {
            if (fullQuality && !m_cache.contains(pageIndex)) {
                m_cacheBytes += static_cast<qint64>(img.sizeInBytes());
                m_cache.insert(pageIndex, img);
            }
            evictCache();

            if (fullQuality && m_tileCache && m_tileCache->isOpen() && !m_writingCache.contains(pageIndex)) {
                m_writingCache.insert(pageIndex);
                QImage cacheImg = img;
                auto* watcher = new QFutureWatcher<void>(this);
                connect(watcher, &QFutureWatcher<void>::finished, this,
                        [this, watcher, pageIndex]() {
                    m_writingCache.remove(pageIndex);
                    watcher->deleteLater();
                });
                watcher->setFuture(QtConcurrent::run([tc = m_tileCache, pageIndex, cacheImg]() {
                    tc->writePage(pageIndex, CacheZoom::Full, cacheImg);
                }));
            }

            emit pageReady(idx, img);
        } else if (fullQuality && idx == m_currentPage && !hasCache && !m_fullRenderRunning) {
            qDebug() << "[perf] SAFETY NET: re-request page=" << idx << "scale=" << m_fullRenderScale;
            requestPage(idx, m_fullRenderScale);
        }
    });
    connect(task, &PageRenderTask::pageObjectCount, this, &PdfRenderer::setPageObjectCount);

    if (isThumb)
        m_thumbPool.start(task, 0);
    else
        m_mainPool.start(task, 10);
}

void PdfRenderer::requestRegion(int pageIndex, double scale, QRect regionPx) {
    if (!m_doc || !m_doc->isOpen()) return;
    m_regionGeneration->fetchAndAddOrdered(1);
    auto genRef = m_regionGeneration;
    auto* task = new RegionRenderTask(this, m_doc,
                                       pageIndex, scale, regionPx,
                                       this, genRef);
    connect(task, &RegionRenderTask::finished, this,
            [this](int idx, double sc, QRect reg, QImage img) {
        if (!img.isNull()) {
            emit regionReady(idx, sc, reg, img);
        }
    });
    m_mainPool.start(task, 5);
}

bool PdfRenderer::requestFromCacheOnly(int pageIndex, double scale) {
    if (!m_doc || !m_doc->isOpen()) return false;
    if (m_cache.contains(pageIndex)) {
        qDebug() << "[perf] cache-only hit mem page=" << pageIndex;
        emit pageReady(pageIndex, m_cache[pageIndex]);
        return true;
    }
    if (m_tileCache && m_tileCache->isOpen() && m_tileCache->hasPage(pageIndex, CacheZoom::Full)) {
        QImage cached = m_tileCache->readPage(pageIndex, CacheZoom::Full);
        if (!cached.isNull()) {
            qDebug() << "[perf] cache-only hit disk page=" << pageIndex;
            m_cache.insert(pageIndex, cached);
            m_cacheBytes += static_cast<qint64>(cached.sizeInBytes());
            evictCache();
            emit pageReady(pageIndex, cached);
            return true;
        }
    }
    return false;
}

bool PdfRenderer::requestFromCacheOnlyForContinuous(int pageIndex, double /*scale*/)
{
    if (!m_doc || !m_doc->isOpen()) return false;

    // All full-quality renders use kFullRenderMaxPx for the long edge,
    // so we compute the actual rendered scale from page geometry, NOT from
    // the requested scale. This ensures cache hits regardless of visual zoom.
    QSizeF pgSz = m_doc->pageSize(pageIndex);
    double longSide = qMax(pgSz.width(), pgSz.height());
    double renderedScale = kFullRenderMaxPx / qMax(longSide, 1.0);

    // Memory cache — accept any full-quality image (zoom-independent)
    if (m_cache.contains(pageIndex)) {
        const QImage& cached = m_cache[pageIndex];
        qDebug() << "[perf] cache-only-for-cont hit mem page=" << pageIndex
                 << "renderedScale=" << renderedScale;
        emit continuousPageReady(pageIndex, cached, renderedScale);
        return true;
    }

    // Disk cache — also zoom-independent (all CacheZoom::Full entries
    // are at kFullRenderMaxPx resolution).
    if (m_tileCache && m_tileCache->isOpen()
        && m_tileCache->hasPage(pageIndex, CacheZoom::Full))
    {
        QImage cached = m_tileCache->readPage(pageIndex, CacheZoom::Full);
        if (!cached.isNull()) {
            qDebug() << "[perf] cache-only-for-cont hit disk page=" << pageIndex
                     << "renderedScale=" << renderedScale;
            m_cache.insert(pageIndex, cached);
            m_cacheBytes += static_cast<qint64>(cached.sizeInBytes());
            evictCache();
            emit continuousPageReady(pageIndex, cached, renderedScale);
            return true;
        }
    }
    return false;
}

void PdfRenderer::requestPageForContinuous(int pageIndex, double /*scale*/)
{
    if (!m_doc || !m_doc->isOpen()) {
        qDebug() << "[perf] cont SKIP page=" << pageIndex << "reason=noDoc";
        return;
    }

    // Compute the actual render scale from page geometry (kFullRenderMaxPx
    // clamping), not from the requested visual zoom. All full-quality renders
    // share the same resolution regardless of user zoom.
    QSizeF pgSz = m_doc->pageSize(pageIndex);
    double longSide = qMax(pgSz.width(), pgSz.height());
    double renderedScale = kFullRenderMaxPx / qMax(longSide, 1.0);

    // Accept any full-quality cached image (all at kFullRenderMaxPx resolution)
    if (m_cache.contains(pageIndex)) {
        qDebug() << "[perf] cont cache hit mem page=" << pageIndex
                 << "renderedScale=" << renderedScale;
        emit continuousPageReady(pageIndex, m_cache[pageIndex], renderedScale);
        return;
    }

    if (m_continuousRunning.load() >= kMaxContinuousRenders) {
        qDebug() << "[perf] cont SKIP page=" << pageIndex << "reason=maxConcurrent(" << m_continuousRunning.load() << "/" << kMaxContinuousRenders << ")";
        return;
    }
    m_continuousRunning.fetch_add(1);

    int gen = m_continuousGen->loadRelaxed();
    RenderRequest req{pageIndex, renderedScale, true, true, gen};

    auto* ptask = new ProgressiveRenderTask(this, m_doc, req, this,
                                             m_continuousGen, m_formibDoc, m_formibGate);

    connect(ptask, &ProgressiveRenderTask::finished, this,
            [this, pageIndex, renderedScale, gen](int idx, QImage img) {
        m_continuousRunning.fetch_sub(1);
        if (img.isNull()) {
            qDebug() << "[perf] cont drop page=" << idx << "reason=nullResult";
            return;
        }
        if (gen != m_continuousGen->loadRelaxed()) {
            qDebug() << "[perf] cont drop page=" << idx << "reason=genStale";
            return;
        }
        if (!m_cache.contains(pageIndex)) {
            m_cacheBytes += static_cast<qint64>(img.sizeInBytes());
            m_cache.insert(pageIndex, img);
        }
        evictCache();

        if (m_tileCache && m_tileCache->isOpen() && !m_writingCache.contains(pageIndex)) {
            m_writingCache.insert(pageIndex);
            QImage cacheImg = img;
            auto* watcher = new QFutureWatcher<void>(this);
            connect(watcher, &QFutureWatcher<void>::finished, this,
                    [this, watcher, pageIndex]() {
                m_writingCache.remove(pageIndex);
                watcher->deleteLater();
            });
            watcher->setFuture(QtConcurrent::run([tc = m_tileCache, pageIndex, cacheImg]() {
                tc->writePage(pageIndex, CacheZoom::Full, cacheImg);
            }));
        }

        qDebug() << "[perf] cont render done page=" << idx;
        emit continuousPageReady(idx, img, renderedScale);
    });

    connect(ptask, &ProgressiveRenderTask::pagePartial, this,
            [this](int idx, double sc, QImage img) {
        emit pagePartial(idx, sc, img);
    });

    qDebug() << "[perf] cont render start page=" << pageIndex;
    m_mainPool.start(ptask, 5);
}

void PdfRenderer::requestTiles(int page, double scale, QRect /*viewportPx*/) {
    if (!m_doc || !m_doc->isOpen()) return;
    // Tile path removed: full-page render at kFullRenderMaxPx serves every
    // zoom level — resolution is free for PDFium (benchmarked: 200px and
    // 4000px both cost ~3.1s on page 4). Tiles only added value for zoom
    // beyond 4000px, but each tile on a heavy page costs the same 3.1s,
    // multiplied by tile count.
    // Keep TileBatchRenderTask / RegionRenderTask and all tile code in
    // case lightweight pages need deep-zoom tiles in the future.
    requestPage(page, scale);
}

void PdfRenderer::preloadAdjacent(int pageIndex, double scale) {
    if (!m_doc || !m_doc->isOpen()) return;
    int total = m_doc->pageCount();
    // Preload only 1 page each side (each full 4000px render costs ~4.4s on heavy pages)
    for (int delta : {1, -1}) {
        int idx = pageIndex + delta;
        if (idx < 0 || idx >= total) continue;
        if (m_cache.contains(idx)) continue;
        qint64 pendKey = pendingCacheKey(idx, true);
        if (m_pendingRequests.contains(pendKey)) {
            qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_pendingRequests.value(pendKey) < 30000) continue;
            m_pendingRequests.remove(pendKey);
        }
        m_pendingRequests.insert(pendKey, QDateTime::currentMSecsSinceEpoch());
        int gen = m_generation->loadRelaxed();
        RenderRequest req{idx, 1.0, false, true, gen};
        auto* task = new PageRenderTask(this, m_doc, req, this,
                                        m_generation, nullptr, nullptr);
        connect(task, &PageRenderTask::finished, this,
                [this, idx](int /*idx2*/, QImage img) {
            qint64 fk = pendingCacheKey(idx, true);
            m_pendingRequests.remove(fk);
            if (!img.isNull() && !m_cache.contains(idx)) {
                m_cacheBytes += static_cast<qint64>(img.sizeInBytes());
                m_cache.insert(idx, img);
                evictCache();
            }
        });
        connect(task, &PageRenderTask::pageObjectCount, this, &PdfRenderer::setPageObjectCount);
        m_thumbPool.start(task, 0);
    }
}

void PdfRenderer::clearCache() {
    m_generation->fetchAndAddOrdered(1);
    m_fullRenderGen->fetchAndAddOrdered(1);
    m_progressiveGen->fetchAndAddOrdered(1);
    m_continuousGen->fetchAndAddOrdered(1);
    m_tileGeneration->fetchAndAddOrdered(1);
    m_cache.clear();
    m_cacheBytes = 0;
    m_tileCacheInMem.clear();
    m_tileCacheBytes = 0;
    m_pendingTiles.clear();
    m_pageObjectCount.clear();
    m_fullRenderRunning = false;
}

void PdfRenderer::invalidatePage(int pageIndex) {
    m_cache.remove(pageIndex);
    m_pageObjectCount.remove(pageIndex);
    m_generation->fetchAndAddOrdered(1);
    m_fullRenderGen->fetchAndAddOrdered(1);
    m_progressiveGen->fetchAndAddOrdered(1);
    m_pendingRequests.clear();
    m_fullRenderRunning = false;
    if (m_tileCache) m_tileCache->invalidatePage(pageIndex);
}

void PdfRenderer::setCurrentPage(int page) { m_currentPage = page; }

void PdfRenderer::setTileCache(std::shared_ptr<TileCacheFile> cache) { m_tileCache = std::move(cache); }
