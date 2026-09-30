#include "ContinuousView.h"
#include "LoadingBadge.h"
#include "ThemeTokens.h"
#include "../core/PdfDocument.h"
#include "../core/PdfRenderer.h"
#include "../core/PdfiumLock.h"
#include "../core/DocTaskGate.h"
#include "../core/PdfCoords.h"
#include "../core/VectorCacheFile.h"
#include "../core/TileCacheFile.h"
#include <QFile>
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
#include <QThreadPool>
#include <QThread>
#include <cmath>
#include <algorithm>
#include <utility>
#include <QOpenGLFunctions>
#include <QElapsedTimer>
#include <QScopeGuard>

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
    // 350 ms thay vi 150: moi nac zoom lam huy va render lai CA TRANG (ban ve A1 ~43 MB),
    // 150 ms qua ngan nen zoom lien tiep la render chong nhau => giat. Cho cu chi zoom
    // dung han roi moi render mot lan duy nhat.
    m_zoomTimer->setInterval(350);
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
 connect(m_scrollTimer, &QTimer::timeout, this, [this] {    { QElapsedTimer _e; _e.start(); 
             { QElapsedTimer _e; _e.start(); 
                     { QElapsedTimer _e; _e.start(); 
                         
                                  int p = pageAtCenter();
                                  if (p != m_lastEmittedPage) {
                                      m_lastEmittedPage = p;
                                      emit pageChanged(p);
                                  }
                                  ensureVectorLayers();
                                  if (m_vecBuildTimer) m_vecBuildTimer->start();
                              
                      const qint64 _m=_e.elapsed(); if(_m>20) qDebug().noquote()<<"[scrollms] scrollTimer ms="<<_m;
                      }
              const qint64 _m=_e.elapsed(); if(_m>20) qDebug().noquote()<<"[scrollms] scrollTimer ms="<<_m;
              }
      const qint64 _m=_e.elapsed(); if(_m>20) qDebug().noquote()<<"[scrollms] scrollTimer ms="<<_m;
      }});

    // Vector layer build timer for primary page (700ms debounce, single-shot)
    m_vecBuildTimer = new QTimer(this);
    m_vecBuildTimer->setSingleShot(true);
    m_vecBuildTimer->setInterval(700);
    connect(m_vecBuildTimer, &QTimer::timeout, this, [this] { buildPrimaryVectorLayer(); });

    // Sharp-region overlay is no longer needed (full-page renders at zoom resolution)

    // Settle timer for primary page (same pattern as single-mode: 400ms delay
    // before starting a new render, so fast scrolling doesn't trigger renders).
    m_contSettleTimer = new QTimer(this);
    m_contSettleTimer->setSingleShot(true);
    m_contSettleTimer->setInterval(400);
    connect(m_contSettleTimer, &QTimer::timeout, this, [this]() {
        m_contSettleTimer->setInterval(400);   // 🔴 33d: lan "doc moi" co the chay
                                               // voi 1 ms — khuyen ve mac dinh.
        // 🔴 2026-08-31: cuon da DUNG => tu gio moi cho phep dung lop vector.
        m_vecBuildArmed = true;
        m_vecTriedNoCache.clear();
        // Doi lai visuals cho moi trang dang nhin thay. Sau mot lan zoom, moi viec dang bay
        // bi huy nen khong con su kien nao keo markup ve — day la luoi an toan cho no.
        for (int i = 0; i < m_pageCount; ++i)
            if (pageVisible(i)) needAnnotVisuals(i);
        // 🔴 2026-09-20: GOI LAI o day — truoc day settle chi bat m_vecBuildArmed va clear
        // m_vecTriedNoCache, nhung KHONG ai goi lai ensureVectorLayers() ⇒ cuon toi trang nang
        // chua co .torvec roi DUNG thi anh van nhoe, phai cuon them mot cai nua moi net (chi
        // m_scrollTimer moi kich hoat lai). Phai co mat tren MOI duong ra khoi lambda (2 nhanh
        // `return` som ben duoi + duong chay het).
        // 🔴 2026-09-21: DAT SAU quyet dinh raster. Goi TRUC TIEP o day ghi m_vecBuildStart
        // DONG BO truoc khi vectorWillRender(m_primaryPage) (dong ben duoi) doc no ⇒ ham do thay
        // "lop vector dang dung" (con trong kVectorGraceMs) ⇒ tra true ⇒ BO requestContinuousRender.
        // Voi trang co comment ngoai ma lop vector KHONG duoc dung (rasterOnlyApplies chan),
        // trang nam mai o thumbnail mo va khong ai xin lai raster (settle la timer MOT LAN).
        // ⇒ Dung qScopeGuard: no chay khi lambda THOAT (moi nhanh return + duong chay het),
        //    tuc SAU khi raster da quyet ⇒ vectorWillRender van doc m_vecBuildStart o trang thai CU.
        // An toan: ensureVectorLayers() la ham dong bo khong cham m_contSettleTimer, va da co
        // chot m_vecBuilding/m_vecStore chan xep lich trung.
        const auto veLai = qScopeGuard([this] { ensureVectorLayers(); });
        if (m_primaryPage < 0 || !m_renderer) {
            qDebug() << "[perf] cont settle SKIP reason=noPrimaryPage";
            return;
        }
        // Skip re-render if we already have a full-quality image (tran = fullQCapPx).
        auto zit = m_pageImageZoom.constFind(m_primaryPage);
        if (zit != m_pageImageZoom.constEnd()) {
            double maxDim = qMax(m_pageSizePt[m_primaryPage].width(), m_pageSizePt[m_primaryPage].height());
            double maxResZoom = m_renderer->fullQCapPx(m_primaryPage, maxDim) / qMax(maxDim, 1.0);
            // 🔴 LƯỢT 31b: so voi scale THẬT (so nguyen px/longSide) nen dung dung sai
            // 0,9 thay bang 1e-9 — anh day du bi le tron 1px van la "du net moi zoom".
            if (maxDim > 0 && zit.value() >= maxResZoom * 0.9) {
                qDebug() << "[perf] cont settle SKIP reason=alreadyFullQuality page=" << m_primaryPage;
                requestNeighborPages();
                return;
            }
        }
        qDebug() << "[perf] cont settle timeout — rendering primary page=" << m_primaryPage;
        if (vectorWillRender(m_primaryPage)) {
            qDebug() << "[cont] skip raster request (vector lo) page=" << m_primaryPage;
        } else {
            requestContinuousRender(m_primaryPage, m_zoom);
        }
        m_primaryRequested = true;
    });

// Sharp-region timer: debounced 180ms, requests crisp overlay for the primary
     // page when zoom is high enough that the base page image is being upscaled.
     m_sharpTimer = new QTimer(this);
     m_sharpTimer->setSingleShot(true);
     m_sharpTimer->setInterval(180);
     connect(m_sharpTimer, &QTimer::timeout, this, [this]() {
           if (!m_renderer || m_pageCount == 0) {
               qDebug() << "[perf] cont sharp request SKIP reason=chuaCoTaiLieu";
               return;
           }
           // 🔴🔴 SUA 2026-09-01 — GOC CUA "Continuous mo cam sau khi zoom".
           // `setZoom` dat m_primaryPage = -1 (chua biet trang nao o giua). Bo hen gio lam net
           // ban 180 ms sau do, thay -1 thi BO LUON — ma no la hen gio MOT LAN, khong thu lai.
           // ⇒ Sau MOI lan zoom, yeu cau lam net roi vao khoang trong nay va mat hut; trang o
           //   lai voi anh nen bi phong to ⇒ MO CAM. Do duoc tren file that: "SKIP
           //   reason=noPrimaryPage" lap lien tuc, xen giua dung MOT lan lam net thanh cong.
           // ⇒ Khong biet thi TU TINH LAY (pageAtCenter); van khong ra thi HEN LAI, dung bo cuoc.
           int pgPrimary = m_primaryPage;
           if (pgPrimary < 0) pgPrimary = pageAtCenter();
           if (pgPrimary < 0 || pgPrimary >= m_pageSizePt.size()) {
               qDebug() << "[perf] cont sharp request HEN LAI — chua biet trang giua";
               m_sharpTimer->start();
               return;
           }
           if (m_primaryPage < 0) m_primaryPage = pgPrimary;
           // VUNG SAC NET CHU THICH NGOAI: lam TRUOC nhanh lam net anh NEN duoi day, vi
           // nhanh do tu bo qua khi anh nen "da du" (dung ca khi nen la VECTOR — lop vector
           // khong chua annotation nen no khong biet lop bu dang nhoe). Xem requestForeignAnnotRegion.
           requestForeignAnnotRegion();
           const double longSidePt = qMax(m_pageSizePt[m_primaryPage].width(),
                                          m_pageSizePt[m_primaryPage].height());
           const double dpr        = devicePixelRatioF();
           const double longSidePx = longSidePt * m_zoom;
           // 🔴🔴 SUA 2026-09-01 (owner: "zoom 100% phai net, doi den 200% moi ro la nhu cut").
           // Nguong cu la MOT SO CUNG 4000 px: duoi no thi khong bao gio ve lai vung dang nhin.
           // Nhung cai quyet dinh NET hay MO khong phai con so do, ma la: anh NEN co du pixel
           // cho thu man hinh dang can khong.
           //   nen co  = min(zoom * 1.15, 4000/khoGiay) * khoGiay   (cong thuc render Continuous)
           //   can co  = khoGiay * zoom * heSoManHinh
           // Man hinh HiDPI lam "can" lon hon "nen co" ngay tu 100% ⇒ mo, ma nguong cu van coi
           // la "zoom thap" nen bo qua. Nay so THANG hai so do: thieu bao nhieu thi ve lai bay nhieu.
           // 🔴🔴 GOC CUA "mo file roi zoom len thi mo" (owner: "ban mo len thi net, toi Open
           // file thi mo") — do duoc 2026-09-01:
           //     mo o 25%  →  "downscale page= 2 from= 4000 to= 685"  (VUT ban goc 4000 px)
           //     zoom len 100%  →  render lai: 0 lan
           //   ⇒ anh 685 px bi keo len 2384 px ⇒ nhoe. Bai do cua toi khong thay vi no dat zoom
           //     NGAY LUC MO, nen anh sinh ra da dung co.
           // Ban cu doi chieu voi CONG THUC ly thuyet (zoom * 1.15 * khoGiay) — tuc gia dinh anh
           // luon duoc ve o dung zoom hien tai. Sai: anh dang giu la anh cua zoom CU.
           // ⇒ Doi chieu voi ANH THAT dang co. Thieu pixel that thi moi ve lai.
           double nenCoPx = qMin(m_zoom * 1.15 * longSidePt, PdfRenderer::kFullRenderMaxPx);
           {
               auto it = m_pageImages.constFind(m_primaryPage);
               if (it != m_pageImages.constEnd() && !it->isNull())
                   nenCoPx = qMax(it->width(), it->height());
           }
           const double canCoPx = longSidePt * m_zoom * dpr;
           if (nenCoPx >= canCoPx * 1.02) {
    m_sharpPage = -1;
    m_sharpPixmap = {};
               qDebug() << "[perf] cont sharp request SKIP reason=anhNenDaDu"
                        << "nenCo=" << nenCoPx << "canCo=" << canCoPx << "longSidePx=" << longSidePx;
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

     // Retry timer for primary page when rendering is skipped due to maxConcurrent limit.
     // 🔴 V2 2026-09-01 (HEAVYCAP): truoc day 300ms VO HAN — log chu san pham 0901 do ra
     // 779 lan "RETRY primary" cung mot trang 2,54 trieu object: moi dat lai lenh render
     // lai tang the he giet luot render dang chay => vong lap trang tron + app do.
     // Nay: dem theo trang, gui 300*2^n (tran 4s), dung han sau 8 lan. State nam trong
     // property cua timer (khong phai them thanh vien header). Doi trang / da co anh => reset.
     m_retryTimer = new QTimer(this);
     m_retryTimer->setInterval(300);
    connect(m_retryTimer, &QTimer::timeout, this, [this] {
        if (!m_renderer || m_pageCount == 0 || m_primaryPage < 0) return;
        // Da co anh hoac da co lop vector => khong can thu lai nua.
        if (m_pageImages.contains(m_primaryPage) || vectorWillRender(m_primaryPage)) {
            m_retryTimer->stop();
            m_retryTimer->setProperty("tries", 0);
            syncUrgentPrimary();
            return;
        }
        // 🔴 0928 LƯỢT 18 (VIỆC 1): do log that — moi dong "RETRY primary" deu co
        // "SKIP duplicate" ngay sau ⇒ luot render DANG BAY khong bi cat (gia thuyet
        // L17 "retry giet luot dang chay" LA SAI). Nhung loi that khac: retry van DEM
        // tries khi luot ve con dang chay ⇒ trang nang (~13 s, do r17) cong het ngan
        // sach 8 lan TRUOC khi ve xong; neu luot ay bi loai (nullResult het luot,
        // genStale) thi "RETRY DUNG" noi ra va KHONG BAO GIO xin lai duoc nua —
        // requestContinuousRender bi chinh m_pendingRenderScale chan "SKIP duplicate"
        // vinh vien ⇒ trang trang. ⇒ CHIX thu lai khi khong con luot nao dang bay/
        // xep hang cho dung trang+zoom nay; con thi im lang, khong dem, khong thay.
        if (qAbs(m_pendingRenderScale.value(m_primaryPage, -1.0) - m_zoom) < 1e-6
            && m_renderer->isContinuousPageActive(m_primaryPage))
            return;
        int tries = m_retryTimer->property("tries").toInt();
         if (m_retryTimer->property("page").toInt() != m_primaryPage) {
             tries = 0;
             m_retryTimer->setProperty("page", m_primaryPage);
         }
         if (tries >= 8) {   // ~25s gui lenh cho MOT luot xem trang — du cho luot ve 150 DPI ~2s
             m_retryTimer->stop();
             qDebug().noquote() << "[cont] RETRY DUNG page=" << m_primaryPage
                                << "— that bai lien tiep" << tries << "lan, dung lai khi doi trang";
             return;
         }
        ++tries;
        m_retryTimer->setProperty("tries", tries);
        m_retryTimer->setInterval(qMin(300 * (1 << tries), 4000));   // lui dan 0.6/1.2/2.4/4/4...s
        qDebug().noquote() << "[cont] RETRY primary page=" << m_primaryPage << " zoom=" << m_zoom
                           << "lan=" << tries;
        // LƯỢT 18: entry cu trong m_pendingRenderScale chinh la thu chan "SKIP
        // duplicate" — xoa de lenh dat lai THAT SU di toi renderer (khong con
        // duong nao xoan bo: khong co luot nao dang bay o day, da kiem phia tren).
        m_pendingRenderScale.remove(m_primaryPage);
        m_continuousRequested.remove(m_primaryPage);
        requestContinuousRender(m_primaryPage, m_zoom);
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

     m_vecPool.setMaxThreadCount(1);
     m_vecPool.setExpiryTimeout(30000);

     setViewport(new QOpenGLWidget(this));
}

ContinuousView::~ContinuousView()
{
     // 🔴 0928 LƯỢT 14: bật cờ huỷ TRƯỚC `waitForDone()` vô hạn. Mọi
     // `VectorLayer::build` của view này kiểm tra cờ ở đầu mỗi lát ⇒ dừng ở ranh
     // giới lát kế tiếp (≤ 40 ms) thay vì để lượt thoát chậm `--thoat-cham` treo
     // tới hết thời gian build (trang nặng mất vài giây, thậm chí 14 s).
     ++m_vecGen;
     if (m_doc) trdoc::cancelAll(m_doc->raw());
     m_vecPool.clear();
     m_vecPool.waitForDone();

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

void ContinuousView::setDocument(PdfDocument* doc, PdfRenderer* renderer,
                                 const QHash<int, QList<AnnotVisual>>* visualsSrc,
                                 QHash<int, std::shared_ptr<VectorLayer>>* vecStore)
{
    // Clear any text selection when document changes
    m_selecting = false;
    m_selStart  = m_selEnd = QPoint();
    m_selDragging = false;
    stopAutoScroll();
    clearTextSelectionInternal();

    // LÁT B 09/02: repoint kho lop vector ve tab dang gan. KHONG clear() — do la du lieu
    // cua tai lieu (DocTab::vecLayers), xoá qua con trỏ nay la xoá mat cua tab khac.
    m_vecStore = vecStore ? vecStore : &m_vecStoreEmpty;

    // Việc B: moi duong vao Continuous deu phai co khoa .torvec hop le. MainWindow
    // bo sot cho goi setVectorCacheKey o duong doi tab (onTabChanged) va tinh hash
    // bat dong bo (initWatcher) — nen setDocument tu bao dam key theo doc dang gan.
    updateVecCacheKeyFromDoc(doc);

    ++m_vecGen;
    m_vecTriedNoCache.clear();
    m_vecPool.clear();
    // LÁT B 09/02: bo `m_vecStore->clear()` — day la du lieu cua tab, khong phai cua view.
    m_fgnLayers.clear();
    clearForeignAnnotRegion();
    m_vecBuilding.clear();
    m_vecBuildStart.clear();
    m_rasterOnlyPages.clear();
    m_thumbRequested.clear();
    m_pageLowRes.clear();
    if (m_retryTimer) m_retryTimer->stop();
    m_pickCandidate = false;
    m_pickPage = -1;

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
        m_pendingRenderScale.clear();
        m_continuousPageReadyConn = connect(
            m_renderer, &PdfRenderer::continuousPageReady,
            this, [this](int idx, QImage img, double renderedScale) {
                acceptContinuousImage(idx, img, renderedScale);   // LƯỢT 36: thu nho xuong nen
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
        // 🔴 SUA 2026-08-31 — GOC CUA "sang View Quality roi quay lai thi trang bi MO".
        // Day la nhanh CUNG TAI LIEU (giu m_pageImages). Nhung anh nen giu lai la ban DA THU NHO
        // (log: "downscale page= 1 from= 3999 to= 821"); do NET thuc su den tu lop phu
        // m_sharpPixmap. Ban cu vut lop phu do moi lan quay lai => trang tut ve ban thu nho => MO.
        // Giu lai la AN TOAN: cho ve da co chot `m_sharpScale == m_zoom`, zoom khac thi tu khong ve.
        requestVisiblePages();
        viewport()->update();
        return;
    }

    m_sharpPage = -1;
    m_sharpPixmap = {};
    // 🔴 LƯỢT 33d (J): TAI LIEU MOI GAN (nhanh dung chung anh o tren khong toi day) —
    // trang dau cua tab vua mo la uu tien cao nhat, khong co cuoc cuon nao de
    // debounce: cache-miss lech settle 400 ms ⇒ ve ngay sau 30 ms.
    m_freshDocument = true;
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
    // LÁT A 09/02: view đọc kho duy nhất của tài liệu đang gắn — không giữ bản chép,
    // không clear (clear sẽ xoá dữ liệu của cả view Single dùng chung kho).
    m_visualsSrc = visualsSrc;
    // GOC1: tell renderer to use zoom-based scale in fast mode
    if (m_renderer) m_renderer->setContinuousUseZoomScale(m_fastMode);
    // LƯỢT 37 (reviewer lỗi 1): tai lieu DOI (ke ca clearDocument -> setDocument(nullptr)) ⇒ tang
    // dinh danh. Moi luot thu nho nen dang bay cua tai lieu CU se bat doc m_contDocGen != docGen
    // trong finished va BI BO, khong ghi anh cu vao kho cua tai lieu moi. (m_contStoreGen KHONG
    // xoa o day — chi-tang de tranh trung gen; docGen moi la thu chan tai lieu cu.)
    ++m_contDocGen;
    m_pageImages.clear();
    m_pageImageZoom.clear();
    m_continuousRequested.clear();
    m_pendingRenderScale.clear();
    // LÁT B 09/02: bo `m_vecStore->clear()` — do la kho cua tai lieu (DocTab), khong phai
    // cua view. View dung chung giua cac tab; clear o day tung xoá mat lop vector cua tab
    // khac (chính là bẫy Lát A gap phải). Repoint o dau ham nay la du.
    m_fgnLayers.clear();
    clearForeignAnnotRegion();
    m_vecBuilding.clear();
    m_vecTriedNoCache.clear();
    m_rasterOnlyPages.clear();
    m_lastEmittedPage = -1;
    // LOI 2 (0921): tai lieu moi => cho phep canh giua lai mot lan (khong giu moc range cua tab cu).
    m_lastHRange = 0;

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
                acceptContinuousImage(idx, img, renderedScale);   // LƯỢT 36: thu nho xuong nen
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
    // Scroll to top. Canvas hep hon khung nhin ⇒ can giua ngang (LOI 2).
    verticalScrollBar()->setValue(0);
    horizontalScrollBar()->setValue(m_canvasW <= viewport()->width()
                                        ? (m_canvasW - viewport()->width()) / 2 : 0);
    requestVisiblePages();
}

void ContinuousView::clearDocument()
{
    setDocument(nullptr, nullptr);
}

void ContinuousView::setVectorCacheKey(const QString& keyPath, quint64 keyHash)
{
    if (keyPath.isEmpty()) {
        // Lenh XOA tu MainWindow (dong het tab) — duong rong khong bao gio la key hop le.
        m_vecKeyPath.clear();
        m_vecKeyHash = 0;
        m_vecKeyDoc = nullptr;
        return;
    }
    // Giu cho key lon xon tu async init: MainWindow goi setVectorCacheKey voi
    // t->pdfHash TRUOC khi initWatcher tinh xong (hash bang 0). Key 0 = nokey =
    // chan ca duong nap cache .torvec. Bo qua de giu key da dan xuat tu setDocument.
    if (keyHash == 0) return;
    m_vecKeyPath = keyPath;
    m_vecKeyHash = keyHash;
}

void ContinuousView::updateVecCacheKeyFromDoc(PdfDocument* doc)
{
    // Chot bao mat: doc DOI thi key cu (file A) phai bo, khong duoc nap cache cua
    // file A cho trang cua file B.
    if (m_vecKeyDoc != doc) {
        m_vecKeyPath.clear();
        m_vecKeyHash = 0;
        m_vecKeyDoc = doc;
    }
    if (!doc || !doc->isOpen()) return;
    if (!m_vecKeyPath.isEmpty() && m_vecKeyHash != 0) return;   // da co key hop le
    const QString fp = doc->filePath();
    if (fp.isEmpty()) return;
    // Self-derive: giong fallback cua MainWindow::buildVectorLayer (pdfHash ? pdfHash
    // : TileCacheFile::hashFile). Từ LƯỢT 31 hash đọc 64KB đầu+cuối + 8 khối 4KB rải
    // giữa file + stat mtime (~192 KB, vẫn rẻ ~vài ms), chỉ goi khi thieu key.
    m_vecKeyPath = fp;
    m_vecKeyHash = (quint64)TileCacheFile::hashFile(fp);
}

void ContinuousView::setPageLowRes(int pageIndex, const QImage& img)
{
    if (img.isNull() || pageIndex < 0) return;
    // 2026-09-20: anh nen ve SAU ⇒ `daCoGiDeNhin` doi tu false sang true. Dau cu trong
    // m_vecTriedNoCache se chan khong cho xep lich lai ⇒ phai xoa. setPageLowRes() duoc
    // goi o MOI duong co anh (raster day du lan thumbnail tu MainWindow), nen day la chot chung.
    m_vecTriedNoCache.remove(pageIndex);
    QImage lowres = (img.width() >= img.height())
        ? img.scaledToWidth(qMin(320, img.width()), Qt::SmoothTransformation)
        : img.scaledToHeight(qMin(320, img.height()), Qt::SmoothTransformation);
    m_pageLowRes.insert(pageIndex, QPixmap::fromImage(lowres));
}

// 🔴 LƯỢT 36 (mục 2) — handler continuousPageReady gom về một chỗ, VIỆC NẶNG xuống nền.
// Đo r36 guilock m1 (bước I, zoom 200% trang quái vật): UI đứng hình 2 326 ms trong cửa sổ
// zoomsharp; chuỗi đồng bộ cacheProbe→emit→`img.scaledToWidth(4000→1100)` + lowres +
// QPixmap::fromImage chạy trên UI MỌI ảnh tới (kể cả phát trực tiếp từ requestFromCacheOnly
// ForContinuous). UI giờ chỉ làm bookkeeping; ảnh quá cỡ thì pool thu nhỏ, về tới nơi kiểm
// generation (ảnh mới hơn đã tới ⇒ bỏ bản cũ, không ghi đè).
void ContinuousView::acceptContinuousImage(int idx, QImage img, double renderedScale)
{
    m_cacheProbed.remove(idx);
    m_pendingRenderScale.remove(idx);
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
    const double needW = pageSizePt(idx).width() * m_zoom * devicePixelRatioF();
    const bool phaiNho = needW > 1.0 && img.width() > needW * 1.5;
    m_vecTriedNoCache.remove(idx);   // 2026-09-20: anh day du ve ⇒ xoa dau (xem setPageLowRes)
    m_thumbRequested.remove(idx);    // BAC 2: da co raster, khong can thumbnail nua
    // ⚠️ KHONG dat nhan vao m_pageImageZoom o day neu anh con do lai nen: settle timer
    // doc no de quyet "alreadyFullQuality" — nhan som = anh 357px mang nhan 4000px
    // ⇒ bo ve lai ⇒ KET MO (chinh cai bẫy 31b). Nhan chi ghi khi ANH THAT da nam trong store.
    if (!phaiNho) m_pageImageZoom[idx] = renderedScale;
    qDebug() << "[perf] cont ACCEPT continuousPageReady idx=" << idx
             << "imgW=" << img.width() << "renderedScale=" << renderedScale
             << (phaiNho ? "nhoNen=background" : "");
    { static const bool dump = qEnvironmentVariableIsSet("TORREADER_DUMP");
      if (dump) {
          const QString fn = QString("contview_dump_p%1.png").arg(idx);
          img.save(fn);
          qDebug() << "[dump] saved" << fn << "w=" << img.width() << "h=" << img.height();
      } }
    m_continuousRequested.remove(idx);
    syncUrgentPrimary();   // L18: trang nay da co anh => tha cho thumbnail
    needAnnotVisuals(idx); // noi dung da san — lay lop bu neu bi DEFER lan 1
    if (idx == m_primaryPage) {
        m_contSettleTimer->stop();
        requestNeighborPages();
    }
    const quint64 gen = ++m_contStoreGen[idx];
    const quint64 docGen = m_contDocGen;   // LƯỢT 37: dinh danh tai lieu luc phat hanh thu nhỏ
    if (!phaiNho) {
        m_pageImages[idx] = QPixmap::fromImage(img);
        setPageLowRes(idx, img);
        if (pageVisible(idx))
            viewport()->update();
        return;
    }
    // Co thang ve DUNG co man hinh: chi con MOT lan lay mau (SUA 2026-09-01 giu nguyen
    // chat luong), nhung GIU NGUOI lam — QtConcurrent, khong phai UI.
    auto* w = new QFutureWatcher<QPair<QImage,QImage>>(this);
    ++m_contDownscaleInFlight;   // LƯỢT 37 bước O: probe biết có thu nhỏ đang bay để đổi tab ĐÚNG LÚC
    connect(w, &QFutureWatcher<QPair<QImage,QImage>>::finished, this, [this, w, idx, gen, docGen, renderedScale] {
        const QPair<QImage,QImage> res = w->result();
        w->deleteLater();
        --m_contDownscaleInFlight;
        // LƯỢT 37 (reviewer lỗi 1): bỏ bản cũ nếu TÀI LIỆU đã đổi (docGen) HOẶC ảnh mới hơn cùng
        // trang đã về (gen). Trước đây chỉ chặn gen ⇒ đổi tab/đóng tab giữa chừng ghi ảnh tài liệu
        // CŨ vào kho tài liệu MỚI + dán nhãn zoom giả ⇒ trang sai vĩnh viễn (settle không vẽ lại).
        if (m_contDocGen != docGen || m_contStoreGen.value(idx, 0) != gen) {
            ++m_contStaleDrops;
            return;
        }
        if (!res.first.isNull()) {
            m_pageImages[idx] = QPixmap::fromImage(res.first);
            m_pageImageZoom[idx] = renderedScale;          // ghi nhan CUNG LUC voi anh (xem canh o tren)
        }
        if (!res.second.isNull())
            m_pageLowRes.insert(idx, QPixmap::fromImage(res.second));
        qDebug().noquote() << "[cont] downscale-xong page=" << idx
                           << "to=" << res.first.width();
        if (pageVisible(idx))
            viewport()->update();
    });
    w->setFuture(QtConcurrent::run([img, needW]() -> QPair<QImage,QImage> {
        // LƯỢT 37 bước O (probe-only, env TORREADER_PROBE_DOWNSCALE_DELAY, mặc định 0 ⇒ không ảnh
        // hưởng sản xuất): kéo dài cửa sổ "thu nhỏ đang bay" để probe ĐỔI TAB đúng giữa chừng và
        // chứng minh guard docGen bỏ bản cũ. Đây là cách duy nhất test deterministic một race bất đồng bộ.
        static const int s_probeDelay = qEnvironmentVariableIntValue("TORREADER_PROBE_DOWNSCALE_DELAY");
        if (s_probeDelay > 0) QThread::msleep(s_probeDelay);
        QImage lowres = (img.width() >= img.height())
            ? img.scaledToWidth(qMin(320, img.width()), Qt::SmoothTransformation)
            : img.scaledToHeight(qMin(320, img.height()), Qt::SmoothTransformation);
        return { img.scaledToWidth(int(needW), Qt::SmoothTransformation), lowres };
    }));
}

// Neo zoom theo TOA DO TAI LIEU (dung chung setZoom + Ctrl+wheel).
//
// 🔴 GOC LOI "Ctrl+cuon zoom lam nhay vi tri": cong thuc cu neo bang ti le pixel
//    `canvas * (newZoom/m_zoom)`. No GIA DINH canvas no deu quanh goc (0,0), nhung
//    bo cuc co HANG SO KHONG NO THEO ZOOM:
//      · kGap = 12  (khoang cach giua cac trang)
//      · kHPad = 40 (dem ngang, m_canvasW = maxPageW + kHPad)
//      · pageLeftX(i) = m_canvasW/2 - pageW(i)/2  (can giua — phu thuoc m_canvasW)
//    ⇒ lech DOC `i * kGap * (ratio - 1)` (CONG DON theo chi so trang) va lech NGANG
//      `(kHPad/2) * (ratio - 1)`. Cang zoom nhieu / cang o cuoi tai lieu cang nhay to.
//    Cach dung: doc lai pageLeftX/pageTopY SAU rebuildLayout, vi moi thu da doi.
//
// anchorVp khong nam tren trang nao (khoang trong giua 2 trang / vung dem) ⇒
// GIU NGUYEN diem tai lieu o TAM KHUNG NHIN, khong neo theo con tro.
void ContinuousView::zoomAnchored(double newZoom, const QPointF& anchorVp)
{
    if (qAbs(newZoom - m_zoom) < 1e-9) return;

    QElapsedTimer tLayout; tLayout.start();
    const int scrollX = horizontalScrollBar()->value();
    const int scrollY = verticalScrollBar()->value();

    if (m_pageCount <= 0) {
        // Khong co trang => khong co moc tai lieu; lui ve ti le thuong.
        const double ratio = newZoom / m_zoom;
        const double cx = scrollX + anchorVp.x();
        const double cy = scrollY + anchorVp.y();
        m_zoom = newZoom;
        rebuildLayout();
        // setValue tu clamp vao range (co the AM khi canvas hep hon khung nhin — LOI 2).
        horizontalScrollBar()->setValue(qRound(cx * ratio - anchorVp.x()));
        verticalScrollBar()->setValue(qMax(0, qRound(cy * ratio - anchorVp.y())));
        qDebug().noquote() << "[zoomms] rebuildLayout ms=" << tLayout.elapsed();
        return;
    }

    // 1) TRUOC khi doi zoom: tim trang GAN diem neo nhat, va diem do nam o dau TRONG
    //    trang — do bang POINT cua PDF (khong phai pixel da ve, tranh sai so lam tron).
    //    🔴 LOI 1 (0921): con tro roi vao KHE 12px giua 2 trang thi ban cu BO NEO, lui
    //    ve neo TAM KHUNG NHIN ⇒ diem duoi con tro troi (cursorY-centerY)*(ratio-1)
    //    (do 400px o ratio 3). Nay: chon trang GAN NHAT, de fy ngoai suy ra ngoai
    //    [0,1] (fy<0 hoac >1) — phep bien doi la TUYEN TINH nen van chinh xac.
    //    Chi lui ve tam khung nhin khi THAT SU khong co trang nao (m_pageCount==0,
    //    da xu ly o tren).
    const double canvasY = scrollY + anchorVp.y();
    int pg = 0;
    double bestDist = 1e18;
    for (int i = 0; i < m_pageCount; ++i) {
        const double top = pageTopY(i);
        const double bot = top + pageH(i);
        if (canvasY >= top && canvasY < bot) { pg = i; bestDist = 0.0; break; }
        const double d = (canvasY < top) ? (top - canvasY) : (canvasY - bot);
        if (d < bestDist) { bestDist = d; pg = i; }   // hoa ⇒ giu trang TREN
    }
    const double pwPt = qMax(1e-6, m_pageSizePt[pg].width());
    const double phPt = qMax(1e-6, m_pageSizePt[pg].height());
    const double fx = (scrollX + anchorVp.x() - pageLeftX(pg)) / (pwPt * m_zoom);
    const double fy = (scrollY + anchorVp.y() - pageTopY(pg)) / (phPt * m_zoom);

    // 2) Doi zoom + dung lai bo cuc.
    m_zoom = newZoom;
    rebuildLayout();

    // 3) SAU rebuild: tinh lai vi tri canvas cua DUNG diem tai lieu do.
    const double newCanvasX = pageLeftX(pg) + fx * pwPt * m_zoom;
    const double newCanvasY = pageTopY(pg)  + fy * phPt * m_zoom;
    // setValue tu clamp vao range ngang (co the AM khi canvas hep hon khung nhin — LOI 2).
    horizontalScrollBar()->setValue(qRound(newCanvasX - anchorVp.x()));
    verticalScrollBar()->setValue(qMax(0, qRound(newCanvasY - anchorVp.y())));
    qDebug().noquote() << "[zoomms] rebuildLayout ms=" << tLayout.elapsed();
}

void ContinuousView::setZoom(double scale)
{
    double newZoom = qBound(0.1, scale, 10.0);
    if (qAbs(newZoom - m_zoom) < 1e-9) return;
    m_zooming = true;
        qDebug().noquote() << "[zoomms] setZoom ENTER from=" << m_zoom << " to=" << newZoom;

    // Giu diem tai lieu o TAM KHUNG NHIN (cung goc neo voi Ctrl+wheel; cong thuc
    // ti le cu lam nhay vi kGap/kHPad khong no theo zoom).
    // Keep old images as blurry placeholders; paintEvent scales them until
    // new renders arrive. Clearing here would cause a blank-page flash.
    zoomAnchored(newZoom, QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0));

    m_sharpPage = -1;
    m_sharpPixmap = {};
    clearForeignAnnotRegion();   // zoom moi => vung sac net cu het hieu luc
    m_primaryPage = -1;
    m_lastRequestZoom = -1.0;
    m_primaryRequested = false;
    m_contSettleTimer->stop();
    // GOC3: don't clear m_continuousRequested — let in-flight renders finish
    // and be accepted as blurry placeholders. Dedup in requestContinuousRender
    // will cancel+replace if scale differs.
    // Don't clear m_pageImageZoom — keep existing scale info so settle timer
    // can check "already full quality" and skip unnecessary re-renders.
    m_pendingRenderScale.clear();
    m_cacheProbed.clear();
    // 🔴🔴 SUA 2026-08-31 — hai loi cua chinh ban va truoc do, lo ra khi owner bao
    // "Continuous bi mat Markup khi zoom":
    // (1) `m_vecBuildArmed` CHI duoc bat boi m_contSettleTimer, ma setZoom lai STOP chinh
    //     bo hen gio do (dong ngay tren) ⇒ sau mot lan zoom, duong vector bi khoa VINH VIEN.
    // (2) Markup ve bang overlay lay tu `kho visuals duy nhat cua tai lieu`, ma visuals chi duoc xin lai khi
    //     raster/vector toi. Zoom huy het viec dang bay ⇒ khong con su kien nao xin lai visuals.
    // ⇒ Hen gio lai sau khi zoom: 400 ms sau se vua mo cong dung vector, vua DOI LAI visuals
    //   cho moi trang dang nhin thay.
    m_vecBuildArmed = false;
    m_vecTriedNoCache.clear();
    m_contSettleTimer->start();
viewport()->update();
 m_zoomTimer->start();
        m_sharpTimer->start();
        if (m_vecBuildTimer) m_vecBuildTimer->start();
        m_zooming = false;
        qDebug().noquote() << "[zoomms] setZoom DONE";
    }

void ContinuousView::setDarkMode(bool dark)
{
    m_darkMode = dark;
    viewport()->update();
}

void ContinuousView::scrollToPage(int pageIndex, const char* via)
{
    if (m_pageCount == 0) return;
    pageIndex = qBound(0, pageIndex, m_pageCount - 1);
    // 🔴 LƯỢT 33f (mục 5 — L tab đang load hiện trang 2 thay vì 0): nghi AI CUỘN LẠI
    // offset sau restore. `via` = caller tự khai ở từng chỗ gọi. Đo, không đoán.
    qDebug().noquote() << "[cont-scroll] scrollToPage" << pageIndex
                       << "via=" << via << "primary=" << m_primaryPage;
    int targetY = pageTopY(pageIndex);
    verticalScrollBar()->setValue(targetY);
}

// 🔴 LƯỢT 33b (L): doc/ghi offset cuon THAT de khoi phuc dung vi tri khi quay lai
// tab. setValue tu clamp vao range ⇒ an toan khi layout cua tab chua bang luc luu.
int ContinuousView::scrollY() const
{
    return verticalScrollBar()->value();
}

void ContinuousView::restoreScrollY(int y)
{
    if (m_pageCount == 0) return;
    // 🔴 LƯỢT 33f (muc 5 — L): moc restore de so sinh voi cac lan cuon SAU do.
    qDebug().noquote() << "[cont-scroll] restoreScrollY" << y
                       << "primary=" << m_primaryPage;
    verticalScrollBar()->setValue(qMax(0, y));   // setValue tu clamp theo maximum
    // Khong de vung xem TRANG: xin anh ngay cac trang vuot vao khung nhin sau khi
    // dat lai offset, va bao view ve (setDocument o tren moi reset ve dinh + clear anh).
    requestVisiblePages();
    viewport()->update();
}

void ContinuousView::scrollToPageRect(int page, const QRectF& rectPdf)
{
    if (m_pageCount == 0) return;
    page = qBound(0, page, m_pageCount - 1);
    const QRectF nr = rectPdf.normalized();
    if (nr.width() < 0.5 || nr.height() < 0.5) {
        scrollToPage(page, "scrollToPageRect");
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
    // setValue tu clamp: range ngang co the AM khi canvas hep hon khung nhin (LOI 2).
    // 🔴 LOI 3 (0921): khi ca trang LOT NGANG (canvas <= vpW), ket qua tim kiem da nam
    // trong tam nhin => KHONG keo ngang nua. Truoc day range 0..0 nen bi kep o 0 => dung
    // yen; tu khi cho range am, tam rect lech tam se truot ca trang sang ngang (nhay mat).
    // Chi keo ngang khi trang RONG hon khung nhin (luc do moi that su can).
    if (m_canvasW > vpW)
        horizontalScrollBar()->setValue(static_cast<int>(rectCx - vpW / 2.0));
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
            QElapsedTimer timerAll; timerAll.start();
            m_pageTopY_cache.resize(m_pageCount);
            m_canvasW = 0;
            m_canvasH = 0;

            if (m_pageCount == 0) {
                updateScrollBars();
                qDebug().noquote() << "[zoomms]   layout loop ms=" << 0;
                qDebug().noquote() << "[zoomms] rebuildLayout BODY ms=" << timerAll.elapsed() << " pages=" << m_pageCount;
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
            QElapsedTimer timerLoop; timerLoop.start();
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

    // 🔴 LOI 2 (0921): khi canvas HEP hon khung nhin (trang nho / zoom thap), ban cu
    // range ngang 0..0 ⇒ scrollX=0, trang dinh MEP TRAI va gian sang phai khi zoom.
    // Vung zoom do dung HANG NGAY (A4 cua so 1500px: zoom < ~245%) nen trieu chung
    // "hay nhay" chua het. Nay: cho range ngang xuong AM — do la luong dem can de
    // truot trang tu mep trai sang mep phai. QUY UOC DUY NHAT KHONG DOI:
    //   viewport_x = canvasX - scrollX
    // (paintEvent + moi ham pageLeftX/pageTopY dung chung quy uoc nay; chi scrollX
    //  duoc phep AM). Mac dinh dat GIUA khung nhin khi ca trang lot trong.
    const bool fitsW = (m_canvasW <= vpW);
    const int hRange = m_canvasW - vpW;

    // 🔴 LOI B (0921): nguoi dung CHUA keo (con o giua) thi khi vpW doi (dong/mo sidebar
    // hoac doi co cua so) PHAI canh giua lai. Ban cu chi canh giua khi `m_lastHRange >= 0`
    // nen khi dang lot-trong (range AM) ma vpW doi thi scrollX giu nguyen => trang lech tam
    // (vd canvas 635 trong vpW 1369: -367; dong sidebar cho vpW 1620 => lech ~125px).
    // Chup TRUOC setRange: "chua keo" = gia tri cu == tam range cu (m_lastHRange/2).
    const bool wasCentred = (m_lastHRange < 0) &&
                            (horizontalScrollBar()->value() == m_lastHRange / 2);

    // 🔴 LOI 1 (0921): Qt hien thanh cuon khi min < max; range am ([hRange,0]) nen thanh
    // LUON hien, ke ca khi khong co gi de cuon — an mat ~17px chieu cao o dung truong hop
    // PHO BIEN NHAT (zoom thap / chua mo tai lieu). Vi `hRange < 0 ⟺ canvas <= vpW`, ta an
    // han thanh ngang trong truong hop lot-trong. Range AM VAN duoc dat (setValue/setRange
    // chay du thanh an) nen `zoomAnchored` VAN neo duoc diem duoi con tro — khong pha 31 PASS.
    // Khong can nguong "|hRange| >= 2px": range am chi ton tai khi canvas lot-trong, ma khi
    // do thanh da an => khong bao gio hien mot thanh ngang "rong".
    // ⛔ KHONG chuyen khoi policy nay xuong CUOI ham. Da do tren file quai vat (2,54 trieu
    // object, `--scroll-probe 12 500`): dat policy o cuoi ⇒ doi policy lam viewport resize ⇒
    // goi LONG lai updateScrollBars ⇒ dat range lan nua ⇒ TANG bo dem the he render ⇒ render
    // dang bay bi VUT (`reason=genMismatchMid progressive`) gap 6 lan (2 -> 12) ⇒ cham ~70%
    // (slice tb 2,0s -> 4,2s). Giu o DAU ham: doi policy TRUOC khi tinh range la duong nhe,
    // khong sinh them the he render. (xem them GIOI HAN da chap nhan o comment LOI 2 duoi.)
    const Qt::ScrollBarPolicy wantPolicy =
        fitsW ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded;
    if (horizontalScrollBarPolicy() != wantPolicy)
        setHorizontalScrollBarPolicy(wantPolicy);

    horizontalScrollBar()->setRange(qMin(0, hRange), qMax(0, hRange));
    horizontalScrollBar()->setPageStep(vpW);
    horizontalScrollBar()->setSingleStep(20);

    verticalScrollBar()->setRange(0, qMax(0, m_canvasH - vpH));
    verticalScrollBar()->setPageStep(vpH);
    verticalScrollBar()->setSingleStep(40);

    // 🔴 LOI 2 (0921): canh giua khi range VUA tro nen lot-trong (m_lastHRange >= 0 = lan
    // dau dat range / truoc do con cuon ngang duoc) HOAC khi nguoi dung con o giua (LOI B:
    // vpW doi luc dang lot-trong). Neu nguoi dung DA keo lech tam thi giu nguyen vi tri
    // (setRange o tren de Qt tu clamp vao range moi). Ban cu ep `setValue(hRange/2)` MOI lan
    // => huy thao tac keo.
    // ⚠️ GIOI HAN DA CHAP NHAN — KHONG "SUA LAI": vi policy doi o DAU ham (truoc setRange),
    // o luot CHUYEN TIEP lot<->khong-lot range DOC lech ~17px (thanh ngang hien/an lam viewport
    // doi chieu cao NGAY trong cung mot luot). Loi nay TU LANH o lan resize/zoom ke tiep.
    // Da can nhac chuyen policy xuong cuoi de het lech — KHONG lam: xem comment khoi policy,
    // cai gia la cham ~70% tren file nang. Dung "sua lai" roi tai pham.
    if (fitsW && (m_lastHRange >= 0 || wasCentred))
        horizontalScrollBar()->setValue(hRange / 2);   // trang giua khung nhin
    m_lastHRange = hRange;
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
{{ QElapsedTimer _e; _e.start(); 
    { QElapsedTimer _e; _e.start(); 
        { QElapsedTimer _e; _e.start(); 
            
                if (m_pageCount == 0) return 0;
                int scrollY = verticalScrollBar()->value();
                int vpH     = viewport()->height();
                int centerY = scrollY + vpH / 2;  // canvas Y at viewport center

                // 🔴🔴 SUA 2026-08-31 — GOC CUA "trang minh dang nhin thi khong ai ve".
                // Luat cu lay trang nam o DIEM GIUA man hinh. Voi ban ve A0 NAM NGANG o muc
                // vua-khung, chieu cao trang tren man hinh nho hon nhieu chieu cao khung nhin,
                // nen diem giua roi vao trang KHAC. Do duoc bang --scroll-probe: cuon toi
                // trang 0 nhung "cont primary" bao 2 — lech DUNG 2 trang, va vi chi trang chinh
                // ±1 duoc dat lenh ve, TRANG 0 KHONG HE DUOC YEU CAU VE MOT LAN NAO
                // (0 dong nhac toi trang 0 trong toan bo log Continuous).
                // Day chinh la trieu chung owner bao: trang 1 quai vat chi thay may net, va
                // "trang 2, 3 mo lau hon cac trang khac" — chung bi lech ra ngoai vung uu tien.
                // ⇒ Lay trang CHIEM NHIEU DIEN TICH NHAT trong khung nhin. Do la trang nguoi
                // dung thuc su dang doc, du trang cao hay thap.
                {
                    const int vpTop = scrollY, vpBot = scrollY + vpH;
                    int bestPg = -1, bestCover = 0;
                    for (int i = 0; i < m_pageCount; ++i) {
                        const int top = pageTopY(i);
                        if (top >= vpBot) break;              // cac trang sau deu nam duoi
                        const int bot = top + pageH(i);
                        if (bot <= vpTop) continue;           // trang nay da troi len tren
                        const int cover = qMin(bot, vpBot) - qMax(top, vpTop);
                        if (cover > bestCover) { bestCover = cover; bestPg = i; }
                    }
                    if (bestPg >= 0) return bestPg;
                }
            
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
        const qint64 _m=_e.elapsed(); if(_m>20) qDebug().noquote()<<"[scrollms] pageAtCenter ms="<<_m;
        }
    const qint64 _m=_e.elapsed(); if(_m>20) qDebug().noquote()<<"[scrollms] pageAtCenter ms="<<_m;
    }
const qint64 _m=_e.elapsed(); if(_m>20) qDebug().noquote()<<"[scrollms] pageAtCenter ms="<<_m;
}}

// Probe-only (--zoomanchor-probe): phep do DUOC cho loi "Ctrl+zoom lam nhay vi tri".
// Dat con tro tai diem (fx,fy) TRONG trang (ti le 0..1), ban mot su kien Ctrl+wheel
// THAT, roi do toa do TAI LIEU (PDF point) duoi con tro TRUOC va SAU zoom.
// Dong thoi mo phong cong thuc CU (ti le `ratio`) de chung minh ban cu SAI.
QString ContinuousView::probeZoomAnchor(int page, double zoomFrom, double zoomTo,
                                        double fx, double fy, int mode, double offY)
{
    if (m_pageCount <= 0) return QStringLiteral("ZOOMANCHOR_SKIP pages=0\n");
    page = qBound(0, page, m_pageCount - 1);

    setZoom(zoomFrom);
    scrollToPage(page, "probeZoomAnchor");

    const int vpW = viewport()->width();
    const int vpH = viewport()->height();

    // Diem neo tren canvas (canvas coords). mode=0: diem (fx,fy) TRONG trang.
    // mode=1 (LOI 1): KHE 12px ngay duoi trang `page`, dat tai 25% khe (gan trang
    // TREN) ⇒ trang gan nhat la trang `page` — cung luat chon trang voi zoomAnchored.
    int refPage = page;
    double anchorX = pageLeftX(page) + fx * pageW(page);
    double anchorY = pageTopY(page)  + fy * pageH(page);
    if (mode == 1) {
        if (page + 1 < m_pageCount)
            anchorY = pageTopY(page) + pageH(page) + kGap * 0.25;
        else
            anchorY = pageTopY(page) + kGap * 0.25;   // trang cuoi: khong co khe duoi
    }

    // Con tro dat lech tam khung nhin `offY` px (de lo loi fallback neo TAM khung nhin).
    const double cursorX = vpW / 2.0;
    const double cursorY = vpH / 2.0 + offY;
    // setValue tu clamp vao range (range ngang co the AM khi canvas hep hon khung nhin).
    horizontalScrollBar()->setValue(qRound(anchorX - cursorX));
    verticalScrollBar()->setValue(qMax(0, qRound(anchorY - cursorY)));
    const QPointF cursorVp(anchorX - horizontalScrollBar()->value(),
                           anchorY - verticalScrollBar()->value());

    auto docPt = [&](int pg) {
        return QPointF((horizontalScrollBar()->value() + cursorVp.x() - pageLeftX(pg)) / m_zoom,
                       (verticalScrollBar()->value() + cursorVp.y() - pageTopY(pg))  / m_zoom);
    };
    // Mep trang GAN con tro nhat (vat ly nguoi dung nhin thay) trong he viewport.
    auto nearestEdgeVpY = [&]() {
        const double bottom  = pageTopY(refPage) + pageH(refPage) - verticalScrollBar()->value();
        if (refPage + 1 < m_pageCount) {
            const double topNext = pageTopY(refPage + 1) - verticalScrollBar()->value();
            return (qAbs(topNext - cursorVp.y()) < qAbs(bottom - cursorVp.y())) ? topNext : bottom;
        }
        return bottom;
    };

    const double zoom0 = m_zoom;
    const double cursorCanvasX = horizontalScrollBar()->value() + cursorVp.x();
    const double cursorCanvasY = verticalScrollBar()->value() + cursorVp.y();
    const double centerCanvasY0 = verticalScrollBar()->value() + vpH / 2.0;  // TRUOC zoom
    const QPointF before = docPt(refPage);
    const double edgeBefore = nearestEdgeVpY();

    // Mot nac Ctrl+wheel THAT, delta du de toi dung zoomTo trong mot su kien.
    const int angle = qRound((zoomTo - zoom0) * 1200.0);
    QWheelEvent ev(cursorVp, mapToGlobal(cursorVp.toPoint()), QPoint(0, 0), QPoint(0, angle),
                   Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
    wheelEvent(&ev);

    const QPointF after = docPt(refPage);
    const double edgeAfter = nearestEdgeVpY();
    const double devX = (after.x() - before.x()) * m_zoom;
    const double devY = (after.y() - before.y()) * m_zoom;
    const double devNew = qMax(qAbs(devX), qAbs(devY));
    const double devEdge = edgeAfter - edgeBefore;

    // Bi chan boi thanh cuon? (range ngang nay co the AM — chi bao khi vuot ca range).
    const int wantScrollX = qRound(pageLeftX(refPage) + before.x() * m_zoom - cursorVp.x());
    const int wantScrollY = qMax(0, qRound(pageTopY(refPage)  + before.y() * m_zoom - cursorVp.y()));
    const bool clampX = qAbs(horizontalScrollBar()->value() - wantScrollX) > 1;
    const bool clampY = qAbs(verticalScrollBar()->value()  - wantScrollY) > 1;
    const bool clamped = clampX || clampY;

    // Mo phong cong thuc CU (da xoa): scroll = canvas * ratio, range ngang 0..max.
    const double ratio = m_zoom / zoom0;
    const int maxX = qMax(0, m_canvasW - vpW);
    const int maxY = qMax(0, m_canvasH - vpH);
    const int oldScrollX = qBound(0, static_cast<int>(cursorCanvasX * ratio - cursorVp.x()), maxX);
    const int oldScrollY = qBound(0, static_cast<int>(cursorCanvasY * ratio - cursorVp.y()), maxY);
    const double oldDocX = (oldScrollX + cursorVp.x() - pageLeftX(refPage)) / m_zoom;
    const double oldDocY = (oldScrollY + cursorVp.y() - pageTopY(refPage))  / m_zoom;
    const double devOld = qMax(qAbs((oldDocX - before.x()) * m_zoom),
                               qAbs((oldDocY - before.y()) * m_zoom));

    // Mo phong FALLBACK CU cua LOI 1: neu diem neo khong nam tren trang nao thi ban cu
    // bo neo, lui ve neo TAM khung nhin (scroll = canvas cua tam * ratio).
    double devOldFallback = devOld;
    if (mode == 1) {
        const int oldFallScrollY = qBound(0, static_cast<int>(centerCanvasY0 * ratio - vpH / 2.0), maxY);
        const double oldFallDocY = (oldFallScrollY + cursorVp.y() - pageTopY(refPage)) / m_zoom;
        devOldFallback = qAbs((oldFallDocY - before.y()) * m_zoom);
    }

    const int rangeMinX = qMin(0, m_canvasW - vpW);
    const int rangeMaxX = qMax(0, m_canvasW - vpW);

    QString rep;
    rep += QStringLiteral("ZOOMANCHOR page=%1 fx=%2 fy=%3 mode=%4 offY=%5 zoom %6 -> %7\n")
               .arg(page).arg(fx, 0, 'f', 2).arg(fy, 0, 'f', 2).arg(mode)
               .arg(offY, 0, 'f', 0).arg(zoom0, 0, 'f', 3).arg(m_zoom, 0, 'f', 3);
    rep += QStringLiteral("  docPt TRUOC (pt) x=%1 y=%2\n")
               .arg(before.x(), 0, 'f', 3).arg(before.y(), 0, 'f', 3);
    rep += QStringLiteral("  docPt SAU   (pt) x=%1 y=%2\n")
               .arg(after.x(), 0, 'f', 3).arg(after.y(), 0, 'f', 3);
    rep += QStringLiteral("  lech MOI (px) dx=%1 dy=%2 max=%3  %4%5\n")
               .arg(devX, 0, 'f', 3).arg(devY, 0, 'f', 3).arg(devNew, 0, 'f', 3)
               .arg(devNew < 2.0 ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
               .arg(clamped ? QStringLiteral(" [CLAMPED]") : QString());
    rep += QStringLiteral("  lech CU  (px) dx=%1 dy=%2 max=%3 (cong thuc ti le cu)\n")
               .arg((oldDocX - before.x()) * m_zoom, 0, 'f', 3)
               .arg((oldDocY - before.y()) * m_zoom, 0, 'f', 3)
               .arg(devOld, 0, 'f', 3);
    if (mode == 1)
        rep += QStringLiteral("  lech FALLBACK CU (px) max=%1 (bo neo -> tam khung nhin)\n")
                   .arg(devOldFallback, 0, 'f', 3);
    if (mode == 1)
        rep += QStringLiteral("  mep trang gan con tro doi (px) %1\n").arg(devEdge, 0, 'f', 3);
    rep += QStringLiteral("  cuon: rangeX=%1..%2 rangeY=0..%3\n")
               .arg(rangeMinX).arg(rangeMaxX).arg(qMax(0, m_canvasH - vpH));
    return rep;
}

// Probe-only (--scrollpersist-probe 0921): LOI 2 + LOI B — updateScrollBars khong ep ve giua
// moi lan goi (resizeEvent). Dat canvas < khung nhin, keo ngang lech tam, goi lai
// updateScrollBars => vi tri nguoi dung giu nguyen; con lan dau dat range thi VAN canh giua.
// LOI B: khi vpW DOI luc dang lot-trong thi chua keo => canh giua lai, da keo => giu nguyen.
QString ContinuousView::probeScrollPersist(double zoom, bool probeRealWindow)
{
    if (m_pageCount <= 0) return QStringLiteral("SCROLLPERSIST_SKIP pages=0\n");
    setZoom(zoom);

    const int vpW = viewport()->width();
    const bool fits = (m_canvasW <= vpW);
    const int hRange = m_canvasW - vpW;
    const int center = hRange / 2;               // cho dat giua khung nhin khi lot-trong
    const int rMin = horizontalScrollBar()->minimum();
    const int rMax = horizontalScrollBar()->maximum();

    // Gia lap nguoi dung keo ngang lech tam (trong range).
    int userVal = qBound(rMin, center + 40, rMax);
    if (userVal == center) userVal = qBound(rMin, center - 40, rMax);
    horizontalScrollBar()->setValue(userVal);
    const int before = horizontalScrollBar()->value();
    updateScrollBars();                          // DUNG duong resizeEvent goi
    const int after = horizontalScrollBar()->value();
    // Cu (ep moi lan): se doi ve center bat cu khi nao canvas lot trong.
    const bool persistPass = (after == before);

    // Lan dau dat range (m_lastHRange==0) + lot-trong => PHAI canh giua.
    m_lastHRange = 0;
    updateScrollBars();
    const int firstCenter = horizontalScrollBar()->value();
    const bool centerPass = !fits || (firstCenter == center);

    // ── LOI B (0921): doi vpW (dong/mo sidebar hoac doi co cua so) ────────────────
    // Probe cu KHONG bat duoc vi no goi lai updateScrollBars voi vpW KHONG DOI.
    // (1) CHUA keo (dang o giua) + doi vpW => PHAI canh giua lai (lech tam < 2px).
    //     Luc nay m_lastHRange = hRange (<0) va value = center (tu khoi "lan dau").
    const int vpW2 = vpW + qMax(1, vpW / 3);
    viewport()->resize(vpW2, viewport()->height());
    updateScrollBars();
    const int hRange2 = m_canvasW - vpW2;
    const int center2 = hRange2 / 2;
    const int nowCentred = horizontalScrollBar()->value();
    const bool recentrePass = !fits || (qAbs(nowCentred - center2) < 2);

    // (2) DA keo lech tam + doi vpW => PHAI giu nguyen vi tri nguoi dung (khong ep ve giua).
    viewport()->resize(vpW, viewport()->height());   // ve co cu
    setZoom(zoom);
    m_lastHRange = hRange;                            // dang lot-trong, dang o giua
    const int dragVal = qBound(rMin, center + 60, rMax);   // keo lech tam
    horizontalScrollBar()->setValue(dragVal);
    updateScrollBars();                               // ghi nhan m_lastHRange, GIU drag
    viewport()->resize(vpW2, viewport()->height());
    updateScrollBars();
    const int nowDragged = horizontalScrollBar()->value();
    const bool keepPass = (qAbs(nowDragged - dragVal) < 2);

    QString rep;
    rep += QStringLiteral("SCROLLPERSIST zoom=%1 vpW=%2 canvasW=%3 fits=%4 rangeX=%5..%6\n")
               .arg(zoom, 0, 'f', 3).arg(vpW).arg(m_canvasW).arg(fits ? 1 : 0).arg(rMin).arg(rMax);
    rep += QStringLiteral("  [LOI 2] keo nguoi dung=%1 -> sau updateScrollBars=%2 (cu ep ve giua=%3)  %4\n")
               .arg(before).arg(after).arg(center).arg(persistPass ? "PASS" : "FAIL");
    rep += QStringLiteral("  [lan dau] m_lastHRange=0 -> sau updateScrollBars=%1 (giua=%2)  %3\n")
               .arg(firstCenter).arg(center).arg(centerPass ? "PASS" : "FAIL");
    rep += QStringLiteral("  [LOI B.1 chua keo] vpW %1->%2, value=%3 (giua moi=%4, lech=%5px)  %6\n")
               .arg(vpW).arg(vpW2).arg(nowCentred).arg(center2)
               .arg(qAbs(nowCentred - center2)).arg(recentrePass ? "PASS" : "FAIL");
    rep += QStringLiteral("  [LOI B.2 da keo] vpW %1->%2, keo=%3 -> giu=%4 (lech=%5px)  %6\n")
               .arg(vpW).arg(vpW2).arg(dragVal).arg(nowDragged)
               .arg(qAbs(nowDragged - dragVal)).arg(keepPass ? "PASS" : "FAIL");
    // Do luong LOI A da CHAP NHAN (khong sua): policy doi o dau ham nen o luot chuyen tiep
    // lot<->khong-lot range doc lech ~17px, roi tu lanh o lan resize/zoom ke tiep. Chi chay
    // khi argv co "real"; IN so do, KHONG tinh PASS/FAIL.
    if (probeRealWindow) {
        ContinuousView wv;
        wv.resize(1600, 1000);          // co cua so: viewport se bi thanh cuon an 17px
        wv.show();
        QCoreApplication::processEvents();
        wv.setDocument(m_doc, nullptr, nullptr, nullptr);
        QCoreApplication::processEvents();
        const int vpWr0 = wv.viewport()->width();
        // Doi luong: day trang = scrollY + vpH (rect page bottom TINH TU scrollY). Neu range
        // ghi thieu/dua 17px (LOI A) thi khi dung tro lai (fits=0) khong dat duoc dong cuoi
        // vao mep duoi => lech khac 0. So nay duoc CHAP NHAN, khong phai tieu chi nghiem thu.
        auto bottomResidual = [&]() {
            const int vpHl = wv.viewport()->height();
            return qAbs((wv.verticalScrollBar()->value() + vpHl) - wv.m_canvasH);
        };
        wv.setZoom(zoom * 0.30);       // LOT: range ep 0, viewport CAO hon (thanh an)
        QCoreApplication::processEvents();
        const int vpHrShrink = wv.viewport()->height();
        wv.setZoom(zoom);              // KHONG LOT: day la luot chuyen tiep
        QCoreApplication::processEvents();
        wv.scrollToPage(0);
        QCoreApplication::processEvents();
        const int vpHrGrow = wv.viewport()->height();
        wv.verticalScrollBar()->setValue(wv.verticalScrollBar()->maximum());
        const int botTransition = bottomResidual();
        wv.setZoom(zoom * 0.30);
        QCoreApplication::processEvents();
        wv.setZoom(zoom);
        QCoreApplication::processEvents();
        wv.scrollToPage(0);
        QCoreApplication::processEvents();
        wv.verticalScrollBar()->setValue(wv.verticalScrollBar()->maximum());
        const int botAgain = bottomResidual();
        rep += QStringLiteral("  [LOI A cua so that — CHAP NHAN] vpW=%1 vpH lot=%2 vpH khong-lot=%3 "
                              "| day sau chuyen tiep lech=%4px / %5px (tu lanh o lan sau, khong tinh PASS/FAIL)\n")
                   .arg(vpWr0).arg(vpHrShrink).arg(vpHrGrow).arg(botTransition).arg(botAgain);
    }
    return rep;
}

void ContinuousView::requestVisiblePages()
{{ QElapsedTimer _e; _e.start(); 
    { QElapsedTimer _e; _e.start(); 
        { QElapsedTimer _e; _e.start(); 
            
                if (!m_renderer || m_pageCount == 0) {
                    qDebug() << "[perf] cont SKIP requestVisiblePages reason=noDoc";
                    return;
                }
                if (m_zooming) {
                    qDebug() << "[cont] SKIP request — dang zoom";
                    return;
                }
            
                int primary = pageAtCenter();
                bool primaryChanged = (primary != m_primaryPage
                                       || qAbs(m_zoom - m_lastRequestZoom) >= 1e-9);
            
if (primaryChanged) {
                      // 🔴 VIỆC 2 (SPEC_SMOOTH_123 31/08) — thay huy SACH bang huy TUNG TRANG.
                      // Luc mo file bo cuc dung dan (0 -> N trang) nen pageAtCenter() nhay lien
                      // tuc (do 12 lan doi y trong vai giay dau). Ban cu goi cancelContinuous()
                      // moi lan doi — vua xoa hang doi VUA giet render DANG CHAY — nen app khong
                      // bao gio ve xong. Gio: chi huy nhung trang DANG RENDER ma KHONG con nhin
                      // thay (tra khe ve), trang con visible thi GIU. Phai lam CUNG LUC voi viec
                      // nang kMaxContinuousRenders (2 -> 6) — lam mot minh se hong: khong huy thi
                      // 2 khe cu bi trang cu chiem, moi trang moi bi tu choi (CEO da thu, 0 anh).
                      if (m_renderer) {
                          const bool zoomChanged = (qAbs(m_zoom - m_lastRequestZoom) >= 1e-9);
                          if (zoomChanged) {
                              // Doi ZOOM: anh cu sai do phan giai — phai giet that.
                              m_renderer->cancelContinuous();
                              qDebug().noquote() << "[cont] huy HET — zoomDoi=" << zoomChanged;
                          } else {
                              // Cuon (trang chinh doi): chi huy trang dang render khong con
                              // visible, giu trang con nhin thay (bo di khe chong nhau).
                              int nCancelled = 0;
                              for (int p = 0; p < m_pageCount; ++p) {
                                  if (!pageVisible(p) && m_renderer->isContinuousPageActive(p)) {
                                      m_renderer->cancelContinuousPage(p);
                                      m_pendingRenderScale.remove(p);
                                      m_continuousRequested.remove(p);
                                      ++nCancelled;
                                  }
                              }
                              qDebug().noquote() << "[cont] huy theo trang n=" << nCancelled
                                                 << "trangCu=" << m_primaryPage
                                                 << "conHien=" << (m_primaryPage >= 0 && m_primaryPage < m_pageCount
                                                                   && pageVisible(m_primaryPage));
                          }
                      }
                      m_pendingRenderScale.clear();
                      qDebug() << "[perf] cont primary=" << primary << "zoom=" << m_zoom;
                      m_primaryPage = primary;
                     m_lastRequestZoom = m_zoom;
                      m_primaryRequested = false;
                      m_retryTimer->setProperty("tries", 0);   // V2: luot xem trang moi = ngan sach thu lai moi
                      m_retryTimer->start();
                      syncUrgentPrimary();   // L18: trang moi dang cho anh => thumbnail phai nhuong
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
                            double maxResZoom = (m_renderer ? m_renderer->fullQCapPx(primary, maxDim)
                                                             : PdfRenderer::kFullRenderMaxPx) / qMax(maxDim, 1.0);
                            if (qAbs(storedZoom - m_zoom) < 1e-9
                                || (maxDim > 0 && storedZoom >= maxResZoom * 0.9)) {   // L31b: dung sai 0,9
                                // Already have a good-enough pixmap: exact zoom match or
                                // full-quality render usable at any zoom.
                                m_continuousRequested.remove(primary);
{ QElapsedTimer _q; _q.start(); requestNeighborPages(); const qint64 _n=_q.elapsed(); if(_n>15) qDebug().noquote()<<"[scrollms] requestNeighborPages ms="<<_n; }
                                goto evict;
                            }
                        }
                    }
                    if (vectorWillRender(primary)) {
                        qDebug() << "[cont] skip raster request (vector lo) page=" << primary;
{ QElapsedTimer _q; _q.start(); requestNeighborPages(); const qint64 _n=_q.elapsed(); if(_n>15) qDebug().noquote()<<"[scrollms] requestNeighborPages ms="<<_n; }
                        goto evict;
                    }
                    bool _hit; { QElapsedTimer _q; _q.start(); _hit = m_renderer->requestFromCacheOnlyForContinuous(primary, m_zoom); const qint64 _n=_q.elapsed(); if(_n>15) qDebug().noquote()<<"[scrollms] cacheProbe PRIMARY ms="<<_n; }
                    if (_hit) {
                        // Cache hit → image arrives synchronously via continuousPageReady
                        // handler, which calls requestNeighborPages for us.
                        m_continuousRequested.remove(primary);
                        // After cache, check if the returned image matches zoom exactly.
                        // If not (e.g. full-quality at different scale), we still accept it
                        // but the settle timer below handles re-render if needed.
                        auto zit = m_pageImageZoom.constFind(primary);
                        if (zit != m_pageImageZoom.constEnd()
                            && qAbs(zit.value() - m_zoom) >= 1e-9) {
                            // 🔴 LƯỢT 31b (reviewer mục 2): renderedScale giờ là kích thước
                            // THẬT của ảnh. Ảnh DƯỚI zoom cần (vd 357px mở lại, zoom 200%)
                            // chỉ là ảnh tạm ⇒ PHẢI lên lịch settle vẽ lại nét (renderer sẽ
                            // vượt cache mem vì memScale < cần). Ảnh >= zoom cần ⇒ đã nét.
                            if (zit.value() < m_zoom * 0.9) {
                                qDebug().noquote() << "[cont] cache-hit anh tam duoi zoom page=" << primary
                                                   << "scale=" << zit.value() << "zoom=" << m_zoom
                                                   << "— hen settle ve net";
                                m_contSettleTimer->start();
                            }
 { QElapsedTimer _q; _q.start(); requestNeighborPages(); const qint64 _n=_q.elapsed(); if(_n>15) qDebug().noquote()<<"[scrollms] requestNeighborPages ms="<<_n; }
                        }
                        goto evict;
                    }
            
                    // Step c: cache miss → record probe (skip re-probe) and start settle timer
                    m_cacheProbed[primary] = m_zoom;
                    qDebug() << "[perf] cont primary cache miss page=" << primary
                             << "— starting 400ms settle";
                    m_vecBuildArmed = false;   // dang cuon => cam dung vector
                    // 🔴 LƯỢT 33d (J): tab vua mo (doc moi gan) — khong co cuoc cuon
                    // nao de debounce ⇒ trang dau ve sau 1 ms thay vi 400 ms.
                    if (m_freshDocument) {
                        m_freshDocument = false;
                        qDebug() << "[perf] cont settle NON ly do=doc-moi page=" << primary;
                        m_contSettleTimer->start(1);
                    } else
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
                // 🔴 LƯỢT 31 (A3): báo renderer cửa sổ trang ĐANG HIỂN để evictCache
                // bảo vệ cả cửa sổ (±2), không chỉ m_currentPage — Continuous hiển
                // nhiều trang một lúc.
                if (m_renderer && first >= 0) m_renderer->setViewportRange(first, last);
                // Chi giu vung nhin +-1 trang. Trang A1 render o 4000 px ton ~43 MB/anh, giu +-4
                // la ~450 MB chi rieng kho nay => tien trinh phinh qua 2 GB va Windows bat dau trao
                // bo nho ra dia, do chinh la cai "Not Responding" khi cuon.
                const int kKeepMargin = 1;
                int keepFirst = qMax(0, first - kKeepMargin);
                int keepLast  = qMin(m_pageCount - 1, last + kKeepMargin);
                for (auto it = m_pageImages.begin(); it != m_pageImages.end(); ) {
                    if (it.key() < keepFirst || it.key() > keepLast) {
                        m_continuousRequested.remove(it.key());
                        m_pendingRenderScale.remove(it.key());
                        m_pageImageZoom.remove(it.key());
                        ++m_contStoreGen[it.key()];   // LƯỢT 37: trang bi don kho ⇒ thu nhỏ nền đang
                                                      // bay cho no khong duoc ghi nguoc lai (cung
                                                      // nguyên nhân như doi tai lieu).
                        it = m_pageImages.erase(it);
                    } else {
                        ++it;
                    }
                }
                // 🔴 VIỆC 3 (SPEC_SMOOTH_123 31/08): KHONG BAO GIO ve o trong.
                // Trang visible chua co noi dung (chua co anh to, chua co lop vector,
                // chua co ban tho) → xin THUMBNAIL ngay (bac 0, re ~200-500ms) de ve
                // de. Khong phai doi render day du (1.300-4.900 ms/trang) moi co hinh.
                for (int i = first; i <= last && i >= 0; ++i) {
                    if (i < 0 || i >= m_pageCount) continue;
                    const bool hasImg = m_pageImages.contains(i);
                    const bool hasVector = (m_vecStore->constFind(i) != m_vecStore->constEnd()
                                            && *m_vecStore->constFind(i)
                                            && (*m_vecStore->constFind(i))->isReady()
                                            && (*m_vecStore->constFind(i))->isComplete()
                                            && !rasterOnlyApplies(i));
                    const bool hasLow = m_pageLowRes.contains(i);
                    if (!hasImg && !hasVector && !hasLow)
                        emit needThumbnail(i);
                }
        const qint64 _m=_e.elapsed(); if(_m>20) qDebug().noquote()<<"[scrollms] requestVisiblePages ms="<<_m;
        }
    const qint64 _m=_e.elapsed(); if(_m>20) qDebug().noquote()<<"[scrollms] requestVisiblePages ms="<<_m;
    }
const qint64 _m=_e.elapsed(); if(_m>20) qDebug().noquote()<<"[scrollms] requestVisiblePages ms="<<_m;
}}

void ContinuousView::requestNeighborPages()
{
    if (m_zooming) {
        qDebug() << "[cont] SKIP requestNeighborPages — dang zoom";
        return;
    }
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
bool _hit; { QElapsedTimer _q; _q.start(); _hit = m_renderer->requestFromCacheOnlyForContinuous(i, m_zoom); const qint64 _n=_q.elapsed(); if(_n>15) qDebug().noquote()<<"[scrollms] cacheProbe NEIGHBOR ms="<<_n; }
         if (_hit)
             continue;
        if (!m_continuousRequested.contains(i)) {
            qDebug() << "[perf] cont neighbor request page=" << i << "zoom=" << m_zoom;
            requestContinuousRender(i, m_zoom);
            m_continuousRequested.insert(i);
        }
    }
}

void ContinuousView::syncUrgentPrimary()
{
    // 🔴 0928 LƯỢT 18 (VIỆC 2): cho hang doi thumbnail biet trang nao cua TAB DANG
    // XEM dang cho anh nen — do log r17: thumbnail trang 3 giu khoa 1335 ms
    // (FPDF_LoadPage pool handle) dung luc render trang 3 can khoa => no dung chai
    // tu lat 3797 ms sang 5233 ms. Thu truong so: day la SO TRANG, khong kem tai
    // lieu — an toan vi hang doi thumbnail chi chay cho tab dang xem (L16 dong bang
    // tab nen), va gia tri cu bi tran nhoi doi 5 s o phia worker.
    auto& g = g_pdfiumUrgentPage();
    // vectorWillRender: trang do lop vector ve, raster KHONG bao gio ve => khong duoc
    // gi thumbnail mai mai (bang 5 s o worker chi la phep bua, day moi la dung).
    const bool waiting = m_primaryPage >= 0 && !m_pageImages.contains(m_primaryPage)
                         && !vectorWillRender(m_primaryPage);
    if (isVisible()) {
        if (waiting)
            g_pdfiumUrgentPageDeadline().store(QDateTime::currentMSecsSinceEpoch() + 5000,
                                               std::memory_order_release);   // L19: trần 5 s TINH TU LUC primary bat dau cho
        g.store(waiting ? m_primaryPage : -1, std::memory_order_release);
        // 🔴 LƯỢT 33f (mục 2): ghi token doc — cửa sổ này là của tab HIỆN HÀNH,
        // trang hiển thị cùng doc KHÔNG được nhường cho nó (chỉ tab khác nhường).
        if (waiting)
            g_pdfiumUrgentDoc().store(m_doc ? (uintptr_t)m_doc->raw() : 0,
                                      std::memory_order_release);
        return;
    }
    // View an (tab nen): chi don dung phan cua minh, khong lot cua tab dang nhin.
    if (waiting) return;
    int expect = m_primaryPage;
    g.compare_exchange_strong(expect, -1, std::memory_order_acq_rel);
}

void ContinuousView::requestContinuousRender(int page, double scale)
{
    if (!m_renderer || !m_doc) return;
    auto it = m_pendingRenderScale.constFind(page);
    if (it != m_pendingRenderScale.constEnd()) {
        const double pending = it.value();
        if (qAbs(pending - scale) < 1e-6) {
            qDebug().noquote() << QString("[cont] SKIP duplicate page=%1 scale=%2")
                                  .arg(page).arg(scale, 0, 'g', 10);
            return;
        }
        // Zoom doi trong luc dang render: huy ban yeu cau cu truoc (cung luc
        // huy het render continuous cu dang bay — khong de chong them ban moi
        // len ban cu). Sau do moi dat yeu cau moi theo scale moi.
        qDebug().noquote() << QString("[cont] zoom change cancels in-flight render page= %1 oldScale= %2 newScale= %3")
                              .arg(page).arg(pending, 0, 'g', 10).arg(scale, 0, 'g', 10);
        m_renderer->cancelContinuous();
        m_pendingRenderScale.clear();
    }
    m_pendingRenderScale.insert(page, scale);
    // 🔴 SUA 2026-08-31: khung nhin ve o `pageWpt * m_zoom * devicePixelRatioF()` (xem
    // `needW` dong ~306/424) nhung truoc day dat hang render CHI voi `scale` — thieu han
    // he so DPR => render 273px trong khi can 547px => phong len thanh MO CAM.
    // Truyen dung do phan giai thiet bi ma khung nhin se ve.
    // VIỆC 2: uu tien — trang dang xem + 1 trang moi phia lam TRUOC (prio=0), con lai sau.
    const int prio = (m_primaryPage >= 0 && page >= m_primaryPage - 1 && page <= m_primaryPage + 1) ? 0 : 2;
    m_renderer->requestPageForContinuous(page, scale * devicePixelRatioF(), prio);
}

bool ContinuousView::vectorWillRender(int pg) const
{
    // Trang chi ve raster (markup khong overlay duoc): lop vector khong duoc giu
    // chan raster, mac du buildPrimaryVectorLayer co the da dung lai lop do.
    if (!rasterOnlyApplies(pg))
        return false;
    // Lop DA san sang → chac chan ve bang vector.
    auto it = m_vecStore->constFind(pg);
    if (it != m_vecStore->constEnd() && *it && (*it)->isReady() && (*it)->isComplete())
        return true;
    // Dang dung → chi duoc bo raster trong mot HAN NGAN. Qua han thi PHAI cho raster
    // chay, khong duoc cho vo han (day chinh la loi lam Continuous trang tron).
    const auto bs = m_vecBuildStart.constFind(pg);
    if (bs != m_vecBuildStart.constEnd()) {
        const qint64 age = QDateTime::currentMSecsSinceEpoch() - *bs;
        if (age < kVectorGraceMs) return true;
    }
    return false;
}

// TORREADER_CONT_RASTER=1 -> Continuous ve HOAN TOAN bang raster, bo han nhanh vector.
// Y tuong cua owner 31/08: Single da co san 3 lop (thumbnail / raster / vector GPU); raster
// von rat muot va nhanh, chi kem net o zoom 400-500%. Continuous chi can raster + xep N trang.
// Bo nhanh vector cung chan duong dung lop vector trong KHO CHUNG (m_vecStore ->
// DocTab::vecLayers) — chinh la thu
// da gay "hai kho vector" va markup mat trong Continuous.
bool ContinuousView::contRasterOnlyEnv()
{
    static const bool on = !qEnvironmentVariableIsEmpty("TORREADER_CONT_RASTER");
    return on;
}

bool ContinuousView::trangCanRaster(int pg) const
{
    if (!m_visualsSrc) return false;
    auto it = m_visualsSrc->constFind(pg);
    if (it == m_visualsSrc->constEnd()) return false;
    bool coThuOverlayKhongVe = false;
    for (const AnnotVisual& av : it.value())
        if (!av.paintByOverlay) { coThuOverlayKhongVe = true; break; }
    if (!coThuOverlayKhongVe) return false;
    // 🔴 2026-09-01: co LOP BU san sang thi khong can hy sinh nen vector nua — lop bu ve
    // chu thich (do PDFium dung, dung phong dung nen) de len tren nen vector.
    auto fl = m_fgnLayers.constFind(pg);
    if (fl != m_fgnLayers.constEnd() && *fl && (*fl)->isReady()
        && (*fl)->pageIndex() == pg && !(*fl)->image().isNull())
        return false;
    return true;
}

bool ContinuousView::vectorBuildBlocked(int pg) const
{
    // CAU TRUC thuan: chan dung/xdp lop vector CHỈ vi env raster hoac trang nang bi ep
    // raster. KHONG nhuc den chu thich — nen vector phai dung duoc cho ca trang co comment
    // (comment do lop comment ve de len). Day la thu giup trang comment thoat ket raster.
    if (contRasterOnlyEnv()) return true;
    if (!m_rasterOnlyPages.contains(pg)) return false;
    if (m_renderer && m_renderer->pageObjectCount(pg) > kHeavyObjectThreshold)
        return false;
    return true;
}

bool ContinuousView::rasterOnlyApplies(int pg) const
{
    // ── LUAT VAY (owner chot 02/09, "mot nguon nhieu lop") — AP DUNG GIONG HET PdfGpuView.
    // Nen vector duoc dung khi: (a) co nen vector (khong bi chan cau truc) VA (b) hoac
    // overlay ve duoc het chu thich, hoac lop comment da san sang ve chu thich de len.
    // Trang co comment ma lop comment CHUA xong => tam ve raster (trangCanRaster true);
    // khi lop comment xong, setForeignAnnotLayer() bao ve lai => chuyen sang vector.
    // (Da go bo cay "cong" m_vecAnnotSafe cu: no ep trang co comment o lai raster VINH VIEN
    //  ma khong biet lop comment da san sang — chinh la nguyen nhan vectorPages=0.)
    if (vectorBuildBlocked(pg)) return true;
    return trangCanRaster(pg);
}

void ContinuousView::ensureVectorLayers()
{
    if (contRasterOnlyEnv()) return;   // raster-only: khong dung lop vector nao ca
    // 🔴 GO BO 2026-08-31: dong `if (m_fastMode) return;` chan CHINH duong lam View Fast nhanh.
    // Owner hoi dung cho: "sao khong xem Single xu ly trang quai vat the nao ma nhanh vay".
    // Cau tra loi: Single dung LOP VECTOR GPU. Con View Fast thi tu loai bo no bang dong return
    // nay, chi con duong raster => phai ve lai 2,54 trieu doi tuong moi lan (do duoc 6-44 giay).
    // Ham nay CHI NAP tu cache .torvec (~46 ms, KHONG cham khoa PDFium) — xem TASK-START ben duoi;
    // khong co cache thi bo cuoc ngay, de raster lo. Nen no RE va AN TOAN, khong co ly do chan.
    // (Dong nay khong co trong cac ban luu 30/08 — them gan day, va chan nham ca duong NAP.)
    // Che do lien tuc CHI NAP lop vector tu cache .torvec (29-50 ms, khong dung PDFium).
    // KHONG BAO GIO dung moi o day: mot luot build om khoa PDFium vai giay va lam chet
    // ca duong raster (do 2026-08-23). Trang chua co cache thi cu de raster ve.

    if (!m_doc || m_pageCount == 0) return;
    int center = pageAtCenter();
    // 🔴🔴 2026-09-20: DOI TRANG TRUNG TAM = xoa dau "da thu, khong co cache".
    // Ban va truoc bo quen dieu kien nay: mot trang lan can bi danh dau luc no chua phai
    // trang trung tam (choPhepDung=0, khong co .torvec) se mang dau MAI MAI, ke ca khi
    // nguoi dung cuon toi dung no ⇒ trang DANG XEM khong bao gio duoc dung lop vector.
    // Chi xoa khi center THAT SU doi, nen khi dung yen khong co lan clear nao (khong quay
    // lai vong lap ban). Cac lan clear khac: setZoom, settle timer, anh nen ve sau.
    if (center != m_vecTriedCenter) {
        m_vecTriedNoCache.clear();
        m_vecTriedCenter = center;
    }
    int lo = qMax(0, center - kKeepRadius);
    int hi = qMin(m_pageCount - 1, center + kKeepRadius);
    qDebug().noquote() << QString("[contvec] ENTER center=%1 lo=%2 hi=%3 layers=%4 building=%5 poolActive=%6 poolMax=%7")
                          .arg(center).arg(lo).arg(hi).arg(m_vecStore->size())
                          .arg(m_vecBuilding.size())
                          .arg(QThreadPool::globalInstance()->activeThreadCount())
                          .arg(QThreadPool::globalInstance()->maxThreadCount());
for (int pg = lo; pg <= hi; ++pg) {
        if (vectorBuildBlocked(pg)) {
            qDebug() << "[contvec] rasterOnly pg=" << pg;
            continue;
        }
        if (m_rasterOnlyPages.contains(pg))
            qDebug().noquote() << "[contvec] rasterOnly BO QUA - trang nang objects="
                               << (m_renderer ? m_renderer->pageObjectCount(pg) : 0)
                               << "pg=" << pg << "(uu tien hien trang)";
        if (m_vecStore->contains(pg) || m_vecBuilding.contains(pg)) {
            qDebug().noquote() << QString("[contvec] skip pg=%1 hasLayer=%2 isBuilding=%3")
                                  .arg(pg).arg(m_vecStore->contains(pg)).arg(m_vecBuilding.contains(pg));
            continue;
        }
        // 2026-09-20: KHONG in o day — dong nay chay MOI trang bi skip MOI nhip ensureVectorLayers
        // tren luong giao dien ⇒ phinh nhat ky. Chi in dung MOT LAN luc ghi dau (xem ben duoi).
        if (m_vecTriedNoCache.contains(pg)) continue;
        // 🔴 2026-09-20: chot `choPhepDung` PHAI chay TRUOC khi ghi m_vecBuilding/m_vecBuildStart
        // va cap phat QFutureWatcher. Neu dat sau, nhanh `continue` (khong co .torvec tren dia)
        // bo lai pg trong m_vecBuilding VINH VIEN: lambda `finished` la cho DUY NHAT xoa no,
        // ma lambda do khong bao gio chay vi w->setFuture() chua duoc goi
        // => trang bi chan dung lop vector mai mai (anh nhoe nang hon truoc khi va).
        const QString keyPath = m_vecKeyPath;
        const quint64 keyHash = m_vecKeyHash;
        // 🔴 2026-08-31: CHI trang TRUNG TAM moi duoc DUNG moi khi khong co cache.
        // Cac trang khac chi duoc NAP tu cache; khong co thi de raster lo.
        // Ly do gioi han 1 trang: moi lop vector cua trang nang ton 4,4 giay dung va 135 MB.
        // Cho ca 5 trang lan can cung dung la 22 giay + ~700 MB — khong kham noi.
        // 🔴🔴 SIET 2026-08-31 — DUNG THU TU: DAY DU-MA-MO TRUOC, NET SAU.
        // Do duoc tren file mo SACH (khong con dem): ban truoc do lao vao dung vector cho trang
        // quai vat chi 32 ms sau khi mo file, dung mat 6.226 ms, va thumbnail trang do mai
        // +6.710 ms moi len => nguoi dung ngoi nhin o TRONG gan 7 giay.
        // Nghich ly: chinh ban va "chon dung trang dang xem" gay ra — truoc do app tuong trang
        // chinh la trang 2 (trang nhe) nen thumbnail chay ve nhanh; nay no nhan dung trang nang.
        // ⇒ Chi dung vector khi: (a) cuon DA DUNG HAN (m_vecBuildArmed) VA (b) trang do DA co
        //    anh day du de nhin (thumbnail hoac anh raster). Net la viec LAM SAU, khong duoc
        //    tranh cho voi viec cho nguoi dung thay noi dung.
        const bool daCoGiDeNhin = m_pageLowRes.contains(pg) || m_pageImages.contains(pg);
        const bool choPhepDung = (pg == center) && m_vecBuildArmed && daCoGiDeNhin;
        if (!choPhepDung) {
            const QString cp = VectorCache::pathFor(keyPath, keyHash, pg);
            if (cp.isEmpty() || !QFile::exists(cp)) {
                qDebug().noquote() << "[contvec] SKIP pg=" << pg << "reason=noCacheFile choPhepDung=0";
                m_vecTriedNoCache.insert(pg);
                continue;
            }
        }
        m_vecBuilding.insert(pg);
        m_vecBuildStart.insert(pg, QDateTime::currentMSecsSinceEpoch());
        qDebug().noquote() << "[contvec] SCHEDULE pg=" << pg;
        auto layer = std::make_shared<VectorLayer>();
        auto* w = new QFutureWatcher<bool>(this);
        const int myGen = m_vecGen;
        connect(w, &QFutureWatcher<bool>::finished, this, [this, w, pg, layer, myGen] {
            if (myGen != m_vecGen) {
                w->deleteLater();
                m_vecBuilding.remove(pg);
                m_vecBuildStart.remove(pg);
                m_vecTriedNoCache.remove(pg);
                return;
            }
            qDebug().noquote() << "[contvec] FINISHED pg=" << pg << " result=" << w->result();
            w->deleteLater();
            m_vecBuilding.remove(pg);
            m_vecBuildStart.remove(pg);
            if (w->result()) { m_vecStore->insert(pg, layer); m_vecTriedNoCache.remove(pg); }
            else { m_vecTriedNoCache.insert(pg); }
            qDebug().noquote() << "[cont] vecLayer" << (w->result() ? "READY" : "SKIP")
                               << "page=" << pg << "cached=" << m_vecStore->size();
            // Noi dung trang da san — bao MainWindow truoc (de push visuals + lay lop bu).
            if (w->result()) needAnnotVisuals(pg);
            viewport()->update();
        });
        FPDF_DOCUMENT docRaw = (m_doc && m_doc->isOpen()) ? m_doc->raw() : nullptr;
        // 🔴 0928 LƯỢT 22: token ĐĂNG KÝ LÚC SPAWN (UI thread) + RAII move —
        // beginClose thấy cả task còn xếp hàng trong m_vecPool.
        // 0928 LƯỢT 14: token sổ việc nền — PdfDocument::close()
        // chờ token này về 0 TRƯỚC FPDF_CloseDocument (4/4 minidump
        // LƯỢT 13 chết ở đúng chuỗi này).
        trdoc::Task task(docRaw, "VectorLayer::build/cont");
        w->setFuture(QtConcurrent::run(&m_vecPool, [layer, pg, keyPath, keyHash, choPhepDung, docRaw, this,
                                                    task = std::move(task)] {        { QElapsedTimer _e; _e.start();
                    { QElapsedTimer _e; _e.start();

                                    QElapsedTimer _t; _t.start();
                                    qDebug().noquote() << "[contvec] TASK-START pg=" << pg
                                                       << "keyPath=" << (keyPath.isEmpty() ? QStringLiteral("(RONG)") : keyPath)
                                                       << "keyHash=" << QString::number(keyHash, 16);
                                    // (thân lambda cũ khai báo Task tại đây — đã dời lên lúc spawn)
                                    // 1. Thu cache .torvec truoc: nap ~46 ms thay vi dung lai het vai giay.
                                    //    KHONG can khoa PDFium vi khong dong toi tai lieu.
                                    if (!keyPath.isEmpty() && VectorCache::tryLoad(*layer, keyPath, keyHash, pg)) {
                                        qDebug().noquote() << "[contvec] TASK-END pg=" << pg
                                                           << " ok= true src=cache ms=" << _t.elapsed();
                                        return true;
                                    }
                                    // 🔴 2026-08-31: khong co cache thi TRANG TRUNG TAM duoc DUNG moi.
                                    // Truoc day luon bo cuoc => Continuous KHONG BAO GIO co duong vector,
                                    // nen trang quai vat phai di raster (6-44 giay, lap di lap lai).
                                    // Chi dung cho MOT trang tai mot thoi diem, va chi khi cuon DA DUNG.
                                    // 🔴 2026-09-20: cau cu ghi "ham nay duoc goi tu contSettleTimer"
                                    // la SAI — truoc ban va, ensureVectorLayers() chi duoc goi tu
                                    // m_scrollTimer (~80 ms) va resizeEvent; contSettleTimer chi bat
                                    // m_vecBuildArmed + clear dau ma khong goi lai, nen settle xong
                                    // khong ai xep lich dung lop ⇒ trang nang chua cache dung yen mai
                                    // nhoe. Nay contSettleTimer da GOI ensureVectorLayers() ngay sau
                                    // vong needAnnotVisuals (tim m_contSettleTimer), nen cau nay moi dung.
                                    if (choPhepDung && docRaw) {
                                        // 0928 LUOT 13: vet huy theo lat — m_vecGen tang moi
                                        // lan doi trang trung tam; trang da roi o rong o giua
                                        // luc cuon thi build dung lai la viec thua.
                                        // CHI doc moc thoi (atomic); KHONG doc m_primaryPage
                                        // o luong nay — no la int thuan, doc la race moi.
                                        // Lua chon "trang nao dang xem" da nam trong choPhepDung
                                        // va duoc kiem lai tren luong giao dien o lambda finished.
                                        const int myGenW = m_vecGen.load(std::memory_order_acquire);
                                        const auto huy = [this, myGenW, &task] {
                                            return m_vecGen.load(std::memory_order_acquire) != myGenW
                                                || task.cancelled();
                                        };
                                        qDebug().noquote() << "[contvec] DUNG MOI pg=" << pg << " (trang trung tam)";
                                        const bool ok = layer->build(docRaw, pg, huy);
                                        qDebug().noquote() << "[contvec] TASK-END pg=" << pg
                                                           << " ok=" << ok << " src=build ms=" << _t.elapsed();
                                        return ok;
                                    }
                                    qDebug().noquote() << "[contvec] TASK-END pg=" << pg << " ok= false src=nocache ms=" << _t.elapsed();
                                    return false;
                                
                    const qint64 _m=_e.elapsed(); if(_m>20) qDebug().noquote()<<"[scrollms] needAnnotVisuals ms="<<_m;
                    }
        const qint64 _m=_e.elapsed(); if(_m>20) qDebug().noquote()<<"[scrollms] needAnnotVisuals ms="<<_m;
        }}));
    }
for (auto it = m_vecStore->begin(); it != m_vecStore->end(); ) {
            const int dist = qAbs(it.key() - center);
            // Viec C: trang nang phai DUNG MOI vai giay moi hien lai — noi long ban
            // kinh giu lop (dist > 3). Trang thong thuong van giu dist > 1 nhu cu.
            const int keep = (m_renderer
                              && m_renderer->pageObjectCount(it.key()) > kHeavyObjectThreshold)
                                 ? kHeavyKeepRadius : kKeepRadius;
            if (dist > keep) {
                qDebug().noquote() << "[cont] vecLayer EVICT page=" << it.key()
                                   << "dist=" << dist << ">" << keep
                                   << "remaining=" << m_vecStore->size() - 1;
                qDebug().noquote() << "[contvec] EVICT pg=" << it.key();
                it = m_vecStore->erase(it);
            } else {
                if (keep > kKeepRadius)
                    qDebug().noquote() << "[cont] vecLayer GIU trang nang pg=" << it.key()
                                       << "dist=" << dist;
                ++it;
            }
}
        evictForeignLayers();
        // Evict building and buildStart outside the window
        for (auto it = m_vecBuilding.begin(); it != m_vecBuilding.end(); ) {
            if (qAbs(*it - center) > kKeepRadius) {
                qDebug().noquote() << "[contvec] DROP-STALE pg=" << *it;
                m_vecBuildStart.remove(*it);
                it = m_vecBuilding.erase(it);
            } else {
                ++it;
            }
}
    }

void ContinuousView::buildPrimaryVectorLayer()
{
    if (m_fastMode) return;
    if (!m_doc || m_pageCount == 0) return;
    const int pg = m_primaryPage;
    if (pg < 0 || pg >= m_pageCount) return;
    if (m_vecStore->contains(pg) || m_vecBuilding.contains(pg)) return;
    if (m_vecKeyPath.isEmpty()) return;

    const int objs = m_renderer ? m_renderer->pageObjectCount(pg) : 0;

    m_vecBuilding.insert(pg);
    m_vecBuildStart.insert(pg, QDateTime::currentMSecsSinceEpoch());
    qDebug().noquote() << "[contvec] BUILD-PRIMARY pg=" << pg;

    auto layer = std::make_shared<VectorLayer>();
    auto* w = new QFutureWatcher<bool>(this);
    const int myGen = m_vecGen;
    connect(w, &QFutureWatcher<bool>::finished, this, [this, w, pg, layer, myGen] {
        if (myGen != m_vecGen) { w->deleteLater(); m_vecBuilding.remove(pg); m_vecBuildStart.remove(pg); return; }
        w->deleteLater();
        m_vecBuilding.remove(pg);
        m_vecBuildStart.remove(pg);
        if (w->result() && pg == m_primaryPage) {
            m_vecStore->insert(pg, layer);
            qDebug().noquote() << "[contvec] BUILD-PRIMARY READY pg=" << pg;
            needAnnotVisuals(pg);
            viewport()->update();
        }
    });
    FPDF_DOCUMENT d = m_doc->raw();
    const QString keyPath = m_vecKeyPath;
    const quint64 keyHash = m_vecKeyHash;
    // 🔴 0928 LƯỢT 22: token ĐĂNG KÝ LÚC SPAWN + RAII move (xem ensureVectorLayers).
    // 0928 LƯỢT 14: token sổ việc nền — xem ensureVectorLayers().
    trdoc::Task task(d, "VectorLayer::build/contPrimary");
    w->setFuture(QtConcurrent::run(&m_vecPool, [layer, d, pg, keyPath, keyHash, objs, this, myGen,
                                                task = std::move(task)] {
        QElapsedTimer t; t.start();
        // KHONG duoc khoa PDFium ben ngoai o day: VectorLayer::build() tu khoa va NHA
        // theo lat (xem VectorLayer.cpp dau ham). Om khoa ben ngoai la vo hieu hoa co
        // che nha-theo-lat => giu khoa PDFium vai giay => chet duong raster.
        // Viec A: TACH hai duong. Trang nang van duoc NAP CACHE .torvec (96 ms, khong
        // dong toi PDFium) — chi CAM DUNG MOI (vai chuc giay + om khoa).
        if (!keyPath.isEmpty() && VectorCache::tryLoad(*layer, keyPath, keyHash, pg)) {
            if (objs > kHeavyObjectThreshold)
                qDebug().noquote() << "[contvec] HEAVY nhung co cache -> nap pg=" << pg;
            qDebug().noquote() << "[contvec] BUILD-PRIMARY DONE pg=" << pg << " ok= true src=cache ms=" << t.elapsed();
            return true;
        }
        if (objs > kHeavyObjectThreshold) {
            // Trang nang MA KHONG co cache: bo cuoc, de raster/lowres lo.
            // Chi ap SKIP-HEAVY cho truong hop phai DUNG MOI — khong duoc build.
            qDebug().noquote() << "[contvec] SKIP-HEAVY pg=" << pg << " objects=" << objs;
            return false;
        }
        // 0928: trang chinh da bi bo roi trong khi build ⇒ dung o ranh giat lat.
        const auto huy = [this, myGen, &task] {
            return m_vecGen.load(std::memory_order_acquire) != myGen
                || task.cancelled();
        };
        const bool ok = layer->build(d, pg, huy);
        if (ok) VectorCache::trySave(*layer, keyPath, keyHash, pg);
        qDebug().noquote() << "[contvec] BUILD-PRIMARY DONE pg=" << pg << " ok=" << ok << " ms=" << t.elapsed();
        return ok;
    }));
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

// 🔴🔴 BAT BIEN 2026-09-01 — "NEN VECTOR KHONG DUOC NUOT MARKUP".
// Lop vector dung tu NOI DUNG TRANG; annotation KHONG nam trong do. Khi overlay ve duoc
// het markup (overlayCapable) thi khong sao. Nhung khi overlayCapable=false, thiet ke cu
// trong cay vao ANH RASTER de ve markup (`pagesNeedGenerate`) — ma o nen vector thi
// KHONG CO raster nao ca ⇒ markup BIEN MAT. Lop vector dung xong luc nao thi mat luc do,
// va zoom chinh la thu kich hoat dung vector ⇒ trieu chung "mat markup khi zoom" o CA
// Single lan Continuous. ⇒ Trang nao chua an toan thi O LAI voi raster.
void ContinuousView::setVectorAnnotSafe(int page, bool safe) {
    if (m_vecAnnotSafe.value(page, true) == safe) return;
    m_vecAnnotSafe[page] = safe;
    qDebug().noquote() << "[vector] cont trang" << page << (safe ? "AN TOAN" : "KHONG an toan")
                       << "cho nen vector (markup)";
    viewport()->update();
}

void ContinuousView::setAnnotVisualsForPage(int page, const QList<AnnotVisual>&) {
    // LÁT A 09/02: khong con ban chép rieng — visuals da nam trong kho duy nhat
    // cua tai lieu (DocTab::visualsCache). Day chi la lenh ve lai.
    Q_UNUSED(page);
    viewport()->update();
}

void ContinuousView::setPageRasterOnly(int page, bool on) {
    if (on && m_renderer && m_renderer->pageObjectCount(page) > kHeavyObjectThreshold) {
        // Trang nang (>= nguong SKIP-HEAVY): raster khong bao gio ve xong, phai dung lop
        // vector (tu cache .torvec). KHONG doi rasterOnly cho trang nay.
        qDebug().noquote() << "[contvec] rasterOnly BO QUA - trang nang objects="
                           << m_renderer->pageObjectCount(page) << "pg=" << page
                           << "(uu tien hien trang)";
        m_rasterOnlyPages.remove(page);
        viewport()->update();
        return;
    }
    if (on) m_rasterOnlyPages.insert(page);
    else m_rasterOnlyPages.remove(page);
    // ⭐ Khong giai phong tai nguyen GPU co the dang nam trong khung hinh dang ve —
    // chi danh dau de cho ve bo qua, de co che EVICT san co don. Xoa layer giua
    // chung khi GPU renderer con giu tham chieu/uid ⇒ access violation Qt6Gui.dll.
    viewport()->update();
}

void ContinuousView::setForeignAnnotLayer(int page, std::shared_ptr<ForeignAnnotLayer> layer) {
    if (layer && layer->pageIndex() != page) {
        qDebug().noquote() << "[fgnlayer] Continuous set mismatch layerPage="
                           << layer->pageIndex() << "want=" << page;
        return;
    }
    if (layer && (!layer->isReady() || layer->image().isNull())) {
        // Lop bu chua co img hop le — giu old (neu co) de khong lam mat markup dang dung.
        qDebug().noquote() << "[fgnlayer] Continuous keep-old page=" << page
                           << "newReady=" << layer->isReady();
        return;
    }
    if (layer)
        m_fgnLayers.insert(page, std::move(layer));
    else
        m_fgnLayers.remove(page);
    // CHOT 4 (doi lop tho => xoa vung sac net cu): vung cu thuoc lop CU, de lai la chong
    // hinh sai. Vung se duoc dung lai theo lop moi o nhip sharpTimer ke tiep.
    clearForeignAnnotRegion();
    evictForeignLayers();
    // Lop tho vua san sang ma nhip lam net gan nhat da chay TRUOC do (lop chua kip ready)
    // => khong co su kien nao keo no quay lai. Hen lai mot nhip de vung sac net duoc xin.
    if (layer && m_sharpTimer) m_sharpTimer->start();
    viewport()->update();
}

// Vung sac net chu thich ngoai (2026-09-21) — xem ContinuousView.h.
void ContinuousView::clearForeignAnnotRegion()
{
    m_fgnRegPage  = -1;
    m_fgnRegScale = 0.0;
    m_fgnRegRect  = QRect();
    m_fgnRegImg   = QImage();
    m_fgnRegFailPage  = -1;
    m_fgnRegFailScale = 0.0;
    // KHONG dong m_fgnRegionBuilding: tac vu dang bay se tu tra ve qua
    // setForeignAnnotRegion() va mo chot. Dong o day la mo duong cho 2 build chong nhau.
}

void ContinuousView::setForeignAnnotRegion(int page, double scale, QRect regionPx, const QImage& img)
{
    // CHOT 3: moi ket thuc (thanh cong hay that bai) deu mo chot chong-chong-viec.
    m_fgnRegionBuilding = false;
    if (img.isNull()) {
        // That bai (huy / alloc): ghi moc da-thu de khong xep lai vo han o cung zoom.
        m_fgnRegFailPage  = page;
        m_fgnRegFailScale = scale;
        return;
    }
    if (qAbs(scale - m_zoom) > 1e-6) return;   // zoom da doi — vung cu, bo
    m_fgnRegPage  = page;
    m_fgnRegScale = scale;
    m_fgnRegRect  = regionPx;
    m_fgnRegImg   = img;
    m_fgnRegFailPage = -1;
    viewport()->update();
}

// Tinh vung nhin cua TRANG TRUNG TAM roi xin mot vung sac net cho lop chu thich ngoai cua
// trang do, theo DUNG zoom hien tai. Gioi han 1 trang/luot (gioi han spec): khong bung cho
// ca 5 trang lan can. Chi chay khi du 4 chot: hasForeign + base vector + chong chong viec
// (m_fgnRegionBuilding) + vung cu duoc xoa khi doi lop tho (clearForeignAnnotRegion).
void ContinuousView::requestForeignAnnotRegion()
{
    if (contRasterOnlyEnv()) return;
    if (m_fgnRegionBuilding) return;                 // chot chong-chong-viec
    if (!m_doc || !m_renderer || m_pageCount == 0) return;
    int pg = m_primaryPage;
    if (pg < 0 || pg >= m_pageCount) pg = pageAtCenter();
    if (pg < 0 || pg >= m_pageCount) return;
    // CHOT 1 — co lop bu chu thich ngoai cho dung trang nay.
    auto fit = m_fgnLayers.constFind(pg);
    if (fit == m_fgnLayers.constEnd() || !*fit || !(*fit)->isReady()
        || (*fit)->pageIndex() != pg || (*fit)->image().isNull())
        return;
    // CHOT 2 — nen phai la VECTOR (nen raster da nung san annotation, khong can bu).
    auto vit = m_vecStore->constFind(pg);
    if (vit == m_vecStore->constEnd() || !*vit || !(*vit)->isReady()
        || !(*vit)->isComplete() || rasterOnlyApplies(pg))
        return;
    if (pg >= m_pageSizePt.size()) return;
    // Vung nhin ∩ trang, toa do px cua trang tai scale = m_zoom (giong pageW/pageH).
    const int scrollX = horizontalScrollBar()->value();
    const int scrollY = verticalScrollBar()->value();
    const int vpW = viewport()->width();
    const int vpH = viewport()->height();
    const int pL = pageLeftX(pg), pT = pageTopY(pg);
    const int pW = pageW(pg), pH = pageH(pg);
    const int visL = qMax(scrollX, pL);
    const int visT = qMax(scrollY, pT);
    const int visR = qMin(scrollX + vpW, pL + pW);
    const int visB = qMin(scrollY + vpH, pT + pH);
    if (visR <= visL || visB <= visT) return;
    const QRect vis(visL - pL, visT - pT, visR - visL, visB - visT);
    // Da co vung dung zoom nay phu kin vung nhin => khong dung lai (pan nhe vo ich).
    if (m_fgnRegPage == pg && qAbs(m_fgnRegScale - m_zoom) < 1e-6
        && !m_fgnRegImg.isNull() && m_fgnRegRect.contains(vis))
        return;
    // Da thu o dung zoom nay ma that bai => khong xep lai.
    if (m_fgnRegFailPage == pg && qAbs(m_fgnRegFailScale - m_zoom) < 1e-6) return;
    // Dung thua 1/4 moi be (giong updateHeavyRegion): pan nhe khong khoi dung lai tu dau.
    QRect region = vis.adjusted(-vis.width() / 4, -vis.height() / 4,
                                 vis.width() / 4,  vis.height() / 4)
                       .intersected(QRect(0, 0, pW, pH));
    if (region.isEmpty()) return;
    m_fgnRegionBuilding = true;
    m_fgnRegFailPage = -1;
    qDebug().noquote() << "[fgncont] YEU CAU vung net page=" << pg
                       << "scale=" << m_zoom << "region=" << region;
    emit foreignAnnotRegionNeeded(pg, m_zoom, region);
}

void ContinuousView::evictForeignLayers() {
    // 🔴🔴 SUA 02/09/2026 — "chu thich tu nhien mat, lat view/doi che do thi hien lai".
    // Luat CU: giu lop co dist-toi-pageAtCenter() ≤ kKeepRadius. Nham: "trang o tam"
    // KHONG PHAI "trang dang xem" — o Continuous nhieu trang cung hien tren man mot luc,
    // tam dich mot cai la lop bu cua trang DANG NHIN bi vut (log: EVICT page=2 dist=2,
    // roi set page=2 ready=true dung lai ngay; trang 1 bi dung lai 11 lan/phiên).
    // Luat MOI (tam nhìn that, khong khoang cach tam):
    //   1. ⛔ Trang DANG HIEN THI tren viewport (gap ≤ 0) KHONG BAO GIO bi duoi.
    //   2. Giu them cac lop trong vung dem 1 chieu cao viewport quanh vung nhìn
    //      (cuon nhe khong phai dung lai).
    //   3. Tran cung kFgnMaxLayers lop (moi lop la ANH LON ~10-45 MB): duoi theo thu
    //      tu XA VUNG NHIN NHAT truoc, bat ke ly do 2 — khong giu vo han (RAM 6,2 GB).
    const int vpTop = verticalScrollBar()->value();
    const int vpBot = vpTop + viewport()->height();
    QVector<QPair<int,int>> cand;   // (gap px, page) — cac lop CHUA hien tren man
    for (auto it = m_fgnLayers.constBegin(); it != m_fgnLayers.constEnd(); ++it) {
        const int top = pageTopY(it.key());
        const int gap = qMax(vpTop - (top + pageH(it.key())),   // <0: trang nay phia tren
                             top - vpBot);                      // >0: trang nay phia duoi
        if (gap <= 0) continue;      // ⛔ dang hien thi — khong bao gio duoi
        cand.append(qMakePair(gap, it.key()));
    }
    std::sort(cand.begin(), cand.end());   // gap tang dan = gan vung nhìn truoc
    // So lop duoc giu them: trong vung dem (gap ≤ chieu cao viewport)...
    int keep = 0;
    while (keep < cand.size() && cand[keep].first <= vpBot - vpTop) ++keep;
    // ...nhung tong so lop khong vuot tran kFgnMaxLayers (duoi XA VUNG NHIN NHAT truoc).
    keep = qMin(keep, qMax(0, kFgnMaxLayers - (m_fgnLayers.size() - cand.size())));
    for (int i = keep; i < cand.size(); ++i) {
        m_fgnLayers.remove(cand[i].second);
        qDebug().noquote() << "[cont] fgnLayer EVICT page=" << cand[i].second
                           << "gapPx=" << cand[i].first << "(ngoai vung dem/vo tran)"
                           << "remaining=" << m_fgnLayers.size();
    }
}

bool ContinuousView::pageHasContent(int page) const {
    if (page < 0 || page >= m_pageCount) return false;
    auto vit = m_vecStore->constFind(page);
    if (vit != m_vecStore->constEnd() && *vit && (*vit)->isReady()) return true;
    if (m_pageImages.contains(page)) return true;
    return false;
}

bool ContinuousView::pageHasVector(int page) const {
    if (page < 0 || page >= m_pageCount) return false;
    // CAU TRUC: co nen vector dung duoc (khong phai "dang duoc ve luc nay"). Trang comment
    // co nen vector xong nhung van tam raster cho den khi lop comment san sang — MainWindow
    // can tho tin nay de know duoc dung lop comment. Dung !vectorBuildBlocked, KHONG phai
    // !rasterOnlyApplies (neu khong thi trang comment ket raster: khong nen => khong lop).
    auto vit = m_vecStore->constFind(page);
    return vit != m_vecStore->constEnd() && *vit && (*vit)->isReady()
        && (*vit)->isComplete() && !vectorBuildBlocked(page);
}

void ContinuousView::datLaiLenhVeTrang(int page) {
    if (!m_renderer || page < 0 || page >= m_pageCount) return;
    qDebug().noquote() << "[markup] dat lai lenh ve trang" << page;
    requestContinuousRender(page, m_zoom);
}

void ContinuousView::invalidatePage(int pageIndex) {
    m_pageImages.remove(pageIndex);
    m_pageImageZoom.remove(pageIndex);
    m_continuousRequested.remove(pageIndex);
    m_pendingRenderScale.remove(pageIndex);
    ++m_contStoreGen[pageIndex];   // LƯỢT 37: trang bi vo hieu hoa (noi dung doi) ⇒ thu nhỏ nen
                                   // dang bay cho no phai bi bo, khong ghi lai anh cu da invalidate.
    // 🔴 LÁT C 0902 — GOC cua "qua Continuous thi khong thay ca 2, giong nhu cho ay".
    // Lop lam net (m_sharpPixmap) nam DE LEN anh nen va chi duoc cap lai khi zoom/scroll.
    // Xoa anh nen ma giu lop phu cu ⇒ nen ve lai co chu thich den dau man hinh van
    // lo anh chap cua luc CHUA co chu thich — va no nam vay cho toi khi ai do zoom
    // ("chờ" chính là đây). Khac voi truong hop 01/09 (giu lop phu khi doi tab —
    // noi dung KHONG doi): o day noi dung trang DA DOI thi lop phu ay that su CU.
    if (m_sharpPage == pageIndex) {
        m_sharpPage = -1;
        m_sharpPixmap = {};
    }
    // Lop chu thich ngoai cua trang nay da doi => vung sac net cu het hieu luc.
    if (m_fgnRegPage == pageIndex) clearForeignAnnotRegion();
    // 🔴🔴 BO 2026-09-01 — day la GOC cua "Single co markup, Continuous khong".
    // Ham nay lam moi ANH TRANG. Markup la LOP RIENG (owner chot) nen no KHONG duoc
    // dinh liu gi o day. Ban cu xoa luon kho visuals ⇒ moi lan sua markup:
    //   refreshAnnotVisuals (bat dong bo) chay truoc → invalidatePage XOA visuals →
    //   neu ban refresh da ve xong TU BO DEM truoc do thi khong con ai day visuals lai nua
    //   ⇒ nen da giau markup cua ta, overlay lai rong ⇒ MARKUP BIEN MAT.
    // Single khong co dong tac xoa nay nen Single van hien — dung nhu owner quan sat.
    // LÁT A 09/02: khong con kho rieng de xoa — ca hai view doc DocTab::visualsCache.
    qDebug() << "[markup] invalidatePage page=" << pageIndex << "(giu nguyen lop markup)";
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

void ContinuousView::setSelectResizable(bool on) {
    m_selResizable = on;
    viewport()->update();
}

void ContinuousView::clearSelectedAnnot() {
    m_hasSel = false;
    m_selPage = -1;
    m_selResizable = false;
    m_resizingCorner = -1;
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
    m_resizingCorner = -1;
    viewport()->update();
}

// ── Resize handles (Insert Image, SPEC_INSERT_IMAGE_2026-08-30) ───────────────
QPointF ContinuousView::selHandlePos(int corner) const {
    const QRectF r = m_selRect;
    switch (corner) {
        case 1: return QPointF(r.right(), r.top());
        case 2: return QPointF(r.right(), r.bottom());
        case 3: return QPointF(r.left(),  r.bottom());
        default: return QPointF(r.left(), r.top());
    }
}

int ContinuousView::handleAt(const QPointF& dispPt) const {
    if (!m_hasSel || !m_selResizable) return -1;
    const double tol = 8.0 / m_zoom;
    for (int c = 0; c < 4; ++c) {
        const QPointF h = selHandlePos(c);
        if (qAbs(dispPt.x() - h.x()) <= tol && qAbs(dispPt.y() - h.y()) <= tol)
            return c;
    }
    return -1;
}

QPointF ContinuousView::selPageWidgetToDisp(const QPoint& widgetPos) const {
    const int sx = horizontalScrollBar()->value();
    const int sy = verticalScrollBar()->value();
    return QPointF((widgetPos.x() + sx - pageLeftX(m_selPage)) / m_zoom,
                   (widgetPos.y() + sy - pageTopY(m_selPage)) / m_zoom);
}

QRectF ContinuousView::resizeRectFor(int corner, const QPointF& drag, bool keepAspect) const {
    const QRectF orig = m_resizeOrigRect;
    const QPointF anchor = selHandlePos((corner + 2) % 4);
    const double kMin = 20.0 / m_zoom;
    const double kAspect = orig.height() / qMax(0.1, orig.width());
    double w = drag.x() - anchor.x();
    double h = drag.y() - anchor.y();
    if (keepAspect) {
        const bool dir = (w * h >= 0.0);
        const double hw = qMax(qAbs(w), kMin);
        w = (w >= 0.0 ? 1.0 : -1.0) * hw;
        h = (dir ? (w >= 0.0 ? 1.0 : -1.0) : (w >= 0.0 ? -1.0 : 1.0)) * hw * kAspect;
    }
    QRectF nr = QRectF(anchor, QPointF(anchor.x() + w, anchor.y() + h)).normalized();
    if (nr.width()  < kMin) { double c = nr.center().x(); nr.setLeft(c - kMin / 2); nr.setRight(c + kMin / 2); }
    if (nr.height() < kMin) { double c = nr.center().y(); nr.setTop(c - kMin / 2); nr.setBottom(c + kMin / 2); }
    return nr;
}

bool ContinuousView::resolvePageDisplayPos(const QPoint& widgetPos, int* page, QPointF* dispPt) const {
    if (!m_doc || !m_doc->isOpen() || m_pageCount == 0) return false;
    const QPoint canvasPos = widgetPos
        + QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
    for (int i = 0; i < m_pageCount; ++i) {
        const int left = pageLeftX(i), top = pageTopY(i);
        const int right = left + pageW(i), bottom = top + pageH(i);
        if (canvasPos.x() >= left && canvasPos.x() < right
            && canvasPos.y() >= top && canvasPos.y() < bottom) {
            if (page) *page = i;
            if (dispPt) *dispPt = QPointF((canvasPos.x() - left) / m_zoom,
                                          (canvasPos.y() - top) / m_zoom);
            return true;
        }
    }
    return false;
}

bool ContinuousView::probeViewportCenter(int* page, QPointF* dispCenter) const {
    if (!viewport() || viewport()->width() <= 0 || viewport()->height() <= 0) return false;
    const QPoint center = viewport()->rect().center();
    return resolvePageDisplayPos(center, page, dispCenter);
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
    QElapsedTimer _e; _e.start();
    viewport()->update();
    requestVisiblePages();
    m_scrollTimer->start();
    m_sharpTimer->start();
    const qint64 _m = _e.elapsed();
    if (_m > 20) qDebug().noquote() << "[scrollms] scrollContentsBy ms=" << _m;
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
    QElapsedTimer _pt;
    _pt.start();
    
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
        // LƯỢT 35: KHONG ve chu o day nua. Ve text bang QPainter len viewport
        // QOpenGLWidget sau khi GL resource cua cac tab bi huy => glyph cache GL hong
        // => chu be/bien dang (owner bao r33h). Man chao bay gio la tab "Welcome"
        // (PdfView widget thuong, ve bang CPU) — MainWindow an ContinuousView khi
        // khong con tai lieu nao. Chi giu nen ban.
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
        auto vit = m_vecStore->constFind(i);
        if (vit != m_vecStore->constEnd() && *vit && (*vit)->isReady() && (*vit)->isComplete()
            && !rasterOnlyApplies(i)) {
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
                QOpenGLContext* ctx = QOpenGLContext::currentContext();
                if (!ctx) {
                    qDebug().noquote() << "[contpaint] SKIP vector - KHONG co GL context page=" << i;
                    p.endNativePainting();
                    // khong ve vector cho trang nay; drewVector giu nguyen false ->
                    // raster lo phan hien thi
                } else {
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
                    QOpenGLFunctions* gl = ctx->functions();
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

QElapsedTimer _dt; _dt.start();
                    m_vgr.draw(*(*vit), mvp, QSize(vpW, vpH), pxPerPt);
                    const qint64 _dms = _dt.elapsed(); if (_dms > 30) qDebug().noquote() << "[contpaint] vgr.draw page=" << i << " ms=" << _dms;
                    
                    if (prevOn)
                        gl->glScissor(prevBox[0], prevBox[1], prevBox[2], prevBox[3]);
                    else
                        gl->glDisable(GL_SCISSOR_TEST);
                }
                p.endNativePainting();
                drewVector = true;
                }

            }
        }

        // Lop bu foreign annot (C1): ve DE LEN lop vector ngay sau khi lop vector xong
        // (cho drewVector == true), lam GIONG PdfGpuView::drawForeignAnnotLayers — dung
        // he toa do (vx, vy, cw, ch) cua trang i trong ContinuousView.
        if (drewVector) {
            auto fit = m_fgnLayers.constFind(i);
            if (fit != m_fgnLayers.constEnd() && *fit && (*fit)->isReady()
                && (*fit)->pageIndex() == i && !(*fit)->image().isNull()) {
                // Nghiem thu (SPEC_BO_RASTERONLY 2026-08-30): trang markup ngoai ve bang
                // lop vector NEN + lop bu DE LEN, dung nhu Single.
                qDebug().noquote() << "[contvec] vector + lop bu page=" << i;
                p.save();
                p.setRenderHint(QPainter::SmoothPixmapTransform, true);
                p.drawImage(QRectF(vx, vy, cw, ch), (*fit)->image());
                p.restore();
            }
            // VUNG SAC NET (theo dung zoom hien tai) DE LEN lop bu THO — giong
            // PdfGpuView::drawForeignAnnotLayers. Lop bu toan trang duoc dung o mot muc zoom
            // (maxPx=900/1103) nen bi keo gian khi zoom len => chu thich doi tac nhoe; vung
            // nay thay dung pixel o 1:1. Anh trong suot: chi pixel chu thich duoc ghi.
            if (m_fgnRegPage == i && qAbs(m_fgnRegScale - m_zoom) < 1e-6 && !m_fgnRegImg.isNull()) {
                p.save();
                p.setRenderHint(QPainter::SmoothPixmapTransform, false);
                p.drawImage(QPoint(vx + m_fgnRegRect.x(), vy + m_fgnRegRect.y()), m_fgnRegImg);
                p.restore();
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
            } else if (m_pageLowRes.contains(i)) {
                // Chua co anh full va chua co lop vector: ve ban THO keo gian ra,
                // giong PaintTile(renderMissing) cua SumatraPDF. Mo nhung CO HINH —
                // tuyet doi khong de trang trang.
                p.fillRect(vx, vy, cw, ch, m_darkMode ? QColor(45, 45, 45) : Qt::white);
                const QPixmap& lo = m_pageLowRes[i];
                p.drawPixmap(QRect(vx, vy, cw, ch), lo, lo.rect());
                qDebug().noquote() << "[bac2] ve bang thumbnail page=" << i;
            } else {
                // Khong co gi de ve — nen tron + bao "Loading" de user khong tuong
                // treo roi bam chong lam app Not Responding (owner 30/08). Chu bien
                // mat khi co noi dung (vector/raster/lowres) ve de len.
                p.fillRect(vx, vy, cw, ch, m_darkMode ? QColor(45, 45, 45) : Qt::white);
                // LUOT 42 (30/09): chu Loading ve qua QImage CPU, khong qua glyph
                // cache GL (viewport la QOpenGLWidget, xem LoadingBadge.h).
                const QColor loadingColor = m_darkMode ? QColor(190, 190, 190) : QColor(150, 150, 150);
                const QImage badge = loadingBadgeImage(QStringLiteral("Loading..."), 11, false,
                                                        loadingColor, devicePixelRatioF());
                drawLoadingBadge(p, QRectF(vx, vy, cw, ch * 0.9), badge);
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

    // ── BAC 2 (SPEC_SMOOTH_123 31/08): yeu cau thumbnail cho trang visible chua co noi dung ─
    for (int i = 0; i < m_pageCount; ++i) {
        int cx = pageLeftX(i), cy = pageTopY(i), cw2 = pageW(i), ch2 = pageH(i);
        if (cy + ch2 <= scrollY || cy >= scrollY + vpH) continue;
        if (cx + cw2 <= scrollX || cx >= scrollX + vpW) continue;
        // Trang visible: co vector? co raster? co low-res?
        auto vit2 = m_vecStore->constFind(i);
        bool hasVector = vit2 != m_vecStore->constEnd() && *vit2 && (*vit2)->isReady() && (*vit2)->isComplete()
                         && !rasterOnlyApplies(i);
        bool hasRaster = m_pageImages.contains(i);
        bool hasLowRes = m_pageLowRes.contains(i);
        // Neu trang visible ma khong co noi dung (vector/raster/lowres) thi yeu cau thumbnail.
        // Chi yeu cau mot lan (m_thumbRequested) de tranh spam.
        if (!hasVector && !hasRaster && !hasLowRes && !m_thumbRequested.contains(i)) {
            m_thumbRequested.insert(i);
            emit needThumbnail(i);
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
            auto vit2 = m_vecStore->constFind(i);
            if (vit2 != m_vecStore->constEnd() && *vit2 && (*vit2)->isReady() && (*vit2)->isComplete()
                && !rasterOnlyApplies(i))
                ++nv;
            else if (m_pageImages.contains(i))
                ++nr;
        }
        if (nv != lastNv || nr != lastNr) {
            qDebug() << "[cont] paint vectorPages=" << nv << "rasterPages=" << nr << "ms=" << _pt.elapsed();
            lastNv = nv; lastNr = nr;
        }
    }

    // ── Annotation overlay visuals ────────────────────────────────────────────
    // 🔴 BO DEM 2026-08-31: dem so markup THUC SU ve ra moi luot paint. Dung de tra loi
    // dut khoat cau "mat markup khi zoom" — mat o dau: khong co visuals, hay co ma khong ve.
    {
        static int _lastN = -1, _lastVis = -1; static double _lastZ = -1;
        int nVis = 0, nOverlay = 0;
        if (m_visualsSrc)
        for (auto it = m_visualsSrc->constBegin(); it != m_visualsSrc->constEnd(); ++it) {
            if (!pageVisible(it.key())) continue;
            const int pg = it.key();
            auto vv = m_vecStore->constFind(pg);
            const bool veVec = (vv != m_vecStore->constEnd() && *vv && (*vv)->isReady()
                                && (*vv)->isComplete() && !rasterOnlyApplies(pg));
            // Dem theo DUNG luat ma ma ve dung (xem vong ve ben duoi), khong dem theo luat cu.
            for (const AnnotVisual& av : it.value()) { ++nVis; if (av.paintByOverlay) ++nOverlay; }
        }
        if (nVis != _lastVis || nOverlay != _lastN || qAbs(m_zoom - _lastZ) > 1e-9) {
            _lastVis = nVis; _lastN = nOverlay; _lastZ = m_zoom;
            qDebug().noquote() << "[markup] overlay visuals=" << nVis << "seVe=" << nOverlay
                               << "zoom=" << m_zoom;
        }
    }
    if (m_visualsSrc && !m_visualsSrc->isEmpty()) {
        p.setRenderHint(QPainter::Antialiasing, true);
        for (int i = 0; i < m_pageCount; ++i) {
            auto vit = m_visualsSrc->constFind(i);
            if (vit == m_visualsSrc->constEnd() || vit->isEmpty()) continue;

            int vx = pageLeftX(i) - scrollX;
            int vy = pageTopY(i) - scrollY;

            // Trang nay co dang duoc ve bang LOP VECTOR khong?
            auto vitV = m_vecStore->constFind(i);
            const bool veBangVector = (vitV != m_vecStore->constEnd() && *vitV
                                       && (*vitV)->isReady() && (*vitV)->isComplete()
                                       && !rasterOnlyApplies(i));
            for (const AnnotVisual& av : *vit) {
                    // 🔴 GO BO 2026-09-01 (owner: "sai font sai nen, markup bien dang").
                    // Ve FreeText/Text bang QPainter la ban VE GAN DUNG: phong chu mac dinh cua Qt,
                    // khong nen, khong vien, khong theo appearance stream (/AP) cua chu thich.
                    // No KHONG BAO GIO giong ban PDFium ve. Single dep vi Single lay comment tu
                    // ANH PDFIUM chu khong tu ban gan dung nay.
                    // ⇒ Quay ve: overlay chi ve thu no ve DUNG duoc. Comment ngoai lay tu anh raster
                    //   (PDFium ve), va do NET nay do duong lam net vung nhin (da sua cung ngay).
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
                        // 3 lớp NỀN→VIỀN→CHỮ. CHU ve qua drawFreeTextOverlay CHUNG
                        // voi PdfGpuView — cung pixelSize=fontSize*zoom, cung le,
                        // cung wrap, khong bao gio cat cuth.
                        p.save();
                        if (av.hasFill && av.fill.isValid() && av.fill.alpha() > 0)
                            p.fillRect(dRect, av.fill);
                        if (av.border > 0.0f) {
                            p.setPen(QPen(av.stroke.isValid() ? av.stroke : QColor(Qt::black),
                                          qMax(1.0, av.border * m_zoom)));
                            p.setBrush(Qt::NoBrush);
                            p.drawRect(dRect);
                        }
                        p.restore();
                        drawFreeTextOverlay(p, dRect, av.text, av.fontSize, m_zoom, av.stroke);
                        break;
                    }
                    case FPDF_ANNOT_STAMP: {
                        // Insert Image: anh goc giu kenh alpha (khong do nen trang).
                        if (!av.image.isNull()) {
                            p.save();
                            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
                            p.drawImage(dRect, av.image);
                            p.restore();
                        }
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
        // Insert Image: 4 tay nam goc khi Stamp dang chon.
        if (m_selResizable) {
            p.setBrush(QColor(0, 120, 215));
            p.setPen(Qt::NoPen);
            for (int c = 0; c < 4; ++c) {
                QPointF hp(sx + selHandlePos(c).x() * m_zoom,
                           sy + selHandlePos(c).y() * m_zoom);
                p.drawRect(QRectF(hp.x() - 5, hp.y() - 5, 10, 10));
            }
        }
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
{{ QElapsedTimer _e; _e.start(); 
    { QElapsedTimer _e; _e.start(); 
        { QElapsedTimer _e; _e.start(); 
            
                if (event->modifiers() & Qt::ControlModifier) {
                    // Zoom gesture — keep canvas point under cursor fixed
                    double delta   = event->angleDelta().y() / 1200.0;
                    double newZoom = qBound(0.1, m_zoom + delta, 10.0);
                    if (qAbs(newZoom - m_zoom) < 1e-9) {
                        event->accept();
                        return;
                    }
            
                    // Neo DIEM TAI LIEU duoi con tro (khong neo bang ti le pixel:
                    // kGap/kHPad/can-giua khong no theo zoom => hinh nhay).
                    zoomAnchored(newZoom, event->position());
            
                    m_sharpPage = -1;
                    m_sharpPixmap = {};
                    clearForeignAnnotRegion();   // zoom moi => vung sac net cu het hieu luc
                    m_primaryPage = -1;
                    m_lastRequestZoom = -1.0;
                    m_primaryRequested = false;
                    m_contSettleTimer->stop();
                    m_pendingRenderScale.clear();
                    // GOC3: don't clear m_pageImageZoom — keep existing images as
                    // blurry placeholders; paintEvent scales them via QPainter.
                    m_cacheProbed.clear();
                    viewport()->update();
                    m_zoomTimer->start();
                    m_sharpTimer->start();
                    event->accept();
                } else {
                    // Normal scroll — let QAbstractScrollArea handle it (moves scrollbars)
                    QAbstractScrollArea::wheelEvent(event);
                }
        const qint64 _m=_e.elapsed(); if(_m>20) qDebug().noquote()<<"[scrollms] wheelEvent ms="<<_m;
        }
    const qint64 _m=_e.elapsed(); if(_m>20) qDebug().noquote()<<"[scrollms] wheelEvent ms="<<_m;
    }
const qint64 _m=_e.elapsed(); if(_m>20) qDebug().noquote()<<"[scrollms] wheelEvent ms="<<_m;
}}

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
            // LUOT 41 (30/09): Continuous chi-xem markup — owner chot 30/09 chi con
            // CHON, KHONG keo di chuyen, KHONG co gian tay nam goc Stamp. Bo han
            // nhanh khoi dong resize (handleAt) + drag (khung chon) truoc day; bam
            // luon roi xuong pick + pan nhu vung thuong. annotationPickRequested
            // van chay o mouseReleaseEvent (m_pickCandidate ben duoi), nen bam lai
            // markup dang chon van CHON duoc, chi khong con keo/co gian.
            int page = -1;
            QPointF dispPt;
            if (resolvePageDisplayPos(event->pos(), &page, &dispPt)) {
                m_pickCandidate = true;
                m_pickPage = page;
                m_pickPt = dispPt;
            } else {
                m_pickCandidate = false;
            }
        }
        // Pan (mac dinh): keo chuot trai = keo trang.
        m_panning      = true;
        m_panPressPos = event->pos();
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
    // Insert Image: keo tay nam goc de co gian (Shift = tu do, giu ty le mac dinh).
    if (m_resizingCorner >= 0) {
        const QPointF dispPt = selPageWidgetToDisp(event->pos());
        const bool keep = !(event->modifiers() & Qt::ShiftModifier);
        m_selRect = resizeRectFor(m_resizingCorner, dispPt, keep);
        viewport()->update();
        event->accept();
        return;
    }
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
    if (m_resizingCorner >= 0 && event->button() == Qt::LeftButton) {
        m_resizingCorner = -1;
        m_dragPixelDelta = QPointF();
        m_dragNoteRect = QRectF();
        m_dragNoteOffsetPt = QPointF();
        viewport()->update();
        // Chi ghi vao PDF khi KET THUC keo.
        if (m_hasSel && m_selPage >= 0)
            emit annotationResizeRequested(m_selPage, m_selRect);
        event->accept();
        return;
    }
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

            // KHONG duoc chan luong chinh o day: khoa PDFium co the dang bi mot luot dung
            // lop vector giu hang chuc giay (do duoc: lockwait 35.973 ms main=1).
            // Goc hop trang lay tu ban CACHE (khong can khoa). Goc xoay lay tu bo nho dem
            // rieng; chua co thi thu lay bang tryLock ngan, that bai thi dung 0.
            int rot = m_pageRot.value(foundPage, -1);
            QPointF box = (m_doc ? m_doc->pageBoxOriginCached(foundPage) : QPointF(0.0, 0.0));
            if (rot < 0) {
                rot = 0;
                if (m_doc && m_doc->raw() && s_pdfiumMutex.tryLock(50)) {
                    FPDF_PAGE pg = FPDF_LoadPage(m_doc->raw(), foundPage);
                    if (pg) {
                        rot = FPDFPage_GetRotation(pg) & 3;
                        FPDF_ClosePage(pg);
                        m_pageRot.insert(foundPage, rot);
                    }
                    s_pdfiumMutex.unlock();
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
        if (m_pickCandidate && (event->pos() - m_panPressPos).manhattanLength() <= 4)
            emit annotationPickRequested(m_pickPage, m_pickPt);
        m_pickCandidate = false;
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
