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
#include <memory>
#include "core/VectorLayer.h"
#include "core/VectorGpuRenderer.h"
#include "core/PdfLinks.h"
#include "core/TextSelection.h"
#include "PdfGpuView.h"

class PdfDocument;
class PdfRenderer;
struct AnnotVisual;

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

    // Cuon toi DUNG VI TRI cua ket qua tim kiem: tam hinh chu nhat vao GIUA
    // vung nhin, giu nguyen zoom. rectPdf o TOA DO HIEN THI (Y-down, goc trai
    // tren, da ap /Rotate va pageBoxOrigin) — cung khong gian voi highlight
    // (xem paintEvent). Dung lai dung phep quy doi paintEvent dang dung.
    void scrollToPageRect(int page, const QRectF& rectPdf);
    // Probe (nghiem thu bang so): vi tri tam cua rectPdf trong viewport SAU khi
    // da cuon — tra (1e9,1e9) neu page/rect khong hop le.
    QPointF probeRectCenterInViewport(int page, const QRectF& rectPdf) const;

    // Nhay link noi bo xong: ve vien khung dich ~1 giay roi mo dan
    // (SPEC_PDF_LINKS muc 4). rectDisp o toa do hien thi cua page.
    void flashPageRect(int page, const QRectF& rectDisp);

    // Page index whose vertical midpoint is closest to the viewport center.
    int currentPage() const;

    // Cong cu dang chon (SPEC_OCR_TAB_AND_SELECT phan 2). Mot nguon su that duy
    // nhat: dung chung enum PdfGpuView::ViewTool, khong chep enum ra cho nay.
    void setTool(PdfGpuView::ViewTool tool);
    PdfGpuView::ViewTool tool() const { return m_tool; }
    // Search highlights cho NHIEU trang cung luc. byPage: rect (toa do hien thi)
    // cua tung trang. currentPage/currentIdxInPage: ket qua dang chon (bo qua
    // bang -1 khi khong co ket qua dang chon). paintEvent chi ve cac trang trong
    // vung nhin.
    void setAllHighlights(const QHash<int, QList<QRectF>>& byPage,
                          int currentPage = -1, int currentIdxInPage = -1);
    void clearAllHighlights();
    // Vung chon chu (SPEC_TEXTSEL_ADOBE): rect toa do hien thi, push tu MainWindow.
    void setSelectionRects(const QHash<int, QList<QRectF>>& byPage);
    void clearSelectionRects();
    void invalidatePage(int pageIndex);
    void setAnnotVisualsForPage(int page, const QList<AnnotVisual>& visuals);

    // ── Chon/keo markup (SPEC_CONTINUOUS_MARKUP_EDIT_2026-08-16) ─────────────
    // Giong PdfGpuView: MainWindow goi setSelectedAnnot khi bam trung annot de
    // ve khung chon; Clear de bo. rectPdf o TOA DO HIEN THI (Y-down, da ap
    // /Rotate + pageBoxOrigin) — cung khong gian voi AnnotInfo.rect.
    void setSelectedAnnot(int page, const QRectF& rectPdf);
    void clearSelectedAnnot();
    // Drag-ghost: uid cua annot dang keo (MainWindow day tu annotationPickRequested).
    void setDragTarget(const QString& uid, const QString& ghostText,
                       float fontSizePt, const QColor& ghostColor);
    void clearDragTarget();
    void setDragNote(const QRectF& rPt);
    void clearDragState();
    // Probe-only (--contedit-test): giam lap nhan/keo/tha khong dung chuot that.
    void probeSimulatePickDrag(int page, const QPointF& pressDisp, const QPointF& dragDisp);


signals:
    // Emitted when the most-visible page changes while scrolling.
    void pageChanged(int pageIndex);
    // Emitted 150 ms after the last Ctrl+scroll zoom gesture.
    void zoomChanged(double scale);
    // Emitted when user finishes dragging a selection rect in text mode.
    // pageRectPts is in PDF-point coordinates (origin bottom-left per PDF spec).
    void textRegionSelected(int pageIndex, QRectF pageRectPts, QPoint globalPos);
    // Emitted 180 ms after the last scroll/zoom/resize to request a sharp-region
    // render of the visible viewport at full zoom (high zoom only).
    void regionNeeded(int pageIndex, double scale, QRect regionPx);
    void needAnnotVisuals(int page);
    // Chuot phai tren vung trang (SPEC_OCR_4 muc 2b): tim trang duoi con tro.
    void pageContextRequested(int pageIndex, QPoint globalPos);
    // Roi chuot qua link (SPEC_PDF_LINKS): chuoi rong = roi khoi link.
    void linkHovered(const QString& text);
    // Bam chuot trai vao link (chi khi tool la Pan/Select): tra ve link that.
    void linkActivated(int pageIndex, const PdfLink& link);
    // Chon chu theo chi so ky tu (SPEC_TEXTSEL_ADOBE phan 3): view bao vi tri
    // anchor/focus, MainWindow ghi vao DocTab::textSel va day rect ve lai.
    void textSelectionChanged(int anchorPage, int anchorChar,
                              int focusPage, int focusChar);
    void textSelectionCleared();
    // Markup pick/context/move (SPEC_CONTINUOUS_MARKUP_EDIT). Dung chung contract
    // voi PdfGpuView de MainWindow goi DUNG handler da co (undo + AnnotationManager).
    // pagePt o TOA DO HIEN THI (Y-down, da ap /Rotate + pageBoxOrigin).
    void annotationPickRequested(int pageIndex, QPointF pagePt);
    void annotationContextRequested(int pageIndex, QPointF pagePt, QPoint globalPos);
    void annotationMoveRequested(int pageIndex, double dx, double dy);

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
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
    PdfGpuView::ViewTool m_tool = PdfGpuView::ViewTool::Pan;

    // ── Drag-pan state ────────────────────────────────────────────────────────
    bool    m_panning      = false;
    QPoint  m_lastMousePos;

    // ── Chon/keo markup (SPEC_CONTINUOUS_MARKUP_EDIT_2026-08-16) ─────────────
    int     m_selPage = -1;        // trang so huu rect chon (display space)
    QRectF  m_selRect;             // toa do hien thi, Y-down
    bool    m_hasSel = false;
    bool    m_draggingAnnot = false;
    QPoint  m_dragStart;
    QRectF  m_dragOrigRect;
    QPointF m_dragPixelDelta;
    QString m_dragUid;
    QRectF  m_dragNoteRect;
    QPointF m_dragNoteOffsetPt;
    // Widget pos → trang + diem hien thi (Y-down, da ap /Rotate + pageBoxOrigin).
    // Giong PdfGpuView::widgetToPdf — MainWindow hit-test AnnotInfo.rect cung khong gian.
    bool resolvePageDisplayPos(const QPoint& widgetPos, int* page, QPointF* dispPt) const;

    // ── Text-selection state (Alt+drag) ──────────────────────────────────────
    bool    m_selecting    = false;
    QPoint  m_selStart;
    QPoint  m_selEnd;

    void drawSelection(QPainter& p);

    // ── Chon chu theo chi so ky tu (SPEC_TEXTSEL_ADOBE) ─────────────────────
    bool    m_selDragging   = false;
    int     m_selAnchorPage = -1;
    int     m_selAnchorChar = -1;
    int     m_selFocusPage  = -1;
    int     m_selFocusChar  = -1;
    QHash<int, QList<QRectF>> m_selRectsByPage;   // toa do hien thi de ve
    // Dem dblclick lien tiep (2=nhay dup, 3=nhay ba) theo doubleClickInterval.
    QElapsedTimer m_clickClock;
    bool m_clickValid = false;
    // Vung chon do nhay dup/ba tao ra: nha chuot phai GIU NGUYEN (khong mo rong).
    bool m_selClickGesture = false;
    // Tu cuon khi keo toi mep tren/duoi vung nhin.
    QTimer* m_autoScrollTimer = nullptr;
    QPoint  m_autoScrollMousePos;

    // Widget pos → trang + diem PDF user space (goi lai duong quy doi paintEvent).
    // load=false (mac dinh): chi doc dem PageCache — mouseMove khong duoc nap.
    // load=true: duoc phep nap (mousePress / bat dau chon chu).
    bool resolvePageSpacePos(const QPoint& widgetPos, int* page, QPointF* pagePt,
                             bool load = false) const;
    // Nhan chuot tai vi tri (clickCount: 1=normal, 2=word, 3=line).
    void beginTextSelection(const QPoint& widgetPos, int clickCount);
    void updateTextSelectionFocus(const QPoint& widgetPos);
    void finishTextSelection();
    void emitSelectionState();
    void clearTextSelectionInternal();
    void updateSelectCursor(const QPoint& widgetPos);
    void startAutoScrollIfNeeded(const QPoint& widgetPos);
    void stopAutoScroll();

    // ── Link (SPEC_PDF_LINKS) ──────────────────────────────────────────────
    // widgetPos la toa do viewport. Goi lai tu notifier khi link tinh xong
    // de cap nhat con tro ngay khong can re chuot.
    void updateLinkHover(const QPoint& widgetPos);
    void onLinksReady(quintptr doc, int pageIndex);
    bool tryActivateLink(QMouseEvent* event);
    bool m_hoveringLink = false;
    QPoint m_lastHoverPos;   // vi tri con tro lan mouseMove cuoi (de linksReady cap nhat)
    QTimer* m_flashTimer = nullptr;   // lap ve khi flash dang chay
    QElapsedTimer m_flashClock;
    int     m_flashPage = -1;
    QRectF  m_flashRect;

    // ── Signals ───────────────────────────────────────────────────────────────
    int     m_lastEmittedPage = -1;
    QTimer* m_zoomTimer = nullptr;     // 150 ms debounce for zoomChanged
    QTimer* m_scrollTimer = nullptr;   // debounce for pageChanged on scroll
    QTimer* m_sharpTimer = nullptr;    // 180 ms debounce for sharp-region request

    // Renderer signal connections (kept so we can disconnect on document change).
    QMetaObject::Connection m_continuousPageReadyConn;
    QMetaObject::Connection m_regionReadyConn;
    QMetaObject::Connection m_regionNeededConn;

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

    // ── Annotation overlay visuals (per page) ─────────────────────────────
    QHash<int, QList<AnnotVisual>> m_pageAnnotVisuals;

    // ── Search highlights (nhieu trang cung luc) ─────────────────────────
    QHash<int, QList<QRectF>> m_highlightsByPage;
    int  m_highlightCurrentPage = -1;  // trang chua ket qua dang chon
    int  m_highlightCurrentIdx  = -1;  // chi so trong list cua trang do

    QHash<int, std::shared_ptr<VectorLayer>> m_vecLayers;
    QSet<int>                                m_vecBuilding;
    void ensureVectorLayers();
    bool vectorWillRender(int pg) const;

    VectorGpuRenderer m_vgr;
    bool m_vgrInit = false;

};
