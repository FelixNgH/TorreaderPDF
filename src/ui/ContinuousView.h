#pragma once
#include <QAbstractScrollArea>
#include <QHash>
#include <QSet>
#include <QPixmap>
#include <QImage>
#include <QPointF>
#include <QRect>
#include <QTimer>
#include <QVector>
    #include <QElapsedTimer>

class PdfDocument;
class PdfRenderer;

// Continuous-scroll PDF viewer.
// All pages are laid out in a vertical strip with kGap pixels between them.
// Uses QAbstractScrollArea (NOT QScrollArea) and drives the scrollbars manually.
class ContinuousView : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit ContinuousView(QWidget* parent = nullptr);
    ~ContinuousView() override;

    // Attach a document + renderer. Pass nullptr to clear.
    void setDocument(PdfDocument* doc, PdfRenderer* renderer);
    void clearDocument();

    void setZoom(double scale);
    void setDarkMode(bool dark);

    // Animate-scroll the scrollbar to the top of page pageIndex.
    void scrollToPage(int pageIndex);

    // Page index whose vertical midpoint is closest to the viewport center.
    int currentPage() const;

    void setSelectedAnnotRect(const QRectF& rectPdf);
    void clearSelectedAnnotRect();
    void invalidatePage(int pageIndex);


signals:
    // Emitted when the most-visible page changes while scrolling.
    void pageChanged(int pageIndex);
    // Emitted 150 ms after the last Ctrl+scroll zoom gesture.
    void zoomChanged(double scale);
    // Emitted when user finishes dragging a selection rect in text mode.
    // pageRectPts is in PDF-point coordinates (origin bottom-left per PDF spec).
    void textRegionSelected(int pageIndex, QRectF pageRectPts, QPoint globalPos);

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    // Gap in pixels between successive pages.
    static constexpr int    kGap        = 12;
    // Horizontal padding added to the canvas width beyond the widest page.
    static constexpr int    kHPad       = 40;
    // Drop-shadow size in pixels.
    static constexpr int    kShadow     = 4;

    // ── Layout ────────────────────────────────────────────────────────────────
    // Recompute m_pageTopY, canvas size, and scrollbar ranges.
    void rebuildLayout();
    // Update scrollbar ranges from current canvas dimensions.
    void updateScrollBars();

    // ── Rendering helpers ─────────────────────────────────────────────────────
    // Return the page index most-visible in the viewport (used by currentPage).
    int pageAtCenter() const;
    // Request renders for visible pages ± 1 buffer page.
    void requestVisiblePages();
    // True if any part of page i overlaps the current scrolled viewport.
    bool pageVisible(int i) const;

    // ── Coordinate helpers ────────────────────────────────────────────────────
    // Canvas Y of the top edge of page i (before scrolling).
    int pageTopY(int i) const;
    // Canvas X of the left edge of page i (centered in canvas).
    int pageLeftX(int i) const;
    // Rendered pixel width of page i.
    int pageW(int i) const;
    // Rendered pixel height of page i.
    int pageH(int i) const;

    // ── Document state ────────────────────────────────────────────────────────
    PdfDocument*   m_doc      = nullptr;
    PdfRenderer*   m_renderer = nullptr;
    int            m_pageCount = 0;
    QVector<QSizeF> m_pageSizePt;   // size in PDF points for each page

    // ── Layout cache ──────────────────────────────────────────────────────────
    QVector<int>   m_pageTopY_cache;  // canvas Y of top of each page
    int            m_canvasW = 0;
    int            m_canvasH = 0;

    // ── Render cache ──────────────────────────────────────────────────────────
    QHash<int, QPixmap> m_pageImages; // keyed by page index
    QHash<int, double>  m_pageImageZoom; // zoom level when each image was rendered
    QSet<int>           m_continuousRequested; // pages currently being rendered

    // ── View state ────────────────────────────────────────────────────────────
    double  m_zoom     = 1.0;
    bool    m_darkMode = false;

    // ── Drag-pan state ────────────────────────────────────────────────────────
    bool    m_panning      = false;
    QPoint  m_lastMousePos;

    // ── Text-selection state (Alt+drag) ──────────────────────────────────────
    bool    m_selecting    = false;
    QPoint  m_selStart;
    QPoint  m_selEnd;

    void drawSelection(QPainter& p);

    // ── Signals ───────────────────────────────────────────────────────────────
    int     m_lastEmittedPage = -1;
    QTimer* m_zoomTimer = nullptr;     // 150 ms debounce for zoomChanged
    QTimer* m_scrollTimer = nullptr;   // debounce for pageChanged on scroll

    // Renderer signal connection (kept so we can disconnect on document change).
    QMetaObject::Connection m_continuousPageReadyConn;
    QMetaObject::Connection m_regionReadyConn;

    // Tracks which (page → zoom) pairs have already been probed from cache.
    // Prevents repeated synchronous disk reads when cached images don't match
    // current continuous zoom. Cleared on zoom change.
    QHash<int, double> m_cacheProbed;

    // ── Sharp-region overlay ─────────────────────────────────────────────────
    // Renders only the visible region at full zoom when the base page image is
    // clamped (kMaxPx limit), providing crisp text at high zoom levels.
    int     m_sharpPage    = -1;
    double  m_sharpScale   = 0.0;
    QRect   m_sharpRegion;
    QPixmap m_sharpPixmap;

    // ── Primary-page settle (single-mode pattern, Việc 1 2026-07-21) ─────────
    // Continuous tracks one "primary page" (the page with most visible area in
    // viewport) and treats it like single-mode: cache-first, then 400ms settle
    // timer, then render. Neighbors only after primary is done.
    QTimer* m_contSettleTimer = nullptr;
    int     m_primaryPage = -1;
    double  m_lastRequestZoom = -1.0;
    bool    m_primaryRequested = false;
    void requestNeighborPages();

    // ── Selection state ──────────────────────────────────────────────────────
    QRectF      m_selRect;
    bool        m_hasSel = false;

};
