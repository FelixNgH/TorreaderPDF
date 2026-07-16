#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "PdfRenderer.h"
#include <QMutex>
#include <QMutexLocker>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QDebug>
#include <fpdf_annot.h>

// s_pdfiumMutex defined in PdfDocument.cpp — serializes all PDFium calls.
extern QMutex s_pdfiumMutex;

// ── PageRenderTask ────────────────────────────────────────────────────────────

PageRenderTask::PageRenderTask(FPDF_DOCUMENT doc, PdfDocument* pdfDoc, RenderRequest req,
                               QObject* receiver,
                               std::shared_ptr<QAtomicInt> genRef,
                               FormibDocPtr formibDoc,
                               std::shared_ptr<FormibGate> gate)
    : m_doc(doc), m_pdfDoc(pdfDoc), m_req(req)
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
        emit finished(m_req.pageIndex, QImage()); return;
    }

    qDebug() << "[Renderer] page=" << m_req.pageIndex
             << "scale=" << m_req.scaleFactor
             << "gen=" << m_req.generation
             << "hasFormib=" << (bool)m_formibDoc;

    // ── Phase 2-C: Try FormibPDF (no global mutex, fully parallel) ────────────
    // Only if the adaptive gate says this page/doc is worth trying — avoids wasting
    // a full parallel render on pages FormibPDF already failed, or on a document it
    // has proven unable to handle.
    // PDFium is the PRIMARY renderer (reference quality). FormibPDF is used ONLY for
    // low-res previews / thumbnails (scale <= 0.4), where its mutex-free parallelism
    // makes scrolling fast and the reduced resolution hides any rendering differences.
    // The main page view and ALL zoom levels render with PDFium for best fidelity.
    // FormibPDF is a preview-quality rasterizer — it can drop objects / break
    // layout on complex pages, and the accept/reject heuristic below only catches
    // blank or mostly-black output (not missing objects). Restrict it to genuine
    // thumbnail scales (panel renders at 0.15). Large drawing pages fit-to-window
    // in the MAIN view land at scale ~0.2–0.5 → now always PDFium (reference).
    bool isPreviewScale = m_req.scaleFactor <= 0.16;
    bool tryFormib = isPreviewScale && m_formibDoc && (!m_gate || m_gate->shouldTry(m_req.pageIndex));
    if (tryFormib && formibpdf_page_count(m_formibDoc.get()) > 0) {
        double wPt = 0, hPt = 0;
        bool sizeOk = formibpdf_page_size(m_formibDoc.get(),
                                static_cast<uint32_t>(m_req.pageIndex),
                                &wPt, &hPt) && wPt > 0 && hPt > 0;
        // Skip oversized pages (A2+ / architectural drawings): FormibPDF is slow
        // and usually fails on these, while a full-page parallel render of a
        // 2400pt page burns CPU/RAM. Normal documents (Letter/A4/A3) are well
        // under this threshold and still use FormibPDF. Skipping does NOT count
        // against the global disable ratio (it's a deliberate choice).
        if (sizeOk && qMax(wPt, hPt) > 8000.0) {
            if (m_gate) m_gate->skipPage(m_req.pageIndex);
            qDebug() << "[Renderer]   FormibPDF skip oversized page" << m_req.pageIndex
                     << "(" << wPt << "x" << hPt << ")";
        } else if (sizeOk) {
            const int kMaxPx = 4000; // raised from 1500: dense architectural sheets need
                                 // more pixels to stay readable when zoomed in.
            double scale = m_req.scaleFactor;
            int imgW = qMax(1, static_cast<int>(wPt * scale));
            int imgH = qMax(1, static_cast<int>(hPt * scale));
            if (imgW > kMaxPx || imgH > kMaxPx) {
                double cap = qMin(static_cast<double>(kMaxPx)/imgW,
                                  static_cast<double>(kMaxPx)/imgH);
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
                    // near-black: sum of channels very low
                    if (qRed(c) + qGreen(c) + qBlue(c) < 60) ++nearBlack;
                }
                double blackFrac = sampled > 0 ? (double)nearBlack / sampled : 0.0;
                qDebug() << "[Renderer]   FormibPDF quality nonWhite=" << nonWhite
                         << "/" << sampled << " blackFrac=" << blackFrac;
                // Accept FormibPDF only if it drew content AND the result is not a
                // mostly-black blob. A >60% near-black page almost always means a
                // FormibPDF rasterizer bug (runaway tiling pattern, bad mesh triangle,
                // exploded stroke width) — fall back to PDFium for a correct render.
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
                // Rejected: record so future renders of this page skip FormibPDF.
                if (m_gate) m_gate->recordReject(m_req.pageIndex);
                if (looksBroken)
                    qDebug() << "[Renderer]   -> FormibPDF BROKEN (too black), fallback to PDFium page" << m_req.pageIndex;
                else
                    qDebug() << "[Renderer]   -> FormibPDF blank, fallback to PDFium page" << m_req.pageIndex;
            } else {
                if (m_gate) m_gate->recordReject(m_req.pageIndex);
                qDebug() << "[Renderer]   -> FormibPDF FAILED, fallback to PDFium page" << m_req.pageIndex;
            }
        } else {
            // page_size failed → reject this page.
            if (m_gate) m_gate->recordReject(m_req.pageIndex);
            qDebug() << "[Renderer]   FormibPDF page_size FAILED for page" << m_req.pageIndex;
        }
    }

    QImage image;
    if (m_genRef->loadRelaxed() != m_req.generation) {
        emit finished(m_req.pageIndex, QImage()); return;
    }

    // ── Single-doc path ── global mutex serializes all PDFium calls ──────
    qDebug() << "[Renderer]   PDFium render page=" << m_req.pageIndex;
    {
        QMutexLocker lock(&s_pdfiumMutex);
        if (m_genRef->loadRelaxed() != m_req.generation) {
            emit finished(m_req.pageIndex, QImage()); return;
        }

        FPDF_PAGE page = FPDF_LoadPage(m_doc, m_req.pageIndex);
        if (!page) { emit finished(m_req.pageIndex, QImage()); return; }

        double w = FPDF_GetPageWidth(page);
        double h = FPDF_GetPageHeight(page);
        if (m_pdfDoc) m_pdfDoc->updatePageSize(m_req.pageIndex, w, h);

        const int kMaxPx = 4000;
        double effectiveScale = m_req.scaleFactor;
        int imgW = qMax(1, static_cast<int>(w * effectiveScale));
        int imgH = qMax(1, static_cast<int>(h * effectiveScale));
        if (imgW > kMaxPx || imgH > kMaxPx) {
            double capRatio = qMin((double)kMaxPx / imgW, (double)kMaxPx / imgH);
            effectiveScale *= capRatio;
            imgW = qMax(1, static_cast<int>(w * effectiveScale));
            imgH = qMax(1, static_cast<int>(h * effectiveScale));
        }

        image = QImage(imgW, imgH, QImage::Format_ARGB32);
        image.fill(Qt::white);
        FPDF_BITMAP bmp = FPDFBitmap_CreateEx(imgW, imgH, FPDFBitmap_BGRA,
                                               image.bits(), image.bytesPerLine());
        FPDF_RenderPageBitmap(bmp, page, 0, 0, imgW, imgH, 0, FPDF_ANNOT);
        FPDFBitmap_Destroy(bmp);
        FPDF_ClosePage(page);
    }

    if (m_genRef->loadRelaxed() != m_req.generation) return;
    qDebug() << "[Renderer]   PDFium done page=" << m_req.pageIndex
             << "imgSize=" << image.size();
    emit finished(m_req.pageIndex, image);
}

// ── RegionRenderTask ──────────────────────────────────────────────────────────

RegionRenderTask::RegionRenderTask(FPDF_DOCUMENT doc, PdfDocument* pdfDoc,
                                   int pageIndex, double scale, QRect regionPx,
                                   QObject* receiver,
                                   std::shared_ptr<QAtomicInt> genRef)
    : m_doc(doc), m_pdfDoc(pdfDoc)
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

    QMutexLocker lock(&s_pdfiumMutex);
    if (!m_genRef || m_genRef->loadRelaxed() != m_reqGen) {
        emit finished(m_pageIndex, m_scale, m_regionPx, QImage()); return;
    }

    FPDF_PAGE page = FPDF_LoadPage(m_doc, m_pageIndex);
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
    FPDF_RenderPageBitmap(bmp, page,
                          -m_regionPx.x(), -m_regionPx.y(),
                          fullW, fullH, 0, FPDF_ANNOT);
    FPDFBitmap_Destroy(bmp);
    FPDF_ClosePage(page);

    if (!m_genRef || m_genRef->loadRelaxed() != m_reqGen) {
        emit finished(m_pageIndex, m_scale, m_regionPx, QImage()); return;
    }

    emit finished(m_pageIndex, m_scale, m_regionPx, image);
}

// ── PdfRenderer ───────────────────────────────────────────────────────────────

PdfRenderer::PdfRenderer(QObject* parent)
    : QObject(parent)
    , m_generation(std::make_shared<QAtomicInt>(0))
    , m_regionGeneration(std::make_shared<QAtomicInt>(0))
    , m_formibGate(std::make_shared<FormibGate>())
{
    // Thumbnails via FormibPDF are mutex-free (each FormibDoc is independent).
    // Use half the available CPUs for thumbnails so main renders still get cores.
    const int cpus = qMax(1, QThread::idealThreadCount());
    m_thumbPool.setMaxThreadCount(qMax(2, cpus / 2));
    m_thumbPool.setExpiryTimeout(-1);
    // Main pool: 2 threads only.
    // PDFium has a global mutex so extra threads just queue → freeze when user
    // navigates fast. FormibPDF uses rayon internally (parallel strips inside
    // one render call), so 1 FormibPDF render already saturates all CPUs.
    // With 2 threads: current page + 1 preload can run concurrently; remaining
    // preloads stay queued and are cancelled cheaply on navigation.
    m_mainPool.setMaxThreadCount(2);
    m_mainPool.setExpiryTimeout(-1);
}

PdfRenderer::~PdfRenderer() {
    m_generation->fetchAndAddOrdered(999);
    m_thumbPool.waitForDone();
    m_mainPool.waitForDone();
    // FormibDoc reset AFTER all tasks done (tasks hold shared_ptr copies)
    m_formibDoc.reset();
}

void PdfRenderer::setDocument(PdfDocument* doc) {
    cancelPending();
    m_doc = doc;
    clearCache();

    // Reset old FormibDoc (tasks already cancelled, no races)
    m_formibDoc.reset();
    // Fresh adaptive gate for the new document — verdicts don't carry over.
    m_formibGate = std::make_shared<FormibGate>();

    // FormibDoc is opened separately via setFormibDoc() after async loading.
    // Do not block the UI thread here.
}

void PdfRenderer::setFormibDoc(FormibDocPtr doc) {
    m_formibDoc = std::move(doc);
}

void PdfRenderer::cancelPending() {
    m_generation->fetchAndAddOrdered(1);
    m_pendingRequests.clear();
    m_thumbPool.clear();  // drop queued-but-not-started tasks instantly
    m_mainPool.clear();
}

void PdfRenderer::clearStalePending() {
    // Only clears the tracking set — does NOT increment generation or stop running
    // tasks. Stale tasks that returned early without emitting finished left their
    // keys permanently in m_pendingRequests, blocking those pages from ever being
    // re-requested. Clearing the set on each scroll tick unblocks them; running
    // tasks that eventually emit finished call remove() which is a safe no-op.
    m_pendingRequests.clear();
}

// Single O(n log n) pass: sort entries by distance from current page, evict farthest first.
void PdfRenderer::evictCache() {
    if (m_cacheBytes <= kMaxCacheBytes) return;
    using KV = QPair<int, qint64>;
    QVector<KV> byDist;
    byDist.reserve(m_cache.size());
    for (auto it = m_cache.cbegin(); it != m_cache.cend(); ++it)
        byDist.append({qAbs((int)(it.key() / 10000LL) - m_currentPage), it.key()});
    std::sort(byDist.begin(), byDist.end(),
              [](const KV& a, const KV& b) { return a.first > b.first; });
    for (const auto& kv : byDist) {
        if (m_cacheBytes <= kMaxCacheBytes || m_cache.size() <= 1) break;
        m_cacheBytes -= static_cast<qint64>(m_cache[kv.second].sizeInBytes());
        m_cache.remove(kv.second);
    }
}

void PdfRenderer::requestPage(int pageIndex, double scale) {
    if (!m_doc || !m_doc->isOpen()) return;

    // If FormibPDF proved unable to handle this document, free its in-memory copy
    // of the whole file (can be hundreds of MB) and stop attaching it to tasks.
    if (m_formibDoc && m_formibGate && m_formibGate->disabled.load()) {
        qDebug() << "[Renderer] FormibPDF disabled for this document — freeing FormibDoc, using PDFium only";
        m_formibDoc.reset();
    }

    bool   isThumb = (scale <= 0.25);
    double qs      = quantize(scale);
    qint64 key     = cacheKey(pageIndex, qs);

    if (m_cache.contains(key)) {
        QImage cached = m_cache[key];
        // Validate in-memory cache dimensions — same stale-DPI issue as tile cache.
        bool inMemOk = true;
        if (m_doc && !cached.isNull()) {
            QSizeF ps = m_doc->pageSize(pageIndex);
            if (!ps.isEmpty()) {
                int expW = qMax(1, qRound(ps.width()  * qs));
                int expH = qMax(1, qRound(ps.height() * qs));
                int tolW = qMax(2, (int)(expW * 0.05));
                int tolH = qMax(2, (int)(expH * 0.05));
                inMemOk = (qAbs(cached.width()  - expW) <= tolW &&
                           qAbs(cached.height() - expH) <= tolH);
                if (!inMemOk) {
                    qDebug() << "[Renderer] in-mem cache STALE page=" << pageIndex
                             << "cached=" << cached.size()
                             << "expected=(" << expW << "x" << expH << ") — evicting";
                    m_cacheBytes -= static_cast<qint64>(cached.sizeInBytes());
                    m_cache.remove(key);
                }
            }
        }
        if (inMemOk) {
            emit pageReady(pageIndex, cached);
            return;
        }
        // Stale — fall through to re-render
    }

    // Check persistent disk tile cache (instant on second open)
    if (m_tileCache && m_tileCache->isOpen()) {
        CacheZoom band = zoomBandFor(scale);
        if (m_tileCache->hasPage(pageIndex, band)) {
            QImage cached = m_tileCache->readPage(pageIndex, band);
            if (!cached.isNull()) {
                // Validate dimensions: stale cache entries from old DPI scale or
                // pre-rotation-fix sessions have wrong image size → discard them.
                bool sizeOk = true;
                if (m_doc) {
                    QSizeF ps = m_doc->pageSize(pageIndex);
                    if (!ps.isEmpty()) {
                        int expW = qMax(1, qRound(ps.width()  * qs));
                        int expH = qMax(1, qRound(ps.height() * qs));
                        int tolW = qMax(2, (int)(expW * 0.05));
                        int tolH = qMax(2, (int)(expH * 0.05));
                        sizeOk = (qAbs(cached.width()  - expW) <= tolW &&
                                  qAbs(cached.height() - expH) <= tolH);
                        if (!sizeOk)
                            qDebug() << "[Renderer] tile cache STALE page=" << pageIndex
                                     << "cached=" << cached.size()
                                     << "expected=(" << expW << "x" << expH << ") — discarding";
                    }
                }
                if (sizeOk) {
                    m_cache.insert(key, cached);
                    m_cacheBytes += static_cast<qint64>(cached.sizeInBytes());
                    evictCache();
                    emit pageReady(pageIndex, cached);
                    return;
                }
                // Stale — fall through to re-render; new result will overwrite cache
            }
        }
        // No low-res placeholder: showing blurry then sharp causes visual jitter
        // worse than a brief blank. Preloaded pages hit the cache directly.
    }

    // Skip if an identical request is already in the queue.
    if (m_pendingRequests.contains(key)) return;

    // No progressive/blurry placeholder — show white until the correct-zoom
    // render is ready. Blurry placeholders cause text to look garbled and then
    // "jump" to sharp, which looks worse than a brief white flash.

    m_pendingRequests.insert(key);

    int gen = m_generation->loadRelaxed();
    RenderRequest req{pageIndex, scale, true, gen};
    auto* task = new PageRenderTask(m_doc->raw(), m_doc, req, this,
                                    m_generation,
                                    m_formibDoc, m_formibGate); // shared_ptr copies — safe

    connect(task, &PageRenderTask::finished, this,
            [this, key, scale](int idx, QImage img) {
        m_pendingRequests.remove(key);  // always remove — cleans up stale keys
        if (!img.isNull()) {
            if (!m_cache.contains(key)) {
                m_cacheBytes += static_cast<qint64>(img.sizeInBytes());
                m_cache.insert(key, img);
            }
            evictCache();
            if (m_tileCache && m_tileCache->isOpen()) {
                CacheZoom band = zoomBandFor(scale);
                if (!m_tileCache->hasPage(idx, band))
                    m_tileCache->writePage(idx, band, img);
            }
        }
        emit pageReady(idx, img);
    });

    if (isThumb)
        m_thumbPool.start(task, 0);
    else
        m_mainPool.start(task, 10);
}

void PdfRenderer::requestRegion(int pageIndex, double scale, QRect regionPx) {
    if (!m_doc || !m_doc->isOpen()) return;
    m_regionGeneration->fetchAndAddOrdered(1);
    auto genRef = m_regionGeneration;
    auto* task = new RegionRenderTask(m_doc->raw(), m_doc,
                                       pageIndex, scale, regionPx,
                                       this, genRef);
    connect(task, &RegionRenderTask::finished, this,
            [this](int idx, double sc, QRect reg, QImage img) {
        if (!img.isNull())
            emit regionReady(idx, sc, reg, img);
    });
    m_mainPool.start(task, 5);
}

void PdfRenderer::preloadAdjacent(int pageIndex, double scale) {
    if (!m_doc || !m_doc->isOpen()) return;
    int total = m_doc->pageCount();
    // Preload only the immediate neighbours (±2). Complex pages can take 3-4s each
    // in PDFium; aggressively preloading ±5 jams the 2-thread pool and stalls
    // navigation for many seconds when the user jumps around.
    for (int delta : {1, -1, 2, -2}) {
        int idx = pageIndex + delta;
        if (idx < 0 || idx >= total) continue;

        // Cap preload resolution so speculative renders stay cheap and free the pool
        // threads quickly. The sharp full-res render happens via requestPage() when
        // the user actually lands on the page.
        double preScale = scale;
        QSizeF ps = m_doc->pageSize(idx);
        if (!ps.isEmpty()) {
            double longPx = qMax(ps.width(), ps.height()) * preScale;
            const double kPreloadMaxPx = 1000.0;
            if (longPx > kPreloadMaxPx)
                preScale = scale * (kPreloadMaxPx / longPx);
        }

        double qs  = quantize(preScale);
        qint64 key = cacheKey(idx, qs);
        if (m_cache.contains(key) || m_pendingRequests.contains(key)) continue;
        m_pendingRequests.insert(key);
        int gen = m_generation->loadRelaxed();
        RenderRequest req{idx, preScale, false, gen};
        auto* task = new PageRenderTask(m_doc->raw(), m_doc, req, this,
                                        m_generation,
                                        m_formibDoc, m_formibGate);
        connect(task, &PageRenderTask::finished, this,
                [this, key](int /*idx2*/, QImage img) {
            m_pendingRequests.remove(key);
            if (!img.isNull() && !m_cache.contains(key)) {
                m_cacheBytes += static_cast<qint64>(img.sizeInBytes());
                m_cache.insert(key, img);
                evictCache();
            }
            // Silently cache only — no pageReady emit from preloads.
            // Emitting here would show a stale low-res image before the
            // full-res main render arrives, causing a blurry→sharp jitter.
            // requestPage() emits immediately on cache hit when the user navigates.
        });
        m_mainPool.start(task, 2);
    }
}

void PdfRenderer::clearCache() {
    m_cache.clear();
    m_cacheBytes = 0;
}

void PdfRenderer::setCurrentPage(int page) { m_currentPage = page; }

void PdfRenderer::setTileCache(TileCacheFile* cache) { m_tileCache = cache; }
