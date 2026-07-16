#include "ContinuousView.h"
#include "../core/PdfDocument.h"
#include "../core/PdfRenderer.h"

#include <QPainter>
#include <QScrollBar>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QResizeEvent>
#include <QPaintEvent>
#include <QApplication>
#include <QFont>
#include <QFontMetrics>
#include <algorithm>

// ── Constructor / Destructor ──────────────────────────────────────────────────

ContinuousView::ContinuousView(QWidget* parent)
    : QAbstractScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    viewport()->setMouseTracking(true);

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

    // Sharp-region debounce: request sharp overlay 180 ms after last zoom/scroll
    m_sharpTimer = new QTimer(this);
    m_sharpTimer->setSingleShot(true);
    m_sharpTimer->setInterval(180);
    connect(m_sharpTimer, &QTimer::timeout, this, [this] {
        if (!m_renderer) return;
        int page = pageAtCenter();
        if (page < 0 || page >= m_pageCount) return;

        auto it = m_pageImages.find(page);
        if (it == m_pageImages.end()) return;
        if (it->width() >= pageW(page) - 2) return;

        int scrollX = horizontalScrollBar()->value();
        int scrollY = verticalScrollBar()->value();
        int vpW = viewport()->width();
        int vpH = viewport()->height();

        int cx = pageLeftX(page);
        int cy = pageTopY(page);
        int cw = pageW(page);
        int ch = pageH(page);

        int interLeft = qMax(cx, scrollX);
        int interTop = qMax(cy, scrollY);
        int interRight = qMin(cx + cw, scrollX + vpW);
        int interBottom = qMin(cy + ch, scrollY + vpH);
        if (interRight <= interLeft || interBottom <= interTop) return;

        const int margin = 256;
        int rx = qMax(0, (interLeft - cx) - margin);
        int ry = qMax(0, (interTop - cy) - margin);
        int rw = qMin(cw, (interRight - interLeft) + 2 * margin);
        int rh = qMin(ch, (interBottom - interTop) + 2 * margin);

        m_renderer->requestRegion(page, m_zoom, QRect(rx, ry, rw, rh));
    });
}

ContinuousView::~ContinuousView() = default;

// ── Public API ────────────────────────────────────────────────────────────────

void ContinuousView::setDocument(PdfDocument* doc, PdfRenderer* renderer)
{
    // Clear any text selection when document changes
    m_selecting = false;
    m_selStart  = m_selEnd = QPoint();

    // Disconnect old renderer
    if (m_renderer) {
        disconnect(m_pageReadyConn);
        disconnect(m_regionReadyConn);
    }

    // Guard: same doc+renderer → keep m_pageImages, just reconnect signals & refresh
    if (doc && renderer && doc == m_doc && renderer == m_renderer
        && m_pageCount > 0 && m_pageCount == doc->pageCount())
    {
        m_doc      = doc;
        m_renderer = renderer;
        m_pageReadyConn = connect(
            m_renderer, &PdfRenderer::pageReady,
            this, [this](int idx, QImage img) {
                if (img.isNull()) return;
                if (idx < 0 || idx >= m_pageCount) return;
                auto it = m_pageImages.find(idx);
                if (it != m_pageImages.end()) {
                    int expectedW = qMax(1, static_cast<int>(
                        m_pageSizePt[idx].width() * m_zoom));
                    bool existingIsCurrentZoom =
                        qAbs(it->width() - expectedW) < expectedW * 0.15;
                    if (existingIsCurrentZoom
                        && img.width() < it->width()
                        && img.height() < it->height())
                        return;
                }
                m_pageImages[idx] = QPixmap::fromImage(img);
                if (pageVisible(idx))
                    viewport()->update();
            });
        m_regionReadyConn = connect(
            m_renderer, &PdfRenderer::regionReady,
            this, [this](int pageIndex, double scale, QRect regionPx, QImage img) {
                if (img.isNull()) return;
                int curPage = pageAtCenter();
                if (pageIndex != curPage) return;
                if (qAbs(scale - m_zoom) > 1e-9) return;
                m_sharpPage = pageIndex;
                m_sharpScale = scale;
                m_sharpRegion = regionPx;
                m_sharpPixmap = QPixmap::fromImage(img);
                viewport()->update();
            });
        requestVisiblePages();
        viewport()->update();
        m_sharpTimer->start();
        return;
    }

    m_sharpPage = -1;
    m_doc      = doc;
    m_renderer = renderer;
    m_pageImages.clear();
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
        m_pageReadyConn = connect(
            m_renderer, &PdfRenderer::pageReady,
            this, [this](int idx, QImage img) {
                if (img.isNull()) return;
                if (idx < 0 || idx >= m_pageCount) return;
                // Zoom-aware replacement: only refuse a smaller image if the existing
                // one is already at the current zoom level (progressive loading).
                // If zoom changed, the existing image is stale — always replace it.
                auto it = m_pageImages.find(idx);
                if (it != m_pageImages.end()) {
                    int expectedW = qMax(1, static_cast<int>(
                        m_pageSizePt[idx].width() * m_zoom));
                    bool existingIsCurrentZoom =
                        qAbs(it->width() - expectedW) < expectedW * 0.15;
                    if (existingIsCurrentZoom
                        && img.width() < it->width()
                        && img.height() < it->height())
                        return;
                }
                m_pageImages[idx] = QPixmap::fromImage(img);
                if (pageVisible(idx))
                    viewport()->update();
            });

        m_regionReadyConn = connect(
            m_renderer, &PdfRenderer::regionReady,
            this, [this](int pageIndex, double scale, QRect regionPx, QImage img) {
                if (img.isNull()) return;
                int curPage = pageAtCenter();
                if (pageIndex != curPage) return;
                if (qAbs(scale - m_zoom) > 1e-9) return;
                m_sharpPage = pageIndex;
                m_sharpScale = scale;
                m_sharpRegion = regionPx;
                m_sharpPixmap = QPixmap::fromImage(img);
                viewport()->update();
            });
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
    viewport()->update();
    m_zoomTimer->start();
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
    if (!m_renderer || m_pageCount == 0) return;

    // Find first and last visible page
    int first = -1, last = -1;
    for (int i = 0; i < m_pageCount; ++i) {
        if (pageVisible(i)) {
            if (first == -1) first = i;
            last = i;
        } else if (first != -1) {
            break;  // past the visible range
        }
    }
    if (first == -1) return;

    // Include one-page preload buffer on each side
    int preloadFirst = qMax(0, first - 1);
    int preloadLast  = qMin(m_pageCount - 1, last + 1);

    for (int i = preloadFirst; i <= preloadLast; ++i)
        m_renderer->requestPage(i, m_zoom);

    // Bound RAM: drop cached page pixmaps outside a small window around the
    // visible range. Without this, scrolling through a large document keeps one
    // full-resolution QPixmap per visited page resident forever (GBs on big
    // merged files with large drawing pages). The renderer keeps its own bounded
    // cache, so re-scrolling re-renders cheaply from there.
    const int kKeepMargin = 4;
    int keepFirst = qMax(0, first - kKeepMargin);
    int keepLast  = qMin(m_pageCount - 1, last + kKeepMargin);
    for (auto it = m_pageImages.begin(); it != m_pageImages.end(); ) {
        if (it.key() < keepFirst || it.key() > keepLast)
            it = m_pageImages.erase(it);
        else
            ++it;
    }
}

// ── QAbstractScrollArea overrides ─────────────────────────────────────────────

void ContinuousView::scrollContentsBy(int /*dx*/, int /*dy*/)
{
    viewport()->update();
    requestVisiblePages();
    m_scrollTimer->start();
    m_sharpTimer->start();
}

void ContinuousView::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateScrollBars();
    requestVisiblePages();
}

// ── Paint ─────────────────────────────────────────────────────────────────────

void ContinuousView::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(viewport());
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    // Background
    QColor bg = m_darkMode ? QColor(30, 30, 30) : QColor(50, 50, 50);
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

        // Page content
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

    // Draw Alt+drag selection rect on top of all pages
    if (m_selecting || m_selStart != m_selEnd)
        drawSelection(p);
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
        viewport()->update();
        m_zoomTimer->start();
        event->accept();
    } else {
        // Normal scroll — let QAbstractScrollArea handle it (moves scrollbars)
        QAbstractScrollArea::wheelEvent(event);
    }
}

void ContinuousView::mousePressEvent(QMouseEvent* event)
{
    // Ctrl+Left drag = text selection for translation
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
    if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) {
        m_panning      = true;
        m_lastMousePos = event->pos();
        viewport()->setCursor(Qt::ClosedHandCursor);
        event->accept();
    } else {
        QAbstractScrollArea::mousePressEvent(event);
    }
}

void ContinuousView::mouseMoveEvent(QMouseEvent* event)
{
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
        QAbstractScrollArea::mouseMoveEvent(event);
    }
}

void ContinuousView::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_selecting && event->button() == Qt::LeftButton) {
        m_selecting = false;
        viewport()->setCursor(Qt::ArrowCursor);

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
            double left   = (cx0 - pageLeftX(foundPage)) / m_zoom;
            double right  = (cx1 - pageLeftX(foundPage)) / m_zoom;
            double top_s  = (cy0 - pageTopY(foundPage))  / m_zoom;
            double bot_s  = (cy1 - pageTopY(foundPage))  / m_zoom;

            // Convert to PDF coords (PDF origin is bottom-left)
            double pageH_pts = m_pageSizePt[foundPage].height();
            QRectF pageRect(left, pageH_pts - bot_s, right - left, bot_s - top_s);
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
