#include "ContinuousView.h"
#include "ThemeTokens.h"
#include "../core/PdfDocument.h"
#include "../core/PdfRenderer.h"
#include "../core/PdfiumLock.h"
#include "../core/PdfCoords.h"
#include "../core/VectorCacheFile.h"
#include "../core/TileCacheFile.h"
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
#include <cmath>
#include <algorithm>
#include <utility>
#include <QOpenGLFunctions>
#include <QElapsedTimer>

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
        // 🔴 2026-08-31: cuon da DUNG => tu gio moi cho phep dung lop vector.
        m_vecBuildArmed = true;
        // Doi lai visuals cho moi trang dang nhin thay. Sau mot lan zoom, moi viec dang bay
        // bi huy nen khong con su kien nao keo markup ve — day la luoi an toan cho no.
        for (int i = 0; i < m_pageCount; ++i)
            if (pageVisible(i)) needAnnotVisuals(i);
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
     m_retryTimer = new QTimer(this);
     m_retryTimer->setInterval(300);
     connect(m_retryTimer, &QTimer::timeout, this, [this] {
         if (!m_renderer || m_pageCount == 0 || m_primaryPage < 0) return;
         // Da co anh hoac da co lop vector => khong can thu lai nua.
         if (m_pageImages.contains(m_primaryPage) || vectorWillRender(m_primaryPage)) {
             m_retryTimer->stop();
             return;
         }
         qDebug().noquote() << "[cont] RETRY primary page=" << m_primaryPage << " zoom=" << m_zoom;
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

void ContinuousView::setDocument(PdfDocument* doc, PdfRenderer* renderer)
{
    // Clear any text selection when document changes
    m_selecting = false;
    m_selStart  = m_selEnd = QPoint();
    m_selDragging = false;
    stopAutoScroll();
    clearTextSelectionInternal();

    // Việc B: moi duong vao Continuous deu phai co khoa .torvec hop le. MainWindow
    // bo sot cho goi setVectorCacheKey o duong doi tab (onTabChanged) va tinh hash
    // bat dong bo (initWatcher) — nen setDocument tu bao dam key theo doc dang gan.
    updateVecCacheKeyFromDoc(doc);

    ++m_vecGen;
    m_vecPool.clear();
    m_vecLayers.clear();
    m_fgnLayers.clear();
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
                {
                    QImage use = img;
                    const double needW = pageSizePt(idx).width() * m_zoom * devicePixelRatioF();
                    if (needW > 1.0 && img.width() > needW * 1.5) {
                        // 🔴🔴 SUA 2026-09-01 (owner: "zoom 100% khong net").
                        // Ban cu thu nho ve `needW * 1.15` — tuc VAN LON hon co man hinh 15%.
                        // Sau do khung nhin lai co tiep ve dung co ⇒ anh bi LAY MAU LAI HAI LAN.
                        // Hai lan lay mau lien tiep lam net manh nhoe han di; do la cai "mo" o
                        // 100% chu khong phai thieu do phan giai (do duoc: anh nen 2741 px trong
                        // khi man hinh chi can 2384 — tuc DU, ma van mo).
                        // ⇒ Co thang ve DUNG co man hinh: chi con MOT lan lay mau, ve 1:1.
                        use = img.scaledToWidth(int(needW), Qt::SmoothTransformation);
                        qDebug().noquote() << "[cont] downscale page=" << idx
                                           << "from=" << img.width() << "to=" << use.width();
                    }
                    m_pageImages[idx] = QPixmap::fromImage(use);
                m_thumbRequested.remove(idx);  // BAC 2: da co raster, khong can thumbnail nua
                }
                setPageLowRes(idx, img);
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
                // Raster toi — noi dung da san. Bao MainWindow: neu trang bi DEFER lan 1
                // (chua co gi de ve) thi lan nay lop bu phai DUNG (chot C3 2026-08-30).
                needAnnotVisuals(idx);
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
    // GOC1: tell renderer to use zoom-based scale in fast mode
    if (m_renderer) m_renderer->setContinuousUseZoomScale(m_fastMode);
    m_pageImages.clear();
    m_pageImageZoom.clear();
    m_pageAnnotVisuals.clear();
    m_continuousRequested.clear();
    m_pendingRenderScale.clear();
    m_vecLayers.clear();
    m_fgnLayers.clear();
    m_vecBuilding.clear();
    m_rasterOnlyPages.clear();
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
                {
                    QImage use = img;
                    const double needW = pageSizePt(idx).width() * m_zoom * devicePixelRatioF();
                    if (needW > 1.0 && img.width() > needW * 1.5) {
                        // 🔴🔴 SUA 2026-09-01 (owner: "zoom 100% khong net").
                        // Ban cu thu nho ve `needW * 1.15` — tuc VAN LON hon co man hinh 15%.
                        // Sau do khung nhin lai co tiep ve dung co ⇒ anh bi LAY MAU LAI HAI LAN.
                        // Hai lan lay mau lien tiep lam net manh nhoe han di; do la cai "mo" o
                        // 100% chu khong phai thieu do phan giai (do duoc: anh nen 2741 px trong
                        // khi man hinh chi can 2384 — tuc DU, ma van mo).
                        // ⇒ Co thang ve DUNG co man hinh: chi con MOT lan lay mau, ve 1:1.
                        use = img.scaledToWidth(int(needW), Qt::SmoothTransformation);
                        qDebug().noquote() << "[cont] downscale page=" << idx
                                           << "from=" << img.width() << "to=" << use.width();
                    }
                m_thumbRequested.remove(idx);  // BAC 2: da co raster, khong can thumbnail nua
                    m_pageImages[idx] = QPixmap::fromImage(use);
                }
                setPageLowRes(idx, img);
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
                needAnnotVisuals(idx);   // noi dung da san — lay lop bu neu bi DEFER lan 1
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
    // : TileCacheFile::hashFile). Chi doc 128 KB nen re (~vai ms), chi goi khi thieu key.
    m_vecKeyPath = fp;
    m_vecKeyHash = (quint64)TileCacheFile::hashFile(fp);
}

void ContinuousView::setPageLowRes(int pageIndex, const QImage& img)
{
    if (img.isNull() || pageIndex < 0) return;
    QImage lowres = (img.width() >= img.height())
        ? img.scaledToWidth(qMin(320, img.width()), Qt::SmoothTransformation)
        : img.scaledToHeight(qMin(320, img.height()), Qt::SmoothTransformation);
    m_pageLowRes.insert(pageIndex, QPixmap::fromImage(lowres));
}

void ContinuousView::setZoom(double scale)
{
    double newZoom = qBound(0.1, scale, 10.0);
    if (qAbs(newZoom - m_zoom) < 1e-9) return;
    m_zooming = true;
        qDebug().noquote() << "[zoomms] setZoom ENTER from=" << m_zoom << " to=" << newZoom;

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
    // (2) Markup ve bang overlay lay tu `m_pageAnnotVisuals`, ma visuals chi duoc xin lai khi
    //     raster/vector toi. Zoom huy het viec dang bay ⇒ khong con su kien nao xin lai visuals.
    // ⇒ Hen gio lai sau khi zoom: 400 ms sau se vua mo cong dung vector, vua DOI LAI visuals
    //   cho moi trang dang nhin thay.
    m_vecBuildArmed = false;
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
                     m_retryTimer->start();
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
                            // Image is at full quality but zoom doesn't match user's zoom.
                            // No re-render needed — full quality is always good enough.
                            // Just proceed to neighbors.
{ QElapsedTimer _q; _q.start(); requestNeighborPages(); const qint64 _n=_q.elapsed(); if(_n>15) qDebug().noquote()<<"[scrollms] requestNeighborPages ms="<<_n; }
                        }
                        goto evict;
                    }
            
                    // Step c: cache miss → record probe (skip re-probe) and start settle timer
                    m_cacheProbed[primary] = m_zoom;
                    qDebug() << "[perf] cont primary cache miss page=" << primary
                             << "— starting 400ms settle";
                    m_vecBuildArmed = false;   // dang cuon => cam dung vector
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
                    const bool hasVector = (m_vecLayers.constFind(i) != m_vecLayers.constEnd()
                                            && *m_vecLayers.constFind(i)
                                            && (*m_vecLayers.constFind(i))->isReady()
                                            && (*m_vecLayers.constFind(i))->isComplete()
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
    auto it = m_vecLayers.constFind(pg);
    if (it != m_vecLayers.constEnd() && *it && (*it)->isReady() && (*it)->isComplete())
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
// Bo nhanh vector cung xoa luon kho vector RIENG cua Continuous (m_vecLayers) — chinh la thu
// da gay "hai kho vector" va markup mat trong Continuous.
bool ContinuousView::contRasterOnlyEnv()
{
    static const bool on = !qEnvironmentVariableIsEmpty("TORREADER_CONT_RASTER");
    return on;
}

bool ContinuousView::trangCanRaster(int pg) const
{
    auto it = m_pageAnnotVisuals.constFind(pg);
    if (it == m_pageAnnotVisuals.constEnd()) return false;
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

bool ContinuousView::rasterOnlyApplies(int pg) const
{
    // 🔴🔴 CONG 2026-09-01 — "NEN VECTOR KHONG CHUA ANNOTATION".
    // Do that tren file co comment cua phan mem khac (owner: "260917 3Fl th checked"):
    //   markupCuaTa=0 · hasForeign=true · visuals=1 · overlay seVe=0
    //   [fgnlayer] DEFER - chua biet so object   ← lop bu KHONG BAO GIO dung duoc
    // ⇒ Chu thich ngoai CHI ton tai trong anh raster. Single dang ve nen raster nen thay;
    //   Continuous da chuyen sang nen vector nen mat sach. Dung nhu owner quan sat.
    // ⇒ Trang co chu thich NGOAI phai O LAI raster. Luat nay KHONG ap cho markup CUA TA —
    //   markup cua ta la lop rieng, overlay ve, khong quan tam nen la gi (owner chot).
    if (!m_vecAnnotSafe.value(pg, true)) return true;
    // Chot tu quyet: co Note/Text/chu thich ngoai tren trang => phai ve bang raster,
    // vi chi PDFium ve dung chung va nen vector khong chua annotation nao.
    if (trangCanRaster(pg)) return true;
    if (contRasterOnlyEnv()) return true;      // cong tac: raster cho MOI trang
    if (!m_rasterOnlyPages.contains(pg)) return false;
    // Trang nang (>= nguong): bo qua rasterOnly, cho phep dung lop vector de hien trang.
    if (m_renderer && m_renderer->pageObjectCount(pg) > kHeavyObjectThreshold)
        return false;
    return true;
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
     int lo = qMax(0, center - kKeepRadius);
     int hi = qMin(m_pageCount - 1, center + kKeepRadius);
    qDebug().noquote() << QString("[contvec] ENTER center=%1 lo=%2 hi=%3 layers=%4 building=%5 poolActive=%6 poolMax=%7")
                          .arg(center).arg(lo).arg(hi).arg(m_vecLayers.size())
                          .arg(m_vecBuilding.size())
                          .arg(QThreadPool::globalInstance()->activeThreadCount())
                          .arg(QThreadPool::globalInstance()->maxThreadCount());
for (int pg = lo; pg <= hi; ++pg) {
        if (rasterOnlyApplies(pg)) {
            qDebug() << "[contvec] rasterOnly pg=" << pg;
            continue;
        }
        if (m_rasterOnlyPages.contains(pg))
            qDebug().noquote() << "[contvec] rasterOnly BO QUA - trang nang objects="
                               << (m_renderer ? m_renderer->pageObjectCount(pg) : 0)
                               << "pg=" << pg << "(uu tien hien trang)";
        if (m_vecLayers.contains(pg) || m_vecBuilding.contains(pg)) {
            qDebug().noquote() << QString("[contvec] skip pg=%1 hasLayer=%2 isBuilding=%3")
                                  .arg(pg).arg(m_vecLayers.contains(pg)).arg(m_vecBuilding.contains(pg));
            continue;
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
                return;
            }
            qDebug().noquote() << "[contvec] FINISHED pg=" << pg << " result=" << w->result();
            w->deleteLater();
            m_vecBuilding.remove(pg);
            m_vecBuildStart.remove(pg);
            if (w->result()) m_vecLayers.insert(pg, layer);
            qDebug().noquote() << "[cont] vecLayer" << (w->result() ? "READY" : "SKIP")
                               << "page=" << pg << "cached=" << m_vecLayers.size();
            // Noi dung trang da san — bao MainWindow truoc (de push visuals + lay lop bu).
            if (w->result()) needAnnotVisuals(pg);
            viewport()->update();
        });
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
        FPDF_DOCUMENT docRaw = (m_doc && m_doc->isOpen()) ? m_doc->raw() : nullptr;
        w->setFuture(QtConcurrent::run(&m_vecPool, [layer, pg, keyPath, keyHash, choPhepDung, docRaw] {        { QElapsedTimer _e; _e.start(); 
                    { QElapsedTimer _e; _e.start(); 
                        
                                    QElapsedTimer _t; _t.start();
                                    qDebug().noquote() << "[contvec] TASK-START pg=" << pg
                                                       << "keyPath=" << (keyPath.isEmpty() ? QStringLiteral("(RONG)") : keyPath)
                                                       << "keyHash=" << QString::number(keyHash, 16);
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
                                    // Chi dung cho MOT trang tai mot thoi diem, va chi khi cuon DA DUNG
                                    // (ham nay duoc goi tu contSettleTimer), nen khong phi cong luc luot qua.
                                    if (choPhepDung && docRaw) {
                                        qDebug().noquote() << "[contvec] DUNG MOI pg=" << pg << " (trang trung tam)";
                                        const bool ok = layer->build(docRaw, pg);
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
for (auto it = m_vecLayers.begin(); it != m_vecLayers.end(); ) {
            const int dist = qAbs(it.key() - center);
            // Viec C: trang nang phai DUNG MOI vai giay moi hien lai — noi long ban
            // kinh giu lop (dist > 3). Trang thong thuong van giu dist > 1 nhu cu.
            const int keep = (m_renderer
                              && m_renderer->pageObjectCount(it.key()) > kHeavyObjectThreshold)
                                 ? kHeavyKeepRadius : kKeepRadius;
            if (dist > keep) {
                qDebug().noquote() << "[cont] vecLayer EVICT page=" << it.key()
                                   << "dist=" << dist << ">" << keep
                                   << "remaining=" << m_vecLayers.size() - 1;
                qDebug().noquote() << "[contvec] EVICT pg=" << it.key();
                it = m_vecLayers.erase(it);
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
    if (m_vecLayers.contains(pg) || m_vecBuilding.contains(pg)) return;
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
            m_vecLayers.insert(pg, layer);
            qDebug().noquote() << "[contvec] BUILD-PRIMARY READY pg=" << pg;
            needAnnotVisuals(pg);
            viewport()->update();
        }
    });
    FPDF_DOCUMENT d = m_doc->raw();
    const QString keyPath = m_vecKeyPath;
    const quint64 keyHash = m_vecKeyHash;
    w->setFuture(QtConcurrent::run(&m_vecPool, [layer, d, pg, keyPath, keyHash, objs] {
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
        const bool ok = layer->build(d, pg);
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

void ContinuousView::setAnnotVisualsForPage(int page, const QList<AnnotVisual>& visuals) {
    m_pageAnnotVisuals[page] = visuals;
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
    evictForeignLayers();
    viewport()->update();
}

void ContinuousView::evictForeignLayers() {
    // Giong m_vecLayers: gio giu lop trong ban kinh kKeepRadius quanh trang chinh
    // (toi da 2*kKeepRadius+1 = 3 lop). Moi lop la mot ANH LON, khong giu het trang.
    const int center = pageAtCenter();
    for (auto it = m_fgnLayers.begin(); it != m_fgnLayers.end(); ) {
        if (qAbs(it.key() - center) > kKeepRadius) {
            qDebug().noquote() << "[cont] fgnLayer EVICT page=" << it.key()
                               << "dist=" << qAbs(it.key() - center) << ">" << kKeepRadius;
            it = m_fgnLayers.erase(it);
        } else {
            ++it;
        }
    }
}

bool ContinuousView::pageHasContent(int page) const {
    if (page < 0 || page >= m_pageCount) return false;
    auto vit = m_vecLayers.constFind(page);
    if (vit != m_vecLayers.constEnd() && *vit && (*vit)->isReady()) return true;
    if (m_pageImages.contains(page)) return true;
    return false;
}

bool ContinuousView::pageHasVector(int page) const {
    if (page < 0 || page >= m_pageCount) return false;
    auto vit = m_vecLayers.constFind(page);
    return vit != m_vecLayers.constEnd() && *vit && (*vit)->isReady()
        && (*vit)->isComplete() && !rasterOnlyApplies(page);
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
    // 🔴🔴 BO 2026-09-01 — day la GOC cua "Single co markup, Continuous khong".
    // Ham nay lam moi ANH TRANG. Markup la LOP RIENG (owner chot) nen no KHONG duoc
    // dinh liu gi o day. Ban cu xoa luon `m_pageAnnotVisuals` ⇒ moi lan sua markup:
    //   refreshAnnotVisuals (bat dong bo) chay truoc → invalidatePage XOA visuals →
    //   neu ban refresh da ve xong TU BO DEM truoc do thi khong con ai day visuals lai nua
    //   ⇒ nen da giau markup cua ta, overlay lai rong ⇒ MARKUP BIEN MAT.
    // Single khong co dong tac xoa nay nen Single van hien — dung nhu owner quan sat.
    // m_pageAnnotVisuals.remove(pageIndex);   ← KHONG khoi phuc
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
        if (vit != m_vecLayers.constEnd() && *vit && (*vit)->isReady() && (*vit)->isComplete()
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
                p.setPen(m_darkMode ? QColor(190, 190, 190) : QColor(150, 150, 150));
                QFont lf = p.font();
                lf.setPointSize(11);
                p.setFont(lf);
                p.drawText(QRectF(vx, vy, cw, ch * 0.9), Qt::AlignHCenter | Qt::AlignVCenter,
                           QString("Loading..."));
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
        auto vit2 = m_vecLayers.constFind(i);
        bool hasVector = vit2 != m_vecLayers.constEnd() && *vit2 && (*vit2)->isReady() && (*vit2)->isComplete()
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
            auto vit2 = m_vecLayers.constFind(i);
            if (vit2 != m_vecLayers.constEnd() && *vit2 && (*vit2)->isReady() && (*vit2)->isComplete()
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
        for (auto it = m_pageAnnotVisuals.constBegin(); it != m_pageAnnotVisuals.constEnd(); ++it) {
            if (!pageVisible(it.key())) continue;
            const int pg = it.key();
            auto vv = m_vecLayers.constFind(pg);
            const bool veVec = (vv != m_vecLayers.constEnd() && *vv && (*vv)->isReady()
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
    if (!m_pageAnnotVisuals.isEmpty()) {
        p.setRenderHint(QPainter::Antialiasing, true);
        for (int i = 0; i < m_pageCount; ++i) {
            auto vit = m_pageAnnotVisuals.constFind(i);
            if (vit == m_pageAnnotVisuals.constEnd() || vit->isEmpty()) continue;

            int vx = pageLeftX(i) - scrollX;
            int vy = pageTopY(i) - scrollY;

            // Trang nay co dang duoc ve bang LOP VECTOR khong?
            auto vitV = m_vecLayers.constFind(i);
            const bool veBangVector = (vitV != m_vecLayers.constEnd() && *vitV
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
                        double fs = qMax(6.0, av.fontSize * m_zoom);
                        QFont ft = p.font();
                        ft.setPointSizeF(fs);
                        p.setFont(ft);
                        p.setPen(av.stroke.isValid() ? QPen(av.stroke) : QPen(Qt::black));
                        p.drawText(dRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, av.text);
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
            
                    // Canvas position under cursor before zoom
                    QPointF cursorVp = event->position();
                    int scrollX = horizontalScrollBar()->value();
                    int scrollY = verticalScrollBar()->value();
                    double cursorCanvasX = scrollX + cursorVp.x();
                    double cursorCanvasY = scrollY + cursorVp.y();
            
                    double ratio = newZoom / m_zoom;
                    m_zoom = newZoom;
{ QElapsedTimer _e; _e.start(); rebuildLayout(); qDebug().noquote()<<"[zoomms] rebuildLayout ms="<<_e.elapsed(); }
            
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
            // Chon/keo markup (SPEC_CONTINUOUS_MARKUP_EDIT): bam vao khung chon
            // da co → bat dau keo; nguoc lai → pick de MainWindow hit-test.
            if (m_hasSel && m_selPage >= 0) {
                // Insert Image: bam trung tay nam goc cua Stamp dang chon => co gian.
                if (m_selResizable) {
                    const int corner = handleAt(selPageWidgetToDisp(event->pos()));
                    if (corner >= 0) {
                        m_resizingCorner = corner;
                        m_resizeOrigRect = m_selRect;
                        m_dragPixelDelta = QPointF();
                        m_dragNoteRect = QRectF();
                        m_dragNoteOffsetPt = QPointF();
                        event->accept();
                        return;
                    }
                }
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
