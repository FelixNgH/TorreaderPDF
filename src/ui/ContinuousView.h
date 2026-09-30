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
#include <QThreadPool>
#include <atomic>
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
    // LÁT A 09/02: `visualsSrc` là KHO CHÚ THÍCH DUY NHẤT theo tài liệu
    // (DocTab::visualsCache) — view ĐỌC, không sở hữu, không tự xoá.
    // LÁT B 09/02: `vecStore` là KHO LỚP VECTOR DUY NHẤT theo tài liệu
    // (DocTab::vecLayers) — view đọc/ghi qua con trỏ, KHÔNG sở hữu, KHÔNG tự xoá.
    void setDocument(PdfDocument* doc, PdfRenderer* renderer,
                     const QHash<int, QList<AnnotVisual>>* visualsSrc = nullptr,
                     QHash<int, std::shared_ptr<VectorLayer>>* vecStore = nullptr);
    void clearDocument();
    // 🔴 0928 LƯỢT 22 — MainWindow cần biết view còn ĐANG CẦM renderer của tab
    // sắp đóng không (đóng tab hiện hành lúc ở Single thì onTabChanged không
    // setDocument lại ⇒ con trỏ treo vào object closeJob sẽ delete).
    bool isUsingRenderer(const PdfRenderer* r) const { return m_renderer == r && r != nullptr; }
    // LÁT B 09/02: repoint kho lớp vector (DocTab::vecLayers cua tab hien hanh). Goi moi
    // lan doi/dong tab de con tro khong bao gio tro vao tai lieu da giai phong. Null -> ve
    // kho trong cua rieng view (khong bao gio giu du lieu that).
    void setVectorStore(QHash<int, std::shared_ptr<VectorLayer>>* store) {
        m_vecStore = store ? store : &m_vecStoreEmpty;
    }

    // Khoa cache .torvec, MainWindow day sang (ContinuousView khong tu biet duong dan goc).
    void setVectorCacheKey(const QString& keyPath, quint64 keyHash);

    void setZoom(double scale);
    double zoom() const { return m_zoom; }
    void setDarkMode(bool dark);

    // Animate-scroll the scrollbar to the top of page pageIndex.
    void scrollToPage(int pageIndex, const char* via = "?"); // 🔴 33f muc 5: `via` do AI gọi

    // 🔴 LƯỢT 33b (L): vi tri cuon THAT cua view de luu/khoi phuc theo tab.
    // scrollY() = gia tri verticalScrollBar hien tai. restoreScrollY(y) dat lai
    // dung offset (clamp vao range) ROI xin anh cac trang dang hien + ve ngay —
    // khong de vung xem TRANG khi quay lai tab.
    int  scrollY() const;
    void restoreScrollY(int y);

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
    void setVectorAnnotSafe(int page, bool safe);
    // 🔴🔴 2026-09-01: sau khi bo anh cu vi chu thich doi, PHAI dat lai lenh ve.
    // ⚠️ KHONG dung `requestVisiblePages()` — toan bo than ham do nam trong `if (primaryChanged)`,
    //   nen khi van dung o trang cu (dung canh vua tao chu thich) no KHONG LAM GI CA.
    //   Do duoc: invalidatePage chay 6 lan ma khong mot lenh render nao duoc dat.
    // ⇒ Dat lenh THANG cho dung trang do.
    void datLaiLenhVeTrang(int page);
    void setAnnotVisualsForPage(int page, const QList<AnnotVisual>& visuals);
    // LÁT A 09/02: repoint kho doc duoc duy nhat (DocTab::visualsCache cua tab hien hanh).
    // Goi moi lan doi/dong tab de con tro khong bao gio tro vao tai lieu da giai phong.
    void setVisualsStore(const QHash<int, QList<AnnotVisual>>* src) { m_visualsSrc = src; }
    void setPageLowRes(int pageIndex, const QImage& img);
    // 🔴 LƯỢT 36 (mục 2): toàn bộ việc làm khi continuousPageReady tới — gom từ 2 lambda
    // trùng nhau; phần thu nhỏ ảnh quá cỡ chạy ở nền, UI chỉ ghi bookkeeping.
    void acceptContinuousImage(int idx, QImage img, double renderedScale);
    // Bai do bo nho: dung luong 2 kho anh cua Continuous
    qint64 bytesPageImages() const {
        qint64 t=0; for (auto it=m_pageImages.constBegin(); it!=m_pageImages.constEnd(); ++it)
            t += qint64(it.value().width())*it.value().height()*4; return t; }
    qint64 bytesPageLowRes() const {
        qint64 t=0; for (auto it=m_pageLowRes.constBegin(); it!=m_pageLowRes.constEnd(); ++it)
            t += qint64(it.value().width())*it.value().height()*4; return t; }
    int countPageImages() const { return m_pageImages.size(); }
    int countPageLowRes() const { return m_pageLowRes.size(); }
    // Probe-only (--owner3tab-probe LƯỢT 17): trang p da co anh raster trong chinh
    // kho ma "[perf] cont ACCEPT continuousPageReady" ghi vao. CHI-DOC.
    bool hasPageImage(int p) const { return !m_pageImages.value(p).isNull(); }
    // Probe-only (LƯỢT 31b): scale THAT cua anh trang p (m_pageImageZoom), -1 khi chua co.
    double pageImageScale(int p) const { return m_pageImageZoom.value(p, -1.0); }
    // Probe-only (LƯỢT 37 bước O): co thu nhỏ nền đang chạy? / bao nhiêu lần guard bỏ bản cũ?
    int    probeDownscaleInFlight() const { return m_contDownscaleInFlight; }
    qint64 probeStaleDrops() const { return m_contStaleDrops; }
    // Trang co markup khong overlay duoc (SPEC_CONT_MARKUP_TAB_SAFE muc 1): cam dung
    // lop vector cho trang nay, markup hien qua duong raster (setPageAnnotRender).
    // Chi dat/xoa co — KHONG go lop vector dang dung (GPU renderer con giu uid), de
    // co che EVICT san co don. setDocument() xoa sach co nay.
    void setPageRasterOnly(int page, bool on);

    // ── Lop bu annot phan mem khac (lam giong Single/PdfGpuView) ─────────────
    // MainWindow day lop bu da build (ForeignAnnotLayer) xuong theo tung trang.
    // ContinuousView ve lop nay DE LEN lop vector ngay sau endNativePainting.
    // Giu TOI DA 3 trang, evict theo khoang cach toi trang chinh (giong m_vecStore).
    // Truyen nullptr de xoa lop cua trang.
    void setForeignAnnotLayer(int page, std::shared_ptr<ForeignAnnotLayer> layer);
    // Lop bu cua DUNG trang nay (de MainWindow goi buildRegion tren chinh doi tuong dang ve).
    std::shared_ptr<ForeignAnnotLayer> foreignAnnotLayer(int page) const {
        auto it = m_fgnLayers.constFind(page);
        return (it != m_fgnLayers.constEnd()) ? *it : nullptr;
    }
    // VUNG SAC NET cua lop bu theo dung zoom hien tai — tuong duong PdfGpuView::setForeignAnnotRegion.
    // MainWindow goi khi buildRegion() xong (img rong = that bai, chi mo chot chong-chong-viec).
    void setForeignAnnotRegion(int page, double scale, QRect regionPx, const QImage& img);
    // Trang da co noi dung de ve (lop vector san sang HOAC da co anh raster)?
    // C3: lop bu chi dung SAU KHI trang hien ra roi, tranh chan hien thi trang nang.
    bool pageHasContent(int page) const;
    // Trang DANG ve bang lop vector (m_vecStore san san + not rasterOnly)? C3 / 30-08:
    // lop bu chi can khi NEN la vector (lop nay khong chua annotation). Nen raster da
    // nung san annotation nen khong can lop bu.
    bool pageHasVector(int page) const;

    // ── Chon/keo markup (SPEC_CONTINUOUS_MARKUP_EDIT_2026-08-16) ─────────────
    // Giong PdfGpuView: MainWindow goi setSelectedAnnot khi bam trung annot de
    // ve khung chon; Clear de bo. rectPdf o TOA DO HIEN THI (Y-down, da ap
    // /Rotate + pageBoxOrigin) — cung khong gian voi AnnotInfo.rect.
    void setSelectedAnnot(int page, const QRectF& rectPdf);
    void clearSelectedAnnot();
    // Insert Image (SPEC_INSERT_IMAGE_2026-08-30): cho phep co gian bang 4 tay nam
    // goc (chi Stamp cua TorReader). Goi tu MainWindow.
    void setSelectResizable(bool on);
    bool selectResizable() const { return m_selResizable; }
    // Insert Image (SPEC_INSERT_IMAGE_2026-08-30): trang + diem giua vung nhin
    // (toa do hien thi) de dat anh moi. Tra false neu khong co trang hop le.
    bool probeViewportCenter(int* page, QPointF* dispCenter) const;
    QSizeF pageSizePt(int page) const { return (page >= 0 && page < m_pageSizePt.size()) ? m_pageSizePt[page] : QSizeF(); }
    // Drag-ghost: uid cua annot dang keo (MainWindow day tu annotationPickRequested).
    void setDragTarget(const QString& uid, const QString& ghostText,
                       float fontSizePt, const QColor& ghostColor);
    void clearDragTarget();
    void setDragNote(const QRectF& rPt);
    void clearDragState();
    // Probe-only (--contedit-test): giam lap nhan/keo/tha khong dung chuot that.
    void probeSimulatePickDrag(int page, const QPointF& pressDisp, const QPointF& dragDisp);
    // Probe-only (--zoomanchor-probe 2026-09-21): dat con tro tai (fx,fy) trong trang,
    // ban mot su kien Ctrl+wheel THAT, do toa do TAI LIEU duoi con tro truoc/sau.
    // mode=0: diem trong trang; mode=1: con tro trong KHE 12px duoi trang (LOI 1).
    // offY: lech con tro khoi tam khung nhin (px) de lo fallback neo tam.
    // Tra ve bao cao nhieu dong (lech MOI vs cong thuc CU vs fallback CU).
    QString probeZoomAnchor(int page, double zoomFrom, double zoomTo, double fx, double fy,
                            int mode = 0, double offY = 0.0);
    // Probe-only (--scrollpersist-probe 2026-09-21): chung minh LOI 2 + LOI B da va —
    // updateScrollBars (duong resizeEvent goi) KHONG con ep ve giua moi lan, NHUNG khi
    // vpW DOI (dong/mo sidebar, doi co cua so) thi: chua keo => canh giua lai; da keo
    // lech tam => giu nguyen vi tri nguoi dung. probeRealWindow=true: chay them duong
    // CUA SO THAT (show(), co thanh cuon) de DO luong LOI A da chap nhan. Tra ve bao cao.
    QString probeScrollPersist(double zoom, bool probeRealWindow = false);


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
    // Insert Image (SPEC_INSERT_IMAGE_2026-08-30): ket thuc keo tay nam goc.
    void annotationResizeRequested(int pageIndex, QRectF newRectDisp);
    // VIỆC 3 (SPEC_SMOOTH_123 31/08): trang visible chua co anh to/lop vector → xin
    // thumbnail (bac 0) de KHONG BAO GIO ve o trong. MainWindow noi toi ThumbnailRenderPool.
    void needThumbnail(int page);
    // Vung sac net cho CHU THICH NGOAI (lop bu) cua trang trung tam dang nhin, theo zoom that.
    // Truoc 2026-09-21 Continuous KHONG co duong nay: lop bu toan trang dung MOT lan o zoom nho
    // (maxPx=900/1103) roi bi keo gian khi zoom len => chu thich doi tac NHOE. MainWindow noi
    // tin hieu nay vao CUNG duong ForeignAnnotLayer::buildRegion dang dung cho PdfGpuView.
    void foreignAnnotRegionNeeded(int page, double scale, QRect regionPx);

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
    // 0928 LƯỢT 18 (VIỆC 2): ghi/xoa g_pdfiumUrgentPage — cho thumbnail biet trang
    // primary cua tab dang xem dang cho anh thi no khong duoc tranh khoa.
    void syncUrgentPrimary();
// Gap in pixels between successive pages.
     static constexpr int    kGap        = 12;
     // Horizontal padding added to the canvas width beyond the widest page.
     static constexpr int    kHPad       = 40;
     // Drop-shadow size in pixels.
     static constexpr int    kShadow     = 4;
     // Vector build grace period (ms) to allow raster fallback after vector build starts.
     static constexpr qint64 kVectorGraceMs = 400;
     // Radius (in pages) around center to keep vector layers/builds active.
     static constexpr int    kKeepRadius    = 1;
      // Trang nang phai DUNG MOI vai giay moi hien lai — noi long ban kinh giu lop
      // vector (Viec C 2026-08-30): cuon di 2-3 trang roi quay lai khong phai xay lai.
      static constexpr int    kHeavyKeepRadius = 3;
      // 🔴 SUA 02/09/2026 — tran so lop bu foreign giu dong thoi. Moi lop la mot anh
      // ARGB32 ca trang (long side ≤ 4000px ⇒ toi da ~45 MB/lop, thuong 10-25 MB).
      // 6 lop ≈ toi da ~270 MB. Khong bao gio giu vo han (bai hoc RAM 6,2 GB).
      static constexpr int    kFgnMaxLayers = 6;
     // Object-count threshold above which a page is "heavy": raster can never finish
     // (2,5 trieu path), so rasterOnly is bypassed and the vector layer stays allowed.
     static constexpr int    kHeavyObjectThreshold = 400000;

    // ── Layout ────────────────────────────────────────────────────────────────
    // Recompute m_pageTopY, canvas size, and scrollbar ranges.
    void rebuildLayout();
    // Update scrollbar ranges from current canvas dimensions.
    void updateScrollBars();
    // Doi zoom sao cho DIEM TAI LIEU duoi `anchorVp` (toa do khung nhin) dung yen.
    // Dung chung cho Ctrl+wheel (anchor = con tro) va setZoom (anchor = tam khung nhin).
    // 🔴 KHONG neo bang ti le pixel `canvas * (newZoom/m_zoom)`: bo cuc co kGap/kHPad
    // hang so + can giua (pageLeftX = m_canvasW/2 - pageW/2), khong no theo zoom.
    void zoomAnchored(double newZoom, const QPointF& anchorVp);

    // ── Rendering helpers ─────────────────────────────────────────────────────
    // Return the page index most-visible in the viewport (used by currentPage).
    int pageAtCenter() const;
    // Request renders for visible pages ± 1 buffer page.
    void requestVisiblePages();
    // True if any part of page i overlaps the current scrolled viewport.
    bool pageVisible(int i) const;
    // Request a continuous render for page at scale, but skip if an identical
    // (page, scale) render is already in flight (m_pendingRenderScale). If a
    // render for the page is pending at a DIFFERENT scale, cancel it first
    // instead of stacking a duplicate. All continuous render requests must
    // route through here (settle timer, retry timer, neighbor pages).
    void requestContinuousRender(int page, double scale);

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
    // LOI 2 (0921): hRange lan truoc. updateScrollBars CHI canh giua khi range vua
    // tro nen lot-trong (m_lastHRange >= 0); lan resize sau do giu vi tri nguoi dung.
    int            m_lastHRange = 0;

    // ── Render cache ──────────────────────────────────────────────────────────
    QHash<int, QPixmap> m_pageImages; // keyed by page index
    QHash<int, QPixmap> m_pageLowRes; // low-res placeholders for fast scroll
    QHash<int, quint64> m_contStoreGen; // LƯỢT 36: gen mỗi lần nhận ảnh — kết quả thu nhỏ cũ bị bỏ
    // LƯỢT 37 (reviewer lỗi 1): m_contStoreGen chỉ chặn "ảnh MỚI hơn cùng trang", KHÔNG chặn
    // "tài liệu đã đổi". Đổi tab/đóng tab/setDocument không đụng m_contStoreGen ⇒ lambda thu nhỏ
    // nền của tài liệu cũ vẫn khớp gen và ghi ảnh CŨ vào kho của tài liệu MỚI (kèm nhãn
    // m_pageImageZoom giả ⇒ settle timer không vẽ lại ⇒ trang sai vĩnh viễn). docGen tăng ở MỌI
    // setDocument/clearDocument ⇒ định danh tài liệu gắn vào gen. Chỉ-tăng, không reset.
    quint64 m_contDocGen = 0;
    // LƯỢT 37: bằng chứng cho probe bước O — đếm thu nhỏ đang chạy + số lần guard bỏ bản cũ.
    int   m_contDownscaleInFlight = 0;
    qint64 m_contStaleDrops = 0;
    QHash<int, double>  m_pageImageZoom; // zoom level when each image was rendered
    QSet<int>           m_continuousRequested; // pages currently being rendered
    // Yeu cau render dang bay theo cap (page, scale): lan chong bom viec trung —
    // trang 2,54 trieu path render lau, setDocument/scroll retry hoi lai lien tuc
    // cung (page, scale) lam render CU chua xong da bi da di lam lai tu dau.
    // Xoa khi anh ve xong (continuousPageReady) va khi doi tai lieu/reset zoom.
    QHash<int, double>  m_pendingRenderScale;

// ── View state ────────────────────────────────────────────────────────────
     double  m_zoom     = 1.0;
     // Dang zoom: cam moi yeu cau anh (requestVisiblePages/requestNeighborPages) de
     // khong doc cache dia DONG BO o luong giao dien giua setZoom. Chi huy sau khi
     // zoom dung (cuoi setZoom) — anh cu van duoc ve scale tam trong paintEvent.
     bool    m_zooming  = false;
     bool    m_darkMode = false;
     bool    m_fastMode = true;   // default to fast mode
     PdfGpuView::ViewTool m_tool = PdfGpuView::ViewTool::Pan;

    // ── Drag-pan state ────────────────────────────────────────────────────────
    bool    m_panning      = false;
    // 🔴 0928 LƯỢT 13: đọc TỪ luồng worker (vét huỷ trong VectorLayer::build) nên
    // phải là atomic — nếu không thì đây là data race mới, đúng thứ mà LƯỢT 12
    // dựng khoá chung để diệt. GUI thread ghi, worker chỉ đọc.
    std::atomic<int> m_vecGen{0};
    bool    m_pickCandidate = false;
    int     m_pickPage = -1;
    QPointF m_pickPt;
    QPoint  m_panPressPos;
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
    // Insert Image: co gian Stamp bang 4 tay nam goc (giu ty le, Shift = tu do).
    bool    m_selResizable   = false;
    int     m_resizingCorner = -1;   // 0=TL,1=TR,2=BR,3=BL
    QRectF  m_resizeOrigRect;
    QPointF selHandlePos(int corner) const;
    int     handleAt(const QPointF& dispPt) const;
    QRectF  resizeRectFor(int corner, const QPointF& dragDisp, bool keepAspect) const;
    // Widget pos (viewport) -> toa do hien thi cua m_selPage.
    QPointF selPageWidgetToDisp(const QPoint& widgetPos) const;
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

    // Retry timer for primary page when rendering is skipped due to maxConcurrent limit.
    QTimer* m_retryTimer = nullptr;

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
    // 🔴 2026-08-31: cong cho phep DUNG lop vector. Mac dinh TAT.
    // Chi bat khi (a) cuon da dung han va (b) trang do DA co thu gi day du de nhin.
    bool m_vecBuildArmed = false;
    // 🔴 LƯỢT 33d (J): true khi setDocument gan TAI LIEU MOI (khong phai nhanh giu
    // anh cu) — lan cache-miss dau tien khong doi settle 400 ms, ve ngay.
    bool m_freshDocument = false;
    QHash<int, bool> m_vecAnnotSafe;   // trang -> nen vector co nuot markup khong
    int     m_primaryPage = -1;
    double  m_lastRequestZoom = -1.0;
    bool    m_primaryRequested = false;
    void requestNeighborPages();

    // Vector layer build timer for primary page (700ms debounce, single-shot)
    QTimer* m_vecBuildTimer = nullptr;
    void buildPrimaryVectorLayer();

    // ── Annotation overlay visuals (LÁT A 09/02) ──────────────────────────────
    // Con trỏ KHÔNG SỞ HỮU tới kho duy nhất theo tài liệu (DocTab::visualsCache).
    // Hai view (Single + Continuous) cùng đọc một kho ⇒ không còn lệch bản chép.
    const QHash<int, QList<AnnotVisual>>* m_visualsSrc = nullptr;

    // Cache trang xoay (de khong phai khoa PDFium o main thread).
    QHash<int, int> m_pageRot;

    // ── Search highlights (nhieu trang cung luc) ─────────────────────────
    QHash<int, QList<QRectF>> m_highlightsByPage;
    int  m_highlightCurrentPage = -1;  // trang chua ket qua dang chon
    int  m_highlightCurrentIdx  = -1;  // chi so trong list cua trang do

     // LÁT B 09/02: CON TRỎ KHÔNG SỞ HỮU tới kho lớp vector duy nhất theo tài liệu
     // (DocTab::vecLayers). View DÙNG CHUNG giữa các tab -> repoint khi đổi tab. Không
     // bao giờ clear() qua con trỏ này (sẽ xoá dữ liệu của tab). m_vecStoreEmpty là kho
     // TRONG cua riêng view, chi dung khi chua gan tab nao (khong giu du lieu that), de
     // cac duong ve khong bao gio null-deref.
     QHash<int, std::shared_ptr<VectorLayer>>  m_vecStoreEmpty;
     QHash<int, std::shared_ptr<VectorLayer>>* m_vecStore = &m_vecStoreEmpty;
      // Lop bu foreign annot theo trang (C1): moi lop la mot ANH LON, co tran cung
      // kFgnMaxLayers va vung dem quanh tam nhin that (khong dung ban kinh tam).
      QHash<int, std::shared_ptr<ForeignAnnotLayer>> m_fgnLayers;
      // Evict theo tam nhin that (02/09/2026): khong duoi trang dang hien thi, giu
      // vung dem 1 viewport, duoi xa-nhat-truoc den khi ≤ tran. (Loi goi chung tu
      // setForeignAnnotLayer va ensureVectorLayers).
     void evictForeignLayers();
     // ── Vung sac net chu thich ngoai (2026-09-21) ─────────────────────────────
     // Vung anh TRONG SUOT do MainWindow (ForeignAnnotLayer::buildRegion) tra ve, ve DE LEN
     // lop bu tho cua CA MOT trang. Chi trang TRUNG TAM duoc lam net (gioi han 1 trang/luot).
     void requestForeignAnnotRegion();   // tinh vung nhin trang trung tam + phat tin hieu
     void clearForeignAnnotRegion();     // chot 4: doi lop tho / doi zoom => xoa vung cu
     int    m_fgnRegPage  = -1;
     double m_fgnRegScale = 0.0;
     QRect  m_fgnRegRect;
     QImage m_fgnRegImg;
     // Chot 3 chong-chong-viec: mot lan buildRegion bay tai mot thoi diem. Mo trong
     // setForeignAnnotRegion() (ke ca khi that bai) — thieu chot nay la cac lan render de nhau.
     bool   m_fgnRegionBuilding = false;
     // Da thu o dung (page, zoom) ma that bai (huy/alloc) => khong xep lai vo han.
     int    m_fgnRegFailPage  = -1;
     double m_fgnRegFailScale = 0.0;
     QSet<int>                                m_vecBuilding;
     QSet<int>                                m_vecTriedNoCache;
     // 2026-09-20: `center` cua lan ensureVectorLayers truoc. Doi trang trung tam thi
     // xoa dau `m_vecTriedNoCache` — dieu kien spec muc 1 ("doi trang trung tam") bi bo quen
     // o ban va truoc ⇒ trang dang xem bi danh dau luc no chi la lan can thi khong bao gio
     // duoc xep lich lai nua. Xem ensureVectorLayers().
     int                                      m_vecTriedCenter = -1;
     QThreadPool                              m_vecPool;
     QHash<int, qint64>                       m_vecBuildStart;
     QSet<int>                                m_rasterOnlyPages;   // trang chi ve bang raster (spec cont2)
     QSet<int>                                m_thumbRequested;    // trang da yeu cau thumbnail (BAC 2)
     void ensureVectorLayers();
     bool vectorWillRender(int pg) const;
     // rasterOnly co duoc ap cho trang nay khong? Trang nang (>= nguong) KHONG bi cam
     // lop vector: raster 2,54 trieu path khong bao gio ve xong, phai dung lop vector
     // tu cache de hien trang (owner chot 2026-08-30).
     // 🔴 2026-09-01: trang co chu thich ma overlay KHONG ve (Note, Text, comment ngoai) thi
    // KHONG duoc ve bang nen vector — nen vector khong chua annotation. Tu quyet tai cho,
    // khong phu thuoc MainWindow co goi setVectorAnnotSafe dung luc hay khong.
    bool trangCanRaster(int pg) const;
    bool rasterOnlyApplies(int pg) const;
    // CAU TRUC — co nen dung/xdp lop vector cho trang nay khong? CHI chan khi env raster
    // thuan hoac trang nang bi ep raster. KHONG nhuc den chu thich: lop vector la NEN,
    // phai dung duoc cho ca trang co comment (comment do lop khac ve de len). Dung cho
    // ensureVectorLayers (1220) va pageHasVector (1521) — de trang comment khong ket raster.
    bool vectorBuildBlocked(int pg) const;
     static bool contRasterOnlyEnv();   // TORREADER_CONT_RASTER=1 -> Continuous raster thuan

VectorGpuRenderer m_vgr;
    bool m_vgrInit = false;

// Khoa cache .torvec, MainWindow day sang (ContinuousView khong tu biet duong dan goc).
     QString m_vecKeyPath;
     quint64 m_vecKeyHash = 0;
     // Doc so huu key hien tai. Doi doc thi key cu PHAI bo — khong duoc dung cache
     // cua file A cho trang cua file B (bao mat noi dung).
     PdfDocument* m_vecKeyDoc = nullptr;
     // Moi duong vao Continuous deu phai co key hop le. setDocument tu dan xuat key
     // tu doc khi MainWindow quen day (duong doi tab) hoac day hash=0 (initWatcher
     // chua tinh xong) — day la nguyen nhan [torvec] SKIP reason=nokey.
     void updateVecCacheKeyFromDoc(PdfDocument* doc);
};
