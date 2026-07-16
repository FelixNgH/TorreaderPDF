#include "PdfView.h"
#include <QPainter>
#include <QPen>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QResizeEvent>

PdfView::PdfView(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    // Declare that paintEvent always redraws the entire widget, so Qt doesn't
    // try to preserve the background — eliminates all stale-pixel artifacts.
    setAttribute(Qt::WA_OpaquePaintEvent, true);

    m_zoomTimer = new QTimer(this);
    m_zoomTimer->setSingleShot(true);
    m_zoomTimer->setInterval(150);
    connect(m_zoomTimer, &QTimer::timeout, this, [this]{
        emit zoomChanged(m_zoom);
    });
}

// ── Page management ───────────────────────────────────────────────────────────

void PdfView::setPage(int pageIndex, const QImage& pageImage, QSizeF pageSizePt) {
    bool newPage = (pageIndex != m_pageIndex);
    m_pageIndex  = pageIndex;
    m_pageSizePt = pageSizePt;
    m_loading    = false;
    m_pixmap     = QPixmap::fromImage(pageImage);
    if (newPage) {
        m_panOffset = {};
        m_highlights.clear();
        if (m_viewMode == ViewMode::Single) {
            m_pixmap2      = {};
            m_pageSizePt2  = {};
            m_pageIndex2   = -1;
        }
    }
    update();
}

void PdfView::setHighlights(const QList<QRectF>& rects) {
    m_highlights = rects;
    update();
}

void PdfView::clearHighlights() {
    m_highlights.clear();
    update();
}

void PdfView::setAnnotOverlays(const QList<AnnotOverlay>& overlays) {
    m_annotOverlays = overlays;
    update();
}

void PdfView::clearAnnotOverlays() {
    m_annotOverlays.clear();
    update();
}

void PdfView::setSecondPage(int pageIndex, const QImage& pageImage, QSizeF pageSizePt) {
    m_pageIndex2  = pageIndex;
    m_pageSizePt2 = pageSizePt;
    m_pixmap2     = QPixmap::fromImage(pageImage);
    update();
}

void PdfView::updatePageImage(const QImage& pageImage) {
    m_loading = false;
    m_pixmap  = QPixmap::fromImage(pageImage);
    update();
}

void PdfView::beginLoading() {
    m_loading = true;
    update();
}

void PdfView::setZoom(double scale) {
    m_zoom = qBound(0.1, scale, 10.0);
    update();
}

void PdfView::setViewMode(ViewMode mode) {
    m_viewMode = mode;
    if (mode == ViewMode::Single) {
        m_pixmap2     = {};
        m_pageSizePt2 = {};
        m_pageIndex2  = -1;
    }
    update();
}

void PdfView::setDarkMode(bool dark) {
    m_darkMode = dark;
    update();
}

void PdfView::setTool(ViewTool tool) {
    m_tool = tool;
    setCursor(tool == ViewTool::PlaceNote ? Qt::CrossCursor : Qt::ArrowCursor);
}

// ── Coordinate helpers ────────────────────────────────────────────────────────

QPointF PdfView::pageOrigin() const {
    if (m_viewMode == ViewMode::Double && !m_pixmap2.isNull()) {
        // Left page: right-edge aligned to center
        double pageWpx = m_pageSizePt.width() * m_zoom;
        double pageHpx = m_pageSizePt.height() * m_zoom;
        static constexpr double kGap = 8.0;
        double x = width() / 2.0 - pageWpx - kGap / 2.0 + m_panOffset.x();
        double y = height() / 2.0 - pageHpx / 2.0 + m_panOffset.y();
        return {x, y};
    }
    double pageWpx = m_pageSizePt.width() * m_zoom;
    double pageHpx = m_pageSizePt.height() * m_zoom;
    return QPointF(width()  / 2.0 - pageWpx / 2.0,
                   height() / 2.0 - pageHpx / 2.0) + m_panOffset;
}

QPointF PdfView::page2Origin() const {
    double pageWpx  = m_pageSizePt.width()  * m_zoom;
    double pageWpx2 = m_pageSizePt2.width() * m_zoom;
    double pageHpx2 = m_pageSizePt2.height() * m_zoom;
    static constexpr double kGap = 8.0;
    double x = width() / 2.0 + kGap / 2.0 + m_panOffset.x();
    double y = height() / 2.0 - pageHpx2 / 2.0 + m_panOffset.y();
    Q_UNUSED(pageWpx);
    Q_UNUSED(pageWpx2);
    return {x, y};
}

QPointF PdfView::widgetToPdf(const QPointF& wp) const {
    return (wp - pageOrigin()) / m_zoom;
}

QPointF PdfView::pdfToWidget(const QPointF& pp) const {
    return pp * m_zoom + pageOrigin();
}

QRectF PdfView::pdfRectToWidget(const QRectF& r) const {
    return QRectF(pdfToWidget(r.topLeft()), pdfToWidget(r.bottomRight()));
}

// ── Paint ─────────────────────────────────────────────────────────────────────

void PdfView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setClipping(true);
    p.setClipRect(rect());
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    QColor bg = m_darkMode ? QColor(30, 30, 30) : QColor(80, 80, 80);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), bg);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    if (m_pixmap.isNull() && !m_loading) {
        p.setPen(QColor(200, 200, 200));
        QFont f = p.font(); f.setPointSize(13); p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter,
                   "TorReader PDF\n\nOpen a PDF to get started\n"
                   "File → Open   or   drag & drop");
        return;
    }

    auto drawPage = [&](const QPixmap& px, QPointF origin, QSizeF sizePt) {
        if (px.isNull() || sizePt.isEmpty()) return;
        double pw = sizePt.width()  * m_zoom;
        double ph = sizePt.height() * m_zoom;
        QRectF dst(origin, QSizeF(pw, ph));
        // Draw shadow BEFORE page so the page covers any overlap, preventing residual pixels.
        p.fillRect(dst.adjusted(4, 4, 4, 4), QColor(0, 0, 0, 80));
        p.drawPixmap(dst, px, px.rect());
    };

    if (!m_pixmap.isNull()) {
        drawPage(m_pixmap, pageOrigin(), m_pageSizePt);
        if (m_viewMode == ViewMode::Double && !m_pixmap2.isNull())
            drawPage(m_pixmap2, page2Origin(), m_pageSizePt2);
    }

    // Draw search / annotation highlights (PDF coords, Y upward → convert to widget)
    if (!m_highlights.isEmpty() && !m_pixmap.isNull() && !m_pageSizePt.isEmpty()) {
        p.save();
        p.setBrush(QColor(255, 220, 0, 120));
        p.setPen(QPen(QColor(255, 150, 0, 200), 1.5));
        QPointF origin = pageOrigin();
        double pageH = m_pageSizePt.height();
        for (const QRectF& r : m_highlights) {
            QRectF nr = r.normalized();
            if (nr.width() < 0.5 || nr.height() < 0.5) continue;
            // PDF: y grows up. nr.y()=bottom-Y, nr.bottom()=top-Y in PDF space.
            double wx = origin.x() + nr.x() * m_zoom;
            double wy = origin.y() + (pageH - nr.bottom()) * m_zoom;
            double ww = nr.width()  * m_zoom;
            double wh = nr.height() * m_zoom;
            p.drawRect(QRectF(wx, wy, ww, wh));
        }
        p.restore();
    }

    // Draw note annotation overlays (larger, more visible badges on top of the page).
    if (!m_annotOverlays.isEmpty() && !m_pixmap.isNull()) {
        QPointF origin = pageOrigin();
        double pageH = m_pageSizePt.height();
        p.save();
        for (const auto& ov : m_annotOverlays) {
            if (ov.pageIndex != m_pageIndex) continue;
            QRectF nr = ov.pdfRect.normalized();
            double cx = origin.x() + (nr.x() + nr.width()  / 2) * m_zoom;
            double cy = origin.y() + (pageH - nr.y() - nr.height() / 2) * m_zoom;
            // Draw 28x28 rounded badge (golden, semi-transparent).
            QRectF badge(cx - 14, cy - 14, 28, 28);
            p.setBrush(QColor(245, 158, 11, 220));
            p.setPen(QColor(180, 100, 0, 200));
            p.drawRoundedRect(badge, 6, 6);
            p.setPen(Qt::white);
            QFont f = p.font(); f.setPointSize(10); f.setBold(true); p.setFont(f);
            p.drawText(badge, Qt::AlignCenter, "N");
            // Snippet label below the badge.
            if (!ov.snippet.isEmpty()) {
                p.setPen(QColor(50, 50, 50));
                QFont sf = p.font(); sf.setPointSize(8); sf.setBold(false); p.setFont(sf);
                p.drawText(QRectF(cx - 60, cy + 16, 120, 20), Qt::AlignCenter, ov.snippet);
            }
        }
        p.restore();
    }

    if (m_loading) {
        p.fillRect(rect(), QColor(0, 0, 0, 60));
        p.setPen(Qt::white);
        QFont f = p.font(); f.setPointSize(12); p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter, "Loading…");
    }
}

// ── Input events ──────────────────────────────────────────────────────────────

void PdfView::wheelEvent(QWheelEvent* e) {
    if (e->modifiers() & Qt::ControlModifier) {
        double delta   = e->angleDelta().y() / 1200.0;
        double newZoom = qBound(0.1, m_zoom + delta, 10.0);
        if (qAbs(newZoom - m_zoom) < 1e-6) return;

        QPointF cursorPdf = widgetToPdf(e->position());
        m_zoom = newZoom;
        QPointF newWidget = pdfToWidget(cursorPdf);
        m_panOffset += e->position() - newWidget;

        update();
        m_zoomTimer->start();
    } else {
        double dy = e->angleDelta().y() * 0.6;
        m_panOffset.ry() += dy;

        double pageHpx = m_pageSizePt.height() * m_zoom;
        double margin  = (pageHpx > height())
            ? (pageHpx - height()) / 2.0 + height() * 0.05
            : pageHpx * 0.1;

        int step = (m_viewMode == ViewMode::Double) ? 2 : 1;

        if (m_panOffset.y() > margin) {
            m_panOffset.setY(0.0);
            emit scrolledToPage(m_pageIndex - step);
        } else if (m_panOffset.y() < -margin) {
            m_panOffset.setY(0.0);
            emit scrolledToPage(m_pageIndex + step);
        }
        update();
    }
}

void PdfView::mousePressEvent(QMouseEvent* e) {
    if (m_tool == ViewTool::PlaceNote && e->button() == Qt::LeftButton) {
        if (!m_pixmap.isNull()) {
            // In double mode: determine which page was clicked
            if (m_viewMode == ViewMode::Double && !m_pixmap2.isNull()) {
                QPointF po2 = page2Origin();
                if (e->position().x() >= po2.x()) {
                    // Clicked on right page
                    QPointF pdfPt = (e->position() - po2) / m_zoom;
                    emit noteRequested(m_pageIndex2, pdfPt);
                    return;
                }
            }
            emit noteRequested(m_pageIndex, widgetToPdf(e->position()));
        }
        return;
    }
    if (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton) {
        m_panning      = true;
        m_lastMousePos = e->position();
        setCursor(Qt::ClosedHandCursor);
    }
}

void PdfView::mouseMoveEvent(QMouseEvent* e) {
    if (m_panning) {
        m_panOffset   += e->position() - m_lastMousePos;
        m_lastMousePos = e->position();
        update();
    }
}

void PdfView::mouseReleaseEvent(QMouseEvent*) {
    m_panning = false;
    setCursor(m_tool == ViewTool::PlaceNote ? Qt::CrossCursor : Qt::ArrowCursor);
}

void PdfView::resizeEvent(QResizeEvent*) { update(); }
