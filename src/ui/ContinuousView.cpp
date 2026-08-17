#include "ContinuousView.h"
#include "ThemeTokens.h"
#include "../core/PdfDocument.h"
#include "../core/PdfRenderer.h"
#include "../core/PdfCoords.h"
#include <fpdfview.h>
#include <fpdf_edit.h>
#include "../annotations/AnnotationManager.h"
#include "../core/VectorGpuRenderer.h"

#include <QPainter>
#include <QScrollBar>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QResizeEvent>
#include <QPaintEvent>
#include <QApplication>
#include <QDebug>
#include <QFont>
#include <QFontMetrics>
#include <QOpenGLWidget>
#include <QOpenGLContext>
#include <QMatrix4x4>
#include <QVector4D>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <cmath>
#include <algorithm>
#include <QOpenGLFunctions>

extern QMutex s_pdfiumMutex;
// ── Constructor / Destructor ──────────────────────────────────────────────────

ContinuousView::ContinuousView(QWidget* parent)
    : QAbstractScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    // QAbstractScrollArea mac dinh ve corner mau palette o goc phai-duoi khi
    // ca 2 scrollbar hien ra — dat corner widget rieng de to mau theo token.
    auto* corner = new QWidget(this);
    corner->setObjectName("contCorner");
    setCornerWidget(corner);
    viewport()->setMouseTracking(true);

    // Link tinh xong o background: cap nhat con tro/hover ngay (SPEC_NO_SYNC_PAGELOAD).
    connect(PdfLinks::notifier(), &PdfLinksNotifier::linksReady, this,
            [this](quintptr doc, int page) { onLinksReady(doc, page); });

    // Zoom debounce: emit zoomChanged after 150 ms of no further scroll
    m_zoomTimer = new QTimer(this);
    m_zoomTimer->setSingleShot(true);
    m_zoomTimer->setInterval(150);
    connect(m_zoomTimer, &QTimer::timeout, this, [this] {
        emit zoomChanged(m_zoom);
        // Re-request all visible pages at the new zoom level now that the
        // user has finished the gesture.
        requestVisiblePages();
    });

    // Page-changed debounce: emit pageChanged 80 ms after scroll settles
    m_scrollTimer = new QTimer(this);
    m_scrollTimer->setSingleShot(true);
    m_scrollTimer->setInterval(80);
    connect(m_scrollTimer, &QTimer::timeout, this, [this] {
        int p = pageAtCenter();
        if (p != m_lastEmittedPage) {
            m_lastEmittedPage = p;
            emit pageChanged(p);
        }
    });

    // Sharp-region overlay is no longer needed (full-page renders at zoom resolution)

    // Settle timer for primary page (same pattern as single-mode: 400ms delay
    // before starting a new render, so fast scrolling doesn't trigger renders).
    m_contSettleTimer = new QTimer(this);
    m_contSettleTimer->setSingleShot(true);
    m_contSettleTimer->setInterval(400);
    connect(m_contSettleTimer, &QTimer::timeout, this, [this]() {
        if (m_primaryPage < 0 || !m_renderer) {
            qDebug() << "[perf] cont settle SKIP reason=noPrimaryPage";
            return;
        }
        // Skip re-render if we already have a full-quality (kFullRenderMaxPx) image.
        auto zit = m_pageImageZoom.constFind(m_primaryPage);
        if (zit != m_pageImageZoom.constEnd()) {
            double maxDim = qMax(m_pageSizePt[m_primaryPage].width(), m_pageSizePt[m_primaryPage].height());
            double maxResZoom = PdfRenderer::kFullRenderMaxPx / qMax(maxDim, 1.0);
            if (maxDim > 0 && qAbs(zit.value() - maxResZoom) < 1e-9) {
                qDebug() << "[perf] cont settle SKIP reason=alreadyFullQuality page=" << m_primaryPage;
                requestNeighborPages();
                return;
            }
        }
        qDebug() << "[perf] cont settle timeout — rendering primary page=" << m_primaryPage;
        if (vectorWillRender(m_primaryPage)) {
            qDebug() << "[cont] skip raster request (vector lo) page=" << m_primaryPage;
        } else {
            m_renderer->requestPageForContinuous(m_primaryPage, m_zoom);
        }
        m_primaryRequested = true;
    });

    // Sharp-region timer: debounced 180ms, requests crisp overlay for the primary
    // page when zoom is high enough that the base page image is being upscaled.
    m_sharpTimer = new QTimer(this);
    m_sharpTimer->setSingleShot(true);
    m_sharpTimer->setInterval(180);
    connect(m_sharpTimer, &QTimer::timeout, this, [this]() {
        if (!m_renderer || m_pageCount == 0 || m_primaryPage < 0) {
            qDebug() << "[perf] cont sharp request SKIP reason=noPrimaryPage";
            return;
        }
        const double longSidePx = qMax(m_pageSizePt[m_primaryPage].width(), m_pageSizePt[m_primaryPage].height()) * m_zoom;
        if (longSidePx <= PdfRenderer::kFullRenderMaxPx * 1.02) {
            m_sharpPage = -1;
            m_sharpPixmap = {};
            qDebug() << "[perf] cont sharp request SKIP reason=lowZoom longSidePx=" << longSidePx;
            return;
        }
        int scrollX = horizontalScrollBar()->value();
        int scrollY = verticalScrollBar()->value();
        int vpW = viewport()->width();
        int vpH = viewport()->height();
        int pg = m_primaryPage;
        int pL = pageLeftX(pg);
        int pT = pageTopY(pg);
        int visL = qMax(scrollX, pL);
        int visT = qMax(scrollY, pT);
        int visR = qMin(scrollX + vpW, pL + pageW(pg));
        int visB = qMin(scrollY + vpH, pT + pageH(pg));
        if (visR <= visL || visB <= visT) {
            qDebug() << "[perf] cont sharp request SKIP reason=emptyRegion";
            return;
        }
        QRect regionPx(visL - pL, visT - pT, visR - visL, visB - visT);
        qDebug() << "[perf] cont sharp request page=" << pg << "scale=" << m_zoom << "region=" << regionPx;
        emit regionNeeded(pg, m_zoom, regionPx);
    });

    // ── Flash link dich (SPEC_PDF_LINKS muc 4): ve lai lien tuc ~1 giay ────
    m_flashTimer = new QTimer(this);
    m_flashTimer->setInterval(50);
    connect(m_flashTimer, &QTimer::timeout, this, [this] {
        if (m_flashClock.elapsed() >= 1000) {
            m_flashTimer->stop();
            m_flashPage = -1;
            m_flashRect = QRectF();
        }
        viewport()->update();
    });

    // Tu cuon khi keo chon chu toi mep tren/duoi vung nhin (SPEC_TEXTSEL_ADOBE).
    m_autoScrollTimer = new QTimer(this);
    m_autoScrollTimer->setInterval(30);
    connect(m_autoScrollTimer, &QTimer::timeout, this, [this]{
        if (!m_selDragging) { stopAutoScroll(); return; }
        const int scrollY = verticalScrollBar()->value();
        const int vpH     = viewport()->height();
        const int zone    = 48;
        int delta = 0;
        if (m_autoScrollMousePos.y() < zone)
            delta = -std::max(8, (zone - m_autoScrollMousePos.y()) * 2);
        else if (m_autoScrollMousePos.y() > vpH - zone)
            delta = std::max(8, (m_autoScrollMousePos.y() - (vpH - zone)) * 2);
        if (delta != 0) {
            verticalScrollBar()->setValue(scrollY + delta);
            viewport()->update();
            updateTextSelectionFocus(m_autoScrollMousePos);
        }
    });

    setViewport(new QOpenGLWidget(this));
}

ContinuousView::~ContinuousView()
{
    if (m_vgrInit) {
        auto* glw = qobject_cast<QOpenGLWidget*>(viewport());
        if (glw) {
            glw->makeCurrent();
            m_vgr.release();
            glw->doneCurrent();
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void ContinuousView::setDocument(PdfDocument* doc, PdfRenderer* renderer)
{
    // Clear any text selection when document changes
    m_selecting = false;
    m_selStart  = m_selEnd = QPoint();
    m_selDragging = false;
    stopAutoScroll();
    clearTextSelectionInternal();

    // Clear markup selection/drag state (SPEC_CONTINUOUS_MARKUP_EDIT).
    m_hasSel = false;
    m_selPage = -1;
    m_draggingAnnot = false;
    clearDragTarget();
    m_dragPixelDelta = QPointF();
    m_dragNoteRect = QRectF();
    m_dragNoteOffsetPt = QPointF();

    // Disconnect old renderer
    if (m_renderer) {
        disconnect(m_continuousPageReadyConn);
        disconnect(m_regionReadyConn);
        disconnect(m_regionNeededConn);
    }

    // Guard: same doc+renderer → keep m_pageImages, just reconnect signals & refresh
    if (doc && renderer && doc == m_doc && renderer == m_renderer
        && m_pageCount > 0 && m_pageCount == doc->pageCount())
    {
        m_doc      = doc;
        m_renderer = renderer;
        m_continuousRequested.clear();
        m_continuousPageReadyConn = connect(
            m_renderer, &PdfRenderer::continuousPageReady,
            this, [this](int idx, QImage img, double renderedScale) {
                m_cacheProbed.remove(idx);
                if (img.isNull()) {
                    qDebug() << "[perf] cont SKIP continuousPageReady idx=" << idx << "reason=nullImage";
                    return;
                }
                if (idx < 0 || idx >= m_pageCount) {
                    qDebug() << "[perf] cont SKIP continuousPageReady idx=" << idx << "reason=outOfRange count=" << m_pageCount;
                    return;
                }
                // Accept any image — even zoom-mismatched — as a temporary placeholder.
                // The settle timer will request a zoom-matched render if needed.
                m_pageImages[idx] = QPixmap::fromImage(img);
                m_pageImageZoom[idx] = renderedScale;
                qDebug() << "[perf] cont ACCEPT continuousPageReady idx=" << idx
                         << "imgW=" << img.width() << "renderedScale=" << renderedScale;
                { static const bool dump = qEnvironmentVariableIsSet("TORREADER_DUMP");
                  if (dump && !img.isNull()) {
                      QString fn = QString("contview_dump_p%1.png").arg(idx);
                      img.save(fn);
                      qDebug() << "[dump] saved" << fn << "w=" << img.width() << "h=" << img.height();
                  }
                }
                m_continuousRequested.remove(idx);
                if (idx == m_primaryPage) {
                    m_contSettleTimer->stop();
                    requestNeighborPages();
                }
                if (pageVisible(idx))
                    viewport()->update();
            });
        m_regionReadyConn = connect(
            m_renderer, &PdfRenderer::regionReady,
            this, [this](int pageIndex, double scale, QRect regionPx, QImage img) {
                if (img.isNull()) {
                    qDebug() << "[perf] cont SKIP regionReady page=" << pageIndex << "reason=nullImage";
                    return;
                }
                int curPage = pageAtCenter();
                if (pageIndex != curPage) {
                    qDebug() << "[perf] cont SKIP regionReady page=" << pageIndex << "reason=notCenterPage center=" << curPage;
                    return;
                }
                if (qAbs(scale - m_zoom) > 1e-9) {
                    qDebug() << "[perf] cont SKIP regionReady page=" << pageIndex << "reason=zoomMismatch scale=" << scale << "zoom=" << m_zoom;
                    return;
                }
                m_sharpPage = pageIndex;
                m_sharpScale = scale;
                m_sharpRegion = regionPx;
                m_sharpPixmap = QPixmap::fromImage(img);
                viewport()->update();
            });
        m_regionNeededConn = connect(this, &ContinuousView::regionNeeded,
                                     m_renderer, &PdfRenderer::requestRegion);
        m_sharpPage = -1;
        m_sharpPixmap = {};
        requestVisiblePages();
        viewport()->update();
        return;
    }

    m_sharpPage = -1;
    m_sharpPixmap = {};
    if (m_vgrInit) {
        auto* glw = qobject_cast<QOpenGLWidget*>(viewport());
        if (glw) {
            glw->makeCurrent();
            m_vgr.release();
            glw->doneCurrent();
        }
        m_vgrInit = false;
    }
    m_doc      = doc;
    m_renderer = renderer;
    m_pageImages.clear();
    m_pageImageZoom.clear();
    m_pageAnnotVisuals.clear();
    m_continuousRequested.clear();
    m_vecLayers.clear();
    m_vecBuilding.clear();
    m_lastEmittedPage = -1;

    if (!m_doc || !m_doc->isOpen()) {
        m_pageCount = 0;
        m_pageSizePt.clear();
        rebuildLayout();
        viewport()->update();
        return;
    }

    m_pageCount = m_doc->pageCount();
    m_pageSizePt.resize(m_pageCount);
    for (int i = 0; i < m_pageCount; ++i)
        m_pageSizePt[i] = m_doc->pageSize(i);

    if (m_renderer) {
        m_continuousPageReadyConn = connect(
            m_renderer, &PdfRenderer::continuousPageReady,
            this, [this](int idx, QImage img, double renderedScale) {
                m_cacheProbed.remove(idx);
                if (img.isNull()) {
                    qDebug() << "[perf] cont SKIP continuousPageReady idx=" << idx << "reason=nullImage";
                    return;
                }
                if (idx < 0 || idx >= m_pageCount) {
                    qDebug() << "[perf] cont SKIP continuousPageReady idx=" << idx << "reason=outOfRange count=" << m_pageCount;
                    return;
                }
                // Accept any image — even zoom-mismatched — as a temporary placeholder.
                // The settle timer will request a zoom-matched render if needed.
                m_pageImages[idx] = QPixmap::fromImage(img);
                m_pageImageZoom[idx] = renderedScale;
                qDebug() << "[perf] cont ACCEPT continuousPageReady idx=" << idx
                         << "imgW=" << img.width() << "renderedScale=" << renderedScale;
                { static const bool dump = qEnvironmentVariableIsSet("TORREADER_DUMP");
                  if (dump && !img.isNull()) {
                      QString fn = QString("contview_dump_p%1.png").arg(idx);
                      img.save(fn);
                      qDebug() << "[dump] saved" << fn << "w=" << img.width() << "h=" << img.height();
                  }
                }
                m_continuousRequested.remove(idx);
                if (idx == m_primaryPage) {
                    m_contSettleTimer->stop();
                    requestNeighborPages();
                }
                if (pageVisible(idx))
                    viewport()->update();
            });

        m_regionReadyConn = connect(
            m_renderer, &PdfRenderer::regionReady,
            this, [this](int pageIndex, double scale, QRect regionPx, QImage img) {
                if (img.isNull()) {
                    qDebug() << "[perf] cont SKIP regionReady page=" << pageIndex << "reason=nullImage";
                    return;
                }
                int curPage = pageAtCenter();
                if (pageIndex != curPage) {
                    qDebug() << "[perf] cont SKIP regionReady page=" << pageIndex << "reason=notCenterPage center=" << curPage;
                    return;
                }
                if (qAbs(scale - m_zoom) > 1e-9) {
                    qDebug() << "[perf] cont SKIP regionReady page=" << pageIndex << "reason=zoomMismatch scale=" << scale << "zoom=" << m_zoom;
                    return;
                }
                m_sharpPage = pageIndex;
                m_sharpScale = scale;
                m_sharpRegion = regionPx;
                m_sharpPixmap = QPixmap::fromImage(img);
                viewport()->update();
            });

        m_regionNeededConn = connect(this, &ContinuousView::regionNeeded,
                                     m_renderer, &PdfRenderer::requestRegion);
    }

    rebuildLayout();
    // Scroll to top
    verticalScrollBar()->setValue(0);
    horizontalScrollBar()->setValue(0);
    requestVisiblePages();
}

void ContinuousView::clearDocument()
{
    setDocument(nullptr, nullptr);
}

void ContinuousView::setZoom(double scale)
{
    double newZoom = qBound(0.1, scale, 10.0);
    if (qAbs(newZoom - m_zoom) < 1e-9) return;

    // Preserve the canvas point at the viewport center
    QPoint vCenter(viewport()->width() / 2, viewport()->height() / 2);
    int scrollX = horizontalScrollBar()->value();
    int scrollY = verticalScrollBar()->value();
    // Canvas position under viewport center
    double canvasCX = scrollX + vCenter.x();
    double canvasCY = scrollY + vCenter.y();

    double ratio = newZoom / m_zoom;
    m_zoom = newZoom;
    // Keep old images as blurry placeholders; paintEvent scales them until
    // new renders arrive. Clearing here would cause a blank-page flash.
    rebuildLayout();

    // Keep the same canvas point under center
    int newScrollX = qMax(0, static_cast<int>(canvasCX * ratio) - vCenter.x());
    int newScrollY = qMax(0, static_cast<int>(canvasCY * ratio) - vCenter.y());
    horizontalScrollBar()->setValue(newScrollX);
    verticalScrollBar()->setValue(newScrollY);

    m_sharpPage = -1;
    m_sharpPixmap = {};
    m_primaryPage = -1;
    m_lastRequestZoom = -1.0;
    m_primaryRequested = false;
    m_contSettleTimer->stop();
    m_continuousRequested.clear();
    m_pageImageZoom.clear();
    m_cacheProbed.clear();
    viewport()->update();
    m_zoomTimer->start();
    m_sharpTimer->start();
}

void ContinuousView::setDarkMode(bool dark)
{
    m_darkMode = dark;
    viewport()->update();
}

void ContinuousView::scrollToPage(int pageIndex)
{
    if (m_pageCount == 0) return;
    pageIndex = qBound(0, pageIndex, m_pageCount - 1);
    int targetY = pageTopY(pageIndex);
    verticalScrollBar()->setValue(targetY);
}

void ContinuousView::scrollToPageRect(int page, const QRectF& rectPdf)
{
    if (m_pageCount == 0) return;
    page = qBound(0, page, m_pageCount - 1);
    const QRectF nr = rectPdf.normalized();
    if (nr.width() < 0.5 || nr.height() < 0.5) {
        scrollToPage(page);
        return;
    }
    // Dung lai DUNG phep quy doi paintEvent dang dung de ve highlight:
    // display point -> canvas pixel (linear theo pageW/pageH tren pageSizePt).
    double pw   = pageW(page);
    double ph   = pageH(page);
    double pwPt = m_pageSizePt[page].width();
    double phPt = m_pageSizePt[page].height();
    double rectCx = pageLeftX(page) + (nr.x() + nr.width()  / 2.0) / (pwPt > 0 ? pwPt : 1.0) * pw;
    double rectCy = pageTopY(page) + (nr.y() + nr.height() / 2.0) / (phPt > 0 ? phPt : 1.0) * ph;
    int vpW = viewport()->width();
    int vpH = viewport()->height();
    // Tam rect vao GIUA vung nhin (goc cuon = tam - nua vung nhin), khong doi zoom.
    horizontalScrollBar()->setValue(qMax(0, static_cast<int>(rectCx - vpW / 2.0)));
    verticalScrollBar()->setValue(qMax(0, static_cast<int>(rectCy - vpH / 2.0)));
}

QPointF ContinuousView::probeRectCenterInViewport(int page, const QRectF& rectPdf) const
{
    if (page < 0 || page >= m_pageCount) return QPointF(1e9, 1e9);
    const QRectF nr = rectPdf.normalized();
    double pw   = pageW(page);
    double ph   = pageH(page);
    double pwPt = m_pageSizePt[page].width();
    double phPt = m_pageSizePt[page].height();
    double rectCx = pageLeftX(page) + (nr.x() + nr.width()  / 2.0) / (pwPt > 0 ? pwPt : 1.0) * pw;
    double rectCy = pageTopY(page) + (nr.y() + nr.height() / 2.0) / (phPt > 0 ? phPt : 1.0) * ph;
    return QPointF(rectCx - horizontalScrollBar()->value(),
                   rectCy - verticalScrollBar()->value());
}

void ContinuousView::flashPageRect(int page, const QRectF& rectDisp)
{
    m_flashPage = qBound(0, page, qMax(0, m_pageCount - 1));
    m_flashRect = rectDisp.normalized();
    m_flashClock.restart();
    m_flashTimer->start();
    viewport()->update();
}

int ContinuousView::currentPage() const
{
    return pageAtCenter();
}

// ── Layout ────────────────────────────────────────────────────────────────────

void ContinuousView::rebuildLayout()
{
    m_pageTopY_cache.resize(m_pageCount);
    m_canvasW = 0;
    m_canvasH = 0;

    if (m_pageCount == 0) {
        updateScrollBars();
        return;
    }

    // Find widest rendered page for canvas width
    int maxPageW = 0;
    for (int i = 0; i < m_pageCount; ++i) {
        int w = pageW(i);
        if (w > maxPageW) maxPageW = w;
    }
    m_canvasW = maxPageW + kHPad;

    // Stack pages top-to-bottom
    int y = 0;
    for (int i = 0; i < m_pageCount; ++i) {
        m_pageTopY_cache[i] = y;
        y += pageH(i) + kGap;
    }
    // Remove the trailing gap after the last page
    m_canvasH = (m_pageCount > 0) ? y - kGap : 0;

    updateScrollBars();
}

void ContinuousView::updateScrollBars()
{
    int vpW = viewport()->width();
    int vpH = viewport()->height();

    horizontalScrollBar()->setRange(0, qMax(0, m_canvasW - vpW));
    horizontalScrollBar()->setPageStep(vpW);
    horizontalScrollBar()->setSingleStep(20);

    verticalScrollBar()->setRange(0, qMax(0, m_canvasH - vpH));
    verticalScrollBar()->setPageStep(vpH);
    verticalScrollBar()->setSingleStep(40);
}

// ── Coordinate helpers ────────────────────────────────────────────────────────

int ContinuousView::pageTopY(int i) const
{
    if (i < 0 || i >= m_pageTopY_cache.size()) return 0;
    return m_pageTopY_cache[i];
}

int ContinuousView::pageLeftX(int i) const
{
    // Center each page horizontally inside the canvas
    int center = m_canvasW / 2;
    return center - pageW(i) / 2;
}

int ContinuousView::pageW(int i) const
{
    if (i < 0 || i >= m_pageSizePt.size()) return 0;
    return qMax(1, static_cast<int>(m_pageSizePt[i].width() * m_zoom));
}

int ContinuousView::pageH(int i) const
{
    if (i < 0 || i >= m_pageSizePt.size()) return 0;
    return qMax(1, static_cast<int>(m_pageSizePt[i].height() * m_zoom));
}

bool ContinuousView::pageVisible(int i) const
{
    if (i < 0 || i >= m_pageCount) return false;
    int scrollY = verticalScrollBar()->value();
    int vpH     = viewport()->height();
    int top     = pageTopY(i);
    int bottom  = top + pageH(i);
    return bottom > scrollY && top < scrollY + vpH;
}

// ── Rendering helpers ─────────────────────────────────────────────────────────

int ContinuousView::pageAtCenter() const
{
    if (m_pageCount == 0) return 0;
    int scrollY = verticalScrollBar()->value();
    int vpH     = viewport()->height();
    int centerY = scrollY + vpH / 2;  // canvas Y at viewport center

    // Binary search for the page whose range contains centerY
    int lo = 0, hi = m_pageCount - 1, best = 0;
    while (lo <= hi) {
        int mid    = (lo + hi) / 2;
        int top    = pageTopY(mid);
        int bottom = top + pageH(mid);
        if (centerY >= top && centerY < bottom) {
            return mid;
        }
        if (centerY < top) {
            best = mid;   // centerY is in the gap before mid; keep as candidate
            hi = mid - 1;
        } else {
            best = mid;
            lo = mid + 1;
        }
    }
    return qBound(0, best, m_pageCount - 1);
}

void ContinuousView::requestVisiblePages()
{
    if (!m_renderer || m_pageCount == 0) {
        qDebug() << "[perf] cont SKIP requestVisiblePages reason=noDoc";
        return;
    }

    int primary = pageAtCenter();
    bool primaryChanged = (primary != m_primaryPage
                           || qAbs(m_zoom - m_lastRequestZoom) >= 1e-9);

    if (primaryChanged) {
        qDebug() << "[perf] cont primary=" << primary << "zoom=" << m_zoom;
        m_primaryPage = primary;
        m_lastRequestZoom = m_zoom;
        m_primaryRequested = false;
        m_contSettleTimer->stop();
        m_cacheProbed.clear();

        // Step a: primary page already determined (primary).
        //
        // Step b: try cache first — same pattern as single-mode:
        // requestFromCacheOnly → if hit, image served immediately (synchronous
        // signal), no settle.
        // Accept image at exact zoom match OR at full quality (kFullRenderMaxPx).
        auto it = m_pageImages.find(primary);
        if (it != m_pageImages.end()) {
            auto zit = m_pageImageZoom.constFind(primary);
            if (zit != m_pageImageZoom.constEnd()) {
                double storedZoom = zit.value();
                double maxDim = qMax(m_pageSizePt[primary].width(), m_pageSizePt[primary].height());
                double maxResZoom = PdfRenderer::kFullRenderMaxPx / qMax(maxDim, 1.0);
                if (qAbs(storedZoom - m_zoom) < 1e-9
                    || (maxDim > 0 && qAbs(storedZoom - maxResZoom) < 1e-9)) {
                    // Already have a good-enough pixmap: exact zoom match or
                    // full-quality render usable at any zoom.
                    m_continuousRequested.remove(primary);
                    requestNeighborPages();
                    goto evict;
                }
            }
        }
        if (vectorWillRender(primary)) {
            qDebug() << "[cont] skip raster request (vector lo) page=" << primary;
            requestNeighborPages();
            goto evict;
        }
        if (m_renderer->requestFromCacheOnlyForContinuous(primary, m_zoom)) {
            // Cache hit → image arrives synchronously via continuousPageReady
            // handler, which calls requestNeighborPages for us.
            m_continuousRequested.remove(primary);
            // After cache, check if the returned image matches zoom exactly.
            // If not (e.g. full-quality at different scale), we still accept it
            // but the settle timer below handles re-render if needed.
            auto zit = m_pageImageZoom.constFind(primary);
            if (zit != m_pageImageZoom.constEnd()
                && qAbs(zit.value() - m_zoom) >= 1e-9) {
                // Image is at full quality but zoom doesn't match user's zoom.
                // No re-render needed — full quality is always good enough.
                // Just proceed to neighbors.
                requestNeighborPages();
            }
            goto evict;
        }

        // Step c: cache miss → record probe (skip re-probe) and start settle timer
        m_cacheProbed[primary] = m_zoom;
        qDebug() << "[perf] cont primary cache miss page=" << primary
                 << "— starting 400ms settle";
        m_contSettleTimer->start();
    }

evict:
    // Always bound RAM: drop pages outside a small window around visible range.
    int first = -1, last = -1;
    for (int i = 0; i < m_pageCount; ++i) {
        if (pageVisible(i)) {
            if (first == -1) first = i;
            last = i;
        } else if (first != -1) {
            break;
        }
    }
    const int kKeepMargin = 4;
    int keepFirst = qMax(0, first - kKeepMargin);
    int keepLast  = qMin(m_pageCount - 1, last + kKeepMargin);
    for (auto it = m_pageImages.begin(); it != m_pageImages.end(); ) {
        if (it.key() < keepFirst || it.key() > keepLast) {
            m_continuousRequested.remove(it.key());
            m_pageImageZoom.remove(it.key());
            it = m_pageImages.erase(it);
        } else {
            ++it;
        }
    }
}

void ContinuousView::requestNeighborPages()
{
    // Neighbors (primary ±1) are requested AFTER primary is done, using the
    // non-cancellable requestPageForContinuous path. Never before primary.
    if (m_primaryPage < 0) return;
    int pages[] = {m_primaryPage - 1, m_primaryPage + 1};
    for (int i : pages) {
        if (i < 0 || i >= m_pageCount) continue;
        if (vectorWillRender(i)) {
            qDebug() << "[cont] skip raster request (vector lo) page=" << i;
            continue;
        }
        if (m_pageImages.contains(i))
            continue;
        if (m_renderer->requestFromCacheOnlyForContinuous(i, m_zoom))
            continue;
        if (!m_continuousRequested.contains(i)) {
            qDebug() << "[perf] cont neighbor request page=" << i << "zoom=" << m_zoom;
            m_renderer->requestPageForContinuous(i, m_zoom);
            m_continuousRequested.insert(i);
        }
    }
}

bool ContinuousView::vectorWillRender(int pg) const
{
    if (m_vecBuilding.contains(pg)) return true;
    auto it = m_vecLayers.constFind(pg);
    if (it != m_vecLayers.constEnd() && *it && (*it)->isReady() && (*it)->isComplete())
        return true;
    return false;
}

void ContinuousView::ensureVectorLayers()
{
    if (!m_doc || m_pageCount == 0) return;
    int center = pageAtCenter();
    int lo = qMax(0, center - 1);
    int hi = qMin(m_pageCount - 1, center + 1);
    for (int pg = lo; pg <= hi; ++pg) {
        if (m_vecLayers.contains(pg) || m_vecBuilding.contains(pg)) continue;
        m_vecBuilding.insert(pg);
        auto layer = std::make_shared<VectorLayer>();
        auto* w = new QFutureWatcher<bool>(this);
        connect(w, &QFutureWatcher<bool>::finished, this, [this, w, pg, layer] {
            w->deleteLater();
            m_vecBuilding.remove(pg);
            if (w->result()) m_vecLayers.insert(pg, layer);
            qDebug().noquote() << "[cont] vecLayer" << (w->result() ? "READY" : "SKIP")
                               << "page=" << pg << "cached=" << m_vecLayers.size();
            if (w->result() && !m_pageAnnotVisuals.contains(pg))
                needAnnotVisuals(pg);
            viewport()->update();
        });
        FPDF_DOCUMENT d = m_doc->raw();
        w->setFuture(QtConcurrent::run([layer, d, pg] {
            QMutexLocker lk(&s_pdfiumMutex);
            return layer->build(d, pg);
        }));
    }
    for (auto it = m_vecLayers.begin(); it != m_vecLayers.end(); ) {
        if (qAbs(it.key() - center) > 1) {
            qDebug().noquote() << "[cont] vecLayer EVICT page=" << it.key()
                               << "dist=" << qAbs(it.key() - center) << ">1"
                               << "remaining=" << m_vecLayers.size() - 1;
            it = m_vecLayers.erase(it);
        } else {
            ++it;
        }
    }
}

void ContinuousView::setTool(PdfGpuView::ViewTool tool) {
    m_tool = tool;
    m_selecting = false;   // huy vung chon dang keo khi doi cong cu
    m_selDragging = false;
    stopAutoScroll();
    if (tool != PdfGpuView::ViewTool::SelectText)
        clearTextSelectionInternal();
    // Chon markup: roi Pan thi bo chon (giong PdfGpuView doi tool xoa chon).
    if (tool != PdfGpuView::ViewTool::Pan) {
        m_hasSel = false;
        m_selPage = -1;
        clearDragTarget();
    }
    viewport()->setCursor(tool == PdfGpuView::ViewTool::SelectText
                              ? Qt::IBeamCursor : Qt::ArrowCursor);
}

void ContinuousView::setAllHighlights(const QHash<int, QList<QRectF>>& byPage,
                                      int currentPage, int currentIdxInPage) {
    m_highlightsByPage = byPage;
    m_highlightCurrentPage = currentPage;
    m_highlightCurrentIdx  = currentIdxInPage;
    qDebug().noquote() << QString("[find] paint highlights pages=%1 current=%2/%3")
        .arg(byPage.size()).arg(currentPage).arg(currentIdxInPage);
    viewport()->update();
}

void ContinuousView::clearAllHighlights() {
    m_highlightsByPage.clear();
    m_highlightCurrentPage = -1;
    m_highlightCurrentIdx  = -1;
    viewport()->update();
}

void ContinuousView::setAnnotVisualsForPage(int page, const QList<AnnotVisual>& visuals) {
    m_pageAnnotVisuals[page] = visuals;
    viewport()->update();
}

void ContinuousView::invalidatePage(int pageIndex) {
    m_pageImages.remove(pageIndex);
    m_pageImageZoom.remove(pageIndex);
    m_continuousRequested.remove(pageIndex);
    m_pageAnnotVisuals.remove(pageIndex);
    qDebug() << "[markup] invalidatePage page=" << pageIndex;
}

// ── Chon/keo markup (SPEC_CONTINUOUS_MARKUP_EDIT_2026-08-16) ────────────────
// rectPdf o TOA DO HIEN THI (Y-down, da ap /Rotate + pageBoxOrigin) — cung
// khong gian voi AnnotInfo.rect nen ve thang khong can quy doi lai.

void ContinuousView::setSelectedAnnot(int page, const QRectF& rectPdf) {
    m_dragPixelDelta = QPointF();
    m_selPage  = page;
    m_selRect  = rectPdf;
    m_hasSel   = true;
    viewport()->update();
}

void ContinuousView::clearSelectedAnnot() {
    m_hasSel = false;
    m_selPage = -1;
    clearDragTarget();
    viewport()->update();
}

void ContinuousView::setDragTarget(const QString& uid, const QString& ghostText,
                                   float fontSizePt, const QColor& ghostColor) {
    m_dragUid = uid;
    Q_UNUSED(ghostText); Q_UNUSED(fontSizePt); Q_UNUSED(ghostColor);
}

void ContinuousView::clearDragTarget() {
    m_dragUid.clear();
}

void ContinuousView::setDragNote(const QRectF& rPt) {
    m_dragNoteRect = rPt;
    m_dragNoteOffsetPt = QPointF();
    viewport()->update();
}

void ContinuousView::clearDragState() {
    m_dragPixelDelta = QPointF();
    m_dragNoteRect = QRectF();
    m_dragNoteOffsetPt = QPointF();
    m_dragUid.clear();
    m_draggingAnnot = false;
    viewport()->update();
}

bool ContinuousView::resolvePageDisplayPos(const QPoint& widgetPos, int* page, QPointF* dispPt) const {
    if (!m_doc || !m_doc->isOpen() || m_pageCount == 0) return false;
    const QPoint canvasPos = widgetPos
        + QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
    for (int i = 0; i < m_pageCount; ++i) {
        const int left = pageLeftX(i), top = pageTopY(i);
        const int right = left + pageW(i), bottom = top + pageH(i);
        if (canvasPos.x() < left || canvasPos.x() >= right
            || canvasPos.y() < top || canvasPos.y() >= bottom) continue;
        *page   = i;
        *dispPt = QPointF((canvasPos.x() - left) / m_zoom,
                          (canvasPos.y() - top) / m_zoom);
        return true;
    }
    return false;
}

void ContinuousView::probeSimulatePickDrag(int page, const QPointF& pressDisp, const QPointF& dragDisp) {
    if (page < 0 || page >= m_pageCount) return;
    int scrollX = horizontalScrollBar()->value();
    int scrollY = verticalScrollBar()->value();
    const QPointF pressW(pageLeftX(page) + pressDisp.x() * m_zoom - scrollX,
                         pageTopY(page) + pressDisp.y() * m_zoom - scrollY);
    const QPointF relW(pressW.x() + dragDisp.x() * m_zoom,
                       pressW.y() + dragDisp.y() * m_zoom);
    // Nhan chuot trai → pick (MainWindow handler chay dong bo, dat m_hasSel).
    {
        QMouseEvent pe(QEvent::MouseButtonPress, pressW, pressW,
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        mousePressEvent(&pe);
    }
    // Keo → cap nhat khung chon.
    {
        QMouseEvent me(QEvent::MouseMove, relW, relW,
                       Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        mouseMoveEvent(&me);
    }
    // Tha → emit annotationMoveRequested.
    {
        QMouseEvent re(QEvent::MouseButtonRelease, relW, relW,
                       Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        mouseReleaseEvent(&re);
    }
}

// ── QAbstractScrollArea overrides ─────────────────────────────────────────────

void ContinuousView::scrollContentsBy(int /*dx*/, int /*dy*/)
{
    viewport()->update();
    requestVisiblePages();
    ensureVectorLayers();
    m_scrollTimer->start();
    m_sharpTimer->start();
}

void ContinuousView::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateScrollBars();
    requestVisiblePages();
    ensureVectorLayers();
    m_sharpTimer->start();
}

// ── Paint ─────────────────────────────────────────────────────────────────────

void ContinuousView::paintEvent(QPaintEvent* /*event*/)
{
    { static bool logged = false;
      if (!logged) {
          qDebug() << "[cont] viewport=" << (qobject_cast<QOpenGLWidget*>(viewport()) ? "QOpenGLWidget" : "plain");
          logged = true;
      }
    }

    QElapsedTimer paintTimer;
    paintTimer.start();

    QPainter p(viewport());
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    // Background
    // Mau nen ban (sau to giay) theo theme HC: dark => #000000, light => bgAlt.
    QColor bg = m_darkMode ? QColor(darkHC().deskBg) : QColor(lightHC().deskBg);
    p.fillRect(viewport()->rect(), bg);

    if (m_pageCount == 0) {
        p.setPen(QColor(200, 200, 200));
        QFont f = p.font();
        f.setPointSize(13);
        p.setFont(f);
        p.drawText(viewport()->rect(), Qt::AlignCenter,
                   "TorReader PDF\n\nOpen a PDF to get started\n"
                   "File → Open   or   drag & drop");
        return;
    }

    int scrollX = horizontalScrollBar()->value();
    int scrollY = verticalScrollBar()->value();
    int vpW     = viewport()->width();
    int vpH     = viewport()->height();

    // Nghiem thu bang so (SPEC_SEARCH_STATE_R3): dem highlight duoc ve.
    int hlTotalPages = 0;
    int hlVisiblePages = 0;
    int hlDrawnRects = 0;
    for (auto it = m_highlightsByPage.constBegin(); it != m_highlightsByPage.constEnd(); ++it)
        if (!it->isEmpty()) ++hlTotalPages;

    for (int i = 0; i < m_pageCount; ++i) {
        // Canvas rect of this page
        int cx = pageLeftX(i);
        int cy = pageTopY(i);
        int cw = pageW(i);
        int ch = pageH(i);

        // Quick visibility test (canvas coords vs scroll window)
        if (cy + ch <= scrollY || cy >= scrollY + vpH) continue;
        if (cx + cw <= scrollX || cx >= scrollX + vpW) continue;

        // Convert canvas rect to viewport coords
        int vx = cx - scrollX;
        int vy = cy - scrollY;

        // Drop shadow (offset 4 px, semi-transparent)
        p.fillRect(vx + kShadow, vy + kShadow, cw, ch, QColor(0, 0, 0, 80));

        // White page background
        p.fillRect(vx, vy, cw, ch, Qt::white);

        // Vector overlay
        bool drewVector = false;
        auto vit = m_vecLayers.constFind(i);
        if (vit != m_vecLayers.constEnd() && *vit && (*vit)->isReady() && (*vit)->isComplete()) {
            const QSizeF vpSize = (*vit)->pageSizePt();
            if (vpSize.width() > 0 && vpSize.height() > 0) {
                const int rot = (*vit)->rotation() & 3;
                const double sx = (rot & 1) ? (cw / vpSize.height()) : (cw / vpSize.width());
                const double sy = (rot & 1) ? (ch / vpSize.width())  : (ch / vpSize.height());
                QMatrix4x4 mvp;
                mvp.ortho(0.f, (float)vpW, (float)vpH, 0.f, -1.f, 1.f);
                mvp.translate((float)vx, (float)vy, 0.f);
                switch (rot) {
                    case 1: mvp.translate((float)cw, 0.f, 0.f);       mvp.rotate(90.f,  0.f,0.f,1.f); break;
                    case 2: mvp.translate((float)cw, (float)ch, 0.f); mvp.rotate(180.f, 0.f,0.f,1.f); break;
                    case 3: mvp.translate(0.f, (float)ch, 0.f);       mvp.rotate(270.f, 0.f,0.f,1.f); break;
                    default: break;
                }
                mvp.scale((float)sx, (float)sy, 1.f);
                const float pxPerPt = (float)sx;
                p.beginNativePainting();
                if (!m_vgrInit) { m_vgr.initialize(); m_vgrInit = true; }
                {
                    QVector4D c0 = mvp * QVector4D(0, 0, 0, 1);
                    QVector4D c1 = mvp * QVector4D((float)vpW, (float)vpH, 0, 1);
                    static int s_i = -1; static float s_vx = -1, s_vy = -1;
                    static int s_cw = -1, s_ch = -1;
                    static int s_vpW = -1, s_vpH = -1;
                    static float s_c0x = 0, s_c0y = 0, s_c1x = 0, s_c1y = 0;
                    if (i != s_i || vx != s_vx || vy != s_vy || cw != s_cw || ch != s_ch
                        || vpW != s_vpW || vpH != s_vpH
                        || qAbs(c0.x() - s_c0x) > 0.01f || qAbs(c0.y() - s_c0y) > 0.01f
                        || qAbs(c1.x() - s_c1x) > 0.01f || qAbs(c1.y() - s_c1y) > 0.01f) {
                        qDebug().noquote() << "[cont] page=" << i << "vx=" << vx << "vy=" << vy
                                           << "cw=" << cw << "ch=" << ch
                                           << "vpW=" << vpW << "vpH=" << vpH
                                           << "ndc0=(" << c0.x() << "," << c0.y() << ")"
                                           << "ndc1=(" << c1.x() << "," << c1.y() << ")";
                        s_i = i; s_vx = vx; s_vy = vy; s_cw = cw; s_ch = ch;
                        s_vpW = vpW; s_vpH = vpH;
                        s_c0x = c0.x(); s_c0y = c0.y(); s_c1x = c1.x(); s_c1y = c1.y();
                    }
                }
                {
                    QOpenGLFunctions* gl = QOpenGLContext::currentContext()->functions();
                    const qreal dpr = devicePixelRatioF();
                    const int sx = int(std::floor(vx * dpr));
                    const int sy = int(std::floor((vpH - (vy + ch)) * dpr));
                    const int sw = int(std::ceil(cw * dpr));
                    const int sh = int(std::ceil(ch * dpr));
                    GLint prevBox[4] = {0,0,0,0};
                    gl->glGetIntegerv(GL_SCISSOR_BOX, prevBox);
                    GLboolean prevOn = gl->glIsEnabled(GL_SCISSOR_TEST);
                    gl->glEnable(GL_SCISSOR_TEST);
                    gl->glScissor(sx, sy, qMax(0, sw), qMax(0, sh));

                    m_vgr.draw(*(*vit), mvp, QSize(vpW, vpH), pxPerPt);

                    if (prevOn)
                        gl->glScissor(prevBox[0], prevBox[1], prevBox[2], prevBox[3]);
                    else
                        gl->glDisable(GL_SCISSOR_TEST);
                }
                p.endNativePainting();
                drewVector = true;
                if (m_pageImages.contains(i)) {
                    m_pageImages.remove(i);
                    m_pageImageZoom.remove(i);
                    m_continuousRequested.remove(i);
                    qDebug() << "[cont] drop raster (da co vector) page=" << i;
                }
            }
        }

        // Page content — skip raster + sharp region when vector layer is present
        if (!drewVector) {
            if (m_pageImages.contains(i)) {
                const QPixmap& px = m_pageImages[i];
                p.drawPixmap(QRect(vx, vy, cw, ch), px, px.rect());
                if (i == m_sharpPage && qAbs(m_sharpScale - m_zoom) < 1e-9 && !m_sharpPixmap.isNull()) {
                    p.drawPixmap(QPoint(vx + m_sharpRegion.x(), vy + m_sharpRegion.y()), m_sharpPixmap);
                }
            } else {
                // Placeholder while render is pending — white background only,
                // no text, so the transition from blank → blurry → sharp is smooth.
                p.fillRect(vx, vy, cw, ch, m_darkMode ? QColor(45, 45, 45) : Qt::white);
            }
        }

        // Search highlights cua trang nay — chi ve trang dang trong vung nhin.
        auto hit = m_highlightsByPage.constFind(i);
        if (hit != m_highlightsByPage.constEnd() && !hit->isEmpty()) {
            const QList<QRectF>& hls = *hit;
            double pwPt = m_pageSizePt[i].width();
            double phPt = m_pageSizePt[i].height();
            for (int hi = 0; hi < hls.size(); ++hi) {
                QRectF nr = hls[hi].normalized();
                if (nr.width() < 0.5 || nr.height() < 0.5) continue;
                double hx = vx + nr.x() / pwPt * cw;
                double hy = vy + nr.y() / phPt * ch;
                double hw = nr.width() / pwPt * cw;
                double hh = nr.height() / phPt * ch;
                if (i == m_highlightCurrentPage && hi == m_highlightCurrentIdx) {
                    p.fillRect(QRectF(hx, hy, hw, hh), QColor(255, 140, 0, 220));
                    p.setPen(QPen(QColor(200, 80, 0, 240), 2.0));
                    p.drawRect(QRectF(hx, hy, hw, hh));
                } else {
                    p.fillRect(QRectF(hx, hy, hw, hh), QColor(255, 220, 0, 90));
                }
                ++hlDrawnRects;
            }
            ++hlVisiblePages;
        }

        // Vung chon chu (SPEC_TEXTSEL_ADOBE): cung DUONG quy doi hien thi nhu
        // highlight search, mau xanh ban trong suot kieu Adobe, khong vien.
        auto sit = m_selRectsByPage.constFind(i);
        if (sit != m_selRectsByPage.constEnd() && !sit->isEmpty()) {
            double pwPt = m_pageSizePt[i].width();
            double phPt = m_pageSizePt[i].height();
            for (const QRectF& nr0 : *sit) {
                QRectF nr = nr0.normalized();
                if (nr.width() < 0.5 || nr.height() < 0.5) continue;
                double hx = vx + nr.x() / pwPt * cw;
                double hy = vy + nr.y() / phPt * ch;
                double hw = nr.width() / pwPt * cw;
                double hh = nr.height() / phPt * ch;
                p.fillRect(QRectF(hx, hy, hw, hh), QColor(0, 102, 204, 89));  // rgba(0,102,204,0.35)
            }
        }

        // Flash khung dich khi nhay link noi bo (SPEC_PDF_LINKS muc 4):
        // ve vien ~1 giay roi mo dan. rectDisp cung khong gian toa do hien thi.
        if (i == m_flashPage && !m_flashRect.isEmpty() && m_flashClock.isValid()) {
            double pwPt = m_pageSizePt[i].width();
            double phPt = m_pageSizePt[i].height();
            double fx = vx + m_flashRect.x() / (pwPt > 0 ? pwPt : 1.0) * cw;
            double fy = vy + m_flashRect.y() / (phPt > 0 ? phPt : 1.0) * ch;
            double fw = m_flashRect.width()  / (pwPt > 0 ? pwPt : 1.0) * cw;
            double fh = m_flashRect.height() / (phPt > 0 ? phPt : 1.0) * ch;
            double fade = qBound(0.0, 1.0 - m_flashClock.elapsed() / 1000.0, 1.0);
            p.setPen(QPen(QColor(255, 120, 0, int(255 * fade)),
                          qMax(1.5, 2.0 * m_zoom)));
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(fx, fy, fw, fh));
        }
    }

    {
        static int lastHlP = -1, lastHlV = -1, lastHlD = -1;
        if (hlTotalPages != lastHlP || hlVisiblePages != lastHlV || hlDrawnRects != lastHlD) {
            qInfo().noquote() << QString("[hl] pages=%1 visiblePages=%2 drawn=%3")
                .arg(hlTotalPages).arg(hlVisiblePages).arg(hlDrawnRects);
            lastHlP = hlTotalPages; lastHlV = hlVisiblePages; lastHlD = hlDrawnRects;
        }
    }

    {
        static int lastNv = -1, lastNr = -1;
        int nv = 0, nr = 0;
        for (int i = 0; i < m_pageCount; ++i) {
            int cx = pageLeftX(i), cy = pageTopY(i), cw2 = pageW(i), ch2 = pageH(i);
            if (cy + ch2 <= scrollY || cy >= scrollY + vpH) continue;
            if (cx + cw2 <= scrollX || cx >= scrollX + vpW) continue;
            auto vit2 = m_vecLayers.constFind(i);
            if (vit2 != m_vecLayers.constEnd() && *vit2 && (*vit2)->isReady() && (*vit2)->isComplete())
                ++nv;
            else if (m_pageImages.contains(i))
                ++nr;
        }
        if (nv != lastNv || nr != lastNr) {
            qDebug() << "[cont] paint vectorPages=" << nv << "rasterPages=" << nr;
            lastNv = nv; lastNr = nr;
        }
    }

    // ── Annotation overlay visuals ────────────────────────────────────────────
    if (!m_pageAnnotVisuals.isEmpty()) {
        p.setRenderHint(QPainter::Antialiasing, true);
        for (int i = 0; i < m_pageCount; ++i) {
            auto vit = m_pageAnnotVisuals.constFind(i);
            if (vit == m_pageAnnotVisuals.constEnd() || vit->isEmpty()) continue;

            int vx = pageLeftX(i) - scrollX;
            int vy = pageTopY(i) - scrollY;

            for (const AnnotVisual& av : *vit) {
                // FreeText/Note are page objects in the renderer — overlay skips them to avoid double-draw
                if (!av.paintByOverlay) continue;
                const bool isDragged = m_draggingAnnot && !m_dragUid.isEmpty()
                    && av.page == m_selPage && av.uid == m_dragUid;
                if (isDragged) { p.save(); p.translate(m_dragPixelDelta); }
                QPointF dOrig(vx + av.rect.x() * m_zoom, vy + av.rect.y() * m_zoom);
                QRectF dRect(dOrig, QSizeF(av.rect.width() * m_zoom, av.rect.height() * m_zoom));
                QPen strokePen(av.stroke.isValid() ? av.stroke : QColor(Qt::red), qMax(1.0, av.border * m_zoom));
                p.setPen(strokePen);
                p.setBrush(av.fill.isValid() && av.fill.alpha() > 0 ? QBrush(av.fill) : Qt::NoBrush);

                switch (av.subtype) {
                    case FPDF_ANNOT_INK: {
                        for (const auto& stroke : av.ink) {
                            if (stroke.size() < 2) continue;
                            QPolygonF poly;
                            for (const QPointF& pt : stroke)
                                poly << QPointF(vx + pt.x() * m_zoom, vy + pt.y() * m_zoom);
                            p.setBrush(Qt::NoBrush);
                            p.drawPolyline(poly);
                        }
                        break;
                    }
                    case FPDF_ANNOT_SQUARE:
                        p.drawRect(dRect);
                        break;
                    case FPDF_ANNOT_CIRCLE:
                        p.drawEllipse(dRect);
                        break;
                    case FPDF_ANNOT_HIGHLIGHT: {
                        if (!av.quads.isEmpty()) {
                            p.setBrush(QColor(255, 255, 0, 90));
                            p.setPen(Qt::NoPen);
                            for (const QRectF& qr : av.quads) {
                                QPointF qo(vx + qr.x() * m_zoom, vy + qr.y() * m_zoom);
                                p.drawRect(QRectF(qo, QSizeF(qr.width() * m_zoom, qr.height() * m_zoom)));
                            }
                        } else {
                            p.setBrush(QColor(255, 255, 0, 90));
                            p.setPen(Qt::NoPen);
                            p.drawRect(dRect);
                        }
                        break;
                    }
                    case FPDF_ANNOT_LINE: {
                        QPointF lA(vx + av.rect.left() * m_zoom, vy + av.rect.top() * m_zoom);
                        QPointF lB(vx + av.rect.right() * m_zoom, vy + av.rect.bottom() * m_zoom);
                        p.drawLine(lA, lB);
                        break;
                    }
                    case FPDF_ANNOT_POLYGON: {
                        if (!av.ink.isEmpty() && av.ink[0].size() >= 3) {
                            QPolygonF poly;
                            for (const QPointF& pt : av.ink[0])
                                poly << QPointF(vx + pt.x() * m_zoom, vy + pt.y() * m_zoom);
                            p.drawPolygon(poly);
                        }
                        break;
                    }
                    case FPDF_ANNOT_FREETEXT: {
                        double fs = qMax(6.0, av.fontSize * m_zoom);
                        QFont ft = p.font();
                        ft.setPointSizeF(fs);
                        p.setFont(ft);
                        p.setPen(av.stroke.isValid() ? QPen(av.stroke) : QPen(Qt::black));
                        p.drawText(dRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, av.text);
                        break;
                    }
                    case FPDF_ANNOT_TEXT: {
                        QRectF badge(dRect.center().x() - 14, dRect.center().y() - 14, 28, 28);
                        p.setBrush(QColor(245, 158, 11, 220));
                        p.setPen(QColor(180, 100, 0, 200));
                        p.drawRoundedRect(badge, 6, 6);
                        p.setPen(Qt::white);
                        QFont fnt = p.font(); fnt.setPointSize(10); fnt.setBold(true); p.setFont(fnt);
                        p.drawText(badge, Qt::AlignCenter, "N");
                        break;
                    }
                    default: break;
                }
                if (isDragged) p.restore();
            }
        }
    }

    // Khung chon markup (SPEC_CONTINUOUS_MARKUP_EDIT): giong PdfGpuView —
    // vien gach, toa do hien thi cua m_selPage, adjusted(-3,-3,3,3).
    if (m_hasSel && m_selPage >= 0 && m_selPage < m_pageCount) {
        int sx = pageLeftX(m_selPage) - scrollX;
        int sy = pageTopY(m_selPage) - scrollY;
        QRectF wr(sx + m_selRect.x() * m_zoom, sy + m_selRect.y() * m_zoom,
                  m_selRect.width() * m_zoom, m_selRect.height() * m_zoom);
        wr = wr.adjusted(-3, -3, 3, 3);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 120, 215), 1.5, Qt::DashLine));
        p.drawRect(wr);
    }

    // Draw Alt+drag selection rect on top of all pages
    if (m_selecting || m_selStart != m_selEnd)
        drawSelection(p);

    qint64 paintMs = paintTimer.elapsed();
    if (paintMs > 16)
        qDebug() << "[perf] cont paint ms=" << paintMs;
}

// ── Input events ──────────────────────────────────────────────────────────────

void ContinuousView::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        // Zoom gesture — keep canvas point under cursor fixed
        double delta   = event->angleDelta().y() / 1200.0;
        double newZoom = qBound(0.1, m_zoom + delta, 10.0);
        if (qAbs(newZoom - m_zoom) < 1e-9) {
            event->accept();
            return;
        }

        // Canvas position under cursor before zoom
        QPointF cursorVp = event->position();
        int scrollX = horizontalScrollBar()->value();
        int scrollY = verticalScrollBar()->value();
        double cursorCanvasX = scrollX + cursorVp.x();
        double cursorCanvasY = scrollY + cursorVp.y();

        double ratio = newZoom / m_zoom;
        m_zoom = newZoom;
        rebuildLayout();

        // Restore cursor canvas point to same viewport position
        int newScrollX = qMax(0, static_cast<int>(cursorCanvasX * ratio - cursorVp.x()));
        int newScrollY = qMax(0, static_cast<int>(cursorCanvasY * ratio - cursorVp.y()));
        horizontalScrollBar()->setValue(newScrollX);
        verticalScrollBar()->setValue(newScrollY);

        m_sharpPage = -1;
        m_sharpPixmap = {};
        m_primaryPage = -1;
        m_lastRequestZoom = -1.0;
        m_primaryRequested = false;
        m_contSettleTimer->stop();
        m_pageImageZoom.clear();
        m_cacheProbed.clear();
        viewport()->update();
        m_zoomTimer->start();
        m_sharpTimer->start();
        event->accept();
    } else {
        // Normal scroll — let QAbstractScrollArea handle it (moves scrollbars)
        QAbstractScrollArea::wheelEvent(event);
    }
}

void ContinuousView::mousePressEvent(QMouseEvent* event)
{
    // Ctrl+Left drag = text selection for translation (giu nguyen, SPEC phan 2).
    if ((event->modifiers() & Qt::ControlModifier)
        && event->button() == Qt::LeftButton)
    {
        m_selecting = true;
        m_selStart  = event->pos();
        m_selEnd    = event->pos();
        viewport()->setCursor(Qt::IBeamCursor);
        viewport()->update();
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        // Link: chi khi tool la Pan hoac Select, khong bat Ctrl. Bam vao link
        // thi di theo link truoc khi bat dau pan/chon chu (SPEC_PDF_LINKS muc 3).
        if (m_tool == PdfGpuView::ViewTool::Pan
            || m_tool == PdfGpuView::ViewTool::SelectText) {
            if (tryActivateLink(event)) {
                event->accept();
                return;
            }
        }
        if (m_tool == PdfGpuView::ViewTool::SelectText) {
            // Cong cu Select: nhan chuot = chon chu theo CHI SO KY TU (Adobe),
            // khong con quet hinh chu nhat.
            if (m_clickValid && m_clickClock.elapsed() > QApplication::doubleClickInterval())
                m_clickValid = false;
            m_clickClock.restart();
            m_clickValid = true;
            beginTextSelection(event->pos(), 1);
            event->accept();
            return;
        }
        if (m_tool == PdfGpuView::ViewTool::Pan) {
            // Chon/keo markup (SPEC_CONTINUOUS_MARKUP_EDIT): bam vao khung chon
            // da co → bat dau keo; nguoc lai → pick de MainWindow hit-test.
            if (m_hasSel && m_selPage >= 0) {
                int scrollX = horizontalScrollBar()->value();
                int scrollY = verticalScrollBar()->value();
                QRectF wr(pageLeftX(m_selPage) + m_selRect.x() * m_zoom - scrollX,
                          pageTopY(m_selPage) + m_selRect.y() * m_zoom - scrollY,
                          m_selRect.width()  * m_zoom,
                          m_selRect.height() * m_zoom);
                if (wr.adjusted(-3, -3, 3, 3).contains(event->pos())) {
                    m_draggingAnnot = true;
                    m_dragStart = event->pos();
                    m_dragOrigRect = m_selRect;
                    m_dragPixelDelta = QPointF(0, 0);
                    m_dragNoteRect = m_dragOrigRect;
                    m_dragNoteOffsetPt = QPointF();
                    event->accept();
                    return;
                }
            }
            int page = -1;
            QPointF dispPt;
            if (resolvePageDisplayPos(event->pos(), &page, &dispPt)) {
                emit annotationPickRequested(page, dispPt);
                event->accept();
                return;
            }
        }
        // Pan (mac dinh): keo chuot trai = keo trang.
        m_panning      = true;
        m_lastMousePos = event->pos();
        viewport()->setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::MiddleButton) {
        m_panning      = true;
        m_lastMousePos = event->pos();
        viewport()->setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void ContinuousView::mouseMoveEvent(QMouseEvent* event)
{
    if (m_draggingAnnot) {
        // Keo khung chon theo con tro (display space, Y-down nhu m_selRect).
        QPointF d = (QPointF(event->pos()) - QPointF(m_dragStart)) / m_zoom;
        m_selRect = m_dragOrigRect.translated(d);
        m_dragPixelDelta = event->pos() - m_dragStart;
        m_dragNoteOffsetPt = d;
        viewport()->update();
        event->accept();
        return;
    }

    if (m_selDragging) {
        m_autoScrollMousePos = event->pos();
        startAutoScrollIfNeeded(event->pos());
        updateTextSelectionFocus(event->pos());
        event->accept();
        return;
    }

    if (m_selecting) {
        m_selEnd = event->pos();
        viewport()->update();
        event->accept();
        return;
    }

    if (m_panning) {
        QPoint delta   = event->pos() - m_lastMousePos;
        m_lastMousePos = event->pos();

        int newH = verticalScrollBar()->value()   - delta.y();
        int newV = horizontalScrollBar()->value() - delta.x();
        verticalScrollBar()->setValue(newH);
        horizontalScrollBar()->setValue(newV);
        event->accept();
    } else {
        m_lastHoverPos = event->pos();
        updateLinkHover(event->pos());
        // Select: con tro I-beam chi khi roi tren chu; vung trong → mui ten.
        if (m_tool == PdfGpuView::ViewTool::SelectText && !m_hoveringLink)
            updateSelectCursor(event->pos());
        QAbstractScrollArea::mouseMoveEvent(event);
    }
}

void ContinuousView::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_draggingAnnot && event->button() == Qt::LeftButton) {
        m_draggingAnnot = false;
        m_dragPixelDelta = QPointF();
        m_dragNoteRect = QRectF();
        m_dragNoteOffsetPt = QPointF();
        viewport()->update();
        // Cung dinh dang PdfGpuView: dx,dy o he display (Y up) — MainWindow se
        // xoay nguoc theo /Rotate truoc khi goi AnnotationManager (dung duong san co).
        double dx = (event->pos().x() - m_dragStart.x()) / m_zoom;
        double dy = -(event->pos().y() - m_dragStart.y()) / m_zoom;
        if (qAbs(dx) > 1.0 || qAbs(dy) > 1.0)
            emit annotationMoveRequested(m_selPage, dx, dy);
        event->accept();
        return;
    }
    if (m_selDragging && event->button() == Qt::LeftButton) {
        if (m_selClickGesture)
            m_selClickGesture = false;   // nhay dup/ba: giu nguyen vung chon
        else
            updateTextSelectionFocus(event->pos());
        finishTextSelection();
        event->accept();
        return;
    }
    if (m_selecting && event->button() == Qt::LeftButton) {
        m_selecting = false;
        viewport()->setCursor(m_tool == PdfGpuView::ViewTool::SelectText
                                  ? Qt::IBeamCursor : Qt::ArrowCursor);

        int scrollX = horizontalScrollBar()->value();
        int scrollY = verticalScrollBar()->value();

        // Canvas coordinates of the selection rect
        int cx0 = qMin(m_selStart.x(), m_selEnd.x()) + scrollX;
        int cx1 = qMax(m_selStart.x(), m_selEnd.x()) + scrollX;
        int cy0 = qMin(m_selStart.y(), m_selEnd.y()) + scrollY;
        int cy1 = qMax(m_selStart.y(), m_selEnd.y()) + scrollY;

        // Ensure minimum height so horizontal drag captures a line of text
        const int kMinSelHeight = static_cast<int>(18 * m_zoom);
        if (cy1 - cy0 < kMinSelHeight) {
            int mid = (cy0 + cy1) / 2;
            cy0 = mid - kMinSelHeight / 2;
            cy1 = mid + kMinSelHeight / 2;
        }

        // Find the page that contains the top of the selection
        int foundPage = -1;
        for (int i = 0; i < m_pageCount; ++i) {
            int top    = pageTopY(i);
            int bottom = top + pageH(i);
            if (cy0 >= top && cy0 < bottom) { foundPage = i; break; }
        }

        if (foundPage >= 0 && m_zoom > 0.0) {
            // Canvas pixel -> diem hien thi (Y-down, goc trai tren). Day la
            // CUNG khong gian ma scrollToPageRect / highlight ve dang dung:
            // pageW/pageH da la kich thuoc hien thi (render thread sua theo
            // /Rotate), nen buoc nay khong can biet rotation.
            double xd0 = (cx0 - pageLeftX(foundPage)) / m_zoom;
            double xd1 = (cx1 - pageLeftX(foundPage)) / m_zoom;
            double yd0 = (cy0 - pageTopY(foundPage))  / m_zoom;
            double yd1 = (cy1 - pageTopY(foundPage))  / m_zoom;
            const QRectF disp = QRectF(QPointF(xd0, yd0), QPointF(xd1, yd1)).normalized();
            const QSizeF dsp  = m_pageSizePt[foundPage];

            // Lay /Rotate + goc hop trang de nghich dao qua dispToPdf — DUNG LAI
            // dung phep quy doi cua PdfCoords (search/scrollToPageRect dung de ra
            // toa do hien thi), khong viet ban thu hai (SPEC phan 2 muc 4).
            int rot = 0;
            QPointF box(0.0, 0.0);
            if (m_doc && m_doc->raw()) {
                QMutexLocker lock(&s_pdfiumMutex);
                FPDF_PAGE pg = FPDF_LoadPage(m_doc->raw(), foundPage);
                if (pg) {
                    rot = FPDFPage_GetRotation(pg) & 3;
                    box = pdfBoxOrigin(pg);
                    FPDF_ClosePage(pg);
                }
            }
            const double Wd = dsp.width(), Hd = dsp.height();
            const QPointF tl = dispToPdf(disp.left(),  disp.top(),    Wd, Hd, rot, box.x(), box.y());
            const QPointF br = dispToPdf(disp.right(), disp.bottom(), Wd, Hd, rot, box.x(), box.y());
            const QRectF pageRect = QRectF(tl, br).normalized();
            emit textRegionSelected(foundPage, pageRect,
                                    event->globalPosition().toPoint());
        }

        viewport()->update();
        event->accept();
        return;
    }
    if (m_panning &&
        (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton))
    {
        m_panning = false;
        viewport()->setCursor(Qt::ArrowCursor);
        event->accept();
    } else {
        QAbstractScrollArea::mouseReleaseEvent(event);
    }
}

void ContinuousView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (m_tool == PdfGpuView::ViewTool::SelectText && event->button() == Qt::LeftButton) {
        // Nhay ba = dblclick thu 2 lien tiep trong doubleClickInterval.
        const bool isTriple = m_clickValid && m_clickClock.elapsed() <= QApplication::doubleClickInterval();
        m_clickClock.restart();
        m_clickValid = true;
        beginTextSelection(event->pos(), isTriple ? 3 : 2);
        event->accept();
        return;
    }
    QAbstractScrollArea::mouseDoubleClickEvent(event);
}

void ContinuousView::contextMenuEvent(QContextMenuEvent* event)
{
    // Tim trang duoi con tro chuot (tinh theo vi tri cuon). Bam phai len annot
    // → annotationContextRequested (MainWindow hien menu Edit/Properties/Delete nhu
    // PdfGpuView — SPEC_CONTINUOUS_MARKUP_EDIT muc 4). Chuot phai vung trong → van
    // la menu OCR page (handler cung xu ly miss).
    int page = -1;
    QPointF dispPt;
    if (resolvePageDisplayPos(event->pos(), &page, &dispPt)) {
        emit annotationContextRequested(page, dispPt, event->globalPos());
        return;
    }
    QAbstractScrollArea::contextMenuEvent(event);
}

void ContinuousView::drawSelection(QPainter& p)
{
    if (!m_selecting || m_selStart == m_selEnd) return;
    int x = qMin(m_selStart.x(), m_selEnd.x());
    int y = qMin(m_selStart.y(), m_selEnd.y());
    int w = qAbs(m_selEnd.x() - m_selStart.x());
    int h = qAbs(m_selEnd.y() - m_selStart.y());
    p.fillRect(x, y, w, h, QColor(0, 120, 255, 50));
    p.setPen(QPen(QColor(0, 100, 220, 200), 1));
    p.drawRect(x, y, w, h);
}

// ── Chon chu theo chi so ky tu (SPEC_TEXTSEL_ADOBE) ──────────────────────────

void ContinuousView::setSelectionRects(const QHash<int, QList<QRectF>>& byPage) {
    m_selRectsByPage = byPage;
    viewport()->update();
}

void ContinuousView::clearSelectionRects() {
    m_selRectsByPage.clear();
    viewport()->update();
}

bool ContinuousView::resolvePageSpacePos(const QPoint& widgetPos, int* page, QPointF* pagePt,
                                         bool load) const {
    if (!m_doc || !m_doc->isOpen() || m_pageCount == 0) return false;
    const QPoint canvasPos = widgetPos
        + QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
    for (int i = 0; i < m_pageCount; ++i) {
        const int left = pageLeftX(i), top = pageTopY(i);
        const int right = left + pageW(i), bottom = top + pageH(i);
        if (canvasPos.x() < left || canvasPos.x() >= right
            || canvasPos.y() < top || canvasPos.y() >= bottom) continue;
        // Diem hien thi (Y-down, goc trai tren, da ap /Rotate + CropBox) —
        // cung khong gian voi highlight search.
        const QPointF disp((canvasPos.x() - left) / m_zoom,
                           (canvasPos.y() - top) / m_zoom);
        // mouseMove chi doc dem (khong nap); press duoc phep nap.
        const TextSelection::PageInfo info = load
            ? TextSelection::pageFor(m_doc->raw(), i)
            : TextSelection::pageForCached(m_doc->raw(), i);
        if (!info.tp) return false;   // trang chua san — khong xac dinh toa do
        *page   = i;
        *pagePt = TextSelection::dispToPagePt(info, disp);
        return true;
    }
    return false;
}

void ContinuousView::beginTextSelection(const QPoint& widgetPos, int clickCount) {
    int page = -1;
    QPointF pagePt;
    if (!resolvePageSpacePos(widgetPos, &page, &pagePt, /*load=*/true)) {
        clearTextSelectionInternal();
        viewport()->update();
        return;
    }
    const TextSelection::PageInfo info = TextSelection::pageFor(m_doc->raw(), page);
    const double tolX = 4.0 / m_zoom;
    const double tolY = 6.0 / m_zoom;
    int idx = TextSelection::charIndexAt(info.tp, pagePt.x(), pagePt.y(), tolX, tolY);
    if (idx < 0)
        idx = TextSelection::nearestCharAt(info.tp, pagePt.x(), pagePt.y(), tolY);
    if (idx < 0) {
        // Khong co ky tu/dong nao gan: vung chon RONG, khong ve gi.
        clearTextSelectionInternal();
        m_selDragging = true;
        viewport()->update();
        return;
    }
    if (clickCount >= 3) {
        int s = 0, c = 0;
        TextSelection::lineRange(info.tp, idx, &s, &c);
        m_selAnchorPage = page; m_selAnchorChar = s;
        m_selFocusPage  = page; m_selFocusChar  = s + c - 1;
    } else if (clickCount == 2) {
        int s = 0, c = 0;
        TextSelection::wordRange(info.tp, idx, &s, &c);
        m_selAnchorPage = page; m_selAnchorChar = s;
        m_selFocusPage  = page; m_selFocusChar  = s + c - 1;
    } else {
        m_selAnchorPage = page; m_selAnchorChar = idx;
        m_selFocusPage  = page; m_selFocusChar  = idx;
    }
    m_selClickGesture = (clickCount >= 2);
    m_selDragging = true;
    emitSelectionState();
    viewport()->update();
}

void ContinuousView::updateTextSelectionFocus(const QPoint& widgetPos) {
    int page = -1;
    QPointF pagePt;
    if (!resolvePageSpacePos(widgetPos, &page, &pagePt)) return;
    const TextSelection::PageInfo info = TextSelection::pageForCached(m_doc->raw(), page);
    const double tolX = 4.0 / m_zoom;
    const double tolY = 6.0 / m_zoom;
    int idx = TextSelection::charIndexAt(info.tp, pagePt.x(), pagePt.y(), tolX, tolY);
    if (idx < 0)
        idx = TextSelection::nearestCharAt(info.tp, pagePt.x(), pagePt.y(), tolY);
    if (idx < 0) return;   // keo qua vung trong: giu focus cu
    m_selFocusPage = page;
    m_selFocusChar = idx;
    emitSelectionState();
    viewport()->update();
}

void ContinuousView::finishTextSelection() {
    m_selDragging = false;
    stopAutoScroll();
    viewport()->setCursor(m_tool == PdfGpuView::ViewTool::SelectText
                              ? Qt::IBeamCursor : Qt::ArrowCursor);
}

void ContinuousView::emitSelectionState() {
    emit textSelectionChanged(m_selAnchorPage, m_selAnchorChar,
                              m_selFocusPage, m_selFocusChar);
}

void ContinuousView::clearTextSelectionInternal() {
    m_selAnchorPage = m_selFocusPage = -1;
    m_selAnchorChar = m_selFocusChar = -1;
    m_selRectsByPage.clear();
    m_selClickGesture = false;
    emit textSelectionCleared();
}

void ContinuousView::updateSelectCursor(const QPoint& widgetPos) {
    int page = -1;
    QPointF pagePt;
    bool overText = false;
    if (resolvePageSpacePos(widgetPos, &page, &pagePt)) {
        const TextSelection::PageInfo info = TextSelection::pageForCached(m_doc->raw(), page);
        const double tolX = 4.0 / m_zoom;
        const double tolY = 6.0 / m_zoom;
        const int idx = TextSelection::charIndexAt(info.tp, pagePt.x(), pagePt.y(), tolX, tolY);
        overText = idx >= 0;
    }
    viewport()->setCursor(overText ? Qt::IBeamCursor : Qt::ArrowCursor);
}

void ContinuousView::startAutoScrollIfNeeded(const QPoint& widgetPos) {
    const int vpH = viewport()->height();
    const int zone = 48;
    const bool atEdge = (widgetPos.y() < zone || widgetPos.y() > vpH - zone);
    if (atEdge && !m_autoScrollTimer->isActive())
        m_autoScrollTimer->start();
    else if (!atEdge && m_autoScrollTimer->isActive())
        m_autoScrollTimer->stop();
}

void ContinuousView::stopAutoScroll() {
    if (m_autoScrollTimer && m_autoScrollTimer->isActive())
        m_autoScrollTimer->stop();
}

// ── Link hover / click (SPEC_PDF_LINKS) ──────────────────────────────────────

void ContinuousView::updateLinkHover(const QPoint& widgetPos)
{
    const bool linkTool = (m_tool == PdfGpuView::ViewTool::Pan
                           || m_tool == PdfGpuView::ViewTool::SelectText);
    const QString hoverTxt = [&]() -> QString {
        if (!linkTool || !m_doc || !m_doc->isOpen() || m_pageCount == 0)
            return QString();
        const QPoint canvasPos = widgetPos
            + QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
        for (int i = 0; i < m_pageCount; ++i) {
            int left = pageLeftX(i), top = pageTopY(i);
            int right = left + pageW(i), bottom = top + pageH(i);
            if (canvasPos.x() < left || canvasPos.x() >= right
                || canvasPos.y() < top || canvasPos.y() >= bottom) continue;

            const QPointF dispPt((canvasPos.x() - left) / m_zoom,
                                 (canvasPos.y() - top) / m_zoom);
            QElapsedTimer _lt; _lt.start();
            const PdfLinks::CachedPage cp = PdfLinks::cachedForPage(m_doc->raw(), i);
            qDebug().noquote() << "[links] hover page=" << i
                               << "cached=" << (cp.ready ? 1 : 0)
                               << "blockedMs=" << _lt.elapsed();
            if (!cp.ready) {
                // Chua tinh link: KHONG chan giao dien. Con tro giu mui ten,
                // xep viec tinh nen; xong thi notifier bao ve lai (lan re sau
                // cung co ban tay).
                PdfLinks::requestPage(m_doc->raw(), i);
                return QString();
            }
            const int li = PdfLinks::linkAt(cp.links, dispPt, cp.info);
            if (li >= 0) {
                // Link LAUNCH/REMOTEGOTO khong co uri va dest: khong doi con tro.
                if (cp.links[li].uri.isEmpty() && cp.links[li].destPage < 0) return QString();
                return cp.links[li].uri.isEmpty()
                    ? QString("Page %1").arg(cp.links[li].destPage + 1) : cp.links[li].uri;
            }
            return QString();
        }
        return QString();
    }();

    if (hoverTxt.isEmpty()) {
        if (m_hoveringLink) {
            m_hoveringLink = false;
            if (m_tool != PdfGpuView::ViewTool::SelectText)
                viewport()->setCursor(Qt::ArrowCursor);
            emit linkHovered(QString());
        }
    } else {
        if (!m_hoveringLink) {
            m_hoveringLink = true;
            viewport()->setCursor(Qt::PointingHandCursor);
        }
        emit linkHovered(hoverTxt);
    }
}

// Link tinh xong: neu con tro van dang nam tren widget thi chay lai hover de
// hien ban tay / boi to ngay, khong can re chuot (SPEC_NO_SYNC_PAGELOAD muc 1).
void ContinuousView::onLinksReady(quintptr /*doc*/, int pageIndex)
{
    if (!viewport()->underMouse()) return;
    if (pageIndex < 0 || pageIndex >= m_pageCount) return;
    updateLinkHover(m_lastHoverPos);
}

bool ContinuousView::tryActivateLink(QMouseEvent* event)
{
    if (!m_doc || !m_doc->isOpen() || m_pageCount == 0) return false;
    const QPoint canvasPos = event->pos()
        + QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
    for (int i = 0; i < m_pageCount; ++i) {
        int left = pageLeftX(i), top = pageTopY(i);
        int right = left + pageW(i), bottom = top + pageH(i);
        if (canvasPos.x() < left || canvasPos.x() >= right
            || canvasPos.y() < top || canvasPos.y() >= bottom) continue;

        const QPointF dispPt((canvasPos.x() - left) / m_zoom,
                             (canvasPos.y() - top) / m_zoom);
        const PdfLinks::CachedPage cp = PdfLinks::cachedForPage(m_doc->raw(), i);
        if (!cp.ready) {
            // Chua co dem: KHONG tra link, xu ly nhu binh thuong (Pan/Select).
            // Khong duoc "cho cho co dem roi moi xu ly" — cho la dung hinh.
            PdfLinks::requestPage(m_doc->raw(), i);
            return false;
        }
        const int li = PdfLinks::linkAt(cp.links, dispPt, cp.info);
        if (li >= 0) {
            emit linkActivated(i, cp.links[li]);
            return true;
        }
        return false;
    }
    return false;
}
